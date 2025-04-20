/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_detect.h>
#include <optional>
#include <util.h> // Includes tinyformat.h, filesystem, etc.
#include <release.h>

// Standard Libs
#include <cstdlib>     // For getenv(), system()
#include <cstdint>     // For int64_t, uint64_t
#include <cstring>     // For strcmp, strerror
#include <chrono>      // For std::chrono
#include <thread>      // For std::this_thread, std::thread
#include <atomic>      // For std::atomic
#include <fstream>     // For std::ofstream
#include <system_error>// For std::error_code
#include <csignal>     // For signal handling (sigaction etc)
#include <variant>     // For std::get
#include <cerrno>       // For errno

// Platform Specific Libs
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>    // For pipe, read, write, close, getenv, sleep
#include <sys/types.h> // Usually included by others
#include <sys/mman.h>   // For mmap, munmap, shm_open
#include <sys/stat.h>   // For mode constants with shm_open
#include <sys/wait.h>  // Usually included by others
#include <poll.h>      // For poll()
#include <fcntl.h>     // For O_NONBLOCK, O_CLOEXEC

// D-Bus Libs (if needed for fallback)
#include <gio/gio.h>


//! Global config singleton for idle_detect
IdleDetectConfig g_config;

//! Global idle_detect event monitor singleton for state overrides
IdleDetect::IdleDetectControlMonitor g_idle_detect_control_monitor;

//! Global flag for signal handling
std::atomic<bool> g_shutdown_requested = false;

//! Global flag for exit code
std::atomic<int> g_exit_code;

const int MAX_X_CONNECT_RETRIES = 6;  // e.g., 6 attempts
const int X_RETRY_DELAY_MS = 500;   // e.g., 500ms between attempts (~3 sec total)

//! \brief Function to safely get XDG_RUNTIME_DIR environment variable.
std::optional<std::string> GetXdgRuntimeDir() {
    return GetEnvVariable("XDG_RUNTIME_DIR");
}

void Shutdown(const int& exit_code)
{
    g_exit_code.store(exit_code);
    g_shutdown_requested.store(true);
}

// IdleDetectConfig class

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

    // shmem_name

    m_config.insert(std::make_pair("shmem_name", GetArgString("shmem_name", "/idle_detect_shmem")));

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
//!
//! \brief DEFAULT_IDLE_THRESHOLD_SECONDS is actually set in main() from the config file and ProcessArgs()
//! has the default value. It is initialized to zero here.
//!
int DEFAULT_IDLE_THRESHOLD_SECONDS = 0;

//!
//! \brief DEFAULT_CHECK_INTERVAL_SECONDS is set at 1 second and is not configurable.
//!
constexpr int DEFAULT_CHECK_INTERVAL_SECONDS = 1;

//!
//! \brief Helper function to determine whether GUI session is Wayland.
//! \return true if GUI session is Wayland.
//!
static bool IsWaylandSession() {
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    return (waylandDisplay != nullptr && strlen(waylandDisplay) > 0);
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

//!
//! \brief Helper function to read from shared memory.
//! \param shm_name
//! \return
//!
static int64_t ReadTimestampViaShmem(const std::string& shm_name) {
    if (shm_name.empty() || shm_name[0] != '/') {
        error_log("%s: Invalid shared memory name provided: %s", __func__, shm_name.c_str());
        return -1;
    }
    debug_log("INFO: %s: Attempting to read timestamp from shm: %s", __func__, shm_name.c_str());

    const size_t shmem_size = sizeof(int64_t[2]);
    int shm_fd = -1;
    void* mapped_mem = MAP_FAILED;
    int64_t* shm_ptr = nullptr;
    int64_t last_active_timestamp = -1;

    errno = 0;
    shm_fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (shm_fd == -1) {
        if (errno != ENOENT) {
            error_log("%s: shm_open(RO) failed for '%s': %s (%d)", __func__, shm_name.c_str(), strerror(errno), errno);
        }
        else { debug_log("INFO: %s: Shared memory '%s' not found (ENOENT).", __func__, shm_name.c_str()); }
        return -1;
    }

    errno = 0;
    mapped_mem = mmap(nullptr, shmem_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (mapped_mem == MAP_FAILED) {
        error_log("%s: mmap(RO) failed for shm '%s': %s (%d)", __func__, shm_name.c_str(), strerror(errno), errno);
        return -1;
    }

    shm_ptr = static_cast<int64_t*>(mapped_mem);
    last_active_timestamp = shm_ptr[1]; // Read index [1]
    debug_log("INFO: %s: Read last_active %lld from shm %s", __func__, (int64_t)last_active_timestamp, shm_name.c_str());

    errno = 0;
    if (munmap(mapped_mem, shmem_size) == -1) {
        log("WARN: %s: munmap failed for shm '%s': %s (%d)", __func__, shm_name.c_str(), strerror(errno), errno);
    }

    return last_active_timestamp;
}

/**
 * @brief Sends a notification message to the event_detect named pipe.
 *
 * @param pipe_path The full path to the event_detect named pipe.
 * @param the last active time to send to event_detect
 */
void SendPipeNotification(const std::filesystem::path& pipe_path,
                          const int64_t& last_active_time,
                          const EventMessage::EventType& event_type = EventMessage::EventType::USER_ACTIVE) {
    // Construct the message payload using EventMessage format
    EventMessage msg(last_active_time, event_type);
    if (!msg.IsValid()) {
        error_log("%s: Failed to construct valid EventMessage.",
                  __func__);
        return;
    }

    std::string message_str = msg.ToString() + "\n"; // Add newline for pipe message termination

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

//!
//! \brief Helper function that determines whether session is tty only.
//! \return true if session is tty only (i.e. non-GUI).
//!
static bool IsTtySession() {
    const char* display = getenv("DISPLAY");
    const char* wayland_display = getenv("WAYLAND_DISPLAY");

    // If neither display variable is set, likely a TTY.
    return (display == nullptr || strlen(display) == 0) &&
           (wayland_display == nullptr || strlen(wayland_display) == 0);
}

//!
//! \brief Helper function to get idle time from KDE DBus interface for KDE sessions, either X or Wayland.
//! \return int64_t idle time in seconds. -1 for error.
//!
static int64_t GetIdleTimeKdeDBus() {
    debug_log("INFO: %s: Querying org.kde.ksmserver GetSessionIdleTime via D-Bus.", __func__);

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    int64_t idle_time_seconds = -1; // Default to error/unknown

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            error_log("%s: Failed to connect to session bus for KDE idle query: %s", __func__, dbus_error->message);
            g_error_free(dbus_error);
        } else {
            error_log("%s: Failed to connect to session bus for KDE idle query (unknown error).", __func__);
        }
        return -1; // Return error
    }

    const char* service_name = "org.kde.ksmserver";
    const char* object_path = "/ScreenSaver"; // As determined by user testing
    const char* interface_name = "org.freedesktop.ScreenSaver";
    const char* method_name = "GetSessionIdleTime";

    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name, object_path, interface_name, method_name,
                                              nullptr, // No input parameters
                                              G_VARIANT_TYPE("(u)"), // <--- FIX: Expect uint32 reply type 'u'
                                              G_DBUS_CALL_FLAGS_NONE,
                                              1000, // Timeout 1 sec
                                              nullptr, // Cancellable
                                              &dbus_error);

    if (dbus_error) {
        // Error during D-Bus call
        error_log("%s: Error calling %s on %s: %s", __func__, method_name, interface_name, dbus_error->message);
        g_error_free(dbus_error);
        idle_time_seconds = -1; // Error
    } else if (dbus_result) {
        // Call succeeded, unpack the result
        uint32_t idle_time_ms = 0; // <--- FIX: Use uint32_t to store result
        g_variant_get(dbus_result, "(u)", &idle_time_ms); // <--- FIX: Unpack type 'u'

        // Convert milliseconds to seconds
        idle_time_seconds = static_cast<int64_t>(idle_time_ms / 1000);

        // FIX: Use %u format specifier for uint32_t in log message
        debug_log("INFO: %s: ksmserver GetSessionIdleTime reported: %u ms (%lld seconds)",
                  __func__,
                  idle_time_ms, // Pass uint32_t directly
                  (int64_t)idle_time_seconds); // Cast int64_t for %lld

        g_variant_unref(dbus_result); // Clean up reply variant
    } else {
        // Should not happen if error is null, but handle defensively
        error_log("%s: Call to %s on %s returned no result and no error.", __func__, method_name, interface_name);
        idle_time_seconds = -1; // Treat as error
    }

    g_object_unref(connection); // Clean up connection reference
    return idle_time_seconds;
}

//!
//! \brief This helper function checks for idle inhibited on Gnome sessions and works for both Gnome X and Wayland.
//! \return
//!
static bool CheckGnomeInhibition() {
    // This function assumes it might be called even if not strictly a GNOME session,
    // relying on the D-Bus call to fail gracefully if the service/method isn't present.
    debug_log("INFO: %s: Checking GNOME session inhibitions via D-Bus IsInhibited.", __func__);

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    bool is_inhibited = false; // Default: not inhibited

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            // This error is expected on non-Gnome, log as debug
            debug_log("INFO: %s: Cannot connect to session bus for GNOME inhibit check: %s", __func__, dbus_error->message);
            g_error_free(dbus_error);
        } else {
            debug_log("INFO: %s: Cannot connect to session bus for GNOME inhibit check (unknown error).", __func__);
        }
        return false; // Cannot determine state, assume not inhibited
    }

    // Flags: 1=logout, 2=user-switch, 4=suspend, 8=idle. Check if any are active.
    guint32 flags_to_check = 1 | 2 | 4 | 8; // = 15

    const char* service_name = "org.gnome.SessionManager";
    const char* object_path = "/org/gnome/SessionManager";
    const char* interface_name = "org.gnome.SessionManager";
    const char* method_name = "IsInhibited";

    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name, object_path, interface_name, method_name,
                                              g_variant_new("(u)", flags_to_check), // Input flags
                                              G_VARIANT_TYPE("(b)"), // Expect boolean reply
                                              G_DBUS_CALL_FLAGS_NONE,
                                              500, // Timeout (ms)
                                              nullptr, &dbus_error);

    if (dbus_error) {
        // Method likely doesn't exist or failed - expected on non-Gnome/older Gnome
        debug_log("INFO: %s: Error calling %s: %s (Perhaps not GNOME or method unavailable?)",
                  __func__, method_name, dbus_error->message);
        g_error_free(dbus_error);
        is_inhibited = false; // Assume not inhibited if call fails
    } else if (dbus_result) {
        gboolean inhibited_result = FALSE;
        g_variant_get(dbus_result, "(b)", &inhibited_result);
        is_inhibited = (inhibited_result == TRUE);
        debug_log("INFO: %s: org.gnome.SessionManager.IsInhibited(flags=%u) returned: %s",
                  __func__, flags_to_check, is_inhibited ? "true" : "false");
        g_variant_unref(dbus_result);
    } else {
        // Null result without error is strange
        error_log("%s: Call to %s returned null result without error.", __func__, method_name);
        is_inhibited = false; // Assume not inhibited
    }

    g_object_unref(connection);
    return is_inhibited;
}

//!
//! \brief This is the D-Bus implementation for Gnome Mutter. Note that it does NOT take into account
//! idle inhibit, unlike the corresponding KDE D-Bus call.
//! \return int64_t idle time in seconds. -1 for error.
//!
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
        return -1; // Return -1 on connection error
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
        return -1;
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
        return -1;
    }

    g_object_unref(connection);

    return idle_time_seconds;
}

//!
//! \brief This determines whether the GUI session is KDE.
//! \return true if KDE session, false otherwise.
//!
static bool IsKdeSession() {
    // Checking for KSMServer D-Bus service might be more reliable than env vars
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection) return false; // Cannot check if bus unavailable

    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.DBus", // Standard service
                                                   "/org/freedesktop/DBus", // Standard path
                                                   "org.freedesktop.DBus",   // Standard interface
                                                   "NameHasOwner",           // Method
                                                   g_variant_new("(s)", "org.kde.ksmserver"), // Parameter: service name
                                                   G_VARIANT_TYPE("(b)"),    // Reply: boolean
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   500, // Short timeout
                                                   nullptr, &error);
    bool has_owner = false;
    if (error) {
        debug_log("INFO: %s: Error checking D-Bus owner for org.kde.ksmserver: %s", __func__, error->message);
        g_error_free(error);
    } else if (result) {
        g_variant_get(result, "(b)", &has_owner);
        g_variant_unref(result);
    }
    g_object_unref(connection);
    debug_log("INFO: %s: org.kde.ksmserver D-Bus service running? %s", __func__, has_owner ? "Yes" : "No");
    return has_owner;
    // Alternative: Keep using getenv("KDE_SESSION_VERSION") if preferred/reliable
    // const char* kdeSession = getenv("KDE_SESSION_VERSION");
    // return (kdeSession != nullptr && strlen(kdeSession) > 0);
}

//!
//! \brief Helper function for X11 XScreenSaver query
//! \return int64_t idle time in seconds. -1 for error.
//!
static int64_t GetIdleTimeXss() {
    debug_log("INFO: %s: Using XScreenSaver.", __func__);
    Display* display = nullptr;

    for (int attempt = 1; attempt <= MAX_X_CONNECT_RETRIES; ++attempt) {
        display = XOpenDisplay(nullptr);
        if (display) break;
        if (attempt < MAX_X_CONNECT_RETRIES) {
            error_log("WARNING: %s: Could not open X display (attempt %d/%d). Retrying...", __func__, attempt, MAX_X_CONNECT_RETRIES);
            std::this_thread::sleep_for(std::chrono::milliseconds(X_RETRY_DELAY_MS));
        } else {
            error_log("%s: Could not open X display after %d attempts.", __func__, MAX_X_CONNECT_RETRIES);
            return -1;
        }
    }

    int event_base, error_base;
    if (!XScreenSaverQueryExtension(display, &event_base, &error_base)) {
        error_log("%s: XScreenSaver extension unavailable.", __func__);
        XCloseDisplay(display);
        return -1;
    }
    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    if (!info) {
        error_log("%s: Could not allocate XScreenSaverInfo.", __func__);
        XCloseDisplay(display);
        return -1;
    }
    Window root = DefaultRootWindow(display);
    XScreenSaverQueryInfo(display, root, info);
    int64_t idle_time_ms = info->idle;
    XFree(info);
    XCloseDisplay(display);
    int64_t idle_time_seconds = idle_time_ms / 1000;
    debug_log("INFO: %s: XScreenSaver reported: %lld ms (%lld seconds)", __func__, (int64_t)idle_time_ms, (int64_t)idle_time_seconds);
    return idle_time_seconds;
}

//!
//! \brief This function determines the LOCAL session idle time using appropriate fallback logic based on the current
//! API layout for GUI environments.
//! \return int64_t idle time in seconds. -1 for error, -2 if tty session only.
//!
int64_t GetIdleTimeSeconds() {
    if (IsTtySession()) {
        debug_log("INFO: %s: TTY session detected, idle check not applicable.", __func__);
        // Returns -2 to indicate that idle_detect should use event_detect regardless of the config setting.
        // (Tty monitoring is done in event_detect.)

        return -2;
    }

    if (IsKdeSession()) { // For KDE X or Wayland
        // KDE D-Bus method handles inhibition internally by periodically resetting the idle time returned.
        debug_log("INFO: %s: KDE session detected. Using KDE D-Bus method.", __func__);

        return GetIdleTimeKdeDBus(); // Returns >= 0 or -1
    } else if (IsWaylandSession()) { // Non-KDE Wayland (Try GNOME D-Bus for both idle time and inhibition)
        debug_log("INFO: %s: Non-KDE Wayland session. Checking GNOME D-Bus idle time and inhibition.", __func__);

        if (CheckGnomeInhibition()) { // Check inhibition first - this returns false if call fails.
            debug_log("INFO: %s: GNOME session is inhibited (Wayland), returning 0 idle seconds.", __func__);

            return 0; // Treat as active if inhibited
        } else {
            // Not inhibited, get input idle time from Mutter
            return GetIdleTimeWaylandGnomeViaDBus(); // Returns >= 0 on success, -1 on error
        }
    } else {
        // Non-KDE X11 session
        debug_log("INFO: %s: Non-KDE X11 session. Checking XSS idle time and GNOME D-Bus inhibition.", __func__);
        if (CheckGnomeInhibition()) { // Also check inhibition for X11 (might be Gnome on X11)
            debug_log("INFO: %s: GNOME session is inhibited (X11), returning 0 idle seconds.", __func__);
            return 0; // Treat as active if inhibited
        } else {
            // Not inhibited, get input idle time from XScreenSaver
            return GetIdleTimeXss(); // Returns >= 0 on success, -1 on error
        }
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

IdleDetectControlMonitor::IdleDetectControlMonitor()
    : m_interrupt_idle_detect_control_monitor(false)
    , m_state(UNKNOWN)
{}

void IdleDetectControlMonitor::IdleDetectControlMonitorThread()
{
    debug_log("INFO: %s: started.",
              __func__);

    bool idle_detect_control_pipe_initialized = false;

    // Path for user runtime directory (i.e. /run/user/UID)
    std::optional<fs::path> xdg_runtime_dir = GetXdgRuntimeDir();
    fs::path idle_detect_control_pipe_path;

    // Create the named pipe if it doesn't exist (idempotent)
    if (xdg_runtime_dir) {
        idle_detect_control_pipe_path = xdg_runtime_dir.value() / "idle_detect_control_pipe";

        // Create the named pipe if it doesn't exist (idempotent)
        if (mkfifo(idle_detect_control_pipe_path.c_str(), 0600) == -1) {
            if (errno != EEXIST) {
                error_log("%s: Error creating named pipe: %s",
                          __func__,
                          strerror(errno));
            }
            idle_detect_control_pipe_initialized = true;
        } else {
            idle_detect_control_pipe_initialized = true;
        }
    }

    if (!idle_detect_control_pipe_initialized) {
        error_log("%s: Failed to create named pipe for idle_detect control. Exiting.",
                  __func__);
        Shutdown(1);
        return;
    }

    // Note the mode above combined with the umask does not always result in the right permissions,
    // so we override with the correct ones.

    fs::perms desired_perms = fs::perms::owner_read | fs::perms::owner_write;

    std::error_code ec; // To capture potential errors

    // Set the permissions, replacing existing ones (default behavior)
    fs::permissions(idle_detect_control_pipe_path, desired_perms, fs::perm_options::replace, ec);

    if (ec) {
        // Handle the error if permissions couldn't be set
        error_log("%s: Error setting permissions (0600) on named pipe %s: %s",
                  __func__,
                  idle_detect_control_pipe_path.string().c_str(),
                  ec.message().c_str()); // Use the error_code's message
        Shutdown(1);
        return;
    } else {
        debug_log("%s: Successfully set permissions on %s to 0600.",
                  __func__, idle_detect_control_pipe_path.string().c_str());
    }

    int fd = -1;
    char buffer[256];
    ssize_t bytes_read;
    const int poll_timeout_ms = 100;

    m_initialized = true;
    m_state = NORMAL;

    while (g_exit_code == 0) {
        std::unique_lock<std::mutex> lock(mtx_idle_detect_control_monitor_thread);
        cv_idle_detect_control_monitor_thread.wait_for(lock, std::chrono::milliseconds(100),
                                                       []{ return g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.load(); });

        if (g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor) {
            break;
        }

        lock.unlock();

        // Only execute this if the pipe isn't already open in non-blocking mode.
        if (fd == -1) {
            fd = open(idle_detect_control_pipe_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd == -1) {
                if (errno != ENXIO) { // No process has the pipe open for writing
                    error_log("%s: Error opening named pipe for reading (non-blocking): %s",
                              __func__,
                              strerror(errno));
                    // Consider a longer sleep here to avoid rapid retries on other errors
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                continue; // Try to open again in the next iteration
            } else {
                debug_log("INFO: %s: Successfully opened pipe for reading (non-blocking).",
                          __func__);
            }
        }

        if (fd != -1) {
            struct pollfd fds[1];
            fds[0].fd = fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;

            int ret = poll(fds, 1, poll_timeout_ms);

            if (ret > 0) {
                if (fds[0].revents & POLLIN) {
                    bytes_read = read(fd, buffer, sizeof(buffer) - 1);

                    if (bytes_read > 0) {
                        buffer[bytes_read] = '\0';
                        std::string event_data(buffer);
                        debug_log("INFO: %s: Received data: %s", __func__,
                                  event_data);

                        std::stringstream ss(event_data);
                        std::string segment;
                        std::vector<std::string> parts;
                        while (std::getline(ss, segment, ':')) {
                            parts.push_back(segment);
                        }

                        if (parts.size() == 2) {
                            try {
                                EventMessage event(TrimString(parts[0]), TrimString(parts[1]));

                                debug_log("INFO: %s: event.m_timestamp = %lld, event.m_event_type = %s",
                                          __func__,
                                          event.m_timestamp,
                                          event.EventTypeToString());

                                if (event.IsValid()) {
                                    debug_log("INFO: %s: Valid override event received with timestamp %lld",
                                              __func__,
                                              event.m_timestamp);

                                    if (event.m_event_type == EventMessage::USER_UNFORCE) {
                                        m_state = NORMAL;
                                    } else if (event.m_event_type == EventMessage::USER_FORCE_IDLE) {
                                        m_state = FORCED_IDLE;
                                    } else if (event.m_event_type == EventMessage::USER_FORCE_ACTIVE) {
                                        m_state = FORCED_ACTIVE;
                                    }

                                    debug_log("INFO: %s: Current idle detect monitor override time %lld, state %s",
                                              __func__,
                                              event.m_timestamp,
                                              StateToString());
                                } else {
                                    error_log("%s: Invalid event data received: %s",
                                              __func__,
                                              event_data);
                                }
                            } catch (const std::invalid_argument& e) {
                                error_log("%s: Error parsing timestamp: %s in data %s",
                                          __func__,
                                          e.what(),
                                          event_data);
                            } catch (const std::out_of_range& e) {
                                error_log("%s: Timestamp out of range: %s in data %s",
                                          __func__,
                                          e.what(),
                                          event_data);
                            }
                        } else if (bytes_read == 0) {
                            // Pipe closed by writers, sleep a bit to avoid spinning
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        } else if (bytes_read < 0) {
                            if (errno == EINTR) {
                                // Interrupted by a signal, this is expected during shutdown
                                debug_log("INFO: %s: Read interrupted by signal.",
                                          __func__);
                                break;
                            } else {
                                error_log("%s: Error reading from named pipe: %s",
                                          __func__,
                                          strerror(errno));
                                break;
                            }
                        }
                    }
                } else if (ret < 0) {
                    error_log("%s: Error in poll() for pipe read: %s",
                              __func__,
                              strerror(errno));

                    g_exit_code = 1;
                    break;
                } // ret = 0 means poll timeed out. Allow while loop to iterate.
            }
        }
    }

    if (fd != -1) {
        close(fd);
    }

    debug_log("INFO: %s: thread exiting.",
              __func__);

    // If g_exit_code was set to 1 above then the thread is in abnormal state at exit and the rest of the
    // application needs to be shutdown.
    if (g_exit_code == 1) {
        Shutdown(1);
    }
}

bool IdleDetectControlMonitor::IsInitialized() const
{
    return m_initialized.load();
}


IdleDetectControlMonitor::State IdleDetectControlMonitor::GetState() const
{
    return m_state.load();
}

std::string IdleDetectControlMonitor::StateToString(const State& status)
{
    std::string out;

    switch (status) {
    case UNKNOWN:
        out = "UNKNOWN";
        break;
    case NORMAL:
        out = "NORMAL";
        break;
    case FORCED_ACTIVE:
        out = "FORCED_ACTIVE";
        break;
    case FORCED_IDLE:
        out = "FORCED_IDLE";
        break;
    }

    return out;
}

std::string IdleDetectControlMonitor::StateToString() const
{
    return StateToString(m_state);
}

} // namespace IdleDetect

//!
//! \brief Signal handler
//! \param signum
//!
void HandleSignal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        // Use log() here as it's presumably safe enough during shutdown signal
        log("INFO: %s: Received signal %d. Requesting shutdown.",
            __func__,
            signum);
        g_shutdown_requested.store(true);

        g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.store(true);
        g_idle_detect_control_monitor.cv_idle_detect_control_monitor_thread.notify_all();
    } else {
        log("INFO: %s: Received unexpected signal %d.",
            __func__,
            signum);
    }
}

//!
//! \brief main
//! \param argc
//! \param argv. Currently one argument is expected, which is the config file path.
//! \return exit code, 0 for normal, non-zero otherwise.
//!
int main(int argc, char* argv[])
{
    const char* journal_stream = getenv("JOURNAL_STREAM");
    if (journal_stream != nullptr && strlen(journal_stream) > 0) {
        // If JOURNAL_STREAM is set, assume output is handled by journald
        g_log_timestamps.store(false);
        // Optional: Log that internal timestamps are disabled
        // std::cout << "INFO: Detected systemd journal logging, disabling internal timestamps." << std::endl;
        // (Use cout directly here as logging itself might not be fully set up)
    } else {
        // Not running under journal (or var not set), keep internal timestamps
        g_log_timestamps.store(true);
    }

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
    bool execute_dc_control_scripts = true;
    std::string active_command;
    std::string idle_command;
    std::string shmem_name = "/event_detect_last_active"; // Default
    bool use_event_detect = true; // This flag now controls fallback via file OR shmem
    std::string last_active_time_cpp_filename;

    try {
        idle_threshold_seconds = std::get<int>(g_config.GetArg("inactivity_time_trigger"));
        should_update_event_detect = std::get<bool>(g_config.GetArg("update_event_detect"));
        event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));
        active_command = std::get<std::string>(g_config.GetArg("active_command"));
        idle_command = std::get<std::string>(g_config.GetArg("idle_command"));
        execute_dc_control_scripts = std::get<bool>(g_config.GetArg("execute_dc_control_scripts"));
        shmem_name = std::get<std::string>(g_config.GetArg("shmem_name"));
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

    fs::path event_registration_pipe_path = event_data_path / "event_registration_pipe";
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

    pid_t current_pid = getpid();

    log("INFO: %s: idle_detect C++ program, %s, started, pid %i",
        __func__,
        g_version,
        current_pid);

    debug_log("INFO: %s: Idle threshold: %d seconds, Check interval: %d seconds",
              __func__,
              idle_threshold_seconds,
              check_interval_seconds);
    debug_log("INFO: %s: Update event_detect: %s, Pipe path: %s",
              __func__,
              should_update_event_detect ? "true" : "false",
              event_registration_pipe_path.string());
    debug_log("INFO: %s: Execute dc control scripts: %s",
              __func__,
              execute_dc_control_scripts ? "true" : "false");
    debug_log("INFO: %s: Active command: '%s'",
              __func__,
              active_command);
    debug_log("INFO: %s: Idle command: '%s'",
              __func__,
              idle_command);

    // --- Start Idle Detect Control Monitor Thread ---
    log("INFO: %s: Starting Idle Detect Control Monitor thread...", __func__);
    // Reset interrupt flag *before* starting thread
    g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.store(false);
    bool control_monitor_started = false;
    try {
        // Launch the thread using the member function pointer and the global instance
        g_idle_detect_control_monitor.m_idle_detect_control_monitor_thread =
            std::thread(&IdleDetect::IdleDetectControlMonitor::IdleDetectControlMonitorThread,
                        std::ref(g_idle_detect_control_monitor));
        control_monitor_started = true; // Assume success if no exception
        // Wait a very short time to allow pipe creation/initialization perhaps
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!g_idle_detect_control_monitor.IsInitialized()) {
            log("WARN: %s: Control monitor thread started but not initialized quickly.", __func__);
        } else {
            debug_log("DEBUG: %s: Control monitor thread started and initialized.", __func__);
        }
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Idle Detect Control Monitor thread: %s. Exiting.", __func__, e.what());
        // Stop other monitors if started (e.g., Wayland) before exiting
        // if (wayland_monitor_started) { g_wayland_idle_monitor.Stop(); }
        return 1;
    } catch (...) {
        error_log("%s: Unknown error starting Idle Detect Control Monitor thread. Exiting.", __func__);
        // if (wayland_monitor_started) { g_wayland_idle_monitor.Stop(); }
        return 1;
    }

    // --- Main Loop ---
    bool was_previously_idle = false; // Track state changes
    IdleDetect::IdleDetectControlMonitor::State previous_control_state = IdleDetect::IdleDetectControlMonitor::UNKNOWN;

    //bool send_forced_idle = false;
    int64_t effective_last_active_time_prev = 0;
    int64_t effective_last_active_time = 0;
    bool using_event_detect_as_only_source = false;

    while (!g_shutdown_requested.load()) {
        int64_t idle_seconds = IdleDetect::GetIdleTimeSeconds();

        if (idle_seconds >= 0) {
            debug_log("INFO: %s: idle time from GUI session: %lld seconds.",
                      __func__,
                      (int64_t)idle_seconds);
        } else if (idle_seconds == -2 && !use_event_detect) {
            debug_log("INFO: %s: Tty session. Overriding use_event_detect and using event_detect anyway.",
                      __func__);
            using_event_detect_as_only_source = true;
        }

        // This is how tty idle is captured -- use_event_detect defaults to true and is overridden to true if this is
        // a tty session regardless of the config setting, since tty information is in event_detect.
        if (use_event_detect || using_event_detect_as_only_source) {
            debug_log("INFO: %s: Attempting to use event_detect via shared memory: %s", __func__, shmem_name.c_str());
            int64_t shmem_timestamp = IdleDetect::ReadTimestampViaShmem(shmem_name);

            if (shmem_timestamp >= 0) { // Use >= 0 check, as 0 might be valid initial state
                int64_t current_time = GetUnixEpochTime();
                int64_t calculated_idle = current_time - shmem_timestamp;
                calculated_idle = (calculated_idle > 0) ? calculated_idle : 0; // Ensure non-negative

                // If idle_seconds is < 0, this indicates an error state from IdleDetect::GetIdleTimeSeconds(), or
                // tty only session if -2, in which case the value of idle_seconds should not be used directly.
                if (idle_seconds >= 0) {
                    idle_seconds = std::min(idle_seconds, calculated_idle);
                } else {
                    idle_seconds = calculated_idle;
                }

                debug_log("INFO: %s: idle time including info from event_detect via shmem: %lld seconds "
                          "(current: %lld, shmem: %lld)",
                          __func__,
                          (int64_t)idle_seconds,
                          (int64_t)current_time,
                          (int64_t)shmem_timestamp);
            } else {
                // ReadTimestampViaShmem returns -1 on error
                debug_log("INFO: %s: Attempting to get idle informationi from event_detect via file: %s",
                          __func__,
                          dat_file_path.string());

                int64_t file_timestamp = IdleDetect::ReadLastActiveTimeFile(dat_file_path);
                if (file_timestamp > 0) {
                    int64_t current_time = GetUnixEpochTime();
                    int64_t calculated_idle = current_time - file_timestamp;
                    calculated_idle = (calculated_idle > 0) ? calculated_idle : 0; // Ensure non-negative

                    // If idle_seconds is < 0, this indicates an error state from IdleDetect::GetIdleTimeSeconds(), or
                    // tty only session if -2, in which case the value of idle_seconds should not be used directly.
                    if (idle_seconds >= 0) {
                        idle_seconds = std::min(idle_seconds, calculated_idle);
                    } else {
                        idle_seconds = calculated_idle;
                    }

                    debug_log("INFO: %s: idle time including info from event_detect via file: %lld seconds "
                              "(current: %lld, file: %lld)",
                              __func__,
                              (int64_t)idle_seconds,
                              (int64_t)current_time,
                              (int64_t)file_timestamp);
                } else {
                    error_log("%s: Getting idle_info from event_detect failed: Could not read/parse valid timestamp "
                              "from event_detect file.",
                              __func__);
                }
            }
        }

        if (idle_seconds < 0) {
            error_log("%s: Idle time could not be determined from any available source. Assuming active.",
                      __func__);

            idle_seconds = 0;
        }

        // --- State Calculation & Actions (using effective idle_seconds) ---
        bool is_currently_idle = (idle_seconds >= idle_threshold_seconds);
        IdleDetect::IdleDetectControlMonitor::State control_state = g_idle_detect_control_monitor.GetState();

        debug_log("INFO: %s: Current control idle_detect control state: %s",
                  __func__,
                  g_idle_detect_control_monitor.StateToString());

        // --- Check for forced idle/active state ---
        if (control_state == IdleDetect::IdleDetectControlMonitor::FORCED_IDLE) {
            if (!is_currently_idle) debug_log("INFO: %s: Overriding state to IDLE due to control monitor.", __func__);
            is_currently_idle = true;
        } else if (control_state == IdleDetect::IdleDetectControlMonitor::FORCED_ACTIVE) {
            if (is_currently_idle) debug_log("INFO: %s: Overriding state to ACTIVE due to control monitor.", __func__);
            is_currently_idle = false;
        }

        debug_log("INFO: %s: Effective idle time: %lld seconds. State: %s",
                  __func__,
                  (int64_t)idle_seconds,
                  is_currently_idle ? "Idle" : "Active");

        // --- Handle State Change for Commands ---
        if (is_currently_idle != was_previously_idle) {
            if (is_currently_idle) {
                // Became Idle
                log("INFO: %s: User became idle (%llds >= %ds).",
                    __func__,
                    (int64_t)idle_seconds, idle_threshold_seconds);

                if (execute_dc_control_scripts) {
                    IdleDetect::ExecuteCommandBackground(idle_command);
                }
            } else {
                // Became Active
                log("INFO: %s: User became active (%llds < %ds).",
                    __func__,
                    (int64_t)idle_seconds,
                    idle_threshold_seconds);

                if (execute_dc_control_scripts) {
                    IdleDetect::ExecuteCommandBackground(active_command);
                }
            }

            was_previously_idle = is_currently_idle; // Update previous state
        }

        // Send Pipe Notification if not currently idle and should update event detect and
        // effective last active time has changed. Note if using_event_detect_as_only_source is true, then
        // the pipe message should not be sent as that would be circular and a waste of bandwidth on the
        // pipe.
        //
        // With the addition of forced overrides, the message to propagate to event_detect must be sent when
        // a change in override state occurs, regardless of the effective last active time.
        effective_last_active_time = GetUnixEpochTime() - idle_seconds;

        if ((!is_currently_idle
             && should_update_event_detect
             && using_event_detect_as_only_source == false
             && (effective_last_active_time != effective_last_active_time_prev))
            || control_state != previous_control_state) {
            debug_log("INFO: %s: Sending active notification to pipe.", __func__);

            EventMessage::EventType event_type = EventMessage::USER_ACTIVE;

            if (control_state != previous_control_state) {
                switch(control_state) {
                case IdleDetect::IdleDetectControlMonitor::NORMAL:
                    event_type = EventMessage::USER_UNFORCE;
                    break;
                case IdleDetect::IdleDetectControlMonitor::FORCED_IDLE:
                    event_type = EventMessage::USER_FORCE_IDLE;
                    break;
                case IdleDetect::IdleDetectControlMonitor::FORCED_ACTIVE:
                    event_type = EventMessage::USER_FORCE_ACTIVE;
                    break;
                default:
                    event_type = EventMessage::UNKNOWN;
                    break;
                }

                previous_control_state = control_state;

                debug_log("INFO: %s: Sending forced state notification to pipe: %s",
                          __func__,
                          IdleDetect::IdleDetectControlMonitor::StateToString(control_state));
            }

            IdleDetect::SendPipeNotification(event_registration_pipe_path, effective_last_active_time, event_type);

            effective_last_active_time_prev = effective_last_active_time;

        }

        // Sleep for the check interval, but check for shutdown periodically
        // This loop sleeps for check_interval_seconds total, checking every 100ms
        auto wake_up_time = std::chrono::steady_clock::now() + std::chrono::seconds(check_interval_seconds);
        while (std::chrono::steady_clock::now() < wake_up_time) {
            if (g_shutdown_requested.load()) {
                debug_log("INFO: %s: Shutdown requested during sleep interval.",
                          __func__);
                break; // Exit inner sleep loop
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (g_shutdown_requested.load()) {
            break; // Exit main loop
        }
    } // End main loop

    log("INFO: %s: Shutdown requested. Cleaning up...",
        __func__);

    // --- Shutdown sequence ---

    // --- Stop Idle Detect Control Monitor thread ---
    if (control_monitor_started && g_idle_detect_control_monitor.m_idle_detect_control_monitor_thread.joinable()) {
        log("INFO: %s: Stopping Idle Detect Control Monitor thread...", __func__);
        // Flag should have been set by HandleSignal
        g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.store(true);
        g_idle_detect_control_monitor.cv_idle_detect_control_monitor_thread.notify_all();
        try {
            g_idle_detect_control_monitor.m_idle_detect_control_monitor_thread.join();
            log("INFO: %s: Idle Detect Control Monitor thread stopped.", __func__);
        } catch (const std::system_error& e) {
            error_log("ERROR: %s: Error joining control monitor thread: %s", __func__, e.what());
        }
    }

    log("Idle Detect shutdown complete.");
    return g_exit_code.load();
}
