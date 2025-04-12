/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <cstring>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <release.h>

#include <event_detect.h>

const fs::path g_lockfile = "event_detect.pid";

pthread_t g_main_thread_id = 0;
std::atomic<int> g_exit_code = 0;

using namespace EventDetect;

//!
//! \brief Global config singleton.
//!
EventDetectConfig g_config;

//!
//! \brief Global event monitor singleton
//!
Monitor g_event_monitor;

//!
//! \brief Global tty monitor singleton
//!
TtyMonitor g_tty_monitor;

//!
//! \brief Global idle detect monitor singleton
//!
IdleDetectMonitor g_idle_detect_monitor;

//!
//! \brief Global event recorders singleton
//!
InputEventRecorders g_event_recorders;

const char* SHM_NAME = "/event_detect_last_active"; // Define name consistently
EventDetect::SharedMemoryTimestampExporter g_shm_exporter(SHM_NAME); // Global instance manages lifecycle via RAII
bool g_shm_initialized_successfully = false; // Flag to track if setup worked

// Class Monitor

Monitor::Monitor()
    : m_interrupt_monitor(false)
    , m_event_device_paths()
    , m_last_active_time(0)
    , m_initialized(false)
{}

void Monitor::EventActivityMonitorThread()
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

        debug_log("INFO: %s: loop: input devices last_active_time = %lld: %s",
                  __func__,
                  m_last_active_time.load(),
                  FormatISO8601DateTime(m_last_active_time.load()));

        // This will be zero if tty monitoring is not active, but that is ok, because the max
        // of the tty monitoring last active and the event monitoring last active is used.
        int64_t tty_last_active_time = g_tty_monitor.GetLastTtyActiveTime();

        debug_log("INFO: %s: loop: ttys last_active_time = %lld: %s",
                  __func__,
                  tty_last_active_time,
                  FormatISO8601DateTime(tty_last_active_time));

        m_last_active_time = std::max(m_last_active_time.load(), tty_last_active_time);

        int64_t last_idle_detect_active_time = g_idle_detect_monitor.GetLastIdleDetectActiveTime();

        debug_log("INFO: %s: loop: idle detect last_active_time = %lld: %s",
                  __func__,
                  last_idle_detect_active_time,
                  FormatISO8601DateTime(last_idle_detect_active_time));

        m_last_active_time = std::max(m_last_active_time.load(), last_idle_detect_active_time);

        debug_log("INFO: %s: loop: overall last_active time = %lld: %s",
                  __func__,
                  m_last_active_time.load(),
                  FormatISO8601DateTime(m_last_active_time.load()));

        bool write_last_active_time_to_file = std::get<bool>(g_config.GetArg("write_last_active_time_to_file"));

        if (write_last_active_time_to_file) {
            fs::path event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));
            std::string last_active_time_cpp_filename = std::get<std::string>(g_config.GetArg("last_active_time_cpp_filename"));

            fs::path last_active_time_filepath = event_data_path / last_active_time_cpp_filename;

            WriteLastActiveTimeToFile(last_active_time_filepath);
        }

        // --- In Monitor::EventActivityMonitorThread loop ---
        if (g_shm_initialized_successfully) { // Check if setup in main worked
            if (!g_shm_exporter.UpdateTimestamp(m_last_active_time.load())) {
                error_log("%s: Failed to update shared memory timestamp.", __func__);
            }
        }
    }
}

bool Monitor::IsInitialized() const
{
    return m_initialized.load();
}

int64_t Monitor::GetLastActiveTime() const
{
    return m_last_active_time.load();
}

std::vector<fs::path> Monitor::EnumerateEventDevices()
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
        Shutdown();
    }

    debug_log("INFO: %s: event_devices.size() = %u",
              __func__,
              event_devices.size());

    return event_devices;
}

void Monitor::WriteLastActiveTimeToFile(const fs::path& last_active_time_filepath)
{
    // Open the file for writing (overwrites)
    std::ofstream output_file(last_active_time_filepath);

    if (!output_file.is_open()) {
        error_log("%s: Could not open file %s for writing.",
                  __func__,
                  last_active_time_filepath);

        g_exit_code = 1;
        Shutdown();;
    }

    output_file << m_last_active_time << std::endl;

    if (!output_file.good()) {
        error_log("%s: Error writing to file %s.",
                  __func__,
                  last_active_time_filepath);
        output_file.close();

        g_exit_code = 1;
        Shutdown();;
    }
}

std::vector<fs::path> Monitor::GetEventDevices() const
{
    std::unique_lock<std::mutex> lock(mtx_event_monitor);

    return m_event_device_paths;
}

void Monitor::UpdateEventDevices()
{
    debug_log("INFO: %s: started",
              __func__);

    std::unique_lock<std::mutex> lock(mtx_event_monitor);

    m_event_device_paths = EnumerateEventDevices();
}


// Class InputEventRecorders

InputEventRecorders::InputEventRecorders()
    : m_interrupt_recorders(false)
    , m_event_recorder_ptrs()
{}

std::vector<std::shared_ptr<InputEventRecorders::EventRecorder>>& InputEventRecorders::GetEventRecorders()
{
    std::unique_lock<std::mutex> lock(mtx_event_recorders);

    return m_event_recorder_ptrs;
}

void InputEventRecorders::ResetEventRecorders()
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
int64_t InputEventRecorders::GetTotalEventCount() const
{
    int64_t tally = 0;

    for (auto& event_recorder : m_event_recorder_ptrs) {
        tally += event_recorder->GetEventCount();
    }

    return tally;
}


// Class InputEventRecorders::EventRecorder

InputEventRecorders::EventRecorder::EventRecorder(fs::path event_device_path)
    : m_event_device_path(event_device_path)
    , m_event_count(0)
{}

//!
//! \brief Returns a read only fs::path of the input event device path associated with the recorder. No locking is
//! needed as this can only be changed on construction.
//!
//! \return fs::path
//!
fs::path InputEventRecorders::EventRecorder::GetEventDevicePath() const
{
    return m_event_device_path;
}

int64_t InputEventRecorders::EventRecorder::GetEventCount() const
{
    // No explicit lock needed.

    return m_event_count.load();
}

void InputEventRecorders::EventRecorder::EventActivityRecorderThread()
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
    }

    int rc;

    // Initialize libevdev
    if (g_exit_code == 0)
    {
        rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            error_log("%s: Failed to init libevdev for device %s: %s",
                      __func__,
                      device_access_path,
                      strerror(-rc));

            close(fd);
            g_exit_code = 1;
        }

        debug_log("INFO: %s: Device: %s, Path: %s, Physical Path: %s, Unique: %s",
                  __func__,
                  libevdev_get_name(dev),
                  device_access_path,
                  libevdev_get_phys(dev),
                  libevdev_get_uniq(dev));
    }

    struct input_event ev;

    while (g_exit_code == 0) {
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


// Class TtyMonitor

TtyMonitor::TtyMonitor()
    : m_interrupt_tty_monitor(false)
    , m_tty_device_paths()
    , m_ttys()
    , m_last_ttys_active_time(0)
    , m_initialized(false)
{}

std::vector<fs::path> TtyMonitor::GetTtyDevices() const
{
    std::unique_lock<std::mutex> lock(mtx_tty_monitor);

    return m_tty_device_paths;
}

void TtyMonitor::UpdateTtyDevices()
{
    debug_log("INFO: %s: started",
              __func__);

    std::vector<fs::path> tty_device_paths = EnumerateTtyDevices();

    if (tty_device_paths != m_tty_device_paths) {
        m_initialized = false;
        m_tty_device_paths = tty_device_paths;
    } else {
        m_initialized = true;
    }

}

bool TtyMonitor::IsInitialized() const
{
    return m_initialized.load();
}

int64_t TtyMonitor::GetLastTtyActiveTime() const
{
    return m_last_ttys_active_time.load();
}


std::vector<fs::path> TtyMonitor::EnumerateTtyDevices()
{
    debug_log("INFO: %s: started",
              __func__);

    std::vector<fs::path> ptss = FindDirEntriesWithWildcard(fs::path {"/dev/pts"}, ".*");

    debug_log("INFO: %s: ptss.size() = %u",
              __func__,
              ptss.size());

    std::vector<fs::path> ttys = FindDirEntriesWithWildcard(fs::path {"/dev"}, "tty.*");

    debug_log("INFO: %s: ttys.size() = %u",
              __func__,
              ttys.size());

    ptss.insert(ptss.end(), ttys.begin(), ttys.end());

    debug_log("INFO: %s: total terminal sessions to monitor = %u",
              __func__,
              ptss.size());

    if (ptss.empty()) {
        error_log("%s: No ptys/ttys identified to monitor.",
                  __func__);
    }

    return ptss;
}

void TtyMonitor::TtyMonitorThread()
{
    std::vector<fs::path> tty_device_paths_prev;

    // Critical section block
    {
        std::unique_lock<std::mutex> lock(mtx_tty_monitor);

        tty_device_paths_prev = m_tty_device_paths;
    }

    int64_t last_ttys_active_time = 0;

    while (true) {
        debug_log("INFO: %s: tty monitor thread loop at top of iteration",
                  __func__);

        std::unique_lock<std::mutex> lock(mtx_tty_monitor_thread);
        cv_tty_monitor_thread.wait_for(lock, std::chrono::seconds(1), []{ return g_tty_monitor.m_interrupt_tty_monitor.load(); });

        if (g_tty_monitor.m_interrupt_tty_monitor) {
            break;
        }

        // Critical section block
        {
            std::unique_lock<std::mutex> lock(mtx_tty_monitor);

            UpdateTtyDevices();

            if (!IsInitialized()) {
                m_ttys.clear();

                for (const auto& entry : m_tty_device_paths) {
                    Tty tty(entry);

                    m_ttys.push_back(tty);
                }

                m_initialized = true;
            }

            for (auto& entry : m_ttys) {
                struct stat sbuf;

                if (stat(entry.m_tty_device_path.c_str(), &sbuf) == 0){
                    entry.m_tty_last_active_time = sbuf.st_atime;
                }

                // last_tty_active_time MUST be monotonic. It cannot go backwards. This is important, because terminals sometimes
                // disappear.
                last_ttys_active_time = std::max(entry.m_tty_last_active_time, last_ttys_active_time);
            }
        }

        m_last_ttys_active_time = last_ttys_active_time;
    }
}

TtyMonitor::Tty::Tty(const fs::path& tty_device_path)
    : m_tty_device_path(tty_device_path)
    , m_tty_last_active_time(0)
{}


// IdleDetectMonitor class

IdleDetectMonitor::IdleDetectMonitor()
    : m_interrupt_idle_detect_monitor(false)
    , m_last_idle_detect_active_time(0)
{}

void IdleDetectMonitor::IdleDetectMonitorThread()
{
    debug_log("INFO: %s: started.",
              __func__);

    fs::path event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));
    fs::path pipe_path = event_data_path / "event_registration_pipe";

    // Create the named pipe if it doesn't exist (idempotent)
    if (mkfifo(pipe_path.c_str(), 0666) == -1) {
        if (errno != EEXIST) {
            error_log("%s: Error creating named pipe: %s",
                      __func__,
                      strerror(errno));
            Shutdown(1);
            return;
        }
    }

    // Note the mode above combined with the umask does not result in the right permissions,
    // so we override with the correct ones.

    fs::perms desired_perms = fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read | fs::perms::group_write |
                              fs::perms::others_write;

    std::error_code ec; // To capture potential errors

    // Set the permissions, replacing existing ones (default behavior)
    fs::permissions(pipe_path, desired_perms, fs::perm_options::replace, ec);

    if (ec) {
        // Handle the error if permissions couldn't be set
        error_log("%s: Error setting permissions (0666) on named pipe %s: %s",
                  __func__,
                  pipe_path.string().c_str(),
                  ec.message().c_str()); // Use the error_code's message
        Shutdown(1);
        return;
    } else {
        debug_log("%s: Successfully set permissions on %s to 0666.",
                  __func__, pipe_path.string().c_str());
    }

    int fd = -1;
    char buffer[256];
    ssize_t bytes_read;
    const int poll_timeout_ms = 100;

    m_initialized = true;

    int64_t last_idle_detect_active_time = 0;

    while (g_exit_code == 0) {
        debug_log("INFO: %s: idle_detect monitor thread loop at top of iteration",
                  __func__);

        std::unique_lock<std::mutex> lock(mtx_idle_detect_monitor_thread);
        cv_idle_detect_monitor_thread.wait_for(lock, std::chrono::milliseconds(100),
                                               []{ return g_idle_detect_monitor.m_interrupt_idle_detect_monitor.load(); });

        if (g_idle_detect_monitor.m_interrupt_idle_detect_monitor) {
            break;
        }

        // Only execute this if the pipe isn't already open in non-blocking mode.
        if (fd == -1) {
            fd = open(pipe_path.c_str(), O_RDONLY | O_NONBLOCK);
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
                                    last_idle_detect_active_time = event.m_timestamp;

                                    debug_log("INFO: %s: Valid activity event received with timestamp %lld",
                                              __func__,
                                              last_idle_detect_active_time);

                                    // last_idle_detect_active_time MUST be monotonic. It cannot go backwards.
                                    m_last_idle_detect_active_time = std::max(m_last_idle_detect_active_time.load(),
                                                                            last_idle_detect_active_time);

                                    debug_log("INFO: %s: Current idle detect monitor last active time %lld",
                                              __func__,
                                              m_last_idle_detect_active_time);
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

bool IdleDetectMonitor::IsInitialized() const
{
    return m_initialized.load();
}

int64_t IdleDetectMonitor::GetLastIdleDetectActiveTime() const
{
    return m_last_idle_detect_active_time.load();
}

// EventDetectConfig class

void EventDetectConfig::ProcessArgs()
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

    // startup_delay

    int startup_delay = 0;

    try {
        startup_delay = ParseStringToInt(GetArgString("startup_delay", "0"));
    } catch (std::exception& e) {
        error_log("%s: startup_delay parameter in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    m_config.insert(std::make_pair("startup_delay", startup_delay));

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

    // write_last_active_time_to_file

    bool last_active_time_to_file = false;

    try {
        last_active_time_to_file = ParseStringToInt(GetArgString("write_last_active_time_to_file", "0"));
    } catch (std::exception& e) {
        error_log("%s: write_last_active_time_to_file in config file has invalid value: %s",
                  __func__,
                  e.what());
    }

    m_config.insert(std::make_pair("write_last_active_time_to_file", last_active_time_to_file));


    // last_active_time_cpp_filename

    std::string last_active_time_cpp_filename = GetArgString("last_active_time_cpp_filename", "last_active_time.dat");

    m_config.insert(std::make_pair("last_active_time_cpp_filename", last_active_time_cpp_filename));

    // monitor_ttys

    std::string monitor_ttys_arg = GetArgString("monitor_ttys", "true");

    if (monitor_ttys_arg == "1" || ToLower(monitor_ttys_arg) == "true") {
        m_config.insert(std::make_pair("monitor_ttys", true));
    } else if (monitor_ttys_arg == "0" || ToLower(monitor_ttys_arg) == "false"){
        m_config.insert(std::make_pair("monitor_ttys", false));
    } else {
        error_log("%s: monitor_ttys parameter in config file has invalid value: %s",
                  __func__,
                  debug_arg);
    }

    // monitor_idle_detect_events

    std::string monitor_idle_detect_events_arg = GetArgString("monitor_idle_detect_events", "false");

    if (monitor_idle_detect_events_arg == "1" || ToLower(monitor_idle_detect_events_arg) == "true") {
        m_config.insert(std::make_pair("monitor_idle_detect_events", true));
    } else if (monitor_idle_detect_events_arg == "0" || ToLower(monitor_idle_detect_events_arg) == "false"){
        m_config.insert(std::make_pair("monitor_idle_detect_events", false));
    } else {
        error_log("%s: monitor_idle_detect_events parameter in config file has invalid value: %s",
                  __func__,
                  debug_arg);
    }

    // use_shared_memory

    bool use_shm = true;

    std::string use_shm_arg = GetArgString("use_shared_memory", "true");

    if (use_shm_arg == "1" || ToLower(use_shm_arg) == "true") {
        use_shm = true;
    } else if (use_shm_arg == "0" || ToLower(use_shm_arg) == "false") {
        use_shm = false;
    } else {
        error_log("%s: use_shared_memory parameter has invalid value: %s; defaulting to true.", __func__, use_shm_arg);
        use_shm = true;
    }

    m_config.insert(std::make_pair("use_shared_memory", use_shm));
}


// SharedMemoryTimestampExporter class

SharedMemoryTimestampExporter::SharedMemoryTimestampExporter(const std::string& name) :
    m_shm_name(name),
    m_shm_fd(-1),
    m_mapped_ptr(nullptr),
    m_size(sizeof(int64_t)),
    m_is_creator(false), // Assume not creator initially
    m_is_initialized(false)
{
    if (m_shm_name.empty() || m_shm_name[0] != '/') {
        // Log error or throw? Let's log for now.
        error_log("ERROR: %s: Shared memory name '%s' must be non-empty and start with '/'", __func__, m_shm_name.c_str());
        // Mark as invalid? State is already !m_is_initialized.
    }
}

SharedMemoryTimestampExporter::~SharedMemoryTimestampExporter() {
    Cleanup(); // RAII guarantees cleanup
}

bool SharedMemoryTimestampExporter::CreateOrOpen(mode_t mode) {
    if (m_is_initialized.load()) {
        debug_log("INFO: %s: Shared memory %s already initialized.", __func__, m_shm_name.c_str());
        return true;
    }
    debug_log("INFO: %s: Initializing shared memory segment %s", __func__, m_shm_name.c_str());

    Cleanup(); // Ensure clean state if called again after failure
    m_is_creator = false; // Reset ownership flag

    // 1. Create or open RW
    errno = 0;
    m_shm_fd = shm_open(m_shm_name.c_str(), O_CREAT | O_RDWR, mode);
    if (m_shm_fd == -1) {
        error_log("ERROR: %s: shm_open failed for %s: %s", __func__, m_shm_name.c_str(), strerror(errno));
        return false;
    }

    // 2. Check current size, truncate if necessary
    struct stat shm_stat;
    if (fstat(m_shm_fd, &shm_stat) == -1) {
        error_log("ERROR: %s: fstat failed for shm fd %d: %s", __func__, m_shm_fd, strerror(errno));
        close(m_shm_fd); m_shm_fd = -1;
        // Don't unlink here, might not have been created by us or error is permissions.
        return false;
    }

    if (shm_stat.st_size != (off_t)m_size) {
        debug_log("INFO: %s: Shm %s has size %ld, resizing to %zu bytes.", __func__, m_shm_name.c_str(), (long)shm_stat.st_size, m_size);
        m_is_creator = true; // We are responsible for setting the size
        errno = 0;
        if (ftruncate(m_shm_fd, m_size) == -1) {
            error_log("ERROR: %s: ftruncate failed for shm %s: %s", __func__, m_shm_name.c_str(), strerror(errno));
            close(m_shm_fd); m_shm_fd = -1;
            shm_unlink(m_shm_name.c_str()); // Clean up object we tried to create/resize
            return false;
        }
    } else {
        debug_log("INFO: %s: Shm %s exists with correct size.", __func__, m_shm_name.c_str());
        // Cannot reliably determine ownership if just opening existing, assume not creator.
        m_is_creator = false;
    }

    // 3. Map memory RW
    errno = 0;
    void* mapped_mem = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);

    // 4. Close FD immediately after mmap (no longer needed)
    close(m_shm_fd);
    m_shm_fd = -1;

    if (mapped_mem == MAP_FAILED) {
        error_log("ERROR: %s: mmap failed for shm %s: %s", __func__, m_shm_name.c_str(), strerror(errno));
        // If we created it, unlink it. If not, maybe leave it? Unlink might fail if permissions wrong.
        if (m_is_creator) shm_unlink(m_shm_name.c_str());
        return false;
    }

    // 5. Store pointer, initialize if we created it
    m_mapped_ptr = static_cast<std::atomic<int64_t>*>(mapped_mem);
    if (m_is_creator) {
        int64_t initial_time = GetUnixEpochTime(); // Assumes GetUnixEpochTime() is available
        m_mapped_ptr->store(initial_time, std::memory_order_relaxed);
        debug_log("INFO: %s: Shared memory segment %s initialized to %lld.", __func__, m_shm_name.c_str(), (long long)initial_time);
    } else {
        debug_log("INFO: %s: Shared memory segment %s mapped.", __func__, m_shm_name.c_str());
    }

    m_is_initialized.store(true);
    return true;
}

void SharedMemoryTimestampExporter::Cleanup() {
    if (!m_is_initialized.load() && m_mapped_ptr == nullptr && m_shm_fd == -1) {
        return; // Already clean or never initialized
    }
    debug_log("DEBUG: %s: Cleaning up shared memory %s...", __func__, m_shm_name.c_str());

    // 1. Unmap memory
    if (m_mapped_ptr != nullptr) {
        if (munmap(m_mapped_ptr, m_size) == -1) {
            error_log("ERROR: %s: munmap failed for %s: %s", __func__, m_shm_name.c_str(), strerror(errno));
        } else {
            debug_log("DEBUG: %s: Shared memory %s unmapped.", __func__, m_shm_name.c_str());
        }
        m_mapped_ptr = nullptr;
    }

    // 2. Close FD if it's somehow still open
    if (m_shm_fd != -1) {
        debug_log("WARNING: %s: Shared memory FD %d was open during cleanup, closing.", __func__, m_shm_fd);
        close(m_shm_fd);
        m_shm_fd = -1;
    }

    // 3. Unlink the name IF we think we were the creator/resizer.
    // This is imperfect (creator might crash before resize, opener might resize if size is wrong).
    // A more robust strategy involves lock files or always unlinking on clean shutdown,
    // but requires careful coordination if multiple processes might map it.
    // Let's stick with unlinking only if we set the size.
    // **Alternative:** Always unlink on clean shutdown from the main service.
    // Let's adopt the "always unlink on clean shutdown" via destructor.
    debug_log("INFO: %s: Requesting unlink for shared memory %s (will succeed only if no other refs).", __func__, m_shm_name.c_str());
    if (shm_unlink(m_shm_name.c_str()) == -1) {
        if (errno != ENOENT) { // Ignore "No such file or directory"
            error_log("ERROR: %s: shm_unlink failed for %s: %s", __func__, m_shm_name.c_str(), strerror(errno));
        }
    } else {
        debug_log("INFO: %s: Shared memory segment %s unlinked successfully.", __func__, m_shm_name.c_str());
    }


    m_is_initialized.store(false);
}

bool SharedMemoryTimestampExporter::UpdateTimestamp(int64_t timestamp) {
    if (!m_is_initialized.load() || m_mapped_ptr == nullptr) {
        // error_log("ERROR: %s: Cannot update timestamp, shared memory not initialized.", __func__); // Can be noisy
        return false;
    }
    // relaxed is likely fine if the only synchronization needed is the atomic write itself
    m_mapped_ptr->store(timestamp, std::memory_order_relaxed);
    return true;
}

bool SharedMemoryTimestampExporter::IsInitialized() const {
    return m_is_initialized.load();
}


void EventDetect::Shutdown(const int& exit_code)
{
    g_exit_code = exit_code;

    pthread_kill(g_main_thread_id, SIGTERM);
}


// Global scope functions. Note these do not have declarations in the event_detect.h file.

//!
//! \brief Signal handler for threads.
//! \param signum
//!
void HandleSignals(int signum)
{
    debug_log("INFO: %s: started",
              __func__);

    switch (signum) {
    case SIGINT:
        debug_log("INFO: %s: SIGINT received",
                  __func__);
        break;
    case SIGTERM:
        debug_log("INFO: %s: SIGTERM received",
                  __func__);
        break;
    case SIGHUP:
        debug_log("INFO: %s: SIGHUP received",
                  __func__);
        break;
    default:
        log("WARNING: Unknown signal received.");
        break;
    }

    if (signum == SIGHUP || signum == SIGINT || signum == SIGTERM) {
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
    }

    if (signum == SIGINT || signum == SIGTERM) {
        g_idle_detect_monitor.m_interrupt_idle_detect_monitor = true;
        g_idle_detect_monitor.cv_idle_detect_monitor_thread.notify_all();

        g_tty_monitor.m_interrupt_tty_monitor = true;
        g_tty_monitor.cv_tty_monitor_thread.notify_all();

        g_event_monitor.m_interrupt_monitor = true;
        g_event_monitor.cv_monitor_thread.notify_all();
    }
}

//!
//! \brief Initiates the event activity recorder threads.
//!
void InitiateEventActivityRecorders()
{
    debug_log("INFO: %s: started",
              __func__);

    g_event_recorders.m_interrupt_recorders = false;

    g_event_recorders.ResetEventRecorders();

    for (auto& recorder : g_event_recorders.GetEventRecorders()) {
            recorder->m_event_recorder_thread = std::thread(&InputEventRecorders::EventRecorder::EventActivityRecorderThread,
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

//!
//! \brief Initiates the event activity monitor thread. Note this also checks the output of the tty monitor, so it is really
//! the overall monitor.
//!
void InitiateEventActivityMonitor()
{
    debug_log("INFO: %s: started",
              __func__);

    g_event_monitor.m_interrupt_monitor = false;

    g_event_monitor.m_monitor_thread = std::thread(&Monitor::EventActivityMonitorThread, std::ref(g_event_monitor));
}

//!
//! \brief Initiates the tty monitor thread.
//!
void InitiateTtyMonitor()
{
    debug_log("INFO: %s: started",
              __func__);

    g_tty_monitor.m_interrupt_tty_monitor = false;

    g_tty_monitor.m_tty_monitor_thread = std::thread(&TtyMonitor::TtyMonitorThread, std::ref(g_tty_monitor));
}

void InitiateIdleDetectMonitor()
{
    debug_log("INFO: %s: started.",
              __func__);

    g_idle_detect_monitor.m_interrupt_idle_detect_monitor = false;

    g_idle_detect_monitor.m_idle_detect_monitor_thread = std::thread(&IdleDetectMonitor::IdleDetectMonitorThread,
                                                                     std::ref(g_idle_detect_monitor));
}

//!
//! \brief Create event data directory if needed and set proper permissions.
//! \param data_dir_path
//!
void SetupDataDir(const fs::path& data_dir_path)
{
    try {
        if (!fs::exists(data_dir_path)) {
            fs::create_directory(data_dir_path);
        }

        fs::permissions(data_dir_path,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                            fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace);
    } catch (FileSystemException& e) {
        error_log("%: Unable to create and/or set permissions on event_detect data directory at path: %s",
                  __func__,
                  data_dir_path);
        g_exit_code = 1;
        Shutdown();
    }
}

//!
//! \brief Cleans up data files in the event_count_files_path, including the application lockfile, during a graceful shutdown.
//! \param sig
//!
void CleanUpFiles(int sig)
{
    debug_log("INFO: %s: started",
              __func__);

    fs::path event_data_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));

    std::vector<fs::path> files_to_clean_up = FindDirEntriesWithWildcard(event_data_path, "^event.*\\.dat$");

    for (const auto& file : files_to_clean_up) {
        try {
            fs::remove(file);
        } catch (FileSystemException& e) {
            log("WARNING: %s: event data file could not be removed: %s",
                __func__,
                e.what());
        }
    }

    std::string last_active_time_cpp_filename = std::get<std::string>(g_config.GetArg("last_active_time_cpp_filename"));

    fs::path last_active_time_path = event_data_path / last_active_time_cpp_filename;

    if ((sig == SIGINT || sig == SIGTERM) && fs::exists(last_active_time_path)) {
        try {
            fs::remove(last_active_time_path);
        } catch (FileSystemException& e) {
            log("WARNING: %s: last_active_time file could not be removed: %s",
                __func__,
                e.what());
        }
    }

    fs::path pipe_path = event_data_path / "event_registration_pipe";

    if ((sig == SIGINT || sig == SIGTERM) && fs::exists(pipe_path)) {
        try {
            fs::remove(pipe_path);
        } catch (FileSystemException& e) {
            log("WARNING: %s: last_active_time file could not be removed: %s",
                __func__,
                e.what());
        }
    }

    if ((sig == SIGINT || sig == SIGTERM) && fs::exists(event_data_path / g_lockfile)) {
        try {
            fs::remove(event_data_path / g_lockfile);
        } catch (FileSystemException& e) {
            error_log("%s: application lockfile unable to be removed at application termination or interrupt.");

            g_exit_code = 1;
        }
    }
}

//!
//! \brief This is the main function for event_detect
//! \param argc
//! \param argv
//! \return exit code
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
    } else {
        log("WARNING: %s: Argument invalid for config file. Using defaults.",
            __func__);

        config_file_path = "";
    }

    // Read the config file for config. If an error is encountered reading the config file, then defaults will be used.
    g_config.ReadAndUpdateConfig(config_file_path);

    // Populate g_debug from the config to avoid having to call the heavyweight GetArg in each log function call.
    g_debug = std::get<bool>(g_config.GetArg("debug"));

    pid_t current_pid = getpid();

    fs::path data_dir_path = std::get<fs::path>(g_config.GetArg("event_count_files_path"));

    SetupDataDir(data_dir_path);

    // Lockfile management. There should only be one instance of this application running.
    // Check if the lockfile exists. If it cannot be removed indicate event_detect is already running and exit with failure.
    if (g_exit_code == 0 && fs::exists(data_dir_path / g_lockfile)) {
        std::ifstream lockfile(data_dir_path / g_lockfile);
        pid_t old_pid;
        lockfile >> old_pid;

        if (kill(old_pid, 0) == 0) {
            error_log("%s: event_detect is already running with PID: %s",
                      __func__,
                      old_pid);

            g_exit_code = 1;

            return g_exit_code;
        }
    }

    // Scope to close the lockfile after writing pid.
    {
        // Create or overwrite the lockfile
        std::ofstream lockfile(data_dir_path / g_lockfile);
        lockfile << current_pid;
    }

    int startup_delay = std::get<int>(g_config.GetArg("startup_delay"));

    if (startup_delay > 0) {
        log("INFO: %s: Waiting for %u seconds to start.",
            __func__,
            startup_delay);

        std::this_thread::sleep_for(std::chrono::seconds(startup_delay));
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

    log("INFO: %s: event_detect C++ program, %s, started, pid %i",
        __func__,
        g_version,
        current_pid);

    debug_log("INFO: %s: main_thread_id = %lld",
              __func__,
              g_main_thread_id);

    // --- shmem setupg ---
    bool use_shared_memory = false; // Local variable for main scope
    try {
        use_shared_memory = std::get<bool>(g_config.GetArg("use_shared_memory"));
    } catch (...) { use_shared_memory = true; /* Default or log error */ }

    if (use_shared_memory) {
        // CreateOrOpen attempts shm_open, ftruncate, mmap
        if (g_shm_exporter.CreateOrOpen(0664)) {
            log("INFO: %s: Shared memory exporter initialized successfully.", __func__);
            g_shm_initialized_successfully = true; // Set flag for Monitor thread
        } else {
            error_log("ERROR: %s: Failed to initialize shared memory exporter. Shared memory export disabled.", __func__);
            g_shm_initialized_successfully = false;
            // Maybe exit if shm is critical? Or just continue without it?
        }
    } else {
        log("INFO: %s: Shared memory export disabled by configuration.", __func__);
        g_shm_initialized_successfully = false;
    }

    try {
        InitiateEventActivityMonitor();
    } catch (ThreadException& e) {
        error_log("%s: Error creating event monitor thread: %s",
                  __func__,
                  e.what());

        Shutdown(1);
    }

    while (true) {
        // wait for up to 10 seconds for InitiateEventActivityMonitor() thread to finish initializing:
        debug_log("INFO: %s: Waiting for monitor thread to finish initializing.",
                  __func__);

        for (int i = 0; g_exit_code == 0 && !g_event_monitor.IsInitialized() && i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!g_event_monitor.IsInitialized()) {
            error_log("%s: Unable to initialize event monitor thread. Exiting.",
                      __func__);

            Shutdown(1);
        }

        // Includes the reset of EventActivityRecorders
        if (g_exit_code == 0) {
            try {
                InitiateEventActivityRecorders();
            } catch (ThreadException& e) {
                error_log("%s: Error creating event monitor thread: %s",
                          __func__,
                          e.what());

                Shutdown(1);
            }
        }

        // If the tty_monitor is already initialized, then don't try to do this again.
        if (g_exit_code == 0
            && std::get<bool>(g_config.GetArg("monitor_ttys")) == true
            && !g_tty_monitor.IsInitialized()) {
            try {
                InitiateTtyMonitor();
            } catch (ThreadException& e) {
                error_log("%s: Error creating event monitor thread: %s",
                          __func__,
                          e.what());

                Shutdown(1);
            }
        }

        if (g_exit_code == 0
            && std::get<bool>(g_config.GetArg("monitor_idle_detect_events")) == true
            && !g_idle_detect_monitor.IsInitialized()) {
            try {
                InitiateIdleDetectMonitor();
            } catch (ThreadException& e) {
                error_log("%s: Error creating event monitor thread: %s",
                          __func__,
                          e.what());

                Shutdown(1);
            }
        }

        // Wait for signal. This will also cause a shutdown at this point if Shutdown() was/is called.
        if (sigwait(&mask, &sig) == 0) {
            HandleSignals(sig);
        }

        log("INFO: %s: joining event activity worker threads",
            __func__);

        // Wait for all recorder threads to finish (this blocks)
        for (auto& recorder : g_event_recorders.GetEventRecorders()) {
            if (recorder->m_event_recorder_thread.joinable()) {
                recorder->m_event_recorder_thread.join();
            }
        }

        if (sig == SIGHUP) {
            CleanUpFiles(sig);
        }

        if (sig == SIGINT || sig == SIGTERM) {
            log("INFO: %s: joining monitor threads",
                __func__);

            if (g_idle_detect_monitor.m_idle_detect_monitor_thread.joinable()) {
                g_idle_detect_monitor.m_idle_detect_monitor_thread.join();
            }

            if (g_tty_monitor.m_tty_monitor_thread.joinable()) {
                g_tty_monitor.m_tty_monitor_thread.join();
            }

            if (g_event_monitor.m_monitor_thread.joinable()) {
                g_event_monitor.m_monitor_thread.join();
            }

            // --- In main() shutdown sequence ---
            // NO explicit cleanup needed here anymore for g_shm_exporter!
            // Its destructor will be called automatically when main exits,
            // which calls Cleanup() -> munmap() / shm_unlink().

            CleanUpFiles(sig);

            break;
        }
    }

    return g_exit_code;
}
