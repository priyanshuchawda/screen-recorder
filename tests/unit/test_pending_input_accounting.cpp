#include <gtest/gtest.h>

#include <atomic>
#include <deque>

#include "encoder/pending_input_accounting.h"

TEST(PendingInputAccountingTest, AbandoningEncoderInputsCountsAndClearsEveryPendingTimestamp) {
    std::deque<int64_t> pending_pts{100, 200, 300};
    std::atomic<uint64_t> pending_count{3};
    std::atomic<uint64_t> abandoned_count{0};

    const uint64_t abandoned = sr::abandon_pending_encoder_inputs(
        pending_pts, pending_count, abandoned_count);

    EXPECT_EQ(abandoned, 3u);
    EXPECT_TRUE(pending_pts.empty());
    EXPECT_EQ(pending_count.load(), 0u);
    EXPECT_EQ(abandoned_count.load(), 3u);
}

TEST(PendingInputAccountingTest, AbandoningNoInputsDoesNotChangeAbandonedCount) {
    std::deque<int64_t> pending_pts;
    std::atomic<uint64_t> pending_count{0};
    std::atomic<uint64_t> abandoned_count{2};

    EXPECT_EQ(sr::abandon_pending_encoder_inputs(
                  pending_pts, pending_count, abandoned_count), 0u);
    EXPECT_EQ(abandoned_count.load(), 2u);
}
