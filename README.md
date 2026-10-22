# ublkpp

[![Conan Build](https://github.com/ebay/ublkpp/actions/workflows/merge_build.yml/badge.svg?branch=main)](https://github.com/ebay/ublkpp/actions/workflows/merge_build.yml)
[![CodeCov](https://codecov.io/gh/ebay/ublkpp/graph/badge.svg?token=2N5W3458RK)](https://codecov.io/gh/ebay/ublkpp)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

> A high-performance C++23 library for Linux's userspace block (ublk) driver, featuring **CraftDisk** — a
> quorum-replicated block device — alongside classic RAID0/1/10.

## 🚀 Features

- **CraftDisk — quorum-replicated block device (flagship)**: A `/dev/ublkbN` backed by a CRAFT replica set.
  Writes broadcast to N replicas and commit at a **quorum** (never waiting for the slowest); reads route to an
  eligible replica and fail over around ones that are down or missing a range. Replicas can be in-process
  (reference) or **remote over TCP**. The successor to RAID1 — cross-host redundancy with quorum instead of two
  local devices — with a RAID1-cost **"skinny" mode** (two data replicas + a quorum arbiter) on the way.
- **RAID Support**: RAID0 (striping), RAID1 (mirroring), and RAID10 (stripe of mirrors)
- **RAID1 Resilient Bitmap**: Memory-efficient dirty tracking (4 KiB page tracks 1 GiB data)
- **Thin-Aware, Resumable Resync**: Per-scenario copy modes (blind / compare-skip / zero-detect) persisted in the superblock; a cleanly-stopped resync resumes where it left off
- **Hot Device Replacement**: Swap devices in degraded RAID1 arrays without downtime
- **Lock-Free I/O Path**: Read/write operations use lock-free algorithms (x86-64/ARM64)
- **Factory-Based API**: Replicated volumes, file-backed disks, and RAID compositions through supported factory functions
- **Coroutine I/O**: Single-event-loop, CQE-driven coroutine pipeline
- **Comprehensive Testing**: High test coverage with unit and functional (fio-driven) tests
- **Modern C++**: Built with C++23, leveraging `std::expected` for error handling

## 📋 Table of Contents

- [Quick Start](#-quick-start)
- [Architecture](#-architecture)
- [CraftDisk (Replicated Volume)](#-craftdisk-replicated-volume)
- [RAID Features](#-raid-features)
- [Example Application](#-example-application)
- [Development](#-development)
- [Testing](#-testing)
- [Dependencies](#-dependencies)
- [License](#-license)

## 🏃 Quick Start

### Prerequisites

- Linux kernel with ublk support (5.19+)
- Conan 2.0+
- CMake 3.22+
- C++23 compatible compiler (GCC 13+, Clang 17+)

### Build Library

```bash
git clone https://github.com/ebay/ublkpp
cd ublkpp
./prepare_v2.sh
conan build -s:h build_type=Debug --build missing .
```

### Build Options

```bash
# Release build
conan build -s:h build_type=Release --build missing .

# With coverage
conan build -s:h build_type=Debug -o ublkpp/*:coverage=True --build missing .

# With sanitizers (address or thread)
conan build -s:h build_type=Debug -o ublkpp/*:sanitize=address --build missing .
conan build -s:h build_type=Debug -o ublkpp/*:sanitize=thread --build missing .
```

## 🏗️ Architecture

### Project Structure

```
ublkpp/
├── include/ublkpp/       # Public headers
│   ├── craft_disk.hpp    # CraftDisk (replicated volume) factories
│   ├── drivers.hpp       # File-backed disk factory
│   ├── raid.hpp          # RAID factories and helpers
│   ├── target.hpp        # ublk target interface
│   └── lib/              # Base disk subclassing API
├── src/
│   ├── driver/           # File-backed + CraftDisk backend implementations
│   ├── lib/              # Core ublk_disk base classes
│   ├── metrics/          # I/O and RAID metrics
│   ├── raid/             # RAID logic (bitmap, superblock)
│   └── target/           # ublkpp_tgt
└── example/              # Sample applications
```

### Core Abstractions

- **`ublk_disk`**: Base class for all block devices
- **`disk_handle`**: Shared ownership handle for disks and RAID composites
- **`make_craft_disk_local()` / `make_craft_disk_tcp()`**: CraftDisk (replicated volume) factories — in-process reference or remote-over-TCP replicas
- **`make_fs_disk()`**: File/block-backed disk construction
- **`make_raid0_disk()` / `make_raid1_disk()`**: RAID composition factories
- **`raid0::*` / `raid1::*`**: Free-function helpers for topology and mirror management
- **`ublkpp_tgt`**: Exposes devices to kernel via ublk

## 💠 CraftDisk (Replicated Volume)

`CraftDisk` exposes a **CRAFT replica set** as a single `/dev/ublkbN`. Where RAID1 mirrors to two *local* block
devices, CraftDisk **replicates across N replicas that may be remote** — so redundancy spans hosts, not just
devices. It is the intended **successor to RAID1**.

**Model:**
- **Quorum writes.** A write broadcasts to every replica at a client-assigned data-LSN and returns as soon as a
  majority acks — never waiting for the slowest replica; a straggler keeps running and still lands the write.
  No write flows through a consensus log.
- **Routed reads.** Reads are unicast to one eligible replica at a read horizon and fail over on a miss/down —
  the N-way, cross-host analogue of RAID1's degraded-mode read routing.
- **Pluggable transport.** Replicas are an in-process reference cluster (`make_craft_disk_local`, no servers, no
  wire) or remote servers over io_uring TCP (`make_craft_disk_tcp`). The driver is identical either way.
- **Self-configuring.** The device sizes itself (capacity + block size) from what the replica set reports at
  login — no out-of-band geometry.
- **Multi-queue (blk-mq).** Runs at any `--nr_hw_queues`: each ublk queue thread drives CRAFT on **its own**
  io_uring, so a reply is reaped by the thread that issued it and completions never cross threads. The client
  opens one connection per (queue ring, replica) — an `nr_hw_queues × N` grid — while the replica set itself
  stays shared.

**Skinny mode — in progress.** For a RAID1-cost deployment, CraftDisk runs **two data-replicating backends plus
a lightweight arbiter**: the two backends hold the data (a 2-copy footprint, same as a RAID1 mirror), while the
arbiter stores no data but votes in the configuration / LSN quorum. That preserves a true majority (2 of 3
members) — so failover and fencing stay split-brain-safe — without paying for a third full data copy. This is
what makes CraftDisk a drop-in RAID1 replacement: the same storage cost, with real quorum behind it.

> Backed by the standalone [`craft_client`](https://github.com/szmyd/craft_client) package (the CRAFT wire
> protocol, client, and reference model). See `include/ublkpp/craft_disk.hpp`.

## 💾 RAID Features

### RAID0 (Striping)

- Configurable stripe size (default: 128 KiB)
- Distributes data across devices for performance
- Linear capacity aggregation

### RAID1 (Mirroring)

> **Being superseded by [CraftDisk](#-craftdisk-replicated-volume)** — a quorum-replicated, cross-host volume;
> its "skinny mode" (two data replicas + an arbiter) matches RAID1's storage cost with real quorum behind it.

**Key Features:**
- Two-way mirroring with dirty bitmap tracking
- Degraded mode operation (single device failure)
- Hot device replacement via `swap_device()`
- Read routing round-robins

**Bitmap Efficiency:**
- 4 KiB pages track 32 KiB chunks (default)
- Memory footprint: ~0.4% of capacity (e.g., 8 MiB for 2 TB)
- SuperBitmap optimization for fast initialization

**Resync Features:**
- Background resync with per-region I/O coordination
- Lock-free write tracking: resync yields only for chunks that conflict with an in-flight write
- Two-phase conflict check with shadow completion log to close the mid-copy race window
- Copy mode selected by recovery scenario, deciding per 4 KiB page:
  - **BLIND**: full copy -- known-divergent dirty sets (a degraded leg's outage writes) and unverified fresh legs
  - **CHECK**: read + `memcmp` the destination, rewrite only divergent pages -- power-loss self-heal, re-added legs
  - **ZERO_TEST**: zero-detect the source, skip unallocated regions with no destination read -- fresh-leg rebuilds on thin devices (requires `assume_clean`)
- Mode persists in the superblock: a cleanly-stopped resync resumes where it left off; an unclean stop falls back to a full CHECK pass
- New arrays run an md-style initial sync unless constructed with `assume_clean` (see below)
- Configurable delay intervals

**`assume_clean` (per-device opt-in on `make_raid1_disk()` / `swap_device()`, `--assume_clean` on the example):**
asserts a genuinely-fresh leg (no superblock) reads back zero for never-written blocks, e.g. a
newly-provisioned thin volume or sparse file. Enables ZERO_TEST rebuilds and skips the new-array
initial sync (both legs already read identically), preserving thin provisioning. Leave unset for
recycled/raw disks -- the initial sync then makes the mirrors read deterministically.

### RAID10 (Stripe of Mirrors)

- RAID0 striping across RAID1 pairs
- Combines performance and redundancy
- Requires even number of devices (min: 4)

## 🖥️ Example Application

The `ublkpp_disk` application demonstrates all RAID capabilities with a single target.

### Build and Run

```bash
# Build release version
conan build -s:h build_type=Release --build missing .

# Load kernel module
sudo modprobe ublk_drv

# Create backing files
fallocate -l 2G file1.dat
fallocate -l 2G file2.dat
fallocate -l 2G file3.dat
fallocate -l 2G file4.dat

# Launch RAID10 device (sparse files read zero: --assume_clean skips the new-array initial sync)
sudo ublkpp/build/Release/example/ublkpp_disk --raid10 file1.dat,file2.dat,file3.dat,file4.dat --assume_clean
```

### Usage Examples

```bash
# CraftDisk: a self-contained 1 GiB replicated volume over an in-process reference cluster (no servers, no wire)
sudo ublkpp_disk --craft 1024
# (raw block I/O works today; DISCARD/WRITE_ZEROES return ENOTSUP, so hold off on mkfs until that lands)

# Single device (loop mode)
sudo ublkpp_disk --loop /dev/sdb

# RAID0 (striping)
sudo ublkpp_disk --raid0 /dev/sdc,/dev/sdd --stripe_size 262144

# RAID1 (mirroring; a brand-new array runs an initial sync to make the mirrors identical)
sudo ublkpp_disk --raid1 /dev/sde,/dev/sdf

# RAID10 (4+ devices; sparse/thin backing reads zero, so skip the initial sync)
sudo ublkpp_disk --raid10 file1.dat,file2.dat,file3.dat,file4.dat --assume_clean

# Recover existing device
sudo ublkpp_disk --device_id 0 --raid1 /dev/sde,/dev/sdf
```

### Verify Device

```bash
$ lsblk
NAME        MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS
...
ublkb0      259:3    0    4G  0 disk

# Make Filesystem
$ sudo mkfs.xfs /dev/ublkb0
$ sudo mount /dev/ublkb0 /mnt
```

## 🛠️ Development

### Code Style

- **Indentation**: 4 spaces
- **Line Length**: 120 characters
- **Pointers**: Left alignment (`Type* ptr`)
- **Standard**: C++23
- **Headers**: `#pragma once`

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| **Public API types** (`include/ublkpp/`) | `lower_snake_case` | `ublk_disk`, `disk_handle`, `ublkpp_tgt` |
| **Public API factories** (free functions) | `make_<thing>` | `make_fs_disk()`, `make_raid1_disk()` |
| **Internal classes** (`src/`) | `PascalCase` | `SuperBlock`, `Bitmap`, `Raid1Disk` (impl), `MirrorDevice` |
| Functions / methods | `snake_case` | `async_iov()`, `prepare()`, `swap_device()` |
| Members | `_snake_case` | `_device`, `_dirty_bitmap` |
| Constants | `k_snake_case` | `k_page_size` |
| Macros / Enums | `SCREAMING_SNAKE_CASE` | `UBLK_IO_OP_WRITE` |

Driver and RAID array implementations are not part of the public surface; consumers construct
opaque `disk_handle`s via `make_*_disk()` factories and compose them.

### Workflow

```bash
# 1. Write code
# 2. Write tests (see Testing section)
# 3. Format code
./apply-clang-format.sh

# 4. Build and test
conan build -s:h build_type=Debug --build missing .
```

### Error Handling

Uses `std::expected<T, std::error_condition>` pattern:

```cpp
using io_result = std::expected<size_t, std::error_condition>;

io_result write_data(uint64_t addr, uint32_t len) {
    if (auto res = device->sync_iov(UBLK_IO_OP_WRITE, iov, 1, addr); !res) {
        DLOGE("Write failed at {:#x}: {}", addr, res.error().message());
        return res;
    }
    return len;
}
```

## 🧪 Testing

### Test Organization

```
src/<component>/tests/
├── test_*_common.hpp      # Shared test utilities
├── simple/                # Basic functionality tests
├── failures/              # Error handling tests
├── bitmap/                # RAID1 bitmap tests
└── superblock/            # Superblock I/O tests
```

### Running Tests

```bash
# Tests run automatically during build
conan build -s:h build_type=Debug --build missing .

# Coverage report
conan build -s:h build_type=Debug -o ublkpp/*:coverage=True --build missing .
# View: build/Debug/coverage_html/index.html

# Thread sanitizer
conan build -s:h build_type=Debug -o ublkpp/*:sanitize=thread --build missing .

# Address sanitizer
conan build -s:h build_type=Debug -o ublkpp/*:sanitize=address --build missing .
```

### Writing Tests

Framework: Google Test (GTest) with GMock

```cpp
#include "test_raid1_common.hpp"

TEST(Raid1, YourTestName) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = 2 * Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = 2 * Gi});

    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);

    auto raid = ublkpp::make_raid1_disk(uuid, device_a, device_b);

    // Test logic...
    EXPECT_EQ(expected, actual);
}
```

## 📦 Dependencies

### Core Dependencies

- **[craft_client](https://github.com/szmyd/craft_client)**: CRAFT wire protocol, client, and reference model — the CraftDisk backend
- **[sisl](https://github.com/eBay/sisl)** v14+: Logging, options, metrics, HTTP server
- **[ublksrv](https://github.com/ublk-org/ublksrv)**: ublk driver interface
- **isa-l**: RAID acceleration primitives
- **boost**: UUID generation
- **liburing**: io_uring support

### Optional Dependencies

- **[stdexec](https://github.com/NVIDIA/stdexec)**: C++ sender/receiver framework — provided transitively via the sisl conan package
- **fio**: Functional I/O testing (optional; tests skip gracefully if absent)

### Build Tools

- Conan 2.0+
- CMake 3.22+
- clang-format (code formatting)
- gcovr (coverage reporting)

## 📚 Documentation

### Development & Contributing

- **[CHANGELOG.md](CHANGELOG.md)**: Version history and release notes
- **[CLAUDE.md](.claude/CLAUDE.md)**: Development guidelines and workflows
- **[docs/error_codes.md](docs/error_codes.md)**: RAID async_iov error code reference (EIO vs EAGAIN matrix)
- **[docs/functional_testing.md](docs/functional_testing.md)**: Functional test procedures
- **[Linux ublk Documentation](https://docs.kernel.org/block/ublk.html)**: Kernel driver details

## 🤝 Contributing

Contributions are welcome! Please:

1. Follow the code style (run `./apply-clang-format.sh`)
2. Add tests for new functionality
3. Update CHANGELOG.md and version in conanfile.py
4. Ensure all tests pass with sanitizers
5. Submit pull requests against `main`

## 📄 License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.

**Primary Author**: [Brian Szmyd](https://github.com/szmyd)

---

**Links:**
- 🐛 [Report Issues](https://github.com/ebay/ublkpp/issues)
- 💬 [Discussions](https://github.com/ebay/ublkpp/discussions)
- 📖 [ublksrv GitHub](https://github.com/ublk-org/ublksrv)
