// idle_detect.cpp
#include "idle_detect.h"
#include "util.h"
#include "tinyformat.h"

#include <iostream>
#include <thread>
#include <cstdlib> // For system()
#include <cstdint> // For int64_t
#include <string>
#include <cstring> // For strcmp

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gio/gio.h> // For D-Bus
#endif

//!
//! \brief This is populated by main from an early GetArg so that the debug_log doesn't use the heavyweight GetArg call constantly.
//!
std::atomic<bool> g_debug = false;

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

static bool isWaylandSession() {
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    return (waylandDisplay != nullptr && strlen(waylandDisplay) > 0);
}

static bool isGnomeSession() {
    const char* gnomeSession = getenv("GNOME_DESKTOP_SESSION_ID");
    return (gnomeSession != nullptr && strlen(gnomeSession) > 0);
}

static bool isKdeSession() {
    const char* kdeSession = getenv("KDE_SESSION_VERSION");
    return (kdeSession != nullptr && strlen(kdeSession) > 0);
}

static int64_t getIdleTimeWaylandGnome() {
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection) {
        debug_log("Error: Could not connect to session bus for Gnome idle query.");
        return 0;
    }

    GVariant* result = g_dbus_connection_call_sync(
        connection,
        "org.gnome.SessionManager",
        "/org/gnome/SessionManager/Presence",
        "org.gnome.SessionManager.Presence",
        "GetStatus",
        nullptr,
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
        );

    int64_t idleTimeSeconds = 0;
    if (result) {
        guint status;
        g_variant_get(result, "(u)", &status);
        // Status 0: Available, Status 1: Idle, Status 2: Away
        if (status == 1 || status == 2) {
            // Gnome's "GetStatus" doesn't directly give idle time.
            // Need to investigate further for the correct Gnome API.
            debug_log("Gnome session is idle or away, but cannot get precise idle time via GetStatus.");
            // Placeholder - need to find the correct Gnome API for idle time.
            idleTimeSeconds = DEFAULT_IDLE_THRESHOLD_SECONDS; // Returning a default value for now.
        } else {
            debug_log("Gnome session is active.");
        }
        g_variant_unref(result);
    } else {
        debug_log("Error calling GetStatus on Gnome SessionManager.");
    }

    g_object_unref(connection);
    return idleTimeSeconds;
}

static int64_t getIdleTimeWaylandKde() {
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection) {
        debug_log("Error: Could not connect to session bus for KDE idle query.");
        return 0;
    }

    // Need to find the correct KDE D-Bus service and method for idle time.
    // A common one is org.kde.Solid.PowerManagement.PolicyAgent, but the method might vary.
    // Placeholder - need to research the correct KDE API.
    debug_log("KDE Wayland idle time query not yet implemented.");
    g_object_unref(connection);
    return 0;
}

int64_t getIdleTimeSeconds() {
    if (isWaylandSession()) {
        debug_log("Wayland session detected.");
        if (isGnomeSession()) {
            debug_log("Gnome session detected on Wayland.");
            return getIdleTimeWaylandGnome();
        } else if (isKdeSession()) {
            debug_log("KDE session detected on Wayland.");
            return getIdleTimeWaylandKde();
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

int main() {
    int idleThresholdSeconds = IdleDetect::DEFAULT_IDLE_THRESHOLD_SECONDS;
    int checkIntervalSeconds = IdleDetect::DEFAULT_CHECK_INTERVAL_SECONDS;

    log("Idle Detect started.");
    debug_log("INFO: %s: Idle threshold: %d seconds, Check interval: %d seconds",
              __func__,
              idleThresholdSeconds,
              checkIntervalSeconds);

    while (true) {
        int64_t idleSeconds = IdleDetect::getIdleTimeSeconds();
        if (idleSeconds >= idleThresholdSeconds) {
            log("INFO: %s: User is idle (%lld seconds)", __func__, idleSeconds);
        } else {
            debug_log("INFO: %s: User is active (%lld seconds)", idleSeconds);
        }
        std::this_thread::sleep_for(std::chrono::seconds(checkIntervalSeconds));
    }

    return 0;
}
