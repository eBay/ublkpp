#include "ublkpp/craft_disk.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <stdexcept>

#include <boost/uuid/uuid_io.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h> // nr_hw_queues / qdepth (ublkpp_tgt's group) -- the client's aggregate IO bound
#include <ublksrv.h>              // ublksrv_queue, ublk_io_data, ublksrv_get_op

#include <ublkpp/lib/ublk_disk.hpp>

#include <craft/client.hpp> // client_handle + make_client + the free-function verbs + capacity/lba_size/term observers
#include <craft/local.hpp>  // make_local_cluster + backends
#include <craft/tcp.hpp>    // make_tcp_cluster + backends

#include <sisl/async/light_task.hpp> // sisl::async::sync_get (login off the reactor)

#include "lib/common.hpp"  // ilog2, SECTOR_SHIFT
#include "lib/logging.hpp" // DLOG*

namespace ublkpp {

// ── on-ring CRAFT completion: THE RING TRAVELS WITH THE VERB ──
//
// async_iov awaits the CRAFT verb DIRECTLY -- craft::read/write return freestanding tasks
// (sisl::async::light_task), co_await-able from this disk_task frame -- and hands the verb THIS QUEUE'S ring
// (craft_client >= 0.3.0: `q` sits right after the handle, the ublk parameter order). The op starts INLINE on
// the ublk queue thread: dLSN reservation, quorum broadcast, and transport serialize all run there before the
// first suspension. Every reply then arrives as a CQE on the ring we passed -- the mem transport times out on
// it, the TCP transport recvs on it -- so run_queue_loop reaps them and dispatches the foreign (_on_complete)
// cqe_states, resuming craft's transport coroutines ON THIS QUEUE'S THREAD. When the verb finishes it resumes
// this frame inline (a light_task resumes its awaiter on the completing thread -- the contract documented on
// the verbs in craft/client.hpp), and disk_task's continuation carries the result up to ublksrv_complete_io.
// No shim coroutine, no detach, no rendezvous state: the continuation chain IS the completion path, and no CQE
// ever needs attributing to a tag.
//
// The ring is therefore PART OF EVERY CALL, not session state: there is nothing to bind, and no client-wide
// ring for a second queue to collide with. AFFINITY IS OUR CONTRACT, exactly as it is with a raw io_uring: a
// ring is passed only from the thread that owns and reaps it, which is what a ublk queue thread already is. So
// nr_hw_queues > 1 just works -- each queue drives CRAFT on its own ring (the client opens one connection per
// (ring, replica), the nr_hw_queues x N grid, lazily at that ring's first verb), and one op's whole leg chain
// (broadcast legs, read failovers, the keep_alives it spawns) rides the ring it was called with. Client-wide
// state (the dLSN tracker, the route map) is shared across queues by design; only the transport is per ring.
//
// Exception edge (unchanged in kind from the shim design): the resume chain runs under the transport thunk's
// noexcept boundary, so nothing between here and ublksrv_complete_io may throw -- craft errors travel as
// values, and the post-await code below is allocation-free.
namespace {

// ublk speaks in 512-byte sectors regardless of the device's logical block size.
constexpr uint32_t k_sector_shift = SECTOR_SHIFT;

class CraftDisk : public ublk_disk {
public:
    // `client` is already logged in (so capacity/lba_size are known). `transport` is the opaque cluster handle
    // that owns the replicas + the transport; it is held as a keep-alive and must outlive the client (see the
    // member order below).
    CraftDisk(std::shared_ptr< void > transport, craft::client_handle client, craft::volume_id_t vol_id) :
            ublk_disk(),
            _transport(std::move(transport)),
            _client(std::move(client)),
            _id_str(boost::uuids::to_string(vol_id)) {
        if (!_client) throw std::runtime_error("craft_disk: null client");

        // Self-size EVERYTHING from what login reported -- no out-of-band geometry. lba_size, capacity, AND max_tx
        // are the volume's, conveyed once via login; nothing here re-picks them (see craft::max_tx).
        auto const page_size = craft::lba_size(_client);
        auto const capacity = craft::capacity(_client);
        auto const max_tx = craft::max_tx(_client);
        if (page_size == 0 || (page_size & (page_size - 1)) != 0)
            throw std::runtime_error("craft_disk: lba_size from login is not a power of two");
        if (max_tx < (1u << k_sector_shift)) throw std::runtime_error("craft_disk: max_tx from login too small");
        if (capacity < (1u << k_sector_shift)) throw std::runtime_error("craft_disk: capacity from login too small");

        // CRAFT reads/writes through its own datapath, never the kernel page cache.
        _direct_io = true;

        auto const bs_shift = static_cast< uint8_t >(std::countr_zero(page_size));
        auto& p = *params();
        p.basic.logical_bs_shift = bs_shift;
        p.basic.physical_bs_shift = bs_shift;
        // craft::max_tx(client) is ALREADY the max DATA transfer whose wire reply fits the volume's message bound
        // -- craft_client accounts for the read-reply framing (extent table + data), so the disk just caps to it.
        // max_sectors also cannot exceed the ublk per-tag buffer (ublkpp --max_io_size), which the kernel EINVAL's.
        p.basic.max_sectors = std::min(p.basic.max_sectors, static_cast< uint32_t >(max_tx >> k_sector_shift));
        p.basic.dev_sectors = capacity >> k_sector_shift;
        // Trim to whole logical blocks -- the real device-size constraint. Rounding to max_sectors (as before)
        // silently drops up to a full max IO of capacity whenever it is not a power-of-two divisor of the size.
        uint32_t const sectors_per_block = page_size >> k_sector_shift;
        p.basic.dev_sectors -= (p.basic.dev_sectors % sectors_per_block);

        // DISCARD/WRITE_ZEROES map onto a CRAFT zero write but need exact ublk_param_discard geometry; disabled
        // for this first cut (also rejected defensively in async_iov). TODO: thin zero-write mapping.
        p.types &= ~UBLK_PARAM_TYPE_DISCARD;

        DLOGI("craft_disk [vol={}] sectors={} lbs={} max_sectors={} term={}", _id_str, p.basic.dev_sectors, page_size,
              p.basic.max_sectors, craft::term(_client))
    }

    ~CraftDisk() override = default;

    std::string id() const noexcept override { return _id_str; }

    prepare_result prepare(ublksrv_queue const*, int const) override {
        // Nothing to bind: the ring travels with each verb (see the note above), so every queue is served by the
        // same client with no per-queue setup. All prepare does is SIZE the queue's ring for the on-ring data
        // path. The peak delivery-SQE count of one in-flight IO is NOT the replica count: a WRITE is one dLSN
        // broadcast to N replicas (N legs), but the client splits a READ into one sub-read PER horizon segment --
        // up to max_tx/page_size of them -- and issue_plan sends every sub-read to the SAME replica (unicast), so
        // a fully-split read puts max_tx/page_size legs on the ring at once. That read ceiling dominates N, so we
        // size to it (an extra *N would exceed IORING_MAX_ENTRIES at a deep qd, since
        // tgt_ring_depth = qd*(max_sqes_per_io+1)+1). SQ-full still degrades gracefully (ring_delay falls back to
        // inline completion), so this is a "keep it on the ring" ceiling, not a correctness bound. (init_queue
        // also reserves the per-IO cqe_state pool to this.) Called once per queue, plus once as prepare(nullptr,
        // 0) from init_tgt on the setup thread -- the answer is the same either way.
        auto const& bp = params()->basic;
        uint32_t const page = uint32_t{1} << bp.logical_bs_shift; // == lba_size(_client)
        uint64_t const max_io_bytes = static_cast< uint64_t >(bp.max_sectors) << k_sector_shift;
        return prepare_result{.fds = {}, .max_sqes_per_io = std::max< size_t >(1, max_io_bytes / page)};
    }

    disk_task< int > async_iov(ublksrv_queue const* q, ublk_io_data const* data, iovec* iovecs, uint32_t nr_vecs,
                               uint64_t addr) override {
        auto const op = ublksrv_get_op(data->iod);
        if (op == UBLK_IO_OP_FLUSH) co_return 0; // CRAFT IO is durable once acked (quorum-appended)
        if (op == UBLK_IO_OP_DISCARD || op == UBLK_IO_OP_WRITE_ZEROES) co_return -ENOTSUP; // TODO: CRAFT zero write

        bool const is_read = (op == UBLK_IO_OP_READ);

        // Copy the iovec DESCRIPTORS into an sg_list before the first co_await. iov_base points into the kernel-
        // mapped ublk IO buffer and stays valid until ublksrv_complete_io, so the client reads/writes it in place.
        sisl::sg_list sgs;
        sgs.size = 0;
        for (uint32_t i = 0; i < nr_vecs; ++i) {
            sgs.iovs.push_back(iovecs[i]);
            sgs.size += iovecs[i].iov_len;
        }
        uint64_t const len = sgs.size;

        // Read everything we need off the queue BEFORE the first co_await: `data` belongs to the queue and must
        // not be touched once the client has us suspended. That is the tag (tracing only) and THIS QUEUE'S ring,
        // which we hand to the verb -- we are on the queue thread here, the one that owns and reaps it, which is
        // exactly the affinity craft asks of the caller.
        int const tag = data->tag;
        auto* const ring = q->ring_ptr;

        DLOGT("craft io start [tag:{:#x}] {} addr={} len={}", tag, is_read ? "RD" : "WR", addr, len)
        // Await the verb directly (see the on-ring completion note above): runs inline to its first suspension,
        // puts its legs on `ring`, and resumes here on this queue's thread when the reply CQEs finish it.
        auto const res = is_read ? co_await craft::read(_client, ring, addr, len, std::move(sgs))
                                 : co_await craft::write(_client, ring, addr, len, std::move(sgs));

        if (res.has_value()) {
            DLOGT("craft io done  [tag:{:#x}] {} result={}", tag, is_read ? "RD" : "WR", res.value())
            co_return static_cast< int >(res.value());
        }
        if (res.error() == std::make_error_condition(std::errc::invalid_argument)) {
            DLOGD("craft io EINVAL [tag:{:#x}] {} addr={} len={}", tag, is_read ? "RD" : "WR", addr, len)
            co_return -EINVAL; // misaligned addr/len
        }
        DLOGE("craft io -EIO  [tag:{:#x}] {} addr={} len={} err=[{}]", tag, is_read ? "RD" : "WR", addr, len,
              res.error().message())
        co_return -EIO; // term fenced / no quorum / replica down / timed out -- the error message says which
    }

    void probe_tick(ublksrv_queue const* q) noexcept override {
        // Idle queue: no read/write to piggyback the commit watermark on. Drive a keep_alive at every leg (one
        // outstanding each) so the session does not expire -- the client's timer-less liveness mechanism. Fire it
        // on THIS queue's ring: the legs are detached, so this never blocks the queue thread, and they complete
        // as foreign CQEs that our own run_queue_loop reaps. The one-outstanding-per-leg collapse is client-wide,
        // not per queue, so with several idle queues whichever gets there first fires the leg on its own ring and
        // the rest find it already outstanding. A null ring is craft's blocking tier (the ringless overload
        // passes exactly that), so a caller without a queue still drives liveness, just off-ring.
        craft::drive_keepalives(_client, q ? q->ring_ptr : nullptr);
    }

private:
    // TEARDOWN ORDER (members destroyed in reverse of declaration): _client first (releases its replica refs),
    // then _transport (the cluster handle drains the transport's worker pools while it still owns every replica).
    std::shared_ptr< void > _transport; // opaque cluster keep-alive; must outlive _client
    craft::client_handle _client;
    std::string _id_str;
};

// craft::make_client's `max_inflight` is the AGGREGATE in-flight bound across every queue, so a blk-mq driver
// owes it nr_hw_queues x qdepth. It is not just a statistic: it sizes the dLSN tracker's winner-scan tripwire,
// and a read whose unresolved-dLSN window exceeds that cap is FENCED rather than served -- undersize it and
// deep-queue reads can fail spuriously. Both options belong to ublkpp_tgt's group, which a unit test or the fio
// engine does not enable (there are no ublk queues there either); fall back to craft's own default in that case.
constexpr uint32_t k_default_max_inflight = 128; // == craft::make_client's default

uint32_t aggregate_inflight() {
    if (!SISL_OPTIONS.count("nr_hw_queues") || !SISL_OPTIONS.count("qdepth")) return k_default_max_inflight;
    auto const queues = uint32_t{SISL_OPTIONS["nr_hw_queues"].as< uint16_t >()};
    auto const qdepth = uint32_t{SISL_OPTIONS["qdepth"].as< uint16_t >()};
    if (queues == 0 || qdepth == 0) return k_default_max_inflight;
    return queues * qdepth;
}

// Log the client in on the setup thread (off any reactor), or throw. Returns the logged-in handle.
craft::client_handle login_or_throw(craft::client_handle client, uint64_t token, char const* what) {
    auto const st = sisl::async::sync_get(craft::login(client, token));
    if (!st.has_value()) throw std::runtime_error(fmt::format("{}: login failed: {}", what, st.error().message()));
    return client;
}

} // namespace

// ── factories: fold the builder in, log in, then hand the disk a logged-in client + the cluster keep-alive ──

disk_handle make_craft_disk_local(craft::volume_id_t vol_id, uint32_t n, uint32_t page_size, uint64_t capacity,
                                  uint32_t max_tx, uint64_t client_token) {
    // `max_tx` configures the in-process volume here; the disk reads it back via craft::max_tx(client) after
    // login, so the device geometry is single-sourced from the client, never re-picked.
    auto cluster = craft::make_local_cluster(vol_id, n, page_size, capacity, max_tx);
    auto client = login_or_throw(craft::make_client(craft::backends(cluster), /*leader=*/0, aggregate_inflight()),
                                 client_token, "make_craft_disk_local");
    return std::make_shared< CraftDisk >(std::static_pointer_cast< void >(cluster), std::move(client), vol_id);
}

disk_handle make_craft_disk_tcp(std::vector< craft::replica_endpoint > members, craft::volume_id_t vol_id,
                                uint64_t client_token) {
    // No max_tx here: the SERVER's volume defines it, and the disk reads it via craft::max_tx(client) after login.
    auto cluster = craft::make_tcp_cluster(members, vol_id);
    auto client = login_or_throw(craft::make_client(craft::backends(cluster), /*leader=*/0, aggregate_inflight()),
                                 client_token, "make_craft_disk_tcp");
    return std::make_shared< CraftDisk >(std::static_pointer_cast< void >(cluster), std::move(client), vol_id);
}

} // namespace ublkpp
