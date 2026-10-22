#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <boost/uuid/random_generator.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include "ublkpp/craft_disk.hpp"
#include "ublkpp/lib/ublk_disk.hpp"
#include "tests/mock_ublksrv/mock_ublksrv.hpp" // drive async_iov over a real io_uring, no kernel/ublk module

SISL_LOGGING_INIT(ublk_drivers)

SISL_OPTIONS_ENABLE(logging)

// A craft_disk built over an in-process reference cluster self-sizes from what login reports -- no kernel, no
// server, no /dev/ublkbN. This exercises the whole assembly path (make_local_cluster -> make_client -> login ->
// derive geometry) and asserts the disk's advertised geometry matches the volume it logged into.
TEST(CraftDisk, LocalAssemblesAndSelfSizes) {
    auto const vol = boost::uuids::random_generator()();
    constexpr uint64_t k_capacity = uint64_t{64} << 20; // 64 MiB
    constexpr uint32_t k_page = 4096;

    auto disk = ublkpp::make_craft_disk_local(vol, /*n=*/3, k_page, k_capacity);
    ASSERT_TRUE(disk);

    EXPECT_EQ(disk->block_size(), k_page);   // from lba_size(client)
    EXPECT_EQ(disk->capacity(), k_capacity); // from capacity(client); 64 MiB is a whole multiple of max_sectors
    EXPECT_FALSE(disk->id().empty());        // the volume uuid string
}

// A non-power-of-two page size can never come back from login here, but a caller passing a bad size to the
// builder should fail construction rather than produce a broken device.
TEST(CraftDisk, RejectsBadPageSize) {
    auto const vol = boost::uuids::random_generator()();
    EXPECT_THROW((void)ublkpp::make_craft_disk_local(vol, 3, /*page_size=*/4000, uint64_t{64} << 20),
                 std::runtime_error);
}

// Drive the on-ring CRAFT transport through the REAL ublk_disk path AT DEPTH. MockUblksrv owns an io_uring and
// exposes it as the queue's ring_ptr, which async_iov hands to each CRAFT verb; it starts async_iov for N tags
// before polling, so every leg's delivery SQE sits on that ring at once and is reaped by the mock's
// run_queue_loop-shaped poll (the _on_complete dispatch that tells craft's cqe_awaitable from ublkpp's own
// cqe_state). That depth -- many scattered IOs in flight together -- is exactly what QD=1 cannot produce and
// what CRAFT's correctness is emergent from. Round-trip every block to prove data integrity too.
TEST(CraftDisk, OnRingDepthWriteReadRoundTrip) {
    auto const vol = boost::uuids::random_generator()();
    constexpr uint32_t k_page = 4096;
    constexpr uint64_t k_capacity = uint64_t{64} << 20;
    constexpr int N = 8;                         // tags in flight at once == queue depth
    constexpr uint32_t k_sectors = k_page / 512; // 8 sectors per 4 KiB block

    auto disk = ublkpp::make_craft_disk_local(vol, /*n=*/3, k_page, k_capacity);
    ASSERT_TRUE(disk);

    // q_depth = N: submit_io starts async_iov (inline, to its first suspend) without driving the ring, so all N
    // tags are pending before the first poll.
    ublkpp::MockUblksrv mock(disk, /*q_depth=*/N, /*nr_queues=*/1);

    // ── writes: distinct pattern per block, all N submitted BEFORE polling (3*N replica legs on the ring) ──
    for (int i = 0; i < N; ++i) {
        auto* buf = static_cast< uint8_t* >(mock.io_buf(i));
        std::memset(buf, static_cast< int >(0x10 + i), k_page);
        auto const r = mock.submit_io(i, UBLK_IO_OP_WRITE, static_cast< uint64_t >(i) * k_sectors, k_sectors, buf);
        ASSERT_TRUE(r.has_value()) << "submit write tag " << i;
    }
    auto const wc = mock.poll(N, std::chrono::milliseconds{4000});
    ASSERT_EQ(wc.size(), static_cast< std::size_t >(N)) << "every write must complete over the ring";
    for (auto const& c : wc)
        EXPECT_GT(c.result, 0) << "write tag " << c.tag << " succeeded";

    // ── reads: zero each buffer first (a stale write pattern must not masquerade as a read hit), then verify ──
    for (int i = 0; i < N; ++i) {
        auto* buf = static_cast< uint8_t* >(mock.io_buf(i));
        std::memset(buf, 0, k_page);
        auto const r = mock.submit_io(i, UBLK_IO_OP_READ, static_cast< uint64_t >(i) * k_sectors, k_sectors, buf);
        ASSERT_TRUE(r.has_value()) << "submit read tag " << i;
    }
    auto const rc = mock.poll(N, std::chrono::milliseconds{4000});
    ASSERT_EQ(rc.size(), static_cast< std::size_t >(N)) << "every read must complete over the ring";
    for (int i = 0; i < N; ++i) {
        auto const* buf = static_cast< uint8_t const* >(mock.io_buf(i));
        std::vector< uint8_t > const want(k_page, static_cast< uint8_t >(0x10 + i));
        EXPECT_EQ(0, std::memcmp(buf, want.data(), k_page)) << "block " << i << " reads back its written bytes";
    }

    // Drain any still-detached legs (a read fires keep_alives at the replicas it did not serve) before the mock's
    // io_uring is exited, so no queued SQE outlives its coroutine frame. All delays are 0, so this returns at once.
    (void)mock.poll(1 << 20, std::chrono::milliseconds{100});
}

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging);
    sisl::logging::SetLogger(std::string(argv[0]));
    return RUN_ALL_TESTS();
}
