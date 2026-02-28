// test_bounded_queue.cpp — Unit tests for BoundedQueue (T005)
// Multi-threaded producer/consumer, capacity enforcement

#include <gtest/gtest.h>
#include "utils/bounded_queue.h"
#include <thread>
#include <atomic>
#include <vector>

using sr::BoundedQueue;

TEST(BoundedQueueTest, StartsEmpty) {
    BoundedQueue<int, 5> q;
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_EQ(q.size(), 0u);
}

TEST(BoundedQueueTest, PushAndPop) {
    BoundedQueue<int, 5> q;
    EXPECT_TRUE(q.try_push(42));
    EXPECT_EQ(q.size(), 1u);

    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
    EXPECT_TRUE(q.empty());
}

TEST(BoundedQueueTest, RejectsWhenFull) {
    BoundedQueue<int, 3> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.try_push(4)); // Rejected
    EXPECT_EQ(q.size(), 3u);
}

TEST(BoundedQueueTest, PopFromEmpty) {
    BoundedQueue<int, 5> q;
    auto val = q.try_pop();
    EXPECT_FALSE(val.has_value());
}

TEST(BoundedQueueTest, FIFOOrder) {
    BoundedQueue<int, 5> q;
    for (int i = 0; i < 5; i++) q.try_push(std::move(i));

    for (int i = 0; i < 5; i++) {
        auto val = q.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i);
    }
}

TEST(BoundedQueueTest, WaitPopTimeout) {
    BoundedQueue<int, 5> q;
    auto val = q.wait_pop(std::chrono::milliseconds(50));
    EXPECT_FALSE(val.has_value());
}

TEST(BoundedQueueTest, WaitPopSucceeds) {
    BoundedQueue<int, 5> q;
    q.try_push(99);
    auto val = q.wait_pop(std::chrono::milliseconds(100));
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 99);
}

TEST(BoundedQueueTest, MoveSemantics) {
    BoundedQueue<std::unique_ptr<int>, 3> q;
    q.try_push(std::make_unique<int>(42));
    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(**val, 42);
}

TEST(BoundedQueueTest, MultiThreadedProducerConsumer) {
    BoundedQueue<int, 5> q;
    std::atomic<int> consumed_count{0};
    std::atomic<int> produced_count{0};
    std::atomic<bool> producers_done{false};
    constexpr int ITEMS_PER_PRODUCER = 100;

    // 2 producers
    auto producer = [&](int start) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
            while (!q.try_push(start + i)) {
                std::this_thread::yield();
            }
            produced_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // 1 consumer — keeps draining until both producers are done AND queue is empty
    // This avoids the race where producers push items after the consumer exits
    auto consumer = [&]() {
        while (!producers_done.load(std::memory_order_acquire) || !q.empty()) {
            auto val = q.try_pop();
            if (val.has_value()) {
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::thread p1(producer, 0);
    std::thread p2(producer, 1000);
    std::thread c1(consumer);

    p1.join();
    p2.join();
    // Signal consumer that no more items will be produced
    producers_done.store(true, std::memory_order_release);
    c1.join();

    EXPECT_EQ(consumed_count.load(), 2 * ITEMS_PER_PRODUCER);
    EXPECT_EQ(produced_count.load(), 2 * ITEMS_PER_PRODUCER);

    // Queue must be empty after all producers are done and consumer has drained it
    EXPECT_TRUE(q.empty());
}

TEST(BoundedQueueTest, QueueNeverExceedsCapacity) {
    BoundedQueue<int, 5> q;
    std::atomic<bool> done{false};
    std::atomic<size_t> max_observed{0};

    auto observer = [&]() {
        while (!done.load(std::memory_order_relaxed)) {
            size_t s = q.size();
            size_t prev = max_observed.load(std::memory_order_relaxed);
            if (s > prev) max_observed.store(s, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    auto producer = [&]() {
        for (int i = 0; i < 500; i++) {
            q.try_push(std::move(i));
            std::this_thread::yield();
        }
    };

    auto consumer = [&]() {
        for (int i = 0; i < 500; i++) {
            q.try_pop();
            std::this_thread::yield();
        }
    };

    std::thread obs(observer);
    std::thread p(producer);
    std::thread c(consumer);

    p.join();
    c.join();
    done.store(true, std::memory_order_relaxed);
    obs.join();

    EXPECT_LE(max_observed.load(), 5u);
}
