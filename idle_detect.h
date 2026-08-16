/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_DETECT_H
#define IDLE_DETECT_H

#include <condition_variable>
#include <cstdint> // For int64_t
#include <string>
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
//! \brief Queries KDE ksmserver for session idle time via the org.freedesktop.ScreenSaver interface.
//!
//! ksmserver handles inhibition internally by periodically resetting the idle time it reports, so callers
//! must NOT add a separate inhibition check on top of this value. Note that Plasma 6 removed
//! GetSessionIdleTime on Wayland, where this call returns an error rather than a value.
//!
//! \return Idle seconds >= 0, or -1 on error.
//!
int64_t GetIdleTimeKdeDBus();

//!
//! \brief Queries GNOME Mutter's IdleMonitor for input idle time. Works on both GNOME X11 and Wayland.
//!
//! Unlike the KDE call above this does NOT account for idle inhibition, which must be checked separately
//! with CheckGnomeInhibition().
//!
//! \return Idle seconds >= 0, or -1 on error.
//!
int64_t GetIdleTimeWaylandGnomeViaDBus();

//!
//! \brief Checks whether KDE reports screen idle inhibition via the PowerManagement PolicyAgent.
//! \return true if screen idle is inhibited, false otherwise (including on D-Bus errors).
//!
bool CheckKdeInhibition();

//!
//! \brief Checks whether GNOME reports session idle inhibition. Works on both GNOME X11 and Wayland.
//! \return true if inhibited, false otherwise (including on D-Bus errors).
//!
bool CheckGnomeInhibition();

//!
//! \brief Queries XScreenSaver for the idle time of a specific X display.
//!
//! The connection is opened and closed within the call, so this is stateless and cannot go stale.
//!
//! \param display X display string, e.g. ":1". Empty uses the DISPLAY environment variable, which is the
//!        historical behavior.
//! \return Idle seconds >= 0, or -1 on error.
//!
int64_t GetIdleTimeXss(const std::string& display);

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

    //!
    //! \brief Initializes the Wayland display and registry, and starts the idle notifier.
    //!
    //! The socket name binds this monitor to one specific compositor endpoint, which is what allows more than
    //! one monitor to coexist in the same process. An empty name is the historical behavior: the socket is
    //! derived from the WAYLAND_DISPLAY environment variable by libwayland itself.
    //!
    //! \param socket_name Wayland socket to connect to (e.g. "wayland-0"). Empty means use the WAYLAND_DISPLAY
    //!        environment variable.
    //! \param notification_timeout_ms Idle notification threshold in milliseconds.
    //! \return true if the monitor was started (or was already running), false on failure.
    //!
    bool Start(const std::string& socket_name, int notification_timeout_ms);

    //! \brief Stops the Wayland idle monitor and cleans up resources.
    void Stop();

    //! \brief Checks if the Wayland idle monitor is available.
    bool IsAvailable() const;

    //!
    //! \brief Returns the Wayland socket this monitor is bound to.
    //!
    //! This is the name that was passed to the most recent Start(). It is empty if the monitor has never been
    //! started, or if it was started with an empty name and is therefore bound to the environment-derived
    //! socket.
    //!
    //! \return Socket name, empty if environment-derived.
    //!
    const std::string& GetSocketName() const;

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

    //!
    //! \brief Handles the compositor removing a global that this monitor depends on. Destroys the affected
    //! proxy and flags the monitor as failed so the connection is rebuilt.
    //!
    //! This is called from the registry listener's global_remove callback, which runs on the monitor thread
    //! during event dispatch and on the main thread during the roundtrips in InitializeWayland().
    //!
    //! \param name The registry name of the removed global.
    //!
    void OnGlobalRemoved(uint32_t name);

private:
    //! \brief The WaylandIdleMonitor thread that monitors the Wayland session for idle state changes.
    std::thread m_monitor_thread;

    //! \brief Mutex for thread synchronization
    std::atomic<bool> m_interrupt_monitor;

    //! \brief Pipe for thread interrupt handling
    int m_interrupt_pipe_fd[2];

    //! \brief Flag to indicate whether the monitor has been initialized.
    std::atomic<bool> m_initialized;

    //!
    //! \brief Flag set when the compositor removes a global the monitor depends on.
    //!
    //! This is deliberately separate from m_interrupt_monitor: m_globals_lost means the monitor has failed and
    //! its Wayland connection must be rebuilt, whereas m_interrupt_monitor means the monitor was asked to stop.
    //!
    std::atomic<bool> m_globals_lost;

    //! \brief Wayland display and registry objects
    wl_display* m_display;
    wl_registry* m_registry;

    //! \brief Idle notification object
    ext_idle_notification_v1* m_idle_notification;

    //! \brief Timeout for idle notification in milliseconds
    int m_notification_timeout_ms;

    //!
    //! \brief Wayland socket name this instance connects to. Empty means use WAYLAND_DISPLAY.
    //!
    //! Stored by Start() before any fallible work and deliberately left alone by ReapFailedThread(), so that
    //! reaping a self-exited thread and restarting reconnects to the same endpoint rather than falling back to
    //! whatever WAYLAND_DISPLAY happens to say.
    //!
    std::string m_socket_name;

    //!
    //! \brief Private method that performs the fallible portion of Start(): initializing Wayland, validating the
    //! required interfaces, creating the idle notification object, and launching the monitor thread.
    //!
    //! This method performs no cleanup of its own. Start() owns the teardown of both the Wayland resources and the
    //! interrupt pipe on failure. m_notification_timeout_ms must already be stored by Start() before this is called.
    //!
    //! It clears m_globals_lost immediately before launching the monitor thread, so that global churn during
    //! initialization cannot make the thread exit on its first iteration.
    //!
    //! \return true if the monitor thread was started, false on any failure.
    //!
    bool StartInternal();

    //!
    //! \brief Private method that reaps a monitor thread which exited on its own, returning the object to the
    //! state it has before a first-ever Start().
    //!
    //! A monitor thread that exits unexpectedly clears m_initialized itself, but it cannot join itself or close
    //! the interrupt pipe, so it leaves m_monitor_thread joinable and both pipe FDs open. Start() must therefore
    //! call this before it does anything else: re-running Start() over that state would leak the pipe FDs and
    //! move-assign onto a joinable std::thread, which calls std::terminate(). It is a no-op when there is no
    //! thread to reap, which is the ordinary first-ever Start() path.
    //!
    //! \return true if the object is ready for a fresh Start(), false if a previous thread could not be reaped
    //! and its resources must therefore be left alone.
    //!
    bool ReapFailedThread();

    //! \brief Private method to initialize Wayland and set up the idle notification.
    bool InitializeWayland();

    //!
    //! \brief Private method to destroy (or release, depending on the bound version) the wl_seat proxy and
    //! clear the cached pointer. Safe to call when no seat is bound.
    //!
    void DestroySeat();

    //! \brief Private method to clean up Wayland resources.
    void CleanupWayland();

    //! \brief Private method to clear cached Wayland object pointers and IDs without destroying them.
    void ResetWaylandState();

    //! \brief Private method to create the idle notification object.
    void CreateIdleNotification();

    //!
    //! \brief Prepares the Wayland read queue, dispatching pending events until the queue is locked.
    //!
    //! \return true on success, false if dispatch failed and the monitor thread should exit.
    //!
    bool PrepareRead();

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
