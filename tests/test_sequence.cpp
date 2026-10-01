#include "gs/sequence_tracker.hpp"

#include <gtest/gtest.h>

using namespace gs;

TEST(Sequence, FirstThenInOrder) {
    SequenceTracker t;
    EXPECT_EQ(t.observe(10), SeqVerdict::First);
    EXPECT_EQ(t.observe(11), SeqVerdict::InOrder);
    EXPECT_EQ(t.observe(12), SeqVerdict::InOrder);
    EXPECT_EQ(t.stats().received, 3u);
    EXPECT_EQ(t.stats().lost, 0u);
}

TEST(Sequence, GapCountsLost) {
    SequenceTracker t;
    t.observe(0);
    EXPECT_EQ(t.observe(5), SeqVerdict::Gap);
    EXPECT_EQ(t.stats().lost, 4u);
    EXPECT_EQ(t.highest(), 5u);
}

TEST(Sequence, DuplicateOfHead) {
    SequenceTracker t;
    t.observe(1);
    EXPECT_EQ(t.observe(1), SeqVerdict::Duplicate);
    EXPECT_EQ(t.stats().duplicates, 1u);
    EXPECT_EQ(t.stats().received, 1u);
}

TEST(Sequence, DuplicateBehindHead) {
    SequenceTracker t;
    for (uint32_t s = 0; s < 10; ++s) t.observe(s);
    EXPECT_EQ(t.observe(4), SeqVerdict::Duplicate);
}

TEST(Sequence, LateArrivalIsReorderedAndUncountsLoss) {
    SequenceTracker t;
    t.observe(0);
    t.observe(2);  // 1 looks lost
    EXPECT_EQ(t.stats().lost, 1u);
    EXPECT_EQ(t.observe(1), SeqVerdict::Reordered);
    EXPECT_EQ(t.stats().lost, 0u);
    EXPECT_EQ(t.stats().reordered, 1u);
    EXPECT_EQ(t.stats().received, 3u);
}

TEST(Sequence, ReorderedThenDuplicateOfReordered) {
    SequenceTracker t;
    t.observe(0);
    t.observe(2);
    t.observe(1);
    EXPECT_EQ(t.observe(1), SeqVerdict::Duplicate);
}

TEST(Sequence, SwappedPair) {
    SequenceTracker t;
    t.observe(0);
    EXPECT_EQ(t.observe(2), SeqVerdict::Gap);
    EXPECT_EQ(t.observe(1), SeqVerdict::Reordered);
    EXPECT_EQ(t.observe(3), SeqVerdict::InOrder);
    EXPECT_EQ(t.stats().lost, 0u);
}

TEST(Sequence, TooOldOutsideWindow) {
    SequenceTracker t;
    t.observe(0);
    t.observe(SequenceTracker::kWindow + 10);
    EXPECT_EQ(t.observe(1), SeqVerdict::TooOld);
    EXPECT_EQ(t.stats().too_old, 1u);
}

TEST(Sequence, LargeJumpClearsStaleBits) {
    SequenceTracker t;
    for (uint32_t s = 0; s < 50; ++s) t.observe(s);
    // Jump forward by exactly one window: slot (50 + kWindow) % kWindow == 50 % kWindow was never seen
    // in this lap, but slot for seq 10 + kWindow collides with old seq 10.
    t.observe(SequenceTracker::kWindow + 40);
    EXPECT_EQ(t.observe(SequenceTracker::kWindow + 10), SeqVerdict::Reordered);
}

TEST(Sequence, WrapAroundUint32) {
    SequenceTracker t;
    t.observe(0xFFFFFFFEu);
    EXPECT_EQ(t.observe(0xFFFFFFFFu), SeqVerdict::InOrder);
    EXPECT_EQ(t.observe(0u), SeqVerdict::InOrder);
    EXPECT_EQ(t.observe(1u), SeqVerdict::InOrder);
    EXPECT_EQ(t.stats().lost, 0u);
}

TEST(Sequence, WrapAroundWithGap) {
    SequenceTracker t;
    t.observe(0xFFFFFFFEu);
    EXPECT_EQ(t.observe(2u), SeqVerdict::Gap);
    EXPECT_EQ(t.stats().lost, 3u);  // FFFFFFFF, 0, 1
    EXPECT_EQ(t.observe(0u), SeqVerdict::Reordered);
    EXPECT_EQ(t.stats().lost, 2u);
}

TEST(Sequence, MixedImpairmentAccounting) {
    SequenceTracker t;
    // 0..9 with 3 lost, 5 duplicated, 7/8 swapped.
    for (uint32_t s : {0u, 1u, 2u, 4u, 5u, 5u, 6u, 8u, 7u, 9u}) t.observe(s);
    const auto& s = t.stats();
    EXPECT_EQ(s.received, 9u);
    EXPECT_EQ(s.lost, 1u);
    EXPECT_EQ(s.duplicates, 1u);
    EXPECT_EQ(s.reordered, 1u);
}
