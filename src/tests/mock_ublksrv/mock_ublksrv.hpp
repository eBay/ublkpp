#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include <liburing.h>
#include <ublksrv.h>

#include "ublkpp/lib/cqe_state.hpp"
#include "ublkpp/lib/ublk_disk.hpp"

namespace ublkpp {

// Drives ublk_disk I/O directly with a real io_uring, bypassing ublkpp_tgt coroutines
// and the ublk kernel module. Suitable for CI where the ublk module is unavailable.
//
// submit_io starts a disk async_iov task and returns the number of registered CqeStates.
// inject_cqe delivers synthetic results for each suspended co_await *state without io_uring round-trips.
// poll() drains real io_uring CQEs for disks that submit actual SQEs (e.g. FSDisk).
//
// Completion model mirrors the production target (__handle_io_async in ublkpp_tgt.cpp): each submit
// wraps async_iov in a recorder coroutine whose tail pushes {tag, result} onto _completed, so the tag
// travels in the coroutine frame and no CQE→tag attribution is ever needed — an IO finished
// transitively by a foreign transport CQE (CraftDisk) completes exactly like a RAID/fs one. Each
// completion is delivered EXACTLY ONCE, by whichever of poll() / inject_cqe() drains _completed next.
class MockUblksrv {
public:
    struct Completion {
        int tag;
        int result;
    };

    // disk: disk to drive (FSDisk, Raid0Disk, Raid1Disk, ...)
    // q_depth: number of concurrent I/O slots (must be >= fio iodepth)
    // nr_queues: number of simulated queue threads (calls prepare once per queue)
    explicit MockUblksrv(std::shared_ptr< ublk_disk > disk, int q_depth = 128, int nr_queues = 1);
    ~MockUblksrv();

    MockUblksrv(MockUblksrv const&) = delete;
    MockUblksrv& operator=(MockUblksrv const&) = delete;

    // Start disk async_iov task; runs until first co_await *state.
    // tag: slot index [0, q_depth), op: UBLK_IO_OP_READ / _WRITE / _FLUSH / _DISCARD
    // start_sector: byte offset >> SECTOR_SHIFT, nr_sectors: byte length >> SECTOR_SHIFT
    // buf: sector-aligned buffer (caller owns lifetime)
    // Returns the number of CqeStates registered (one per pending SQE).
    io_result submit_io(int tag, uint8_t op, uint64_t start_sector, uint32_t nr_sectors, void* buf);

    // Drain io_uring CQEs until at least min_completions are collected or timeout expires.
    std::vector< Completion > poll(int min_completions, std::chrono::milliseconds timeout);

    // Deliver a synthetic result to the cqe_state currently suspended in the disk_task for the
    // given tag (call once per awaited stripe for multi-stripe IOs), then drain _completed.
    // Returns every completion the resume produced — usually the given tag's, plus any tasks it
    // unblocked as a side effect, plus any not-yet-drained synchronous completion (e.g. flush).
    std::vector< Completion > inject_cqe(int tag, int result);

    // Per-tag sector-aligned I/O buffer (max_io_size = DEF_BUF_SIZE bytes).
    void* io_buf(int tag);

    // Direct reference to the iovec passed to async_iov for the given tag slot.
    // Allows tests to mutate the caller-side iov after submission to verify that the
    // disk under test has snapshotted the values rather than holding a raw pointer.
    iovec& iov_ref(int tag) noexcept { return _tags[tag].iov; }

    int q_depth() const noexcept { return _q_depth; }
    int nr_queues() const noexcept { return static_cast< int >(_queues.size()); }
    ublksrv_queue const* queue(int q_id = 0) const noexcept { return &_queues[q_id]; }
    uint64_t capacity_sectors() const noexcept;

private:
    struct TagState {
        ublksrv_io_desc iod{};
        ublk_io_data data{};
        // Per-tag storage — io_uring reads iovecs at submit time (which is deferred to
        // poll()), so a single shared/thread_local iovec would be overwritten by later
        // submit_io() calls before the kernel sees the earlier SQE.
        iovec iov{};
    };

    void process_cqe(io_uring_cqe* cqe);

    // Recorder coroutine — the mock's __handle_io_async: co_awaits the disk's async_iov and records
    // {tag, result} on _completed when the whole IO finishes, whichever CQE (ours or foreign) drove
    // the final resume.
    disk_task< int > run_io(int tag, int qid, uint8_t op, uint64_t addr);

    int _q_depth;
    ublksrv_dev _dev{};
    std::vector< ublksrv_queue > _queues;
    io_uring _ring{};
    std::shared_ptr< ublk_disk > _disk;
    std::vector< TagState > _tags;

    // async_io _pool — one per tag, mirrors what init_queue does via placement new
    std::vector< async_io > _io_states;

    // hot_task handles for the async API path — one optional slot per tag. The slot owns the
    // recorder frame (and, through it, the disk_task frame); reset on tag reuse.
    std::vector< std::optional< hot_task< int > > > _async_tasks;

    // Completions recorded by run_io tails, drained (exactly once) by poll() / inject_cqe().
    std::vector< Completion > _completed;

    // Aligned I/O buffers — one per tag, DEF_BUF_SIZE bytes each
    std::vector< uint8_t > _io_buf_storage;
    std::vector< void* > _io_buf_ptrs;
};

} // namespace ublkpp
