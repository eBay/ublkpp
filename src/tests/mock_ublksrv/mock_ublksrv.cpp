#include "mock_ublksrv.hpp"

extern "C" {
#include <unistd.h>
}

#include <stdexcept>

#include <ublksrv.h>

#include "lib/common.hpp"

namespace ublkpp {

// Matches ublkpp_tgt default max_io_buf_bytes (DEF_BUF_SIZE = 512 KiB)
static constexpr size_t k_max_io_size = DEF_BUF_SIZE;
static constexpr size_t k_sector_align = 512;

MockUblksrv::MockUblksrv(std::shared_ptr< ublk_disk > disk, int q_depth, int nr_queues) :
        _q_depth(q_depth),
        _disk(std::move(disk)),
        _tags(q_depth),
        _queues(nr_queues),
        _io_states(q_depth),
        _async_tasks(q_depth) {
    // q_depth * 4 gives headroom for RAID1 write amplification (2x replicas +
    // 2x bitmap SQEs per user write) without false "ring full" auto-submits.
    if (io_uring_queue_init(q_depth * 4, &_ring, 0) < 0) throw std::runtime_error("io_uring_queue_init failed");

    // Populate ublksrv_dev so disk code can reach tgt_data and ring
    _dev.tgt.tgt_data = _disk.get();
    _dev.tgt.dev_size = _disk->capacity();
    _dev.tgt.tgt_ring_depth = static_cast< unsigned >(q_depth);
    _dev.tgt.nr_fds = 1; // slot 0 reserved (mirrors ublkpp_tgt convention)

    // Populate ublksrv_queue structs before calling prepare so that any implementation
    // which reads ring_ptr or dev inside prepare sees valid values.
    for (int qi = 0; qi < nr_queues; ++qi) {
        _queues[qi].q_id = qi;
        _queues[qi].q_depth = q_depth;
        _queues[qi].ring_ptr = &_ring;
        _queues[qi].dev = &_dev;
        _queues[qi].private_data = nullptr;
    }

    // Simulate init_queue: call prepare once per queue thread so the disk can count queues
    // and perform per-queue initialization (e.g. Raid1Disk sets _nr_hw_queues and enables resync).
    size_t max_sqes_per_io = 1;
    for (int qi = 0; qi < nr_queues; ++qi) {
        auto const prep = _disk->prepare(&_queues[qi], _dev.tgt.nr_fds);
        for (auto const fd : prep.fds) {
            if (_dev.tgt.nr_fds < UBLKSRV_TGT_MAX_FDS) _dev.tgt.fds[_dev.tgt.nr_fds++] = fd;
        }
        max_sqes_per_io = prep.max_sqes_per_io;
    }

    // Wire up per-tag data.iod pointers and async_io backing storage; pre-reserve pool to the
    // SQE ceiling so push_back during I/O never reallocates and cqe_state* pointers stay stable.
    for (int tag = 0; tag < q_depth; ++tag) {
        _tags[tag].data.tag = tag;
        _tags[tag].data.iod = &_tags[tag].iod;
        _tags[tag].data.private_data = &_io_states[tag];
        _io_states[tag]._tag = tag;
        _io_states[tag]._pool.reserve(max_sqes_per_io);
    }

    // Allocate sector-aligned I/O buffers (one per tag)
    size_t const stride = k_max_io_size + k_sector_align;
    _io_buf_storage.resize(q_depth * stride);
    _io_buf_ptrs.resize(q_depth);
    for (int tag = 0; tag < q_depth; ++tag) {
        auto raw = reinterpret_cast< uintptr_t >(_io_buf_storage.data() + tag * stride);
        auto aligned = (raw + k_sector_align - 1) & ~(k_sector_align - 1);
        _io_buf_ptrs[tag] = reinterpret_cast< void* >(aligned);
    }
}

MockUblksrv::~MockUblksrv() {
    io_uring_queue_exit(&_ring);
    for (unsigned i = 1; i < _dev.tgt.nr_fds; ++i) {
        if (_dev.tgt.fds[i] > 0) close(_dev.tgt.fds[i]);
    }
}

// The mock's __handle_io_async (ublkpp_tgt.cpp): the tag lives in this frame, so when the disk_task
// finishes — resumed by one of OUR per-IO CQEs (RAID/fs) or transitively by a FOREIGN transport CQE
// (CraftDisk) — final_suspend symmetric-transfers here and the completion is recorded with zero
// CQE→tag attribution. FLUSH is never dispatched to the disk (mirrors the pre-recorder behavior);
// it records synchronously during .start().
disk_task< int > MockUblksrv::run_io(int tag, int qid, uint8_t op, uint64_t addr) {
    int res = 0;
    if (op != UBLK_IO_OP_FLUSH)
        res = co_await _disk->async_iov(&_queues[qid], &_tags[tag].data, &_tags[tag].iov, 1, addr);
    _completed.push_back({tag, res});
    co_return res;
}

io_result MockUblksrv::submit_io(int tag, uint8_t op, uint64_t start_sector, uint32_t nr_sectors, void* buf) {
    auto& ts = _tags[tag];
    ts.iod.op_flags = op;
    ts.iod.nr_sectors = nr_sectors;
    ts.iod.start_sector = start_sector;
    ts.iod.addr = reinterpret_cast< uint64_t >(buf);

    // Reset async_io state between IOs on the same tag slot
    _io_states[tag]._pool.clear();
    _async_tasks[tag].reset();

    ts.iov.iov_base = reinterpret_cast< void* >(ts.iod.addr);
    ts.iov.iov_len = ts.iod.nr_sectors << SECTOR_SHIFT;
    int const qid = tag % static_cast< int >(_queues.size());
    _async_tasks[tag].emplace(run_io(tag, qid, op, ts.iod.start_sector << SECTOR_SHIFT).start());
    // Pool size == number of CqeStates registered (one per pending stripe SQE); 0 means the IO
    // completed synchronously (flush, pre-SQE error) and is already sitting on _completed.
    return io_result{_io_states[tag]._pool.size()};
}

void MockUblksrv::process_cqe(io_uring_cqe* cqe) {
    if (!sisl::async::is_managed_user_data(cqe->user_data)) return (void)io_uring_cqe_seen(&_ring, cqe);
    // Decode to the SHARED base and mirror run_queue_loop's dispatch. A managed CQE is one of: a probe-null
    // sentinel; a FOREIGN generic cqe_state (craft_client's on-ring cqe_awaitable, _on_complete set) that a
    // transport submitted on this ring; or ONE OF OURS (ublkpp::cqe_state, _on_complete null). Branch on
    // _on_complete BEFORE touching _waiter, which lives at our layout's offset a foreign state does not share.
    auto* base = static_cast< sisl::async::cqe_state* >(sisl::async::decode_managed_user_data(cqe->user_data));
    if (!base) return (void)io_uring_cqe_seen(&_ring, cqe); // managed-null (probe timeout) sentinel
    int const res = cqe->res;
    io_uring_cqe_seen(&_ring, cqe); // consume before resuming so a resumed leg's own peek sees the next CQE

    if (base->_on_complete) {
        sisl::async::complete_cqe_state(*base, res); // craft on-ring: dispatch via its thunk (resumes its waiter)
        return;
    }
    auto* state = static_cast< cqe_state* >(base);
    state->_result = res;
    state->_result_ready = true;
    if (auto h = std::exchange(state->_waiter, {})) h.resume();
    // No completion reporting here: whichever resume chain finishes an IO — this one or a foreign
    // dispatch above — runs the recorder's tail (run_io), which records {tag, result} on _completed.
}

std::vector< MockUblksrv::Completion > MockUblksrv::inject_cqe(int tag, int result) {
    // Find the cqe_state currently suspended in the disk_task (_waiter is set). No suspended state
    // means the task completed synchronously (e.g. flush) and its completion is already recorded;
    // result is ignored and the drain below returns it.
    for (auto& s : _io_states[tag]._pool) {
        if (s._waiter) {
            s._result = result;
            s._result_ready = true;
            if (auto h = std::exchange(s._waiter, {})) h.resume();
            break;
        }
    }
    return std::exchange(_completed, {});
}

std::vector< MockUblksrv::Completion > MockUblksrv::poll(int min_completions, std::chrono::milliseconds timeout) {
    // Drain first: IOs that completed synchronously (flush, pre-SQE error, craft inline completion)
    // recorded themselves during submit_io and produce no CQE to wait for.
    std::vector< Completion > completions = std::exchange(_completed, {});
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (static_cast< int >(completions.size()) < min_completions) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        auto remaining_ms = std::chrono::duration_cast< std::chrono::milliseconds >(deadline - now);
        __kernel_timespec ts{.tv_sec = remaining_ms.count() / 1000,
                             .tv_nsec = (remaining_ms.count() % 1000) * 1'000'000LL};

        // Match the production queue loop: submit pending SQEs and wait for at
        // least one CQE in a single syscall. Callers of async_iov rely on the
        // event loop to submit; using io_uring_wait_cqe_timeout here would leave
        // queued SQEs in the SQ forever and hang.
        io_uring_cqe* cqe = nullptr;
        int r = io_uring_submit_and_wait_timeout(&_ring, &cqe, 1, &ts, nullptr);
        if (r == -ETIME || r == -EINTR || r < 0 || cqe == nullptr) break;

        // Process this CQE then drain any additional ones that are ready
        do {
            process_cqe(cqe);
        } while (io_uring_peek_cqe(&_ring, &cqe) == 0 && cqe != nullptr);

        // Collect whatever the batch's resume chains recorded (run_io tails), foreign-driven or not.
        for (auto const& c : _completed)
            completions.push_back(c);
        _completed.clear();
    }

    return completions;
}

void* MockUblksrv::io_buf(int tag) { return _io_buf_ptrs[tag]; }

uint64_t MockUblksrv::capacity_sectors() const noexcept { return _disk->capacity() >> SECTOR_SHIFT; }

} // namespace ublkpp
