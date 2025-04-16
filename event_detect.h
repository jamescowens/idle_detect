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
#include <thread>
#include <filesystem>

#include <util.h>

namespace EventDetect {

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

class IdleDetectMonitor
{
public:
    //!
    //! \brief Holds the actual idle_detect monitor thread.
    //!
    std::thread m_idle_detect_monitor_thread;

    //!
    //! \brief Condition variable for control/synchronization of the idle_detect monitor threads.
    //!
    std::condition_variable cv_idle_detect_monitor_thread;

    //!
    //! \brief Atomic boolean that interrupts the idle_detect monitor thread.
    //!
    std::atomic<bool> m_interrupt_idle_detect_monitor;

    //! Constructor.
    IdleDetectMonitor();

    //!
    //! \brief Method to instantiate the tty monitor thread.
    //!
    void IdleDetectMonitorThread();

    //!
    //! \brief Provides a flag to indicate whether the monitor has been initialized. This is used in main in the application
    //! control paths.
    //! \return boolean flag
    //!
    bool IsInitialized() const;

    //!
    //! \brief Returns the overall last active time of all of the message receipts from idle_detect instances.
    //! \return Unix Epoch time in seconds
    //!
    int64_t GetLastIdleDetectActiveTime() const;

private:
    //!
    //! \brief This is the mutex member that provides lock control for the tty monitor object. This is used to ensure the
    //! tty monitor is thread-safe.
    //!
    mutable std::mutex mtx_idle_detect_monitor;

    //!
    //! \brief This provides lock control for the tty monitor worker thread itself.
    //!
    mutable std::mutex mtx_idle_detect_monitor_thread;

    //!
    //! \brief Atomic that holds the overall last active time across all of the monitored pts/ttys.
    //!
    std::atomic<int64_t> m_last_idle_detect_active_time;

    //!
    //! \brief This holds the flag as to whether the tty monitor has been initialized and is provided by the IsInitialized() public
    //! method.
    //!
    std::atomic<bool> m_initialized;
};

/**
 * @brief Manages a POSIX shared memory segment for exporting timestamps.
 * Stores an array of two int64_t: {update_time, last_active_time}.
 * Handles creation, mapping, updating, and cleanup via RAII.
 */
class SharedMemoryTimestampExporter {
public:
    /**
     * @brief Construct with the desired shared memory name.
     * @param name Must start with '/' (e.g., "/idle_detect_shmem").
     */
    explicit SharedMemoryTimestampExporter(const std::string& name);

    /**
     * @brief Destructor handles unmapping and potentially unlinking the shared memory.
     */
    ~SharedMemoryTimestampExporter();

    // Prevent copying/moving
    SharedMemoryTimestampExporter(const SharedMemoryTimestampExporter&) = delete;
    SharedMemoryTimestampExporter& operator=(const SharedMemoryTimestampExporter&) = delete;
    SharedMemoryTimestampExporter(SharedMemoryTimestampExporter&&) = delete;
    SharedMemoryTimestampExporter& operator=(SharedMemoryTimestampExporter&&) = delete;

    /**
     * @brief Creates (if necessary) and opens the shared memory segment,
     * sets its size (to sizeof(int64_t[2])), and maps it for writing.
     * Must be called before UpdateTimestamps or IsInitialized.
     * @param mode Permissions (e.g., 0666 or 0660) to use if creating the segment.
     * @return True on success, false on any failure (shm_open, ftruncate, mmap).
     */
    bool CreateOrOpen(mode_t mode = 0666);

    /**
     * @brief Updates both timestamps in the mapped shared memory.
     * @param update_time The timestamp of the current update cycle.
     * @param last_active_time The calculated overall last active time.
     * @return True if updated successfully, false if not initialized or pointer is invalid.
     */
    // *** SIGNATURE CHANGED ***
    bool UpdateTimestamps(int64_t update_time, int64_t last_active_time);

    /**
     * @brief Checks if the shared memory was successfully initialized (opened and mapped).
     * @return True if initialized and ready for updates, false otherwise.
     */
    bool IsInitialized() const;

    /**
     * @brief Explicitly unlinks the shared memory segment.
     * Call during clean shutdown if desired. Idempotent.
     * @return True if unlink succeeded or segment already gone, false on error.
     */
    bool UnlinkSegment();

private:
    /**
     * @brief Performs resource cleanup (munmap). Called by destructor.
     */
    void Cleanup();

    //!
    //! \brief This protects against multiple threads in the event_detect process from simultaneously accessing
    //! and writing to the shared memory segment. It does NOT protect from another process encountering a torn
    //! read. I would prefer to use a pthread_mutex_t in the shmem data structure and manege the mutex across
    //! the writer and reader, but the BOINC architect does not believe this is necessary in practice given the small
    //! size and low frequency of writing and reading.
    //!
    std::mutex mtx_shmem;

    std::string m_shm_name;
    int m_shm_fd;
    int64_t* m_mapped_ptr;             // Pointer to the start of the int64_t[2] array
    const size_t m_size;               // Size of the int64_t[2] array
    bool m_is_creator;                 // Did this instance create/resize the segment?
    std::atomic<bool> m_is_initialized;
};

//!
//! \brief Sends the SIGTERM signal to the main thread id initiating a shutdown of all worker threads and the main thread
//! via the HandleSignals function.
//!
void Shutdown(const int &exit_code = 0);

} // namespace event_detect

//!
//! \brief The EventDetectConfig class. This specializes the Config class and implements the virtual method ProcessArgs()
//! for event_detect.
//!
class EventDetectConfig : public Config
{
    void ProcessArgs() override;
};

#endif // EVENT_DETECT_H
