/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license
 */

#ifndef EVENT_DETECT_H
#define EVENT_DETECT_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <variant>
#include <exception>
#include <string>
#include <filesystem>

#include <util.h>

//!
//! The EventDetect namespace contains EventDetect specific code for the event_detect executable.
//!
namespace EventDetect {

//!
//! \brief The EventDetectException class is a customized exception handling class for the event_detect application.
//!
class EventDetectException : public std::exception
{
public:
    EventDetectException(const std::string& message) : m_message(message) {}
    EventDetectException(const char* message) : m_message(message) {}

    const char* what() const noexcept override {
        return m_message.c_str();
    }

protected:
    std::string m_message;
};

//! File system related exceptions
class FileSystemException : public EventDetectException
{
public:
    FileSystemException(const std::string& message, const std::filesystem::path& path)
        : EventDetectException(message + " Path: " + path.string()), m_path(path) {}

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

//! Threading Related Exceptions
class ThreadException : public EventDetectException
{
public:
    ThreadException(const std::string& message) : EventDetectException(message) {}
};


//!
//! \brief The Monitor class provides the framework for monitoring event activity recorded by the EventRecorders
//! class EventRecorder threads. It also monitors changes in the input event devices and resets the EventRecorder threads
//! if they change. It also updates the m_last_active_time. The class is a singleton and has one instantiated thread.
//! It uses locks to protect the event_monitor device paths and the thread. The m_last_active_time is an atomic and requires
//! no explicit locking.
//!
class Monitor
{
public:
    std::thread m_monitor_thread;
    std::condition_variable cv_monitor_thread;
    std::atomic<bool> m_interrupt_monitor;

    Monitor();

    std::vector<fs::path> GetEventDevices() const;

    void UpdateEventDevices();

    void EventActivityMonitorThread();

    bool IsInitialized() const;

    int64_t GetLastActiveTime() const;

private:
    static std::vector<fs::path> EnumerateEventDevices();

    void WriteLastActiveTimeToFile(const fs::path& filepath);

    mutable std::mutex mtx_event_monitor;
    mutable std::mutex mtx_event_monitor_thread;

    std::vector<fs::path> m_event_device_paths;

    std::atomic<int64_t> m_last_active_time;

    std::atomic<bool> m_initialized;
};

//!
//! \brief The InputEventRecorders class provides the framework for recording event activity from each of the input event devices that
//! are classified as a pointing device (mouse). It is a singleton, but has multiple subordinate threads running, 1 thread for each
//! identified device to monitor.
//!
class InputEventRecorders
{
public:
    std::condition_variable cv_recorder_threads;
    std::atomic<bool> m_interrupt_recorders;

    InputEventRecorders();

    class EventRecorder
    {
    public:
        std::thread m_event_recorder_thread;

        EventRecorder(fs::path event_device_path);

        fs::path GetEventDevicePath() const;

        int64_t GetEventCount() const;

        void EventActivityRecorderThread();

    private:
        mutable std::mutex mtx_event_recorder;

        fs::path m_event_device_path;

        std::atomic<int64_t> m_event_count;
    };

    int64_t GetTotalEventCount() const;

    std::vector<std::shared_ptr<EventRecorder>>& GetEventRecorders();

    void ResetEventRecorders();

private:
    mutable std::mutex mtx_event_recorders;
    mutable std::mutex mtx_event_recorder_threads;

    std::vector<std::shared_ptr<EventRecorder>> m_event_recorder_ptrs;
};

class TtyMonitor
{
public:
    std::thread m_tty_monitor_thread;
    std::condition_variable cv_tty_monitor_thread;
    std::atomic<bool> m_interrupt_tty_monitor;

    TtyMonitor();

    std::vector<fs::path> GetTtyDevices() const;

    void UpdateTtyDevices();

    void TtyMonitorThread();

    bool IsInitialized() const;

    int64_t GetLastTtyActiveTime() const;

    class Tty
    {
    public:
        Tty(const fs::path& tty_device_path);

        fs::path m_tty_device_path;
        int64_t m_tty_last_active_time;
    };

private:
    static std::vector<fs::path> EnumerateTtyDevices();

    mutable std::mutex mtx_tty_monitor;
    mutable std::mutex mtx_tty_monitor_thread;

    std::vector<fs::path> m_tty_device_paths;
    std::vector<Tty> m_ttys;

    std::atomic<int64_t> m_last_ttys_active_time;

    std::atomic<bool> m_initialized;
};

typedef std::variant<bool, int, std::string, fs::path> config_variant;

//!
//! \brief The Config class is a singleton that stores program config read from the config file, with applied defaults if the
//! config file cannot be read, or a config parameter is not in the config file.
//!
class Config
{
public:
    Config();

    void ReadAndUpdateConfig(const fs::path& config_file);

    config_variant GetArg(const std::string& arg);

private:
    void ProcessArgs();

    std::string GetArgString(const std::string& arg, const std::string& default_value) const;

    mutable std::mutex mtx_config;

    std::multimap<std::string, std::string> m_config_in;
    std::multimap<std::string, config_variant> m_config;
};

void Shutdown();

} // namespace event_detect

#endif // EVENT_DETECT_H
