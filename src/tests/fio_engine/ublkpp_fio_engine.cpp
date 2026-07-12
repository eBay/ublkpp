/// ublkpp fio external ioengine
///
/// Drives FSDisk / Raid0Disk / Raid1Disk through MockUblksrv so fio can
/// exercise real io_uring I/O paths without the ublk kernel module.
///
/// Usage (fio job file):
///   ioengine=external:${ENGINE_SO}
///   disk_type=fsdisk          # or raid0 / raid1
///   disk_files=/tmp/a.img     # colon-separated for RAID
///   raid_chunk_size=32768     # optional, default 32 KiB

#include <cassert>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <spdlog/spdlog.h>

#include "ublkpp/drivers.hpp"
#include "ublkpp/raid.hpp"
#include "ublkpp/craft_disk.hpp" // make_craft_disk_local (in-process CRAFT reference, no backing files)

#include "lib/common.hpp"
#include "mock_ublksrv/mock_ublksrv.hpp"

// Suppress fio's fallback syscall wrappers — glibc on this platform
// already exports these functions, and fio's static-inline versions
// conflict with the extern declarations in system headers.
#define CONFIG_HAVE_GETTID 1     // glibc >= 2.30 exports gettid()
#define CONFIG_PWRITEV2 1        // glibc exports preadv2/pwritev2
#define CONFIG_SYNC_FILE_RANGE 1 // glibc exports sync_file_range

// fio headers must be included as C
extern "C" {
#include <fio/fio.h>
#include <fio/ioengines.h>
#include <fio/io_u.h>
#include <fio/optgroup.h>
}
// fio/minmax.h defines min/max as C macros — undefine so std::min/max work
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// ---------------------------------------------------------------------------
// SISL initialisation — required before constructing any ublk_disk
// ---------------------------------------------------------------------------
#ifdef BUILD_COVERAGE
// Forward-declare at file scope — extern "C" is not allowed inside a function body.
extern "C" void __gcov_dump(void);
#endif

SISL_LOGGING_INIT(ublk_drivers, ublk_raid, ublksrv)
SISL_OPTIONS_ENABLE(logging, raid1)

static void ensure_sisl_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        int argc = 3;
        char prog[] = "ublkpp_fio";
        char v_flag[] = "-v";
        char v_level[] = "debug";
        char* argv[] = {prog, v_flag, v_level};
        SISL_OPTIONS_LOAD(argc, argv, logging, raid1);
        sisl::logging::SetLogger("ublkpp_fio");
        // Set trace on all loggers — including SISL per-module loggers already created
        spdlog::set_pattern("[%D %T.%e] [%n] [%^%l%$] [%t] %v");
    });
}

// ---------------------------------------------------------------------------
// Custom fio options
// ---------------------------------------------------------------------------
struct ublkpp_options {
    void* pad;                    // required: first field must be void* (thread_data placeholder)
    char* disk_type;              // "fsdisk" | "raid0" | "raid1"
    char* disk_files;             // colon-separated backing file paths
    unsigned int raid_chunk_size; // bytes, default 32 KiB
};

static fio_option engine_options[] = {
    {
        .name = "disk_type",
        .lname = "Disk type",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(struct ublkpp_options, disk_type),
        .help = "Disk type: fsdisk, raid0, raid1, raid10",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "disk_files",
        .lname = "Disk files",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(struct ublkpp_options, disk_files),
        .help = "Colon-separated backing file paths",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "raid_chunk_size",
        .lname = "RAID chunk size",
        .type = FIO_OPT_INT,
        .off1 = offsetof(struct ublkpp_options, raid_chunk_size),
        .help = "RAID chunk size in bytes (default 32768)",
        .def = "32768",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {.name = nullptr},
};

// ---------------------------------------------------------------------------
// Per-thread engine runtime state
// ---------------------------------------------------------------------------
struct EngineData {
    std::shared_ptr< ublkpp::ublk_disk > disk;
    std::unique_ptr< ublkpp::MockUblksrv > mock;
    std::vector< io_u* > pending; // indexed by tag
    std::vector< io_u* > events;  // completions from last getevents()
    std::vector< int > free_tags; // available tag slots
    // poll() drains all available CQEs at once; if that exceeds max_events we
    // cache the surplus here so subsequent getevents() calls can return them
    // without re-entering the ring.
    std::deque< ublkpp::MockUblksrv::Completion > completion_cache;
};

static EngineData* engine_data(struct thread_data* td) { return reinterpret_cast< EngineData* >(td->io_ops_data); }

// Derive a deterministic UUID from a list of backing file paths using UUIDv5
// (name-based SHA1) so that multiple fio job sections opening the same files
// always produce the same UUID and the superblock check passes.
static boost::uuids::uuid uuid_for_paths(std::vector< std::string > const& paths) {
    // ns:OID namespace UUID
    static const boost::uuids::uuid ns = boost::uuids::string_generator()("6ba7b812-9dad-11d1-80b4-00c04fd430c8");
    std::string key;
    for (auto const& p : paths) {
        key += p;
        key += '\0';
    }
    return boost::uuids::name_generator_sha1{ns}(key.c_str(), key.size());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::vector< std::string > split_colon(char const* s) {
    std::vector< std::string > result;
    if (!s || !*s) return result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ':'))
        if (!token.empty()) result.push_back(token);
    return result;
}

// Pre-allocate a backing file to the requested size using fallocate/truncate,
// then pre-populate with zeros to convert unwritten extents to written and
// warm the page cache, eliminating first-write overhead during tests.
static bool ensure_file_size(std::string const& path, uint64_t size) {
    // Block devices are passed directly to FSDisk which handles them natively.
    // Skip sizing and pre-population — they already have fixed capacity.
    // clang-format off
    struct stat st{};
    // clang-format on
    if (stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) return true;

    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return false;

    int r = posix_fallocate(fd, 0, static_cast< off_t >(size));
    if (r != 0) {
        // fall back to truncate (works on tmpfs which doesn't support fallocate)
        r = ftruncate(fd, static_cast< off_t >(size));
    }

    if (r == 0) {
        // Pre-populate with zeros to convert any unwritten extents to written
        // and warm the page cache, eliminating first-write overhead during tests.
        static constexpr size_t k_chunk = 4UL << 20; // 4 MiB
        std::vector< char > const zeros(std::min(k_chunk, static_cast< size_t >(size)), 0);
        bool ok = true;
        for (uint64_t off = 0; off < size && ok; off += k_chunk) {
            auto const n = static_cast< size_t >(std::min(k_chunk, size - off));
            ok = pwrite(fd, zeros.data(), n, static_cast< off_t >(off)) == static_cast< ssize_t >(n);
        }
        if (!ok) r = -1;
    }

    close(fd);
    return r == 0;
}

static uint8_t ddir_to_ublk_op(enum fio_ddir ddir) {
    switch (ddir) {
    case DDIR_READ:
        return UBLK_IO_OP_READ;
    case DDIR_WRITE:
        return UBLK_IO_OP_WRITE;
    case DDIR_TRIM:
        return UBLK_IO_OP_DISCARD;
    case DDIR_SYNC:
    case DDIR_DATASYNC:
        return UBLK_IO_OP_FLUSH;
    default:
        return UBLK_IO_OP_READ;
    }
}

// ---------------------------------------------------------------------------
// Engine callbacks
// ---------------------------------------------------------------------------
static int ublkpp_init(struct thread_data* td) {
    ensure_sisl_init();

    auto* opts = reinterpret_cast< ublkpp_options* >(td->eo);
    char const* disk_type = (opts && opts->disk_type) ? opts->disk_type : "fsdisk";
    char const* disk_files = (opts && opts->disk_files) ? opts->disk_files : "";
    uint32_t chunk_size = (opts && opts->raid_chunk_size) ? opts->raid_chunk_size : 32768u;

    std::string const type(disk_type);
    bool const is_craft = (type == "craft");

    auto paths = split_colon(disk_files);
    if (!is_craft && paths.empty()) {
        log_err("ublkpp_fio: disk_files option is required\n");
        return -1;
    }

    // Size: use td->o.size + td->o.start_offset, rounded to sector, plus
    // 64 MiB overhead for RAID metadata / superblocks
    uint64_t const disk_size = ((td->o.size + td->o.start_offset + 511ULL) & ~511ULL) + (64ULL << 20);

    // CRAFT is an in-process reference cluster -- no backing files to allocate.
    if (!is_craft) {
        for (auto const& p : paths) {
            if (!ensure_file_size(p, disk_size)) {
                log_err("ublkpp_fio: cannot allocate backing file %s\n", p.c_str());
                return -1;
            }
        }
    }

    // Build the disk
    std::shared_ptr< ublkpp::ublk_disk > disk;
    try {
        if (type == "craft") {
            // A 3-replica in-process CRAFT cluster, self-sized from `disk_size` (no files, no server, no kernel).
            // Drives the ON-RING transport: CraftDisk::prepare binds the client to MockUblksrv's ring, so fio at
            // iodepth>1 puts many replica legs in flight together -- the depth CRAFT correctness needs. Each fio
            // job builds its own cluster; writes + verify hit the same in-process disk within the run.
            disk = ublkpp::make_craft_disk_local(boost::uuids::random_generator()(), /*n=*/3, /*page_size=*/4096,
                                                 /*capacity=*/disk_size);
        } else if (type == "fsdisk") {
            disk = ublkpp::make_fs_disk(paths[0]);
        } else if (type == "raid0") {
            if (paths.size() < 2) {
                log_err("ublkpp_fio: raid0 requires at least 2 disk_files\n");
                return -1;
            }
            std::vector< std::shared_ptr< ublkpp::ublk_disk > > members;
            for (auto const& p : paths)
                members.push_back(ublkpp::make_fs_disk(p));
            disk = ublkpp::make_raid0_disk(uuid_for_paths(paths), chunk_size, std::move(members));
        } else if (type == "raid1") {
            if (paths.size() < 2) {
                log_err("ublkpp_fio: raid1 requires 2 disk_files\n");
                return -1;
            }
            // Sparse test images read zero where unallocated: assume_clean skips the initial sync.
            disk = ublkpp::make_raid1_disk(uuid_for_paths(paths), ublkpp::make_fs_disk(paths[0]),
                                           ublkpp::make_fs_disk(paths[1]), "", true);
        } else if (type == "raid10") {
            if (paths.size() != 4) {
                log_err("ublkpp_fio: raid10 requires exactly 4 disk_files\n");
                return -1;
            }
            // RAID10: two mirrored pairs striped together
            std::vector< std::string > const pair_a_paths{paths[0], paths[1]};
            std::vector< std::string > const pair_b_paths{paths[2], paths[3]};
            auto pair_a = ublkpp::make_raid1_disk(uuid_for_paths(pair_a_paths), ublkpp::make_fs_disk(paths[0]),
                                                  ublkpp::make_fs_disk(paths[1]), "", true);
            auto pair_b = ublkpp::make_raid1_disk(uuid_for_paths(pair_b_paths), ublkpp::make_fs_disk(paths[2]),
                                                  ublkpp::make_fs_disk(paths[3]), "", true);
            std::vector< std::shared_ptr< ublkpp::ublk_disk > > members{std::move(pair_a), std::move(pair_b)};
            disk = ublkpp::make_raid0_disk(uuid_for_paths(paths), chunk_size, std::move(members));
        } else {
            log_err("ublkpp_fio: unknown disk_type '%s'\n", disk_type);
            return -1;
        }
    } catch (std::exception const& e) {
        log_err("ublkpp_fio: disk init failed: %s\n", e.what());
        return -1;
    }

    int const iodepth = td->o.iodepth;
    auto ed = new EngineData();
    try {
        ed->mock = std::make_unique< ublkpp::MockUblksrv >(disk, iodepth);
    } catch (std::exception const& e) {
        log_err("ublkpp_fio: MockUblksrv init failed: %s\n", e.what());
        delete ed;
        return -1;
    }
    ed->disk = std::move(disk);
    ed->pending.assign(iodepth, nullptr);
    ed->events.reserve(iodepth);
    ed->free_tags.reserve(iodepth);
    for (int i = iodepth - 1; i >= 0; --i)
        ed->free_tags.push_back(i);

    td->io_ops_data = ed;
    return 0;
}

static void ublkpp_cleanup(struct thread_data* td) {
    auto* ed = engine_data(td);
    if (!ed) return;
    delete ed;
    td->io_ops_data = nullptr;
#ifdef BUILD_COVERAGE
    // fio calls dlclose() on the engine .so after cleanup, which happens before
    // the process-level atexit() handlers run.  Flush gcov coverage data here,
    // while the .so is still mapped, so the io_uring I/O paths exercised by the
    // functional tests are included in the coverage report.
    __gcov_dump();
#endif
}

static enum fio_q_status ublkpp_queue(struct thread_data* td, struct io_u* io_u) {
    auto* ed = engine_data(td);

    if (ed->free_tags.empty()) return FIO_Q_BUSY;

    int const tag = ed->free_tags.back();
    ed->free_tags.pop_back();

    uint8_t const op = ddir_to_ublk_op(io_u->ddir);
    uint64_t const start_sector = io_u->offset >> ublkpp::SECTOR_SHIFT;
    uint32_t nr_sectors = 0;
    void* buf = io_u->xfer_buf;

    if (op == UBLK_IO_OP_READ || op == UBLK_IO_OP_WRITE) {
        nr_sectors = static_cast< uint32_t >(io_u->xfer_buflen >> ublkpp::SECTOR_SHIFT);
    } else if (op == UBLK_IO_OP_DISCARD) {
        nr_sectors = static_cast< uint32_t >(io_u->xfer_buflen >> ublkpp::SECTOR_SHIFT);
        buf = nullptr;
    }
    // FLUSH: nr_sectors = 0, buf = nullptr

    auto res = ed->mock->submit_io(tag, op, start_sector, nr_sectors, buf);
    if (!res) {
        io_u->error = EIO;
        ed->free_tags.push_back(tag);
        return FIO_Q_COMPLETED;
    }

    // Synchronous completions (0 sub_cmds: flush, pre-SQE error) are recorded on the mock's
    // completed list at submit and delivered exactly once by the next poll(), so they are queued
    // like any other IO — completing them here too would double-report the tag.
    io_u->engine_data = reinterpret_cast< void* >(static_cast< uintptr_t >(tag));
    ed->pending[tag] = io_u;
    return FIO_Q_QUEUED;
}

static int ublkpp_getevents(struct thread_data* td, unsigned int min_events, unsigned int max_events,
                            const struct timespec* t) {
    auto* ed = engine_data(td);
    ed->events.clear();

    // Convert timespec → milliseconds; nullptr means "block indefinitely"
    auto timeout = std::chrono::milliseconds{30'000};
    if (t) {
        timeout = std::chrono::duration_cast< std::chrono::milliseconds >(std::chrono::seconds{t->tv_sec} +
                                                                          std::chrono::nanoseconds{t->tv_nsec});
    }

    // Helper: move one Completion into ed->events (updates pending/free_tags)
    auto consume = [&](ublkpp::MockUblksrv::Completion const& c) {
        io_u* u = ed->pending[c.tag];
        assert(u);
        ed->pending[c.tag] = nullptr;
        ed->free_tags.push_back(c.tag);
        if (c.result < 0) {
            u->error = -c.result;
        } else {
            u->error = 0;
            u->resid = 0;
        }
        ed->events.push_back(u);
    };

    // Drain cached surplus completions from a previous poll() call first
    while (!ed->completion_cache.empty() && ed->events.size() < max_events) {
        consume(ed->completion_cache.front());
        ed->completion_cache.pop_front();
    }

    // If the cache already satisfied min_events we are done
    if (ed->events.size() >= min_events) return static_cast< int >(ed->events.size());

    // Still need more — ask the ring for them
    int const still_need = static_cast< int >(min_events - ed->events.size());
    auto completions = ed->mock->poll(still_need, timeout);

    for (auto const& c : completions) {
        if (ed->events.size() < max_events) {
            consume(c);
        } else {
            // poll() drained more CQEs than fio asked for; cache them so they
            // are returned on the next getevents() call instead of being lost
            ed->completion_cache.push_back(c);
        }
    }

    return static_cast< int >(ed->events.size());
}

static struct io_u* ublkpp_event(struct thread_data* td, int event) {
    auto* ed = engine_data(td);
    return ed->events[static_cast< size_t >(event)];
}

// fio 3.28 asserts that open_file/close_file are non-null even with FIO_DISKLESSIO.
// Provide no-ops so fio doesn't crash during job setup.
static int ublkpp_open_file(struct thread_data* /*td*/, struct fio_file* /*f*/) { return 0; }
static int ublkpp_close_file(struct thread_data* /*td*/, struct fio_file* /*f*/) { return 0; }

// ---------------------------------------------------------------------------
// Engine ops registration
// ---------------------------------------------------------------------------
static struct ioengine_ops ioengine = {
    .name = "ublkpp",
    .version = FIO_IOOPS_VERSION,
    // FIO_RAWIO:     sector-aligned buffer allocation
    // FIO_DISKLESSIO: skip fio's file size-check; open_file/close_file are still called
    // FIO_NOEXTEND:  don't try to extend files
    .flags = FIO_RAWIO | FIO_DISKLESSIO | FIO_NOEXTEND,
    .init = ublkpp_init,
    .queue = ublkpp_queue,
    .getevents = ublkpp_getevents,
    .event = ublkpp_event,
    .cleanup = ublkpp_cleanup,
    .open_file = ublkpp_open_file,
    .close_file = ublkpp_close_file,
    .option_struct_size = sizeof(struct ublkpp_options),
    .options = engine_options,
};

extern "C" {
void get_ioengine(struct ioengine_ops** ops) { *ops = &ioengine; }
} // extern "C"
