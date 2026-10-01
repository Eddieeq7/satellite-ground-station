#include "gs/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <thread>

using namespace gs;

TEST(SpscRing, RejectsNonPowerOfTwo) {
    EXPECT_THROW(SpscRing<int>(3), std::invalid_argument);
    EXPECT_THROW(SpscRing<int>(0), std::invalid_argument);
    EXPECT_NO_THROW(SpscRing<int>(8));
}

TEST(SpscRing, EmptyPopReturnsNullopt) {
    SpscRing<int> r(4);
    EXPECT_FALSE(r.try_pop());
}

TEST(SpscRing, FifoOrder) {
    SpscRing<int> r(8);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(r.try_push(int{i}));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(*r.try_pop(), i);
}

TEST(SpscRing, FullRejectsPush) {
    SpscRing<int> r(4);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(r.try_push(int{i}));
    EXPECT_FALSE(r.try_push(99));
    EXPECT_EQ(*r.try_pop(), 0);
    EXPECT_TRUE(r.try_push(99));
}

TEST(SpscRing, WrapsManyTimes) {
    SpscRing<int> r(4);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(r.try_push(int{i}));
        ASSERT_EQ(*r.try_pop(), i);
    }
    EXPECT_EQ(r.size_approx(), 0u);
}

TEST(SpscRing, MoveOnlyType) {
    SpscRing<std::unique_ptr<int>> r(2);
    ASSERT_TRUE(r.try_push(std::make_unique<int>(7)));
    auto v = r.try_pop();
    ASSERT_TRUE(v && *v);
    EXPECT_EQ(**v, 7);
}

// Concurrent producer/consumer: every value arrives exactly once, in order.
// Run under ThreadSanitizer in CI (GS_SANITIZE=thread).
TEST(SpscRing, ConcurrentTransferPreservesOrder) {
    constexpr uint64_t kN = 2'000'000;
    SpscRing<uint64_t> r(1024);
    std::thread producer([&] {
        for (uint64_t i = 0; i < kN; ++i)
            while (!r.try_push(uint64_t{i})) std::this_thread::yield();
    });
    uint64_t expected = 0;
    while (expected < kN) {
        if (auto v = r.try_pop()) {
            ASSERT_EQ(*v, expected);
            ++expected;
        }
    }
    producer.join();
    EXPECT_FALSE(r.try_pop());
}
