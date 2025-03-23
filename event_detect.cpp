/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license
 */

//#include <fstream>

#include <event_detect.h>

//std::string event_count_files_path = "/path/to/event_count_files";

pthread_t main_thread_id = 0;

//std::atomic<bool> interrupt_recorders = 0;
//std::atomic<bool> interrupt_monitor = 0;

std::atomic<int64_t> event_count = 0;

// Global event monitor singleton
EventMonitor g_event_monitor;

// Global event recorders singleton
EventRecorders g_event_recorders;

// Class EventMonitor

EventMonitor::EventMonitor()
    : m_interrupt_monitor(false)
    , m_event_device_paths()
{}

EventMonitor::EventMonitor(std::vector<fs::path> event_device_paths)
    : m_event_device_paths(event_device_paths)
{}

void EventMonitor::EventActivityMonitorThread()
{
    debug_log("EventMonitor::EventActivityMonitorThread() called");

    size_t event_devices_size = 0;
    size_t event_devices_size_prev = 0;

    while (true) {
        std::unique_lock<std::mutex> lock(mtx_event_monitor_thread);
        cv_monitor_thread.wait_for(lock, std::chrono::seconds(1), []{ return g_event_monitor.m_interrupt_monitor.load(); });

        if (g_event_monitor.m_interrupt_monitor) {
            break;
        }

        std::stringstream message;

        message << "event_activity_monitor";

        log(message.str());

        event_devices_size_prev = GetEventDevices().size();

        UpdateEventDevices();

        event_devices_size = GetEventDevices().size();

        if (event_devices_size != event_devices_size_prev) {
            log("Input event device count changed. Restarting recorder threads.");
            pthread_kill(main_thread_id, SIGHUP);
        }
    }
}


std::vector<fs::path> EventMonitor::EnumerateEventDevices()
{
    debug_log("EventMonitor::EnumerateEventDevices() called");

    std::vector<fs::path> event_devices;

    fs::path event_device_path = "/sys/class/input";

    std::vector<fs::path> event_device_candidates = FindDirEntriesWithWildcard(event_device_path, "event.*");

    for (const auto& event_device : event_device_candidates) {
        std::stringstream message;

        if (FindDirEntriesWithWildcard(event_device / "device", "mouse.*").empty()) {
            continue;
        }

        event_devices.push_back(event_device);
    }

    if (event_devices.empty()) {
        log("ERROR: No pointing devices identified to monitor. Exiting.");
        exit(1);
    }

    return event_devices;
}

std::vector<fs::path> EventMonitor::GetEventDevices()
{
    std::unique_lock<std::mutex> lock(mtx_event_monitor);

    return m_event_device_paths;
}

void EventMonitor::UpdateEventDevices()
{
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
    std::unique_lock<std::mutex> lock(mtx_event_recorder);

    return m_event_count;
}

void EventRecorders::EventRecorder::EventActivityRecorderThread()
{
    debug_log("EventRecorders::EventRecorder::EventActivityRecorderThread() called");
    debug_log(GetEventDevicePath());

    //std::ofstream fifo(event_count_files_path + "/" + event_id + "_signal.dat");
    //fifo << "none";
    //fifo.close();

    while (true) {
        // Simulate evemu-record

        std::unique_lock<std::mutex> lock(g_event_recorders.mtx_event_recorder_threads);
        g_event_recorders.cv_recorder_threads.wait_for(lock, std::chrono::seconds(1),
                                                       []{ return g_event_recorders.m_interrupt_recorders.load(); });

        if (g_event_recorders.m_interrupt_recorders) {
            break;
        }

        std::stringstream message;

        message << "event_activity_recorder for "
                << GetEventDevicePath();

        log(message.str());
    }
}

void HandleSignals(int signum)
{
    debug_log("HandleSignals() called");

    switch (signum) {
    case SIGINT:
        debug_log("SIGINT received.");
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
        break;
    case SIGTERM:
        debug_log("SIGTERM received");
        g_event_recorders.m_interrupt_recorders = true;
        g_event_recorders.cv_recorder_threads.notify_all();
        break;
    case SIGHUP:
        debug_log("SIGHUP received");
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
    debug_log("InitiateEventActivityRecorders() called");

    g_event_recorders.m_interrupt_recorders = false;

    g_event_recorders.ResetEventRecorders();

    for (auto& recorder : g_event_recorders.GetEventRecorders()) {
        recorder->m_event_recorder_thread = std::thread(&EventRecorders::EventRecorder::EventActivityRecorderThread,
                                                        recorder);
    }

     for (const auto& recorder : g_event_recorders.GetEventRecorders()) {
        std::stringstream message;

        message << "event device path = "
                << recorder->GetEventDevicePath()
                << " thread id = "
                << recorder->m_event_recorder_thread.get_id();

        debug_log(message.str());
    }

    std::cout << std::endl;
}

void InitiateEventActivityMonitor()
{
    debug_log("InitiateEventActivityMonitor() called");

    g_event_monitor.m_interrupt_monitor = false;

    g_event_monitor.m_event_monitor_thread = std::thread(&EventMonitor::EventActivityMonitorThread, std::ref(g_event_monitor));

}

void CleanUpEventDatFiles()
{
    debug_log("CleanUpEventDatFiles() called");

    //for (const std::string& event_id : event_device_id_array) {
    //    std::remove((event_count_files_path + "/" + event_id + "_count.dat").c_str());
    //    std::remove((event_count_files_path + "/" + event_id + "_evemu_pid.dat").c_str());
    //    std::remove((event_count_files_path + "/" + event_id + "_signal.dat").c_str());
    //}
    //std::remove((event_count_files_path + "/last_active_time.dat").c_str());
}

int main()
{
    int sig = 0;

    // Need the main thread id to be able to send signal from monitor thread back to main.
    main_thread_id = pthread_self();

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

    log("event_detect C++ program started");

    //std::ofstream last_active_time_file(event_count_files_path + "/last_active_time.dat");
    //last_active_time_file << "0";
    //last_active_time_file.close();

    InitiateEventActivityMonitor();

    while (true) {
        // Includes the reset of EventActivityRecorders
        InitiateEventActivityRecorders();

        // Wait for signal
        if (sigwait(&mask, &sig) == 0) {
            HandleSignals(sig);
        }

        // Wait for all threads to finish (blocks)
        log("joining event activity worker threads");
        for (auto& recorder : g_event_recorders.GetEventRecorders()) {
            if (recorder->m_event_recorder_thread.joinable()) {
                recorder->m_event_recorder_thread.join();
            }
        }

        if (sig == SIGHUP) {
            CleanUpEventDatFiles();
        }

        if (sig == SIGINT || sig == SIGTERM) {
            log("killing event id monitor thread");
            g_event_monitor.m_interrupt_monitor = true;
            g_event_monitor.cv_monitor_thread.notify_all();

            /*
            if (monitor_thread.second.joinable()) {
                monitor_thread.second.join();
            }
            */

            if (g_event_monitor.m_event_monitor_thread.joinable()) {
                g_event_monitor.m_event_monitor_thread.join();
            }

            CleanUpEventDatFiles();

            break;
        }
    }

    return 0;
}
