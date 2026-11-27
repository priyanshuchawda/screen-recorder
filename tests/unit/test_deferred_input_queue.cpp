#include <gtest/gtest.h>

#include "encoder/deferred_input_queue.h"

TEST(DeferredInputQueueTest, WaitsForInputEventWithoutDiscardingSample) {
    sr::DeferredInputQueue<int, 4> queue;
    ASSERT_TRUE(queue.try_push(42));
    unsigned input_events = 0;
    size_t calls = 0;

    const auto first = sr::submit_ready_inputs(queue, input_events, [&](int&) {
        ++calls;
        return sr::DeferredSubmitResult::Accepted;
    });

    EXPECT_EQ(first.accepted, 0u);
    EXPECT_EQ(calls, 0u);
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front(), 42);
}

TEST(DeferredInputQueueTest, RetriesAndAcceptsSampleOnLaterInputEvent) {
    sr::DeferredInputQueue<int, 4> queue;
    ASSERT_TRUE(queue.try_push(42));
    unsigned input_events = 1;
    int submitted = 0;

    const auto result = sr::submit_ready_inputs(queue, input_events, [&](int& value) {
        submitted = value;
        return sr::DeferredSubmitResult::Accepted;
    });

    EXPECT_EQ(result.accepted, 1u);
    EXPECT_EQ(submitted, 42);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(input_events, 0u);
}

TEST(DeferredInputQueueTest, KeepsSampleAfterTransientNotAcceptedResult) {
    sr::DeferredInputQueue<int, 4> queue;
    ASSERT_TRUE(queue.try_push(7));
    unsigned input_events = 1;
    const auto rejected = sr::submit_ready_inputs(queue, input_events, [](int&) {
        return sr::DeferredSubmitResult::NotAccepted;
    });
    EXPECT_EQ(rejected.not_accepted, 1u);
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(input_events, 0u);

    input_events = 1;
    const auto retried = sr::submit_ready_inputs(queue, input_events, [](int&) {
        return sr::DeferredSubmitResult::Accepted;
    });
    EXPECT_EQ(retried.accepted, 1u);
    EXPECT_TRUE(queue.empty());
}

TEST(DeferredInputQueueTest, EnforcesCapacityAndTracksHighWatermark) {
    sr::DeferredInputQueue<int, 2> queue;
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_EQ(queue.high_watermark(), 2u);
    EXPECT_EQ(queue.front(), 1);
}
