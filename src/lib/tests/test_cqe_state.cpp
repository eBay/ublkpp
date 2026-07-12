#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <coroutine>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <ublksrv.h>

#include <ublkpp/lib/cqe_state.hpp>

SISL_LOGGING_INIT(ublksrv)

SISL_OPTIONS_ENABLE(logging)

namespace {

struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

// ============================================================================
// async_io::next_state
// ============================================================================

TEST(AsyncIo, NextStateCreatesNewState) {
    ublkpp::async_io io{};
    io._pool.reserve(1);
    auto* s = io.next_state();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->_owner, &io);
    EXPECT_EQ(s->_result, 0);
    EXPECT_FALSE(s->_result_ready);
    EXPECT_FALSE(s->_waiter);
}

TEST(AsyncIo, NextStateGrowsPool) {
    ublkpp::async_io io{};
    io._pool.reserve(3);
    auto* s1 = io.next_state();
    auto* s2 = io.next_state();
    auto* s3 = io.next_state();
    EXPECT_NE(s1, s2);
    EXPECT_NE(s2, s3);
    EXPECT_EQ(io._pool.size(), 3u);
}

TEST(AsyncIo, NextStateAddressStability) {
    ublkpp::async_io io{};
    io._pool.reserve(64);
    auto* first = io.next_state();
    for (int i = 1; i < 64; ++i)
        io.next_state();
    // std::vector with pre-reserved capacity guarantees push_back doesn't invalidate pointers
    EXPECT_EQ(first, &io._pool.front());
}

// ============================================================================
// build_cqe_state_data
// ============================================================================

TEST(BuildCqeStateData, RegistersStateInPool) {
    ublkpp::async_io io{};
    io._pool.reserve(1);
    ublk_io_data fake{};
    fake.private_data = &io;
    auto const [state, user_data] = ublkpp::build_cqe_state_data(&fake);
    // bit 63 marks it as a target SQE
    EXPECT_NE(user_data & (1ULL << 63), 0ULL);
    // lower bits decode to the registered cqe_state
    auto* decoded = reinterpret_cast< ublkpp::cqe_state* >(user_data & ~(1ULL << 63));
    ASSERT_EQ(io._pool.size(), 1u);
    EXPECT_EQ(decoded, &io._pool.front());
    EXPECT_EQ(state, decoded);
    EXPECT_EQ(state->_owner, &io);
}

TEST(BuildCqeStateData, EachCallGrowsPool) {
    ublkpp::async_io io{};
    io._pool.reserve(3);
    ublk_io_data fake{};
    fake.private_data = &io;
    ublkpp::build_cqe_state_data(&fake);
    ublkpp::build_cqe_state_data(&fake);
    ublkpp::build_cqe_state_data(&fake);
    EXPECT_EQ(io._pool.size(), 3u);
}

TEST(BuildCqeStateData, ReturnedPointerMatchesDecodedUserData) {
    ublkpp::async_io io{};
    io._pool.reserve(1);
    ublk_io_data fake{};
    fake.private_data = &io;
    auto const [state, user_data] = ublkpp::build_cqe_state_data(&fake);
    auto* decoded = reinterpret_cast< ublkpp::cqe_state* >(user_data & ~(1ULL << 63));
    EXPECT_EQ(state, decoded);
}

// ============================================================================
// cqe_state awaitable
// ============================================================================

// Coroutine that co_awaits *state and writes the result to *out.
static FireAndForget await_cqe_and_capture(ublkpp::cqe_state* state, int* out) { *out = co_await *state; }

TEST(cqe_state, AwaitReadyFalseWhenNotReady) {
    ublkpp::async_io io{};
    ublkpp::cqe_state state{};
    state._owner = &io;
    EXPECT_FALSE(state.await_ready());
}

TEST(cqe_state, AwaitReadyTrueWhenReady) {
    ublkpp::async_io io{};
    ublkpp::cqe_state state{};
    state._owner = &io;
    state._result_ready = true;
    EXPECT_TRUE(state.await_ready());
}

TEST(cqe_state, AwaitResumeReturnsResult) {
    ublkpp::async_io io{};
    ublkpp::cqe_state state{};
    state._owner = &io;
    state._result = 42;
    state._result_ready = true;
    EXPECT_EQ(state.await_resume(), 42);
}

TEST(cqe_state, AwaitSuspendInstallsWaiterInState) {
    ublkpp::async_io io{};
    ublkpp::cqe_state state{};
    state._owner = &io;
    EXPECT_FALSE(state._waiter);
    int captured = -1;
    await_cqe_and_capture(&state, &captured); // suspends; installs handle in state._waiter
    ASSERT_TRUE(state._waiter);
    EXPECT_EQ(captured, -1); // not resumed yet

    state._result = 7;
    state._result_ready = true;
    state._waiter.resume(); // triggers await_resume() -> captured = 7
    EXPECT_EQ(captured, 7);
}

TEST(cqe_state, FastPathSkipsSuspendWhenAlreadyReady) {
    ublkpp::async_io io{};
    ublkpp::cqe_state state{};
    state._owner = &io;
    state._result = 55;
    state._result_ready = true;
    int captured = -1;
    await_cqe_and_capture(&state, &captured); // await_ready=true -> no suspension
    EXPECT_FALSE(state._waiter);
    EXPECT_EQ(captured, 55);
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging);
    sisl::logging::SetLogger(std::string(argv[0]));
    spdlog::set_pattern("[%D %T.%e] [%n] [%^%l%$] [%t] %v");
    parsed_argc = 1;
    return RUN_ALL_TESTS();
}
