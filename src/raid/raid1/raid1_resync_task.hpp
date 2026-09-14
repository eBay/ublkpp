#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <sys/uio.h>
#include <thread>

#include "metrics/ublk_raid_metrics.hpp"
#include "raid1_superblock.hpp"
#include "region_tracker.hpp"
#include "ublkpp/raid.hpp"

namespace ublkpp::raid1 {

using namespace std::chrono_literals;
constexpr auto k_state_spin_time = 50us;
constexpr uint32_t k_default_slot_count = 256;

class Bitmap;
class MirrorDevice;

// State transitions:
//   IDLE → ACTIVE (launch())
//   ACTIVE ⟷ SLEEPING (yield for I/O)
//   any → STOPPING (shutdown requested)
//   STOPPING → IDLE (shutdown complete)
ENUM(resync_state, uint32_t, IDLE = 0, ACTIVE = 1, SLEEPING = 2, STOPPING = 3);

// State transition actions for __transition_to helper
enum class transition_action : uint8_t {
    RETRY,            // Continue CAS loop immediately
    RETRY_WITH_SLEEP, // Sleep k_state_spin_time then retry
    SUCCESS,          // Exit loop (CAS succeeded or found target state)
    EARLY_EXIT        // Exit caller function immediately
};

// Result from state transition handler callback
struct transition_result {
    resync_state next_state; // State to expect on next retry
    transition_action action;
};

class Raid1ResyncTask {

    // Global counter for active resyncs across all RAID1 devices
    static inline std::atomic_uint32_t s_active_resyncs{0};

    // Number of times __yield() has been called; used by tests to wait for at least one sweep
    // without relying on wall-clock timing.
    std::atomic< uint64_t > _yield_count{0};

    std::shared_ptr< raid1::Bitmap > const _dirty_bitmap;
    std::shared_ptr< ublkpp::UblkRaidMetrics > const _metrics;

    // The smallest I/O both devices support (RAID logical block size)
    uint32_t const _io_size;
    // The largest I/O both devices support
    uint32_t const _max_size;
    // This is the offset we should copy the disks @ to avoid writing on the BITMAP itself.
    uint64_t const _offset;

    // Fallback copy-mode storage, used only when no external mode source is supplied -- mode-agnostic
    // unit tests (the concurrency/region-tracker suites) that just want the default BLIND behavior.
    std::atomic< resync_copy_mode > _owned_mode{resync_copy_mode::BLIND};
    // The single authoritative (persisted) copy mode: Raid1Disk::_resync_mode in production, the copy-
    // mode tests' own atomic otherwise, else &_owned_mode. The task reads it fresh per copy so an
    // I/O-path re-dirty or this run's own Phase-2 conflict is observed immediately, and publishes the
    // ZERO_TEST -> CHECK downgrade back through it so the change is durable across a stop/relaunch.
    // Owned by Raid1Disk, which joins this task before the atomic is destroyed.
    std::atomic< resync_copy_mode >* const _live_mode;

    std::atomic< resync_state > _state{resync_state::IDLE};
    static_assert(std::atomic< resync_state >::is_always_lock_free);

    // Tracks the LBA range of each in-flight write. Resync checks for overlap before
    // and after each copy so it only skips regions that actually conflict with a write;
    // unrelated regions proceed without any global pause.
    RegionTracker _region_tracker;

    std::mutex _launch_lock;
    std::thread _resync_task;

    // State access helpers
    resync_state __load_state() const noexcept { return _state.load(std::memory_order_acquire); }

    bool __cas_state(resync_state& expected, resync_state desired) noexcept {
        return _state.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // iov: source/scratch buffer. cmp_iov: destination-read buffer (CHECK, and ZERO_TEST which may
    // downgrade). The effective mode is re-read from _live_mode per copy; a write conflicting with a
    // ZERO_TEST copy publishes ZERO_TEST -> CHECK through _live_mode so the deferred region re-syncs
    // via compare (not a stale zero-skip) and the downgrade persists. blind_fallback pins the run to
    // BLIND when the compare buffer could not be allocated -- a run-local fallback, never published.
    resync_state __run(auto& clean_mirror, auto& dirty_mirror, iovec* iov, iovec* cmp_iov,
                       bool blind_fallback) noexcept;

    // Generic state transition helper - reduces duplication across launch/stop.
    // noinline: gcov attributes inlined template instructions to the call-site line numbers
    // rather than to the template body, making the entire retry loop appear uncovered.
    template < typename StateHandler >
    [[gnu::noinline]] bool __transition_to(resync_state initial, resync_state target, StateHandler&& handler) noexcept;

    void _start(std::string str_uuid, std::shared_ptr< MirrorDevice >& clean_mirror,
                std::shared_ptr< MirrorDevice >& dirty_mirror, std::function< bool() >&& complete);

    resync_state __yield(std::chrono::microseconds const yield_for, std::chrono::microseconds const spin_time) noexcept;

    void __clean(uint64_t addr, uint32_t len, MirrorDevice& clean_device);

public:
    Raid1ResyncTask(std::shared_ptr< raid1::Bitmap >& bitmap, uint64_t offset, uint32_t io_size, uint32_t max_io,
                    std::atomic< resync_copy_mode >* live_mode = nullptr, uint32_t slot_count = k_default_slot_count,
                    uint32_t chunk_size = k_min_chunk_size,
                    std::shared_ptr< ublkpp::UblkRaidMetrics > metrics = nullptr);
    ~Raid1ResyncTask() noexcept;

    // Probe a mirror device: reads at reserved_size, clears unavail on success,
    // sets unavail on failure. Returns true if device is available.
    static bool probe_mirror(MirrorDevice& mirror, uint64_t reserved_size) noexcept;

    // The copy mode is read from _live_mode (Raid1Disk::_resync_mode); see resync_copy_mode.
    void launch(std::string const& str_uuid, std::shared_ptr< MirrorDevice > clean_mirror,
                std::shared_ptr< MirrorDevice > dirty_mirror, std::function< bool() >&& complete);

    // Generic method to move Resync StateMachine to STOPPING
    void stop() noexcept;

    void enqueue_write(uint64_t lba, uint32_t len) noexcept { _region_tracker.track(lba, len); }

    void dequeue_write(uint64_t lba, uint32_t len) noexcept { _region_tracker.untrack(lba, len); }

    // Number of times __yield() has been called. Tests poll this to wait for at least one
    // resync sweep without relying on wall-clock timing.
    uint64_t yield_count() const noexcept { return _yield_count.load(std::memory_order_acquire); }

    bool is_copying() const noexcept {
        auto s = _state.load(std::memory_order_acquire);
        return s == resync_state::ACTIVE || s == resync_state::SLEEPING;
    }
};

// RAII guard that calls enqueue_write() on construction and dequeue_write() on destruction.
// Call release() to dequeue early (e.g. at a specific co_await point in a coroutine).
class ResyncWriteGuard {
public:
    ResyncWriteGuard(Raid1ResyncTask& task, uint64_t lba, uint32_t len) noexcept : _task(&task), _lba(lba), _len(len) {
        _task->enqueue_write(_lba, _len);
    }
    ~ResyncWriteGuard() noexcept {
        if (_task) _task->dequeue_write(_lba, _len);
    }
    void release() noexcept {
        if (_task) {
            _task->dequeue_write(_lba, _len);
            _task = nullptr;
        }
    }
    ResyncWriteGuard(ResyncWriteGuard&&) = delete;
    ResyncWriteGuard(ResyncWriteGuard const&) = delete;
    ResyncWriteGuard& operator=(ResyncWriteGuard&&) = delete;
    ResyncWriteGuard& operator=(ResyncWriteGuard const&) = delete;

private:
    Raid1ResyncTask* _task;
    uint64_t _lba;
    uint32_t _len;
};

} // namespace ublkpp::raid1
