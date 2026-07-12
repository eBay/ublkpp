#include "ublkpp/target.hpp"
#include "ublkpp/target_testing.hpp"

#include <chrono>
#include <pthread.h>
#include <ranges>
#include <sched.h>
#include <semaphore.h>
#include <exec/async_scope.hpp>
#include <exec/inline_scheduler.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <thread>

#include <boost/uuid/uuid_io.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <sisl/utility/thread_factory.hpp>
#include <ublksrv_utils.h>
#include <ublksrv.h>

#include "ublkpp/lib/ublk_disk.hpp"
#include "lib/memory_constants.hpp"
#include "lib/logging.hpp"
#include "lib/common.hpp"
#include <ublkpp/lib/cqe_state.hpp>
#include "ublkpp_tgt_impl.hpp"

namespace ublkpp::detail {
struct params_access {
    static ublk_params const* of(ublk_disk const& d) noexcept { return d.params(); }
};
} // namespace ublkpp::detail

SISL_OPTION_GROUP(ublkpp_tgt,
                  (max_io_size, "", "max_io_size", "Maximum I/O size before split",
                   cxxopts::value< std::uint32_t >()->default_value("524288"), "<io_size>"),
                  (nr_hw_queues, "", "nr_hw_queues", "Number of Hardware Queues (threads) per target",
                   cxxopts::value< std::uint16_t >()->default_value("1"), "<queue_cnt>"),
                  (qdepth, "", "qdepth", "I/O Queue Depth per target",
                   cxxopts::value< std::uint16_t >()->default_value("128"), "<qd>"),
                  (feature_zero_copy, "", "feature_zero_copy", "Enable ZeroCopy Feature", cxxopts::value< bool >(), ""),
                  (sched, "", "sched", "Queue thread scheduler policy (other, fifo)",
                   cxxopts::value< std::string >()->default_value("fifo"), "<policy>"))

using namespace std::chrono_literals;

namespace ublkpp {

ublkpp_tgt_impl::ublkpp_tgt_impl(boost::uuids::uuid const& vol_id, std::shared_ptr< ublk_disk > d) :
        volume_uuid(vol_id), device(std::move(d)), metrics(UblkIOMetrics(to_string(vol_id))) {}

static std::mutex _map_lock;
static std::map< ublksrv_ctrl_dev const*, std::shared_ptr< ublkpp_tgt_impl > > _init_map;

constexpr auto k_max_time = 1s;

static bool check_dev(ublksrv_ctrl_dev_info const* info) {
    static auto const sys_path = std::filesystem::path{"/"} / "dev";
    auto const str_path = (sys_path / fmt::format("ublkc{}", info->dev_id)).native();

    auto wait = 0ms;
    while (wait < k_max_time) {
        if (int fd = open(str_path.c_str(), O_RDWR); fd >= 0) {
            close(fd);
            return true;
        }
        std::this_thread::sleep_for(100ms);
        wait += 100ms;
    }
    return false;
}

// Matches UBLKSRV_IO_IDLE_SECS defined privately in ublksrv.c
static constexpr int k_io_idle_secs = 20;

struct ublkpp_queue_state : ublkpp_queue_base {
    std::shared_ptr< ublkpp_tgt_impl > tgt;
    exec::async_scope scope;
    bool is_idle{false};

    explicit ublkpp_queue_state(std::shared_ptr< ublkpp_tgt_impl > t) : tgt(std::move(t)) { metrics = &tgt->metrics; }
};

static void submit_probe_timeout(ublksrv_queue const* q) {
    if (auto* sqe = next_sqe(q)) {
        // clang-format off
        __kernel_timespec ts{.tv_sec = k_io_idle_secs, .tv_nsec = 0};
        // clang-format on
        io_uring_prep_timeout(sqe, &ts, 0, 0);
        sqe->user_data = sisl::async::encode_managed_user_data(nullptr); // sentinel: probe CQE, no cqe_state
        io_uring_submit(q->ring_ptr);
    }
}

// Our own CQE processing loop, replacing ublksrv_process_io.
// Target CQEs have bit 63 set; bits 62:0 hold a raw cqe_state* (non-null) for I/O completions
// or zero for probe timeout CQEs (null-pointer sentinel). Ublk command CQEs delegate to ublksrv.
//
// Drain correctness: ublksrv_queue_is_done returns true only when ublksrv has no pending I/O
// commands. We call ublksrv_complete_io at the end of __handle_io_async, after co_await
// device->async_iov returns, so the scope is already empty when the loop exits. on_empty()
// completes synchronously via its fast path.
static exec::task< void > run_queue_loop(ublksrv_queue const* q, ublkpp_queue_state* qs) {
    auto* ring = q->ring_ptr;
    // clang-format off
    struct __kernel_timespec ts{.tv_sec = k_io_idle_secs, .tv_nsec = 0};
    // clang-format on
    bool queue_done = false;

    while (!queue_done) {
        io_uring_cqe* cqe{};
        auto const ret = io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);

        unsigned head{};
        int count{0};
        int probe_count{0}; // probe timeout CQEs must not count as work for ublksrv_queue_update_idle
        io_uring_for_each_cqe(ring, head, cqe) {
            if (sisl::async::is_managed_user_data(cqe->user_data)) {
                // Decode to the COMMON base. A managed CQE is one of two shapes sharing this base at offset 0:
                // ours (ublkpp::cqe_state, _on_complete null, resumed via _waiter/_owner below) or a generic
                // sisl::async::cqe_state a transport that shares our ring submitted (craft_client's on-ring
                // cqe_awaitable, _on_complete set). We MUST branch on _on_complete before touching _waiter/_owner:
                // those live at OUR layout's offsets, which a foreign cqe_state does not share.
                auto* base =
                    static_cast< sisl::async::cqe_state* >(sisl::async::decode_managed_user_data(cqe->user_data));
                if (!base) {
                    // probe timeout CQE — only ETIME triggers a probe tick; other results ignored.
                    // Excluded from io_count: counting it as work triggers idle_exit, setting
                    // is_idle=false and preventing the probe from re-arming on subsequent fires.
                    ++probe_count;
                    if (cqe->res == -ETIME) {
                        // Gate check before capturing device: if _shutting_down is false,
                        // begin_shutdown's seq_cst store has not committed yet — so
                        // device = {} has not been called. Atomic load gives a well-defined
                        // shared_ptr copy; the local ref keeps the disk alive if device = {}
                        // fires on another thread immediately after.
                        if (!qs->tgt->_shutting_down.load(std::memory_order_seq_cst)) {
                            if (auto dev = qs->tgt->device.load()) dev->probe_tick(q);
                        }
                        if (qs->is_idle && !qs->tgt->_shutting_down.load(std::memory_order_relaxed))
                            submit_probe_timeout(q);
                    }
                } else if (base->_on_complete) {
                    // Generic sisl::async::cqe_state (e.g. craft_client's on-ring transport, submitting delivery
                    // SQEs on this queue's ring): dispatch through its own thunk, which resumes its own waiter.
                    // This is the whole of "cross-boundary completion is free" -- zero knowledge of the transport.
                    sisl::async::complete_cqe_state(*base, cqe->res);
                } else {
                    // Ours: a ublkpp::cqe_state. Resume the coroutine waiting on it; a throw out of resume() is
                    // reported as -EIO on the owning tag (why we resume _waiter directly, not via a noexcept thunk).
                    auto* state = static_cast< cqe_state* >(base);
                    state->_result = cqe->res;
                    state->_result_ready = true;
                    try {
                        if (auto h = std::exchange(state->_waiter, {})) h.resume(); // per-state resume (disk_task path)
                    } catch (std::exception const& e) {
                        TLOGE("I/O threw exception: [{}]", e.what())
                        if (state->_owner) ublksrv_complete_io(q, state->_owner->_tag, -EIO);
                    } catch (...) {
                        TLOGE("I/O threw unknown exception")
                        if (state->_owner) ublksrv_complete_io(q, state->_owner->_tag, -EIO);
                    }
                }
            } else {
                // ublk command CQE (FETCH/COMMIT) -- delegate to libublksrv
                ublksrv_handle_cmd_cqe(q, cqe);
            }
            ++count;
        }
        io_uring_cq_advance(ring, count);
        ublksrv_queue_update_idle(q, ret, count - probe_count);
        queue_done = ublksrv_queue_is_done(q);
    }

    co_await qs->scope.on_empty();
}

static void* ublksrv_queue_handler(std::shared_ptr< ublkpp_tgt_impl > target, int q_id, sem_t* queue_sem,
                                   int* queue_ok) {
    if (SISL_OPTIONS["sched"].as< std::string >() == "fifo") {
        sched_param sp{.sched_priority = sched_get_priority_max(SCHED_FIFO)};
        if (int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); rc != 0)
            TLOGE("queue {}: failed to set SCHED_FIFO: {}", q_id, strerror(rc))
    }
    auto qs = std::make_unique< ublkpp_queue_state >(target);

    // Initialize UBlkSrv IOUring queue and bind queue state pointer
    // NOTE: Removed IORING_SETUP_DEFER_TASK as it was blocking ublksrv_ctrl_del_dev,
    // look at adding this back as it theoretically could improve performance.
    auto q = ublksrv_queue_init_flags(target->ublk_dev, q_id, qs.get(),
                                      IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER);

    // Each thread writes to its own slot — no concurrent writes to the same location.
    // sem_post provides the release that pairs with start()'s sem_wait acquire, so no
    // atomic needed: start() reads queue_ok[] only after all sem_waits complete.
    if (!q) *queue_ok = 0;
    sem_post(queue_sem);
    target.reset();

    // If queue initialization failed, exit
    if (!q) {
        TLOGE("ublk dev queue {} init queue failed", q_id)
        return NULL;
    }

    TLOGD("tid {}: ublk dev queue {} started", ublksrv_gettid(), q->q_id)
    stdexec::sync_wait(run_queue_loop(q, qs.get()));
    TLOGD("ublk dev queue {} exited", q->q_id)
    ublksrv_queue_deinit(q);
    return NULL;
}

static std::expected< std::filesystem::path, std::error_condition > start(std::shared_ptr< ublkpp_tgt_impl > tgt) {
    TLOGD("Initializing Ctrl Device")
    if (!tgt->device_recovering) { // NORMAL Path
        if (tgt->ctrl_dev = ublksrv_ctrl_init(tgt->dev_data.get()); !tgt->ctrl_dev) {
            TLOGE("Cannot init disk {}", tgt->device.load())
            return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
        }
        if (auto ret = ublksrv_ctrl_add_dev(tgt->ctrl_dev); 0 > ret) {
            TLOGE("Cannot add disk {}: {}", tgt->device.load(), ret)
            return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
        }
    } else { // RECOVERY Path
        if (tgt->ctrl_dev = ublksrv_ctrl_recover_init(tgt->dev_data.get()); !tgt->ctrl_dev) {
            TLOGE("Cannot recover disk {}", tgt->device.load())
            return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
        }
        if (auto ret = ublksrv_ctrl_get_info(tgt->ctrl_dev); ret < 0) {
            TLOGE("Cannot get Ctrl Info for disk {}", tgt->device.load())
            return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
        }
        if (auto ret = ublksrv_ctrl_start_recovery(tgt->ctrl_dev); ret < 0) {
            TLOGE("Cannot start recovery for disk {}: {}", tgt->device.load(), ret)
            return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
        }
    }

    auto const dinfo = ublksrv_ctrl_get_dev_info(tgt->ctrl_dev);

    tgt->device_added = true;
    tgt->dev_data->dev_id = dinfo->dev_id;

    // Wait for Ctrl device to appear
    if (!check_dev(dinfo)) {
        TLOGE("dev {} never saw control device", tgt->dev_data->dev_id)
        return std::unexpected(std::make_error_condition(std::errc::no_such_device));
    }

    if (auto ret = ublksrv_ctrl_get_affinity(tgt->ctrl_dev); 0 > ret) {
        TLOGE("dev {} get affinity failed {}", tgt->dev_data->dev_id, ret)
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }

    TLOGD("Start ublksrv io daemon {}-{}", "ublkpp", tgt->dev_data->dev_id)

    // Target is about to initialize! Insert into our map
    {
        auto lk = std::scoped_lock< std::mutex >(_map_lock);
        _init_map.emplace(std::make_pair(tgt->ctrl_dev, tgt));
    }

    tgt->ublk_dev = ublksrv_dev_init(tgt->ctrl_dev);

    {
        auto lk = std::scoped_lock< std::mutex >(_map_lock);
        _init_map.erase(tgt->ctrl_dev);
    }
    if (!tgt->ublk_dev) {
        TLOGE("dev-{} start ublksrv failed", tgt->dev_data->dev_id)
        return std::unexpected(std::make_error_condition(std::errc::no_such_device));
    }

    // Setup Queues
    sem_t queue_sem;
    sem_init(&queue_sem, 0, 0);
    auto queue_ok = std::vector< int >(dinfo->nr_hw_queues, 1);
    for (auto i = 0; i < dinfo->nr_hw_queues; ++i) {
        tgt->queue_handlers.push_back(sisl::named_thread(fmt::format("q_{}_{}", tgt->dev_data->dev_id, i),
                                                         ublksrv_queue_handler, tgt, i, &queue_sem, &queue_ok[i]));
    }
    auto const recovery = tgt->device_recovering;
    auto const dev_name = fmt::format("{}", *tgt->device.load());

    auto ctrl_dev = tgt->ctrl_dev;
    auto dev_sptr = tgt->device.load(); // keep disk alive through start() after tgt.reset()
    auto dev_ptr = dev_sptr.get();
    auto const dev_id = tgt->dev_data->dev_id;

    // Wait for Queues to start
    for (auto i = 0; i < dinfo->nr_hw_queues; ++i)
        sem_wait(&queue_sem);
    sem_destroy(&queue_sem);

    if (std::ranges::any_of(queue_ok, [](int ok) { return !ok; })) {
        TLOGE("dev {} failed: one or more queues did not initialize", dev_id)
        return std::unexpected(std::make_error_condition(std::errc::io_error));
    }

    // Drop start()'s reference; the impl is now owned by ublkpp_tgt._p and each queue
    // thread's qs->tgt (shared_ptr<ublkpp_tgt_impl>). The impl stays alive until both
    // ublkpp_tgt is destroyed AND all queue threads exit and their qs destructs.
    tgt.reset();

    // Start processing I/Os
    if (!recovery) {
        // ublksrv_ctrl_set_params is missing const-correctness in the C header; the call does
        // not mutate. Cast away const at the kernel-API boundary only.
        if (auto err =
                ublksrv_ctrl_set_params(ctrl_dev, const_cast< ublk_params* >(detail::params_access::of(*dev_ptr)));
            err)
            return std::unexpected(std::error_condition(err, std::system_category()));
        if (auto err = ublksrv_ctrl_start_dev(ctrl_dev, getpid()); 0 > err)
            return std::unexpected(std::error_condition(err, std::system_category()));
    } else if (auto err = ublksrv_ctrl_end_recovery(ctrl_dev, getpid()); 0 > err) {
        return std::unexpected(std::error_condition(err, std::system_category()));
    }

    static auto const sys_path = std::filesystem::path{"/"} / "dev";
    auto const res = sys_path / fmt::format("ublkb{}", dev_id);
    TLOGI("{} exposed as UBD device: [{}]", dev_name, res.native());
    return res;
}

// See all_idle() for the memory-ordering argument.
void ublkpp_tgt_impl::try_drain() {
    if (!_shutting_down.load(std::memory_order_acquire)) return;
    if (metrics.all_idle()) {
        bool expected = false;
        if (_device_reset_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            TLOGI("All I/O drained after shutdown - flushing backing store")
            device = disk_handle{};
            _drain_complete.store(true, std::memory_order_release);
            _drain_complete.notify_all();
        }
    }
}

static exec::task< void > __handle_io_async(ublksrv_queue const* q, ublk_io_data const* data) {
    auto* qs = static_cast< ublkpp_queue_state* >(q->private_data);

    auto io = reinterpret_cast< async_io* >(data->private_data);
    io->_pool.clear();
    io->_tag = data->tag;

    auto const op = ublksrv_get_op(data->iod);

    // Increment before the shutdown gate: a concurrent drain check that sees counters==0 would
    // otherwise assign device = {} while we are still about to use the raw device* pointer.
    // With the increment already in place, any concurrent drain check sees counter > 0.
    // The seq_cst increment participates in the C++ total order S alongside begin_shutdown's
    // seq_cst store and all_idle()'s seq_cst loads. Either the increment precedes the store in
    // S (begin_shutdown's counter reads see it → skips reset) or the store precedes the
    // increment in S (our gate check below sees _shutting_down=true → drops).
    qs->tgt->metrics.record_queue_depth_change(q, op, true);

    // Drain gate: during shutdown DROP every op (leave it uncompleted / OWNED_BY_SRV) so the kernel
    // requeues it to the next daemon under UBLK_F_USER_RECOVERY(_REISSUE) on exit. Completing with
    // -EAGAIN instead maps to BLK_STS_AGAIN, which the block layer logs as "nonblocking retry error"
    // and fails.
    if (qs->tgt->_shutting_down.load(std::memory_order_seq_cst)) {
        qs->tgt->metrics.record_queue_depth_change(q, op, false); // undo pre-gate increment (no-op for FLUSH)
        TLOGD("Dropping I/O [tag:{:#0x}] during shutdown", data->tag)
        qs->tgt->try_drain(); // dropped op may be the last in-flight
        co_return;
    }

    uint32_t bytes_transferred = 0;
    int result;
    if (op == UBLK_IO_OP_FLUSH) {
        result = 0;
    } else {
        auto* device = reinterpret_cast< ublk_disk* >(q->dev->tgt.tgt_data);
        auto const* iod = data->iod;
        // Frame-local: io_uring reads iov contents at submit time (deferred to the queue loop's
        // submit_and_wait_timeout). thread_local would be overwritten by sibling __handle_io_async
        // coroutines spawned in the same CQE batch before the kernel sees the SQE. The coroutine
        // frame is alive across co_await, so the iov is valid through the whole IO lifetime.
        iovec iov{.iov_base = reinterpret_cast< void* >(iod->addr), .iov_len = iod->nr_sectors << SECTOR_SHIFT};
        auto const io_start = std::chrono::steady_clock::now();
        result = co_await device->async_iov(q, data, &iov, 1, iod->start_sector << SECTOR_SHIFT);
        auto const latency_us = static_cast< uint64_t >(
            std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now() - io_start)
                .count());
        qs->tgt->metrics.record_io_latency(op, latency_us);
        // Last line of defense: ublk_drv treats a positive short READ/WRITE res as a
        // front-aligned partial completion (first res bytes done, tail requeued), which would
        // silently misplace any unfilled range mid-buffer. No driver may legitimately return a
        // positive non-length value for READ/WRITE (FLUSH/DISCARD/WRITE_ZEROES return 0 and are
        // exempt), so convert to -EIO and count it.
        if ((op == UBLK_IO_OP_READ || op == UBLK_IO_OP_WRITE) && result >= 0 &&
            static_cast< uint64_t >(result) != iov.iov_len) {
            TLOGE("Short {} [tag:{:#0x}]: completed {}B of {}B, failing I/O", op == UBLK_IO_OP_READ ? "READ" : "WRITE",
                  data->tag, result, iov.iov_len)
            qs->tgt->metrics.record_io_short(op);
            result = -EIO;
        }
        // iov_len not result: ublk delivers full completions; a positive result was validated
        // against iov_len above (and FLUSH/DISCARD paths return 0 on success by contract).
        if (result >= 0) bytes_transferred = static_cast< uint32_t >(iov.iov_len);
    }

    qs->tgt->metrics.record_queue_depth_change(q, op, false);
    if (bytes_transferred > 0) qs->tgt->metrics.record_io_bytes(op, bytes_transferred);

    if (0 > result) [[unlikely]] {
        TLOGE("Returning error for [tag:{:#0x}] [res:{}]", data->tag, result)
        qs->tgt->metrics.record_io_error(op);
    } else {
        TLOGT("I/O complete [tag:{:#0x}] [res:{}]", data->tag, result)
    }
    ublksrv_complete_io(q, data->tag, result);

    // Fire device = {} once the last in-flight op (dispatched before begin_shutdown) drains.
    if (qs->tgt->_shutting_down.load(std::memory_order_seq_cst)) qs->tgt->try_drain();
}

// I/O Handler, first entry-point to us for all I/O
static int handle_io_async(ublksrv_queue const* q, ublk_io_data const* data) {
    auto* qs = static_cast< ublkpp_queue_state* >(q->private_data);
    // scope.spawn() throws if the scope has been stopped, but that race cannot occur: the
    // ublksrv io_uring is fully drained before the queue state (and its async_scope) is
    // destroyed, so no new handle_io_async callbacks can arrive after stop is requested.
    // Any other exception (e.g. bad_alloc from coroutine frame or spawn control block)
    // must be caught here: if spawn throws, ublksrv_complete_io is never called and the
    // tag slot is permanently hung. No I/O was submitted so no data was committed;
    // EAGAIN is safe for the block layer to retry.
    try {
        qs->scope.spawn(stdexec::on(exec::inline_scheduler{}, __handle_io_async(q, data)));
    } catch (...) { // LCOV_EXCL_START
        TLOGE("handle_io_async: scope.spawn threw; completing tag {} with EAGAIN", data->tag)
        ublksrv_complete_io(q, data->tag, -EAGAIN);
    } // LCOV_EXCL_STOP
    return 0;
}

// Called in the context of start by ublksrv_dev_init()
static int init_tgt(ublksrv_dev* dev, int, int, char*[]) {
    // Find the registered disk in the disk map and set the tgt_data
    // to its RAW pointer. This will be handed to handle_io_async for each I/O
    // so we have the context of which disk is receiving I/O.
    auto tgt = std::shared_ptr< ublkpp_tgt_impl >();
    auto cdev = ublksrv_get_ctrl_dev(dev);
    {
        auto lk = std::scoped_lock< std::mutex >(_map_lock);
        if (auto it = _init_map.find(cdev); _init_map.end() != it) { tgt = it->second; }
    }
    if (!tgt) {
        TLOGE("Disk not found in map!")
        return -2;
    }
    auto ublk_disk = tgt->device.load();
    dev->tgt.tgt_data = ublk_disk.get();

    // TODO Ublk Recovery
    // if (ublksrv_is_recovering(cdev)) {
    //}

    auto ublksrv_tgt = &dev->tgt;
    ublksrv_tgt->io_data_size = sizeof(struct async_io);
    ublksrv_tgt->dev_size = ublk_disk->capacity();

    // Size the io_uring ring to hold all in-flight SQEs across the full queue depth.
    // prepare() with nullptr collects the SQE ceiling without triggering per-queue side effects
    // (e.g. Raid1 resync enable); each queue's pool is reserved separately in init_queue.
    // +1 per I/O slot for the ublksrv FETCH/COMMIT control SQEs that share the same ring.
    // +1 total for the idle probe timeout SQE, which may still be in-flight when I/O resumes.
    auto const max_sqes = ublk_disk->prepare(nullptr, 0).max_sqes_per_io;
    auto const qd = static_cast< unsigned int >(ublksrv_ctrl_get_dev_info(cdev)->queue_depth);
    ublksrv_tgt->tgt_ring_depth = qd * (static_cast< unsigned int >(max_sqes) + 1) + 1;

    // iouring FD 0 is reserved for the ublkc device; prepare is called per queue in init_queue.
    // NOTE: if future disks export non-empty FDs they must be registered here (before ublksrv_queue_init
    // calls io_uring_register_files). For now all disks return empty so init_queue suffices.
    ublksrv_tgt->nr_fds = 1;
    return 0;
}

static void deinit_tgt(const struct ublksrv_dev*) { TLOGD("Deinit tgt!") }

static void idle_transition(ublksrv_queue const* q, bool enter) {
    TLOGT("Idle Trans: {}", enter)
    auto* qs = static_cast< ublkpp_queue_state* >(q->private_data);
    qs->is_idle = enter;
    // On exit: let any in-flight probe timeout fire naturally; is_idle=false prevents
    // resubmission, and a spurious probe_tick during active I/O is harmless.
    if (enter) submit_probe_timeout(q);
}

static int init_queue(const struct ublksrv_queue* q, void**) {
    TLOGD("Init Queue")
    auto device = reinterpret_cast< ublk_disk* >(q->dev->tgt.tgt_data);
    // All current disk types return no FDs from per-queue init; non-empty means the FDs would go
    // unregistered with io_uring and fixed-file I/O would crash — treat it as a fatal init failure.
    auto const prep = device->prepare(q, 0);
    if (!prep.fds.empty()) return -1;
    // async_io is placement-new'd into ublksrv-calloc'd bytes; _pool is pre-reserved to the SQE
    // ceiling so push_back during I/O never reallocates and cqe_state* pointers stay stable.
    for (int i = 0; i < q->q_depth; ++i) {
        auto* io = new (ublksrv_io_private_data(q, i)) async_io{};
        io->_pool.reserve(prep.max_sqes_per_io);
    }
    return 0;
}

static void deinit_queue(const struct ublksrv_queue* q) {
    TLOGD("Deinit Queue")
    for (int i = 0; i < q->q_depth; ++i)
        static_cast< async_io* >(ublksrv_io_private_data(q, i))->~async_io();
}

// Setup ublksrv ctrl device and initiate adding the target to the ublksrv service and handle all device traffic
ublkpp_tgt::run_result_t ublkpp_tgt::run(boost::uuids::uuid const& vol_id, std::shared_ptr< ublk_disk > device,
                                         int device_id) {
    auto tgt = std::make_shared< ublkpp_tgt_impl >(vol_id, device);
    if (0 <= device_id) tgt->device_recovering = true;
    auto ublk_flags = unsigned(0);
    ublk_flags |= (unsigned)(UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE);
    if (0 < SISL_OPTIONS["feature_zero_copy"].count()) {
        TLOGI("Enabling zero-copy support...: {}", to_string(vol_id))
        ublk_flags |= (unsigned)(UBLK_F_SUPPORT_ZERO_COPY);
    }

    tgt->tgt_type = std::make_unique< ublksrv_tgt_type >(ublksrv_tgt_type{
        .handle_io_async = handle_io_async,
        .tgt_io_done = nullptr,  // handled inline in run_queue_loop
        .handle_event = nullptr, // handled inline in run_queue_loop
        .handle_io_background = nullptr,
        .usage_for_add = nullptr, // Not Implemented
        .init_tgt = init_tgt,
        .deinit_tgt = deinit_tgt,
        .alloc_io_buf = nullptr,    // Not Implemented
        .free_io_buf = nullptr,     // Not Implemented
        .idle_fn = idle_transition, // Called when I/O has stopped
        .type = 0,                  // Deprecated *DO NOT USE*
        .ublk_flags = ublk_flags,
        .ublksrv_flags = 0,
        .pad = 0, // Currently Clear
        .name = "ublkpp",
        .recovery_tgt = nullptr, // Deprecated *DO NOT USE*
        .init_queue = init_queue,
        .deinit_queue = deinit_queue,
        .reserved = {0, 0, 0, 0, 0} // Reserved
    });

    TLOGD("Starting {} [uuid:{}]", static_pointer_cast< ublk_disk >(device), to_string(vol_id))
    tgt->dev_data = std::make_unique< ublksrv_dev_data >(ublksrv_dev_data{
        .dev_id = device_id,
        .max_io_buf_bytes = SISL_OPTIONS["max_io_size"].as< uint32_t >(),
        .nr_hw_queues = SISL_OPTIONS["nr_hw_queues"].as< uint16_t >(),
        .queue_depth = SISL_OPTIONS["qdepth"].as< uint16_t >(),
        .tgt_type = "ublkpp",
        .tgt_ops = tgt->tgt_type.get(),
        .tgt_argc = 0,
        .tgt_argv = nullptr,
        .run_dir = nullptr,
        .flags = tgt->tgt_type->ublk_flags,
        .ublksrv_flags = tgt->tgt_type->ublksrv_flags,
        .reserved = {0, 0, 0, 0, 0, 0, 0},
    });
    auto res = start(tgt);
    if (!res) {
        tgt->destroy();
        return std::unexpected(res.error());
    }
    tgt->device_path = res.value();

    auto new_tgt = new ublkpp_tgt(tgt);
    return std::unique_ptr< ublkpp_tgt >(new_tgt);
}

ublkpp_tgt::ublkpp_tgt(std::shared_ptr< ublkpp_tgt_impl > p) : _p(p) {}

ublkpp_tgt::~ublkpp_tgt() = default;

std::filesystem::path ublkpp_tgt::device_path() const { return _p->device_path; }
std::shared_ptr< ublk_disk > ublkpp_tgt::device() const { return _p->device.load(); }
int ublkpp_tgt::device_id() const {
    RELEASE_ASSERT(_p->dev_data != nullptr, "device_id() called on a make_for_test() target (dev_data is null)");
    return _p->dev_data->dev_id;
}

void ublkpp_tgt::begin_shutdown() {
    // Relaxed load for the idempotency fast-path: benign optimisation. Correctness is
    // guaranteed by the CAS on _device_reset_done, not by this check.
    if (_p->_shutting_down.load(std::memory_order_relaxed)) {
        TLOGD("begin_shutdown() called again - already shutting down, ignoring")
        return;
    }
    // seq_cst: on x86, release compiles to a plain mov that sits in the store buffer.
    // The subsequent all_idle() counter reads go straight to cache, so the store may not be
    // visible to other threads when we read the counter — enabling a UAF. seq_cst uses
    // MFENCE / lock xchg, draining the store buffer before the counter reads and establishing
    // the total order the drain safety argument requires.
    _p->_shutting_down.store(true, std::memory_order_seq_cst);
    // Idle-drain: if no I/O is in-flight at the moment the flag is set, no __handle_io_async
    // completion will ever reach the post-decrement drain check. Assign device = {} here
    // instead so clean_unmount=1 is written even on a quiesced volume.
    //
    // device = {} destroys Raid1Disk inline: its destructor calls _resync_task->stop()
    // (joins the resync thread) then writes clean_unmount=1. This call may therefore block
    // until the resync task drains. Do not invoke from a signal handler.
    if (_p->metrics.all_idle()) {
        bool expected = false;
        if (_p->_device_reset_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            TLOGI("No I/O in-flight at shutdown - flushing backing store immediately")
            _p->device = disk_handle{};
            _p->_drain_complete.store(true, std::memory_order_release);
            _p->_drain_complete.notify_all();
        }
    }
}

void ublkpp_tgt::wait_for_drain() {
    RELEASE_ASSERT(_p->_shutting_down.load(std::memory_order_relaxed),
                   "wait_for_drain() called without begin_shutdown() — would block forever");
    _p->_drain_complete.wait(false, std::memory_order_acquire);
}

void ublkpp_tgt::remove(std::unique_ptr< ublkpp_tgt > tgt) { tgt->_p->destroy(); }

ublkpp_tgt ublkpp_tgt_test_peer::make(disk_handle dev) {
    return ublkpp_tgt{std::make_shared< ublkpp_tgt_impl >(boost::uuids::uuid{}, std::move(dev))};
}
void ublkpp_tgt_test_peer::try_drain(ublkpp_tgt& tgt) { tgt._p->try_drain(); }
UblkIOMetrics& ublkpp_tgt_test_peer::metrics(ublkpp_tgt& tgt) { return tgt._p->metrics; }

void ublkpp_tgt_impl::destroy() {
    auto const str_id = fmt::format("Device {} [uuid:{}]", device_path.native(), to_string(volume_uuid));
    // First send a signal to stop the ublk device and exit all I/O queues
    if (ublk_dev) {
        TLOGD("Stopping {}", str_id)
        ublksrv_ctrl_stop_dev(ctrl_dev);
    }

    // Wait for all queue_handler threads to exit
    TLOGD("Waiting for I/O to stop on {}", str_id)
    for (auto& q : queue_handlers)
        if (q.joinable()) q.join();

    // De-allocate the ublksrv device and free all unowned memory
    if (ublk_dev) {
        TLOGD("De-allocate {}", str_id)
        ublksrv_dev_deinit(ublk_dev);
        ublk_dev = nullptr;
    }

    // De-allocate our devices now, will cause things like RAID-1 to flush Bitmaps
    // and all FSDisk will close their fd's
    device = disk_handle{};

    // Delete the ublk control object (ublkc must be closed!)
    if (device_added) {
        TLOGD("Stopping Control for {}", str_id)
        ublksrv_ctrl_del_dev(ctrl_dev);
    }

    // De-allocate the ublksrv control device finally
    if (ctrl_dev) {
        TLOGD("De-allocate Control for {}", str_id)
        ublksrv_ctrl_deinit(ctrl_dev);
        ctrl_dev = nullptr;
    }
    TLOGI("Stopped {}", str_id)
}

ublkpp_tgt_impl::~ublkpp_tgt_impl() {
    // Queue threads hold qs->tgt (shared_ptr<ublkpp_tgt_impl>) for their entire lifetime, so
    // this destructor fires only when both ublkpp_tgt._p and every qs->tgt have been released.
    // The most common case: the last queue thread to exit destructs qs, releasing its copy and
    // triggering this destructor from inside ublksrv_queue_handler. At that point the thread
    // is finishing but queue_handlers[i].joinable() is still true (not yet joined/detached).
    // Joining from within the thread itself would deadlock; detach marks it as self-managed.
    // The RELEASE_ASSERT ensures begin_shutdown() was called before this point.
    for (auto& q : queue_handlers) {
        if (q.joinable()) {
            RELEASE_ASSERT(_shutting_down.load(std::memory_order_relaxed),
                           "ublkpp_tgt destroyed with live queue thread; begin_shutdown() was not called");
            q.detach();
        }
    }
}

uint64_t ublkpp_tgt::estimate_queue_memory() noexcept {
    // Read current runtime configuration
    auto const qdepth = SISL_OPTIONS["qdepth"].as< uint16_t >();
    auto const max_io_size = SISL_OPTIONS["max_io_size"].as< uint32_t >();
    auto const nr_hw_queues = SISL_OPTIONS["nr_hw_queues"].as< uint16_t >();

    uint64_t ublksrv_buffers = static_cast< uint64_t >(nr_hw_queues) *
        ((static_cast< uint64_t >(qdepth) * max_io_size) + (qdepth * sizeof(async_io)));

    // Thread stacks (OS allocates per queue handler)
    uint64_t thread_overhead = static_cast< uint64_t >(nr_hw_queues) * k_thread_stack_size;

    // Target implementation overhead (small, fixed)
    constexpr uint64_t target_overhead = k_page_size; // ~4 KiB for metadata

    return ublksrv_buffers + thread_overhead + target_overhead;
}

} // namespace ublkpp
