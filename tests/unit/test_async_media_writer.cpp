#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "storage/async_media_writer.h"

using namespace std::chrono_literals;

namespace {
using Writer = sr::AsyncMediaWriter<int>;
using Packet = sr::MediaPacket<int>;

Packet make_packet(int value, uint64_t bytes = 4) {
    return {sr::MediaStream::Video, value, bytes,
            static_cast<int64_t>(value) * 100'000, 100'000};
}
} // namespace

TEST(AsyncMediaWriterTest, DrainsAllAcceptedPacketsBeforeFinishReturns) {
    Writer writer({4, 64, 50'000'000});
    std::mutex mutex;
    std::vector<int> written;
    ASSERT_TRUE(writer.start([&](Writer::Packet& packet) {
        std::lock_guard lock(mutex);
        written.push_back(packet.payload);
        return true;
    }));
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(writer.enqueue(make_packet(i)), sr::MediaQueueResult::Accepted);
    }
    EXPECT_TRUE(writer.finish());
    EXPECT_EQ(writer.snapshot().samples_accepted, 4u);
    EXPECT_EQ(writer.snapshot().samples_written, 4u);
    ASSERT_EQ(written.size(), 4u);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(written[i], i);
}

TEST(AsyncMediaWriterTest, DelayedWriterAppliesByteAndPacketBackpressure) {
    Writer writer({2, 8, 50'000'000});
    std::promise<void> entered;
    std::promise<void> unblock;
    auto release = unblock.get_future().share();
    std::atomic<bool> first_call{true};
    ASSERT_TRUE(writer.start([&](Writer::Packet&) {
        if (first_call.exchange(false)) {
            entered.set_value();
            release.wait();
        }
        return true;
    }));
    ASSERT_EQ(writer.enqueue(make_packet(0)), sr::MediaQueueResult::Accepted);
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(writer.enqueue(make_packet(1)), sr::MediaQueueResult::Accepted);
    EXPECT_EQ(writer.enqueue(make_packet(2)), sr::MediaQueueResult::Accepted);
    EXPECT_EQ(writer.enqueue(make_packet(3)), sr::MediaQueueResult::PacketLimit);
    const auto blocked = writer.snapshot();
    EXPECT_EQ(blocked.queue.packet_count, 2u);
    EXPECT_LE(blocked.queue.queued_bytes, 8u);
    unblock.set_value();
    EXPECT_TRUE(writer.finish());
    EXPECT_EQ(writer.snapshot().samples_written, 3u);
}

TEST(AsyncMediaWriterTest, WriteFailureIsTerminalAndAccountsDiscardedPackets) {
    Writer writer({4, 64, 50'000'000});
    std::promise<void> entered;
    std::promise<void> unblock;
    auto release = unblock.get_future().share();
    std::atomic<unsigned> calls{0};
    ASSERT_TRUE(writer.start([&](Writer::Packet&) {
        const unsigned call = calls.fetch_add(1);
        if (call == 0) {
            entered.set_value();
            release.wait();
            return false;
        }
        return true;
    }));
    ASSERT_EQ(writer.enqueue(make_packet(0)), sr::MediaQueueResult::Accepted);
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_EQ(writer.enqueue(make_packet(1)), sr::MediaQueueResult::Accepted);
    ASSERT_EQ(writer.enqueue(make_packet(2)), sr::MediaQueueResult::Accepted);
    unblock.set_value();
    EXPECT_FALSE(writer.finish());
    const auto snapshot = writer.snapshot();
    EXPECT_TRUE(snapshot.failed);
    EXPECT_EQ(snapshot.write_failures, 1u);
    EXPECT_EQ(snapshot.samples_accepted, 3u);
    EXPECT_EQ(snapshot.samples_written, 0u);
    EXPECT_EQ(snapshot.queue.discarded_packets, 2u);
}

TEST(AsyncMediaWriterTest, WorkerSetupFailureDiscardsQueuedMedia) {
    Writer writer({4, 64, 50'000'000});
    EXPECT_FALSE(writer.start([](Writer::Packet&) { return true; }, [] { return false; }));
    EXPECT_FALSE(writer.finish());
    const auto snapshot = writer.snapshot();
    EXPECT_TRUE(snapshot.failed);
    EXPECT_EQ(snapshot.samples_accepted, 0u);
    EXPECT_EQ(snapshot.samples_written, 0u);
}

