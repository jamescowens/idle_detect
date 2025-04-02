/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_DETECT_H
#define IDLE_DETECT_H

#include <cstdint> // For int64_t

#include <thread>
#include <util.h>


// Forward declare Wayland types to minimize header dependencies
struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;

namespace IdleDetect {

// Global fixed or default config parameter(s).
int DEFAULT_IDLE_THRESHOLD_SECONDS = 0; // This is set in main from config, and config has the default value.
constexpr int DEFAULT_CHECK_INTERVAL_SECONDS = 1;

// Function to get the user's idle time in seconds
int64_t GetIdleTimeSeconds();

// --- Wayland Idle Monitor Class ---
class WaylandIdleMonitor
{
public:
    WaylandIdleMonitor();
    ~WaylandIdleMonitor();

    // Delete copy/move operations for safety with threads and resources
    WaylandIdleMonitor(const WaylandIdleMonitor&) = delete;
    WaylandIdleMonitor& operator=(const WaylandIdleMonitor&) = delete;
    WaylandIdleMonitor(WaylandIdleMonitor&&) = delete;
    WaylandIdleMonitor& operator=(WaylandIdleMonitor&&) = delete;

    /**
     * @brief Initializes Wayland connection and starts the monitor thread.
     * Needs to be called only if IsWaylandSession() is true.
     * @param notification_timeout_ms Timeout for ext-idle-notify-v1 in milliseconds.
     * @return True if initialization succeeded and protocol is available, false otherwise.
     */
    bool Start(int notification_timeout_ms);

    /**
     * @brief Stops the monitor thread and cleans up Wayland resources.
     */
    void Stop();

    /**
     * @brief Checks if the monitor was successfully initialized and the protocol is available.
     * @return True if available, false otherwise.
     */
    bool IsAvailable() const;

    /**
     * @brief Calculates the current idle time based on monitored state.
     * @return Current idle time in seconds, or 0 if active or unavailable.
     */
    int64_t GetIdleSeconds() const;

    /**
     * @brief Gets the raw idle state.
     * @return True if currently considered idle by the monitor, false otherwise.
     */
    bool IsIdle() const;

public: // <--- Make accessible to static C callbacks
    // Wayland Objects needed by callbacks
    wl_seat* m_seat;
    ext_idle_notifier_v1* m_idle_notifier; // The manager object
    uint32_t m_seat_id;
    uint32_t m_idle_notifier_id;
    // State needed by callbacks
    std::atomic<bool> m_is_idle;
    std::atomic<int64_t> m_idle_start_time; // Unix timestamp

private:
    // --- Threading ---
    std::thread m_monitor_thread;
    // std::condition_variable m_cv_monitor_thread; // Using pipe for interrupt instead
    // std::mutex m_mtx_monitor_thread;             // Using pipe for interrupt instead
    std::atomic<bool> m_interrupt_monitor;
    int m_interrupt_pipe_fd[2]; // Pipe FDs for interrupting poll() [read_end, write_end]

    // --- State ---
    std::atomic<bool> m_initialized; // If Wayland connection and protocol setup succeeded

    // --- Wayland Objects ---
    wl_display* m_display;
    wl_registry* m_registry;
    ext_idle_notification_v1* m_idle_notification; // Our specific notification request
   int m_notification_timeout_ms;

    // --- Wayland Setup/Teardown ---
    bool InitializeWayland();
    void CleanupWayland();
    void CreateIdleNotification(); // Helper

    // --- Thread Function ---
    void WaylandMonitorThread();

    // --- Static Wayland Listener Callbacks (implementation detail in .cpp) ---
    static void HandleGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void HandleGlobalRemove(void *data, wl_registry *registry, uint32_t name);
    static void HandleIdled(void *data, ext_idle_notification_v1 *notification);
    static void HandleResumed(void *data, ext_idle_notification_v1 *notification);

    // Pointers to the static listener structs (defined in .cpp)
    static const void* c_registry_listener_ptr;
    static const void* c_idle_notification_listener_ptr;
};

} // namespace IdleDetect

//!
//! \brief The IdleDetectConfig class. This specializes the Config class and implements the virtual method ProcessArgs()
//! for idle_detect.
//!
class IdleDetectConfig : public Config
{
    void ProcessArgs() override;
};

#endif // IDLE_DETECT_H
