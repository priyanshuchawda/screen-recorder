#include <gtest/gtest.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <deque>
#include <atomic>
#include <functional>
#include <iomanip>
#include <thread>
#include <wrl/implements.h>

#include "encoder/async_mft_scheduler.h"
#include "encoder/async_mft_callback.h"

namespace {

class FakeAsyncMft {
public:
    HRESULT process_input(int input) {
        if (reject_next_) {
            reject_next_ = false;
            return MF_E_NOTACCEPTING;
        }
        accepted_.push_back(input);
        return S_OK;
    }

    void reject_next_input() { reject_next_ = true; }

    void make_outputs_ready() {
        while (!accepted_.empty()) {
            outputs_.push_back(accepted_.front());
            accepted_.pop_front();
        }
    }

    bool process_output(int& output) {
        if (outputs_.empty()) return false;
        output = outputs_.front();
        outputs_.pop_front();
        return true;
    }

private:
    bool reject_next_ = false;
    std::deque<int> accepted_;
    std::deque<int> outputs_;
};

class FakeEventGenerator final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFMediaEventGenerator> {
public:
    HRESULT initialize() { return MFCreateEventQueue(&queue_); }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
        return queue_ ? queue_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
        return queue_ ? queue_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) override {
        return queue_ ? queue_->GetEvent(flags, event) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended_type,
                                         HRESULT status, const PROPVARIANT* prop_value) override {
        return queue_ ? queue_->QueueEventParamVar(type, extended_type, status, prop_value) : MF_E_SHUTDOWN;
    }
    HRESULT queue(IMFMediaEvent* event) {
        return queue_ ? queue_->QueueEvent(event) : MF_E_SHUTDOWN;
    }
    void shutdown() { if (queue_) queue_->Shutdown(); }

private:
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> queue_;
};

class MfTestApartment {
public:
    HRESULT initialize() {
        com_hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_hr_)) return com_hr_;
        mf_hr_ = MFStartup(MF_VERSION);
        return mf_hr_;
    }
    ~MfTestApartment() {
        if (SUCCEEDED(mf_hr_)) MFShutdown();
        if (SUCCEEDED(com_hr_)) CoUninitialize();
    }

private:
    HRESULT com_hr_ = E_FAIL;
    HRESULT mf_hr_ = E_FAIL;
};

bool wait_until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

Microsoft::WRL::ComPtr<IMFMediaEvent> make_event(MediaEventType type) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    if (FAILED(MFCreateMediaEvent(type, GUID_NULL, S_OK, nullptr, &event))) return {};
    return event;
}

} // namespace

TEST(AsyncMftStallWatchdogTest, StartsDeadlineOnNewWorkAndResetsAfterIdle) {
    using Clock = sr::AsyncMftStallWatchdog::Clock;
    const auto start = Clock::time_point{};
    sr::AsyncMftStallWatchdog watchdog(std::chrono::seconds(5));

    EXPECT_FALSE(watchdog.until_stalled(false, start).has_value());
    ASSERT_TRUE(watchdog.until_stalled(true, start).has_value());
    EXPECT_EQ(*watchdog.until_stalled(true, start + std::chrono::seconds(4)),
              std::chrono::seconds(1));
    EXPECT_EQ(*watchdog.until_stalled(true, start + std::chrono::seconds(5)),
              std::chrono::milliseconds::zero());

    EXPECT_FALSE(watchdog.until_stalled(false, start + std::chrono::minutes(2)).has_value());
    EXPECT_EQ(*watchdog.until_stalled(true, start + std::chrono::minutes(2)),
              std::chrono::seconds(5));
}

TEST(AsyncMftStallWatchdogTest, ProgressRestartsThePendingWorkDeadline) {
    using Clock = sr::AsyncMftStallWatchdog::Clock;
    const auto start = Clock::time_point{};
    sr::AsyncMftStallWatchdog watchdog(std::chrono::seconds(5));
    watchdog.until_stalled(true, start);
    watchdog.mark_progress(start + std::chrono::seconds(4));
    EXPECT_EQ(*watchdog.until_stalled(true, start + std::chrono::seconds(8)),
              std::chrono::seconds(1));
}

TEST(AsyncMftSchedulerTest, QueuedInputWaitsForNeedInputCredit) {
    sr::AsyncMftScheduler<int, int> scheduler;

    ASSERT_TRUE(scheduler.enqueue_input(41));
    EXPECT_FALSE(scheduler.input_ready());

    scheduler.grant_input(3);
    ASSERT_TRUE(scheduler.input_ready());
    auto request = scheduler.take_input_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->stream_id, 3u);
    EXPECT_EQ(request->input, 41);
}

TEST(AsyncMftSchedulerTest, MultipleInputCreditsAreConsumedOnceInOrder) {
    sr::AsyncMftScheduler<int, int> scheduler;
    ASSERT_TRUE(scheduler.enqueue_input(1));
    ASSERT_TRUE(scheduler.enqueue_input(2));
    ASSERT_TRUE(scheduler.enqueue_input(3));
    scheduler.grant_input(0);
    scheduler.grant_input(0);
    scheduler.grant_input(0);

    for (int expected : {1, 2, 3}) {
        auto request = scheduler.take_input_request();
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->input, expected);
        scheduler.mark_input_accepted();
    }
    EXPECT_FALSE(scheduler.input_ready());
    EXPECT_EQ(scheduler.in_flight_inputs(), 3u);
}

TEST(AsyncMftSchedulerTest, RejectedInputRemainsQueuedForANewCredit) {
    sr::AsyncMftScheduler<int, int> scheduler;
    ASSERT_TRUE(scheduler.enqueue_input(9));
    scheduler.grant_input(0);
    auto first = scheduler.take_input_request();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->input, 9);

    ASSERT_TRUE(scheduler.retry_input_front(std::move(first->input)));
    EXPECT_FALSE(scheduler.input_ready());
    scheduler.grant_input(0);
    auto retry = scheduler.take_input_request();
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->input, 9);
}

TEST(AsyncMftSchedulerTest, ReservedSubmissionSlotKeepsRejectedInputRetryable) {
    sr::AsyncMftScheduler<int, int> scheduler(4, 1, 4, 16);
    for (int input : {1, 2, 3, 4}) ASSERT_TRUE(scheduler.enqueue_input(input));
    scheduler.grant_input(0);
    auto request = scheduler.take_input_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->input, 1);

    // The slot vacated by the in-flight submission stays reserved until the
    // MFT accepts it or the exact rejected input is returned to the queue.
    EXPECT_FALSE(scheduler.enqueue_input(5));
    EXPECT_TRUE(scheduler.retry_input_front(std::move(request->input)));
    EXPECT_FALSE(scheduler.failed());
    EXPECT_EQ(scheduler.input_count(), 4u);
}

TEST(AsyncMftSchedulerTest, ProducerWaitsForBoundedInputCapacityInsteadOfDroppingFrame) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 4, 16);
    ASSERT_TRUE(scheduler.enqueue_input(1));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    bool was_backpressured = false;
    bool enqueued = false;
    std::thread producer([&] {
        started.store(true, std::memory_order_release);
        enqueued = scheduler.enqueue_input_wait(
            2, std::chrono::seconds(1), &was_backpressured);
        completed.store(true, std::memory_order_release);
    });

    const bool producer_started = wait_until([&] { return started.load(std::memory_order_acquire); });
    EXPECT_TRUE(producer_started);
    if (!producer_started) {
        scheduler.fail(E_ABORT);
        producer.join();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(completed.load(std::memory_order_acquire));

    scheduler.grant_input(0);
    auto request = scheduler.take_input_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->input, 1);
    scheduler.mark_input_accepted();

    const bool producer_completed = wait_until([&] { return completed.load(std::memory_order_acquire); });
    EXPECT_TRUE(producer_completed);
    if (!producer_completed) scheduler.fail(E_ABORT);
    producer.join();
    EXPECT_TRUE(was_backpressured);
    EXPECT_TRUE(enqueued);
    EXPECT_EQ(scheduler.input_count(), 1u);
    EXPECT_FALSE(scheduler.failed());
}

TEST(AsyncMftSchedulerTest, TerminalFailureWakesProducerWaitingForInputCapacity) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 4, 16);
    ASSERT_TRUE(scheduler.enqueue_input(1));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    bool was_backpressured = false;
    bool enqueued = true;
    std::thread producer([&] {
        started.store(true, std::memory_order_release);
        enqueued = scheduler.enqueue_input_wait(
            2, std::chrono::seconds(1), &was_backpressured);
        completed.store(true, std::memory_order_release);
    });

    const bool producer_started = wait_until([&] { return started.load(std::memory_order_acquire); });
    EXPECT_TRUE(producer_started);
    if (!producer_started) {
        scheduler.fail(E_ABORT);
        producer.join();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler.fail(E_FAIL);

    const bool producer_completed = wait_until([&] { return completed.load(std::memory_order_acquire); });
    EXPECT_TRUE(producer_completed);
    if (!producer_completed) scheduler.fail(E_ABORT);
    producer.join();
    EXPECT_TRUE(was_backpressured);
    EXPECT_FALSE(enqueued);
    EXPECT_TRUE(scheduler.failed());
}

TEST(AsyncMftSchedulerTest, FullInputQueueWaitHasAnExplicitTimeout) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 4, 16);
    ASSERT_TRUE(scheduler.enqueue_input(1));
    bool was_backpressured = false;
    EXPECT_FALSE(scheduler.enqueue_input_wait(
        2, std::chrono::milliseconds(10), &was_backpressured));
    EXPECT_TRUE(was_backpressured);
    EXPECT_EQ(scheduler.input_count(), 1u);
    EXPECT_FALSE(scheduler.failed());
}

TEST(AsyncMftSchedulerTest, OutputQueueEnforcesPacketAndByteBoundsWithoutConsumingRejectedOutput) {
    sr::AsyncMftScheduler<int, int> scheduler(4, 4, 2, 5);
    int first = 1;
    int second = 2;
    int oversized_for_remaining_budget = 3;

    ASSERT_TRUE(scheduler.publish_output(first, 4));
    ASSERT_TRUE(scheduler.publish_output(second, 1));
    EXPECT_FALSE(scheduler.publish_output(oversized_for_remaining_budget, 1));
    EXPECT_EQ(oversized_for_remaining_budget, 3);
    EXPECT_EQ(scheduler.output_count(), 2u);
    EXPECT_EQ(scheduler.output_bytes(), 5u);

    ASSERT_TRUE(scheduler.try_pop_output().has_value());
    EXPECT_TRUE(scheduler.publish_output(oversized_for_remaining_budget, 1));
}

TEST(AsyncMftSchedulerTest, FailedShutdownCanReleaseBufferedOutputsAndCapacity) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 2, 16);
    int first = 1;
    int second = 2;
    ASSERT_TRUE(scheduler.publish_output(first, sizeof(first)));
    ASSERT_TRUE(scheduler.publish_output(second, sizeof(second)));
    EXPECT_EQ(scheduler.output_count(), 2u);
    EXPECT_EQ(scheduler.output_bytes(), 2 * sizeof(int));

    EXPECT_EQ(scheduler.discard_outputs(), 2u);
    EXPECT_EQ(scheduler.output_count(), 0u);
    EXPECT_EQ(scheduler.output_bytes(), 0u);
}

TEST(AsyncMftSchedulerTest, OutputEventsAreConsumedOneAtATimeAndWakeWorker) {
    sr::AsyncMftScheduler<int, int> scheduler;
    scheduler.grant_output();
    scheduler.grant_output();
    EXPECT_EQ(scheduler.output_events_pending(), 2u);
    EXPECT_TRUE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
    EXPECT_TRUE(scheduler.take_output_event());
    EXPECT_EQ(scheduler.output_events_pending(), 1u);
    scheduler.restore_output_event();
    EXPECT_EQ(scheduler.output_events_pending(), 2u);
}

TEST(AsyncMftSchedulerTest, OutputEventOverflowFailsInsteadOfSilentlyLosingCredit) {
    sr::AsyncMftScheduler<int, int> scheduler;
    for (int i = 0; i < 16; ++i) scheduler.grant_output();
    EXPECT_EQ(scheduler.output_events_pending(), 16u);
    scheduler.grant_output();
    EXPECT_TRUE(scheduler.failed());
}

TEST(AsyncMftSchedulerTest, OversizedSingleOutputFailsExplicitly) {
    sr::AsyncMftScheduler<int, int> scheduler(4, 4, 2, 5);
    int too_large = 8;

    EXPECT_FALSE(scheduler.publish_output(too_large, 6));
    EXPECT_EQ(too_large, 8);
    EXPECT_TRUE(scheduler.failed());
}

TEST(AsyncMftSchedulerTest, DrainWaitsUntilApplicationInputsHaveBeenSubmitted) {
    sr::AsyncMftScheduler<int, int> scheduler;
    ASSERT_TRUE(scheduler.enqueue_input(17));
    scheduler.request_drain();
    EXPECT_FALSE(scheduler.take_drain_command());

    scheduler.grant_input(0);
    auto input = scheduler.take_input_request();
    ASSERT_TRUE(input.has_value());
    scheduler.mark_input_accepted();
    EXPECT_TRUE(scheduler.take_drain_command());
    EXPECT_FALSE(scheduler.take_drain_command());
}

TEST(AsyncMftSchedulerTest, DrainCompleteAndFailureAreTerminalSignals) {
    sr::AsyncMftScheduler<int, int> drained;
    drained.request_drain();
    ASSERT_TRUE(drained.take_drain_command());
    drained.signal_drain_complete();
    EXPECT_TRUE(drained.drain_complete());
    EXPECT_FALSE(drained.enqueue_input(1));

    sr::AsyncMftScheduler<int, int> failed;
    failed.fail(E_FAIL);
    EXPECT_TRUE(failed.failed());
    EXPECT_EQ(failed.failure_reason(), E_FAIL);
    EXPECT_FALSE(failed.enqueue_input(1));
}

TEST(AsyncMftSchedulerTest, OutputProducedBeforeDrainCompleteCanBePublishedAfterEvent) {
    sr::AsyncMftScheduler<int, int> scheduler;
    scheduler.request_drain();
    ASSERT_TRUE(scheduler.take_drain_command());
    scheduler.signal_drain_complete();

    int output = 42;
    EXPECT_TRUE(scheduler.publish_output(output, sizeof(output)));
    auto drained = scheduler.try_pop_output();
    ASSERT_TRUE(drained.has_value());
    EXPECT_EQ(*drained, 42);
}

TEST(AsyncMftSchedulerTest, FullOutputQueueDoesNotCauseImmediateWorkerWakeSpin) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 1, sizeof(int));
    int queued = 7;
    ASSERT_TRUE(scheduler.publish_output(queued, sizeof(queued)));
    scheduler.grant_output();

    EXPECT_FALSE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
    ASSERT_TRUE(scheduler.try_pop_output().has_value());
    EXPECT_TRUE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
}

TEST(AsyncMftSchedulerTest, WorkerDrainSignalWakesStopWaiter) {
    sr::AsyncMftScheduler<int, int> scheduler;
    scheduler.signal_worker_drained();
    EXPECT_TRUE(scheduler.wait_for_output_or_terminal(std::chrono::milliseconds(1)));
    EXPECT_TRUE(scheduler.worker_drained());
}

TEST(AsyncMftSchedulerTest, DrainCompleteWithFullOutputQueueDoesNotSpin) {
    sr::AsyncMftScheduler<int, int> scheduler(1, 1, 1, sizeof(int));
    int output = 1;
    ASSERT_TRUE(scheduler.publish_output(output, sizeof(output)));
    scheduler.grant_output();
    scheduler.request_drain();
    ASSERT_TRUE(scheduler.take_drain_command());
    scheduler.signal_drain_complete();

    EXPECT_FALSE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
    ASSERT_TRUE(scheduler.try_pop_output().has_value());
    EXPECT_TRUE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
}

TEST(AsyncMftSchedulerTest, IndefiniteWorkerWaitIsWokenByShutdown) {
    sr::AsyncMftScheduler<int, int> scheduler;
    std::atomic<bool> returned{ false };
    std::thread worker([&] {
        scheduler.wait_for_work();
        returned.store(true, std::memory_order_release);
    });
    scheduler.stop();
    worker.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(AsyncMftSchedulerTest, QueuedInputWakesIdleWorkerOnceThenWaitsForNeedInput) {
    sr::AsyncMftScheduler<int, int> scheduler;
    std::atomic<bool> returned{ false };
    std::thread worker([&] {
        scheduler.wait_for_work();
        returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(scheduler.enqueue_input(42));
    for (int attempt = 0; attempt < 100 && !returned.load(std::memory_order_acquire); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool woke_for_input = returned.load(std::memory_order_acquire);
    EXPECT_FALSE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
    scheduler.grant_input(0);
    worker.join();
    EXPECT_TRUE(woke_for_input);
    EXPECT_TRUE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
}

TEST(AsyncMftSchedulerTest, OutputAndWorkerWaitersAreNotifiedByTheirOwnPredicates) {
    sr::AsyncMftScheduler<int, int> scheduler;
    std::atomic<bool> worker_woke{ false };
    std::atomic<bool> consumer_woke{ false };
    std::thread worker([&] {
        worker_woke.store(scheduler.wait_for_work(std::chrono::milliseconds(250)),
                          std::memory_order_release);
    });
    std::thread consumer([&] {
        consumer_woke.store(scheduler.wait_for_output_or_terminal(std::chrono::milliseconds(250)),
                            std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    int output = 7;
    ASSERT_TRUE(scheduler.publish_output(output, sizeof(output)));
    for (int attempt = 0; attempt < 100 && !consumer_woke.load(std::memory_order_acquire); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool consumer_woke_for_output = consumer_woke.load(std::memory_order_acquire);
    const bool worker_stayed_asleep = !worker_woke.load(std::memory_order_acquire);
    scheduler.stop();
    worker.join();
    consumer.join();
    EXPECT_TRUE(consumer_woke_for_output);
    EXPECT_TRUE(worker_stayed_asleep);
}

TEST(FakeAsyncMftTest, DelayedOutputsResolveMultipleAcceptedInputsInOrder) {
    sr::AsyncMftScheduler<int, int> scheduler(4, 4, 4, 16);
    FakeAsyncMft mft;
    for (int input : {10, 20, 30}) ASSERT_TRUE(scheduler.enqueue_input(input));

    for (int expected : {10, 20, 30}) {
        scheduler.grant_input(0);
        auto input = scheduler.take_input_request();
        ASSERT_TRUE(input.has_value());
        EXPECT_EQ(input->input, expected);
        ASSERT_EQ(mft.process_input(input->input), S_OK);
        scheduler.mark_input_accepted();
    }
    EXPECT_EQ(scheduler.in_flight_inputs(), 3u);

    mft.make_outputs_ready(); // Simulate delayed encoder completion.
    for (int expected : {10, 20, 30}) {
        scheduler.grant_output();
        ASSERT_TRUE(scheduler.take_output_event());
        int output = 0;
        ASSERT_TRUE(mft.process_output(output));
        EXPECT_EQ(output, expected);
        scheduler.mark_output_produced();
        ASSERT_TRUE(scheduler.publish_output(output, sizeof(output)));
    }
    EXPECT_EQ(scheduler.in_flight_inputs(), 0u);
    for (int expected : {10, 20, 30}) {
        auto output = scheduler.try_pop_output();
        ASSERT_TRUE(output.has_value());
        EXPECT_EQ(*output, expected);
    }
}

TEST(FakeAsyncMftTest, NotAcceptingRetriesSameInputOnlyAfterAnotherNeedInput) {
    sr::AsyncMftScheduler<int, int> scheduler;
    FakeAsyncMft mft;
    mft.reject_next_input();
    ASSERT_TRUE(scheduler.enqueue_input(77));
    scheduler.grant_input(0);
    auto first = scheduler.take_input_request();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(mft.process_input(first->input), MF_E_NOTACCEPTING);
    ASSERT_TRUE(scheduler.retry_input_front(std::move(first->input)));
    EXPECT_FALSE(scheduler.input_ready());

    scheduler.grant_input(0);
    auto retry = scheduler.take_input_request();
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->input, 77);
    ASSERT_EQ(mft.process_input(retry->input), S_OK);
    scheduler.mark_input_accepted();
    mft.make_outputs_ready();
    scheduler.grant_output();
    ASSERT_TRUE(scheduler.take_output_event());
    int output = 0;
    ASSERT_TRUE(mft.process_output(output));
    EXPECT_EQ(output, 77);
    scheduler.mark_output_produced();
}

TEST(FakeAsyncMftTest, OutputCreditIsHeldUntilBoundedQueueHasRoom) {
    sr::AsyncMftScheduler<int, int> scheduler(2, 2, 1, sizeof(int));
    FakeAsyncMft mft;
    ASSERT_EQ(mft.process_input(1), S_OK);
    ASSERT_EQ(mft.process_input(2), S_OK);
    mft.make_outputs_ready();
    scheduler.grant_output();
    scheduler.grant_output();

    ASSERT_TRUE(scheduler.take_output_event());
    int first = 0;
    ASSERT_TRUE(mft.process_output(first));
    ASSERT_TRUE(scheduler.publish_output(first, sizeof(first)));
    EXPECT_FALSE(scheduler.output_has_capacity());
    EXPECT_EQ(scheduler.output_events_pending(), 1u);

    auto queued = scheduler.try_pop_output();
    ASSERT_TRUE(queued.has_value());
    ASSERT_TRUE(scheduler.output_has_capacity());
    ASSERT_TRUE(scheduler.take_output_event());
    int second = 0;
    ASSERT_TRUE(mft.process_output(second));
    ASSERT_TRUE(scheduler.publish_output(second, sizeof(second)));
    EXPECT_EQ(*scheduler.try_pop_output(), 2);
}

TEST(MftAsyncEventCallbackTest, EndGetEventRearmsAndDrainCompleteStopsRearming) {
    MfTestApartment apartment;
    ASSERT_HRESULT_SUCCEEDED(apartment.initialize());

    using Scheduler = sr::AsyncMftScheduler<int, int>;
    Microsoft::WRL::ComPtr<FakeEventGenerator> generator =
        Microsoft::WRL::Make<FakeEventGenerator>();
    ASSERT_TRUE(generator);
    ASSERT_HRESULT_SUCCEEDED(generator->initialize());
    auto scheduler_owner = std::make_shared<Scheduler>();
    Scheduler& scheduler = *scheduler_owner;
    ASSERT_TRUE(scheduler.enqueue_input(31));
    Microsoft::WRL::ComPtr<sr::MftAsyncEventCallback<Scheduler>> callback =
        Microsoft::WRL::Make<sr::MftAsyncEventCallback<Scheduler>>();
    ASSERT_TRUE(callback);
    ASSERT_HRESULT_SUCCEEDED(callback->start(generator.Get(), scheduler_owner));

    auto need_input = make_event(METransformNeedInput);
    ASSERT_TRUE(need_input);
    ASSERT_HRESULT_SUCCEEDED(need_input->SetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, 9));
    ASSERT_HRESULT_SUCCEEDED(generator->queue(need_input.Get()));
    ASSERT_TRUE(wait_until([&] { return scheduler.input_ready(); }));
    auto request = scheduler.take_input_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->stream_id, 9u);
    EXPECT_EQ(request->input, 31);
    scheduler.mark_input_accepted();

    auto have_output = make_event(METransformHaveOutput);
    ASSERT_TRUE(have_output);
    ASSERT_HRESULT_SUCCEEDED(generator->queue(have_output.Get()));
    ASSERT_TRUE(wait_until([&] { return scheduler.output_events_pending() == 1; }));
    ASSERT_TRUE(scheduler.take_output_event());
    scheduler.mark_output_produced();

    scheduler.request_drain();
    ASSERT_TRUE(scheduler.take_drain_command());
    auto drain_complete = make_event(METransformDrainComplete);
    ASSERT_TRUE(drain_complete);
    ASSERT_HRESULT_SUCCEEDED(generator->queue(drain_complete.Get()));
    ASSERT_TRUE(wait_until([&] { return scheduler.drain_complete(); }));
    EXPECT_TRUE(callback->wait_quiescent(2000));
    EXPECT_EQ(scheduler.need_input_events_received(), 1u);
    EXPECT_EQ(scheduler.have_output_events_received(), 1u);

    callback->disarm();
    generator->shutdown();
}

TEST(MftAsyncEventCallbackTest, LateEventUsesOnlyItsRetainedSchedulerGeneration) {
    MfTestApartment apartment;
    ASSERT_HRESULT_SUCCEEDED(apartment.initialize());

    using Scheduler = sr::AsyncMftScheduler<int, int>;
    Microsoft::WRL::ComPtr<FakeEventGenerator> generator =
        Microsoft::WRL::Make<FakeEventGenerator>();
    ASSERT_TRUE(generator);
    ASSERT_HRESULT_SUCCEEDED(generator->initialize());
    auto old_scheduler = std::make_shared<Scheduler>();
    std::weak_ptr<Scheduler> old_generation = old_scheduler;
    auto callback = Microsoft::WRL::Make<sr::MftAsyncEventCallback<Scheduler>>();
    ASSERT_TRUE(callback);
    ASSERT_HRESULT_SUCCEEDED(callback->start(generator.Get(), old_scheduler));
    old_scheduler.reset(); // Callback ownership must keep its generation alive.
    callback->disarm();

    auto next_scheduler = std::make_shared<Scheduler>();
    auto need_input = make_event(METransformNeedInput);
    ASSERT_TRUE(need_input);
    ASSERT_HRESULT_SUCCEEDED(need_input->SetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, 0));
    ASSERT_HRESULT_SUCCEEDED(generator->queue(need_input.Get()));
    ASSERT_TRUE(wait_until([&] {
        auto old = old_generation.lock();
        return old && old->need_input_events_received() == 1;
    }));
    EXPECT_EQ(next_scheduler->need_input_events_received(), 0u);
    EXPECT_FALSE(old_generation.expired());
    EXPECT_TRUE(callback->wait_quiescent(2000));

    callback.Reset();
    EXPECT_TRUE(wait_until([&] { return old_generation.expired(); }));
    generator->shutdown();
}

TEST(AsyncMftSchedulerTest, ShutdownRejectsNewWorkAndWakesWaiters) {
    sr::AsyncMftScheduler<int, int> scheduler;
    scheduler.stop();
    EXPECT_TRUE(scheduler.stopped());
    EXPECT_FALSE(scheduler.enqueue_input(1));
    EXPECT_TRUE(scheduler.wait_for_work(std::chrono::milliseconds(1)));
}
