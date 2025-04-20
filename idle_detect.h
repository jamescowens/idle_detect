/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_DETECT_H
#define IDLE_DETECT_H

#include <condition_variable>
#include <cstdint> // For int64_t
#include <thread>
#include <util.h>

namespace IdleDetect {
// Function to get the user's idle time in seconds
int64_t GetIdleTimeSeconds();

//!
//! \brief The IdleDetectControlMonitor class is a singleton that monitors the idle_detect control pipe. It is used to
//! allow the override of the state of the idle_detect monitor. It is a singleton and has one instantiated thread. Note
//! that a message of the override is propagated to event_detect.
//!
class IdleDetectControlMonitor
{
public:
    enum State {
        UNKNOWN,
        NORMAL,
        FORCED_ACTIVE,
        FORCED_IDLE
    };

    //!
    //! \brief Holds the actual idle_detect monitor thread.
    //!
    std::thread m_idle_detect_control_monitor_thread;

    //!
    //! \brief Condition variable for control/synchronization of the idle_detect monitor threads.
    //!
    std::condition_variable cv_idle_detect_control_monitor_thread;

    //!
    //! \brief Atomic boolean that interrupts the idle_detect monitor thread.
    //!
    std::atomic<bool> m_interrupt_idle_detect_control_monitor;

    //! Constructor.
    IdleDetectControlMonitor();

    //!
    //! \brief Method to instantiate the tty monitor thread.
    //!
    void IdleDetectControlMonitorThread();

    //!
    //! \brief Provides a flag to indicate whether the monitor has been initialized. This is used in main in the application
    //! control paths.
    //! \return boolean flag
    //!
    bool IsInitialized() const;

    //!
    //! \brief Returns the state of the idle control monitor. This is NORMAL, FORCED_ACTIVE or FORCED_IDLE.
    //! \return State enum value
    //!
    State GetState() const;

    //!
    //! \brief Returns the string representation of the input state enum value.
    //! \param State enum state
    //! \return string representation of the state
    //!
    static std::string StateToString(const State& state);

    //!
    //! \brief Returns the string representation of the idle monitor object state.
    //! \return string represenation of the state
    //!
    std::string StateToString() const;

private:
    //!
    //! \brief This is the mutex member that provides lock control for the tty monitor object. This is used to ensure the
    //! tty monitor is thread-safe.
    //!
    mutable std::mutex mtx_idle_detect_control_monitor;

    //!
    //! \brief This provides lock control for the tty monitor worker thread itself.
    //!
    mutable std::mutex mtx_idle_detect_control_monitor_thread;

    //!
    //! \brief Holds the current state of the idle monitor. NORMAL means idle detect follows the normal threshold (trigger) rules
    //! for idle detection. FORCED_ACTIVE means the user has forced the system to be active and FORCED_IDLE means the user has
    //! forced the system to be idle. The state is set by the event_detect process and is used to determine the ultimate
    //! last active time.
    //!
    std::atomic<State> m_state;

    //!
    //! \brief This holds the flag as to whether the tty monitor has been initialized and is provided by the IsInitialized() public
    //! method.
    //!
    std::atomic<bool> m_initialized;
};
} // namespace IdleDetect

//!
//! \brief The IdleDetectConfig class. This specializes the Config class and implements the virtual method ProcessArgs()
//! for idle_detect.
//!
class IdleDetectConfig : public Config
{
    //!
    //! \brief The is the ProcessArgs() implementation for idle_detect.
    //!
    void ProcessArgs() override;
};

#endif // IDLE_DETECT_H
