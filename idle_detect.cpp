/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_detect.h>
#include <idle_sources_system.h>
#include <optional>
#include <session_discovery.h>
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
#include <cerrno>      // For errno
#include <future>      // For std::async in Stop() timeout
#include <filesystem> // Needed for first-run config copy logic
#include <mutex>       // For std::call_once installing the X error handlers
#include <csetjmp>     // For sigsetjmp/siglongjmp in the pre-libX11-1.7 I/O error fallback

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
#include <wayland-client.h>
#include "ext-idle-notify-v1-protocol.h" // Generated header

// D-Bus Libs (if needed for fallback)
#include <gio/gio.h>


//! Global config singleton for idle_detect
IdleDetectConfig g_config;

//! Global idle_detect event monitor singleton for state overrides
IdleDetect::IdleDetectControlMonitor g_idle_detect_control_monitor;

//!
//! \brief Global pool of live idle sources. Declared in idle_sources_system.h.
//!
//! The production factories are bound here, which is the only place in the program where the pool's abstract
//! interface meets real D-Bus, Wayland and X11 contact. None of them makes system contact at construction, so
//! this is safe to build at static initialization time; the pool is empty until main() reconciles it against
//! discovery for the first time.
//!
IdleDetect::IdleSourcePool IdleDetect::g_idle_source_pool(
    IdleDetect::MakeEndpointSourceFactory(IdleDetect::WAYLAND_IDLE_NOTIFICATION_TIMEOUT_MS),
    IdleDetect::MakeShellSourceFactory(),
    IdleDetect::MakeInhibitionQuery());

//! Global flag for signal handling
std::atomic<bool> g_shutdown_requested = false;

//! Global flag for exit code
std::atomic<int> g_exit_code;

// The attempt count is no longer a constant: it is a parameter of GetIdleTimeXss(), defaulting to
// IdleDetect::X_STARTUP_CONNECT_RETRIES. See idle_detect.h for why.
const int X_RETRY_DELAY_MS = 500;     // e.g., 500ms between attempts

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

    std::string execute_dc_control_scripts_arg = GetArgString("execute_dc_control_scripts", "false");

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
        normal_log("WARN: %s: munmap failed for shm '%s': %s (%d)", __func__, shm_name.c_str(), strerror(errno), errno);
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

// Declared in idle_detect.h. Not static: idle_sources_system.cpp calls this for the shell idle source.
int64_t GetIdleTimeKdeDBus() {
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
                                              G_VARIANT_TYPE("(u)"), // Expect uint32 reply type 'u'
                                              G_DBUS_CALL_FLAGS_NONE,
                                              5000, // Timeout 5 sec — ksmserver can be slow to register at login
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

// Declared in idle_detect.h. Not static: idle_sources_system.cpp calls this from ShellMonitor.
//
// This is needed because ext_idle_notifier_v1 may not reflect D-Bus-level inhibitions (e.g. from video players
// using org.freedesktop.ScreenSaver.Inhibit). The HasInhibition(1) call checks for ChangeScreenSettings
// inhibitions (type 1), which prevent screen idle/blanking.
bool CheckKdeInhibition() {
    debug_log("INFO: %s: Checking KDE screen idle inhibitions via PolicyAgent D-Bus HasInhibition.", __func__);

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    bool is_inhibited = false;

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            debug_log("INFO: %s: Cannot connect to session bus for KDE inhibit check: %s", __func__, dbus_error->message);
            g_error_free(dbus_error);
        } else {
            debug_log("INFO: %s: Cannot connect to session bus for KDE inhibit check (unknown error).", __func__);
        }
        return false;
    }

    const char* service_name = "org.kde.Solid.PowerManagement";
    const char* object_path = "/org/kde/Solid/PowerManagement/PolicyAgent";
    const char* interface_name = "org.kde.Solid.PowerManagement.PolicyAgent";
    const char* method_name = "HasInhibition";

    // Type 1 = ChangeScreenSettings (prevents screen idle/blanking)
    guint32 inhibition_type = 1;

    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name, object_path, interface_name, method_name,
                                              g_variant_new("(u)", inhibition_type),
                                              G_VARIANT_TYPE("(b)"),
                                              G_DBUS_CALL_FLAGS_NONE,
                                              500, // Timeout (ms)
                                              nullptr, &dbus_error);

    if (dbus_error) {
        debug_log("INFO: %s: Error calling %s: %s (Perhaps not KDE or method unavailable?)",
                  __func__, method_name, dbus_error->message);
        g_error_free(dbus_error);
        is_inhibited = false;
    } else if (dbus_result) {
        gboolean inhibited_result = FALSE;
        g_variant_get(dbus_result, "(b)", &inhibited_result);
        is_inhibited = (inhibited_result == TRUE);
        debug_log("INFO: %s: PolicyAgent.HasInhibition(type=%u) returned: %s",
                  __func__, inhibition_type, is_inhibited ? "true" : "false");
        g_variant_unref(dbus_result);
    } else {
        error_log("%s: Call to %s returned null result without error.", __func__, method_name);
        is_inhibited = false;
    }

    g_object_unref(connection);
    return is_inhibited;
}

// Declared in idle_detect.h. Not static: idle_sources_system.cpp calls this from ShellMonitor.
bool CheckGnomeInhibition() {
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

// Declared in idle_detect.h. Not static: idle_sources_system.cpp calls this for the shell idle source.
//
// Note that this does NOT take idle inhibit into account, unlike the corresponding KDE D-Bus call.
int64_t GetIdleTimeWaylandGnomeViaDBus() {
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

namespace {

//
// ---------------------------------------------------------------------------------------------------
// X error handling
// ---------------------------------------------------------------------------------------------------
//
// Xlib's defaults terminate the process, which is unacceptable for a daemon that queries X displays it
// does not own. GetIdleTimeXss() is aimed at displays discovered from other sessions, so a display that
// dies while it is being read must cost one reading, not the daemon. That is exactly the multi-endpoint
// VNC case this design exists for: an endpoint appearing and disappearing is normal operation, not an
// error condition.
//
// Xlib splits this into two unrelated failures, and they need different treatment.
//
// 1. PROTOCOL errors (X_Error events, e.g. BadWindow from a root window that vanished with its screen)
//    are non-fatal. Xlib's _XDefaultError calls exit(1), but a replacement handler is explicitly
//    permitted to return and Xlib carries on with the connection intact. HandleXProtocolError() logs and
//    returns 0.
//
// 2. Fatal I/O errors (the connection itself is gone) are terminal by default, and no single hook fixes
//    them. Reading libX11 1.8.10's XlibInt.c, _XIOError does:
//
//        if (_XIOErrorFunction != NULL) (*_XIOErrorFunction)(dpy); else _XDefaultIOError(dpy);
//        exit_handler(dpy, exit_handler_data);
//
//    _XDefaultIOError is _X_NORETURN and calls exit(1), and the per-display exit_handler defaults to
//    _XDefaultIOErrorExit, which also calls exit(1). Two consequences, both measured against a killed
//    Xvfb rather than assumed:
//
//      - XSetIOErrorHandler is MANDATORY even on a libX11 that has XSetIOErrorExitHandler. With the I/O
//        handler omitted the probe died with "X connection to :82 broken" and status 1, because
//        _XDefaultIOError never returns and the exit hook is therefore never reached.
//      - Returning from both hooks is safe. _XIOError returns 1 to its Xlib caller, the in-flight call
//        unwinds normally, XScreenSaverQueryInfo() returns Status 0, and the subsequent XFree() and
//        XCloseDisplay() on the dead Display both complete cleanly. A later connection to a different,
//        live display is unaffected, so no global state is poisoned.
//
// THE WINDOW THE EXIT HANDLER CANNOT COVER
//
// XSetIOErrorExitHandler takes a Display*, so it cannot be installed until XOpenDisplay() has returned.
// An I/O error raised INSIDE XOpenDisplay therefore still falls through to _XDefaultIOErrorExit and
// exits. This is not theoretical: driving GetIdleTimeXss() through a proxy that severs the connection
// after a chosen number of client writes, severing anywhere in the connection handshake killed the
// process with status 1 while our I/O handler was logging, and only a severance after the handshake was
// recoverable by the exit handler. That window matters here more than in a normal X client, because this
// function opens a fresh connection on every single query rather than holding one open.
//
// So the two mechanisms are used where each is actually clean:
//
//   - XOpenDisplay() is wrapped in sigsetjmp/siglongjmp. The global I/O handler is the ONLY hook that
//     runs in this phase, on every libX11 version, so escaping through it is the only available fix.
//   - Everything after the open relies on the exit handler, which lets Xlib unwind its own call and
//     leaves the Display safe to close normally. This is the common case and it is leak-free: 200
//     induced post-open failures left the descriptor count unchanged at 4.
//
// A jump is not free, which is the other reason to confine it to the open. It cannot be followed by
// XCloseDisplay(), because siglongjmp leaves Xlib's request buffers mid-write and a failed
// XOpenDisplay() never hands out the pointer at all, so the Display structure is lost. Measured over 200
// induced failures, abandoning it whole grew the descriptor table from 4 to 204 and RSS by 14 MB, which
// is an outage for a daemon rather than a wasted reading. ReclaimDyingXConnection() therefore closes the
// socket, which bounds the damage to memory; see it for why that is safe.
//
// On libX11 < 1.7 there is no exit hook at all, so the guard is widened to cover the post-open phases
// too, and those failures pay the same cost. Doing that on a build that predates 2021 is the lesser evil
// against exiting.
//
// Constraints the jump imposes, all satisfied by construction below:
//   - The guarded regions are OpenXDisplayGuarded() and QueryXssIdleMs(), whose locals are raw pointers
//     and integers only. Nothing in either has a non-trivial destructor, so the jump skips no cleanup.
//     Both deliberately keep logging OUT of the guarded region, so no string temporary can ever be live
//     across a jump.
//   - No local written between sigsetjmp() and siglongjmp() is read on the jump path unless it is
//     declared volatile, so no local can be read with an indeterminate value.
//   - The jump buffer and its armed flag are thread_local, not global, so two threads resolving two X
//     displays cannot land in each other's stack frame. The handler is re-entrant-safe because it
//     disarms before jumping, so a second I/O error cannot jump into a frame that has already been left.
//
// A failed query leaves its XScreenSaverInfo untouched, which is why the Status return is now checked.
// The old code ignored it and published info->idle regardless; since XScreenSaverAllocInfo() zeroes the
// struct, a dying display would have reported "idle 0 seconds", i.e. "the user is active right now", on
// a display that no longer exists. Suspending compute forever on a display that is gone is a worse
// failure than the crash this fix is about.
//
// XInitThreads() is deliberately NOT called. It would have to be the first Xlib call in the process, and
// it is not needed today: every X resolution runs on the main loop thread, and GetIdleTimeXss() opens and
// closes its own Display inside the call, so no Display is ever shared between threads or even between
// calls. If Phase 3 ever resolves X11IdleSources on worker threads, XInitThreads() becomes REQUIRED --
// not for the per-display state, but because XOpenDisplay() mutates a process-global display list under
// _Xglobal_lock, which is a no-op until XInitThreads() is called. It must then be added as the first
// statement of main(). Note that it would also make _XIOError take the display's user lock without
// releasing it before our handler runs, so the jump paths above would leave that lock held; that is
// another reason not to enable it until it is actually needed.
//

//!
//! \brief Set by the fatal I/O error handlers when the display being queried has died, so that
//! GetIdleTimeXss() can report the failure and choose a safe teardown. Thread local because the handlers
//! are process-global but the failure belongs to one call on one thread.
//!
thread_local bool t_x_display_died = false;

//!
//! \brief Landing point for the fatal I/O error escape. Always compiled: it guards XOpenDisplay() on
//! every libX11 version, because the per-display exit handler cannot exist during the open.
//!
thread_local sigjmp_buf t_x_io_error_jump;

//!
//! \brief Whether t_x_io_error_jump currently refers to a live frame that is willing to be jumped into.
//! When false the I/O handler returns instead, leaving the outcome to the exit handler.
//!
thread_local bool t_x_io_error_jump_armed = false;

//!
//! \brief The Display the fatal I/O error arrived on, handed from the handler to the landing site so
//! that the abandoned connection's socket can be reclaimed. Only ever set immediately before a jump, and
//! cleared as soon as it is consumed, so it can never name a stale Display.
//!
thread_local Display* t_x_dying_display = nullptr;

//!
//! \brief Closes the socket of a Display abandoned by a jump, bounding the cost of a failure that a
//! daemon will meet repeatedly.
//!
//! A jump cannot be followed by XCloseDisplay(): siglongjmp leaves Xlib's request buffers mid-write, and
//! during a failed XOpenDisplay() Xlib never even hands out the Display pointer, so the structure is only
//! half built. The memory is therefore lost, and there is no public API to reclaim it.
//!
//! The file descriptor is a different matter, and is the one that actually threatens the process.
//! Measured over 200 induced failures, abandoning the connection outright grew the descriptor table one
//! entry per failure, 4 to 204, which walks a long-running daemon into its descriptor limit. Closing it
//! here is safe because the Display is unreachable the moment we return: nothing in this process holds a
//! pointer to it, no thread can query it, and Xlib only ever revisits a Display through a caller-supplied
//! pointer.
//!
void ReclaimDyingXConnection()
{
    Display* dying = t_x_dying_display;
    t_x_dying_display = nullptr;

    if (dying == nullptr) {
        return;
    }

    int connection_fd = ConnectionNumber(dying);

    if (connection_fd >= 0) {
        close(connection_fd);
    }
}

//!
//! \brief Replacement for Xlib's _XDefaultError, which calls exit(1). Returning from a protocol error
//! handler is explicitly safe and Xlib continues with the connection.
//!
//! Only the numeric codes are logged. The handler must not perform operations on the display, and
//! XGetErrorText() would allocate and consult the resource database on a connection that may be in the
//! process of dying. The codes are sufficient here because this process issues exactly one kind of
//! request on these displays.
//!
//! \param x_display Display the error arrived on. Unused; logging it would mean touching it.
//! \param error_event The X error.
//! \return 0 always. Xlib ignores the value.
//!
int HandleXProtocolError(Display* x_display, XErrorEvent* error_event)
{
    (void) x_display;

    error_log("WARNING: %s: X protocol error: error_code=%d request_code=%d minor_code=%d serial=%lu. "
              "Continuing; this display's reading is discarded.",
              __func__,
              (int) error_event->error_code,
              (int) error_event->request_code,
              (int) error_event->minor_code,
              (unsigned long) error_event->serial);

    return 0;
}

//!
//! \brief Replacement for Xlib's _XDefaultIOError, which is _X_NORETURN and calls exit(1).
//!
//! Installing this is mandatory even when XSetIOErrorExitHandler() is available, because _XIOError runs
//! this hook first and the default never returns, so the exit hook would otherwise never be reached.
//!
//! \param x_display Display whose connection was lost. Unused; the connection is gone.
//! \return 0 when a guarded region is not active, handing control to the exit handler. When one is
//!         active this does not return at all, having jumped back into that region.
//!
int HandleXIoError(Display* x_display)
{
    t_x_display_died = true;

    if (t_x_io_error_jump_armed) {
        // Disarmed before jumping so that a second I/O error cannot target a frame that has already
        // been left, which is what makes this handler safe to re-enter.
        t_x_io_error_jump_armed = false;

        // Xlib passes the Display even when the failure happened inside XOpenDisplay() and the caller
        // therefore never received it. That is the only chance to reclaim its socket.
        t_x_dying_display = x_display;

        siglongjmp(t_x_io_error_jump, 1);
    }

    return 0;
}

#ifdef HAVE_X11_IO_ERROR_EXIT_HANDLER
//!
//! \brief Replacement for Xlib's _XDefaultIOErrorExit, which calls exit(1). Returning from here makes
//! _XIOError return to its Xlib caller, which then unwinds the in-flight call normally and leaves the
//! Display safe to close.
//! \param x_display Display whose connection was lost. Unused.
//! \param user_data Unused; the per-call state is the thread_local flag above.
//!
void HandleXIoErrorExit(Display* x_display, void* user_data)
{
    (void) x_display;
    (void) user_data;

    t_x_display_died = true;
}
#endif

//!
//! \brief Installs the two process-global X error handlers exactly once.
//!
//! These are process-global, not per-display, so re-registering them on every GetIdleTimeXss() call
//! would be a pointless write to shared state from whichever thread happened to call first. The
//! per-display exit handler is a different thing and must be set on each Display, since it is a field of
//! the Display itself; GetIdleTimeXss() does that after each successful open.
//!
void EnsureXErrorHandlersInstalled()
{
    static std::once_flag installed;

    std::call_once(installed, []() {
        XSetErrorHandler(HandleXProtocolError);
        XSetIOErrorHandler(HandleXIoError);

        debug_log("INFO: %s: Installed X protocol and I/O error handlers (%s).",
                  __func__,
#ifdef HAVE_X11_IO_ERROR_EXIT_HANDLER
                  "XSetIOErrorExitHandler available for the post-open phases"
#else
                  "no XSetIOErrorExitHandler; siglongjmp guards the whole cycle"
#endif
                  );
    });
}

//!
//! \brief Opens an X display, surviving a connection that dies during the handshake.
//!
//! The guard is what distinguishes this from a bare XOpenDisplay(). An I/O error raised inside the open
//! cannot be caught by the per-display exit handler, because there is no Display to attach one to yet,
//! so the global I/O handler jumps back here instead.
//!
//! Only raw pointers live in this frame, so the jump skips no cleanup, and x_display is written after
//! the sigsetjmp() but read only on the path that did not jump.
//!
//! \param display_name Display to open, or nullptr for the DISPLAY environment variable.
//! \return The open display, or nullptr if it could not be opened or died during connection setup.
//!
Display* OpenXDisplayGuarded(const char* display_name)
{
    if (sigsetjmp(t_x_io_error_jump, 1) != 0) {
        // Arrived from HandleXIoError(). Xlib never handed us a Display, so the half-built structure is
        // unreachable and only its socket can be recovered.
        ReclaimDyingXConnection();
        return nullptr;
    }

    t_x_io_error_jump_armed = true;
    Display* x_display = XOpenDisplay(display_name);
    t_x_io_error_jump_armed = false;

    return x_display;
}

//!
//! \brief Outcome of the post-open XScreenSaver sequence, kept separate from the value so that the
//! logging can happen outside the guarded region.
//!
enum class XssOutcome {
    OK,
    NO_EXTENSION,
    ALLOC_FAILED,
    QUERY_FAILED,
    DISPLAY_DIED
};

//!
//! \brief Runs the post-open XScreenSaver sequence: extension probe, info allocation, and the idle
//! query.
//!
//! This is a separate function so that the guarded region is small enough to audit at a glance. Its
//! locals are two ints, a raw pointer and an enum, none of which has a non-trivial destructor, and it
//! contains no logging so that no string temporary can be live across a jump.
//!
//! On a libX11 with an exit handler no jump is armed here at all: Xlib unwinds the failed call by itself
//! and the caller can close the Display normally. The guard is only compiled in for the fallback.
//!
//! \param x_display Open display to query.
//! \param outcome Set to the reason for failure, or OK.
//! \return Idle milliseconds >= 0 when outcome is OK, otherwise -1.
//!
int64_t QueryXssIdleMs(Display* x_display, XssOutcome* outcome)
{
    // volatile because the fallback build reads it on the jump path, where a non-volatile local written
    // after sigsetjmp() would be indeterminate. The pointer is volatile, not the pointee, so it still
    // passes to XFree() without a cast.
    XScreenSaverInfo* volatile info = nullptr;

#ifndef HAVE_X11_IO_ERROR_EXIT_HANDLER
    if (sigsetjmp(t_x_io_error_jump, 1) != 0) {
        // Arrived from HandleXIoError(). info is our own Xmalloc'd buffer and Xlib holds no reference to
        // it, so reclaiming it is safe even though the stack was unwound from inside Xlib. The Display
        // is NOT closed here; see the caller.
        if (info != nullptr) {
            XFree(info);
        }

        ReclaimDyingXConnection();

        *outcome = XssOutcome::DISPLAY_DIED;
        return -1;
    }

    t_x_io_error_jump_armed = true;
#endif

    int64_t idle_time_ms = -1;
    int event_base = 0;
    int error_base = 0;

    if (!XScreenSaverQueryExtension(x_display, &event_base, &error_base)) {
        *outcome = XssOutcome::NO_EXTENSION;
    } else if ((info = XScreenSaverAllocInfo()) == nullptr) {
        *outcome = XssOutcome::ALLOC_FAILED;
    } else {
        // A failed query leaves info->idle exactly as XScreenSaverAllocInfo() zeroed it, so the Status
        // must gate the read. Publishing the zero would mean "the user is active right now" on a display
        // that has just died.
        if (XScreenSaverQueryInfo(x_display, DefaultRootWindow(x_display), info)) {
            idle_time_ms = static_cast<int64_t>(info->idle);
            *outcome = XssOutcome::OK;
        } else {
            *outcome = XssOutcome::QUERY_FAILED;
        }

        XFree(info);
        info = nullptr;
    }

#ifndef HAVE_X11_IO_ERROR_EXIT_HANDLER
    t_x_io_error_jump_armed = false;
#endif

    // The exit-handler path unwinds through the normal return above rather than jumping, so the died
    // flag is the only thing that distinguishes it from an ordinary query failure.
    if (t_x_display_died) {
        *outcome = XssOutcome::DISPLAY_DIED;
        return -1;
    }

    return idle_time_ms;
}

} // anonymous namespace

// Declared in idle_detect.h. Not static: idle_sources_system.cpp calls this from X11IdleSource.
//
// A display that does not answer is reported at debug level rather than error level, with the one exception
// noted below. This function's only caller is X11IdleSource::ResolveIdleSeconds(), which runs once per
// discovered X candidate per reconcile tick. Discovery is deliberately over-inclusive and rejection is its
// designed outcome, so "this display is not a usable endpoint" is ordinary information, not a fault: a KDE
// Wayland desktop whose Xwayland does not advertise MIT-SCREEN-SAVER would otherwise write an error to the
// journal every second, forever, about a display that is doing nothing wrong. Losing every source is still
// reported, by the main loop, which is the level at which it is actually a problem.
//
// The local Display* is named x_display rather than display so it does not shadow the display name parameter.
int64_t GetIdleTimeXss(const std::string& display, int max_connect_retries) {
    const char* display_label = display.empty() ? "<default>" : display.c_str();

    debug_log("INFO: %s: Using XScreenSaver on display '%s'.", __func__, display_label);

    // A display that is not this process's own can die at any point below, including during the
    // connection handshake. Without these handlers that takes the whole daemon down.
    EnsureXErrorHandlersInstalled();

    Display* x_display = nullptr;

    // A null name is libX11's "use the DISPLAY environment variable", which is the historical behavior and
    // what an empty parameter continues to mean.
    const char* display_name = display.empty() ? nullptr : display.c_str();

    // A caller asking for zero or fewer attempts still means "try", not "do nothing".
    const int max_attempts = (max_connect_retries > 1) ? max_connect_retries : 1;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        // Cleared before every attempt: a previous attempt's dead connection must not condemn this one.
        t_x_display_died = false;

        x_display = OpenXDisplayGuarded(display_name);
        if (x_display) break;
        if (attempt < max_attempts) {
            debug_log("INFO: %s: Could not open X display '%s' (attempt %d/%d). Retrying...",
                      __func__,
                      display_label,
                      attempt,
                      max_attempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(X_RETRY_DELAY_MS));
        } else {
            debug_log("INFO: %s: Could not open X display '%s' after %d attempt(s).",
                      __func__,
                      display_label,
                      max_attempts);
            return -1;
        }
    }

    // Cleared again: the open succeeded, so anything the flag still holds belongs to an earlier attempt.
    t_x_display_died = false;

#ifdef HAVE_X11_IO_ERROR_EXIT_HANDLER
    // Unlike the two handlers above this is a field of the Display, so it is set per connection. It only
    // covers what happens from here on; the open itself was covered by the guard inside
    // OpenXDisplayGuarded().
    XSetIOErrorExitHandler(x_display, HandleXIoErrorExit, nullptr);
#endif

    XssOutcome outcome = XssOutcome::QUERY_FAILED;
    int64_t idle_time_ms = QueryXssIdleMs(x_display, &outcome);

    if (outcome == XssOutcome::DISPLAY_DIED) {
#ifdef HAVE_X11_IO_ERROR_EXIT_HANDLER
        // Xlib unwound its own call, so the Display is internally consistent and XCloseDisplay() tears it
        // down without touching the dead socket. Verified against a killed Xvfb.
        XCloseDisplay(x_display);
        error_log("WARNING: %s: X display '%s' died during the query. Discarding this reading.",
                  __func__,
                  display_label);
#else
        // siglongjmp left Xlib's request buffers mid-write and XCloseDisplay() would walk them, so the
        // structure is abandoned and only its socket was reclaimed. Only reachable on libX11 < 1.7.
        error_log("WARNING: %s: X display '%s' died during the query. Discarding this reading; the "
                  "Display structure cannot be freed safely on this libX11.",
                  __func__,
                  display_label);
#endif
        return -1;
    }

    XCloseDisplay(x_display);

    switch (outcome) {
    case XssOutcome::OK:
        break;
    case XssOutcome::NO_EXTENSION:
        debug_log("INFO: %s: XScreenSaver extension unavailable on display '%s'.", __func__, display_label);
        return -1;
    case XssOutcome::ALLOC_FAILED:
        // Not a property of the display: this is a memory allocation failing, and it says nothing about
        // whether this display is a usable endpoint. It stays at error level for that reason.
        error_log("%s: Could not allocate XScreenSaverInfo.", __func__);
        return -1;
    case XssOutcome::QUERY_FAILED:
    case XssOutcome::DISPLAY_DIED:
        debug_log("INFO: %s: XScreenSaver query failed on display '%s'.", __func__, display_label);
        return -1;
    }

    int64_t idle_time_seconds = idle_time_ms / 1000;
    debug_log("INFO: %s: XScreenSaver reported: %lld ms (%lld seconds)", __func__, (int64_t)idle_time_ms, (int64_t)idle_time_seconds);
    return idle_time_seconds;
}

//
// Declared in idle_detect.h. The session typing this replaced decided everything up front from
// IsTtySession() and IsWaylandSession(), both of which read the process environment. That environment is
// frozen at exec, so a daemon started before its graphical session saw a tty session forever, no matter what
// appeared afterwards: the root cause of issue #12. IsKdeSession() went with them, superseded by
// ShellMonitor::DetectShellKind(), which asks the same D-Bus question but also answers it for GNOME.
//
// The fix is that this function now determines nothing at all. It reports what the live source set currently
// says, and keeping that set current is main()'s job.
//
int64_t GetIdleTimeSeconds() {
    return g_idle_source_pool.GetIdleSeconds();
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
                // idle_detect_control_pipe_initialized remains false from initialized value above.
            } else {
                // Pipe already exists, so we can proceed.
                idle_detect_control_pipe_initialized = true;
            }
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

// Forward declare the actual static listener structs (C-style linkage needed)
extern "C" {
extern const struct wl_registry_listener g_registry_listener;
extern const struct ext_idle_notification_v1_listener g_idle_notification_listener;
}

// Initialize static pointers defined in the header
const void* WaylandIdleMonitor::c_registry_listener_ptr = &g_registry_listener;
const void* WaylandIdleMonitor::c_idle_notification_listener_ptr = &g_idle_notification_listener;

//!
//! \brief Renders a Wayland socket name for logging. Once more than one monitor can be alive at a time, the log
//! has to say which endpoint each line is about, and an empty name has to read as something other than "".
//!
//! \param socket_name Socket name as stored by WaylandIdleMonitor::Start().
//! \return The socket name, or a placeholder for the environment-derived socket.
//!
static std::string SocketNameForLog(const std::string& socket_name) {
    return socket_name.empty() ? std::string("<WAYLAND_DISPLAY>") : socket_name;
}

// Constructor
WaylandIdleMonitor::WaylandIdleMonitor() :
    m_seat(nullptr),
    m_idle_notifier(nullptr),
    m_seat_id(0),
    m_idle_notifier_id(0),
    m_is_idle(false), // Start assuming active
    m_idle_start_time(0),
    m_interrupt_monitor(false),
    m_initialized(false),
    m_globals_lost(false),
    m_display(nullptr),
    m_registry(nullptr),
    m_idle_notification(nullptr),
    m_notification_timeout_ms(0)
{
    m_interrupt_pipe_fd[0] = -1; // read end
    m_interrupt_pipe_fd[1] = -1; // write end
}

// Destructor
WaylandIdleMonitor::~WaylandIdleMonitor() {
    Stop(); // Ensure resources are cleaned up
}

// Start method
bool WaylandIdleMonitor::Start(const std::string& socket_name, int notification_timeout_ms, int max_init_retries) {
    // Debug rather than normal, because every call to this is now a validation attempt: the only caller is
    // WaylandIdleSource::Start(), which IdleSourcePool's endpoint factory uses to decide whether a discovered
    // candidate is a real compositor. Announcing the attempt at normal level made an ATTEMPT as loud as an
    // OUTCOME, and attempts against a candidate that can never validate -- a stale socket with no listener, or
    // any Wayland candidate in a GNOME session, which advertises no ext_idle_notifier_v1 -- repeat for the life
    // of the process. The outcomes are still reported: the pool logs the source it added, and it logs the
    // rejection and the backoff it applied.
    debug_log("INFO: %s: Starting Wayland idle monitor on socket %s.",
              __func__,
              SocketNameForLog(socket_name));

    if (m_initialized.load()) {
        // The already-running monitor keeps the socket it was started with. Log both names so a mismatched
        // restart request is visible rather than silently ignored.
        debug_log("INFO: %s: Monitor already initialized on socket %s.",
                  __func__,
                  SocketNameForLog(m_socket_name));
        return true; // Already running
    }

    // m_initialized being clear does not mean this object is clean. A monitor thread that exited unexpectedly
    // clears m_initialized itself but cannot join itself or close the interrupt pipe, so it leaves a joinable
    // std::thread and two open FDs behind. Reap that before touching anything else: the pipe2() below would
    // otherwise overwrite the live FDs and the thread launch would move-assign onto a joinable std::thread,
    // which calls std::terminate(). This is a no-op on the first-ever Start().
    if (!ReapFailedThread()) {
        error_log("%s: Could not reap the previous Wayland monitor thread. Not restarting.", __func__);
        return false;
    }

    // Store the parameters before any fallible work below, so that InitializeWayland() and
    // CreateIdleNotification() see them regardless of which failure path is taken from here.
    m_notification_timeout_ms = notification_timeout_ms;
    m_socket_name = socket_name;

    // Create pipe for interrupting poll() before initializing Wayland
    // pipe2 is Linux-specific, use pipe() for broader POSIX if needed
    if (pipe2(m_interrupt_pipe_fd, O_CLOEXEC | O_NONBLOCK) == -1) {
        error_log("%s: Failed to create interrupt pipe: %s (%d)", __func__, strerror(errno), errno);
        return false;
    }

    // Reset state flags
    m_interrupt_monitor.store(false);
    m_globals_lost.store(false);
    m_is_idle.store(false);
    m_idle_start_time.store(0);

    // Perform the fallible setup. StartInternal() does no cleanup of its own; the teardown below is the single
    // failure path for everything it may have partially constructed. Note that StartInternal() sets
    // m_initialized before it launches the monitor thread, so the store below clears it again on failure.
    if (!StartInternal(max_init_retries)) {
        CleanupWayland();
        if (m_interrupt_pipe_fd[0] != -1) { close(m_interrupt_pipe_fd[0]); m_interrupt_pipe_fd[0] = -1; }
        if (m_interrupt_pipe_fd[1] != -1) { close(m_interrupt_pipe_fd[1]); m_interrupt_pipe_fd[1] = -1; }
        m_initialized.store(false);
        return false;
    }

    normal_log("INFO: %s: Wayland idle monitor started successfully on socket %s.",
               __func__,
               SocketNameForLog(m_socket_name));
    return true;
}

// StartInternal method. This performs no cleanup; Start() owns the failure teardown.
bool WaylandIdleMonitor::StartInternal(int max_init_retries) {
    // Initialize Wayland connection, get initial state, and subscribe
    // Includes retries internally now
    if (!InitializeWayland(max_init_retries)) {
        // Debug, not error. A candidate that does not turn out to be an ext_idle_notifier_v1 compositor is an
        // ordinary outcome of validating an over-inclusive discovery set, and InitializeWayland() has already
        // said why at the same level. The pool decides what the operator hears about a rejected candidate.
        debug_log("INFO: %s: Failed to initialize Wayland or find required protocols after retries.", __func__);
        return false;
    }

    // Check again after InitializeWayland succeeded
    if (!m_seat || !m_idle_notifier) {
        error_log("%s: Required Wayland interfaces not bound even after InitializeWayland success (logic error?).", __func__);
        return false;
    }

    // Create the specific idle notification request object. This uses m_notification_timeout_ms, which Start() has
    // already stored.
    CreateIdleNotification();
    if (!m_idle_notification) {
        error_log("%s: Failed to create Wayland idle notification object.", __func__);
        return false;
    }

    // Clear the globals-lost flag now that setup has succeeded. OnGlobalRemoved() runs on this thread during
    // InitializeWayland()'s roundtrips, so the flag may have been set by churn that the bound-pointer re-check
    // above has already proven harmless: if both globals are bound at this instant the connection is healthy,
    // whatever happened while it was being built. From here on the flag carries its intended meaning only -
    // "a global went away while we were relying on it" - which is what the monitor thread tests. Without this
    // the thread could break on its very first iteration and leave the monitor permanently unavailable despite
    // a fully successful initialization.
    m_globals_lost.store(false);

    // If Wayland setup okay, start the thread to run the event loop.
    //
    // m_initialized is set *before* the thread is launched, not after it. The monitor thread clears
    // m_initialized when it exits unexpectedly, and a thread that fails immediately would otherwise have its
    // clear overwritten by a store(true) issued after the launch, leaving the monitor advertising itself as
    // available with no thread behind it. Start() clears the flag again if the launch below fails.
    m_initialized.store(true);

    try {
        m_monitor_thread = std::thread(&WaylandIdleMonitor::WaylandMonitorThread, this);
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Wayland monitor thread: %s", __func__, e.what());
        return false;
    } catch (...) {
        error_log("%s: Unknown error starting Wayland monitor thread.", __func__);
        return false;
    }

    return true;
}

// ReapFailedThread method. Called by Start() before any setup work; see the header for why.
bool WaylandIdleMonitor::ReapFailedThread() {
    // Nothing to reap. This is both the first-ever Start() and the Start()-after-a-clean-Stop() path, since
    // Stop() joins the thread itself.
    if (!m_monitor_thread.joinable()) {
        return true;
    }

    // Never reap from within the monitor thread. join() on self throws resource_deadlock_would_occur, and the
    // teardown below would destroy the Wayland resources the caller is still standing on.
    if (m_monitor_thread.get_id() == std::this_thread::get_id()) {
        error_log("WARN: %s: called from within the Wayland monitor thread. Not reaping.", __func__);
        return false;
    }

    error_log("WARN: %s: Reaping a Wayland monitor thread that exited on its own before restarting.", __func__);

    // joinable() cannot distinguish "finished but not joined" from "still alive and blocked in poll()". Only the
    // former is reachable today, because the thread clears m_initialized on its way out and Start() returns early
    // while that flag is still set. Signal the interrupt anyway so this join is bounded by the same mechanism
    // Stop() uses, rather than resting on that reasoning once the endpoint pool drives Start()/Stop() per
    // endpoint. Setting m_interrupt_monitor also makes a still-live thread skip its unexpected-exit tail, which
    // is correct here: this is a requested teardown, and the flags are reset below regardless.
    m_interrupt_monitor.store(true);

    if (m_interrupt_pipe_fd[1] != -1) {
        char buf = 'X';
        ssize_t written = write(m_interrupt_pipe_fd[1], &buf, 1);

        if (written <= 0 && errno != EAGAIN) {
            error_log("%s: Failed to write to interrupt pipe while reaping: %s (%d)",
                      __func__,
                      strerror(errno),
                      errno);
        }
    }

    try {
        m_monitor_thread.join();
    } catch (const std::system_error& e) {
        // The thread may still be alive, so its Wayland resources must not be torn down here and the object
        // must not be restarted. Leaving m_monitor_thread joinable is deliberate: the caller aborts the start.
        error_log("%s: Error joining the previous Wayland monitor thread: %s", __func__, e.what());
        return false;
    }

    // Past the join this is exactly the teardown Stop() performs, and for the same reason: the thread is gone,
    // so nothing else can be touching the Wayland connection or the interrupt pipe. The FD guards are what keep
    // this and a subsequent Stop() from double-closing.
    CleanupWayland();

    if (m_interrupt_pipe_fd[0] != -1) { close(m_interrupt_pipe_fd[0]); m_interrupt_pipe_fd[0] = -1; }
    if (m_interrupt_pipe_fd[1] != -1) { close(m_interrupt_pipe_fd[1]); m_interrupt_pipe_fd[1] = -1; }

    // Reset the state flags so the caller sees a pristine object. Start() sets these again itself, but a
    // half-reset object between the two would report a stale idle time through IsIdle()/GetIdleSeconds().
    //
    // The reset stops at the flags on purpose. m_socket_name and m_notification_timeout_ms are configuration,
    // not run state: clearing m_socket_name here would make a reap-then-restart silently fall back to the
    // WAYLAND_DISPLAY-derived socket instead of reconnecting to the endpoint this monitor owns. Start()
    // overwrites both from its arguments anyway, so there is nothing stale to guard against either.
    m_interrupt_monitor.store(false);
    m_globals_lost.store(false);
    m_is_idle.store(false);
    m_idle_start_time.store(0);

    return true;
}

// Stop method
void WaylandIdleMonitor::Stop() {
    normal_log("INFO: %s: Stopping Wayland idle monitor on socket %s...",
               __func__,
               SocketNameForLog(m_socket_name));

    // Use exchange to prevent concurrent Stop calls and get previous state
    if (m_interrupt_monitor.exchange(true)) {
        debug_log("INFO: %s: Stop already in progress or completed.", __func__);
        // Join attempt with timeout in case thread is stuck
        if(m_monitor_thread.joinable() && m_monitor_thread.get_id() != std::this_thread::get_id()) {
            auto future = std::async(std::launch::async, &std::thread::join, &m_monitor_thread);
            if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                error_log("%s: Timed out waiting for Wayland thread join during concurrent stop.", __func__);
            }
        }
        return;
    }

    // Signal the monitor thread to wake up from poll() by writing to the pipe
    if (m_interrupt_pipe_fd[1] != -1) {
        char buf = 'X'; // Data doesn't matter, just the event causing poll to wake
        ssize_t written = write(m_interrupt_pipe_fd[1], &buf, 1);
        if (written <= 0 && errno != EAGAIN) {
            error_log("%s: Failed to write to interrupt pipe: %s (%d)", __func__, strerror(errno), errno);
        } else {
            debug_log("INFO: %s: Sent interrupt signal via pipe.", __func__);
        }
    } else {
        error_log("WARN: %s: Interrupt pipe write fd invalid during Stop().", __func__);
    }

    // Join the thread (wait for it to finish)
    if (m_monitor_thread.joinable() && m_monitor_thread.get_id() != std::this_thread::get_id()) {
        debug_log("INFO: %s: Joining Wayland monitor thread...", __func__);
        try {
            m_monitor_thread.join();
            debug_log("INFO: %s: Wayland monitor thread joined.", __func__);
        } catch (const std::system_error& e) {
            error_log("%s: Error joining Wayland monitor thread: %s", __func__, e.what());
        }
    } else if (m_monitor_thread.get_id() == std::this_thread::get_id()) {
        error_log("WARN: %s: Stop() called from within the Wayland monitor thread!", __func__);
    }

    // Clean up Wayland resources *after* thread has stopped using them
    CleanupWayland();

    // Close interrupt pipe FDs (owned by Start()/Stop(), not CleanupWayland())
    if (m_interrupt_pipe_fd[0] != -1) { close(m_interrupt_pipe_fd[0]); m_interrupt_pipe_fd[0] = -1; }
    if (m_interrupt_pipe_fd[1] != -1) { close(m_interrupt_pipe_fd[1]); m_interrupt_pipe_fd[1] = -1; }

    m_initialized.store(false); // Mark as no longer initialized

    // m_socket_name is deliberately not cleared: it is what the next Start() would be reconnecting to, and it
    // keeps GetSocketName() meaningful for a stopped monitor that is still sitting in an endpoint pool.
    normal_log("INFO: %s: Wayland idle monitor on socket %s stopped.",
               __func__,
               SocketNameForLog(m_socket_name));
}

void WaylandIdleMonitor::ResetWaylandState() {
    m_seat = nullptr;
    m_idle_notifier = nullptr;
    m_seat_id = 0;
    m_idle_notifier_id = 0;
    m_idle_notification = nullptr;
}

// InitializeWayland (with simplified retry logic focusing on connect and roundtrip check)
bool WaylandIdleMonitor::InitializeWayland(int max_retries) {
    // Every failure path below logs at debug level. This method is reached only through Start(), whose only
    // caller validates a discovery candidate, and a candidate that fails to connect or that connects to a
    // compositor without ext_idle_notifier_v1 is an ordinary outcome rather than an error: discovery is
    // deliberately over-inclusive and validation is how the wrong guesses are removed. At normal and error
    // level these lines were the bulk of a measured 87 journal messages in 8 seconds from a single stale
    // socket, because each failed attempt emitted several of them and the attempt repeated every tick.
    // IdleSourcePool reports the rejection itself, once, and backs the candidate off.
    const int INIT_RETRY_DELAY_SECONDS = 2; // Example delay

    // A caller asking for zero or fewer attempts still means "try", not "do nothing". Clamping here rather
    // than rejecting keeps every caller's failure path identical to a genuine connection failure.
    const int max_init_retries = (max_retries > 1) ? max_retries : 1;

    for (int attempt = 1; attempt <= max_init_retries; ++attempt) {
        debug_log("INFO: %s: Wayland initialization attempt %d/%d...", __func__, attempt, max_init_retries);

        // Reset pointers for this attempt
        CleanupWayland(); // Ensure clean slate before connection attempt

        // A global removed during a previous attempt's roundtrips must not condemn this attempt.
        //
        // This reset is NOT sufficient on its own to guarantee a clear flag on success. The global_remove
        // callback runs on this (the main) thread during the two roundtrips below, which happen after this
        // store. If the compositor removes a global and advertises a replacement across those roundtrips,
        // HandleGlobal re-binds it (its guard is the cached pointer being null, which OnGlobalRemoved just
        // made true) and the bound-pointer check below passes with the flag still set. StartInternal()
        // therefore clears the flag again once the bound-pointer re-check passes.
        m_globals_lost.store(false);

        // An empty socket name means "let libwayland derive the socket from WAYLAND_DISPLAY", which is what this
        // call did unconditionally before the monitor became addressable.
        m_display = wl_display_connect(m_socket_name.empty() ? nullptr : m_socket_name.c_str());
        if (!m_display) {
            debug_log("INFO: %s: Failed to connect to Wayland display %s (attempt %d).",
                      __func__,
                      SocketNameForLog(m_socket_name),
                      attempt);
            // Go directly to sleep and retry
        } else {
            m_registry = wl_display_get_registry(m_display);
            if (!m_registry) {
                debug_log("INFO: %s: Failed to get Wayland registry (attempt %d).", __func__, attempt);
                CleanupWayland();
                // Go to sleep and retry
            } else {
                // Reset potential stale globals found from previous failed attempts
                ResetWaylandState();

                wl_registry_add_listener(m_registry, (const wl_registry_listener*)c_registry_listener_ptr, this);

                // Perform roundtrips to get globals advertised
                if (wl_display_roundtrip(m_display) != -1 && wl_display_roundtrip(m_display) != -1) {
                    // Check if required globals were actually found and bound by the listener
                    if (m_seat != nullptr && m_idle_notifier != nullptr) {
                        debug_log("INFO: %s: Wayland connection to %s and required globals found on attempt %d.",
                                  __func__,
                                  SocketNameForLog(m_socket_name),
                                  attempt);
                        // Success! Don't cleanup, just return true.
                        // Note: Registry listener remains attached.
                        return true;
                    } else {
                        // Roundtrip succeeded but didn't get the needed globals yet. This is the GNOME case, and
                        // it is permanent there rather than a timing artifact: mutter implements no
                        // ext_idle_notifier_v1 for this to find on any attempt, ever.
                        debug_log("INFO: %s: Wayland roundtrip ok, but required globals "
                                  "(wl_seat/ext_idle_notifier_v1) not found (attempt %d).",
                                  __func__,
                                  attempt);
                        // Clean up this attempt's resources before retrying
                        CleanupWayland();
                        // Go to sleep and retry
                    }
                } else {
                    debug_log("INFO: %s: Wayland display roundtrip failed (attempt %d).", __func__, attempt);
                    // Clean up this attempt's resources
                    CleanupWayland();
                    // Go to sleep and retry
                }
            } // end registry check
        } // end display check

        // --- Wait before retrying if not the last attempt ---
        if (attempt < max_init_retries) {
            debug_log("INFO: %s: Waiting %d seconds before next Wayland init attempt...", __func__, INIT_RETRY_DELAY_SECONDS);
            // Check for shutdown request to avoid waiting unnecessarily
            for (int i = 0; i < INIT_RETRY_DELAY_SECONDS * 10; ++i) { // Check every 100ms
                if (g_shutdown_requested.load()) {
                    error_log("%s: Shutdown requested during Wayland init retry wait.", __func__);
                    return false; // Abort initialization on shutdown request
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } // end retry loop

    debug_log("INFO: %s: Failed to initialize Wayland on socket %s after %d attempts.",
              __func__,
              SocketNameForLog(m_socket_name),
              max_init_retries);
    CleanupWayland(); // Final cleanup after all attempts fail
    return false;
}

void WaylandIdleMonitor::DestroySeat() {
    if (!m_seat) {
        return;
    }

    // Check version before calling release (available since v5). Below v5 there is no
    // release request, so destroy the proxy directly rather than leaking it until
    // wl_display_disconnect().
    if (wl_proxy_get_version((struct wl_proxy *)m_seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(m_seat);
    } else {
        wl_proxy_destroy((struct wl_proxy *)m_seat);
    }

    m_seat = nullptr;
}

void WaylandIdleMonitor::OnGlobalRemoved(uint32_t name) {
    debug_log("INFO: %s: Wayland global removed: %u", __func__, name);

    // The proxies must be destroyed here rather than simply forgotten. CleanupWayland() gates its destroys on
    // the cached pointers being non-null, and wl_display_disconnect() does not free live proxies, so nulling a
    // pointer without destroying it leaks the proxy for the life of the connection. Destroying a proxy from
    // inside a dispatch callback is legal, and clearing the pointer immediately afterwards keeps the later
    // CleanupWayland() from destroying it a second time.
    // The zero checks matter: an unbound global has a cached id of 0, and matching a removal against that
    // would destroy a proxy this monitor never bound.
    if (m_seat_id != 0 && name == m_seat_id) {
        error_log("WARN: %s: Monitored wl_seat (name %u) was removed!", __func__, name);
        DestroySeat();
        m_seat_id = 0;
    } else if (m_idle_notifier_id != 0 && name == m_idle_notifier_id) {
        error_log("WARN: %s: Idle notifier global (name %u) was removed!", __func__, name);
        if (m_idle_notifier) {
            ext_idle_notifier_v1_destroy(m_idle_notifier);
            m_idle_notifier = nullptr;
        }
        m_idle_notifier_id = 0;
    } else {
        // Not a global this monitor depends on.
        return;
    }

    // Without either global this monitor can no longer report idle time, so flag it as failed. The monitor
    // thread breaks out of its loop on this and marks the monitor unavailable, so GetIdleTimeSeconds() falls
    // back to the other detection paths instead of serving a frozen value against a global that no longer
    // exists. This is intentionally not m_interrupt_monitor, which means "asked to stop".
    //
    // Note the monitor is not rebuilt automatically today: Start() has a single caller in main(). Rebuilding is
    // the job of the endpoint pool that replaces that call site, which reconciles sources against discovery and
    // will restart a failed one. ReapFailedThread() exists to make that restart safe.
    m_globals_lost.store(true);
}

void WaylandIdleMonitor::CleanupWayland() {
    // Only log if resources might actually exist
    if (m_display || m_registry || m_seat || m_idle_notifier || m_idle_notification || m_interrupt_pipe_fd[0] != -1) {
        debug_log("INFO: %s: Cleaning up Wayland resources.", __func__);
    } else {
        // Nothing to clean
        return;
    }

    // Destroy specific notification first
    if (m_idle_notification) {
        ext_idle_notification_v1_destroy(m_idle_notification);
        m_idle_notification = nullptr;
    }
    // Destroy/release globals
    if (m_idle_notifier) {
        ext_idle_notifier_v1_destroy(m_idle_notifier);
        m_idle_notifier = nullptr;
    }
    DestroySeat();
    // Destroy registry
    if (m_registry) { wl_registry_destroy(m_registry); m_registry = nullptr; }
    // Disconnect display
    if (m_display) {
        wl_display_flush(m_display); // Flush remaining requests if possible
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
    m_seat_id = 0;
    m_idle_notifier_id = 0;

    // Note: Interrupt pipe FDs are NOT closed here. They are owned by Start()/Stop(), not by the
    // Wayland connection lifecycle. CleanupWayland() is called from InitializeWayland() retry loops
    // where the pipe must survive across attempts.

    // m_is_initialized is set in Stop() AFTER cleanup is done.
}

// CreateIdleNotification
void WaylandIdleMonitor::CreateIdleNotification() {
    if (!m_idle_notifier || !m_seat || m_idle_notification) {
        debug_log("INFO: %s: Cannot create idle notification (missing deps or already exists).", __func__);
        return;
    }
    m_idle_notification = ext_idle_notifier_v1_get_idle_notification(
        m_idle_notifier, m_notification_timeout_ms, m_seat);

    if (!m_idle_notification) {
        error_log("%s: ext_idle_notifier_v1_get_idle_notification failed.", __func__);
        return;
    }
    ext_idle_notification_v1_add_listener(m_idle_notification,
                                          (const ext_idle_notification_v1_listener*)c_idle_notification_listener_ptr,
                                          this);
    // Reset state when creating notification
    m_is_idle.store(false);
    m_idle_start_time.store(0);

    if (wl_display_flush(m_display) == -1) {
        error_log("%s: wl_display_flush failed after adding notification listener.", __func__);
    }
    debug_log("INFO: %s: Created idle notification object (timeout %d ms).", __func__, m_notification_timeout_ms);
}

// PrepareRead helper. Extracted from the monitor thread loop so that a dispatch failure in the inner loop can
// terminate the outer loop rather than only the inner one.
bool WaylandIdleMonitor::PrepareRead() {
    while (wl_display_prepare_read(m_display) != 0) {
        // Dispatch pending events that arrived before prepare_read locked the queue
        if (wl_display_dispatch_pending(m_display) == -1) {
            error_log("%s: wl_display_dispatch_pending() failed in prepare loop. Exiting thread.", __func__);
            return false;
        }
    }

    return true;
}

// WaylandMonitorThread (using poll)
void WaylandIdleMonitor::WaylandMonitorThread() {
    debug_log("INFO: %s: Wayland monitor thread started.", __func__);

    if (!m_display || m_interrupt_pipe_fd[0] == -1) {
        error_log("CRITICAL: %s: Wayland display or interrupt pipe not ready. Exiting thread.", __func__);
        m_initialized.store(false); // Mark as failed
        return;
    }

    struct pollfd fds[2];
    fds[0].fd = wl_display_get_fd(m_display);
    fds[0].events = POLLIN | POLLERR | POLLHUP;
    fds[1].fd = m_interrupt_pipe_fd[0]; // Interrupt pipe read end
    fds[1].events = POLLIN | POLLERR | POLLHUP;

    int poll_ret;

    while (!m_interrupt_monitor.load(std::memory_order_relaxed)) {
        // Prepare read BEFORE blocking in poll. A failure here must exit the thread, not just the prepare loop,
        // otherwise we would flush and poll on a display that has already failed to dispatch.
        if (!PrepareRead()) {
            break;
        }

        // A global this monitor depends on may have been removed by a callback dispatched inside PrepareRead().
        // The read lock is held at this point, so it must be released before leaving the loop.
        if (m_globals_lost.load(std::memory_order_relaxed)) {
            error_log("%s: Required Wayland global removed by the compositor. Exiting thread.", __func__);
            wl_display_cancel_read(m_display);
            break;
        }

        // Flush requests to ensure server gets listener setups etc. before we block
        if (wl_display_flush(m_display) == -1 && errno != EAGAIN) {
            error_log("%s: wl_display_flush() failed: %s (%d). Exiting thread.", __func__, strerror(errno), errno);
            wl_display_cancel_read(m_display);
            break;
        }

        // Block in poll() until Wayland FD has events OR interrupt pipe is written/closed
        poll_ret = poll(fds, 2, -1); // No timeout

        if (poll_ret < 0) {
            if (errno == EINTR) {
                // Interrupted by an unrelated signal. The read lock taken by wl_display_prepare_read() must be
                // released before restarting the loop, otherwise the next prepare_read() increments the reader
                // count a second time for this thread and wl_display_read_events() blocks forever waiting for a
                // reader that will never arrive.
                wl_display_cancel_read(m_display);
                continue;
            }
            error_log("%s: poll() failed: %s (%d). Exiting thread.", __func__, strerror(errno), errno);
            wl_display_cancel_read(m_display); // Need to cancel before error exit? Yes.
            break;
        }

        // Check for interrupt first
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            debug_log("INFO: %s: Interrupt or pipe error detected.", __func__);
            if (fds[1].revents & POLLIN) { // Drain pipe if readable
                char buf[8]; [[maybe_unused]] ssize_t drain = read(m_interrupt_pipe_fd[0], buf, sizeof(buf));
            }
            wl_display_cancel_read(m_display); // Cancel Wayland read preparation
            break; // Exit loop on interrupt
        }

        // Check for Wayland events or errors on Wayland FD
        if (fds[0].revents & (POLLERR | POLLHUP)) {
            error_log("%s: Error/Hangup on Wayland display FD. Exiting thread.", __func__);
            // Don't need to cancel read if FD is likely dead
            break;
        }

        // If we woke up for Wayland FD, read events
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(m_display) == -1) {
                error_log("%s: wl_display_read_events() failed. Exiting thread.", __func__);
                break;
            }
            // Dispatch the read events which trigger callbacks
            if (wl_display_dispatch_pending(m_display) == -1) {
                error_log("%s: wl_display_dispatch_pending() failed after read. Exiting thread.", __func__);
                break;
            }
        } else {
            // Woke up but not for Wayland FD (shouldn't happen with poll=-1 unless interrupted)
            wl_display_cancel_read(m_display);
        }

        // Re-check interrupt flag after dispatching events
        if (m_interrupt_monitor.load(std::memory_order_relaxed)) {
            debug_log("INFO: %s: Interrupt detected after event dispatch.", __func__);
            break;
        }

        // The dispatch above may have run the global_remove callback. No read lock is held here, because
        // wl_display_read_events() released it.
        if (m_globals_lost.load(std::memory_order_relaxed)) {
            error_log("%s: Required Wayland global removed by the compositor. Exiting thread.", __func__);
            break;
        }
    } // end while

    debug_log("INFO: %s: Wayland monitor thread exiting.", __func__);

    // Every exit above other than the interrupt-driven one is a failure: the compositor hung up, a read or a
    // dispatch failed, or a required global was removed. In those cases no further idle notifications will
    // arrive, so the monitor must stop advertising itself as available. Leaving m_initialized set would make
    // IsAvailable() keep returning true while GetIdleSeconds() served a frozen value for the life of the
    // daemon, pinning the session as permanently active or permanently idle. Clearing it makes
    // GetIdleTimeSeconds() fall back to the other detection paths and to event_detect.
    //
    // The requested-stop case is deliberately left alone: Stop() clears m_initialized itself after joining
    // this thread and cleaning up the Wayland resources, and clearing it here would only duplicate that
    // bookkeeping ahead of the join. Both orderings are harmless (the stores are idempotent and this tail
    // touches no Wayland resource), but Stop() stays the single owner of the ordered shutdown.
    if (!m_interrupt_monitor.load(std::memory_order_relaxed)) {
        error_log("%s: Wayland monitor thread exited unexpectedly. Marking the monitor unavailable.", __func__);

        // Reset the reported state first so that a reader that still sees m_initialized set does not observe a
        // stale idle time, and so that a later Start() does not inherit it.
        m_is_idle.store(false, std::memory_order_relaxed);
        m_idle_start_time.store(0, std::memory_order_relaxed);
        m_initialized.store(false);
    }

    // Cleanup of Wayland resources happens in Stop() or ~WaylandIdleMonitor()
    // which is called after this thread is joined.
}

// IsAvailable getter
bool WaylandIdleMonitor::IsAvailable() const {
    return m_initialized.load();
}

// GetSocketName getter
const std::string& WaylandIdleMonitor::GetSocketName() const {
    return m_socket_name;
}

// IsIdle getter
bool WaylandIdleMonitor::IsIdle() const {
    return m_is_idle.load(std::memory_order_relaxed);
}

// GetIdleSeconds getter
int64_t WaylandIdleMonitor::GetIdleSeconds() const {
    if (!m_initialized.load()) {
        return -1; // Indicate not available rather than 0 (active)
    }
    if (m_is_idle.load(std::memory_order_relaxed)) {
        int64_t start_time = m_idle_start_time.load(std::memory_order_relaxed);
        int64_t current_time = GetUnixEpochTime(); // Assumed thread-safe
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
    debug_log("INFO: %s: Global: %s v%u (name %u)", __func__, interface, version, name);

    // wl_registry_bind() returns nullptr on allocation failure or object map exhaustion. The bound pointer must
    // be checked before it is used at all: debug_log() is a function template, so wl_proxy_get_version() below
    // is evaluated regardless of whether debug logging is enabled, and it dereferences the proxy unguarded.
    if (strcmp(interface, wl_seat_interface.name) == 0 && monitor->m_seat == nullptr) {
        monitor->m_seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)) // Request v5 for release
            );

        if (monitor->m_seat == nullptr) {
            error_log("%s: Failed to bind wl_seat (name %u).", __func__, name);
        } else {
            monitor->m_seat_id = name;
            debug_log("INFO: %s: Bound wl_seat (name %u) version %u.",
                      __func__,
                      name,
                      wl_proxy_get_version((wl_proxy*)monitor->m_seat));
        }
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0 &&
               monitor->m_idle_notifier == nullptr) {
        monitor->m_idle_notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, std::min(version, 1u)) // Request v1 or v2? Start with 1.
            );

        if (monitor->m_idle_notifier == nullptr) {
            error_log("%s: Failed to bind %s (name %u).", __func__, ext_idle_notifier_v1_interface.name, name);
        } else {
            monitor->m_idle_notifier_id = name;
            debug_log("INFO: %s: Bound %s (name %u) version %u.",
                      __func__,
                      ext_idle_notifier_v1_interface.name,
                      name,
                      wl_proxy_get_version((wl_proxy*)monitor->m_idle_notifier));
        }
    }
}

void WaylandIdleMonitor_HandleGlobalRemove(void *data, wl_registry * /* registry */, uint32_t name)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    monitor->OnGlobalRemoved(name);
}

void WaylandIdleMonitor_HandleIdled(void *data, ext_idle_notification_v1 * /* notification */)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    if (!monitor->m_is_idle.exchange(true, std::memory_order_relaxed)) { // Only update time on transition
        monitor->m_idle_start_time.store(GetUnixEpochTime(), std::memory_order_relaxed);
        debug_log("INFO: %s: Wayland Idle state entered at %lld", __func__, monitor->m_idle_start_time.load(std::memory_order_relaxed));
    }
}

void WaylandIdleMonitor_HandleResumed(void *data, ext_idle_notification_v1 * /* notification */)
{
    WaylandIdleMonitor *monitor = static_cast<WaylandIdleMonitor*>(data);
    if (monitor->m_is_idle.exchange(false, std::memory_order_relaxed)) { // Only update time on transition
        monitor->m_idle_start_time.store(0, std::memory_order_relaxed);
        debug_log("INFO: %s: Wayland Idle state exited (resumed).", __func__);
    }
}

// Define the actual static listener structs using positional initialization
const struct wl_registry_listener g_registry_listener = {
    /* .global = */ WaylandIdleMonitor_HandleGlobal,
    /* .global_remove = */ WaylandIdleMonitor_HandleGlobalRemove
};

const struct ext_idle_notification_v1_listener g_idle_notification_listener = {
    /* .idled = */ WaylandIdleMonitor_HandleIdled,
    /* .resumed = */ WaylandIdleMonitor_HandleResumed
};

} // extern "C"


} // namespace IdleDetect

//!
//! \brief Signal handler
//! \param signum
//!
void HandleSignal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        // Use log() here as it's presumably safe enough during shutdown signal
        normal_log("INFO: %s: Received signal %d. Requesting shutdown.",
            __func__,
            signum);
        g_shutdown_requested.store(true);

        g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.store(true);
        g_idle_detect_control_monitor.cv_idle_detect_control_monitor_thread.notify_all();
    } else {
        normal_log("INFO: %s: Received unexpected signal %d.",
            __func__,
            signum);
    }
}

// Helper function to get the user's config path
// Returns empty path on error
static fs::path GetUserConfigPath() {
    // GetEnvVariable is in your util.h/cpp
    std::optional<std::string> home_dir_opt = GetEnvVariable("HOME");
    if (!home_dir_opt) {
        error_log("ERROR: %s: HOME environment variable not set. Cannot determine user config path.", __func__);
        return {}; // Return empty path
    }
    // Standard XDG config location
    return fs::path(home_dir_opt.value()) / ".config" / "idle_detect.conf";
}

//!
//! \brief Runs one discovery-and-reconcile pass over the global idle source pool.
//!
//! Both inputs are re-derived from the running system on every call, and that is the entire point. The hints
//! change when a compositor or X server starts or stops, and the shell's bus name ownership changes when the
//! desktop shell starts or stops, so anything cached here would be the frozen-at-exec answer this design
//! exists to eliminate, merely relocated.
//!
//! Finding nothing is a normal outcome rather than a failure, so this reports nothing and cannot fail. An
//! empty pool makes GetIdleTimeSeconds() report IDLE_NO_GUI_SESSION, which the main loop already handles by
//! deferring to event_detect, and the very next pass picks the session up once it exists.
//!
//! ShellMonitor holds no state and no connection, so it is constructed per call rather than kept alive across
//! calls. The D-Bus connection underneath it is GIO's shared per-process session bus connection, which is
//! established once and reused.
//!
static void ReconcileIdleSources()
{
    const IdleDetect::ShellMonitor shell_monitor;

    IdleDetect::g_idle_source_pool.Reconcile(IdleDetect::DiscoverEndpoints(IdleDetect::BuildDiscoveryHints()),
                                             shell_monitor.DetectShellKind());
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

    fs::path config_file_to_load; // Will hold the final path to the config file

    // --- Determine Config Path ---
    if (argc == 1) {
        // --- Case 1: No argument provided, use default user path ---
        normal_log("INFO: %s: No config file specified, using default user path.", __func__);
        config_file_to_load = GetUserConfigPath();
        if (config_file_to_load.empty()) {
            error_log("%s: HOME environment variable not set. Cannot determine default config path.", __func__);
            return 1;
        }

        // --- First-Run User Config Setup (only for default path) ---
        if (!fs::exists(config_file_to_load)) {
            // Path to the system-wide default config installed by the package
            fs::path default_config_template = "/usr/share/idle_detect/idle_detect.conf.default";

            if (!fs::exists(default_config_template)) {
                // The default template for a package install does not exist. Check locally installed path
                default_config_template = "/usr/local/share/idle_detect/idle_detect.conf.default";
            }

            normal_log("INFO: %s: User config not found at '%s'. Creating from default template...",
                       __func__,
                       config_file_to_load.string());

            if (fs::exists(default_config_template)) {
                try {
                    // Ensure parent directory (e.g., ~/.config) exists
                    fs::create_directories(config_file_to_load.parent_path());
                    // Copy the default file to the user's config location
                    fs::copy_file(default_config_template, config_file_to_load);
                    normal_log("INFO: %s: Successfully created user config file.", __func__);
                } catch (const fs::filesystem_error& e) {
                    error_log("%s: Failed to create user config from template: %s", __func__, e.what());
                    return 1; // Exit if we can't create the user config
                }
            } else {
                normal_log("WARN: %s: Default config template not found at '%s'. Proceeding with internal defaults.",
                    __func__,
                    default_config_template.string());
                // Let the app continue; ReadAndUpdateConfig will handle the non-existent file
            }
        }
    } else if (argc == 2) {
        // --- Case 2: One argument provided, use it as the path ---
        config_file_to_load = fs::path(argv[1]);
        normal_log("INFO: %s: Using specified config file: %s", __func__, config_file_to_load.string());

        // The first-run copy logic is skipped; we assume the user provided a valid file.
        // Warn if it doesn't exist, as the app will run with only internal defaults.
        if (!fs::exists(config_file_to_load)) {
            normal_log("WARN: %s: Specified config file does not exist.", __func__);
        }
    } else {
        // --- Case 3: Too many arguments, show usage and exit ---
        error_log("%s: Too many arguments provided. Usage: %s [path_to_config_file]", __func__, argc > 0 ? argv[0] : "idle_detect");
        return 1;
    }

    // --- Configuration Loading ---
    // This block now uses the 'config_file_to_load' path determined above
    try {
        g_config.ReadAndUpdateConfig(config_file_to_load);
    } catch (const std::exception& e) {
        error_log("ERROR: %s: Failed to read/process config from '%s': %s",
                  __func__,
                  config_file_to_load.string().c_str(), e.what());
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

    normal_log("INFO: %s: idle_detect C++ program, %s, started, pid %i",
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
    normal_log("INFO: %s: Starting Idle Detect Control Monitor thread...", __func__);
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
            normal_log("WARN: %s: Control monitor thread started but not initialized quickly.", __func__);
        } else {
            debug_log("INFO: %s: Control monitor thread started and initialized.", __func__);
        }
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Idle Detect Control Monitor thread: %s. Exiting.", __func__, e.what());
        return 1;
    } catch (...) {
        error_log("%s: Unknown error starting Idle Detect Control Monitor thread. Exiting.", __func__);
        return 1;
    }

    // --- Initial discovery pass ---
    //
    // This replaces the one-shot Wayland monitor that used to be started here, which ran only when
    // getenv("WAYLAND_DISPLAY") was set at exec, bound itself to whatever socket that named, and was never
    // revisited.
    //
    // Startup is not special. Finding nothing here is a normal state, not a failure and not a reason to
    // exit: the graphical session may simply not exist yet, which is the ordinary case for a user unit
    // ordered after graphical-session.target on a machine that boots to a display manager. The main loop
    // reconciles again on every iteration and picks the session up whenever it appears.
    normal_log("INFO: %s: Running initial idle source discovery...", __func__);

    ReconcileIdleSources();

    normal_log("INFO: %s: Initial discovery found %u endpoint source(s)%s.",
               __func__,
               IdleDetect::g_idle_source_pool.EndpointSourceCount(),
               IdleDetect::g_idle_source_pool.HasShellSource() ? " and a desktop shell source" : "");

    // --- Main Loop ---
    bool first_check = true;
    bool was_previously_idle = false; // Track state changes
    IdleDetect::IdleDetectControlMonitor::State previous_control_state = IdleDetect::IdleDetectControlMonitor::UNKNOWN;

    //bool send_forced_idle = false;
    int64_t effective_last_active_time_prev = 0;
    int64_t effective_last_active_time = 0;
    bool using_event_detect_as_only_source = false;

    while (!g_shutdown_requested.load()) {
        // Reconcile before resolving, so this iteration's reading comes from the sources that exist now. This
        // is the self-heal path: a graphical session that appears after the daemon started is picked up here,
        // within one check interval, with no restart and no external wrapper watching for it. It is equally
        // the teardown path, since an endpoint whose socket is gone is evicted rather than left reporting
        // against a compositor that no longer exists.
        ReconcileIdleSources();

        int64_t idle_seconds = IdleDetect::GetIdleTimeSeconds();

        if (idle_seconds >= 0) {
            debug_log("INFO: %s: idle time from GUI session: %lld seconds.",
                      __func__,
                      (int64_t)idle_seconds);
        }

        // IDLE_NO_GUI_SESSION means no idle source exists at all, and it overrides the use_event_detect
        // config setting, because in that state the only activity there is to see -- tty and ssh -- is
        // visible only to event_detect.
        //
        // The override is re-derived on every iteration rather than latched. It used to be set once and
        // never cleared, which pinned a daemon that started before its graphical session to event_detect for
        // the rest of its life: discovery would find the session, GetIdleTimeSeconds() would report a real
        // GUI idle time, and the pipe notification would still be suppressed as circular. That would have
        // masked most of this fix, since starting before the session is exactly the case being fixed.
        //
        // IDLE_ERROR deliberately does not hold the override on. Sources exist in that case, so a GUI
        // session exists and only the reading failed. Whether to fall back to event_detect for a failed
        // reading is the use_event_detect setting's decision, not this override's.
        if (idle_seconds == IdleDetect::IDLE_NO_GUI_SESSION && !use_event_detect) {
            if (!using_event_detect_as_only_source) {
                normal_log("INFO: %s: No GUI session found. Overriding use_event_detect and using "
                           "event_detect as the only source.",
                           __func__);
            }

            using_event_detect_as_only_source = true;
        } else if (using_event_detect_as_only_source) {
            normal_log("INFO: %s: A GUI session is now present. No longer overriding use_event_detect.",
                       __func__);

            using_event_detect_as_only_source = false;
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

                // If idle_seconds is < 0, it is a sentinel from IdleDetect::GetIdleTimeSeconds() -- IDLE_ERROR if
                // the GUI sources could not be read, or IDLE_NO_GUI_SESSION if there is no GUI session at all --
                // and the value must not be used directly.
                // If idle_seconds is >= 0, it indicates a valid idle time from GUI session.
                // If shmem_timestamp == 0, it indicates that force_idle was set via event_detect directly, and we should use the
                // calculated idle time. This is important to capture force_idle injected directly into event_detect via a script
                // from a service. Note this has the same effect as setting FORCED_IDLE in the control monitor.
                if (idle_seconds >= 0 && shmem_timestamp > 0) {
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
                debug_log("INFO: %s: Attempting to get idle information from event_detect via file: %s",
                          __func__,
                          dat_file_path.string());

                int64_t file_timestamp = IdleDetect::ReadLastActiveTimeFile(dat_file_path);
                if (file_timestamp > 0) {
                    int64_t current_time = GetUnixEpochTime();
                    int64_t calculated_idle = current_time - file_timestamp;
                    calculated_idle = (calculated_idle > 0) ? calculated_idle : 0; // Ensure non-negative

                    // If idle_seconds is < 0, it is a sentinel from IdleDetect::GetIdleTimeSeconds() -- IDLE_ERROR if
                    // the GUI sources could not be read, or IDLE_NO_GUI_SESSION if there is no GUI session at all --
                    // and the value must not be used directly.
                    // If idle_seconds is >= 0, it indicates a valid idle time from GUI session.
                    // If file_timestamp == 0, it indicates that force_idle was set via event_detect directly, and we should use the
                    // calculated idle time. This is important to capture force_idle injected directly into event_detect via a script
                    // from a service. Note this has the same effect as setting FORCED_IDLE in the control monitor.
                    if (idle_seconds >= 0 && file_timestamp > 0) {
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
        if (is_currently_idle != was_previously_idle || first_check) {
            if (is_currently_idle) {
                // Became Idle
                normal_log("INFO: %s: User became idle (%llds >= %ds).",
                    __func__,
                    (int64_t)idle_seconds, idle_threshold_seconds);

                if (execute_dc_control_scripts) {
                    IdleDetect::ExecuteCommandBackground(idle_command);
                }
            } else {
                // Became Active
                normal_log("INFO: %s: User became active (%llds < %ds).",
                    __func__,
                    (int64_t)idle_seconds,
                    idle_threshold_seconds);

                if (execute_dc_control_scripts) {
                    IdleDetect::ExecuteCommandBackground(active_command);
                }
            }

            was_previously_idle = is_currently_idle; // Update previous state
            first_check = false; // Clear first check flag
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
            int64_t event_timestamp = effective_last_active_time;

            if (control_state != previous_control_state) {
                switch(control_state) {
                case IdleDetect::IdleDetectControlMonitor::NORMAL:
                    event_type = EventMessage::USER_UNFORCE;
                    event_timestamp = GetUnixEpochTime(); // Use current time for normal state
                    break;
                case IdleDetect::IdleDetectControlMonitor::FORCED_IDLE:
                    event_type = EventMessage::USER_FORCE_IDLE;
                    event_timestamp = GetUnixEpochTime(); // Use current time for forced idle state
                    break;
                case IdleDetect::IdleDetectControlMonitor::FORCED_ACTIVE:
                    event_type = EventMessage::USER_FORCE_ACTIVE;
                    event_timestamp = GetUnixEpochTime(); // Use current time for forced active state
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

            IdleDetect::SendPipeNotification(event_registration_pipe_path, event_timestamp, event_type);

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

    normal_log("INFO: %s: Shutdown requested. Cleaning up...",
        __func__);

    // --- Shutdown sequence ---

    // --- Tear down the idle sources ---
    //
    // Done before the control monitor stops, so that the compositor connections and monitor threads the pool
    // owns are joined while the process is still otherwise intact. Shutdown() is idempotent and leaves the
    // pool reusable, so the destructor that runs at exit finds nothing left to do.
    normal_log("INFO: %s: Stopping idle sources...", __func__);
    IdleDetect::g_idle_source_pool.Shutdown();
    normal_log("INFO: %s: Idle sources stopped.", __func__);

    // --- Stop Idle Detect Control Monitor thread ---
    if (control_monitor_started && g_idle_detect_control_monitor.m_idle_detect_control_monitor_thread.joinable()) {
        normal_log("INFO: %s: Stopping Idle Detect Control Monitor thread...", __func__);
        // Flag should have been set by HandleSignal
        g_idle_detect_control_monitor.m_interrupt_idle_detect_control_monitor.store(true);
        g_idle_detect_control_monitor.cv_idle_detect_control_monitor_thread.notify_all();
        try {
            g_idle_detect_control_monitor.m_idle_detect_control_monitor_thread.join();
            normal_log("INFO: %s: Idle Detect Control Monitor thread stopped.", __func__);
        } catch (const std::system_error& e) {
            error_log("%s: Error joining control monitor thread: %s", __func__, e.what());
        }
    }

    normal_log("Idle Detect shutdown complete.");
    return g_exit_code.load();
}
