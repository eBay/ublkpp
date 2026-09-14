# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.37.0] - 2026-09-10

### Added

- **RAID1 dynamic write backpressure during resync**: `Raid1Disk::async_iov` now yields via a NOP
  io_uring SQE when resync is actively copying and concurrent user writes exceed a configurable cap
  (default 16). Resync uses a blocking `sync_iov` that competes at the device queue level with user
  I/Os; capping in-flight writes reduces the effective queue wait per resync copy from ~38ms (under
  fio iodepth=16) to near-idle levels, significantly accelerating resync throughput while fio is
  running. The cap is disabled automatically when resync is not copying (IDLE/STOPPING state), so
  there is no overhead on healthy arrays. Configurable via `--resync_write_cap <N>` (0 = disabled).

## [0.36.0] - 2026-07-21

### Added

- **Memory estimation API**: `ublkpp::raid1::estimate_device_overhead(uint64_t volume_size)` returns
  worst-case RAID1 memory (one superblock + all bitmap pages dirty + per-PageData overhead); and
  `ublkpp_tgt::estimate_queue_memory()` returns target-level overhead (ublksrv I/O buffers + queue
  thread stacks). Both use current SISL runtime options. RAID0 has no device-specific overhead.

## [0.35.0] - 2026-07-02

### Added

- **RAID1 resync copy modes (thin-aware self-heal and rebuild)**: the resync now selects one of three per-region copy strategies instead of always blind-copying, deciding at 4 KiB (`k_page_size`) granularity while reading the source at `max_io` and coalescing contiguous writes. The compare-vs-blind choice keys only on whether the destination leg still holds data (a superblock present); `assume_clean` is orthogonal and only affects a genuinely-fresh leg:
  - **BLIND**: write every dirty region to the destination. Used for a genuinely-fresh leg without `assume_clean`, and for targeted/incremental degraded-return dirty sets, where the destination has no data worth comparing.
  - **CHECK**: read the destination and `memcmp` each page, writing only divergent pages. Selected automatically whenever the destination still holds data -- the both-present-unclean (power-loss) self-heal, and **any replacement/re-add of a leg that still has a superblock** (age-promoted, hot re-add, or an interrupted CHECK self-heal reassembling through the replacement branch via its >1 age gap). It turns a full-array rewrite into a compare scan and skips rewriting matching data (correct for any backend; a read-zeroing thin device also stays thin), and makes an interrupted self-heal resume as CHECK instead of degrading to a blind rewrite.
  - **ZERO_TEST**: `isal_zero_detect` each source page and write only non-zero pages, never reading the destination. Preserves thin provisioning when rebuilding onto a freshly-provisioned leg that reads zero where unallocated. Opt-in per device via a new `assume_clean` parameter on `make_raid1_disk()` and `swap_device()` (default `false`); `assume_clean` asserts *only* that a genuinely-fresh leg (no on-disk superblock) reads zero, upgrading its rebuild from BLIND to ZERO_TEST -- it does not influence the CHECK decision, so a data-bearing leg is never zero-tested and never left divergent. If a write races a ZERO_TEST copy (a region conflict is deferred), the run downgrades to CHECK for the remainder so a subsequent in-place source-zeroing cannot leave a written destination page stale. Making the same page-granular allocate decision at the same offsets on both legs keeps their backend allocation maps aligned.
- **RAID1 resync mode is persisted and resumed across restarts**: the per-region mode is stored in the superblock (`bitmap.resync_mode`, stamped by `write_superblock` alongside `read_route`) and re-read on reassembly, so a degraded resync keeps its mode across a clean restart instead of re-deriving it from a transient age gap. A **clean** degraded stop resumes the persisted mode as-is -- a known-divergent degradation stays BLIND (cheaper write-only copy), a power-loss self-heal stays CHECK, and an interrupted fresh-leg ZERO_TEST thin rebuild continues (its un-synced destination still reads zero). An **unclean** crash dirty-alls the mostly-matching whole array in CHECK regardless (the superbitmap is untrusted, and ZERO_TEST cannot safely cross a crash). Legacy superblocks read the zeroed byte as BLIND. `format_as` reports the mode (`resync:<mode>`). Power-loss resume of the un-synced remainder still requires the write-intent superbitmap and is out of scope; unclean shutdowns continue to dirty-all.


### Changed

- **RAID1 new-array initial sync**: assembling a brand-new pair (neither leg has a superblock) without `assume_clean` now pins `device_a` and runs an md-style initial BLIND sync so both legs read identically (recycled disks hold differing garbage, which made EITHER-routed reads non-deterministic); the array reports degraded/SYNCING until it completes. With `assume_clean` both legs are asserted to read zero -- already identical -- and the initial sync is skipped (the previous behavior, thin-preserving).
- **RAID1 startup self-heal no longer freezes the backup superblock**: `__become_active` writes both legs' superblocks (ages equalize) instead of skipping the stale leg to hold a `>1` age gap. Recovery of a degraded array keys on `read_route` (the canonical leg) plus the persisted resync mode, not on an age divergence, so a reassembled degraded array flows through the degraded-return branch (resume on clean, dirty-all on unclean) and an interrupted self-heal resumes as CHECK there rather than through the replacement branch. The "equal ages imply bit-for-bit identical" guarantee is now scoped to a healthy (`route==EITHER`) array; a degraded array carries its divergence in the route + superbitmap.

## [0.34.2] - 2026-07-24

### Fixed

- **Vendored ublksrv no longer discards I/O buffers that have I/O in flight**: `ublksrv` reclaims
  memory after 20 seconds of CQE silence by calling `madvise(MADV_DONTNEED)` on every per-tag I/O
  buffer, without checking whether any command is outstanding. A backing device that stalls (an
  iSCSI transport losing its session, for example) goes CQE-silent with its entire queue in
  flight, which is indistinguishable from an idle queue, so the discard zaps the mappings of
  buffers that pinned, in-flight reads are still writing into. The DMA lands in the orphaned
  physical pages while the next touch of the buffer faults in a fresh zero page, and the read
  completes full-length and successful carrying all zeros -- undetectable by any length or error
  check, and silently accepted by applications that treat a zeroed block as empty. The vendored
  recipe now skips the discard unless every tag's command is parked in the ring
  (`patches/idle_discard_inflight_1_5_0.patch`). Still present upstream as of ublksrv v1.7.

- **Short (partial-length) I/O completions are no longer silently propagated**: no layer of the
  async read/write path validated that a positive completion carried the full requested byte
  count. A short completion summed through RAID0 loses its position, and `ublk_drv` treats a
  positive short READ result as a *front-aligned partial completion* (first `res` bytes done,
  tail requeued) -- so a short/zero non-final sub-read with full later sub-reads would mark
  never-filled buffer ranges as done, surfacing stale per-tag buffer content (data from a
  different LBA) to the filesystem with no error or log anywhere. Length is now enforced at
  every layer: `FSDisk` fails a short CQE with `-EIO` (logged with device/addr/lengths),
  `Raid1Disk` treats a short primary read like a leg failure (UNAVAIL + failover; `-EIO` if no
  failover leg -- never `-EAGAIN`, which would requeue against the same misbehaving leg),
  `Raid0Disk` rejects any READ/WRITE aggregate that is not exactly the requested length, and
  the target converts any residual short completion to `-EIO` as a last line of defense.
  New `ublk_read_shorts_total` / `ublk_write_shorts_total` counters record every conversion.
  `FLUSH`/`DISCARD`/`WRITE_ZEROES` (which legitimately complete with 0) are exempt.

## [0.34.1] - 2026-06-29

### Fixed

- **RAID1 ZERO_TEST downgrade is now durable and observed live**: the `ZERO_TEST -> CHECK` taint (a write may have left data on a region whose source later reads zero) now updates the authoritative resync mode instead of a run-local copy, so it is stamped into the superblock and resumed as CHECK across a stop/relaunch, and the running resync reads it fresh per copy and picks it up immediately. A live write that re-dirties an already-synced region (an active/backup fan-out failure -- Site 1/2/3) also taints `ZERO_TEST -> CHECK`, closing a window where a later zero-source re-copy could skip the region and leave the mirrors divergent (a stale read once the array returns to `EITHER`). The downgrade target is CHECK, which is still thin-preserving (it only writes divergent pages), so the taint never allocates the thin destination.
- **RAID1 CHECK resync tolerates a destination read error**: a CHECK compare-read that fails (a latent URE / media error on the destination leg) no longer marks the leg unavailable and loops on the same LBA (the probe reads the intact superblock, clears unavailable, and the resync retries the bad block forever). It now falls back to a BLIND write of the already-read source, which may relocate the failing block (a write-based remap) and heals the region. A write failure is still a real device failure and propagates as before. Comprehensive bad-block handling (skip-and-continue with a bad-block list) remains tracked in #326.
- **Shutdown drain no longer errors I/O back to the block layer**: after `begin_shutdown()`, gated I/O was completed with `-EAGAIN`, which the kernel maps to `BLK_STS_AGAIN` and logs as `nonblocking retry error, dev ublkbN ...` before failing the request (a normal, non-`REQ_NOWAIT` request is not retried on `AGAIN`). Gated ops (now including `FLUSH`) are instead **dropped** -- left uncompleted (`OWNED_BY_SRV`) so the kernel requeues/reissues them to the next daemon under `UBLK_F_USER_RECOVERY(_REISSUE)` when the process exits via the recovery path (drop the `unique_ptr`, not `remove()`). The drain accounting (`record_queue_depth_change` / `try_drain`) is unchanged, so `wait_for_drain()` and the `clean_unmount=1` flush still fire exactly once.

## [0.34.0] - 2026-06-17

### Added

- **`ublk_read_bytes_total` / `ublk_write_bytes_total` Prometheus counters**: `UblkIOMetrics` now accumulates bytes transferred on successful IO completion. Counters carry the `entity=<volume_uuid>` label and enable throughput queries via `rate(ublk_read_bytes_total[5m])` / `rate(ublk_write_bytes_total[5m])`.
- **`ublk_resync_remaining_kib` / `ublk_resync_initial_kib` Prometheus gauges**: expose `dirty_data_est()` at resync start and after each sweep, enabling ETA (`remaining / rate(progress_sum)`) and progress-percentage queries in Grafana. Both gauges reset to 0 when resync completes.
- **`ublk_read_latency_us` / `ublk_write_latency_us` histograms**: per-IO latency measured from just before `async_iov` to just after, recorded for both successful and failed IOs.
- **`ublk_read_errors_total` / `ublk_write_errors_total` counters**: incremented whenever `async_iov` returns a negative result, enabling alerting on backing-device failures.
- **`ublk_raid_is_degraded` gauge**: set to 1 in `__become_degraded` and 0 in `__become_clean`, enabling point-in-time degraded-state queries and Grafana alerts.

## [0.33.2] - 2026-06-17
### De-prioritize Resync threads to SCHED_OTHER

- **RAID1**: Resync threads just do sync_io; so move them off the real-time scheduler.

## [0.33.1] - 2026-06-17

### Improved

- **Queue handler threads now run under `SCHED_FIFO` at max priority (99)**: each queue thread calls `pthread_setschedparam(SCHED_FIFO, 99)` on startup, placing it in the real-time scheduling class. This eliminates CFS jitter on the I/O hot path and ensures queue threads preempt all `SCHED_OTHER` work (including management and gRPC threads) whenever they are runnable.

## [0.33.0] - 2026-06-14

### Breaking

- **`ublkpp_tgt` destructor now aborts if `begin_shutdown()` was not called before drop**: previously, dropping an `ublkpp_tgt` without calling `begin_shutdown()` was silent, leaving queue threads running until process exit. The destructor now `RELEASE_ASSERT`s on any joinable queue thread, aborting the process with a diagnostic. **Migration**: always call `tgt->begin_shutdown(); tgt->wait_for_drain();` before dropping the `unique_ptr`. This ensures the RAID-1 bitmap is flushed and `clean_unmount=1` is written before exit.

### Added

- **`ublkpp_tgt::begin_shutdown()`**: signals the target to drain I/O before process exit. After this call, reads and writes are rejected with `EAGAIN` before they reach the backing device; `FLUSH` ops are allowed through (they complete instantly with `result=0` and do not dereference `device*`). When the last in-flight op completes and metrics counters reach zero, the backing device is destroyed exactly once (CAS-protected across queues) — flushing the RAID-1 dirty bitmap and writing `clean_unmount=1`. Idempotent; must be called from a thread context (the idle-drain path destroys the backing device synchronously).

### Fixed

- **`destroy()` missing `ctrl_dev = nullptr`**: after `ublksrv_ctrl_deinit`, `ctrl_dev` was not nulled, leaving a potential double-deinit on the freed pointer. Now nulled immediately after deinit.
- **Unconditional `join()` in `destroy()`**: queue threads were joined without a `joinable()` guard, which would throw `std::system_error` if a thread was never started (e.g., on a failed queue init). Added `if (q.joinable())` guard.

## [0.32.15] - 2026-06-12

### Improved

- (raid0): Only init parts of StripeAccumulator that i/o will actually touch.

## [0.32.14] - 2026-06-10

### Fixed

- **RAID1 (P0)**: Site 2 (`async_iov`/`sync_iov`, backup-unavail path) called `dirty_region()` and `__become_degraded()` without holding `_clean_transition_mutex`, unlike Sites 1 and 3.
- A concurrent `__become_clean` could write EITHER superblocks after Site 2's DEVA write, leaving both devices at EITHER+equal-age on crash; `pick_superblock` tie-broke to round-robin, serving stale data from B against an acknowledged write.
- Fixed by wrapping `dirty_region()` + `__become_degraded()` together inside `_clean_transition_mutex` at all six failure sites (async + sync, Sites 1–3), making `dirty_pages()` a hard gate against premature clean transitions.

## [0.32.13] - 2026-06-10

### Fixed

- **P1 (raid0): DISCARD/WRITE_ZEROES crashes file-backed arrays with more disks than the read/write stripe count**: `prepare()` sized the CQE pool using the read/write fan-out formula `stripes_for_io(max_tx, stripe_size, N)`, which caps at `k = min(ceil(max_tx/stripe_size)+1, N)`. The DISCARD path in `async_iov` always fans out to all N disks via `merged_subcmds`, consuming one pool slot per disk. For N > k (e.g., 10 disks, stripe_size=128 KiB, max_tx=512 KiB → k=5), `RELEASE_ASSERT_LT(_pool.size(), _pool.capacity())` fired on the (k+1)th disk's `next_state()` call — process crash. Fixed by removing the k-bound from `prepare()`: all N children contribute to `max_sqes_per_io`. Over-allocation by (N-k) slots in the read/write path is harmless.
- **Latent (raid1): superblock with version > current not rejected at open**: `load_superblock` had no upper-bound version check, silently accepting a disk formatted by a future binary. `__init_params`'s one-sided `if (sb_version < 2)` branch would compute `_reserved_size` using the v2 formula, placing all I/O at the wrong on-disk offset on a hypothetical v3 disk. Fixed by adding the M3 guard (mirrors RAID0's existing check): any superblock with `version > k_sb_version` is now rejected with `errc::not_supported`. Also exports `k_sb_version` from `raid1_superblock.hpp` so tests can reference it.

## [0.32.12] - 2026-06-06

### Fixed

- **P0 (raid1): data loss when active write fails, backup succeeds, and superblock write fails**: in `async_iov` Site 1, if the active device write failed and `__become_degraded` could not persist the degradation (superblock write failed), the code still returned SUCCESS to the client when the backup write succeeded. On a crash at that point, both on-disk superblocks still showed `route=EITHER` at the pre-degradation age; restart self-healed by resyncing device_a (which held the *old* data) over device_b (which held the *new* data), permanently destroying the client-acked write. Fixed by returning `-EAGAIN` to the client whenever `__become_degraded` fails — forcing a retry rather than acking an unprotected write. The incorrect claim in the `__become_degraded` comment ("dirty bitmap covers the affected region; resync at shutdown will reconcile") is also removed, since the in-memory dirty bitmap does not survive a pod kill. Addresses [issue #302](https://github.com/szmyd/ublkpp/issues/302).
- **RAID1 SB-EITHER crash window in degraded mode**: when `__become_degraded` updated the in-memory route to `DEVX` but its superblock write failed, both on-disk SBs remained at `EITHER`. A crash before a subsequent write reached the device left `pick_superblock` with no age gap to distinguish legs, potentially resyncing the stale leg over the surviving one — silent data loss. Fixed by: (1) retaining the age increment on SB write failure (so any eventual retry carries a higher age than the stale on-disk SB, letting `pick_superblock` select the correct leg); (2) adding `_degraded_sb_pending` flag set pessimistically under `_ctrl_lock` before the SB write; (3) retrying the SB write via `__try_persist_degraded_sb` before acking the next I/O that reaches the array in degraded state.
- **RAID1 `_degraded_sb_pending` premature-ack race with `nr_hw_queues > 1`**: the `_read_route_cache` CAS and `_degraded_sb_pending = true` were not atomic with respect to `_ctrl_lock`. A concurrent I/O thread (T2) that lost the CAS could enter `__try_persist_degraded_sb`, acquire `_ctrl_lock`, read `_degraded_sb_pending == false` (T1 had won the CAS but not yet set the flag), and prematurely ack its I/O while T1's SB write was still in-flight. Fixed by moving the CAS inside `_ctrl_lock` in `__become_degraded` so the CAS win and the `_degraded_sb_pending = true` store are a single atomic transaction under the lock. `__swap_device` already held `_ctrl_lock` for its CAS; this unifies both state-machine transitions under the same serialisation point.
- **RAID1 double `toggle_resync` with concurrent `nr_hw_queues > 1` SB retries**: two I/O coroutines could both pass the `_degraded_sb_pending` fast-path check in `__try_persist_degraded_sb`, both write the SB successfully, and both call `toggle_resync`. A second concurrent `launch()` on a resync task that had not yet transitioned from `IDLE` to `ACTIVE` would join the running thread, stalling an I/O-path coroutine for the full resync duration. Fixed by snapshotting `was_pending = _degraded_sb_pending; _degraded_sb_pending = false` atomically under `_ctrl_lock`; only the coroutine that observes `was_pending == true` calls `toggle_resync`.

## [0.32.11] - 2026-06-09

### Changed

- **RAID0 per-stripe iovec accumulators moved to coroutine frame**: `__distribute`'s
  `StripeAccum` buffer (previously a `thread_local` with `std::vector<iovec>` per slot) is
  now a caller-owned `std::array<StripeAccum, 64>` with a fixed `std::array<iovec, 16>` per
  slot (bound: max_io_size 1 MiB / min_stripe_size 64 KiB = 16). In `async_iov` the buffer
  lives in the coroutine frame (heap, stable for child lifetimes); in `sync_iov` it is a
  plain stack local. Eliminates the iovec lifetime hazard that required the eager
  `io_uring_submit` workaround in `async_iov` and the `iov_snap` copy in
  `Raid1Disk::__failover_read_async`. Removes `DirtyGuard`, `_max_iovecs_per_stripe`, and
  the `thread_local`; introduces `k_max_iovecs_per_stripe = 16` as a file-scope constant.

## [0.32.10] - 2026-06-05

### Fixed

- **RAID1 remount failure after resync-interrupted-by-stop race**: a race between the resync task and `stop()` in the destructor produced an on-disk state of `DEVB + clean_unmount=1 + empty superbitmap`. On second mount this hit an invariant check and threw `std::runtime_error`, making the volume unassemblable. Fixed with two cooperating changes: (1) `_start()` captures `dirty_pages()` before each `__run()` call; when STOPPING fires, if the count was non-zero and is now zero the resync cleared all chunks but was interrupted in `__yield()` — `complete()` is called immediately so the destructor sees `route=EITHER`; (2) the invariant check at mount time is replaced with a warn + `dirty_region(0, capacity())` fallback, providing defense-in-depth for any residual cases not prevented by (1). Addresses [issue #300](https://github.com/szmyd/ublkpp/issues/300).

## [0.32.9] - 2026-06-04

### Fixed

- **RAID1 read-determinism after unclean shutdown (both legs present)**: when both legs were present at startup with equal ages, `clean_unmount=0`, and `read_route=EITHER`, the code previously only logged a warning. Writes in-flight at crash time may have landed on one leg but not the other, so round-robin reads of the same LBA could return different bytes on alternating accesses — a violation of the block-device read-determinism contract that filesystems (XFS, etc.) depend on unconditionally. The fix detects this case and self-heals: manufactures a +16 age gap on the canonical leg (device_a), pins reads to it via `read_route::DEVA`, dirties the whole bitmap, and marks device_b stale (`unavail`) so writes skip it until `probe_mirror` confirms physical health. `__become_active` skips persisting device_b's superblock (new `unavail` guard), preserving the on-disk age gap so any crash mid-resync reassembles through the existing `new_device` path rather than back into this branch. Idempotent across repeated crashes; no new superblock field. Addresses [issue #290](https://github.com/szmyd/ublkpp/issues/290) Phase 1.

## [0.32.8] - 2026-06-01

### Fixed

- **P0 (raid1)**: `__become_clean()` could transition the route to `EITHER` while dirty bits remained in the bitmap, with no resync task to process them. A concurrent write whose backup leg fails calls `dirty_region()` after the resync task's `dirty_pages()==0` observation but before the CAS. Subsequent reads skipped the `is_dirty` guard (only active in degraded mode) and could be served from the stale backup — a write-acknowledgment boundary violation. Fixed by: (1) inverting the CAS/superblock write ordering; (2) re-checking `dirty_pages()` after the CAS — if dirty bits are found the transition is reversed and `__become_clean()` returns `false`; (3) `_start()` loops on the return value so the still-ACTIVE resync task re-runs `__run()` to pick up the newly-dirtied region, converging without requiring a re-launch.
- **SuperBitmap::set_bit()** now uses `memory_order_release` (was `relaxed`) to pair correctly with the `memory_order_acquire` loads in `next_set_bit()` and `dirty_pages()`, ensuring the dirty_pages() double-check in `__become_clean` sees concurrent `set_bit()` calls on all architectures.
- **bitmap.age revert** under SB write failure in `__become_degraded` was not guarded by `_ctrl_lock`, creating a data race with `__swap_device`'s concurrent `+16` age bump. The increment was already guarded; added the same guard to the revert.
- **`_clean_transition_mutex` in `async_iov` Site 1** was held across two `co_await *backup_task` suspensions. Narrowed the lock to cover only `dirty_region()` + `__become_degraded()`; backup drain `co_await` calls happen after lock release.
- **Liveness gap between `complete()==true` break and `ACTIVE→IDLE` CAS**: a write-fail during this window called `toggle_resync()` which `launch()`-EARLY_EXITs on `ACTIVE` state; dirty bits could be stranded with no resync running. Fixed by moving the `ACTIVE→IDLE` CAS inside the `_start()` while loop: after `complete()` confirms a clean bitmap the task CAS-es itself to IDLE, re-checks `dirty_pages()`, and if bits appeared in the gap attempts an `IDLE→ACTIVE` reclaim CAS to stay alive and drain them. If another `launch()` or `stop()` wins the IDLE slot, the new task handles the remaining bits.
- **H1 in `__become_clean()`** no longer attempts `write_superblock` on a missing-disk placeholder for the backup device.

## [0.32.7] - 2026-06-01

### Fixed

- **H3 (raid0)**: the array capacity was computed as `(min_leg_capacity - stripe_size) * leg_count`, which assumes every leg holds a whole number of stripes. When a leg's capacity is not a multiple of `stripe_size`, the partial trailing stripe (`leg_capacity % stripe_size`) was carried into the per-leg term and then multiplied by the leg count, over-reporting the device size by `(leg_capacity % stripe_size) * leg_count`. Reads into that phantom tail (e.g. backup-GPT / partition-table scans at the top of the device) mapped to a per-leg offset past the end of the backing device and returned EIO. The bug was latent because leg capacities happened to be stripe-aligned; raid1 SuperBlock v2 dropped the 512 KiB user-data alignment that had masked it, surfacing top-of-device read failures on RAID10 arrays (observed on a 50-leg array of 3 TiB legs over-reporting by ~3 MiB). Fixed by flooring the smallest leg to a whole number of stripes before reserving the head stripe and striping.

## [0.32.6] - 2026-05-26

### Fixed

- **H1 (raid0)**: `io_uring_submit` failure in `async_iov` left child coroutines waiting for CQEs that would never arrive, deadlocking the drain loop. Staged SQEs remaining in the ring would be submitted later with recycled `cqe_state` pointers, causing UAF. Fixed by retrying submit once; if still failing, synthetically completing pending `cqe_state` entries before draining.
- **M3 (raid0)**: `load_superblock` silently accepted on-disk superblock versions newer than `SB_VERSION`. A future format change adding fields before existing ones would corrupt data. Now throws `std::runtime_error` (returns `std::errc::not_supported`) for `sb_ver > SB_VERSION`.
- **L1 (raid0)**: `_stride_width` was `uint32_t`; for large configs (e.g. 128 MiB stripe × 64 disks = 8 GiB) the multiplication overflowed silently. Widened to `uint64_t` throughout, including `next_subcmd`/`merged_subcmds` signatures in `raid0_impl.hpp`.
- **L2 (raid0)**: Non-power-of-2 `stripe_size_bytes` was silently rounded down by `ilog2` (e.g. 6 KiB treated as 4 KiB), producing wrong geometry. Constructor now throws `std::invalid_argument` for non-power-of-2 stripe sizes.

## [0.32.5] - 2026-05-26

### Fixed

- **H5 (target)**: `ublksrv_ctrl_start_recovery` failure was logged but execution continued into subsequent recovery steps in an undefined state. Now returns `std::errc::operation_not_permitted` immediately on failure.
- **H6 (target)**: the io_uring completion handler only caught `std::exception`; any other thrown type would propagate through the `noexcept` coroutine boundary and terminate the process. Added a `catch (...)` fallback that completes the IO with `-EIO`.
- **M8 (target)**: `sem_destroy` was never called on the queue semaphore, leaking an OS resource on every device teardown.
- **M7 (driver)**: `FSDisk` could `throw` during `fstat`/`ioctl`/`fcntl` without closing `_fd`, leaking the file descriptor. Added an RAII `FdGuard` that closes `_fd` on any exception path.
- **L3 (driver)**: `ilog2(0)` is undefined behaviour (SIGFPE). Added explicit zero-checks for `lbs` and `pbs` before calling `ilog2` on both block-device and regular-file code paths.

## [0.32.4] - 2026-05-25

### Fixed
- raid1: Make stable copy of iovecs in __failover_read

## [0.32.3] - 2026-05-25
### Fixed
- raid1: Remove Optimistic write path which can now race with region tracked resync.

## [0.32.2] - 2026-05-24

### Changed
- **Ban `resync_level=0`**: construction now throws `std::runtime_error` if `resync_level` is set
  to 0. A zero level silently produced zero `copies_left`, causing the resync loop to stall with
  no forward progress. Valid range is 1–32.

## [0.32.1] - 2026-05-24
- Some fixes for gcc-16 compilation

## [0.32.0] - 2026-05-20

### Changed
- **Fixed on-disk reserved region**: `_reserved_size` is now always
  `sizeof(SuperBlock) + k_superbitmap_bits × k_page_size` (~125.6 MiB) regardless of capacity,
  leaving headroom for future volume resize without a format change. `init_to` still writes only
  `_num_pages` (capacity-derived) zero pages — the remainder of the reserved region is claimed by
  layout, not pre-written.
- **Tighter user-data alignment (v2)**: `_reserved_size` padding now aligns to `logical_bs` (~4 KiB)
  instead of `max_sectors_bytes` (~512 KiB), reclaiming up to ~511 KiB of wasted tail space per
  device. v1 arrays keep the old alignment exactly.
- **`SB_VERSION` bumped 1 → 2**: new arrays are stamped v2. Existing v1 arrays open as-is;
  `__init_params` branches on the version field to reconstruct the original capacity-proportional
  `_reserved_size`, preserving the exact on-disk layout.
- Constructor call order fixed: `__load_and_select_superblock` now runs before `__init_params`
  so the SB version is available when choosing the alignment policy.

## 0.31.0 raid1: replace global PAUSE with lock-free per-region write tracker
- Replace global `PAUSE` state with `RegionTracker`: a lock-free flat slot array that tracks
  `(lba, len)` of each in-flight write. Resync now yields only for chunks that actually conflict
  with an in-flight write; unrelated regions copy without any yield.
- Two-phase conflict check: Phase 1 (`overlaps()`) before copy, Phase 2 (`overlaps()` +
  `completed_since()`) after copy. Shadow completion ring closes the race where a write arrives
  and completes entirely during the READ window.
- `resync_skip_from` cursor + `next_dirty_after()`: prevents low-LBA dirty runs from starving
  higher-LBA runs under sustained write pressure.
- Remove `PAUSE` state, `__pause()`, and `sisl::atomic_status_counter`; replace with plain
  `std::atomic<resync_state>` (4 states: IDLE, ACTIVE, SLEEPING, STOPPING).
- `copies_left` budget consumed only by actual copy attempts; Phase 1 skips are free.

## 0.30.0 refactor: async coroutine I/O, public API 1.0.0 uplift, and RAID1 hardening
 - Async I/O (Phases 1-11):
   - cqe_state pool per queue; raw cqe_state* encoded directly in SQE user_data
   - disk_task<T>/hot_task<T> coroutines with zero-overhead symmetric transfer
   - exec::task<void> + stdexec async_scope replace co_io_job; scope.on_empty() drains in-flight work
   - All disks implement pure-virtual async_iov; handle_io_async removed from vtable
   - ublkpp_tgt owns the io_uring CQE loop (run_queue_loop); destroy() renamed to remove()
   - ResyncWriteGuard RAII ensures enqueue/dequeue_write is always balanced in RAID1
 - Public API (1.0.0 prep):
   - make_fs_disk/make_raid0_disk/make_raid1_disk/make_missing_disk factories replace direct construction
   - ublk_disk base + disk_handle ownership alias; geometry through explicit getters
   - Public headers consolidated: drivers.hpp, raid.hpp, target.hpp
 - **Breaking**: removed `handle_rw`, `queue_tgt_io`, `handle_discard`, `sub_cmd_t`, and the `open_for_uring`/`handle_io_async`/`handle_iov_async` virtual surface -- replaced by `prepare` and `async_iov`
 - **Breaking**: updaetd sisl to v14.x which drops Folly and iomgr (in testing) dependencies.

## 0.22.2
- raid1: Fix `stop()` IDLE→STOPPING race - when the resync thread finishes naturally and
  `stop()` is called before `join()`, the `IDLE+joinable` handler now returns `SUCCESS` instead
  of `RETRY_WITH_SLEEP`, preventing an accidental `CAS(IDLE→STOPPING)` that left no thread to
  clear the state; subsequent `launch()` call in `swap_device()` would spin forever.

## 0.22.1
- raid1: Fix dequeue/resume race - `_resync_state` and `_outstanding_writes` are now packed into a single `sisl::atomic_status_counter` so the counter decrement and PAUSE→ACTIVE transition are one indivisible CAS; `__resume()` is removed.
- raid1: Fix enqueue/pause race - `enqueue_write()` now always calls `__pause()` on every enqueue, not only the first; previously a concurrent second enqueuer could skip `__pause()` while the first was still establishing it, allowing resync to overwrite an in-flight write with stale data
- raid1: Replace GCC `__builtin_popcount`/`__builtin_clz`/`__builtin_ctz` with C++23 `std::popcount`/`std::countl_zero`/`std::countr_zero`
- build: `libatomic` is now declared as a Conan system lib on Linux - propagated automatically to consumers, no downstream changes required

## 0.22.0
- raid1: Fix multi-queue idle probe race conditions - probes now start only when all queues are idle, mutex serializes concurrent launch/stop calls, `open_for_uring` counts queue threads for accurate `nr_hw_queues`
- **Breaking**: `UblkDisk::open_for_uring` signature changed from `(int)` to `(ublksrv_queue const*, int)` - out-of-tree subclasses must update their override

## 0.21.6
- raid0: Fix stale alive_cmds in __distribute() corrupting the next I/O on the same thread after a failed multi-stride operation

## 0.21.5
- raid1: Only log in reference to Resync if it was actually running.

## 0.21.x
- raid0: Fix stale alive_cmds in __distribute() corrupting the next I/O on the same thread after a failed multi-stride operation
- raid1: Only log in reference to Resync if it was actually running.
- raid1: Fix resync task hang when stop() is called during unavail wait loop
- raid1: Fix concurrent launch()/stop() race on resync task thread assignment
- raid1: Fix incorrect device logged as degraded in __become_degraded (DEVB route)
- raid1: Fix Raid1ResyncTask::launch() crash on joinable thread reassignment
- raid1: Fix remount failure when bringing up a degraded array with a defunct device
- fio_engine: Pre-populate backing files with zeros after allocation to convert unwritten extents to written state, eliminating first-write journal cost and page fault overhead that inflated write latency on fresh files. Raw block devices are detected and skipped.
- New functional testing framework. See `docs/functional_testing.md` for details.
- raid1: Introduce `UNAVAIL` replica state for transient read failures, routing I/O away from replicas that fail reads without marking them fully offline. A new periodic health monitor (`Raid1AvailProbeTask`) probes unavailable replicas during idle periods and restores them to active routing once they recover. Resync logic is updated to skip copies to unavailable mirrors and to wait for a device to become available before proceeding.

## 0.20.x
- raid1: Fix on_io_complete metrics mapping after device swap - I/O metrics were incorrectly attributed to the wrong device when completions occurred after swap_device() changed the route.
- Fix some more cases where __capture_route_state was being misused.
- raid1: Make Bitmap lock-free by using pre-allocated vector.
- RouteState atomicity: Introduced RouteState struct and __capture_route_state() to atomically capture all 
  route-derived values (active/backup devices, subcmds, degraded flag) at function entry
- Eliminated unsafe macros: Removed CLEAN_DEVICE, DIRTY_DEVICE, IS_DEGRADED, CLEAN_SUBCMD, DIRTY_SUBCMD macros that
  could evaluate differently mid-operation when swap_device changes the route
- raid1: Fix some device swap races by consolidating atomic loads
- raid1: Refactor the Raid1ResyncTask to its own component
- raid1: Consolidate state machine logic.
- raid1: Add outstanding write counter, trigger resync on this.
- **CRITICAL FIX**: raid1: Add thread-safe synchronization to bitmap page map
  - Protects bitmap `_page_map` structure from race conditions between resync thread and async I/O
  - Shared locks for readers (`is_dirty`, `next_dirty`, `clean_region`, `sync_to`) allow concurrent reads
  - Exclusive locks for writers (`dirty_region`, `dirty_pages`, `load_from`) prevent structural corruption
  - Fixes potential crashes and data corruption from concurrent map modification during resync
- raid1: Stop resync before touching bitmap during swap.
- raid1: Ability to bring RAID1 online with missing device.

## 0.19.x
- ublkpp_tgt: Remove DEFER_TASK from flags passed to ublksrv_queue_init
- ublkpp_tgt: Allow oom killing of process.

## 0.18.x
- ublkpp_tgt: Shutdown control device asynchronously.
- ublkpp_tgt: add get device destroy api
- ublkpp_tgt: add get device id api
- raid1: Fix data races accessing superblock read_route field 
- raid0: Increase route from 4 to 6 bits.
- bitmap: SuperBitMap introduction. Improve the bitmap load_from speed by reading superbitmap first.
- Feature: Device Recoverability

## 0.17.x
- bitmap: Increased shutdown speed by batching sync_to requests and tracking the unchanged pages
- iomgr: Shutdown during process termination.
- Introduce Metrics gathering

## 0.16.x
- fs_disk: Fix probing devices w/o partition.
- raid1: Swap with no changes should have zero impact.
- raid1: Reduce instance where a full copy is performed.
- raid1: Fix disk position identification.
- raid1: Improve disk recovery (non-new replace)
- fs_disk: Support probing of partitions (e.g. /dev/sda1)
- logging improvements
- raid1: Improve disk replacement logic (swap_device)
- raid1: Speed up Bitmap initialization
- raid0: Optimize OPT_IO and PHY_IO to match array layout.
- raid1: Fix reservation calculation to ensure user region is aligned to max i/o
- raid1: Reservation size is now dynamically calculated during init, prevents resize

## 0.15.x
- Fix homeblock_disk linkage
- Enable C++23 extensions
- Replace usage of folly::Expected with std::expected

## 0.14.x
- Update 3rd party libs (sisl, boost, folly)
- CI to build with GCC-13
- raid1: Remove UUID from Bitmap (found redundant)
- raid: Cleanup logging
- raid1: Track the amount of dirty data left in the bitmap
- raid1: I/O sharing between re-sync and user path

## 0.13.x
- raid1: Bitmap optimizations.
- raid1: Buffer cleaned BITMAP regions
- raid0: UINT32 truncation bug
- ublk_disk: Align device size to max_sector
- raid1: Buffered BITMAP

## 0.12.x
- raid1: Fix bug in Bitmap::load_pages where we used `&` instead of `*` for arithmetic
- raid1: Improve reading from degraded disk on startup
- raid1: Bitmap fix due to superblock mis-alignment
- fs_disk: Fix conditional on close
- raid1: Resync/Swap interaction fixes
- raid1: Retrieve underlying devices with `::replicas()`
- raid1: Fixed race with `::swap_device` and `__resync_task`

## 0.11.x
- raid1: Fix Bitmap bugs when representing > 4Gi
- ublkpp_disk: Support for HomeBlkDisk type
- raid1: Another resync_task termination fix
- raid1: Fix resync_task termination
- raid1: Resync task handles no-dirty pages
- raid1: Provide ability to gather replica states

## 0.10.x
- Cleanup Public API headers
- iscsi: Name thread
- libiscsi: Update to 1.20.3
- iscsi_disk : Fix iscsi session teardown
- iscsi_disk : Fix memleak in sync_iov
- ublkpp_tgt: Fix multi-device attach
- Fix shutdown
- Start device mgmt logic

## 0.9.x
- raid0: Fix leak on invalid superblock
- raid1: Avoid I/O attempts on known degraded devices

## 0.8.x
- raid1-bitmap: Fix word wrapping on dirty words
- raid1: Do not attempt discard on degraded devices
- raid1: Bugs discovered during testing
- raid1: Initiate resync task
- raid1: Improve latency on resync thread
- raid1: Active resync thread

## 0.7.x
- raid1: Give the BITMAP its own uuid
- raid1: Fix endian encoding of SuperBlock integers
- Fix re-loading dirty device after shutdown
- raid1: Make Bitmap atomic
- raid1: Initialize bitmaps on new array devices
- raid1: Fix degraded discard
- ublkpp_disk: Added "loop" mode
- raid1: Bitmap Load/Init
- General fixes
- raid1: Writes in degraded mode
- raid1: Extract BITMAP into its own class
- raid1: Reads in degraded mode

## 0.6.x
- raid1: Do not re-write unchanged pages
- raid1: Round-Robin reading
- homeblk_disk: Disable by default
- raid1: Calculate reserved area based on limits
- ublkpp_tgt: Clear async_event before calling process_result
- raid1: Bitmap words should be encoded as NETWORK byte order
- raid1: Records dirty chunks to the BITMAP pages

## 0.5.x
- homeblk_disk : introduced
- raid1: more intelligent retry handling
- ublkpp_tgt : fix narrowing conversion

## 0.4.x
- iscsi_disk : introduced

## 0.3.x
- ublkpp_tgt : improvments

## 0.2.x
- ublkpp_tgt : API changes

## 0.1.x
- fs_disk : introduced
- raid0/1 : introduced
