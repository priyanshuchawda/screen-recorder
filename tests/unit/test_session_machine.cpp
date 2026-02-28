// test_session_machine.cpp — Unit tests for SessionMachine (T007)
// Covers all valid transitions and invalid transition rejections

#include <gtest/gtest.h>
#include "controller/session_machine.h"

using sr::SessionMachine;
using sr::SessionState;
using sr::SessionEvent;

class SessionMachineTest : public ::testing::Test {
protected:
    SessionMachine machine;
};

// T007: Valid transitions

TEST_F(SessionMachineTest, StartsInIdle) {
    EXPECT_EQ(machine.state(), SessionState::Idle);
    EXPECT_TRUE(machine.is_idle());
}

TEST_F(SessionMachineTest, IdleToRecording) {
    EXPECT_TRUE(machine.transition(SessionEvent::Start));
    EXPECT_EQ(machine.state(), SessionState::Recording);
}

TEST_F(SessionMachineTest, RecordingToPaused) {
    machine.transition(SessionEvent::Start);
    EXPECT_TRUE(machine.transition(SessionEvent::Pause));
    EXPECT_EQ(machine.state(), SessionState::Paused);
}

TEST_F(SessionMachineTest, PausedToRecording) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Pause);
    EXPECT_TRUE(machine.transition(SessionEvent::Resume));
    EXPECT_EQ(machine.state(), SessionState::Recording);
}

TEST_F(SessionMachineTest, RecordingToStopping) {
    machine.transition(SessionEvent::Start);
    EXPECT_TRUE(machine.transition(SessionEvent::Stop));
    EXPECT_EQ(machine.state(), SessionState::Stopping);
}

TEST_F(SessionMachineTest, PausedToStopping) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Pause);
    EXPECT_TRUE(machine.transition(SessionEvent::Stop));
    EXPECT_EQ(machine.state(), SessionState::Stopping);
}

TEST_F(SessionMachineTest, StoppingToIdle) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Stop);
    EXPECT_TRUE(machine.transition(SessionEvent::Finalized));
    EXPECT_EQ(machine.state(), SessionState::Idle);
}

TEST_F(SessionMachineTest, FullCycle) {
    // Idle -> Recording -> Paused -> Recording -> Stopping -> Idle
    EXPECT_TRUE(machine.transition(SessionEvent::Start));
    EXPECT_TRUE(machine.transition(SessionEvent::Pause));
    EXPECT_TRUE(machine.transition(SessionEvent::Resume));
    EXPECT_TRUE(machine.transition(SessionEvent::Stop));
    EXPECT_TRUE(machine.transition(SessionEvent::Finalized));
    EXPECT_TRUE(machine.is_idle());
}

// T007: Invalid transitions (rejected)

TEST_F(SessionMachineTest, IdleCannotPause) {
    EXPECT_FALSE(machine.transition(SessionEvent::Pause));
    EXPECT_EQ(machine.state(), SessionState::Idle);
}

TEST_F(SessionMachineTest, IdleCannotStop) {
    EXPECT_FALSE(machine.transition(SessionEvent::Stop));
    EXPECT_EQ(machine.state(), SessionState::Idle);
}

TEST_F(SessionMachineTest, IdleCannotResume) {
    EXPECT_FALSE(machine.transition(SessionEvent::Resume));
    EXPECT_EQ(machine.state(), SessionState::Idle);
}

TEST_F(SessionMachineTest, IdleCannotFinalize) {
    EXPECT_FALSE(machine.transition(SessionEvent::Finalized));
    EXPECT_EQ(machine.state(), SessionState::Idle);
}

TEST_F(SessionMachineTest, RecordingCannotStart) {
    machine.transition(SessionEvent::Start);
    EXPECT_FALSE(machine.transition(SessionEvent::Start));
    EXPECT_EQ(machine.state(), SessionState::Recording);
}

TEST_F(SessionMachineTest, RecordingCannotResume) {
    machine.transition(SessionEvent::Start);
    EXPECT_FALSE(machine.transition(SessionEvent::Resume));
    EXPECT_EQ(machine.state(), SessionState::Recording);
}

TEST_F(SessionMachineTest, PausedCannotStart) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Pause);
    EXPECT_FALSE(machine.transition(SessionEvent::Start));
    EXPECT_EQ(machine.state(), SessionState::Paused);
}

TEST_F(SessionMachineTest, PausedCannotPause) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Pause);
    EXPECT_FALSE(machine.transition(SessionEvent::Pause));
    EXPECT_EQ(machine.state(), SessionState::Paused);
}

TEST_F(SessionMachineTest, StoppingCannotStart) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Stop);
    EXPECT_FALSE(machine.transition(SessionEvent::Start));
    EXPECT_EQ(machine.state(), SessionState::Stopping);
}

TEST_F(SessionMachineTest, StoppingCannotStop) {
    machine.transition(SessionEvent::Start);
    machine.transition(SessionEvent::Stop);
    EXPECT_FALSE(machine.transition(SessionEvent::Stop));
    EXPECT_EQ(machine.state(), SessionState::Stopping);
}

// T007: State change callback

TEST_F(SessionMachineTest, CallbackFiredOnTransition) {
    SessionState captured_old = SessionState::Idle;
    SessionState captured_new = SessionState::Idle;
    int callback_count = 0;

    machine.set_callback([&](SessionState old_s, SessionState new_s) {
        captured_old = old_s;
        captured_new = new_s;
        callback_count++;
    });

    machine.transition(SessionEvent::Start);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(captured_old, SessionState::Idle);
    EXPECT_EQ(captured_new, SessionState::Recording);
}

TEST_F(SessionMachineTest, CallbackNotFiredOnInvalidTransition) {
    int callback_count = 0;
    machine.set_callback([&](SessionState, SessionState) {
        callback_count++;
    });

    machine.transition(SessionEvent::Stop); // Invalid from Idle
    EXPECT_EQ(callback_count, 0);
}
