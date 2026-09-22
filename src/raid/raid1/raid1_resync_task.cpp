#include "raid1_resync_task.hpp"

#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <isa-l/mem_routines.h>
#include <ublksrv.h>
#include <sisl/utility/thread_factory.hpp>

#include "lib/logging.hpp"
#include "bitmap.hpp"
#include "raid1_impl.hpp"

namespace ublkpp::raid1 {

Raid1ResyncTask::Raid1ResyncTask(std::shared_ptr< raid1::Bitmap >& bitmap, uint64_t offset, uint32_t io_size,
                                 uint32_t max_io, std::atomic< resync_copy_mode >* live_mode, uint32_t slot_count,
                                 uint32_t chunk_size, std::shared_ptr< ublkpp::UblkRaidMetrics > metrics) :
        _dirty_bitmap(bitmap),
        _metrics(metrics),
        _io_size(io_size),
        _max_size(max_io),
        _offset(offset),
        _live_mode(live_mode ? live_mode : &_owned_mode),
        _region_tracker(slot_count, chunk_size),
        _resync_task() {
    if (!_dirty_bitmap) throw std::runtime_error("No Bitmap");
}

Raid1ResyncTask::~Raid1ResyncTask() noexcept {
    if (_resync_task.joinable()) _resync_task.join();
}

template < typename StateHandler >
[[gnu::noinline]] bool Raid1ResyncTask::__transition_to(resync_state initial, resync_state target,
                                                        StateHandler&& handler) noexcept {
    auto cur_state = initial;
    while (!__cas_state(cur_state, target)) {
        auto [next_state, action] = handler(cur_state);

        switch (action) {
        case transition_action::RETRY: // LCOV_EXCL_LINE
            cur_state = next_state;    // LCOV_EXCL_LINE
            break;                     // LCOV_EXCL_LINE
        case transition_action::RETRY_WITH_SLEEP:
            cur_state = next_state;
            std::this_thread::sleep_for(k_state_spin_time);
            break;
        case transition_action::SUCCESS:
            return true;
        case transition_action::EARLY_EXIT:
            return false;
        }
    }
    return true; // CAS succeeded
}

void Raid1ResyncTask::_start(std::string str_uuid, std::shared_ptr< MirrorDevice >& clean_mirror,
                             std::shared_ptr< MirrorDevice >& dirty_mirror, std::function< bool() >&& complete) {
    RLOGD("Resync Task created for [uuid:{}]", str_uuid)
    // Wait to become Available & IDLE
    auto cur_state = resync_state::IDLE;
    while (dirty_mirror->unavail.test(std::memory_order_acquire) || !__cas_state(cur_state, resync_state::ACTIVE)) {
        // If we're stopped or another we should exit
        if (resync_state::STOPPING == cur_state) break;

        cur_state = resync_state::IDLE;
        if (dirty_mirror->unavail.test(std::memory_order_acquire)) {
            cur_state = __load_state();
            if (resync_state::STOPPING == cur_state) break;
            std::this_thread::sleep_for(std::chrono::seconds(SISL_OPTIONS["avail_delay"].as< uint32_t >()));
            probe_mirror(*dirty_mirror, _offset);
        } else // LCOV_EXCL_START -- CAS IDLE→ACTIVE race inside _start(); not deterministically triggerable
            std::this_thread::sleep_for(std::chrono::microseconds(SISL_OPTIONS["resync_delay"].as< uint32_t >()));
        // LCOV_EXCL_STOP
    }
    cur_state = __load_state();

    // We are now guaranteed to be the only active thread performing I/O on the device
    if (resync_state::STOPPING != cur_state) {
        // Return the resync FSM to IDLE before this thread can exit: only this thread ever leaves
        // ACTIVE, so a lingering ACTIVE would make stop() spin forever. Today the state is always
        // ACTIVE here (stop() only CAS-es SLEEPING/IDLE->STOPPING, never ACTIVE->STOPPING), but
        // letting the CAS failure update `s` lets the loop converge from any state -- so it can never
        // spin on a stale expected value, even if that invariant changes. Also handles spurious
        // compare_exchange_weak failures (ARM/POWER LL/SC).
        auto to_idle = [this] {
            resync_state s = resync_state::ACTIVE;
            while (!__cas_state(s, resync_state::IDLE)) {}
        };

        // Snapshot the live mode for the allocation decision and the log; __run re-reads it per copy.
        auto const initial = _live_mode->load(std::memory_order_acquire);
        RLOGI("Resync starting [uuid:{}] [mode:{}] to: {}", str_uuid, initial, *dirty_mirror->disk)
        auto const initial_resync_size = _dirty_bitmap->dirty_data_est();
        // Set ourselves up with a buffer to do all the read/write operations from
        auto iov = iovec{.iov_base = nullptr, .iov_len = 0};
        if (auto err = ::posix_memalign(&iov.iov_base, _io_size, _max_size); 0 != err || nullptr == iov.iov_base)
            [[unlikely]] { // LCOV_EXCL_START
            RLOGE("Could not allocate memory for I/O: {}", strerror(err))
            if (iov.iov_base) free(iov.iov_base);
            to_idle();
            return;
        } // LCOV_EXCL_STOP

        // Destination-compare buffer: CHECK reads the destination, and ZERO_TEST needs it ready for
        // a mid-run downgrade to CHECK (see __run). On allocation failure fall back to BLIND rather
        // than abort -- a blind copy still converges; aborting would leave the array dirty.
        // blind_fallback pins this run to BLIND when no compare buffer is available (BLIND mode, or
        // an allocation failure). It is run-local and is never published to _live_mode: a relaunch
        // may allocate successfully and resume the real CHECK/ZERO_TEST.
        auto cmp_iov = iovec{.iov_base = nullptr, .iov_len = 0};
        bool blind_fallback = true;
        if (resync_copy_mode::BLIND != initial) {
            if (auto err = ::posix_memalign(&cmp_iov.iov_base, _io_size, _max_size);
                0 != err || nullptr == cmp_iov.iov_base) [[unlikely]] { // LCOV_EXCL_START
                RLOGW("Could not allocate compare memory ({}); resync falling back to BLIND copy", strerror(err))
                cmp_iov.iov_base = nullptr; // posix_memalign leaves this null on failure; keep it so
                // blind_fallback stays true
            } // LCOV_EXCL_STOP
            else
                blind_fallback = false; // compare buffer ready -> honor the live CHECK/ZERO_TEST mode
        }

        auto const resync_start = std::chrono::steady_clock::now();
        // Record resync start - increment global and per-device counters
        // Capture the initial size of data to resync
        if (_metrics) { // GCOVR_EXCL_BR_LINE -- UblkRaidMetrics requires prometheus registry; not constructible in unit
                        // tests
            // LCOV_EXCL_START
            auto const active_count = s_active_resyncs.fetch_add(1, std::memory_order_relaxed) + 1;
            _metrics->record_resync_start();
            _metrics->record_active_resyncs(active_count);
            _metrics->record_resync_initial_size(initial_resync_size);
        } // LCOV_EXCL_STOP

        // Loop until the array is confirmed clean or we are stopped.
        //
        // complete() returns false if dirty_region() fired in the __become_clean transition
        // window (either under the mutex or in the H1 gap); __run() re-enters in degraded mode.
        //
        // The ACTIVE→IDLE CAS is done inside the loop so dirty bits that land in the gap
        // between the last dirty_pages() check and the CAS are caught immediately. If another
        // launch() wins the IDLE slot, that new task handles the remaining bits.
        while (true) {
            auto const pages_before = _dirty_bitmap->dirty_pages();
            cur_state = __run(clean_mirror, dirty_mirror, &iov, cmp_iov.iov_base ? &cmp_iov : nullptr, blind_fallback);
            if (resync_state::STOPPING == cur_state) {
                // All chunks cleared but stopped in __yield(): commit so destructor sees route=EITHER,
                // not DEVA/DEVB + empty-superbitmap. Guard: pages_before>0 skips a zero bitmap at launch.
                if (pages_before > 0 && 0 == _dirty_bitmap->dirty_pages()) {
                    if (!complete()) { RLOGW("complete() returned false on STOPPING — unexpected [uuid:{}]", str_uuid) }
                }
                break;
            }
            DEBUG_ASSERT_EQ(resync_state::ACTIVE, cur_state, "Resync stopped in unexpected state")
            if (0 != _dirty_bitmap->dirty_pages()) continue;
            if (!complete()) continue;
            if (0 != _dirty_bitmap->dirty_pages()) continue;

            // Attempt ACTIVE->IDLE via to_idle (single implementation; handles spurious CAS
            // failures), then re-check for bits that landed in the transition gap.
            to_idle();
            if (0 == _dirty_bitmap->dirty_pages()) break; // clean exit

            // Bits appeared between the dirty_pages() check and the CAS. Try to reclaim
            // ACTIVE and drain them; if another launch() already won the IDLE slot, break —
            // that new task owns the dirty bits.
            auto idle = resync_state::IDLE;
            if (!__cas_state(idle, resync_state::ACTIVE)) break;
            RLOGD("Resync re-entering after concurrent dirty_region [uuid:{}] to: {}", str_uuid, *dirty_mirror->disk)
        }
        free(iov.iov_base);
        if (cmp_iov.iov_base) free(cmp_iov.iov_base);

        if (_metrics) { // GCOVR_EXCL_BR_LINE
            // LCOV_EXCL_START -- UblkRaidMetrics requires prometheus registry; not constructible in unit tests
            auto const final_count = s_active_resyncs.fetch_sub(1, std::memory_order_relaxed) - 1;
            auto const resync_end = std::chrono::steady_clock::now();
            auto const duration_seconds =
                std::chrono::duration_cast< std::chrono::seconds >(resync_end - resync_start).count();
            if (duration_seconds > 0) { _metrics->record_resync_complete(duration_seconds); }
            // Record the size of data that was resynced (initial size before resync started)
            _metrics->record_last_resync_size(initial_resync_size);
            _metrics->record_active_resyncs(final_count);
            _metrics->record_resync_initial_size(0); // clear ETA denominator when resync finishes
        } // LCOV_EXCL_STOP
    }

    cur_state = __load_state(); // reflect actual state after IDLE/STOPPING race in the loop
    if (resync_state::STOPPING == cur_state) {
        RLOGI("Resync Task Stopped for [uuid:{}] to: {}", str_uuid, *dirty_mirror->disk)
        for (auto stopping = resync_state::STOPPING;
             !__cas_state(stopping, resync_state::IDLE) && stopping == resync_state::STOPPING;)
            std::this_thread::yield();
        return;
    }

    // IDLE transition was performed inside the while loop.
    RLOGD("Resync Task Finished for [uuid:{}] to: {}", str_uuid, *dirty_mirror->disk)
}

void Raid1ResyncTask::launch(std::string const& str_uuid, std::shared_ptr< MirrorDevice > clean_mirror,
                             std::shared_ptr< MirrorDevice > dirty_mirror, std::function< bool() >&& complete) {
    auto lg = std::scoped_lock< std::mutex >(_launch_lock);
    // First we must become IDLE
    auto transitioned =
        __transition_to(resync_state::IDLE, resync_state::IDLE, [](resync_state state) -> transition_result {
            switch (state) {
            case resync_state::IDLE: // LCOV_EXCL_LINE -- CAS(IDLE→IDLE) succeeds if state==IDLE; handler never called
                                     // with IDLE
                return {state, transition_action::SUCCESS}; // LCOV_EXCL_LINE
            case resync_state::ACTIVE:
            case resync_state::SLEEPING:
                return {state, transition_action::EARLY_EXIT};
            case resync_state::STOPPING: // LCOV_EXCL_LINE -- transient; not deterministically catchable
                return {resync_state::IDLE, transition_action::RETRY_WITH_SLEEP}; // LCOV_EXCL_LINE
            }
            std::unreachable();
        });

    if (!transitioned) {
        RLOGD("Resync Task aborted for [uuid:{}] - already running", str_uuid)
        return;
    }

    // The previous resync thread may still be joinable even though state is IDLE — there is a
    // window between _start() setting state to IDLE and the thread actually returning. Assigning
    // to a joinable std::thread calls std::terminate(), so join here first.
    if (_resync_task.joinable()) _resync_task.join();

    _resync_task = sisl::named_thread(fmt::format("r_{}", str_uuid.substr(0, 13)),
                                      [this, uuid = str_uuid, clean = std::move(clean_mirror),
                                       dirty = std::move(dirty_mirror), compl_cb = std::move(complete)] mutable {
                                          sched_param sp{.sched_priority = 1};
                                          if (int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); rc != 0)
                                              RLOGE("resync thread: failed to set SCHED_FIFO/1: {}", strerror(rc))
                                          _start(uuid, clean, dirty, std::move(compl_cb));
                                      });
}

void Raid1ResyncTask::__clean(uint64_t addr, uint32_t len, MirrorDevice& clean_mirror) {
    auto const pg_size = _dirty_bitmap->page_size();
    auto iov = iovec{.iov_base = nullptr, .iov_len = pg_size};

    auto const end = addr + len;
    auto cur_off = addr;
    while (end > cur_off) {
        auto [page, pg_offset, sz] = _dirty_bitmap->clean_region(cur_off, end - cur_off);
        cur_off += sz;
        if (!page) continue;
        iov.iov_base = page;

        auto const page_addr = (pg_size * pg_offset) + pg_size;

        // These don't actually need to succeed; this page will remain dirty and loaded the next time
        // we use this bitmap (extra copies for this page).
        if (auto res = clean_mirror.disk->sync_iov(UBLK_IO_OP_WRITE, &iov, 1, page_addr); !res) {
            RLOGW("Failed to clear bitmap page to: {}", *clean_mirror.disk)
        }
    }
}

// Copy the region [addr, addr+len) from src to dest per the copy mode:
//   BLIND     - write the whole region unconditionally (destination known to diverge).
//   CHECK     - read the destination into cmp_iov and memcmp per page; write only divergent pages.
//               An explicit read+compare, correct for any backend. (Future: a backend-offloaded
//               region digest could replace the destination read, with this as the fallback.)
//   ZERO_TEST - destination asserted to read zero where unallocated: zero-detect each source page
//               and write only non-zero pages, with no destination read (thin-preserving).
// Non-blind modes decide at raid1::k_page_size (4 KiB), not the device block size: identical
// skip/allocate decisions at identical offsets keep both legs' thin allocation maps (backend
// B-trees) aligned, and with the source read at max_io and contiguous writes coalesced, the finer
// grain only costs memcmp/zero-detect over resident memory.
// Returns the source byte count on success -- including the all-skipped case, so the caller still
// cleans the bitmap -- or the error on a write failure. A CHECK destination-read failure is not fatal:
// it falls back to a BLIND write of the source (which may remap a media error), rather than looping.
// The resync operates on one contiguous buffer, so nr_vecs is fixed at 1.
static inline io_result __copy_region(iovec* src_iov, iovec* cmp_iov, uint64_t addr, auto& src, auto& dest,
                                      resync_copy_mode mode) {
    auto res = src.sync_iov(UBLK_IO_OP_READ, src_iov, 1, addr);
    if (!res) {
        RLOGE("Could not read Data of [sz:{}] [res:{}]", src_iov->iov_len, res.error().message())
        return res;
    }
    auto const len = static_cast< uint32_t >(src_iov->iov_len);

    // BLIND: one write of the whole region.
    if (resync_copy_mode::BLIND == mode) {
        if (auto w = dest.sync_iov(UBLK_IO_OP_WRITE, src_iov, 1, addr); !w) {
            RLOGW("Could not write clean chunks of [sz:{}] [res:{}]", len, w.error().message())
            return w;
        }
        return res;
    }

    // Non-BLIND always receives a buffer (an allocation failure downgrades to BLIND in _start).
    DEBUG_ASSERT(cmp_iov, "cmp_iov must be provided for non-BLIND copy modes");

    // CHECK: read the destination once so each page can be compared against it. A destination-read
    // failure (a media/URE error) is NOT fatal: fall back to a BLIND write of the source. The write
    // may relocate the failing block (homestore remaps a media error to a fresh LBA), healing the
    // region; without this the read fails forever and the resync loops on the same LBA. A write
    // failure IS a real device failure -- propagate it like any other write error.
    if (resync_copy_mode::CHECK == mode) {
        cmp_iov->iov_len = src_iov->iov_len;
        if (auto cmp = dest.sync_iov(UBLK_IO_OP_READ, cmp_iov, 1, addr); !cmp) {
            RLOGW("Could not read compare chunks of [sz:{}] [res:{}]; blind-writing the source", len,
                  cmp.error().message())
            if (auto w = dest.sync_iov(UBLK_IO_OP_WRITE, src_iov, 1, addr); !w) {
                RLOGW("Could not write clean chunks of [sz:{}] [res:{}]", len, w.error().message())
                return w;
            }
            return res;
        }
    }

    auto* const src_buf = static_cast< uint8_t* >(src_iov->iov_base);

    // Flush a coalesced run of to-write pages [run_start, run_end) as one write.
    auto flush = [&](uint32_t run_start, uint32_t run_end) -> io_result {
        auto w_iov = iovec{.iov_base = src_buf + run_start, .iov_len = run_end - run_start};
        auto w = dest.sync_iov(UBLK_IO_OP_WRITE, &w_iov, 1, addr + run_start);
        if (!w) RLOGW("Could not write clean pages of [sz:{}] [res:{}]", run_end - run_start, w.error().message())
        return w;
    };

    uint32_t run_start = 0;
    bool in_run = false;
    for (uint32_t off = 0; off < len; off += k_page_size) {
        auto const this_page = std::min< uint32_t >(k_page_size, len - off);
        // skip: page identical to the destination (CHECK) or all-zero onto a clean one (ZERO_TEST).
        bool const skip = (resync_copy_mode::CHECK == mode)
            ? (0 == memcmp(src_buf + off, static_cast< uint8_t const* >(cmp_iov->iov_base) + off, this_page))
            : (0 == isal_zero_detect(src_buf + off, this_page));
        if (skip) {
            if (in_run) {
                if (auto f = flush(run_start, off); !f) return f;
                in_run = false;
            }
        } else if (!in_run) {
            run_start = off;
            in_run = true;
        }
    }
    if (in_run) {
        if (auto f = flush(run_start, len); !f) return f;
    }
    return res;
}

resync_state Raid1ResyncTask::__run(auto& clean_mirror, auto& dirty_mirror, iovec* iov, iovec* cmp_iov,
                                    bool blind_fallback) noexcept {
    static auto const unavail_delay = std::chrono::seconds(SISL_OPTIONS["avail_delay"].as< uint32_t >());
    static auto const avail_delay = std::chrono::microseconds(SISL_OPTIONS["resync_delay"].as< uint32_t >());

    auto cur_state = resync_state::ACTIVE;
    uint32_t consecutive_unavail = 0;

    auto nr_pages = _dirty_bitmap->dirty_pages();
    if (_metrics) { _metrics->record_dirty_pages(nr_pages, _dirty_bitmap->dirty_data_est()); } // GCOVR_EXCL_BR_LINE

    // When a dirty run is entirely blocked by in-flight writes (!any_copy), skip past it on
    // the next sweep so higher-LBA dirty runs are not starved. Reset after use within each
    // sweep; set at end of inner loop to advance past a stuck run on the next sweep.
    uint64_t resync_skip_from = 0;

    while (0 < nr_pages) {
        // Skip copies entirely if the dirty mirror is known unavailable
        if (dirty_mirror->unavail.test(std::memory_order_acquire)) {
            if (++consecutive_unavail % 10 == 0)
                RLOGW("Resync blocked: dirty mirror unreachable for ~{}s (probe reads failing) [{}]",
                      consecutive_unavail * SISL_OPTIONS["avail_delay"].as< uint32_t >(), *dirty_mirror->disk)
            if (cur_state = __yield(unavail_delay, avail_delay); resync_state::STOPPING == cur_state) break;
            probe_mirror(*dirty_mirror, _offset);
            nr_pages = _dirty_bitmap->dirty_pages();
            if (_metrics) _metrics->record_dirty_pages(nr_pages, _dirty_bitmap->dirty_data_est()); // GCOVR_EXCL_BR_LINE
            continue;
        }
        consecutive_unavail = 0;

        // TODO Change this so it's easier to control with a future QoS algorithm
        auto copies_left = ((std::min(32U, SISL_OPTIONS["resync_level"].as< uint32_t >()) * 100U) / 32U) * 5U;

        // Use the skip cursor if a fully-conflicting run was detected last sweep.
        auto [logical_off, sz] =
            resync_skip_from > 0 ? _dirty_bitmap->next_dirty_after(resync_skip_from) : _dirty_bitmap->next_dirty();
        if (0 == sz && resync_skip_from > 0) // nothing after cursor — wrap to beginning
            std::tie(logical_off, sz) = _dirty_bitmap->next_dirty();
        resync_skip_from = 0;

        // copies_left counts copy attempts (read+write to replica); only Phase 1 skips are free.
        // any_copy tracks whether this inner pass produced at least one copy; if a full
        // dirty run is exhausted via Phase-1 skips alone, we set resync_skip_from and break
        // to let __yield() fire, advancing past the stuck run on the next sweep.
        bool any_copy = false;
        while (0 < sz && 0U < copies_left) {
            auto const iov_len = std::min(sz, _max_size);

            // Capture generation before Phase 1 so Phase 2 can detect writes that complete
            // entirely between the two checks (slot freed before Phase 2 runs).
            auto const gen_before = _region_tracker.snapshot_gen();

            // Phase 1: pre-copy conflict check — skip this chunk if a write is in-flight
            // for this range. Advance within the dirty run so unrelated chunks still copy.
            if (_region_tracker.overlaps(logical_off, iov_len)) {
                sz -= iov_len;
                logical_off += iov_len;
                if (0 == sz) {
                    if (!any_copy) {
                        resync_skip_from = logical_off; // advance past conflicting run next sweep
                        break;
                    }
                    std::tie(logical_off, sz) = _dirty_bitmap->next_dirty();
                    any_copy = false;
                }
                continue;
            }

            // Re-read the live mode per copy so an I/O-path re-dirty (or this run's own earlier
            // downgrade) takes effect immediately. blind_fallback pins BLIND when no compare buffer
            // was allocated.
            auto const eff = blind_fallback ? resync_copy_mode::BLIND : _live_mode->load(std::memory_order_acquire);
            iov->iov_len = iov_len; // Copy Region from clean to dirty
            if (auto res =
                    __copy_region(iov, cmp_iov, logical_off + _offset, *clean_mirror->disk, *dirty_mirror->disk, eff);
                res) {
                // Phase 2: post-copy conflict check. Two cases require skipping __clean:
                //   (a) overlaps() — write is still in-flight (single CAS slot still holds
                //       the packed value; k_free is not visible until untrack() completes).
                //   (b) completed_since() — write arrived AND fully completed during the READ
                //       window; slot freed before Phase 2 ran; shadow log catches this.
                if (!_region_tracker.overlaps(logical_off, iov_len) &&
                    !_region_tracker.completed_since(logical_off, iov_len, gen_before)) {
                    __clean(logical_off, iov->iov_len, *clean_mirror);
                    if (_metrics) { _metrics->record_resync_progress(iov->iov_len); } // GCOVR_EXCL_BR_LINE
                } else if (resync_copy_mode::ZERO_TEST == eff) {
                    // A write raced this copy; if it zeroes the source in place, a zero-detect
                    // re-sync would skip the region and leave stale destination data we already
                    // wrote. Publish ZERO_TEST -> CHECK through _live_mode (cmp_iov is pre-allocated) so
                    // the rest of the run reads-and-compares, and the downgrade persists across a
                    // stop/relaunch. Monotonic CAS: a no-op if a concurrent re-dirty already tainted it.
                    RLOGI("Resync downgrading ZERO_TEST -> CHECK after a write conflict to: {}", *dirty_mirror->disk)
                    auto exp = resync_copy_mode::ZERO_TEST;
                    _live_mode->compare_exchange_strong(exp, resync_copy_mode::CHECK, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed);
                }
                any_copy = true;
                --copies_left;
                sz -= iov_len;
                logical_off += iov_len;
                if (0 == sz) {
                    std::tie(logical_off, sz) = _dirty_bitmap->next_dirty();
                    any_copy = false;
                }
            } else {
                dirty_mirror->unavail.test_and_set(std::memory_order_acq_rel);
                break;
            }
        }

        // Yield and check for stopped
        if (cur_state = __yield(dirty_mirror->unavail.test(std::memory_order_acquire) ? unavail_delay : avail_delay,
                                avail_delay);
            resync_state::STOPPING == cur_state)
            break;

        // Sweep and count dirty pages left
        nr_pages = _dirty_bitmap->dirty_pages();
        if (_metrics) _metrics->record_dirty_pages(nr_pages, _dirty_bitmap->dirty_data_est()); // GCOVR_EXCL_BR_LINE
    }
    return cur_state;
}

// Abort any on-going resync task by moving to STOPPING and rejoin the thread
void Raid1ResyncTask::stop() noexcept {
    auto lg = std::scoped_lock< std::mutex >(_launch_lock);
    // Targets SLEEPING → STOPPING (waits out ACTIVE first via RETRY_WITH_SLEEP).
    // Never CAS-es ACTIVE→STOPPING directly — this is what makes the ACTIVE→STOPPING
    // assert in __yield Phase 1 unreachable.
    __transition_to(resync_state::SLEEPING, resync_state::STOPPING, [this](resync_state state) -> transition_result {
        switch (state) {
        case resync_state::IDLE: {
            if (_resync_task.joinable()) return {state, transition_action::RETRY_WITH_SLEEP};
            [[fallthrough]];
        }
        case resync_state::STOPPING:
            return {state, transition_action::SUCCESS};
        case resync_state::ACTIVE:
            return {resync_state::SLEEPING, transition_action::RETRY_WITH_SLEEP};
        case resync_state::SLEEPING: // LCOV_EXCL_START -- 0ns window with resync_delay=0
            return {state, transition_action::RETRY};
            // LCOV_EXCL_STOP
        }
        std::unreachable();
    });
    if (_resync_task.joinable()) _resync_task.join();
    // If the thread finished naturally (ACTIVE→IDLE) before stop() CAS'd IDLE→STOPPING,
    // it returned without ever seeing STOPPING and never cleared it. _launch_lock is held
    // so no concurrent caller can observe this window; reset to IDLE so launch() isn't stuck.
    for (auto stopping = resync_state::STOPPING;
         !__cas_state(stopping, resync_state::IDLE) && stopping == resync_state::STOPPING;)
        std::this_thread::yield();
}

resync_state Raid1ResyncTask::__yield(std::chrono::microseconds const yield_for,
                                      std::chrono::microseconds const spin_time) noexcept {
    _yield_count.fetch_add(1, std::memory_order_relaxed);
    auto cur_state = resync_state::ACTIVE;

    // Phase 1: Transition ACTIVE→SLEEPING (give I/O a chance to interrupt)
    while (!__cas_state(cur_state, resync_state::SLEEPING)) {
        // STOPPING here is unreachable: stop() only CAS SLEEPING→STOPPING,
        // never ACTIVE→STOPPING, so this loop exits on the first successful CAS.
        DEBUG_ASSERT_NE(cur_state, resync_state::STOPPING, "impossible ACTIVE→STOPPING transition"); // LCOV_EXCL_LINE
    }
    cur_state = resync_state::SLEEPING;

    // Phase 2: Sleep for yield_for, checking for STOPPING periodically
    auto const end_time = std::chrono::steady_clock::now() + yield_for;
    while (std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(spin_time);
        if (resync_state::STOPPING == __load_state()) return resync_state::STOPPING;
    }

    // Phase 3: Transition SLEEPING→ACTIVE (resume resync)
    while (!__cas_state(cur_state, resync_state::ACTIVE)) {
        if (resync_state::STOPPING == cur_state) return cur_state;
        cur_state = resync_state::SLEEPING; // reset after spurious CAS failure
    }
    return resync_state::ACTIVE;
}

// Probes mirror reachability with a single synchronous page read.
// Returns true  — device is reachable; unavail flag has been cleared.
// Returns false — device failed the probe; unavail flag has been set.
bool Raid1ResyncTask::probe_mirror(MirrorDevice& mirror, uint64_t reserved_size) noexcept {
    alignas(k_page_size) uint8_t probe_buf[k_page_size];
    auto iov = iovec{.iov_base = probe_buf, .iov_len = k_page_size};
    if (auto res = mirror.disk->sync_iov(UBLK_IO_OP_READ, &iov, 1, reserved_size); res) {
        mirror.unavail.clear(std::memory_order_release);
        return true;
    }
    mirror.unavail.test_and_set(std::memory_order_acq_rel);
    return false;
}

} // namespace ublkpp::raid1
