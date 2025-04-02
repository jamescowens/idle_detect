/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include "idle_detect.h"
#include "util.h"

#include <fstream>
#include <thread>
#include <cstdlib> // For system()
#include <cstdint> // For int64_t
#include <string>
#include <cstring> // For strcmp
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gio/gio.h> // For D-Bus

//!
//! \brief This is populated by main from an early GetArg so that the debug_log doesn't use the heavyweight GetArg call constantly.
//!
std::atomic<bool> g_debug = false;

//!
//! \brief Global config singleton
//!
IdleDetectConfig g_config;


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

static bool IsKdeSession() {
    const char* kdeSession = getenv("KDE_SESSION_VERSION");
    return (kdeSession != nullptr && strlen(kdeSession) > 0);
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

/**
 * @brief Gets the idle time on a GNOME Wayland session via D-Bus.
 * @return int64_t Idle time in seconds, or 0 if active or on error.
 */
static int64_t GetIdleTimeWaylandGnome() {
    // Assumes IsGnomeSession() has already confirmed this is appropriate to call.
    // Logging function is snake_case
    debug_log("%s: Querying GNOME Mutter IdleMonitor via D-Bus.", __func__); // __func__ is standard

    GDBusConnection* connection = nullptr;
    GError* dbus_error = nullptr;
    GVariant* dbus_result = nullptr;
    // variable_name is snake_case
    int64_t idle_time_seconds = 0; // Default to 0 (active or error)

    // Get connection to the session bus
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &dbus_error);
    if (!connection) {
        if (dbus_error) {
            // Logging function is snake_case
            debug_log("%s: Error connecting to session bus for Gnome idle query: %s", __func__, dbus_error->message);
            g_error_free(dbus_error);
        } else {
            // Logging function is snake_case
            debug_log("%s: Error connecting to session bus for Gnome idle query (unknown error).", __func__);
        }
        return 0; // Return 0 (active) on connection error
    }

    // D-Bus details for getting idle time from Mutter's IdleMonitor interface
    const char* service_name = "org.gnome.Mutter.IdleMonitor";
    const char* object_path = "/org/gnome/Mutter/IdleMonitor/Core";
    const char* interface_name = "org.gnome.Mutter.IdleMonitor";
    const char* method_name = "GetIdletime";

    // Call the GetIdletime method
    dbus_result = g_dbus_connection_call_sync(connection,
                                              service_name,
                                              object_path,
                                              interface_name,
                                              method_name,
                                              nullptr, // No input parameters for GetIdletime
                                              G_VARIANT_TYPE("(t)"), // Expected reply: tuple containing one uint64 ('t')
                                              G_DBUS_CALL_FLAGS_NONE,
                                              500, // Timeout in milliseconds (adjust if needed)
                                              nullptr, // Cancellable
                                              &dbus_error);

    if (dbus_error) {
        // Logging function is snake_case
        debug_log("%s: Error calling %s on %s: %s", __func__, method_name, interface_name, dbus_error->message);
        g_error_free(dbus_error);
        // Keep idle_time_seconds = 0 on error
    } else if (dbus_result) {
        // D-Bus 't' corresponds to uint64_t for idle time in milliseconds
        uint64_t idle_time_ms = 0;
        // Unpack the uint64 from the tuple reply "(t)"
        g_variant_get(dbus_result, "(t)", &idle_time_ms);

        // Convert milliseconds to seconds for the return value
        idle_time_seconds = static_cast<int64_t>(idle_time_ms / 1000);

        // Logging function is snake_case
        // Use %llu for uint64_t and %lld for int64_t with appropriate casts for portability
        debug_log("%s: Mutter IdleMonitor reported idle time: %llu ms (%lld seconds)",
                  __func__,
                  static_cast<unsigned long long>(idle_time_ms),
                  static_cast<long long>(idle_time_seconds));

        g_variant_unref(dbus_result);
    } else {
        // Logging function is snake_case
        debug_log("%s: Call to %s on %s returned no result and no error.", __func__, method_name, interface_name);
        // Keep idle_time_seconds = 0
    }

    // Clean up the connection reference
    g_object_unref(connection);

    return idle_time_seconds;
}

// Renamed function (placeholder)
static int64_t GetIdleTimeWaylandKde() {
    // Logging function call is snake_case
    debug_log("KDE Wayland idle time query not yet implemented.");
    return 0;
}

/*
// Renamed function (conceptual DBus example)
static int64_t GetIdleTimeWaylandKdeDBus() {
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection) {
        // Logging function call is snake_case
        debug_log("Error: Could not connect to session bus for KDE idle query.");
        return 0;
    }
    // ... (DBus call logic) ...
    GError *dbus_error = nullptr;
    // ...
    if (dbus_error) {
        // Logging function call is snake_case
        debug_log("Error calling KDE DBus method %s: %s", method_name, dbus_error->message);
        g_error_free(dbus_error);
    } else if (dbus_result) {
        // ... (process result) ...
        // Logging function call is snake_case
        debug_log("KDE DBus reported idle time: %lld ms", idle_time_ms);
        // ...
    }
    // ...
    return idle_time_seconds;
}
*/


int64_t GetIdleTimeSeconds() {
    if (IsWaylandSession()) {
        debug_log("Wayland session detected.");
        if (IsGnomeSession()) {
            debug_log("Gnome session detected on Wayland.");
            return GetIdleTimeWaylandGnome();
        } else if (IsKdeSession()) {
            debug_log("KDE session detected on Wayland.");
            return GetIdleTimeWaylandKde();
        } else {
            debug_log("Unknown Wayland compositor. Falling back to other methods.");
            // Attempt gnome-screensaver-command and kdialog as fallbacks (might not work well on Wayland)
            FILE* pipe = popen("gnome-screensaver-command --query", "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    std::string output(buffer);
                    if (output.find("is active") != std::string::npos) {
                        pclose(pipe);
                        return DEFAULT_IDLE_THRESHOLD_SECONDS; // Simulate idle
                    }
                }
                pclose(pipe);
            }

            std::string kdialog_command = tinyformat::format("kdialog --checkidle %d", DEFAULT_IDLE_THRESHOLD_SECONDS);
            pipe = popen(kdialog_command.c_str(), "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    int64_t idleTimeMs = std::stoll(buffer);
                    pclose(pipe);
                    if (idleTimeMs > 0) {
                        return idleTimeMs / 1000;
                    } else {
                        return 0; // Not idle
                    }
                }
                pclose(pipe);
            }
            return 0;
        }
    } else {
        debug_log("X11 session detected.");
        Display *display = XOpenDisplay(nullptr);
        if (!display) {
            debug_log("Error: Could not open X display. Falling back to other methods.");
            // Attempt gnome-screensaver-command and kdialog as fallbacks
            FILE* pipe = popen("gnome-screensaver-command --query", "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    std::string output(buffer);
                    if (output.find("is active") != std::string::npos) {
                        pclose(pipe);
                        return DEFAULT_IDLE_THRESHOLD_SECONDS; // Simulate idle
                    }
                }
                pclose(pipe);
            }

            std::string kdialog_command = tinyformat::format("kdialog --checkidle %d", DEFAULT_IDLE_THRESHOLD_SECONDS);
            pipe = popen(kdialog_command.c_str(), "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    int64_t idleTimeMs = std::stoll(buffer);
                    pclose(pipe);
                    if (idleTimeMs > 0) {
                        return idleTimeMs / 1000;
                    } else {
                        return 0; // Not idle
                    }
                }
                pclose(pipe);
            }
            return 0;
        }

        int event_base, error_base;
        if (!XScreenSaverQueryExtension(display, &event_base, &error_base)) {
            debug_log("Error: XScreenSaver extension not available. Falling back to other methods.");
            XCloseDisplay(display);
            // Attempt gnome-screensaver-command and kdialog as fallbacks
            FILE* pipe = popen("gnome-screensaver-command --query", "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    std::string output(buffer);
                    if (output.find("is active") != std::string::npos) {
                        pclose(pipe);
                        return DEFAULT_IDLE_THRESHOLD_SECONDS; // Simulate idle
                    }
                }
                pclose(pipe);
            }

            std::string kdialog_command = tinyformat::format("kdialog --checkidle %d", DEFAULT_IDLE_THRESHOLD_SECONDS);
            pipe = popen(kdialog_command.c_str(), "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    int64_t idleTimeMs = std::stoll(buffer);
                    pclose(pipe);
                    if (idleTimeMs > 0) {
                        return idleTimeMs / 1000;
                    } else {
                        return 0; // Not idle
                    }
                }
                pclose(pipe);
            }
            return 0;
        }

        XScreenSaverInfo *info = XScreenSaverAllocInfo();
        if (!info) {
            log("Error: Could not allocate XScreenSaverInfo.");
            XCloseDisplay(display);
            return 0;
        }

        Window root = DefaultRootWindow(display);
        XScreenSaverQueryInfo(display, root, info);
        int64_t idleTimeMs = info->idle;
        XFree(info);
        XCloseDisplay(display);

        return idleTimeMs / 1000;
    }
}

} // namespace IdleDetect

int main(int argc, char* argv[]) { // main remains lowercase

    // --- Configuration Loading ---
    if (argc != 2) {
        // Use cerr directly for pre-config errors
        std::cerr << "ERROR: main: One argument must be specified for the location of the config file." << std::endl;
        return 1;
    }
    fs::path config_file_path(argv[1]);
    // Can't use log() before g_debug is set, use std::cout/cerr or defer logging
    if (fs::exists(config_file_path) && fs::is_regular_file(config_file_path)) {
        std::cout << "INFO: main: Using config from " << config_file_path.string() << std::endl;
    } else {
        std::cout << "WARNING: main: Argument invalid for config file '" << config_file_path.string() << "'. Using defaults." << std::endl;
        config_file_path = "";
    }

    try {
        // Assuming ReadAndUpdateConfig is PascalCase
        g_config.ReadAndUpdateConfig(config_file_path);
    } catch (const std::exception& e) {
        // Can't use error_log reliably yet
        std::cerr << "ERROR: main: Failed to read/process config: " << e.what() << std::endl;
        return 1;
    }

    try {
        // Assuming GetArg is PascalCase
        g_debug = std::get<bool>(g_config.GetArg("debug"));
    } catch (...) {
        std::cerr << "ERROR: main: Failed to read 'debug' config setting. Debug disabled." << std::endl;
        g_debug = false;
    }

    // --- Get Relevant Config Values ---
    // (variable_names are snake_case)
    int idle_threshold_seconds = IdleDetect::DEFAULT_IDLE_THRESHOLD_SECONDS;
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
        // Logging function call is snake_case
        error_log("ERROR: main: Configuration value missing or has wrong type: %s. Using defaults where possible.", e.what());
    } catch (...) {
        // Logging function call is snake_case
        error_log("ERROR: main: Unknown error accessing configuration values.");
        return 1;
    }

    fs::path pipe_path = event_data_path / "event_registration_pipe";

    // Logging function call is snake_case
    log("Idle Detect started.");
    // Logging function call is snake_case
    debug_log("INFO: main: Idle threshold: %d seconds, Check interval: %d seconds, Update event_detect: %s, Pipe path: %s",
              idle_threshold_seconds,
              check_interval_seconds,
              should_update_event_detect ? "true" : "false",
              pipe_path.string());

    if (initial_sleep > 0) {
        // Logging function call is snake_case
        log("INFO: main: Waiting for %d seconds to start.", initial_sleep);
        std::this_thread::sleep_for(std::chrono::seconds(initial_sleep));
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

    return 0;
}
