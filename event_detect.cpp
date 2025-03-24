/*
 * Copyright (C) 2025 James C. Owens
 * Portions Copyright (c) 2019 The Bitcoin Core developers
 * Portions Copyright (c) 2025 The Gridcoin developers
 *
 * This code is licensed under the MIT license
 */

#include <cstring>
#include <fcntl.h>

#include <event_detect.h>

pthread_t g_main_thread_id = 0;
std::atomic<int> g_exit_code = 0;

//!
//! \brief global that holds config parameter name-value pairs that were read from config file.
//!
std::multimap<std::string, std::string> g_config;

int g_startup_delay = 0;
fs::path g_event_data_path;
bool g_write_last_active_time_to_file = false;
std::string g_last_active_time_cpp_filename;

//!
//! \brief Global event monitor singleton
//!
EventMonitor g_event_monitor;

//!
//! \brief Global event recorders singleton
//!
EventRecorders g_event_recorders;

// Class EventMonitor

EventMonitor::EventMonitor()
    : m_interrupt_monitor(false)
    , m_event_device_paths()
    , m_initialized(false)
{}

EventMonitor::EventMonitor(std::vector<fs::path> event_device_paths)
    : m_interrupt_monitor(false)
    , m_event_device_paths(event_device_paths)
    , m_initialized(false)
{}

void EventMonitor::EventActivityMonitorThread()
{
    debug_log("INFO: %s: started",
              __func__);

    size_t event_devices_size = 0;
    size_t event_devices_size_prev = 0;
    int64_t event_count_prev = 0;
    int64_t event_count = 0;

    // Set the last active time to the current time at the start of monitoring. This is most likely correct
    // since actions will have to be taken on the system to start this program.
    m_last_active_time = GetUnixEpochTime();

    while (true) {
        debug_log("INFO: %s: event monitor thread loop at top of iteration",
                  __func__);

        std::unique_lock<std::mutex> lock(mtx_event_monitor_thread);
        cv_monitor_thread.wait_for(lock, std::chrono::seconds(1), []{ return g_event_monitor.m_interrupt_monitor.load(); });

        if (g_event_monitor.m_interrupt_monitor) {
            break;
        }

        event_devices_size_prev = GetEventDevices().size();

        UpdateEventDevices();

        event_devices_size = GetEventDevices().size();

        if (m_initialized && event_devices_size != event_devices_size_prev) {
            log("INFO: %s: Input event device count changed. Restarting recorder threads.",
                __func__);

            pthread_kill(g_main_thread_id, SIGHUP);
            m_initialized = false;
        } else {
            m_initialized = true;
        }

        event_count = g_event_recorders.GetTotalEventCount();

        debug_log("INFO: %s: loop: event_count = %lld",
                  __func__,
                  event_count);

        if (event_count != event_count_prev) {
            m_last_active_time = GetUnixEpochTime();
            event_count_prev = event_count;
        }

        debug_log("INFO: %s: loop: m_last_active_time = %lld",
                  __func__,
                  m_last_active_time.load());

        if (g_write_last_active_time_to_file) {
            fs::path last_active_time_filepath = g_event_data_path / g_last_active_time_cpp_filename;

            // Open the file for writing (overwrites)
            std::ofstream output_file(last_active_time_filepath);

            if (!output_file.is_open()) {
                error_log("%s: Could not open file %s for writing.",
                          __func__,
                          last_active_time_filepath);

                g_exit_code = 1;
                pthread_kill(g_main_thread_id, SIGTERM);;
            }

            output_file << m_last_active_time << std::endl;

            if (!output_file.good()) {
                error_log("%s: Error writing to file %s.",
                          __func__,
                          last_active_time_filepath);
                output_file.close();

                g_exit_code = 1;
                pthread_kill(g_main_thread_id, SIGTERM);;
            }
        }
    }
}

bool EventMonitor::IsInitialized()
{
    return m_initialized;
}

int64_t EventMonitor::GetLastActiveTime()
{
    return m_last_active_time.load();
}

std::vector<fs::path> EventMonitor::EnumerateEventDevices()
{
    debug_log("INFO: %s: started",
              __func__);

    std::vector<fs::path> event_devices;

    fs::path event_device_path = "/sys/class/input";

    std::vector<fs::path> event_device_candidates = FindDirEntriesWithWildcard(event_device_path, "event.*");

    debug_log("INFO: %s: event_device_candidates.size() = %u",
              __func__,
              event_device_candidates.size());

    for (const auto& event_device : event_device_candidates) {
        std::stringstream message;

        if (FindDirEntriesWithWildcard(event_device / "device", "mouse.*").empty()) {
            continue;
        }

        event_devices.push_back(event_device);
    }

    if (event_devices.empty()) {
        error_log("%s: No pointing devices identified to monitor. Exiting.",
                  __func__);

        g_exit_code = 1;
        pthread_kill(g_main_thread_id, SIGTERM);
    }

    debug_log("INFO: %s: event_devices.size() = %u",
              __func__,
              event_devices.size());

    return event_devices;
}

std::vector<fs::path> EventMonitor::GetEventDevices()
{
    std::unique_lock<std::mutex> lock(mtx_event_monitor);

    return m_event_device_paths;
}

void EventMonitor::UpdateEventDevices()
{
    debug_log("INFO: %s: started",
              __func__);

    std::unique_lock<std::mutex> lock(mtx_event_monitor);

    m_event_device_paths = EnumerateEventDevices();
}

// Class EventRecorders

EventRecorders::EventRecorders()
    : m_interrupt_recorders(false)
    , m_event_recorder_ptrs()
{}

EventRecorders::EventRecorders(std::vector<fs::path> event_device_paths)
    : m_interrupt_recorders(false)
{
    std::unique_lock<std::mutex> lock(mtx_event_recorders);

    for (const auto& event_device_path : event_device_paths) {
        m_event_recorder_ptrs.emplace_back(std::make_shared<EventRecorder>(event_device_path));
    }
}

std::vector<std::shared_ptr<EventRecorders::EventRecorder>>& EventRecorders::GetEventRecorders()
{
    std::unique_lock<std::mutex> lock(mtx_event_recorders);

    return m_event_recorder_ptrs;
}

void EventRecorders::ResetEventRecorders()
{
    std::unique_lock<std::mutex> lock(mtx_event_recorders);

    m_event_recorder_ptrs.clear();

    for (const auto& event_device_path : g_event_monitor.GetEventDevices()) {
        m_event_recorder_ptrs.emplace_back(std::make_shared<EventRecorder>(event_device_path));
    }
}

//!
//! \brief Returns total event count across all recorders.
//! \return
//!
int64_t EventRecorders::GetTotalEventCount()
{
    int64_t tally = 0;

    for (auto& event_recorder : m_event_recorder_ptrs) {
        tally += event_recorder->GetEventCount();
    }

    return tally;
}

// Class EventRecorders::EventRecorder

EventRecorders::EventRecorder::EventRecorder(fs::path event_device_path)
    : m_event_device_path(event_device_path)
    , m_event_count(0)
{}

//!
//! \brief Returns a read only fs::path of the input event device path associated with the recorder. No locking is
//! needed as this can only be changed on construction.
//!
//! \return fs::path
//!
fs::path EventRecorders::EventRecorder::GetEventDevicePath()
{
    return m_event_device_path;
}

int64_t EventRecorders::EventRecorder::GetEventCount()
{
    // No explicit lock needed.

    return m_event_count.load();
}

void EventRecorders::EventRecorder::EventActivityRecorderThread()
{
    debug_log("INFO: %s: started",
              __func__);

    debug_log("INFO: %s: GetEventDevicePath() = %s",
              __func__,
              GetEventDevicePath());

    fs::path device_access_path = "/dev/input" / GetEventDevicePath().filename();

    debug_log("INFO: %s: device_access_path = %s",
              __func__,
              device_access_path);

    // Note this c-string here should not be a problem for /dev/input, which is
    // standardized and doesn't do anything funky with filenames in other character sets.
    int fd = open(device_access_path.c_str(), O_RDONLY | O_NONBLOCK);;
    struct libevdev *dev = nullptr;

    if (fd < 0) {
        error_log("%s: Failed to open device %s: %s",
                  __func__,
                  device_access_path,
                  strerror(errno));
        g_exit_code = 1;
        return;
    }

    // Initialize libevdev
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
         error_log("%s: Failed to init libevdev for device %s: %s",
                  __func__,
                  device_access_path,
                  strerror(-rc));

        close(fd);
        g_exit_code = 1;
        return;
    }

    debug_log("INFO: %s: Device: %s, Path: %s, Physical Path: %s, Unique: %s",
              __func__,
              libevdev_get_name(dev),
              device_access_path,
              libevdev_get_phys(dev),
              libevdev_get_uniq(dev));

    struct input_event ev;

    while (true) {
        std::unique_lock<std::mutex> lock(g_event_recorders.mtx_event_recorder_threads);
        g_event_recorders.cv_recorder_threads.wait_for(lock, std::chrono::seconds(1),
                                                       []{ return g_event_recorders.m_interrupt_recorders.load(); });

        if (g_event_recorders.m_interrupt_recorders) {
            break;
        }

        debug_log("INFO: %s: event_activity_recorder loop iteration for %s",
                  __func__,
                  GetEventDevicePath());

        int libevdev_mode_flag = LIBEVDEV_READ_FLAG_NORMAL;
        while (true) {
            rc = libevdev_next_event(dev, libevdev_mode_flag /* | LIBEVDEV_READ_FLAG_BLOCKING */, &ev);

            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                ++m_event_count;

                libevdev_mode_flag = LIBEVDEV_READ_FLAG_NORMAL;
            } else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                // We only need to count events to detect activity.
                ++m_event_count;

                libevdev_mode_flag = LIBEVDEV_READ_FLAG_SYNC;

            } else if (rc == -EAGAIN) {
                // No event available, break to the outer loop to listen for signals
                break;
            } else if (rc == -ENODEV) {
                // Device disconnected
                error_log("Device disconnected");
                break;
            } else {
                error_log("%s: reading event: %s",
                          __func__,
                          strerror(-rc));

                // We will break from the read loop and hopefully when it is reentered after 1 sec from the
                // outer thread loop, the error will clear.
                break;
            }
        }
    }

    // Cleanup
    libevdev_free(dev);
    close(fd);
}

void HandleSignals(int signum)
{
    debug_log("INFO: %s: started",
              __func__);

    switch (signum) {
    case SIGINT:
        debug_log("INFO: %s: SIGINT received",
                  __func__);
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
        break;
    case SIGTERM:
        debug_log("INFO: %s: SIGTERM received",
                  __func__);
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
        break;
    case SIGHUP:
        debug_log("INFO: %s: SIGHUP received",
                  __func__);
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
        break;
    default:
        log("WARNING: Unknown signal received.");
        break;
    }
}

void InitiateEventActivityRecorders()
{
    debug_log("INFO: %s: started",
              __func__);

    g_event_recorders.m_interrupt_recorders = false;

    g_event_recorders.ResetEventRecorders();

    for (auto& recorder : g_event_recorders.GetEventRecorders()) {
        recorder->m_event_recorder_thread = std::thread(&EventRecorders::EventRecorder::EventActivityRecorderThread,
                                                        recorder);

        // Spread the thread start out a little bit.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

     for (const auto& recorder : g_event_recorders.GetEventRecorders()) {
        debug_log("INFO: %s: GetEventRecorders() range loop: event device path = %s, thread id = %lld",
                  __func__,
                  recorder->GetEventDevicePath(),
                  recorder->m_event_recorder_thread.get_id());
    }
}

void InitiateEventActivityMonitor()
{
    debug_log("INFO: %s: started",
              __func__);

    g_event_monitor.m_interrupt_monitor = false;

    g_event_monitor.m_event_monitor_thread = std::thread(&EventMonitor::EventActivityMonitorThread, std::ref(g_event_monitor));

}

void CleanUpEventDatFiles()
{
    debug_log("INFO: %s: started",
              __func__);

    fs::path last_active_time_path = g_event_data_path / g_last_active_time_cpp_filename;

    if (fs::exists(last_active_time_path)) {
        try {
            fs::remove(last_active_time_path);
        } catch (std::exception& e) {
            log("WARNING: %s: last_active_time file %s could not be removed",
                __func__,
                last_active_time_path);
        }
    }
}

std::string GetArg(std::string arg, std::string default_value)
{
    auto iter = g_config.find(arg);

    if (iter != g_config.end()) {
        return iter->second;
    } else {
        return default_value;
    }
}

void ProcessArgs()
{
    std::string debug_arg = GetArg("debug", "true");

    if (debug_arg == "1" || ToLower(debug_arg) == "true") {
        g_debug = 1;
    }

    try {
        g_startup_delay = ParseStringToInt(GetArg("startup_delay", "0"));
    } catch (std::exception& e) {
        error_log("%s: startup_delay parameter in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    g_event_data_path = GetArg("event_count_files_path", "/run/event_detect");

    try {
        g_write_last_active_time_to_file = ParseStringToInt(GetArg("write_last_active_time_to_file", "0"));
    } catch (std::exception& e) {
        error_log("%s: write_last_active_time_to_file in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    g_last_active_time_cpp_filename = GetArg("last_active_time_cpp_filename", "last_active_time.dat");
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        error_log("%s: One argument must be specified for the location of the config file.",
                  __func__);
        g_exit_code = 1;

        return g_exit_code;
    }

    fs::path config_file_path(argv[1]);

    if (fs::exists(config_file_path) && fs::is_regular_file(config_file_path)) {
        log("INFO: %s: Using config from %s",
            __func__,
            config_file_path);

        g_config = ReadConfig(config_file_path);
    } else {
        log("WARNING: %s: Argument invalid for config file. Using defaults.",
            __func__);
    }

    ProcessArgs();

    if (g_startup_delay > 0) {
        log("INFO: %s: Waiting for %u seconds to start.",
            __func__,
            g_startup_delay);

        std::this_thread::sleep_for(std::chrono::seconds(g_startup_delay));
    }

    int sig = 0;

    // Need the main thread id to be able to send signal from monitor thread back to main.
    g_main_thread_id = pthread_self();

    // To avoid signal races, block signals in the main thread
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);

    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        perror("pthread_sigmask");
        return 1;
    }

    // Register signal handler
    struct sigaction sa;
    sa.sa_handler = HandleSignals;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        perror("sigaction");
        return 1;
    }

    log("INFO: %s: event_detect C++ program started",
        __func__);

    debug_log("INFO: %s: main_thread_id = %lld",
              __func__,
              g_main_thread_id);

    InitiateEventActivityMonitor();

    while (true) {
        // wait for InitiateEventActivityMonitor() thread to finish initializing:
        while(!g_event_monitor.IsInitialized()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Includes the reset of EventActivityRecorders
        InitiateEventActivityRecorders();

        // Wait for signal
        if (sigwait(&mask, &sig) == 0) {
            HandleSignals(sig);
        }

        log("INFO: %s: joining event activity worker threads",
            __func__);

        // Wait for all threads to finish (this blocks)
        for (auto& recorder : g_event_recorders.GetEventRecorders()) {
            if (recorder->m_event_recorder_thread.joinable()) {
                recorder->m_event_recorder_thread.join();
            }
        }

        if (sig == SIGHUP) {
            CleanUpEventDatFiles();
        }

        if (sig == SIGINT || sig == SIGTERM) {
            log("INFO: %s: joining event id monitor thread",
                __func__);

            g_event_monitor.m_interrupt_monitor = true;
            g_event_monitor.cv_monitor_thread.notify_all();

            if (g_event_monitor.m_event_monitor_thread.joinable()) {
                g_event_monitor.m_event_monitor_thread.join();
            }

            CleanUpEventDatFiles();

            break;
        }
    }

    return g_exit_code;
}
