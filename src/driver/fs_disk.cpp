#include "ublkpp/drivers.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
}

#include <fstream>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <ublksrv_utils.h>
#include <ublksrv.h>

#include <ublkpp/lib/cqe_state.hpp>
#include <ublkpp/lib/ublk_disk.hpp>

#include "fs_disk_impl.hpp"
#include "lib/common.hpp"
#include "lib/logging.hpp"
#include "metrics/ublk_fsdisk_metrics.hpp"
#include "target/ublkpp_tgt_impl.hpp"

namespace ublkpp {

// On kernel <= 5.4, io_uring CQE delivery for buffered (non-O_DIRECT) I/O has a write-ordering
// bug: the CQE is returned before the data is committed to the page cache.  A subsequent buffered
// read can therefore see stale data, causing verify failures.  This is distinct from the fio
// engine getevents() bug (fixed via completion_cache) and is a genuine kernel defect.
// Production deployments are unaffected because ublk_drv requires kernel >= 5.19, but test
// environments (MockUblksrv) may run on older kernels.  Fall back to synchronous preadv2/pwritev2
// on affected kernels so CQE delivery implies the data is already visible to page-cache readers.
// When the minimum supported test kernel is raised above 5.4, this guard and the sync_iov
// fallback in async_iov can be removed.
static bool buffered_uring_broken() {
    // clang-format off
    struct utsname uts{};
    // clang-format on
    if (uname(&uts) != 0) return true; // conservative: assume broken if uname fails
    unsigned major = 0, minor = 0;
    sscanf(uts.release, "%u.%u", &major, &minor); // NOLINT(cert-err34-c)
    return major < 5 || (major == 5 && minor <= 4);
}
static bool const k_buffered_uring_broken = buffered_uring_broken();

// File-local concrete ublk_disk; constructed only via the make_fs_disk factory below. The public
// header exposes only the factory; consumers (raid composers, target wiring) operate against the
// ublk_disk virtual interface.
class FSDisk : public ublk_disk {
    std::filesystem::path _path;
    int _fd{-1};
    bool _block_device{false};
    std::unique_ptr< UblkFSDiskMetrics > _metrics;

public:
    explicit FSDisk(std::filesystem::path const& path, std::string const& parent_id = "");
    ~FSDisk() override;

    std::string id() const noexcept override { return _path.native(); }

    prepare_result prepare(ublksrv_queue const*, int const) override;
    disk_task< int > async_iov(ublksrv_queue const* q, ublk_io_data const* data, iovec* iovecs, uint32_t nr_vecs,
                               uint64_t addr) override;
    io_result sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept override;

private:
    std::pair< io_result, cqe_state* > handle_discard(ublksrv_queue const* q, ublk_io_data const* data, uint32_t len,
                                                      uint64_t addr);
};

FSDisk::FSDisk(std::filesystem::path const& path, std::string const& parent_id) : ublk_disk(), _path(path) {
    // Create metrics with parent_id for correlation
    if (!parent_id.empty()) { _metrics = std::make_unique< UblkFSDiskMetrics >(parent_id, _path.string()); }
    auto const str_path = _path.native();
    _fd = open(str_path.c_str(), O_RDWR);
    if (_fd < 0) {
        DLOGE("backing file {} can't be opened: {}", str_path, strerror(errno))
        throw std::runtime_error("Open Failed!");
    }
    // RAII guard: closes _fd if any subsequent constructor step throws.
    struct FdGuard {
        FSDisk* _self;
        bool _released{false};
        explicit FdGuard(FSDisk* s) noexcept : _self(s) {}
        ~FdGuard() noexcept {
            if (!_released && _self->_fd >= 0) {
                close(_self->_fd);
                _self->_fd = -1;
            }
        }
        void release() noexcept { _released = true; }
    } fd_scope{this};

    // clang-format off
    struct stat st{};
    // clang-format on
    if (fstat(_fd, &st) < 0) {
        DLOGE("fstat({}) failed: {}", str_path, strerror(errno))
        throw std::runtime_error("fstat Failed!");
    }
    // Reject backing files with no write permission bits for anyone, independent of the
    // process's effective privileges -- root's CAP_DAC_OVERRIDE would otherwise let open()
    // silently succeed R/W on a file that was deliberately marked read-only.
    if ((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
        DLOGE("backing file {} has no write permission bits set", str_path)
        throw std::runtime_error("Backing file is read-only!");
    }

    uint64_t bytes{0};
    _block_device = S_ISBLK(st.st_mode);
    auto& our_params = *params();
    if (_block_device) {
        uint32_t lbs;
        uint32_t pbs;
        if (ioctl(_fd, BLKGETSIZE64, &bytes) != 0 || ioctl(_fd, BLKSSZGET, &lbs) != 0 ||
            ioctl(_fd, BLKPBSZGET, &pbs) != 0)
            throw std::runtime_error("ioctl Failed!");
        if (block_has_unmap(st)) our_params.types |= UBLK_PARAM_TYPE_DISCARD;
        if (lbs == 0 || pbs == 0) throw std::runtime_error("Block device reported zero block size!");
        our_params.basic.logical_bs_shift = static_cast< uint8_t >(ilog2(lbs));
        our_params.basic.physical_bs_shift = static_cast< uint8_t >(ilog2(pbs));
        DLOGD("Backing is a block device [{}:{}:{}]!", str_path, lbs, pbs)

    } else if (S_ISREG(st.st_mode)) {
        bytes = st.st_size;
        our_params.types |= UBLK_PARAM_TYPE_DISCARD;
        auto const lbs = static_cast< uint32_t >(st.st_blksize);
        if (lbs == 0) throw std::runtime_error("Regular file reported zero block size!");
        our_params.basic.logical_bs_shift = static_cast< uint8_t >(ilog2(lbs));
        our_params.basic.physical_bs_shift = our_params.basic.logical_bs_shift;
        DLOGD("Backing is a regular file [{}:{}:{}]!", str_path, lbs, lbs)
    } else {
        DLOGE("fstat({}) returned non-block/non-regular file!", str_path, strerror(errno))
        throw std::runtime_error("Bad file!");
    }
    if (st.st_blksize && can_discard()) our_params.discard.discard_granularity = static_cast< uint32_t >(st.st_blksize);

    // in case of buffered io, use common bs/pbs so that all FS
    // image can be supported.
    // On overlayfs (common in Docker/CI), fcntl F_SETFL O_DIRECT succeeds and
    // pread/pwrite syscalls succeed, but io_uring I/O returns EINVAL.  Detect
    // overlayfs via statfs and skip O_DIRECT unconditionally on that filesystem.
    // For other filesystems, fall back to buffered if fcntl itself fails.
    // clang-format off
    struct statfs sfs{};
    // clang-format on
    constexpr unsigned long k_overlayfs_magic = 0x794c7630UL;
    bool const overlayfs = (fstatfs(_fd, &sfs) == 0 && static_cast< unsigned long >(sfs.f_type) == k_overlayfs_magic);
    if (overlayfs) {
        DLOGD("overlayfs detected — O_DIRECT not supported by io_uring on this fs, using BUFFERED.")
    } else if (!fcntl(_fd, F_SETFL, O_DIRECT)) {
        _direct_io = true;
    } else {
        DLOGD("Unable to support DIRECT I/O, using BUFFERED.")
    }
    our_params.basic.dev_sectors = bytes >> SECTOR_SHIFT;
    // Align size to max_sector size
    our_params.basic.dev_sectors -= (our_params.basic.dev_sectors % our_params.basic.max_sectors);
    // discard_granularity is zero-initialized and only set from st.st_blksize when the device
    // supports discard (can_discard()). If it stayed zero, discard was not configured — strip flag.
    if (our_params.discard.discard_granularity == 0) { our_params.types &= ~UBLK_PARAM_TYPE_DISCARD; }
    fd_scope.release(); // constructor succeeded: _fd ownership transferred to this
}

FSDisk::~FSDisk() {
    if (0 <= _fd) {
        if (!_direct_io) fdatasync(_fd);
        close(_fd);
    }
}

// TODO:
// This is an optimization to register the Linux FDs in the kernel, currently it is disabled
// as we don't support unregistering an FD during RAID1::swap_devcice, re-enable if this is fixed. and
// enable IOSQE_FIXED_FILE below.
// prepare_result FSDisk::prepare(ublksrv_queue const*, int const iouring_device_start) {
//    RELEASE_ASSERT_GT(_fd, -1, "FileDescriptor invalid {}", _fd)
//    _uring_device = iouring_device_start;
//    // We duplicate the FD here so ublksrv doesn't close it before we're ready
//    return {.fds = {dup(_fd)}, .max_sqes_per_io = 1};
//}

FSDisk::prepare_result FSDisk::prepare(ublksrv_queue const*, int const) {
    return {.max_sqes_per_io = 1}; // READ/WRITE submit 1 SQE; FLUSH and block-DISCARD submit 0
}

disk_task< int > FSDisk::async_iov(ublksrv_queue const* q, ublk_io_data const* data, iovec* iovecs, uint32_t nr_vecs,
                                   uint64_t addr) {
    auto const op = ublksrv_get_op(data->iod);

    if (op == UBLK_IO_OP_FLUSH) co_return 0;

    // Captured before any suspension: callers may pass frame-local iovecs that are invalid
    // after start() returns (see the async_iov invariant in ublk_disk.hpp).
    auto const expected_len = iovec_len(iovecs, iovecs + nr_vecs);

    io_result res;
    cqe_state* state{nullptr};
    bool track_metrics{false};
    if (op == UBLK_IO_OP_DISCARD || op == UBLK_IO_OP_WRITE_ZEROES) {
        uint32_t const len = (nr_vecs > 0) ? static_cast< uint32_t >(iovecs[0].iov_len) : 0;
        auto [discard_res, discard_state] = handle_discard(q, data, len, addr);
        res = discard_res;
        state = discard_state;
    } else {
        DLOGT("{} {} : [tag:{:#0x}] ublk io [addr:{:#0x}|len:{:#0x}]", op == UBLK_IO_OP_READ ? "READ" : "WRITE",
              _path.native(), data->tag, addr, iovec_len(iovecs, iovecs + nr_vecs))
        // LCOV_EXCL_START — kernel <= 5.4 sync fallback, not exercised in production
        if (!_direct_io && k_buffered_uring_broken) {
            auto r = sync_iov(op, iovecs, nr_vecs, static_cast< off_t >(addr));
            if (!r) co_return -static_cast< int >(r.error().value());
            if (r.value() != expected_len) {
                DLOGE("Short {} {} : [tag:{:#0x}] ublk io [addr:{:#0x}] completed {}B of {}B",
                      op == UBLK_IO_OP_READ ? "READ" : "WRITE", _path.native(), data->tag, addr, r.value(),
                      expected_len)
                if (auto* m = volume_metrics(q)) m->record_io_short(op);
                co_return -EIO;
            }
            co_return static_cast< int >(r.value()); // inline completion
        }
        // LCOV_EXCL_STOP

        auto sqe = next_sqe(q);
        if (!sqe) [[unlikely]]
            co_return -EBUSY;
        DEBUG_ASSERT_GE(capacity(), iovecs->iov_len + addr, "Access beyond device bounds!");

        if (UBLK_IO_OP_READ == op) {
            io_uring_prep_readv(sqe, _fd, iovecs, nr_vecs, addr);
        } else {
            io_uring_prep_writev(sqe, _fd, iovecs, nr_vecs, addr);
        }

        if (UBLK_IO_OP_READ != op && (data->iod->op_flags & UBLK_IO_F_FUA)) sqe->rw_flags |= RWF_DSYNC;
        auto [s, sqe_data] = build_cqe_state_data(data);
        state = s;
        sqe->user_data = sqe_data;
        if (_metrics) { // GCOVR_EXCL_BR_LINE -- UblkFSDiskMetrics requires prometheus registry; not constructible in
                        // unit tests
            // LCOV_EXCL_START
            _metrics->record_io_start(data);
            track_metrics = true;
        } // LCOV_EXCL_STOP
        res = 1;
    }

    if (!res) co_return -static_cast< int >(res.error().value());
    if (res.value() == 0) co_return 0;

    auto const cqe_result = co_await *state;
    if (track_metrics) { _metrics->record_io_complete(data); } // GCOVR_EXCL_BR_LINE
    // A positive-but-short READ/WRITE completion must not propagate: summed upstream (RAID0) and
    // handed to ublk_drv it becomes a front-aligned partial completion, silently misplacing the
    // unfilled range. Fail loudly instead; RAID1 treats the -EIO as a leg failure and fails over.
    if ((UBLK_IO_OP_READ == op || UBLK_IO_OP_WRITE == op) && cqe_result >= 0 &&
        static_cast< uint64_t >(cqe_result) != expected_len) {
        DLOGE("Short {} {} : [tag:{:#0x}] ublk io [addr:{:#0x}] completed {}B of {}B",
              op == UBLK_IO_OP_READ ? "READ" : "WRITE", _path.native(), data->tag, addr, cqe_result, expected_len)
        if (auto* m = volume_metrics(q)) m->record_io_short(op);
        co_return -EIO;
    }
    co_return cqe_result;
}

std::pair< io_result, cqe_state* > FSDisk::handle_discard(ublksrv_queue const* q, ublk_io_data const* data,
                                                          uint32_t len, uint64_t addr) {
    DLOGD("DISCARD {}: [tag:{:#0x}] ublk io [addr:{:#0x}|len:{:#0x}]", _path.native(), data->tag, addr, len)
    if (!_block_device) {
        auto sqe = next_sqe(q);
        if (!sqe) [[unlikely]]
            return {std::unexpected(std::make_error_condition(std::errc::device_or_resource_busy)), nullptr};
        io_uring_prep_fallocate(sqe, _fd, discard_to_fallocate(data->iod), addr, len);

        auto [state, sqe_data] = build_cqe_state_data(data);
        sqe->user_data = sqe_data;
        return {1, state};
    }

    uint64_t r[2]{addr, len};
    auto res = ioctl(_fd, BLKDISCARD, &r);
    if (0 == res) [[likely]]
        return {0, nullptr};
    DEBUG_ASSERT_LT(res, 0, "Positive ioctl")
    if (0 < res) {
        DLOGE("ioctl BLKDISCARD on {} returned postive result: {}", _path.native(), res)
        return {std::unexpected(std::make_error_condition(std::errc::io_error)), nullptr};
    }
    DLOGE("ioctl BLKDISCARD on {} returned error: {}", _path.native(), strerror(errno))
    return {std::unexpected(std::make_error_condition(static_cast< std::errc >(errno))), nullptr};
}

io_result FSDisk::sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) noexcept {
    if (0 > _fd) {
        DLOGE("Direct read on un-opened device!")
        return std::unexpected(std::make_error_condition(std::errc::io_error));
    }
    [[maybe_unused]] auto const len = iovec_len(iovecs, iovecs + nr_vecs);
    DLOGT("{} {} : [INTERNAL] ublk io [addr:{:#0x}|len:{:#0x}]", op == UBLK_IO_OP_READ ? "READ" : "WRITE",
          _path.native(), addr, len)
    DEBUG_ASSERT_GE(capacity(), len + addr, "Access beyond device bounds!");
    auto res = ssize_t{-1};
    switch (op) {
    case UBLK_IO_OP_READ: {
        res = preadv2(_fd, iovecs, nr_vecs, addr, RWF_HIPRI);
    } break;
    case UBLK_IO_OP_WRITE: {
        res = pwritev2(_fd, iovecs, nr_vecs, addr, RWF_DSYNC | RWF_HIPRI);
    } break;
    default:
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (0 > res) {
        DLOGE("{} {} : {}", op == UBLK_IO_OP_READ ? "preadv" : "pwritev", _path.native(), strerror(errno))
        return std::unexpected(std::make_error_condition(std::errc::io_error));
    }
    return res;
}

std::shared_ptr< ublk_disk > make_fs_disk(std::filesystem::path const& path, std::string const& parent_id) {
    return std::make_shared< FSDisk >(path, parent_id);
}

} // namespace ublkpp
