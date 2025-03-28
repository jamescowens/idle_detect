/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
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
    //!
    //! \brief Holds the actual monitor thread.
    //!
    std::thread m_monitor_thread;

    //!
    //! \brief Condition variable for control/synchronization of the monitor thread.
    //!
    std::condition_variable cv_monitor_thread;

    //!
    //! \brief Atomic boolean that interrupts the monitor thread.
    //!
    std::atomic<bool> m_interrupt_monitor;

    //!
    //! \brief Constructor.
    //!
    Monitor();

    //!
    //! \brief Provides a copy of the m_event_device_paths private member. The copy is provided instead of a reference to
    //! minimize the lock time held on mtx_event_monitor.
    //! \return std::vector<fs::path> of the input event device paths.
    //!
    std::vector<fs::path> GetEventDevices() const;

    //!
    //! \brief This calls the private method EnumerateEventDevices() and updates the m_event_device_paths private member.
    //!
    void UpdateEventDevices();

    //!
    //! \brief This is the function that is the entry point for the event activity monitor worker thread.
    //!
    void EventActivityMonitorThread();

    //!
    //! \brief Provides a flag to indicate whether the monitor has been initialized. This is used in main in the application
    //! control paths.
    //! \return boolean flag
    //!
    bool IsInitialized() const;

    //!
    //! \brief Provides the last active time on this machine globally based on the activated monitors/recorders.
    //! \return
    //!
    int64_t GetLastActiveTime() const;

private:
    //!
    //! \brief This is a private method to determine the input event devices to monitor. In particular we only want to monitor
    //! pointing devices (mice).
    //! \return
    //!
    static std::vector<fs::path> EnumerateEventDevices();

    //!
    //! \brief This is the whole point of the application. This writes out the last active time determined by the monitor
    //! \param filepath
    //!
    void WriteLastActiveTimeToFile(const fs::path& filepath);

    //!
    //! \brief This is the mutex member that provides lock control for the event monitor object. This is used to ensure the
    //! event monitor is thread-safe.
    //!
    mutable std::mutex mtx_event_monitor;

    //!
    //! \brief This provides lock control for the monitor worker thread itself.
    //!
    mutable std::mutex mtx_event_monitor_thread;

    //!
    //! \brief Holds the paths of the input event devices to monitor.
    //!
    std::vector<fs::path> m_event_device_paths;

    //!
    //! \brief holds the last active time determined by the monitor. This is an atomic, which means it can be written to/read from
    //! without holding the mtx_event_monitor lock.
    //!
    std::atomic<int64_t> m_last_active_time;

    //!
    //! \brief This holds the flag as to whether the monitor has been initialized and is provided by the IsInitialized() public
    //! method.
    //!
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
    //!
    //! \brief Condition variable for control/synchronization of the recorder threads.
    //!
    std::condition_variable cv_recorder_threads;

    //!
    //! \brief Atomic boolean that interrupts the recorder threads.
    //!
    std::atomic<bool> m_interrupt_recorders;

    //!
    //! \brief Constructor.
    //!
    InputEventRecorders();

    //!
    //! \brief The EventRecorder class formalizes the event recorder instance and is instantiated for each
    //! event recorder thread. These instantiations are wrapped by shared_ptr objects and the shared_ptrs stored
    //! in the m_event_recorder_ptrs vector. This is essentially a very specialized thread pool.
    //!
    class EventRecorder
    {
    public:
        //!
        //! \brief Holds the actual event recorder thread.
        //!
        std::thread m_event_recorder_thread;

        //!
        //! \brief Parameterized constructor.
        //! \param Required parameter for construction: event_device_path
        //!
        explicit EventRecorder(fs::path event_device_path);

        //!
        //! \brief Gets the event device path.
        //! \return A copy of the m_event_device_path. A copy is returned to avoid holding the mtx_event_recorder lock
        //! for an extended period.
        //!
        fs::path GetEventDevicePath() const;

        //!
        //! \brief Returns the current event count tallied by the recorder.
        //! \return
        //!
        int64_t GetEventCount() const;

        //!
        //! \brief Method to run in the instantiated recorder thread.
        //!
        void EventActivityRecorderThread();

    private:
        //!
        //! \brief This is the mutex member that provides lock control for the individual event recorder.
        //!
        mutable std::mutex mtx_event_recorder;

        //!
        //! \brief Holds the event device path of the monitored device.
        //!
        fs::path m_event_device_path;

        //!
        //! \brief Atomic that holds the current event tally for the monitored device.
        //!
        std::atomic<int64_t> m_event_count;
    };

    //!
    //! \brief Provides the total count (tally) of events across all monitored devices.
    //! \return int64_t of total event count
    //!
    int64_t GetTotalEventCount() const;

    //!
    //! \brief Returns a reference to the event recorder objects thread pool.
    //! \return vector of smart shared pointers to the event recorders
    //!
    std::vector<std::shared_ptr<EventRecorder>>& GetEventRecorders();

    void ResetEventRecorders();

private:
    //!
    //! \brief This is the mutex member that provides lock control for the input event recorders object. This is used to
    //! ensure the input event recorders is thread-safe. Note that the subordinate individual event recorders are covered
    //! by their own individual locks.
    //!
    mutable std::mutex mtx_event_recorders;

    //!
    //! \brief This provides lock control for the recorder worker threads themselves.
    //!
    mutable std::mutex mtx_event_recorder_threads;

    //!
    //! \brief Holds smart shared pointers to the event recorder threads. This is a specialized thread pool with associated metadata.
    //!
    std::vector<std::shared_ptr<EventRecorder>> m_event_recorder_ptrs;
};

class TtyMonitor
{
public:
    //!
    //! \brief Holds the actual tty monitor thread.
    //!
    std::thread m_tty_monitor_thread;

    //!
    //! \brief Condition variable for control/synchronization of the tty monitor threads.
    //!
    std::condition_variable cv_tty_monitor_thread;

    //!
    //! \brief Atomic boolean that interrupts the tty monitor thread.
    //!
    std::atomic<bool> m_interrupt_tty_monitor;

    //! Constructor.
    TtyMonitor();

    //!
    //! \brief Returns a copy of the vector of pts/tty paths. A copy is returned to avoid holding the lock on mtx_tty_monitor
    //! for an extended period.
    //! \return std::vector<fs::path> of the pts/tty paths
    //!
    std::vector<fs::path> GetTtyDevices() const;

    //!
    //! \brief Initializes/Updates the pts/tty device paths to monitor.
    //!
    void UpdateTtyDevices();

    //!
    //! \brief Method to instantiate the tty monitor thread.
    //!
    void TtyMonitorThread();

    //!
    //! \brief Provides a flag to indicate whether the monitor has been initialized. This is used in main in the application
    //! control paths.
    //! \return boolean flag
    //!
    bool IsInitialized() const;

    //!
    //! \brief Returns the overall last active time of all of the monitored pts/ttys.
    //! \return Unix Epoch time in seconds
    //!
    int64_t GetLastTtyActiveTime() const;

    //!
    //! \brief The Tty class is a small class to hold pts/tty information. It is essentially a struct with a parameterized
    //! constructor.
    //!
    class Tty
    {
    public:
        //!
        //! \brief Parameterized constructor.
        //! \param tty_device_path
        //!
        explicit Tty(const fs::path& tty_device_path);

        //!
        //! \brief Holds the pts/tty device path.
        //!
        fs::path m_tty_device_path;

        //!
        //! \brief Holds the last active time of the pts/tty.
        //!
        int64_t m_tty_last_active_time;
    };

private:
    //!
    //! \brief Provides the enumerated pts/tty devices.
    //! \return std::vector<fs::path> of enumerated pts/tty devices.
    //!
    static std::vector<fs::path> EnumerateTtyDevices();

    //!
    //! \brief This is the mutex member that provides lock control for the tty monitor object. This is used to ensure the
    //! tty monitor is thread-safe.
    //!
    mutable std::mutex mtx_tty_monitor;

    //!
    //! \brief This provides lock control for the tty monitor worker thread itself.
    //!
    mutable std::mutex mtx_tty_monitor_thread;

    //!
    //! \brief Holds the device paths of the monitored pts/ttys. Note that this is duplicative of information in the individual
    //! tty objects, but it allows efficient comparison of the inventory of ttys.
    //!
    std::vector<fs::path> m_tty_device_paths;

    //!
    //! \brief std::vector holding the tty objects.
    //!
    std::vector<Tty> m_ttys;

    //!
    //! \brief Atomic that holds the overall last active time across all of the monitored pts/ttys.
    //!
    std::atomic<int64_t> m_last_ttys_active_time;

    //!
    //! \brief This holds the flag as to whether the tty monitor has been initialized and is provided by the IsInitialized() public
    //! method.
    //!
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
    //!
    //! \brief Constructor.
    //!
    Config();

    //!
    //! \brief Reads and parses the config file provided by the argument and populates m_config_in, then calls private
    //! method ProcessArgs() to populate m_config.
    //! \param config_file
    //!
    void ReadAndUpdateConfig(const fs::path& config_file);

    //!
    //! \brief Provides the config_variant type value of the config parameter (argument).
    //! \param arg (key) to look up value.
    //! \return config_variant type value of the value of the config parameter (argument).
    //!
    config_variant GetArg(const std::string& arg);

private:
    //!
    //! \brief Private helper method used by ReadAndUpdateConfig
    //!
    void ProcessArgs();

    //!
    //! \brief Private version of GetArg that operates on m_config_in and also selects the provided default value
    //! if the arg is not found. This is how default values for parameters are established.
    //! \param arg (key) to look up value as string.
    //! \param default_value if arg is not found.
    //! \return string value found in lookup, default value if not found.
    //!
    std::string GetArgString(const std::string& arg, const std::string& default_value) const;

    //!
    //! \brief This is the mutex member that provides lock control for the config object. This is used to ensure the
    //! config object is thread-safe.
    //!
    mutable std::mutex mtx_config;

    //!
    //! \brief Holds the raw parsed parameter-values from the config file.
    //!
    std::multimap<std::string, std::string> m_config_in;

    //!
    //! \brief Holds the processed parameter-values, which are strongly typed and in a config_variant union, and where
    //! default values are populated if not found in the config file (m_config_in).
    //!
    std::multimap<std::string, config_variant> m_config;
};

//!
//! \brief Sends the SIGTERM signal to the main thread id initiating a shutdown of all worker threads and the main thread
//! via the HandleSignals function.
//!
void Shutdown();

} // namespace event_detect

#endif // EVENT_DETECT_H
