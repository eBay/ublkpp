#include "async_raid1_common.hpp"

// FLUSH is handled before async_iov dispatches children — no SQEs submitted. The completion is
// recorded synchronously at submit and delivered exactly once by the next inject_cqe/poll drain.
TEST_F(AsyncRaid1Fixture, FlushCompletesSync) {
    EXPECT_CALL(*disk_a, submit_iov(_, _, _, _, _)).Times(0);
    EXPECT_CALL(*disk_b, submit_iov(_, _, _, _, _)).Times(0);

    auto res = mock->submit_io(0, UBLK_IO_OP_FLUSH, 0, 0, nullptr);
    ASSERT_TRUE(res);
    EXPECT_EQ(res.value(), 0u); // no CqeStates registered

    auto completions = mock->inject_cqe(0, 0);
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_EQ(completions[0].tag, 0);
    EXPECT_EQ(completions[0].result, 0);

    // Exactly-once: the completion was drained above; a second injection reports nothing.
    EXPECT_TRUE(mock->inject_cqe(0, 0).empty());
}
