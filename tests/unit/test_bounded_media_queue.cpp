#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "storage/bounded_media_queue.h"

namespace {

using Queue = sr::BoundedMediaQueue<int>;
using Packet = sr::MediaPacket<int>;

Packet packet(sr::MediaStream stream, int value, uint64_t bytes,
              int64_t pts, int64_t duration) {
    return Packet{stream, value, bytes, pts, duration};
}

} // namespace

TEST(BoundedMediaQueueTest, EnforcesPacketLimitWithoutEviction) {
    Queue queue({2, 100, 50'000'000});
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 60, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 2, 40, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 3, 1, 10'000'000, 10'000'000)),
              sr::MediaQueueResult::PacketLimit);

    const auto stats = queue.snapshot();
    EXPECT_EQ(stats.packet_count, 2u);
    EXPECT_EQ(stats.queued_bytes, 100u);
    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->payload, 1);
    EXPECT_EQ(queue.snapshot().packet_count, 1u);
}

TEST(BoundedMediaQueueTest, EnforcesByteLimitWithoutEviction) {
    Queue queue({4, 100, 50'000'000});
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 60, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 2, 40, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 3, 1, 10'000'000, 10'000'000)),
              sr::MediaQueueResult::ByteLimit);
    const auto stats = queue.snapshot();
    EXPECT_EQ(stats.packet_count, 2u);
    EXPECT_EQ(stats.queued_bytes, 100u);
    EXPECT_EQ(stats.rejected_packets, 1u);
}

TEST(BoundedMediaQueueTest, CountsAllocatedCapacityAndActiveWriteAgainstBudget) {
    Queue queue({4, 100, 50'000'000, 100});
    Packet first = packet(sr::MediaStream::Video, 1, 20, 0, 10'000'000);
    first.allocated_bytes = 80;
    ASSERT_EQ(queue.try_push(std::move(first)), sr::MediaQueueResult::Accepted);
    auto active = queue.wait_pop();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(queue.snapshot().active_allocated_bytes, 80u);
    EXPECT_EQ(queue.snapshot().total_allocated_bytes_high_watermark, 80u);

    Packet second = packet(sr::MediaStream::Audio, 2, 20, 0, 10'000'000);
    second.allocated_bytes = 21;
    EXPECT_EQ(queue.try_push(std::move(second)), sr::MediaQueueResult::ByteLimit);
    queue.release_active(*active);

    Packet third = packet(sr::MediaStream::Audio, 3, 20, 0, 10'000'000);
    third.allocated_bytes = 21;
    EXPECT_EQ(queue.try_push(std::move(third)), sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.snapshot().queued_allocated_bytes_high_watermark, 80u);
    EXPECT_EQ(queue.snapshot().total_allocated_bytes_high_watermark, 80u);
}

TEST(BoundedMediaQueueTest, PreservesHistoricalMaximumStreamSpanAfterDrain) {
    Queue queue({4, 100, 50'000'000});
    ASSERT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 10, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    ASSERT_EQ(queue.try_push(packet(sr::MediaStream::Video, 2, 10, 20'000'000, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.snapshot().stream_span_high_watermark_100ns, 30'000'000);
    (void)queue.try_pop();
    (void)queue.try_pop();
    EXPECT_EQ(queue.snapshot().max_stream_span_100ns, 0);
    EXPECT_EQ(queue.snapshot().stream_span_high_watermark_100ns, 30'000'000);
}

TEST(BoundedMediaQueueTest, EnforcesPerStreamTimestampSpan) {
    Queue queue({8, 1024, 20'000'000});
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 1, 10, 10'000'000, 5'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 2, 10, 0, 5'000'000)),
              sr::MediaQueueResult::Accepted);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 3, 10, 25'000'000, 5'000'000)),
              sr::MediaQueueResult::DurationLimit);
    // A different stream has its own span and remains admissible.
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 4, 10, 100'000'000, 5'000'000)),
              sr::MediaQueueResult::Accepted);
}

TEST(BoundedMediaQueueTest, RejectsInvalidMetadataAndClosedQueue) {
    Queue queue({2, 100, 50'000'000});
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 0, 0, 1)),
              sr::MediaQueueResult::InvalidPacket);
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 10, 0, -1)),
              sr::MediaQueueResult::InvalidPacket);
    queue.close();
    EXPECT_EQ(queue.try_push(packet(sr::MediaStream::Video, 1, 10, 0, 1)),
              sr::MediaQueueResult::Closed);
}

TEST(BoundedMediaQueueTest, CloseAllowsDrainThenWakesEmptyWaiter) {
    Queue queue({2, 100, 50'000'000});
    ASSERT_EQ(queue.try_push(packet(sr::MediaStream::Video, 7, 20, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    queue.close();
    auto queued = queue.wait_pop();
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->payload, 7);
    EXPECT_FALSE(queue.wait_pop().has_value());
}

TEST(BoundedMediaQueueTest, DiscardOnFailureIsExplicitAndWakesWaiter) {
    Queue queue({4, 100, 50'000'000});
    ASSERT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 1, 20, 0, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    ASSERT_EQ(queue.try_push(packet(sr::MediaStream::Audio, 2, 30, 10'000'000, 10'000'000)),
              sr::MediaQueueResult::Accepted);
    queue.close_and_discard();
    const auto stats = queue.snapshot();
    EXPECT_EQ(stats.packet_count, 0u);
    EXPECT_EQ(stats.discarded_packets, 2u);
    EXPECT_EQ(stats.discarded_bytes, 50u);
    EXPECT_FALSE(queue.wait_pop().has_value());
}

TEST(BoundedMediaQueueTest, ConcurrentProducerAndConsumerStayWithinBounds) {
    Queue queue({16, 160, 50'000'000});
    std::atomic<uint64_t> accepted{0};
    std::thread producer([&] {
        for (int i = 0; i < 1000; ++i) {
            if (queue.try_push(packet(sr::MediaStream::Audio, i, 10,
                                      static_cast<int64_t>(i) * 10'000,
                                      10'000)) == sr::MediaQueueResult::Accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
            if (i % 4 == 0) std::this_thread::yield();
        }
        queue.close();
    });
    uint64_t consumed = 0;
    while (auto item = queue.wait_pop()) {
        (void)item;
        ++consumed;
        const auto snapshot = queue.snapshot();
        EXPECT_LE(snapshot.packet_count, 16u);
        EXPECT_LE(snapshot.queued_bytes, 160u);
    }
    producer.join();
    EXPECT_EQ(consumed, accepted.load(std::memory_order_relaxed));
    EXPECT_EQ(queue.snapshot().packet_count, 0u);
}

