/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include "idle_detect.h"
#include "util.h"

#include <fstream>
#include <cstdlib> // For system()
#include <cstdint> // For int64_t
#include <future>
#include <cstring> // For strcmp
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gio/gio.h> // For D-Bus
#include <fcntl.h>      // For pipe2, O_NONBLOCK, O_CLOEXEC
#include <poll.h>       // For poll()
#include <unistd.h>     // For pipe, read, write, close
#include <errno.h>      // For errno
#include <algorithm>    // For std::min

// Wayland includes
#include <wayland-client.h>
// This header MUST be generated from ext-idle-notify-v1.xml using wayland-scanner
#include <./build/cmake/ext-idle-notify-v1-protocol.h>
#include <ext-idle-notify-v1-protocol.h>


//!
//! \brief This is populated by main from an early GetArg so that the debug_log doesn't use the heavyweight GetArg call constantly.
//!
std::atomic<bool> g_debug = false;

//!
//! \brief Global config singleton
//!
IdleDetectConfig g_config;

//!
//! \brief Global Wayland idle monitor singleton
//!
IdleDetect::WaylandIdleMonitor g_wayland_idle_monitor;


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
 * @brief Sends a notification message to the event_detect named pipe.
 *
 * @param pipe_path The full path to the event_detect named pipe.
 */
void SendPipeNotification(const std::filesystem::path& pipe_path) { // Renamed function
    // Construct the message payload using EventMessage format
    // Assuming ::ToString and GetUnixEpochTime follow the requested convention if they were defined elsewhere
    // If GetUnixEpochTime() is your function, rename to GetUnixEpochTime()
    EventMessage msg(::ToString(GetUnixEpochTime()), "USER_ACTIVE");
    if (!msg.IsValid()) { // Assuming IsValid is a method, renamed
        error_log("%s: Failed to construct valid EventMessage.", __func__); // __func__ is standard, remains lowercase
        return;
    }
    // variable_name is snake_case
    std::string message_str = msg.ToString() + "\n"; // Assuming ToString is a method, renamed

    // Assuming TrimString follows the requested convention if it's your function
    debug_log("%s: Attempting to send message: %s", __func__, TrimString(message_str));

    // variable_name is snake_case
    std::error_code error_code; // Renamed ec for clarity
    if (!std::filesystem::exists(pipe_path, error_code) || error_code) {
        error_log("%s: Pipe '%s' does not exist or cannot be accessed. Is event_detect running?",
                  __func__, pipe_path.string().c_str());
        return;
    }

    if (!std::filesystem::is_fifo(pipe_path, error_code) || error_code) {
        error_log("%s: Path '%s' is not a named pipe (FIFO).",
                  __func__, pipe_path.string().c_str());
        return;
    }

    // variable_name is snake_case
    std::ofstream pipe_stream(pipe_path, std::ios::out);

    if (!pipe_stream.is_open()) {
        error_log("%s: Failed to open pipe '%s' for writing: %s",
                  __func__, pipe_path.string().c_str(), strerror(errno));
        return;
    }

    pipe_stream << message_str;
    pipe_stream.flush();

    if (pipe_stream.fail()) {
        error_log("%s: Failed to write message to pipe '%s'. Pipe full or other error?",
                  __func__, pipe_path.string().c_str());
    } else {
        debug_log("%s: Sent message to pipe '%s'.", __func__, pipe_path.string().c_str());
    }
}

// Helper to check for non-graphical TTY session more explicitly
static bool IsTtySession() {
    const char* display = getenv("DISPLAY");
    const char* wayland_display = getenv("WAYLAND_DISPLAY");
    // If neither display variable is set, likely a TTY.
    return (display == nullptr || strlen(display) == 0) &&
           (wayland_display == nullptr || strlen(wayland_display) == 0);
}

// D-Bus implementation for Gnome (as defined previously)
static int64_t GetIdleTimeWaylandGnomeViaDBus() {
    // Assumes IsGnomeSession() has already confirmed this is appropriate to call
    debug_log("%s: Querying GNOME Mutter IdleMonitor via D-Bus.", __func__);

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    int64_t idle_time_seconds = 0; // Default to 0 (active)

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            debug_log("%s: Error connecting to session bus for Gnome idle query: %s", __func__, dbus_error->message);
            g_error_free(dbus_error);
        } else {
            debug_log("%s: Error connecting to session bus for Gnome idle query (unknown error).", __func__);
        }
        return 0; // Return 0 on connection error
    }

    // Correct D-Bus details for getting idle time from Mutter
    const char* service_name = "org.gnome.Mutter.IdleMonitor";
    const char* object_path = "/org/gnome/Mutter/IdleMonitor/Core";
    const char* interface_name = "org.gnome.Mutter.IdleMonitor";
    const char* method_name = "GetIdletime";

    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name, object_path, interface_name, method_name,
                                              nullptr, G_VARIANT_TYPE("(t)"), G_DBUS_CALL_FLAGS_NONE,
                                              500, nullptr, &dbus_error);

    if (dbus_error) {
        debug_log("%s: Error calling %s on %s: %s", __func__, method_name, interface_name, dbus_error->message);
        g_error_free(dbus_error);
        // Keep idle_time_seconds = 0
    } else if (dbus_result) {
        uint64_t idle_time_ms = 0;
        g_variant_get(dbus_result, "(t)", &idle_time_ms);
        idle_time_seconds = static_cast<int64_t>(idle_time_ms / 1000);
        debug_log("%s: Mutter IdleMonitor reported idle time: %llu ms (%lld seconds)",
                  __func__, (unsigned long long)idle_time_ms, (long long)idle_time_seconds);
        g_variant_unref(dbus_result);
    } else {
        debug_log("%s: Call to %s on %s returned no result and no error.", __func__, method_name, interface_name);
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
    debug_log("%s: Starting Wayland idle monitor.", __func__);
    if (m_initialized.load()) {
        debug_log("%s: Already started.", __func__);
        return true; // Already running
    }

    m_notification_timeout_ms = notification_timeout_ms;

    // Create pipe for interrupting poll()
    // pipe2 is Linux-specific, use pipe() for more portability if needed
    if (pipe2(m_interrupt_pipe_fd, O_CLOEXEC | O_NONBLOCK) == -1) {
        error_log("%s: Failed to create interrupt pipe: %s", __func__, strerror(errno));
        return false;
    }

    if (!InitializeWayland()) {
        error_log("%s: Failed to initialize Wayland or find required protocols.", __func__);
        CleanupWayland(); // Clean up potentially partial initialization
        return false;
    }

    // Check again after roundtrips
    if (!m_seat || !m_idle_notifier) {
        error_log("%s: Required Wayland interfaces (wl_seat, ext_idle_notifier_v1) were not advertised by compositor.", __func__);
        CleanupWayland();
        return false; // Protocol not available
    }

    CreateIdleNotification();
    if (!m_idle_notification) {
        error_log("%s: Failed to create Wayland idle notification object.", __func__);
        CleanupWayland();
        return false;
    }

    m_interrupt_monitor = false; // Set before starting thread

    try {
        m_monitor_thread = std::thread(&WaylandIdleMonitor::WaylandMonitorThread, this);
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Wayland monitor thread: %s", __func__, e.what());
        CleanupWayland(); // Clean up Wayland resources if thread fails to start
        return false;
    }

    m_initialized = true; // Set initialized only after thread starts successfully
    debug_log("%s: Wayland idle monitor started successfully.", __func__);
    return true;
}

void WaylandIdleMonitor::Stop() {
    debug_log("%s: Stopping Wayland idle monitor...", __func__);
    // Use exchange to prevent concurrent Stop calls and get previous state
    if (m_interrupt_monitor.exchange(true)) {
        debug_log("%s: Already stopping or stopped.", __func__);
        // If joinable, maybe still wait? Or assume previous call handles it.
        if(m_monitor_thread.joinable()) {
            // Avoid infinite wait if something went wrong
            auto future = std::async(std::launch::async, &std::thread::join, &m_monitor_thread);
            if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                error_log("%s: Timed out waiting for Wayland thread join during stop.", __func__);
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
            error_log("%s: Failed to write to interrupt pipe: %s", __func__, strerror(errno));
        } else {
            debug_log("%s: Sent interrupt signal via pipe.", __func__);
        }
    }

    if (m_monitor_thread.joinable()) {
        try {
            m_monitor_thread.join();
            debug_log("%s: Wayland monitor thread joined.", __func__);
        } catch (const std::system_error& e) {
            error_log("%s: Error joining Wayland monitor thread: %s", __func__, e.what());
        }
    }

    CleanupWayland(); // Clean up Wayland resources after thread stops
    m_initialized = false; // Mark as not initialized
    debug_log("%s: Wayland idle monitor stopped.", __func__);
}


bool WaylandIdleMonitor::InitializeWayland() {
    debug_log("%s: Initializing Wayland connection.", __func__);
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        error_log("%s: Failed to connect to Wayland display.", __func__);
        return false;
    }

    m_registry = wl_display_get_registry(m_display);
    if (!m_registry) {
        error_log("%s: Failed to get Wayland registry.", __func__);
        wl_display_disconnect(m_display);
        m_display = nullptr;
        return false;
    }

    // Add listener using the pointer defined in the header, pointing to the actual C-style struct
    wl_registry_add_listener(m_registry, (const wl_registry_listener*)c_registry_listener_ptr, this);

    // Initial roundtrip to get globals list
    if (wl_display_roundtrip(m_display) == -1) {
        error_log("%s: Wayland display roundtrip failed during initialization.", __func__);
        CleanupWayland();
        return false;
    }
    // Sometimes a second roundtrip is needed for objects bound in the first
    if (wl_display_roundtrip(m_display) == -1) {
        error_log("%s: Second Wayland display roundtrip failed during initialization.", __func__);
        CleanupWayland();
        return false;
    }


    // Check if required interfaces were bound by the listener during roundtrips
    return (m_seat != nullptr && m_idle_notifier != nullptr);
}

void WaylandIdleMonitor::CleanupWayland() {
    debug_log("%s: Cleaning up Wayland resources.", __func__);
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
        debug_log("%s: Cannot create idle notification (missing deps or already exists).", __func__);
        return;
    }

    // Use get_input_idle_notification if version >= 2 to ignore inhibitors? Decide based on need.
    // For now, use the standard one.
    m_idle_notification = ext_idle_notifier_v1_get_idle_notification(
        m_idle_notifier,
        m_notification_timeout_ms,
        m_seat);

    if (!m_idle_notification) {
        error_log("%s: ext_idle_notifier_v1_get_idle_notification failed.", __func__);
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
        error_log("%s: wl_display_flush failed after adding notification listener.", __func__);
    }

    debug_log("%s: Created and added listener for idle notification object (timeout %d ms).", __func__, m_notification_timeout_ms);
}

void WaylandIdleMonitor::WaylandMonitorThread() {
    debug_log("%s: Wayland monitor thread started.", __func__);

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
                error_log("%s: wl_display_dispatch_pending() failed in prepare loop. Exiting thread.", __func__);
                goto thread_exit;
            }
        }

        // Flush requests to the server
        if (wl_display_flush(m_display) == -1 && errno != EAGAIN) {
            error_log("%s: wl_display_flush() failed: %s. Exiting thread.", __func__, strerror(errno));
            wl_display_cancel_read(m_display); // Cancel before exiting
            goto thread_exit;
        }

        // Block in poll until Wayland event or interrupt pipe event
        poll_ret = poll(fds, 2, -1); // No timeout, block indefinitely

        if (poll_ret < 0) {
            // Error in poll
            if (errno == EINTR) {
                debug_log("%s: poll() interrupted by signal, continuing.", __func__);
                wl_display_cancel_read(m_display); // Cancel read before looping
                continue; // Loop again
            } else {
                error_log("%s: poll() failed: %s. Exiting thread.", __func__, strerror(errno));
                wl_display_cancel_read(m_display);
                goto thread_exit;
            }
        }

        // Check if interrupted via pipe (check before Wayland FD)
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            debug_log("%s: Interrupt or pipe error detected.", __func__);
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
                error_log("%s: Error/Hangup on Wayland display FD. Exiting thread.", __func__);
                // No need to cancel read, connection is likely dead
                goto thread_exit;
            }

            // If POLLIN, read events
            if (wl_display_read_events(m_display) == -1) {
                error_log("%s: wl_display_read_events() failed. Exiting thread.", __func__);
                goto thread_exit;
            }

            // Dispatch the read events
            // Use dispatch_queue_pending if using separate event queues
            if (wl_display_dispatch_pending(m_display) == -1) {
                error_log("%s: wl_display_dispatch_pending() failed after read. Exiting thread.", __func__);
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
            debug_log("%s: Interrupt detected after dispatch.", __func__);
            break;
        }
    } // end while

thread_exit:
    debug_log("%s: Wayland monitor thread exiting.", __func__);
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
    debug_log("WaylandRegistry: Global: %s v%u (name %u)", interface, version, name);

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        monitor->m_seat_id = name;
        // Bind wl_seat, version 5 needed for release request
        uint32_t bind_version = std::min(version, 5u);
        monitor->m_seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, bind_version)
            );
        debug_log("WaylandRegistry: Bound wl_seat (name %u) version %u.", name, bind_version);
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        monitor->m_idle_notifier_id = name;
        // Bind ext_idle_notifier_v1, version 2 allows ignoring inhibitors if needed
        uint32_t bind_version = std::min(version, 2u);
        monitor->m_idle_notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bind_version)
            );
        debug_log("WaylandRegistry: Bound %s (name %u) version %u.", ext_idle_notifier_v1_interface.name, name, bind_version);
    }
}

void WaylandIdleMonitor_HandleGlobalRemove(void *data, wl_registry * /* registry */, uint32_t name)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    debug_log("WaylandRegistry: Global removed: %u", name);
    // Handle globals being removed if necessary (e.g., seat removed)
    // This might require more complex state management or re-initialization logic
    if (name == monitor->m_seat_id) {
        error_log("WaylandRegistry: Monitored wl_seat (name %u) was removed!", name);
        monitor->m_seat = nullptr; // Mark as gone
    } else if (name == monitor->m_idle_notifier_id) {
        error_log("WaylandRegistry: Idle notifier global (name %u) was removed!", name);
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
        debug_log("WaylandIdleNotify: Idle state entered at %lld", monitor->m_idle_start_time.load(std::memory_order_relaxed));
    }
}

void WaylandIdleMonitor_HandleResumed(void *data, ext_idle_notification_v1 * /* notification */)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    // Use exchange to ensure we only log on transition
    if (monitor->m_is_idle.exchange(false, std::memory_order_relaxed)) {
        monitor->m_idle_start_time.store(0, std::memory_order_relaxed); // Reset start time
        debug_log("WaylandIdleNotify: Idle state exited (resumed).");
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




// --- Update GetIdleTimeSeconds ---
int64_t GetIdleTimeSeconds() {
    if (IsTtySession()) {
        debug_log("%s: TTY session detected, idle check not applicable.", __func__);
        return 0;
    } else if (IsWaylandSession()) {
        debug_log("%s: Wayland session detected.", __func__);
        // Check if the Wayland monitor using the protocol is available and initialized
        if (g_wayland_idle_monitor.IsAvailable()) {
            debug_log("%s: Using WaylandIdleMonitor (ext-idle-notify-v1).", __func__);
            return g_wayland_idle_monitor.GetIdleSeconds();
        }
        // --- Fallback Logic ---
        // Only try D-Bus for Gnome if Wayland monitor failed/unavailable
        else if (IsGnomeSession()) {
            debug_log("%s: WaylandIdleMonitor unavailable, falling back to D-Bus for Gnome.", __func__);
            return GetIdleTimeWaylandGnomeViaDBus(); // Ensure this function exists
        }
        // Add other Wayland fallbacks here if needed (e.g., check for deprecated kde-idle?)
        else {
            debug_log("%s: WaylandIdleMonitor unavailable, no other Wayland method implemented/available.", __func__);
            return 0; // No Wayland method worked
        }
    } else { // Assume X11
        debug_log("%s: X11 session detected. Using XScreenSaver.", __func__);
        // --- X11 implementation as before ---
        Display* display = XOpenDisplay(nullptr);
        if (!display) { error_log("%s: Could not open X display.", __func__); return 0; }
        int event_base, error_base;
        if (!XScreenSaverQueryExtension(display, &event_base, &error_base)) { error_log("%s: XScreenSaver extension unavailable.", __func__); XCloseDisplay(display); return 0; }
        XScreenSaverInfo* info = XScreenSaverAllocInfo();
        if (!info) { error_log("%s: Could not allocate XScreenSaverInfo.", __func__); XCloseDisplay(display); return 0; }
        Window root = DefaultRootWindow(display);
        XScreenSaverQueryInfo(display, root, info);
        int64_t idle_time_ms = info->idle;
        XFree(info);
        XCloseDisplay(display);
        int64_t idle_time_seconds = idle_time_ms / 1000;
        debug_log("%s: XScreenSaver reported: %lld ms (%lld seconds)", __func__, (long long)idle_time_ms, (long long)idle_time_seconds);
        return idle_time_seconds;
        // --- End X11 implementation ---
    }
}


} // namespace IdleDetect


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
    bool should_update_event_detect = false;
    fs::path event_data_path;
    int initial_sleep = 0;

    try {
        // Assuming GetArg is PascalCase
        idle_threshold_seconds = std::get<int>(g_config.GetArg("inactivity_time_trigger"));
        should_update_event_detect = std::get<bool>(g_config.GetArg("update_event_detect"));
        event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));
        initial_sleep = std::get<int>(g_config.GetArg("initial_sleep"));
        // ... get other config vars ...
    } catch (const std::bad_variant_access& e) {
        error_log("%s: Configuration value missing or has wrong type: %s. Using defaults where possible.",
                  __func__,
                  e.what());
    } catch (...) {
        error_log("%s: main: Unknown error accessing configuration values.", __func__);
        return 1;
    }

    fs::path pipe_path = event_data_path / "event_registration_pipe";

    log("Idle Detect started.");
    debug_log("INFO: main: Idle threshold: %d seconds, Check interval: %d seconds, Update event_detect: %s, Pipe path: %s",
              idle_threshold_seconds,
              check_interval_seconds,
              should_update_event_detect ? "true" : "false",
              pipe_path.string());

    if (initial_sleep > 0) {
        log("INFO: %s: Waiting for %d seconds to start.",
            __func__,
            initial_sleep);

        std::this_thread::sleep_for(std::chrono::seconds(initial_sleep));
    }

    bool use_wayland_monitor = false;
    if (IdleDetect::IsWaylandSession()) {
        // Get timeout from config, convert seconds to ms
        // Use a reasonably short timeout for notification. Let's use inactivity_time_trigger for now.
        int notification_timeout_ms = IdleDetect::DEFAULT_IDLE_THRESHOLD_SECONDS * 1000; // Configurable?

        // Start the Wayland monitor; check if successful
        if (g_wayland_idle_monitor.Start(notification_timeout_ms)) {
            debug_log("main: Wayland idle monitor started successfully.");
            use_wayland_monitor = true;
            // Need to handle KDE vs Gnome fallback logic based on IsAvailable()
            // GetIdleTimeSeconds now incorporates the check
        } else {
            error_log("main: Failed to start Wayland idle monitor. Will rely on fallbacks if available.");
        }
    }


    // --- Main Loop ---
    while (true) {
        // FunctionName is PascalCase
        int64_t idle_seconds = IdleDetect::GetIdleTimeSeconds();
        // Logging function call is snake_case
        debug_log("DEBUG: main: Current idle time: %lld seconds", idle_seconds);

        if (idle_seconds >= idle_threshold_seconds) {
            // Logging function call is snake_case
            log("INFO: main: User is idle (%lld seconds >= %d threshold)", idle_seconds, idle_threshold_seconds);
            // Add logic to execute idle_command
        } else {
            // Logging function call is snake_case
            debug_log("INFO: main: User is active (%lld seconds < %d threshold)", idle_seconds, idle_threshold_seconds);
            // Add logic to execute active_command

            if (should_update_event_detect) {
                // FunctionName is PascalCase
                IdleDetect::SendPipeNotification(pipe_path);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(check_interval_seconds));
    }

    // --- Shutdown sequence in main ---
    // ... (before exiting) ...
    if (use_wayland_monitor) { // Or check g_wayland_idle_monitor.IsAvailable() again?
        g_wayland_idle_monitor.Stop();
    }

    return 0;
}
