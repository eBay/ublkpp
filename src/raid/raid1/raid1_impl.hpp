#pragma once

#include <memory>
#include <optional>

#include "ublkpp/raid.hpp"
#include "metrics/ublk_raid_metrics.hpp"
#include "raid1_superblock.hpp"

namespace ublkpp {

namespace raid1 {

// Forward declarations
class Bitmap;
class Raid1ResyncTask;
struct RouteState;

struct MirrorDevice {
    MirrorDevice(boost::uuids::uuid const& uuid, std::shared_ptr< ublk_disk > device);
    std::shared_ptr< ublk_disk > const disk;
    std::shared_ptr< SuperBlock > sb; // Only used during load_superblock time
    std::atomic_flag
        unavail; // not ready for IO; also set at startup self-heal to route away from the stale leg during resync

    // new_device is also promoted on a >1 age gap, so it does NOT mean "reads zero". sb_was_fresh is
    // the genuine no-superblock truth captured at load; only such a leg may rebuild via ZERO_TEST.
    bool new_device{true};
    bool sb_was_fresh{true};
};

class Raid1Disk : public ublk_disk {
    boost::uuids::uuid const _uuid;
    std::string const _str_uuid;
    uint64_t _reserved_size{0UL};

    std::shared_ptr< MirrorDevice > _device_a;
    std::shared_ptr< MirrorDevice > _device_b;

    // Persistent state
    std::shared_ptr< raid1::SuperBlock > _sb;
    std::shared_ptr< raid1::Bitmap > _dirty_bitmap;

    // Runtime cached state (to avoid races on _sb bitfields)
    std::atomic< raid1::read_route > _read_route_cache{raid1::read_route::EITHER};

    // Metrics
    std::shared_ptr< ublkpp::UblkRaidMetrics > _raid_metrics;
    // Active Re-Sync Task
    std::atomic< bool > _resync_enabled{true};
    uint32_t _resync_write_cap{6};

    // Copy mode for the current/next resync, read by toggle_resync at every launch and persisted
    // with every __write_sb, so a clean degraded restart resumes it (an unclean one forces CHECK;
    // see __init_bitmap_and_degraded_route). Set by the degraded-init branches and swap_device;
    // reset to BLIND when the array becomes clean. Read/written across queue/control/resync threads.
    std::atomic< resync_copy_mode > _resync_mode{resync_copy_mode::BLIND};
    std::shared_ptr< Raid1ResyncTask > _resync_task;

    // Guards: (1) swap_device() - serializes concurrent callers on _device_a/_device_b mutations.
    //         (2) _pending_results - serializes prepare() insertions across queue threads.
    std::mutex _ctrl_lock;

    // Guards __become_clean's check + CAS + superblock writes against all three cold failure-path
    // dirty_region() + __become_degraded() calls: active-fail (Site 1), backup-unavail (Site 2),
    // backup-fail (Site 3). dirty_region() is called inside the mutex at all failure sites so
    // dirty_pages() is a hard gate — no in-flight region can slip past the check.
    // Holding the lock across both the check+CAS and the SB writes ensures:
    //   (a) dirty_pages() sees all in-flight regions — prevents premature clean transition.
    //   (b) Failure-path DEVA SB writes always serialize after EITHER SB writes, so the
    //       on-disk SBs cannot show EITHER+dirty on crash (crash-recovery P0).
    // The success path (both legs succeed) does not hold this lock.
    std::mutex _clean_transition_mutex;

    // Counts prepare() calls; used to enable resync on the first queue init.
    std::atomic_uint16_t _nr_hw_queues{0};

    // Pessimistically set to true under _ctrl_lock before __become_degraded's write_superblock
    // call; cleared under the same lock on success. Stays true on failure so subsequent I/Os
    // retry via __try_persist_degraded_sb, which also checks under _ctrl_lock — no concurrent
    // queue thread can read false while a write is in-flight and prematurely ack.
    // The age increment is NOT reverted on failure so any retry carries a higher age than the
    // stale on-disk SB, ensuring pick_superblock selects the correct device on restart.
    // If set at shutdown, the destructor's SB write naturally persists the correct route.
    // Guarded by _ctrl_lock — all accesses are inside lock_guard scopes.
    bool _degraded_sb_pending{false};

    // Shared read/write routing helpers used by both async_iov and sync_iov.
    // Returns {primary_dev, failover_dev}. failover_dev is nullopt when the backup holds stale
    // data for this region (degraded array + dirty bitmap) -- callers must not read from it.
    std::pair< std::shared_ptr< MirrorDevice >, std::optional< std::shared_ptr< MirrorDevice > > >
    __select_read_devices(RouteState const& state, uint64_t addr, uint32_t len) const noexcept;

    // True when a write should be replicated to the backup leg. A dirty region in a degraded
    // array means the backup is owned exclusively by the resync task, so the I/O path must not
    // write it (this is what closes the resync stale-read race); an unavailable backup is skipped
    // likewise. Used identically by both async_iov and sync_iov.
    bool __backup_writable(RouteState const& state, uint64_t addr, uint32_t len) const noexcept;

    // Re-dirty a region during live I/O and, when a ZERO_TEST rebuild is in progress, taint the copy
    // mode to CHECK first: the target leg may now hold written (non-zero) data here, so its
    // read-zero-where-unallocated assumption no longer holds. Monotonic CAS (no-op unless ZERO_TEST;
    // BLIND/CHECK are already safe on any destination). The mode store is sequenced before the bitmap
    // set so a concurrent resync observes CHECK when it re-processes the region. Callers already hold
    // _clean_transition_mutex around the dirty_region; the CAS is a standalone atomic op.
    void __dirty_region_untaint(uint64_t addr, uint32_t len) noexcept;

    // Internal routines
    bool __become_clean();
    // Transitions in-memory route from EITHER→DEVA/DEVB and persists the superblock. Returns true
    // if the array is durably degraded (ack is safe); false if the SB write failed (caller must
    // return -EAGAIN — a crash before the SB is written would corrupt self-heal direction).
    // Idempotent when already degraded: delegates to __try_persist_degraded_sb.
    bool __become_degraded(bool failed_is_active, RouteState const* state, bool spawn_resync = true);
    // Called from __become_degraded when the array is already degraded. Retries any pending SB
    // write (_degraded_sb_pending) and, on success, optionally spawns resync. Returns true if the
    // SB is now durable (no write was pending, or the retry succeeded); false if the retry failed.
    bool __try_persist_degraded_sb(bool spawn_resync);
    disk_task< int > __failover_read_async(ublksrv_queue const* q, ublk_io_data const* data, iovec* iovecs,
                                           uint32_t nr_vecs, uint64_t addr, uint32_t len);
    bool __swap_device(std::string const& outgoing_device_id, std::shared_ptr< MirrorDevice >& incoming_mirror,
                       raid1::read_route const& cur_route);

    // Constructor helpers. Order matters: __load_and_select_superblock must run first to
    // populate _device_a/_device_b/_sb; __init_params then reads _sb->header.version to
    // decide the user-data alignment policy.
    void __load_and_select_superblock(boost::uuids::uuid const& uuid, std::shared_ptr< ublk_disk > dev_a,
                                      std::shared_ptr< ublk_disk > dev_b, std::string const& parent_id);
    void __init_params();
    // assume_clean: caller asserts a fresh leg reads zero where unallocated (enables ZERO_TEST).
    void __init_bitmap_and_degraded_route(bool assume_clean);
    void __become_active();
    // Persist _sb (stamped with the live _resync_mode) to dev; every Raid1 SB write flows through here.
    io_result __write_sb(ublk_disk& dev, bool device_b, read_route route, bool include_superbitmap = false) {
        return write_superblock(dev, _sb.get(), device_b, route, include_superbitmap,
                                _resync_mode.load(std::memory_order_acquire));
    }

    // ☠️ ☠️ ☠️  DANGER: LOCK-FREE SYNCHRONIZATION - DO NOT MODIFY  ☠️ ☠️ ☠️
    //
    // This function uses a CAREFULLY DESIGNED lock-free read-retry pattern with
    // application-level validation. Modifications can introduce:
    // - Use-after-free bugs (torn shared_ptr reads)
    // - ABA problems (if validation is weakened)
    // - Memory corruption (if retry logic is broken)
    //
    // The code is INTENTIONALLY UNSAFE by C++ standard (data race on shared_ptr)
    // but SAFE in practice (on x86-64) due to:
    // 1. Read-validate-retry loop catches torn reads
    // 2. Pointer-sized reads are atomic on x86-64
    // 3. We never use inconsistent data (validation ensures this)
    //
    // TSAN correctly flags this as a data race - suppression file required.
    // DO NOT TOUCH unless you fully understand lock-free memory models.
    //
    // ☠️ ☠️ ☠️  YOU HAVE BEEN WARNED  ☠️ ☠️ ☠️
    // clang-format off
    // noinline: required in all builds so the compiler cannot cache _device_a/_device_b across
    // the retry loop's consistency check (plain shared_ptrs, not atomic).
#ifndef NDEBUG
    // no_sanitize_thread: intentional lock-free race, validated by retry loop (see tsan.supp).
    // no_sanitize("address"): shared_ptr copy has a sub-nanosecond UAF window during swap_device.
    // The race cannot corrupt on-disk state (MirrorDevice dtor writes nothing to disk); worst case
    // is a process crash, which pod restart / UBLK_F_USER_RECOVERY handles. Locking this hot path
    // is not worth the cost for a swap that happens at most once per device lifetime.
    // NOTE: attribute must appear on both declaration and definition for GCC to suppress
    // instrumentation of the function body.
    __attribute__((noinline, no_sanitize_thread, no_sanitize("address")))
#else
    __attribute__((noinline))
#endif
    RouteState __capture_route_state() const;
    // clang-format on

public:
    Raid1Disk(boost::uuids::uuid const& uuid, std::shared_ptr< ublk_disk > dev_a, std::shared_ptr< ublk_disk > dev_b,
              std::string const& parent_id = "", bool assume_clean = false);
    ~Raid1Disk() override;

    /// Raid1Disk API
    /// =============
    // assume_clean: caller asserts the incoming leg reads zero where unallocated (enables ZERO_TEST).
    std::shared_ptr< ublk_disk > swap_device(std::string const& old_device_id, std::shared_ptr< ublk_disk > new_device,
                                             bool assume_clean = false);
    raid1::array_state replica_states() const noexcept;
    uint64_t reserved_size() const noexcept { return _reserved_size; }
    // Mode the next/current resync runs in; exposed for observability and mode-selection tests.
    resync_copy_mode current_resync_mode() const noexcept { return _resync_mode.load(std::memory_order_acquire); }
    // Launches (t) or stops the resync; launch reads _resync_mode, so store any new mode first.
    void toggle_resync(bool t);
    std::pair< std::shared_ptr< ublk_disk >, std::shared_ptr< ublk_disk > > replicas() const noexcept;
    /// =============

    /// UBlkDisk Interface Overrides
    /// ============================
    std::string id() const noexcept override { return "RAID1"; }
    prepare_result prepare(ublksrv_queue const* q, int const iouring_device) override;
    void probe_tick(ublksrv_queue const* q) noexcept override;

    disk_task< int > async_iov(ublksrv_queue const* q, ublk_io_data const* data, iovec* iovecs, uint32_t nr_vecs,
                               uint64_t addr) override;

    io_result sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept override;
    /// ============================
};

} // namespace raid1

} // namespace ublkpp
