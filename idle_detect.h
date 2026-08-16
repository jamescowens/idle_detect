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

//!
//! \brief Resolves the local session's idle time from every live idle source.
//!
//! This is a thin delegation to the global IdleSourcePool, which owns the sources and is reconciled against
//! discovery by main(). It deliberately makes no determination of its own about what kind of session this is:
//! the session typing this replaced read getenv("DISPLAY") and getenv("WAYLAND_DISPLAY"), which are frozen at
//! exec and therefore cannot describe a GUI session that appears after the daemon starts.
//!
//! \return Idle seconds >= 0, IDLE_ERROR (-1) if sources exist but none could be read, or IDLE_NO_GUI_SESSION
//!         (-2) if there is no GUI session at all, which instructs the caller to defer to event_detect
//!         regardless of the use_event_detect config setting.
//!
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
//! \brief XOpenDisplay retry budget that reproduces the historical behavior: six attempts, 500 ms apart, for
//! up to 2.5 seconds of waiting.
//!
//! That budget was sized for one connection to the process's own DISPLAY at startup. It is deliberately NOT
//! the budget a per-endpoint resolver should use; see X11IdleSource::ResolveIdleSeconds().
//!
constexpr int X_STARTUP_CONNECT_RETRIES = 6;

//!
//! \brief Queries XScreenSaver for the idle time of a specific X display.
//!
//! The connection is opened and closed within the call, so this is stateless and cannot go stale.
//!
//! The retry budget is a parameter because the cost of an unreachable display depends on the caller. A single
//! startup query against the process's own DISPLAY can afford to wait out an X server that is still starting.
//! A resolver run against N discovered displays on every reconcile tick cannot: the call is synchronous, so
//! one dead display stalls every other endpoint behind it, every tick.
//!
//! A display dying at any point during the call, including between the connect and the query, yields -1 and
//! does NOT terminate the process. Xlib's defaults do terminate it, so this call installs replacement X error
//! handlers on first use; see the X error handling commentary in idle_detect.cpp for what those handlers must
//! do and why one hook is not enough. The XScreenSaver Status return is checked as part of this, because a
//! query that failed leaves its info buffer unwritten and the zeroed buffer would otherwise be published as a
//! genuine "idle 0 seconds".
//!
//! \param display X display string, e.g. ":1". Empty uses the DISPLAY environment variable, which is the
//!        historical behavior.
//! \param max_connect_retries XOpenDisplay attempts before giving up. Values below one are treated as one.
//!        Defaults to the historical startup budget so existing callers are unaffected.
//! \return Idle seconds >= 0, or -1 on error.
//!
int64_t GetIdleTimeXss(const std::string& display, int max_connect_retries = X_STARTUP_CONNECT_RETRIES);

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
//! \brief Wayland initialization retry budget that reproduces the historical startup behavior: fifteen
//! attempts, two seconds apart, for roughly thirty seconds of waiting.
//!
//! That budget was sized for the single startup-time connection to the process's own session, where a
//! compositor still coming up is worth waiting out. It is deliberately NOT the budget a per-candidate
//! validator should use; see WaylandIdleSource::Start() in idle_sources_system.h.
//!
constexpr int WAYLAND_STARTUP_INIT_RETRIES = 15;

//!
//! \brief Idle notification threshold, in milliseconds, requested from ext_idle_notifier_v1 by every Wayland
//! idle source.
//!
//! This is the resolution of the Wayland idle reading rather than a policy threshold: the compositor notifies
//! once this much time has passed without input, and the monitor converts that notification into an idle time.
//! The inactivity trigger the user configures is applied by the main loop, far downstream of this.
//!
//! It is a constant rather than a config value because the pool that hands it to each new source is
//! constructed before any config file has been read.
//!
constexpr int WAYLAND_IDLE_NOTIFICATION_TIMEOUT_MS = 1000;

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
    //! The retry budget is a parameter rather than a constant because the cost of a failed Start() depends
    //! entirely on the caller. A startup connection to the process's own session can afford to wait; a
    //! validator deciding whether a discovered candidate is a usable endpoint cannot, because it blocks every
    //! other candidate behind it and repeats the wait on every reconcile tick.
    //!
    //! \param socket_name Wayland socket to connect to (e.g. "wayland-0"). Empty means use the WAYLAND_DISPLAY
    //!        environment variable.
    //! \param notification_timeout_ms Idle notification threshold in milliseconds.
    //! \param max_init_retries Number of connect-and-bind attempts before giving up. Values below one are
    //!        treated as one. Defaults to the historical startup budget so existing callers are unaffected.
    //! \return true if the monitor was started (or was already running), false on failure.
    //!
    bool Start(const std::string& socket_name,
               int notification_timeout_ms,
               int max_init_retries = WAYLAND_STARTUP_INIT_RETRIES);

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
    //! \param max_init_retries Retry budget to hand to InitializeWayland(). Passed through rather than stored,
    //!        since nothing after initialization needs it.
    //! \return true if the monitor thread was started, false on any failure.
    //!
    bool StartInternal(int max_init_retries);

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

    //!
    //! \brief Private method to initialize Wayland and set up the idle notification.
    //!
    //! Each attempt connects to the socket, binds the registry, and performs two roundtrips looking for
    //! wl_seat and ext_idle_notifier_v1. Failing to find the globals is retried the same way a failed connect
    //! is, because a compositor that has not yet advertised them is indistinguishable from one that never
    //! will. That is precisely why the budget belongs to the caller: a compositor which does not implement
    //! ext_idle_notifier_v1 at all connects successfully and roundtrips successfully on every attempt, so the
    //! full budget is spent every single time.
    //!
    //! The inter-attempt wait polls g_shutdown_requested every 100 ms and aborts on it.
    //!
    //! \param max_retries Number of attempts to make. Values below one are treated as one.
    //! \return true if the connection was established and both required globals were bound.
    //!
    bool InitializeWayland(int max_retries);

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
