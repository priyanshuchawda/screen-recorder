#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "encoder/async_mft_scheduler.h"
#include "controller/encoder_backpressure_metrics.h"
#include "utils/pipeline_wake_signal.h"

namespace {

using Scheduler = sr::AsyncMftScheduler<int, int>;

template <typename Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

TEST(EncoderBackpressureIntegration,
     ControllerDrainsOutputAndServicesAudioWhileInputCapacityIsFull) {
    Scheduler scheduler(1, 1, 1, sizeof(int));
    ASSERT_TRUE(scheduler.enqueue_input(10));
    int encoded_output = 20;
    ASSERT_TRUE(scheduler.publish_output(encoded_output, sizeof(encoded_output)));
    ASSERT_FALSE(scheduler.input_capacity_available());

    sr::PipelineWakeSignal wake;
    std::atomic<bool> controller_done{false};
    std::atomic<bool> audio_serviced{false};
    std::atomic<bool> output_drained{false};
    std::atomic<bool> retried_input_queued{false};

    std::thread controller([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!controller_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            const auto generation = wake.generation();
            while (scheduler.try_pop_output()) {
                output_drained.store(true, std::memory_order_release);
            }
            // Production controller continues its microphone/loopback mixer
            // pass even when encoder input capacity is unavailable.
            audio_serviced.store(true, std::memory_order_release);
            if (scheduler.input_capacity_available()) {
                retried_input_queued.store(scheduler.enqueue_input(11),
                                           std::memory_order_release);
                controller_done.store(true, std::memory_order_release);
                break;
            }
            wake.wait_for_change(generation, std::chrono::milliseconds(20));
        }
    });

    const bool controller_serviced_pipeline = wait_until([&] {
        return audio_serviced.load(std::memory_order_acquire) &&
               output_drained.load(std::memory_order_acquire);
    });
    if (!controller_serviced_pipeline) {
        controller_done.store(true, std::memory_order_release);
        wake.notify(sr::PipelineWakeSource::Lifecycle);
        controller.join();
        FAIL() << "controller did not drain the output and service audio under input pressure";
    }

    scheduler.grant_input(0);
    auto request = scheduler.take_input_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->input, 10);
    scheduler.mark_input_accepted();
    wake.notify(sr::PipelineWakeSource::EncoderInputCapacity);

    const bool retried = wait_until([&] {
        return retried_input_queued.load(std::memory_order_acquire);
    });
    if (!retried) {
        controller_done.store(true, std::memory_order_release);
        wake.notify(sr::PipelineWakeSource::Lifecycle);
    }
    controller.join();

    EXPECT_TRUE(retried);
    EXPECT_EQ(scheduler.input_count(), 1u);
}

TEST(EncoderBackpressureIntegration, LifecycleNotificationCancelsCooperativeWaitPromptly) {
    Scheduler scheduler(1, 1, 1, sizeof(int));
    ASSERT_TRUE(scheduler.enqueue_input(10));
    sr::PipelineWakeSignal wake;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> loop_iterations{0};

    std::thread controller([&] {
        while (running.load(std::memory_order_acquire)) {
            const auto generation = wake.generation();
            loop_iterations.fetch_add(1, std::memory_order_relaxed);
            if (!scheduler.input_capacity_available()) {
                wake.wait_for_change(generation, std::chrono::seconds(2));
            }
        }
    });

    const bool started_waiting = wait_until([&] {
        return loop_iterations.load(std::memory_order_relaxed) > 0;
    });
    EXPECT_TRUE(started_waiting);
    const auto started = std::chrono::steady_clock::now();
    running.store(false, std::memory_order_release);
    wake.notify(sr::PipelineWakeSource::Lifecycle);
    controller.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
}

TEST(EncoderBackpressureIntegration, BackpressureEpisodesSeparateRecoveryCancellationAndTimeout) {
    sr::EncoderBackpressureMetrics metrics;
    const auto start = sr::EncoderBackpressureMetrics::Clock::now();
    EXPECT_TRUE(metrics.begin_wait(start));
    EXPECT_FALSE(metrics.begin_wait(start + std::chrono::milliseconds(1)));
    metrics.resolve(sr::EncoderBackpressureMetrics::Resolution::Recovered,
                    start + std::chrono::milliseconds(4));
    EXPECT_TRUE(metrics.begin_wait(start + std::chrono::milliseconds(5)));
    metrics.resolve(sr::EncoderBackpressureMetrics::Resolution::Cancelled,
                    start + std::chrono::milliseconds(8));
    EXPECT_TRUE(metrics.begin_wait(start + std::chrono::milliseconds(9)));
    metrics.resolve(sr::EncoderBackpressureMetrics::Resolution::TimedOut,
                    start + std::chrono::milliseconds(15));
    EXPECT_TRUE(metrics.begin_wait(start + std::chrono::milliseconds(16)));
    metrics.resolve(sr::EncoderBackpressureMetrics::Resolution::Failed,
                    start + std::chrono::milliseconds(20));

    const auto result = metrics.snapshot();
    EXPECT_EQ(result.attempts, 4u);
    EXPECT_EQ(result.recoveries, 1u);
    EXPECT_EQ(result.cancellations, 1u);
    EXPECT_EQ(result.timeouts, 1u);
    EXPECT_EQ(result.failures, 1u);
    EXPECT_EQ(result.max_wait_us, 6000u);
    EXPECT_EQ(result.wait_us.samples, 4u);
    EXPECT_EQ(result.wait_us.p95_upper_us, 8192u);
}

} // namespace
