#include "raid/raid1/bitmap.hpp"
#include "test_raid1_common.hpp"

using namespace std::chrono_literals;

// Test 1: Try to swap a device that's not part of the array
TEST(Raid1, SwapUnrecognizedDevice) {
    auto device_a = CREATE_DISK_A((TestParams{.capacity = Gi}));
    auto device_b = CREATE_DISK_B((TestParams{.capacity = Gi}));
    // device_c is not initialized into a RAID, so don't use CREATE_DISK macro
    auto device_c = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "device_c"});

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Try to swap a device that's not in the array (should fail and return incoming device)
    // This covers line 330: refusing to replace unrecognized mirror
    auto result = raid_device.swap_device("unknown_device_id", device_c);

    // Should return the incoming device unchanged (swap refused)
    EXPECT_EQ(result->id(), device_c->id());

    // Cleanup expectations
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test 2: Try to swap a device with itself (already in array)
TEST(Raid1, SwapDeviceAlreadyInArray) {
    auto device_a = CREATE_DISK_A((TestParams{.capacity = Gi}));
    auto device_b = CREATE_DISK_B((TestParams{.capacity = Gi}));

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Try to swap device_a with itself (should fail and return incoming device)
    // This covers line 333: device already in array, nothing to do
    auto result = raid_device.swap_device(device_a->id(), device_a);

    // Should return device_a unchanged (swap refused)
    EXPECT_EQ(result->id(), device_a->id());

    // Cleanup expectations
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test 3: Both devices assigned same slot (initialization error)
TEST(Raid1, BothDevicesSameSlot) {
    // Create devices without setting up any expectations
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});

    // Set up superblocks where both devices think they're device_b
    // This should trigger the exception at line 169
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            if (nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                // Set device_b = 1 (this is device B)
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::iovec_len(iovecs, iovecs + 1);
        });

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            if (nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                // ALSO set device_b = 1 (this is ALSO device B - invalid!)
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::iovec_len(iovecs, iovecs + 1);
        });

    // Should throw runtime_error: "Found both devices were assigned the same slot!"
    EXPECT_THROW(ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b),
                 std::runtime_error);
}

// Test 4: Unclean shutdown while degraded — both devices valid, route persisted, clean_unmount=0.
// Exercises the (read_route != EITHER && clean_unmount == 0) branch: full bitmap dirty + age bump.
TEST(Raid1, UncleanShutdownWhileDegraded) {
    // device_a wins pick_superblock (higher age); its read_route=DEVA is used.
    // Age diff of 1 keeps device_b marked valid (not new_device).
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::DEVA);
            sb->fields.device_b = 0;
            sb->fields.clean_unmount = 0;
            sb->fields.bitmap.age = htobe64(2);
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 1;
            sb->fields.clean_unmount = 0;
            sb->fields.bitmap.age = htobe64(1);
            return ublkpp::raid1::k_page_size;
        });

    // device_a: SB from __become_active, bitmap page from sync_to at shutdown, SB from destructor
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // __become_active: SB written with age bumped +16, route=DEVA, clean_unmount=0
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            EXPECT_EQ(htobe64(18), sb->fields.bitmap.age); // age 2 + 16 bump
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // sync_to at shutdown: bitmap page written to active device
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_GE(addr, ublkpp::raid1::k_page_size); // bitmap area, not SB
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // destructor: SB written with clean_unmount=1
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(1, sb->fields.clean_unmount);
            return ublkpp::raid1::k_page_size;
        });
    // device_b: only SB from __become_active (backup device not written at shutdown when degraded)
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            return ublkpp::raid1::k_page_size;
        });

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
}

// Test 5: Unclean shutdown with both legs present and equal ages — the self-heal path.
// Both legs have route=EITHER, clean_unmount=0, same age.  The self-heal branch must:
//   • bump the canonical (device_a) age by +16 and pin reads to it via route=DEVA
//   • dirty the whole bitmap so a full resync runs
//   • mark device_b stale (unavail) so writes skip it until probe_mirror clears the flag
//   • skip writing device_b's SB in __become_active (preserving the on-disk age gap)
//
// Writes observed:
//   device_a — 3:  __become_active SB (age+16, route=DEVA)
//                   sync_to bitmap page (shutdown, degraded path)
//                   destructor SB (clean_unmount=1)
//   device_b — 0:  __become_active skips (unavail guard); destructor skips (degraded backup)
TEST(Raid1, DISABLED_UncleanShutdownBothPresentSelfHeal) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.clean_unmount = 0;
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 1;
            sb->fields.clean_unmount = 0;
            return ublkpp::raid1::k_page_size;
        });

    // device_a: __become_active SB + bitmap sync_to + destructor SB  (3 writes)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // __become_active: age bumped +16, route=DEVA, clean_unmount=0
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            EXPECT_EQ(htobe64(16), sb->fields.bitmap.age); // age 0 (normal_superblock) + 16
            EXPECT_EQ(0, sb->fields.clean_unmount);
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // sync_to: bitmap page (shutdown degraded path), not at SB offset
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_GE(addr, static_cast< off_t >(ublkpp::raid1::k_page_size));
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // destructor SB: clean_unmount=1, route still DEVA (array remains degraded at shutdown)
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(1, sb->fields.clean_unmount);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            return ublkpp::raid1::k_page_size;
        });
    // device_b: __become_active now writes the backup SB too (ages equalize; the freeze is gone).
    // The destructor still skips it (degraded backup). The backup carries device_b=1, route=DEVA,
    // age 16, clean_unmount=0, and the persisted resync_mode=CHECK so an interrupted self-heal
    // resumes as CHECK via the degraded-return branch.
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(1, sb->fields.device_b);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            EXPECT_EQ(htobe64(16), sb->fields.bitmap.age);
            EXPECT_EQ(0, sb->fields.clean_unmount);
            EXPECT_EQ(static_cast< uint8_t >(ublkpp::raid1::resync_copy_mode::CHECK), sb->fields.bitmap.resync_mode);
            return ublkpp::raid1::k_page_size;
        });

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Reads must be pinned to canonical (device_a): route=DEVA, device_b marked stale (ERROR).
    auto const state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, state.device_b);
    EXPECT_GT(state.bytes_to_sync, 0ULL);

    // Verify read routing: a sync read must dispatch to device_a only.
    // route=DEVA + device_b->unavail means __select_read_devices never returns device_b.
    // Run in a fresh thread to avoid contaminating the thread_local last_read state shared
    // across tests (same pattern used by other tests that exercise __select_read_devices).
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _)).Times(0);
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result {
            memset(iov->iov_base, 0, iov->iov_len);
            return static_cast< int >(iov->iov_len);
        });
    RUN_IN_THREAD({
        alignas(4096) std::array< char, ublkpp::raid1::k_page_size > buf{};
        auto iov = iovec{.iov_base = buf.data(), .iov_len = buf.size()};
        EXPECT_TRUE(raid_device.sync_iov(UBLK_IO_OP_READ, &iov, 1, 0).has_value());
    });
}

// Test 5b: Crash-mid-resync idempotency — reassembly after a self-heal crash uses the
// existing new_device path, not the both-present-unclean branch.
//
// Simulates on-disk state after __become_active on the first self-heal assembly:
//   device_a: age=16, route=DEVA, clean_unmount=0   (canonical, bumped by self-heal)
//   device_b: age=0,  route=EITHER, clean_unmount=0  (stale, SB write was skipped)
//
// pick_superblock picks device_a (higher age).  age_diff = 16 > 1 → _device_b->new_device=true.
// __init_bitmap_and_degraded_route enters the existing one-new-device branch (not the
// both-present-unclean branch), bumps age another +16, and sets route=DEVA.
//
// Writes:
//   device_a — 3: __become_active SB (age=32, route=DEVA) + bitmap sync_to + destructor SB
//   device_b — 2: init_to bitmap pages (new_device path) + __become_active SB (age=32, route=DEVA)
//                 new_device path does NOT set unavail, so __become_active writes backup SB normally
TEST(Raid1, UncleanBothPresentSelfHealIdempotentAfterCrash) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    // device_a: canonical SB from first self-heal assembly (age=16, route=DEVA)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 0;
            sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::DEVA);
            sb->fields.clean_unmount = 0;
            sb->fields.bitmap.age = htobe64(16);
            return ublkpp::raid1::k_page_size;
        });
    // device_b: a leg from an earlier epoch (age=0 vs device_a's 16). Post freeze-drop, __become_active
    // writes BOTH legs' SBs to equal ages, so a crashed self-heal no longer leaves a stale age here;
    // this models a long-detached leg reattaching. The >1 age gap promotes it to new_device -> full
    // rebuild (the "idempotent" property: a re-run self-heal still rebuilds it).
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 1;
            sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::EITHER);
            sb->fields.clean_unmount = 0;
            sb->fields.bitmap.age = htobe64(0);
            return ublkpp::raid1::k_page_size;
        });

    // device_a: __become_active SB (age=32, route=DEVA) + bitmap sync_to + destructor SB
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t addr) -> io_result {
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            EXPECT_EQ(htobe64(32), sb->fields.bitmap.age); // 16 + 16 bump from new_device path
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t addr) -> io_result {
            EXPECT_GE(addr, static_cast< off_t >(ublkpp::raid1::k_page_size));
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t addr) -> io_result {
            EXPECT_EQ(0UL, addr);
            EXPECT_EQ(1, reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.clean_unmount);
            return ublkpp::raid1::k_page_size;
        });
    // device_b: init_to bitmap pages (new_device path), then __become_active SB
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t addr) -> io_result {
            // init_to: first (and only, for 1 GiB) bitmap page at sizeof(SuperBlock) == k_page_size
            EXPECT_EQ(addr, static_cast< off_t >(ublkpp::raid1::k_page_size));
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t addr) -> io_result {
            // __become_active SB: no unavail guard in new_device path, backup gets SB
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(ublkpp::raid1::read_route::DEVA, static_cast< ublkpp::raid1::read_route >(sb->fields.read_route));
            EXPECT_EQ(htobe64(32), sb->fields.bitmap.age);
            return ublkpp::raid1::k_page_size;
        });

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Should still be degraded (route=DEVA), resync pending
    auto const state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::SYNCING, state.device_b); // unavail NOT set by new_device path
    EXPECT_GT(state.bytes_to_sync, 0ULL);
}

// Test 6: Unclean shutdown while degraded (original broken test kept for documentation)
TEST(Raid1, UncleanShutdownDegraded) {
    // Create devices without setting up any expectations
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});

    // Set up device_a as clean, device_b as degraded, with unclean shutdown
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            if (nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                auto* sb = static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
                sb->fields.device_b = 0; // device_a
                sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::DEVA);
                sb->fields.clean_unmount = 0; // UNCLEAN shutdown!
            }
            return ublkpp::iovec_len(iovecs, iovecs + 1);
        });

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            // Device B is new/missing
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    // This should trigger lines 222-224: unclean shutdown in degraded mode
    // Should dirty the entire bitmap and bump age
    // Note: Constructor will fail because device_b cannot be read, so we expect a throw
    EXPECT_THROW(ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b),
                 std::runtime_error);
}

// L4: Device too small to hold the reserved region — must throw with a clear message.
// A 64 MiB device is well below the minimum reserved region for any supported configuration.
TEST(Raid1, DeviceTooSmallThrows) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 64 * Mi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 64 * Mi, .is_slot_b = true});

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            if (iovecs->iov_base) memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            if (iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::raid1::k_page_size;
        });

    EXPECT_THROW(
        {
            try {
                ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
            } catch (std::runtime_error const& e) {
                EXPECT_NE(std::string::npos, std::string(e.what()).find("device too small"))
                    << "Expected 'device too small' in exception message, got: " << e.what();
                throw;
            }
        },
        std::runtime_error);
}

// Test 7: An all-zero superbitmap on a previously-degraded array must be caught at the call site.
// superbitmap_nonempty() is the guard used in raid1.cpp before calling load_from.
TEST(Raid1, SuperbitmapNonemptyReturnsFalseOnCleanBitmap) {
    ublkpp::raid1::SuperBlock sb{};
    // sb.superbitmap_reserved is zero-initialized — no pages marked dirty.
    ublkpp::raid1::Bitmap bitmap(Gi, 32 * Ki, 4096, sb.superbitmap_reserved);
    EXPECT_FALSE(bitmap.superbitmap_nonempty());
}

TEST(Raid1, SuperbitmapNonemptyReturnsTrueAfterDirtyRegion) {
    ublkpp::raid1::SuperBlock sb{};
    ublkpp::raid1::Bitmap bitmap(Gi, 32 * Ki, 4096, sb.superbitmap_reserved);
    bitmap.dirty_region(0, 32 * Ki);
    EXPECT_TRUE(bitmap.superbitmap_nonempty());
}

// Test 8: Clean degraded startup — route=DEVA, clean_unmount=1, superbitmap has dirty pages.
// Exercises the load_from(*active_dev) call site and verifies the destructor persists
// the superbitmap (include_superbitmap=true) so the invariant holds on next startup.
TEST(Raid1, CleanDegradedStartupLoadsFromActiveDevice) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    // device_a: SB read, then bitmap page-0 read (load_from follows SB read)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(2)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::DEVA);
            sb->fields.clean_unmount = 1;
            sb->fields.bitmap.age = htobe64(10);
            sb->superbitmap_reserved[0] = 0x01; // bit 0 set → page 0 is dirty
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // Bitmap page 0 read at offset k_page_size
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(static_cast< off_t >(ublkpp::raid1::k_page_size), addr);
            memset(iovecs->iov_base, 0xFF, ublkpp::raid1::k_page_size); // non-zero → dirty page
            return ublkpp::raid1::k_page_size;
        });

    // device_b: SB read only (age=9, one behind device_a — not new_device)
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 1;
            sb->fields.bitmap.age = htobe64(9);
            return ublkpp::raid1::k_page_size;
        });

    // device_a: __become_active SB write + destructor SB write (include_superbitmap=true)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            return ublkpp::raid1::k_page_size;
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            // Destructor: clean_unmount=1 and superbitmap persisted (non-zero)
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            EXPECT_EQ(1, sb->fields.clean_unmount);
            EXPECT_NE(0, sb->superbitmap_reserved[0]) << "superbitmap must be persisted on shutdown";
            return ublkpp::raid1::k_page_size;
        });

    // device_b: __become_active SB write only — degraded, no destructor write to backup
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            return ublkpp::raid1::k_page_size;
        });

    auto raid_device = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
}

// Test 9: Clean degraded startup with an all-zero superbitmap.
// Covers the superbitmap_nonempty() guard in the clean-degraded branch (Branch 4) of
// __init_bitmap_and_degraded_route. Before Fix 2 this threw; after Fix 2 the constructor
// warns and continues. load_from skips all pages (superbitmap empty) so bytes_to_sync=0;
// route stays DEVA until resync calls complete() on its first pass.
TEST(Raid1, CleanDegradedStartupEmptySuperbitmap) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, 0UL))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::DEVA);
            sb->fields.clean_unmount = 1;
            sb->fields.bitmap.age = htobe64(10);
            // superbitmap_reserved stays zero — simulates the race-produced on-disk state
            return ublkpp::raid1::k_page_size;
        });
    // device_a gets 2 writes: __become_active (DEVA) then destructor clean_unmount (DEVA).
    // No bitmap read — superbitmap is empty so load_from skips all pages.
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, 0UL))
        .Times(2)
        .WillRepeatedly([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result { return iov->iov_len; });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, 0UL))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            EXPECT_EQ(ublkpp::raid1::k_page_size, ublkpp::iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(0UL, addr);
            memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            auto* sb = reinterpret_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base);
            sb->fields.device_b = 1;
            sb->fields.bitmap.age = htobe64(9);
            return ublkpp::raid1::k_page_size;
        });
    // device_b gets 1 write: __become_active (DEVA). Degraded destructor skips the backup device.
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, 0UL))
        .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result { return iov->iov_len; });

    EXPECT_NO_THROW({
        auto raid = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
        raid.toggle_resync(false);
        // Fix 2 post-conditions: route=DEVA (unchanged), empty bitmap (superbitmap empty; load_from skips).
        auto const s = raid.replica_states();
        EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, s.device_a);   // active leg
        EXPECT_EQ(ublkpp::raid1::replica_state::SYNCING, s.device_b); // backup leg, route not yet EITHER
        EXPECT_EQ(0ULL, s.bytes_to_sync);                             // superbitmap empty; nothing to sync
    });
}

// Verifies that resync_level=0 is rejected at construction time.
// Only meaningful when the binary is invoked with --resync_level=0; skipped otherwise so
// the regular Raid1Test CTest entry (resync_level=4) is not affected.
// See CMakeLists.txt: Raid1ZeroResyncLevelThrows target.
TEST(Raid1, ZeroResyncLevelThrows) {
    if (SISL_OPTIONS["resync_level"].as< uint32_t >() != 0) GTEST_SKIP();
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    EXPECT_THROW(ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// Recovery-mode resume (Part A): a reassembled degraded array selects its resync mode from the
// persisted superblock byte -- resume as-is on a clean stop, force CHECK on an unclean crash.
// ---------------------------------------------------------------------------------------------

// One leg of a degraded (route=DEVA) array: its SB read returns route DEVA with the given
// clean_unmount / slot / age / persisted resync_mode. All writes (SB rewrites + destructor flush)
// are accepted.
static std::shared_ptr< ublkpp::TestDisk >
make_degraded_leg(bool slot_b, uint8_t clean_unmount, uint64_t age, ublkpp::raid1::resync_copy_mode mode,
                  ublkpp::raid1::read_route route = ublkpp::raid1::read_route::DEVA,
                  std::shared_ptr< ublkpp::raid1::SuperBlock > captured = nullptr) {
    auto d = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = slot_b});
    EXPECT_CALL(*d, sync_iov(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly([slot_b, clean_unmount, age, mode, route, captured](uint8_t op, iovec* iov, uint32_t,
                                                                            off_t addr) -> io_result {
            if (op == UBLK_IO_OP_READ && 0 == addr && iov->iov_base) {
                memcpy(iov->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                auto* sb = static_cast< ublkpp::raid1::SuperBlock* >(iov->iov_base);
                sb->fields.read_route = static_cast< uint8_t >(route);
                sb->fields.clean_unmount = clean_unmount;
                sb->fields.device_b = slot_b ? 1 : 0;
                sb->fields.bitmap.age = htobe64(age);
                sb->fields.bitmap.resync_mode = static_cast< uint8_t >(mode);
            } else if (op == UBLK_IO_OP_WRITE && 0 == addr && iov->iov_base && captured) {
                memcpy(captured.get(), iov->iov_base, sizeof(ublkpp::raid1::SuperBlock));
            }
            return static_cast< int >(iov->iov_len);
        });
    return d;
}

// Assemble a degraded array (Side-A canonical, equal ages) and return the selected resync mode. A
// clean stop leaves the canonical clean_unmount=1 and the backup 0 (destructor writes only the
// active leg); an unclean crash leaves both 0.
static ublkpp::raid1::resync_copy_mode recovered_mode(bool clean, ublkpp::raid1::resync_copy_mode persisted) {
    auto a = make_degraded_leg(false, clean ? 1 : 0, 5, persisted);
    auto b = make_degraded_leg(true, 0, 5, persisted);
    ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), a, b);
    raid.toggle_resync(false);
    return raid.current_resync_mode();
}

// Clean degraded stop -> resume the persisted mode as-is (branch 4).
TEST(Raid1RecoveryMode, CleanDegradedResumesPersistedMode) {
    using RM = ublkpp::raid1::resync_copy_mode;
    EXPECT_EQ(RM::BLIND, recovered_mode(true, RM::BLIND));         // known-divergent degradation stays BLIND (cheaper)
    EXPECT_EQ(RM::CHECK, recovered_mode(true, RM::CHECK));         // power-loss self-heal stays CHECK
    EXPECT_EQ(RM::ZERO_TEST, recovered_mode(true, RM::ZERO_TEST)); // interrupted fresh-leg thin rebuild continues
}

// Unclean crash of a degraded array -> dirty-all the whole (mostly-matching) array in CHECK
// whatever was persisted; ZERO_TEST also cannot safely cross a crash (branch 3).
TEST(Raid1RecoveryMode, UncleanDegradedForcesCheck) {
    using RM = ublkpp::raid1::resync_copy_mode;
    EXPECT_EQ(RM::CHECK, recovered_mode(false, RM::BLIND));
    EXPECT_EQ(RM::CHECK, recovered_mode(false, RM::CHECK));
    EXPECT_EQ(RM::CHECK, recovered_mode(false, RM::ZERO_TEST));
}

// A legacy superblock has the resync_mode byte zeroed (== BLIND): a clean degraded reassembly
// reproduces today's BLIND behavior.
TEST(Raid1RecoveryMode, LegacySbResumesBlind) {
    using RM = ublkpp::raid1::resync_copy_mode;
    EXPECT_EQ(RM::BLIND, recovered_mode(true, RM::BLIND)); // BLIND == 0 == legacy zeroed byte
}

// A live re-dirty during a ZERO_TEST rebuild must taint the persisted/live mode to CHECK (review
// finding 1): once the target leg has taken a write there, it may hold non-zero data where the
// source now reads zero, and a ZERO_TEST re-copy would zero-skip it and leave the mirrors divergent.
// A write to a CLEAN region replicates to both legs; the target's data write fails, routing through
// Site 3, which untaints ZERO_TEST -> CHECK before dirtying the region.
TEST(Raid1RecoveryMode, LiveReDirtyUntaintsZeroTest) {
    using RM = ublkpp::raid1::resync_copy_mode;
    // Clean degraded array (route DEVA, empty superbitmap) resuming a persisted ZERO_TEST rebuild.
    auto a = make_degraded_leg(false, 1, 5, RM::ZERO_TEST); // canonical source, clean
    auto b = make_degraded_leg(true, 0, 5, RM::ZERO_TEST);  // rebuild target (backup)
    ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), a, b);
    raid.toggle_resync(false); // freeze the resync; we drive the write and inspect the mode directly

    ASSERT_EQ(RM::ZERO_TEST, raid.current_resync_mode());
    ASSERT_EQ(0u, raid.replica_states().bytes_to_sync) << "empty superbitmap -> every region clean";

    // Fail only the target leg's DATA write (offset >= reserved_size); its SB writes still succeed.
    EXPECT_CALL(*b, sync_iov(UBLK_IO_OP_WRITE, _, _, ::testing::Ge(static_cast< off_t >(raid.reserved_size()))))
        .Times(::testing::AnyNumber())
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    // Write to a clean region: it replicates to both legs; the target write fails -> Site 3.
    iovec iov{nullptr, 4 * Ki};
    auto const res = raid.sync_iov(UBLK_IO_OP_WRITE, &iov, 1, 0);
    ASSERT_TRUE(res) << "the source (active) write succeeds, so the op is acked";

    EXPECT_EQ(RM::CHECK, raid.current_resync_mode()) << "Site-3 re-dirty must taint ZERO_TEST -> CHECK";
    EXPECT_GT(raid.replica_states().bytes_to_sync, 0u) << "the failed region must be re-dirtied";
}

// ---------------------------------------------------------------------------------------------
// Missing-leg epoch ordering: constructing with a missing leg from a previously-healthy (EITHER)
// superblock must advance the age, or the absent leg ties arbitration when reattached and the
// array opens healthy over stale data. Clean pull -> +1 (reattach resumes); healthy-crash ->
// +k_age_bump (reattach fully rebuilds); already-degraded restart -> no re-bump.
// ---------------------------------------------------------------------------------------------

// Live leg (slot B) with a crafted SB; every SB write is captured for inspection.
static uint64_t missing_start_age(ublkpp::raid1::read_route route, uint8_t clean_unmount, uint64_t age) {
    auto captured = std::make_shared< ublkpp::raid1::SuperBlock >();
    auto leg = make_degraded_leg(true, clean_unmount, age, ublkpp::raid1::resync_copy_mode::BLIND, route, captured);
    { ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), ublkpp::make_missing_disk(), leg); }
    EXPECT_EQ(1, captured->fields.clean_unmount); // destructor persisted a clean stop
    return be64toh(captured->fields.bitmap.age);
}

TEST(Raid1MissingLegEpoch, CleanPullBumpsByOne) {
    EXPECT_EQ(6u, missing_start_age(ublkpp::raid1::read_route::EITHER, 1, 5));
}

TEST(Raid1MissingLegEpoch, HealthyCrashFencesEpoch) {
    EXPECT_EQ(21u, missing_start_age(ublkpp::raid1::read_route::EITHER, 0, 5)); // 5 + k_age_bump
}

TEST(Raid1MissingLegEpoch, DegradedRestartDoesNotRebump) {
    EXPECT_EQ(6u, missing_start_age(ublkpp::raid1::read_route::DEVB, 1, 6));
}

// Reattach after a clean pull (survivor one ahead): the survivor's SB wins arbitration (persisted
// mode consumed) and the stale leg resumes as a target -- no promotion, no dirty-all.
TEST(Raid1MissingLegEpoch, ReattachAfterCleanPullResumes) {
    using RM = ublkpp::raid1::resync_copy_mode;
    auto a = make_degraded_leg(false, 1, 5, RM::BLIND, ublkpp::raid1::read_route::EITHER); // pulled leg
    auto b = make_degraded_leg(true, 1, 6, RM::CHECK, ublkpp::raid1::read_route::DEVB);    // survivor
    ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), a, b);
    raid.toggle_resync(false);
    EXPECT_EQ(RM::CHECK, raid.current_resync_mode()) << "survivor's SB must win arbitration";
    EXPECT_EQ(0u, raid.replica_states().bytes_to_sync) << "resume from superbitmap, not dirty-all";
}

// Reattach after a healthy-crash fence (survivor k_age_bump ahead): the stale leg is promoted and
// fully rebuilt.
TEST(Raid1MissingLegEpoch, ReattachAfterCrashFenceRebuilds) {
    using RM = ublkpp::raid1::resync_copy_mode;
    auto a = make_degraded_leg(false, 0, 5, RM::BLIND, ublkpp::raid1::read_route::EITHER);
    auto b = make_degraded_leg(true, 1, 21, RM::CHECK, ublkpp::raid1::read_route::DEVB);
    ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), a, b);
    raid.toggle_resync(false);
    EXPECT_EQ(RM::CHECK, raid.current_resync_mode());
    EXPECT_GT(raid.replica_states().bytes_to_sync, Gi / 2) << "promoted stale leg must dirty-all";
}

// Unclean shutdown while already degraded (route stored, clean_unmount=0) with the backup leg still
// absent at restart -- the common post-crash recovery shape. The superbitmap cannot be trusted across
// an unclean degraded crash, so the surviving leg dirty-alls the whole array and fences the epoch
// (+k_age_bump) so a reattached leg fully rebuilds. The persisted mode is deliberately NOT resumed
// here: with no target present no resync runs, and the reattach (promoted) path re-derives the mode.
TEST(Raid1MissingLegEpoch, UncleanDegradedMissingLegDirtyAlls) {
    using RM = ublkpp::raid1::resync_copy_mode;
    auto captured = std::make_shared< ublkpp::raid1::SuperBlock >();
    // Live leg is device_b (route DEVB, canonical); device_a is absent. Persisted mode is CHECK.
    auto leg = make_degraded_leg(true, 0, 5, RM::CHECK, ublkpp::raid1::read_route::DEVB, captured);
    {
        ublkpp::raid1::Raid1Disk raid(boost::uuids::string_generator()(test_uuid), ublkpp::make_missing_disk(), leg);
        raid.toggle_resync(false);
        EXPECT_GT(raid.replica_states().bytes_to_sync, Gi / 2) << "superbitmap untrusted -> dirty-all";
        EXPECT_EQ(RM::BLIND, raid.current_resync_mode())
            << "persisted mode not resumed with the target absent; reattach re-derives it";
    }
    EXPECT_EQ(1, captured->fields.clean_unmount) << "destructor persisted a clean stop";
    EXPECT_EQ(21u, be64toh(captured->fields.bitmap.age)) << "epoch fenced by k_age_bump (5 + 16)";
}
