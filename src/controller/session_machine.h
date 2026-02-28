#pragma once
// session_machine.h — State machine for recording session
// States: Idle -> Recording -> Paused -> Stopping -> Idle

#include <cstdint>
#include <functional>
#include <string>

namespace sr {

enum class SessionState : uint8_t {
    Idle,
    Recording,
    Paused,
    Stopping
};

inline const wchar_t* state_name(SessionState s) {
    switch (s) {
        case SessionState::Idle:      return L"Idle";
        case SessionState::Recording: return L"Recording";
        case SessionState::Paused:    return L"Paused";
        case SessionState::Stopping:  return L"Stopping";
        default:                      return L"Unknown";
    }
}

enum class SessionEvent : uint8_t {
    Start,
    Stop,
    Pause,
    Resume,
    Finalized  // Stopping -> Idle (after flush completes)
};

inline const wchar_t* event_name(SessionEvent e) {
    switch (e) {
        case SessionEvent::Start:     return L"Start";
        case SessionEvent::Stop:      return L"Stop";
        case SessionEvent::Pause:     return L"Pause";
        case SessionEvent::Resume:    return L"Resume";
        case SessionEvent::Finalized: return L"Finalized";
        default:                      return L"Unknown";
    }
}

class SessionMachine {
public:
    using StateChangeCallback = std::function<void(SessionState old_state, SessionState new_state)>;

    SessionMachine() : state_(SessionState::Idle) {}

    SessionState state() const { return state_; }

    // Attempt a state transition. Returns true if valid, false if rejected
    bool transition(SessionEvent event) {
        SessionState new_state;

        switch (state_) {
        case SessionState::Idle:
            if (event == SessionEvent::Start) {
                new_state = SessionState::Recording;
            } else {
                return false; // Invalid
            }
            break;

        case SessionState::Recording:
            if (event == SessionEvent::Stop) {
                new_state = SessionState::Stopping;
            } else if (event == SessionEvent::Pause) {
                new_state = SessionState::Paused;
            } else {
                return false;
            }
            break;

        case SessionState::Paused:
            if (event == SessionEvent::Resume) {
                new_state = SessionState::Recording;
            } else if (event == SessionEvent::Stop) {
                new_state = SessionState::Stopping;
            } else {
                return false;
            }
            break;

        case SessionState::Stopping:
            if (event == SessionEvent::Finalized) {
                new_state = SessionState::Idle;
            } else {
                return false;
            }
            break;

        default:
            return false;
        }

        SessionState old_state = state_;
        state_ = new_state;
        if (on_state_change_) {
            on_state_change_(old_state, new_state);
        }
        return true;
    }

    void set_callback(StateChangeCallback cb) {
        on_state_change_ = std::move(cb);
    }

    bool is_idle()      const { return state_ == SessionState::Idle; }
    bool is_recording() const { return state_ == SessionState::Recording; }
    bool is_paused()    const { return state_ == SessionState::Paused; }
    bool is_stopping()  const { return state_ == SessionState::Stopping; }

private:
    SessionState        state_;
    StateChangeCallback on_state_change_;
};

} // namespace sr
