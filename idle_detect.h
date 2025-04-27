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

// Forward declare Wayland types
struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;

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

//!
//! \brief The WaylandIdleMonitor class implements the ext_idle_notifier_v1 protocol to monitor idle state in Wayland.
//! The intent is to properly handle idle detection in Wayland sessions other than KDE or GNOME. This class is a singleton and has
//! one instantiated thread. It is used to monitor the idle state of the Wayland session.
//!
class WaylandIdleMonitor
{
public:
    //! \brief Constructor
    WaylandIdleMonitor();

    //! \brief Destructor
    ~WaylandIdleMonitor();

    //! \brief Deleted copy and move constructors and assignment operators to prevent copying.
    WaylandIdleMonitor(const WaylandIdleMonitor&) = delete;
    WaylandIdleMonitor& operator=(const WaylandIdleMonitor&) = delete;
    WaylandIdleMonitor(WaylandIdleMonitor&&) = delete;
    WaylandIdleMonitor& operator=(WaylandIdleMonitor&&) = delete;

    //! \brief Initializes the Wayland display and registry, and starts the idle notifier.
    bool Start(int notification_timeout_ms);

    //! \brief Stops the Wayland idle monitor and cleans up resources.
    void Stop();

    //! \brief Checks if the Wayland idle monitor is available.
    bool IsAvailable() const;

    //! \brief Provides the number of seconds the session has been idle via the ext_idle_notifier_v1 protocol.
    int64_t GetIdleSeconds() const;

    //! \brief Checks if the Wayland session is idle.
    bool IsIdle() const;

public: // Accessible to static C callbacks
    wl_seat* m_seat;
    ext_idle_notifier_v1* m_idle_notifier;
    uint32_t m_seat_id;
    uint32_t m_idle_notifier_id;
    std::atomic<bool> m_is_idle;
    std::atomic<int64_t> m_idle_start_time;

private:
    //! \brief The WaylandIdleMonitor thread that monitors the Wayland session for idle state changes.
    std::thread m_monitor_thread;

    //! \brief Mutex for thread synchronization
    std::atomic<bool> m_interrupt_monitor;

    //! \brief Pipe for thread interrupt handling
    int m_interrupt_pipe_fd[2];

    //! \brief Flag to indicate whether the monitor has been initialized.
    std::atomic<bool> m_initialized;

    //! \brief Wayland display and registry objects
    wl_display* m_display;
    wl_registry* m_registry;

    //! \brief Idle notification object
    ext_idle_notification_v1* m_idle_notification;

    //! \brief Timeout for idle notification in milliseconds
    int m_notification_timeout_ms;

    //! \brief Private method to initialize Wayland and set up the idle notification.
    bool InitializeWayland();

    //! \brief Private method to clean up Wayland resources.
    void CleanupWayland();

    //! \brief Private method to create the idle notification object.
    void CreateIdleNotification();

    //! \brief Private method to run the Wayland event loop in a separate thread.
    void WaylandMonitorThread();

    //! \brief Private method to handle global events.
    static void HandleGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);

    //! \brief Private method to handle global removal events.
    static void HandleGlobalRemove(void *data, wl_registry *registry, uint32_t name);

    //! \brief Private method to handle idle notification events.
    static void HandleIdled(void *data, ext_idle_notification_v1 *notification);

    //! \brief Private method to handle resumed events.
    static void HandleResumed(void *data, ext_idle_notification_v1 *notification);

    //! \brief Pointer to the registry listener struct for C linkage.
    static const void* c_registry_listener_ptr;

    //! \brief Pointer to the idle notification listener struct for C linkage.
    static const void* c_idle_notification_listener_ptr;
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
