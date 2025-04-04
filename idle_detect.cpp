/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include "idle_detect.h"
#include "util.h" // Includes tinyformat.h, filesystem, etc.

// Standard Libs
#include <cstdlib>     // For getenv(), system()
#include <cstdint>     // For int64_t, uint64_t
#include <cstring>     // For strcmp, strerror
#include <chrono>      // For std::chrono
#include <thread>      // For std::this_thread, std::thread
#include <atomic>      // For std::atomic
#include <fstream>     // For std::ofstream
#include <future>      // For std::async, std::future
#include <system_error>// For std::error_code
#include <csignal>     // For signal handling (sigaction etc)
#include <variant>     // For std::get
#include <algorithm>   // For std::min, std::max

// Platform Specific Libs
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>    // For pipe, read, write, close, getenv, sleep
#include <sys/types.h> // Usually included by others
#include <sys/wait.h>  // Usually included by others
#include <poll.h>      // For poll()
#include <fcntl.h>     // For O_NONBLOCK, O_CLOEXEC

// Wayland Libs
#include <wayland-client.h>
// This is the correct include now that CMake adds the build dir to include paths
#include "ext-idle-notify-v1-protocol.h"

// --- Globals ---
//! Populated by main after reading config
std::atomic<bool> g_debug = false;
//! Global config
IdleDetectConfig g_config;
//! Global Wayland idle monitor
IdleDetect::WaylandIdleMonitor g_wayland_idle_monitor;
//! DBus idle inhibit monitor
IdleDetect::DBusInhibitMonitor g_dbus_inhibit_monitor;

//! Global flag for signal handling
std::atomic<bool> g_shutdown_requested = false;

void IdleDetectConfig::ProcessArgs()
{
    // debug

    std::string debug_arg = GetArgString("debug", "true");

    if (debug_arg == "1" || ToLower(debug_arg) == "true") {
        m_config.insert(std::make_pair("debug", true));
    } else if (debug_arg == "0" || ToLower(debug_arg) == "false") {
        m_config.insert(std::make_pair("debug", false));
    } else {
        error_log("%s: debug parameter in config file has invalid value: %s",
                  __func__,
                  debug_arg);
    }

    // initial_sleep

    int initial_sleep = 0;

    try {
        initial_sleep = ParseStringToInt(GetArgString("initial_sleep", "0"));
    } catch (std::exception& e) {
        error_log("%s: startup_delay parameter in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    m_config.insert(std::make_pair("initial_sleep", initial_sleep));

    // event_count_files_path

    fs::path event_data_path;

    try {
        event_data_path = fs::path(GetArgString("event_count_files_path", "/run/event_detect"));
    } catch (std::exception& e){
        error_log("%s: event_count_files_path parameter in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    m_config.insert(std::make_pair("event_count_files_path", event_data_path));

    // use_event_detect

    std::string use_event_detect_arg = GetArgString("use_event_detect", "true");

    if (use_event_detect_arg == "1" || ToLower(use_event_detect_arg) == "true") {
        m_config.insert(std::make_pair("use_event_detect", true));
    } else if (use_event_detect_arg == "0" || ToLower(use_event_detect_arg) == "false") {
        m_config.insert(std::make_pair("use_event_detect", false));
    } else {
        error_log("%s: debug parameter in config file has invalid value: %s",
                  __func__,
                  use_event_detect_arg);
    }

    // update_event_detect

    std::string update_event_detect_arg = GetArgString("update_event_detect", "true");

    if (update_event_detect_arg == "1" || ToLower(update_event_detect_arg) == "true") {
        m_config.insert(std::make_pair("update_event_detect", true));
    } else if (update_event_detect_arg == "0" || ToLower(update_event_detect_arg) == "false") {
        m_config.insert(std::make_pair("update_event_detect", false));
    } else {
        error_log("%s: debug parameter in config file has invalid value: %s",
                  __func__,
                  update_event_detect_arg);
    }

    // execute_dc_control_scripts

    std::string execute_dc_control_scripts_arg = GetArgString("execute_dc_control_scripts", "true");

    if (execute_dc_control_scripts_arg == "1" || ToLower(execute_dc_control_scripts_arg) == "true") {
        m_config.insert(std::make_pair("execute_dc_control_scripts", true));
    } else if (execute_dc_control_scripts_arg == "0" || ToLower(execute_dc_control_scripts_arg) == "false") {
        m_config.insert(std::make_pair("execute_dc_control_scripts", false));
    } else {
        error_log("%s: debug parameter in config file has invalid value: %s",
                  __func__,
                  execute_dc_control_scripts_arg);
    }

    // last_active_time_cpp_filename

    std::string last_active_time_cpp_filename = GetArgString("last_active_time_cpp_filename", "last_active_time.dat");

    m_config.insert(std::make_pair("last_active_time_cpp_filename", last_active_time_cpp_filename));

    // inactivity_time_trigger

    int inactivity_time_trigger = 0;

    try {
        inactivity_time_trigger = ParseStringToInt(GetArgString("inactivity_time_trigger", "300"));
    } catch (std::exception& e) {
        error_log("%s: startup_delay parameter in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    m_config.insert(std::make_pair("inactivity_time_trigger", inactivity_time_trigger));

    // active_command

    m_config.insert(std::make_pair("active_command", GetArgString("active_command", "")));

    // idle_command

    m_config.insert(std::make_pair("idle_command", GetArgString("idle_command", "")));
}


namespace IdleDetect {

static bool IsWaylandSession() {
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    return (waylandDisplay != nullptr && strlen(waylandDisplay) > 0);
}

static bool IsGnomeSession() {
    const char* gnomeSession = getenv("GNOME_DESKTOP_SESSION_ID");
    return (gnomeSession != nullptr && strlen(gnomeSession) > 0);
}

/**
 * @brief Reads the timestamp from event_detect's data file.
 * @param file_path Path to the last_active_time.dat file.
 * @return int64_t Timestamp read from file, or 0 if file doesn't exist or error occurs.
 */
static int64_t ReadLastActiveTimeFile(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        debug_log("INFO: %s: Data file not found: %s", __func__, file_path.string());
        return 0;
    }

    std::ifstream time_file(file_path);
    if (!time_file.is_open()) {
        error_log("%s: Could not open data file: %s", __func__, file_path.string());
        return 0;
    }

    std::string line;
    if (std::getline(time_file, line)) {
        try {
            // Use existing parsing utility which includes validation range checks
            int64_t timestamp = ParseStringtoInt64(TrimString(line));
            debug_log("INFO: %s: Read timestamp %lld from %s", __func__, timestamp, file_path.string());
            return timestamp;
        } catch (const std::exception& e) {
            error_log("%s: Failed to parse timestamp from data file '%s': %s", __func__, file_path.string(), e.what());
            return 0;
        }
    } else {
        error_log("%s: Failed to read line from data file: %s", __func__, file_path.string());
        return 0;
    }
}

/**
 * @brief Sends a notification message to the event_detect named pipe.
 *
 * @param pipe_path The full path to the event_detect named pipe.
 * @param the last active time to send to event_detect
 */
void SendPipeNotification(const std::filesystem::path& pipe_path, const int64_t& last_active_time) {
    // Construct the message payload using EventMessage format
    EventMessage msg(::ToString(last_active_time), "USER_ACTIVE");
    if (!msg.IsValid()) {
        error_log("%s: Failed to construct valid EventMessage.",
                  __func__);
        return;
    }

    std::string message_str = msg.ToString() + "\n"; // Assuming ToString is a method, renamed

    debug_log("INFO: %s: Attempting to send message: %s",
              __func__,
              TrimString(message_str));

    std::error_code error_code;
    if (!std::filesystem::exists(pipe_path, error_code) || error_code) {
        error_log("%s: Pipe '%s' does not exist or cannot be accessed. Is event_detect running?",
                  __func__,
                  pipe_path);
        return;
    }

    if (!std::filesystem::is_fifo(pipe_path, error_code) || error_code) {
        error_log("%s: Path '%s' is not a named pipe (FIFO).",
                  __func__,
                  pipe_path);
        return;
    }

    // variable_name is snake_case
    std::ofstream pipe_stream(pipe_path, std::ios::out);

    if (!pipe_stream.is_open()) {
        error_log("%s: Failed to open pipe '%s' for writing: %s",
                  __func__,
                  pipe_path,
                  strerror(errno));
        return;
    }

    pipe_stream << message_str;
    pipe_stream.flush();

    if (pipe_stream.fail()) {
        error_log("%s: Failed to write message to pipe '%s'. Pipe full or other error?",
                  __func__,
                  pipe_path);
    } else {
        debug_log("INFO: %s: Sent message to pipe '%s'.",
                  __func__,
                  pipe_path.string().c_str());
    }
}

// Helper to check for non-graphical TTY session
static bool IsTtySession() {
    const char* display = getenv("DISPLAY");
    const char* wayland_display = getenv("WAYLAND_DISPLAY");

    // If neither display variable is set, likely a TTY.
    return (display == nullptr || strlen(display) == 0) &&
           (wayland_display == nullptr || strlen(wayland_display) == 0);
}

// D-Bus implementation for Gnome
static int64_t GetIdleTimeWaylandGnomeViaDBus() {
    // Assumes IsGnomeSession() has already confirmed this is appropriate to call
    debug_log("INFO: %s: Querying GNOME Mutter IdleMonitor via D-Bus.",
              __func__);

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    int64_t idle_time_seconds = 0; // Default to 0 (active)

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            error_log("%s: Error connecting to session bus for Gnome idle query: %s",
                      __func__,
                      dbus_error->message);
            g_error_free(dbus_error);
        } else {
            error_log("%s: Error connecting to session bus for Gnome idle query (unknown error).",
                      __func__);
        }
        return 0; // Return 0 on connection error
    }

    // D-Bus details for getting idle time from Mutter
    const char* service_name = "org.gnome.Mutter.IdleMonitor";
    const char* object_path = "/org/gnome/Mutter/IdleMonitor/Core";
    const char* interface_name = "org.gnome.Mutter.IdleMonitor";
    const char* method_name = "GetIdletime";

    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name, object_path, interface_name, method_name,
                                              nullptr, G_VARIANT_TYPE("(t)"), G_DBUS_CALL_FLAGS_NONE,
                                              500, nullptr, &dbus_error);

    if (dbus_error) {
        error_log("%s: Error calling %s on %s: %s",
                  __func__,
                  method_name,
                  interface_name,
                  dbus_error->message);
        g_error_free(dbus_error);
        // Keep idle_time_seconds = 0
    } else if (dbus_result) {
        uint64_t idle_time_ms = 0;
        g_variant_get(dbus_result, "(t)", &idle_time_ms);
        idle_time_seconds = static_cast<int64_t>(idle_time_ms / 1000);
        debug_log("INFO: %s: Mutter IdleMonitor reported idle time: %llu ms (%lld seconds)",
                  __func__,
                  idle_time_ms,
                  idle_time_seconds);
        g_variant_unref(dbus_result);
    } else {
        error_log("%s: Call to %s on %s returned no result and no error.",
                  __func__,
                  method_name,
                  interface_name);
        // Keep idle_time_seconds = 0
    }

    g_object_unref(connection);

    return idle_time_seconds;
}

// --- WaylandIdleMonitor Implementation ---

// Forward declare the actual static listener structs (C-style linkage needed by wayland-client)
extern "C" {
extern const struct wl_registry_listener g_registry_listener;
extern const struct ext_idle_notification_v1_listener g_idle_notification_listener;
}

// Initialize static pointers (optional, just for consistency check)
const void* WaylandIdleMonitor::c_registry_listener_ptr = &g_registry_listener;
const void* WaylandIdleMonitor::c_idle_notification_listener_ptr = &g_idle_notification_listener;


WaylandIdleMonitor::WaylandIdleMonitor() :
    m_seat(nullptr),
    m_idle_notifier(nullptr),
    m_seat_id(0),
    m_idle_notifier_id(0),
    m_is_idle(false),
    m_idle_start_time(0),
    m_interrupt_monitor(false),
    m_initialized(false),
    m_display(nullptr),
    m_registry(nullptr),
    m_idle_notification(nullptr),
    m_notification_timeout_ms(0)
{
    m_interrupt_pipe_fd[0] = -1;
    m_interrupt_pipe_fd[1] = -1;
}

WaylandIdleMonitor::~WaylandIdleMonitor() {
    Stop();
}

bool WaylandIdleMonitor::Start(int notification_timeout_ms) {
    debug_log("INFO: %s: Starting Wayland idle monitor.",
              __func__);
    if (m_initialized.load()) {
        debug_log("INFO: %s: Already started.",
                  __func__);
        return true; // Already running
    }

    m_notification_timeout_ms = notification_timeout_ms;

    // Create pipe for interrupting poll()
    // pipe2 is Linux-specific, use pipe() for more portability if needed
    if (pipe2(m_interrupt_pipe_fd, O_CLOEXEC | O_NONBLOCK) == -1) {
        error_log("%s: Failed to create interrupt pipe: %s",
                  __func__,
                  strerror(errno));
        return false;
    }

    if (!InitializeWayland()) {
        error_log("%s: Failed to initialize Wayland or find required protocols.",
                  __func__);
        CleanupWayland(); // Clean up potentially partial initialization
        return false;
    }

    // Check again after roundtrips
    if (!m_seat || !m_idle_notifier) {
        error_log("%s: Required Wayland interfaces (wl_seat, ext_idle_notifier_v1) were not advertised by compositor.",
                  __func__);
        CleanupWayland();
        return false; // Protocol not available
    }

    CreateIdleNotification();
    if (!m_idle_notification) {
        error_log("%s: Failed to create Wayland idle notification object.",
                  __func__);
        CleanupWayland();
        return false;
    }

    m_interrupt_monitor = false; // Set before starting thread

    try {
        m_monitor_thread = std::thread(&WaylandIdleMonitor::WaylandMonitorThread, this);
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Wayland monitor thread: %s",
                  __func__,
                  e.what());
        CleanupWayland(); // Clean up Wayland resources if thread fails to start
        return false;
    }

    m_initialized = true; // Set initialized only after thread starts successfully
    debug_log("INFO: %s: Wayland idle monitor started successfully.",
              __func__);
    return true;
}

void WaylandIdleMonitor::Stop() {
    debug_log("INFO: %s: Stopping Wayland idle monitor...",
              __func__);
    // Use exchange to prevent concurrent Stop calls and get previous state
    if (m_interrupt_monitor.exchange(true)) {
        debug_log("INFO: %s: Already stopping or stopped.",
                  __func__);
        // If joinable, maybe still wait? Or assume previous call handles it.
        if(m_monitor_thread.joinable()) {
            // Avoid infinite wait if something went wrong
            auto future = std::async(std::launch::async, &std::thread::join, &m_monitor_thread);
            if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                error_log("%s: Timed out waiting for Wayland thread join during stop.",
                          __func__);
                // Cannot safely detach here as resources need cleanup by thread potentially.
            }
        }
        return;
    }

    // Signal the monitor thread to wake up from poll() by writing to the pipe
    if (m_interrupt_pipe_fd[1] != -1) {
        char buf = 'X'; // Data doesn't matter, just the event
        ssize_t written = write(m_interrupt_pipe_fd[1], &buf, 1);
        if (written <= 0 && errno != EAGAIN) {
            error_log("%s: Failed to write to interrupt pipe: %s",
                      __func__,
                      strerror(errno));
        } else {
            debug_log("INFO: %s: Sent interrupt signal via pipe.",
                      __func__);
        }
    }

    if (m_monitor_thread.joinable()) {
        try {
            m_monitor_thread.join();
            debug_log("INFO: %s: Wayland monitor thread joined.",
                      __func__);
        } catch (const std::system_error& e) {
            error_log("%s: Error joining Wayland monitor thread: %s",
                      __func__,
                      e.what());
        }
    }

    CleanupWayland(); // Clean up Wayland resources after thread stops
    m_initialized = false; // Mark as not initialized
    debug_log("INFO: %s: Wayland idle monitor stopped.",
              __func__);
}

bool WaylandIdleMonitor::InitializeWayland() {
    debug_log("INFO: %s: Initializing Wayland connection.",
              __func__);
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        error_log("%s: Failed to connect to Wayland display.",
                  __func__);
        return false;
    }

    m_registry = wl_display_get_registry(m_display);
    if (!m_registry) {
        error_log("%s: Failed to get Wayland registry.",
                  __func__);
        wl_display_disconnect(m_display);
        m_display = nullptr;
        return false;
    }

    // Add listener using the pointer defined in the header, pointing to the actual C-style struct
    wl_registry_add_listener(m_registry, (const wl_registry_listener*)c_registry_listener_ptr, this);

    // Initial roundtrip to get globals list
    if (wl_display_roundtrip(m_display) == -1) {
        error_log("%s: Wayland display roundtrip failed during initialization.",
                  __func__);
        CleanupWayland();
        return false;
    }
    // Sometimes a second roundtrip is needed for objects bound in the first
    if (wl_display_roundtrip(m_display) == -1) {
        error_log("%s: Second Wayland display roundtrip failed during initialization.",
                  __func__);
        CleanupWayland();
        return false;
    }


    // Check if required interfaces were bound by the listener during roundtrips
    return (m_seat != nullptr && m_idle_notifier != nullptr);
}

void WaylandIdleMonitor::CleanupWayland() {
    debug_log("INFO: %s: Cleaning up Wayland resources.",
              __func__);
    // Destroy specific notification first
    if (m_idle_notification) {
        ext_idle_notification_v1_destroy(m_idle_notification);
        m_idle_notification = nullptr;
    }
    // Destroy/release globals (check protocol spec if needed - often not required for globals)
    if (m_idle_notifier) {
        // ext_idle_notifier_v1 has no destroy request in spec v1/v2
        m_idle_notifier = nullptr;
    }
    if (m_seat) {
        // wl_seat_release available since version 5
        if (wl_seat_get_version(m_seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(m_seat);
        }
        m_seat = nullptr;
    }
    // Destroy registry
    if (m_registry) {
        wl_registry_destroy(m_registry);;
        m_registry = nullptr;
    }
    // Disconnect display
    if (m_display) {
        // Ensure queue is empty before disconnecting if possible
        // int ret;
        // while ((ret = wl_display_dispatch_queue_pending(m_display, queue)) > 0); // queue handling needed
        wl_display_flush(m_display);
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
    // Close interrupt pipe FDs
    if (m_interrupt_pipe_fd[0] != -1) { close(m_interrupt_pipe_fd[0]); m_interrupt_pipe_fd[0] = -1; }
    if (m_interrupt_pipe_fd[1] != -1) { close(m_interrupt_pipe_fd[1]); m_interrupt_pipe_fd[1] = -1; }
}

void WaylandIdleMonitor::CreateIdleNotification() {
    if (!m_idle_notifier || !m_seat || m_idle_notification) {
        debug_log("INFO: %s: Cannot create idle notification (missing deps or already exists).",
                  __func__);
        return;
    }

    // Use get_input_idle_notification if version >= 2 to ignore inhibitors? Decide based on need.
    // For now, use the standard one.
    m_idle_notification = ext_idle_notifier_v1_get_idle_notification(
        m_idle_notifier,
        m_notification_timeout_ms,
        m_seat);

    if (!m_idle_notification) {
        error_log("%s: ext_idle_notifier_v1_get_idle_notification failed.",
                  __func__);
        return;
    }

    // Add listener using the pointer defined in the header
    ext_idle_notification_v1_add_listener(m_idle_notification,
                                          (const ext_idle_notification_v1_listener*)c_idle_notification_listener_ptr,
                                          this);

    // Initial state is active
    m_is_idle = false;
    m_idle_start_time = 0;

    // We need to flush the display queue for the listener setup to be sent
    if (wl_display_flush(m_display) == -1) {
        error_log("%s: wl_display_flush failed after adding notification listener.",
                  __func__);
    }

    debug_log("INFO: %s: Created and added listener for idle notification object (timeout %d ms).",
              __func__,
              m_notification_timeout_ms);
}

void WaylandIdleMonitor::WaylandMonitorThread() {
    debug_log("INFO: %s: Wayland monitor thread started.",
              __func__);

    struct pollfd fds[2];
    fds[0].fd = wl_display_get_fd(m_display);
    fds[0].events = POLLIN | POLLERR | POLLHUP; // Watch for errors/hangup too
    fds[1].fd = m_interrupt_pipe_fd[0]; // Interrupt pipe read end
    fds[1].events = POLLIN | POLLERR | POLLHUP;

    int poll_ret;

    while (!m_interrupt_monitor.load()) {
        // Prepare read and dispatch pending events BEFORE blocking in poll
        // This handles cases where events arrive between poll calls
        while (wl_display_prepare_read(m_display) != 0) {
            if (wl_display_dispatch_pending(m_display) == -1) {
                error_log("%s: wl_display_dispatch_pending() failed in prepare loop. Exiting thread.",
                          __func__);
                goto thread_exit;
            }
        }

        // Flush requests to the server
        if (wl_display_flush(m_display) == -1 && errno != EAGAIN) {
            error_log("%s: wl_display_flush() failed: %s. Exiting thread.",
                      __func__,
                      strerror(errno));
            wl_display_cancel_read(m_display); // Cancel before exiting
            goto thread_exit;
        }

        // Block in poll until Wayland event or interrupt pipe event
        poll_ret = poll(fds, 2, -1); // No timeout, block indefinitely

        if (poll_ret < 0) {
            // Error in poll
            if (errno == EINTR) {
                debug_log("INFO: %s: poll() interrupted by signal, continuing.",
                          __func__);
                wl_display_cancel_read(m_display); // Cancel read before looping
                continue; // Loop again
            } else {
                error_log("%s: poll() failed: %s. Exiting thread.",
                          __func__,
                          strerror(errno));
                wl_display_cancel_read(m_display);
                goto thread_exit;
            }
        }

        // Check if interrupted via pipe (check before Wayland FD)
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            debug_log("INFO: %s: Interrupt or pipe error detected.", __func__);
            // Read the byte sent to clear the pipe if POLLIN
            if (fds[1].revents & POLLIN) {
                char buf[8]; // Read small amount
                [[maybe_unused]] ssize_t drain = read(m_interrupt_pipe_fd[0], buf, sizeof(buf));
            }
            wl_display_cancel_read(m_display); // Cancel Wayland read
            break; // Exit loop cleanly as requested
        }

        // Check for Wayland events or errors
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (fds[0].revents & (POLLERR | POLLHUP)) {
                error_log("%s: Error/Hangup on Wayland display FD. Exiting thread.",
                          __func__);
                // No need to cancel read, connection is likely dead
                goto thread_exit;
            }

            // If POLLIN, read events
            if (wl_display_read_events(m_display) == -1) {
                error_log("%s: wl_display_read_events() failed. Exiting thread.",
                          __func__);
                goto thread_exit;
            }

            // Dispatch the read events
            // Use dispatch_queue_pending if using separate event queues
            if (wl_display_dispatch_pending(m_display) == -1) {
                error_log("%s: wl_display_dispatch_pending() failed after read. Exiting thread.",
                          __func__);
                goto thread_exit;
            }
        } else {
            // Poll returned >= 0, but no event on Wayland FD? Should not happen with -1 timeout
            // unless only the interrupt pipe had an event (handled above).
            // If it happens, just cancel read prep and loop.
            wl_display_cancel_read(m_display);
        }

        // Check interrupt flag again, in case Stop() was called during event dispatch
        if (m_interrupt_monitor.load()) {
            debug_log("INFO: %s: Interrupt detected after dispatch.",
                      __func__);
            break;
        }
    } // end while

thread_exit:
    debug_log("INFO: %s: Wayland monitor thread exiting.",
              __func__);
    // Stop() / ~WaylandIdleMonitor() will handle CleanupWayland()
}

bool WaylandIdleMonitor::IsAvailable() const {
    // Check if initialized successfully (implies protocol was found)
    return m_initialized.load();
}

bool WaylandIdleMonitor::IsIdle() const {
    return m_is_idle.load();
}

int64_t WaylandIdleMonitor::GetIdleSeconds() const {
    if (!m_initialized.load()) {
        return 0; // Not available
    }
    if (m_is_idle.load()) {
        int64_t start_time = m_idle_start_time.load(std::memory_order_relaxed);
        int64_t current_time = GetUnixEpochTime(); // Assuming this is thread-safe
        // Basic check against clock skew / negative time
        return (current_time > start_time) ? (current_time - start_time) : 0;
    } else {
        return 0; // Active
    }
}

// --- Static Listener Implementations (C-linkage required) ---

extern "C" {

void WaylandIdleMonitor_HandleGlobal(void *data, wl_registry *registry, uint32_t name,
                                     const char *interface, uint32_t version)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    debug_log("INFO: %s: Global: %s v%u (name %u)",
              __func__,
              interface,
              version,
              name);

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        monitor->m_seat_id = name;
        // Bind wl_seat, version 5 needed for release request
        uint32_t bind_version = std::min(version, 5u);
        monitor->m_seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, bind_version)
            );
        debug_log("INFO: %s: Bound wl_seat (name %u) version %u.",
                  __func__,
                  name,
                  bind_version);
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        monitor->m_idle_notifier_id = name;
        // Bind ext_idle_notifier_v1, version 2 allows ignoring inhibitors if needed
        uint32_t bind_version = std::min(version, 2u);
        monitor->m_idle_notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bind_version)
            );
        debug_log("INFO: %s: Bound %s (name %u) version %u.",
                  __func__,
                  ext_idle_notifier_v1_interface.name,
                  name,
                  bind_version);
    }
}

void WaylandIdleMonitor_HandleGlobalRemove(void *data, wl_registry * /* registry */, uint32_t name)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    debug_log("INFO: %s: Global removed: %u",
              __func__,
              name);
    // Handle globals being removed if necessary (e.g., seat removed)
    // This might require more complex state management or re-initialization logic
    if (name == monitor->m_seat_id) {
        error_log("%s: Monitored wl_seat (name %u) was removed!",
                  __func__,
                  name);
        monitor->m_seat = nullptr; // Mark as gone
    } else if (name == monitor->m_idle_notifier_id) {
        error_log("%s: Idle notifier global (name %u) was removed!",
                  __func__,
                  name);
        monitor->m_idle_notifier = nullptr; // Mark as gone
    }
}

void WaylandIdleMonitor_HandleIdled(void *data, ext_idle_notification_v1 * /* notification */)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    // Use memory_order_relaxed likely fine for atomics updated by single thread
    // Use exchange to ensure we only log/set start time on transition
    if (!monitor->m_is_idle.exchange(true, std::memory_order_relaxed)) {
        monitor->m_idle_start_time.store(GetUnixEpochTime(), std::memory_order_relaxed);
        debug_log("INFO: %s: Idle state entered at %lld",
                  __func__,
                  monitor->m_idle_start_time.load(std::memory_order_relaxed));
    }
}

void WaylandIdleMonitor_HandleResumed(void *data, ext_idle_notification_v1 * /* notification */)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    // Use exchange to ensure we only log on transition
    if (monitor->m_is_idle.exchange(false, std::memory_order_relaxed)) {
        monitor->m_idle_start_time.store(0, std::memory_order_relaxed); // Reset start time
        debug_log("INFO: %s: Idle state exited (resumed).",
                  __func__);
    }
}

// Define the actual static listener structs with C linkage
// idle_detect.cpp (global scope, ensure extern "C" block)
const struct wl_registry_listener g_registry_listener = {
    /* .global = */ WaylandIdleMonitor_HandleGlobal,
    /* .global_remove = */ WaylandIdleMonitor_HandleGlobalRemove
};

const struct ext_idle_notification_v1_listener g_idle_notification_listener = {
    /* .idled = */ WaylandIdleMonitor_HandleIdled,
    /* .resumed = */ WaylandIdleMonitor_HandleResumed
};
} // extern "C"


// --- DBusInhibitMonitor Implementation ---

DBusInhibitMonitor::DBusInhibitMonitor() :
    m_interrupt_monitor(false),
    m_initialized(false),
    m_is_inhibited(false), // Assume not inhibited initially
    m_main_loop(nullptr),
    m_connection(nullptr),
    m_signal_subscription_id(0)
{}

DBusInhibitMonitor::~DBusInhibitMonitor() {
    Stop(); // Ensure cleanup on destruction
}

bool DBusInhibitMonitor::Start() {
    debug_log("INFO: %s: Starting D-Bus inhibit monitor.", __func__);
    if (m_initialized.load()) {
        debug_log("INFO: %s: Monitor already initialized.", __func__);
        return true;
    }

    // Reset state flags
    m_interrupt_monitor.store(false);
    m_is_inhibited.store(false); // Reset to default before getting initial state

    // Initialize D-Bus connection, get initial state, and subscribe to signals
    // This happens synchronously before starting the thread.
    if (!InitializeDBus()) {
        error_log("%s: Failed to initialize D-Bus.", __func__);
        CleanupDBus(); // Clean up any partial initialization
        m_initialized.store(false);
        return false;
    }

    // If D-Bus setup is okay, start the thread to run the GMainLoop
    try {
        m_monitor_thread = std::thread(&DBusInhibitMonitor::DBusMonitorThread, this);
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start D-Bus monitor thread: %s", __func__, e.what());
        CleanupDBus(); // Clean up D-Bus resources if thread fails to start
        m_initialized.store(false);
        return false;
    }

    m_initialized.store(true);
    debug_log("INFO: %s: D-Bus inhibit monitor started successfully.", __func__);
    return true;
}

void DBusInhibitMonitor::Stop() {
    debug_log("INFO: %s: Stopping D-Bus inhibit monitor...", __func__);

    // Prevent double stopping and signal thread
    if (m_interrupt_monitor.exchange(true)) {
        debug_log("INFO: %s: Stop already in progress or completed.", __func__);
        // Optional: If thread might be stuck, add forceful exit/timeout? Risky.
        // Ensure join is still attempted if needed
        if(m_monitor_thread.joinable() && m_monitor_thread.get_id() != std::this_thread::get_id()) {
            try { m_monitor_thread.join(); } catch (...) { /* Ignore secondary join errors */ }
        }
        return;
    }

    // Signal the GMainLoop running in the thread to quit
    // Check m_main_loop existence and running status for safety
    if (m_main_loop != nullptr && g_main_loop_is_running(m_main_loop)) {
        debug_log("INFO: %s: Quitting D-Bus GMainLoop.", __func__);
        g_main_loop_quit(m_main_loop);
    } else {
        debug_log("INFO: %s: GMainLoop pointer null or not running, cannot quit.", __func__);
    }


    // Join the thread (wait for it to finish)
    // Avoid joining self if Stop is called from the monitor thread itself (shouldn't happen ideally)
    if (m_monitor_thread.joinable() && m_monitor_thread.get_id() != std::this_thread::get_id()) {
        debug_log("INFO: %s: Joining D-Bus monitor thread...", __func__);
        try {
            m_monitor_thread.join();
            debug_log("INFO: %s: D-Bus monitor thread joined.", __func__);
        } catch (const std::system_error& e) {
            error_log("%s: Error joining D-Bus monitor thread: %s", __func__, e.what());
            // Consider if thread cleanup is still needed / possible
        }
    } else if (m_monitor_thread.get_id() == std::this_thread::get_id()) {
        error_log("WARNING: %s: Stop() called from within the monitor thread! Cannot join.", __func__);
    }


    // Cleanup D-Bus resources (connection, subscriptions)
    // This should happen after the thread has stopped using them.
    CleanupDBus();

    m_initialized.store(false); // Mark as no longer initialized
    debug_log("INFO: %s: D-Bus inhibit monitor stopped.", __func__);
}

// --- Internal Helper Implementations ---

bool DBusInhibitMonitor::InitializeDBus() {
    GError* error = nullptr;
    debug_log("INFO: %s: Connecting to D-Bus session bus...", __func__);

    // 1. Connect to the session bus
    m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr /* cancellable */, &error);
    if (!m_connection) {
        error_log("%s: Failed to connect to D-Bus session bus: %s", __func__, error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        return false;
    }
    debug_log("INFO: %s: Connected to D-Bus session bus.", __func__);

    // 2. Get the initial inhibition state
    if (!GetInitialActiveState()) {
        // Logged inside the function, but maybe warn that initial state is unknown
        error_log("WARNING: %s: Could not determine initial inhibition state. Assuming not inhibited.", __func__);
        // Keep m_is_inhibited as its default (false)
    }

    // 3. Subscribe to the ActiveChanged signal
    debug_log("INFO: %s: Subscribing to D-Bus signal %s.%s...", __func__, SS_INTERFACE, SS_ACTIVE_CHANGED_SIGNAL);
    m_signal_subscription_id = g_dbus_connection_signal_subscribe(
        m_connection,
        nullptr,                            // Sender filter (nullptr = any sender) - More robust as service name might vary? Or use SS_SERVICE? Let's use null for now.
        SS_INTERFACE,                       // Interface name
        SS_ACTIVE_CHANGED_SIGNAL,           // Signal name
        SS_PATH,                            // Object path
        nullptr,                            // Argument filter (NULL = all args)
        G_DBUS_SIGNAL_FLAGS_NONE,
        HandleActiveChangedSignal,          // Callback function
        this,                               // User data (pointer to this instance)
        nullptr                             // User data free function
        );

    if (m_signal_subscription_id == 0) {
        error_log("%s: Failed to subscribe to D-Bus signal %s.%s.", __func__, SS_INTERFACE, SS_ACTIVE_CHANGED_SIGNAL);
        // Maybe the service isn't running or interface isn't present?
        // Consider this non-fatal for now? Or return false? Let's return false.
        g_object_unref(m_connection); // Clean up connection
        m_connection = nullptr;
        return false;
    }
    debug_log("INFO: %s: Subscribed to D-Bus signal (ID: %u).", __func__, m_signal_subscription_id);

    return true; // Success
}

void DBusInhibitMonitor::CleanupDBus() {
    debug_log("INFO: %s: Cleaning up D-Bus resources.", __func__);
    // Unsubscribe from signals
    if (m_connection != nullptr && m_signal_subscription_id > 0) {
        debug_log("INFO: %s: Unsubscribing from D-Bus signal ID %u.", __func__, m_signal_subscription_id);
        g_dbus_connection_signal_unsubscribe(m_connection, m_signal_subscription_id);
        m_signal_subscription_id = 0; // Reset ID
    } else {
        debug_log("INFO: %s: No active D-Bus signal subscription to unsubscribe.", __func__);
    }

    // Release reference to the connection
    if (m_connection != nullptr) {
        debug_log("INFO: %s: Unreferencing D-Bus connection.", __func__);
        g_object_unref(m_connection);
        m_connection = nullptr;
    } else {
        debug_log("INFO: %s: No active D-Bus connection to unreference.", __func__);
    }

    // GMainLoop is managed/unreffed by the thread function itself when it exits
    m_main_loop = nullptr; // Ensure pointer is cleared
}

void DBusInhibitMonitor::DBusMonitorThread() {
    debug_log("INFO: %s: D-Bus monitor thread started (ID: %s).",
              __func__,
              std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))); // Log thread ID

    // Create a GLib main loop context for this thread
    m_main_loop = g_main_loop_new(nullptr /* context */, FALSE /* is_running */);
    if (!m_main_loop) {
        error_log("CRITICAL: %s: Failed to create GMainLoop in monitor thread!", __func__);
        // Cannot proceed without a loop. Mark as uninitialized?
        m_initialized.store(false); // Maybe signal failure state?
        return; // Exit thread
    }

    debug_log("INFO: %s: Running GMainLoop...", __func__);
    // Run the loop. This will block until g_main_loop_quit() is called from Stop()
    // or if the loop encounters a critical error (less likely).
    g_main_loop_run(m_main_loop);

    debug_log("INFO: %s: GMainLoop finished.", __func__);

    // Clean up the loop object after it finishes running
    g_main_loop_unref(m_main_loop);
    m_main_loop = nullptr; // Clear the pointer

    debug_log("INFO: %s: D-Bus monitor thread exiting.", __func__);
}

bool DBusInhibitMonitor::IsAvailable() const {
    return m_initialized.load();
}

bool DBusInhibitMonitor::IsInhibited() const {
    // Return the last known state.
    // If not initialized, this will return the default (false).
    return m_is_inhibited.load(std::memory_order_relaxed);
}

bool DBusInhibitMonitor::GetInitialActiveState() {
    if (!m_connection) return false; // Need connection first

    GError* error = nullptr;
    GVariant* result = nullptr;
    debug_log("INFO: %s: Calling D-Bus method %s.%s...", __func__, SS_INTERFACE, SS_GET_ACTIVE_METHOD);

    result = g_dbus_connection_call_sync(m_connection,
                                         SS_SERVICE,
                                         SS_PATH,
                                         SS_INTERFACE,
                                         SS_GET_ACTIVE_METHOD,
                                         nullptr, // No parameters
                                         G_VARIANT_TYPE("(b)"), // Expect boolean reply in a tuple
                                         G_DBUS_CALL_FLAGS_NONE,
                                         1000, // Timeout in ms (e.g., 1 second)
                                         nullptr, // Cancellable
                                         &error);

    if (error) {
        error_log("%s: Failed to call D-Bus %s: %s", __func__, SS_GET_ACTIVE_METHOD, error->message);
        g_error_free(error);
        return false; // Failed to get state
    }

    if (result) {
        gboolean is_active = FALSE;
        g_variant_get(result, "(b)", &is_active);
        m_is_inhibited.store(!is_active, std::memory_order_relaxed); // Inhibited = NOT active
        debug_log("INFO: %s: Initial D-Bus screensaver active state: %s -> Inhibited: %s",
                  __func__,
                  is_active ? "true" : "false", m_is_inhibited.load() ? "true" : "false");
        g_variant_unref(result);
        return true; // Successfully got state
    } else {
        error_log("%s: D-Bus call to %s returned null result without error.", __func__, SS_GET_ACTIVE_METHOD);
        return false;
    }
}


// --- Static D-Bus Signal Callback Implementation ---
void DBusInhibitMonitor::HandleActiveChangedSignal(GDBusConnection * /*connection*/,
                                                   const gchar *sender_name,
                                                   const gchar * /*object_path*/,
                                                   const gchar * /*interface_name*/,
                                                   const gchar *signal_name,
                                                   GVariant *parameters,
                                                   gpointer user_data)
{
    DBusInhibitMonitor *monitor = static_cast<DBusInhibitMonitor*>(user_data);
    if (!monitor || monitor->m_interrupt_monitor.load()) {
        // Ignore signals if monitor is stopping or pointer is invalid
        return;
    }

    gboolean is_active = FALSE;
    const gchar* sender_display = sender_name ? sender_name : "[Unknown Sender]";

    // Parameter for ActiveChanged is a single boolean 'b' inside a tuple '(b)'
    if (parameters == nullptr || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) {
        error_log("%s: Received invalid parameters for %s signal from %s.", __func__, signal_name, sender_display);
        return;
    }
    g_variant_get(parameters, "(b)", &is_active);

    // Inhibition state is the opposite of the screensaver active state
    bool is_now_inhibited = !is_active;

    // Update atomic state using exchange to detect changes
    bool was_inhibited = monitor->m_is_inhibited.exchange(is_now_inhibited, std::memory_order_relaxed);

    if (is_now_inhibited != was_inhibited) {
        debug_log("INFO: %s: Inhibition state CHANGED via D-Bus signal (%s from %s) -> Inhibited: %s",
                  __func__,
                  signal_name,
                  sender_display,
                  is_now_inhibited ? "true" : "false");
    } else {
        // Log that signal was received, even if state is the same (useful for debugging)
        debug_log("INFO: %s: D-Bus signal received (%s from %s), state unchanged (Inhibited: %s)",
                  __func__,
                  signal_name,
                  sender_display,
                  is_now_inhibited ? "true" : "false");
    }
}


int64_t GetIdleTimeSeconds() {
    if (IsTtySession()) {
        debug_log("INFO: %s: TTY session detected, idle check not applicable.",
                  __func__);
        return 0;
    } else if (IsWaylandSession()) {
        debug_log("INFO: %s: Wayland session detected.", __func__);
        // Check if the Wayland monitor using the protocol is available and initialized
        if (g_wayland_idle_monitor.IsAvailable()) {
            debug_log("INFO: %s: Using WaylandIdleMonitor (ext-idle-notify-v1).",
                      __func__);
            return g_wayland_idle_monitor.GetIdleSeconds();
        }
        // --- Fallback Logic ---
        // Only try D-Bus for Gnome if Wayland monitor failed/unavailable
        else if (IsGnomeSession()) {
            debug_log("INFO: %s: WaylandIdleMonitor unavailable, falling back to D-Bus for Gnome.",
                      __func__);
            return GetIdleTimeWaylandGnomeViaDBus(); // Ensure this function exists
        }
        // Add other Wayland fallbacks here if needed (e.g., check for deprecated kde-idle?)
        else {
            debug_log("INFO: %s: WaylandIdleMonitor unavailable, no other Wayland method implemented/available.",
                      __func__);
            return 0; // No Wayland method worked
        }
    } else { // Assume X11
        debug_log("INFO: %s: X11 session detected. Using XScreenSaver.",
                  __func__);

        // --- X11 implementation as before ---
        Display* display = XOpenDisplay(nullptr);

        if (!display) {
            error_log("%s: Could not open X display.", __func__);
            return 0;
        }

        int event_base;
        int error_base;

        if (!XScreenSaverQueryExtension(display, &event_base, &error_base)) {
            error_log("%s: XScreenSaver extension unavailable.", __func__);
            XCloseDisplay(display);
            return 0;
        }

        XScreenSaverInfo* info = XScreenSaverAllocInfo();

        if (!info) {
            error_log("%s: Could not allocate XScreenSaverInfo.",
                      __func__);
            XCloseDisplay(display);
            return 0;
        }

        Window root = DefaultRootWindow(display);

        XScreenSaverQueryInfo(display, root, info);

        int64_t idle_time_ms = info->idle;

        XFree(info);
        XCloseDisplay(display);

        int64_t idle_time_seconds = idle_time_ms / 1000;

        debug_log("INFO: %s: XScreenSaver reported: %lld ms (%lld seconds)",
                  __func__,
                  (int64_t)idle_time_ms,
                  (int64_t)idle_time_seconds);

        return idle_time_seconds;
        // --- End X11 implementation ---
    }
}


/**
 * @brief Executes a shell command string in the background (detached thread).
 * @param command The command string to execute via /bin/sh -c.
 */
void ExecuteCommandBackground(const std::string& command) {
    if (command.empty()) {
        debug_log("INFO: %s: No command provided.",
                  __func__);
        return;
    }

    debug_log("INFO: %s: Executing background command: %s",
              __func__,
              command);
    try {
        // Launch system() in a detached thread. The lambda captures command by value.
        std::thread([command]() {
            int ret = std::system(command.c_str());
            // Optional: Log result if needed, but tricky as thread is detached.
            // This log might appear long after the main thread has moved on.
            if (ret != 0) {
                // Use thread-safe logging if available or be cautious
                // error_log("Background command '%s' exited with code %d", command, ret);
                // For now, maybe skip logging from detached thread.
            }
        }).detach();
    } catch (const std::system_error& e) {
        error_log("%s: Failed to launch background command thread: %s",
                  __func__,
                  e.what());
    } catch (...) {
        error_log("%s: Unknown error launching background command thread.",
                  __func__);
    }
}

} // namespace IdleDetect

// --- Signal Handler ---
void HandleSignal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        // Use log() here as it's presumably safe enough during shutdown signal
        log("INFO: %s: Received signal %d. Requesting shutdown.",
            __func__,
            signum);
        g_shutdown_requested.store(true);
        // Optional: Notify main thread's condition variable if it were sleeping longer
        // For a simple polling loop like below, setting the flag is sufficient.
    } else {
        log("INFO: %s: Received unexpected signal %d.",
            __func__,
            signum);
    }
}

int main(int argc, char* argv[]) {

    // --- Configuration Loading ---
    //
    // We can safely use error_log and log before reading config.
    if (argc != 2) {
        error_log("%s: One argument must be specified for the location of the config file.",
                  __func__);
        return 1;
    }
    fs::path config_file_path(argv[1]);
    if (fs::exists(config_file_path) && fs::is_regular_file(config_file_path)) {
        log("INFO: %s: Using config from %s",
                  __func__,
                  config_file_path);
    } else {
        log("WARNING: %s: Argument invalid for config file \"%s\". Using defaults.",
            __func__,
            config_file_path.string());

        config_file_path = "";
    }

    try {
        // Assuming ReadAndUpdateConfig is PascalCase
        g_config.ReadAndUpdateConfig(config_file_path);
    } catch (const std::exception& e) {
        error_log("%s: Failed to read/process config: %s",
                  __func__,
                  e.what());

        return 1;
    }

    g_debug = std::get<bool>(g_config.GetArg("debug"));

    IdleDetect::DEFAULT_IDLE_THRESHOLD_SECONDS = std::get<int>(g_config.GetArg("inactivity_time_trigger"));

    // --- Get Relevant Config Values ---
    // (variable_names are snake_case)
    int idle_threshold_seconds = 0;
    int check_interval_seconds = IdleDetect::DEFAULT_CHECK_INTERVAL_SECONDS;
    bool should_update_event_detect = true;
    fs::path event_data_path;
    int initial_sleep = 0;
    bool execute_dc_control_scripts = true;
    std::string active_command;
    std::string idle_command;
    bool use_event_detect = true;
    std::string last_active_time_cpp_filename;

    try {
        // Assuming GetArg is PascalCase
        idle_threshold_seconds = std::get<int>(g_config.GetArg("inactivity_time_trigger"));
        should_update_event_detect = std::get<bool>(g_config.GetArg("update_event_detect"));
        event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));
        initial_sleep = std::get<int>(g_config.GetArg("initial_sleep"));
        active_command = std::get<std::string>(g_config.GetArg("active_command"));
        idle_command = std::get<std::string>(g_config.GetArg("idle_command"));
        execute_dc_control_scripts = std::get<bool>(g_config.GetArg("execute_dc_control_scripts"));
        use_event_detect = std::get<bool>(g_config.GetArg("use_event_detect"));
        last_active_time_cpp_filename = std::get<std::string>(g_config.GetArg("last_active_time_cpp_filename"));
    } catch (const std::bad_variant_access& e) {
        error_log("%s: Configuration value missing or has wrong type: %s. Using defaults where possible.",
                  __func__,
                  e.what());
    } catch (...) {
        error_log("%s: Unknown error accessing configuration values.",
                  __func__);
        return 1;
    }

    fs::path pipe_path = event_data_path / "event_registration_pipe";
    fs::path dat_file_path = event_data_path / last_active_time_cpp_filename;

    // --- Signal Handling Setup ---
    g_shutdown_requested = false;
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = HandleSignal;
    action.sa_flags = 0; // Consider SA_RESTART if needed
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) == -1 || sigaction(SIGTERM, &action, nullptr) == -1) {
        error_log("%s: Failed to set signal handlers: %s",
                  __func__,
                  strerror(errno));
        return 1;
    }
    // Optional: Block signals in other threads if they shouldn't handle them
    // Optional: Ignore SIGPIPE if writing to pipe fails often
    // signal(SIGPIPE, SIG_IGN);


    log("Idle Detect starting.");
    debug_log("INFO: %s: Idle threshold: %d seconds, Check interval: %d seconds",
              __func__,
              idle_threshold_seconds,
              check_interval_seconds);
    debug_log("INFO: %s: Update event_detect: %s, Pipe path: %s",
              __func__,
              should_update_event_detect ? "true" : "false",
              pipe_path.string());
    debug_log("INFO: %s: Execute dc control scripts: %s",
              __func__,
              execute_dc_control_scripts ? "true" : "false");
    debug_log("INFO: %s: Active command: '%s'",
              __func__,
              active_command);
    debug_log("INFO: %s: Idle command: '%s'",
              __func__,
              idle_command);

    if (initial_sleep > 0) {
        log("INFO: %s: Waiting for %d seconds to start.",
            __func__,
            initial_sleep);

        std::this_thread::sleep_for(std::chrono::seconds(initial_sleep));
    }

    // --- Start Wayland Monitor (if applicable) ---
    bool wayland_monitor_started = false;
    if (IdleDetect::IsWaylandSession()) {
        // Use a short timeout for Wayland notification to detect idle start quickly
        int notification_timeout_ms = 1000; // 1 second, or make configurable?
        debug_log("INFO: %s: Attempting to start Wayland idle monitor (timeout %dms)...",
                  __func__,
                  notification_timeout_ms);
        if (g_wayland_idle_monitor.Start(notification_timeout_ms)) {
            debug_log("INFO: %s: Wayland idle monitor started successfully.",
                      __func__);
            wayland_monitor_started = true;
        } else {
            error_log("%s: Failed to start Wayland idle monitor. "
                      "Relying on fallbacks (e.g., D-Bus for Gnome) if available.",
                      __func__);
            // GetIdleTimeSeconds() will handle the fallback internally
        }
    }

    // Attempt to start D-Bus Inhibit Monitor regardless of session type (X11 or Wayland)
    // as inhibitions might be relevant in both.
    bool dbus_monitor_started = false;
    debug_log("INFO: %s: Attempting to start D-Bus inhibit monitor...", __func__);
    if (g_dbus_inhibit_monitor.Start()) {
        debug_log("INFO: %s: D-Bus inhibit monitor started successfully.", __func__);
        dbus_monitor_started = true;
    } else {
        error_log("%s: Failed to start D-Bus inhibit monitor. Inhibition detection unavailable.", __func__);
    }

    // --- Main Loop ---
    bool was_previously_idle = false; // Track previous state (based on final idle seconds)

    // Main loop continues until shutdown is requested via signal
    while (!g_shutdown_requested.load()) {

        // 1. Get Input Idle Time (includes fallback logic)
        // ==================================================
        int64_t input_idle_seconds = 0; // Default to active state (0 seconds idle)
        int64_t direct_idle_seconds = IdleDetect::GetIdleTimeSeconds(); // Primary check

        if (direct_idle_seconds >= 0) {
            // Successfully got idle time directly from Wayland/X11/D-Bus-Gnome
            input_idle_seconds = direct_idle_seconds;
            debug_log("INFO: %s: Using direct input idle time: %lld seconds.", __func__, (int64_t)input_idle_seconds);
        } else {
            // Failed to get idle time directly (-1), try fallback by reading event_detect's file
            debug_log("INFO: %s: Direct idle detection failed/unavailable.", __func__);
            if (use_event_detect) { // Check if fallback is enabled in config
                debug_log("INFO: %s: Attempting fallback using event_detect file: %s", __func__, dat_file_path.string());
                int64_t file_timestamp = IdleDetect::ReadLastActiveTimeFile(dat_file_path); // Helper function defined earlier
                if (file_timestamp > 0) {
                    // Calculate idle time based on file timestamp
                    int64_t current_time = GetUnixEpochTime();
                    int64_t calculated_idle = current_time - file_timestamp;
                    input_idle_seconds = (calculated_idle > 0) ? calculated_idle : 0; // Ensure non-negative
                    debug_log("INFO: %s: Using fallback idle time: %lld seconds (current: %lld, file: %lld)",
                              __func__,
                              (int64_t)input_idle_seconds,
                              (int64_t)current_time,
                              (int64_t)file_timestamp);
                } else {
                    // File read/parse failed
                    error_log("%s: Fallback failed: Could not read/parse valid timestamp from event_detect file. Assuming 0s idle.",
                              __func__);
                    input_idle_seconds = 0; // Assume active if fallback fails
                }
            } else {
                // Fallback disabled in config
                debug_log("INFO: %s: Fallback disabled by config (use_event_detect=false). Assuming 0s idle.", __func__);
                input_idle_seconds = 0; // Assume active if direct fails and fallback disabled
            }
        } // End Fallback Logic


        // 2. Get Inhibition State from D-Bus Monitor
        // ==========================================
        // dbus_monitor_started flag was set earlier when attempting g_dbus_inhibit_monitor.Start()
        bool is_inhibited = dbus_monitor_started && g_dbus_inhibit_monitor.IsInhibited();


        // 3. Determine Final Idle Seconds (Force to 0 if inhibited)
        // =======================================================
        int64_t idle_seconds = 0; // This final value drives decisions
        if (is_inhibited) {
            // Log only if inhibition is actually overriding a non-zero input idle time
            if (input_idle_seconds > 0) {
                debug_log("INFO: %s: Inhibition detected, overriding input idle time (%llds) to 0s.",
                          __func__, (int64_t)input_idle_seconds);
            }
            idle_seconds = 0; // Treat system as active (0s idle) if inhibited
        } else {
            idle_seconds = input_idle_seconds; // Use the actual input idle time (potentially from fallback)
        }


        // 4. Calculate Current State based on final idle seconds
        // ======================================================
        // idle_threshold_seconds was loaded from config earlier
        bool is_currently_idle = (idle_seconds >= idle_threshold_seconds);

        // Comprehensive debug log showing inputs and final decision
        debug_log("INFO: %s: InputIdle: %llds, Inhibited: %s -> FinalIdle: %llds, State: %s",
                  __func__,
                  (int64_t)input_idle_seconds,
                  is_inhibited ? "Yes" : "No",
                  (int64_t)idle_seconds,
                  is_currently_idle ? "Idle" : "Active");


        // --- Handle State Change & Actions (based on the unified final state) ---
        // ======================================================================
        if (is_currently_idle != was_previously_idle) {
            if (is_currently_idle) {
                // Became Idle (Input idle threshold met AND Not inhibited)
                log("INFO: %s: System became idle (Input idle >= %ds AND Not inhibited).",
                    __func__, idle_threshold_seconds);
                if (execute_dc_control_scripts) { // execute_dc_control_scripts from config
                    IdleDetect::ExecuteCommandBackground(idle_command); // idle_command from config
                }
            } else {
                // Became Active (Input became active OR Inhibition started)
                log("INFO: %s: System became active (Input active OR Inhibited).", __func__);
                if (execute_dc_control_scripts) {
                    IdleDetect::ExecuteCommandBackground(active_command); // active_command from config
                }
            }
            was_previously_idle = is_currently_idle; // Update tracked state
        }


        // --- Send Pipe Notification on Every Active Cycle (if enabled) ---
        // ===============================================================
        // Sends if the final state (considering input AND inhibition) is active
        if (!is_currently_idle && should_update_event_detect) { // should_update_event_detect from config
            debug_log("INFO: %s: System effectively active, sending notification to pipe.", __func__);
            IdleDetect::SendPipeNotification(pipe_path, GetUnixEpochTime() - idle_seconds); // pipe_path defined earlier
        }


        // --- Sleep for the check interval, interruptible ---
        // =================================================
        // check_interval_seconds loaded from config earlier (or default)
        auto wake_up_time = std::chrono::steady_clock::now() + std::chrono::seconds(check_interval_seconds);
        while (std::chrono::steady_clock::now() < wake_up_time) {
            if (g_shutdown_requested.load()) {
                debug_log("INFO: %s: Shutdown requested during sleep interval.", __func__);
                break; // Exit inner sleep loop
            }
            // Check more frequently than the full interval for responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_shutdown_requested.load()) {
            debug_log("INFO: %s: Shutdown requested after sleep interval check.", __func__);
            break; // Exit main loop
        }
    } // End main while loop

    // --- Shutdown Sequence ---
    // =======================
    log("INFO: %s: Shutdown requested. Cleaning up...", __func__);

    if (dbus_monitor_started) {
        log("INFO: %s: Stopping D-Bus inhibit monitor...", __func__);
        g_dbus_inhibit_monitor.Stop();
        log("INFO: %s: D-Bus inhibit monitor stopped.", __func__);
    }
    if (wayland_monitor_started) {
        log("INFO: %s: Stopping Wayland idle monitor...", __func__);
        g_wayland_idle_monitor.Stop();
        log("INFO: %s: Wayland idle monitor stopped.", __func__);
    }
    // Add cleanup for other resources if necessary (e.g., remove lock file if managing one)

    log("INFO: Idle Detect shutdown complete.");
    return 0;
} // End main function }
