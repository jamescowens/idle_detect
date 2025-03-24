/*
 * Copyright (C) 2025 James C. Owens
 * Portions Copyright (c) 2019 The Bitcoin Core developers
 * Portions Copyright (c) 2025 The Gridcoin developers
 *
 * This code is licensed under the MIT license
 */

#ifndef EVENT_DETECT_H
#define EVENT_DETECT_H

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <unistd.h>
#include <sys/wait.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <tinyformat.h>

namespace fs = std::filesystem;

bool g_debug = 0;

[[nodiscard]] std::vector<std::string> StringSplit(const std::string& s, const std::string& delim)
{
    size_t pos = 0;
    size_t end = 0;
    std::vector<std::string> elems;

    while((end = s.find(delim, pos)) != std::string::npos)
    {
        elems.push_back(s.substr(pos, end - pos));
        pos = end + delim.size();
    }

    // Append final value
    elems.push_back(s.substr(pos, end - pos));
    return elems;
}

[[nodiscard]] std::string TrimString(const std::string& str, const std::string& pattern = " \f\n\r\t\v")
{
    std::string::size_type front = str.find_first_not_of(pattern);
    if (front == std::string::npos) {
        return std::string();
    }
    std::string::size_type end = str.find_last_not_of(pattern);
    return str.substr(front, end - front + 1);
}

[[nodiscard]] std::string StripQuotes(const std::string& str) {
    if (str.empty()) {
        return str; // No quotes to strip from an empty string.
    }

    std::string result = str; // Create a copy so we can modify it.

    if (result.front() == '"' || result.front() == '\'') {
        result.erase(0, 1); // Remove the leading quote.
    }

    if (!result.empty() && (result.back() == '"' || result.back() == '\'')) {
        result.pop_back(); // Remove the trailing quote.
    }

    return result;
}

/**
 * Converts the given character to its lowercase equivalent.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * This is a feature, not a limitation.
 *
 * @param[in] c     the character to convert to lowercase.
 * @return          the lowercase equivalent of c; or the argument
 *                  if no conversion is possible.
 */
constexpr char ToLower(char c)
{
    return (c >= 'A' && c <= 'Z' ? (c - 'A') + 'a' : c);
}

/**
 * Returns the lowercase equivalent of the given string.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * This is a feature, not a limitation.
 *
 * @param[in] str   the string to convert to lowercase.
 * @returns         lowercased equivalent of str
 */
std::string ToLower(const std::string& str)
{
    std::string r;
    for (auto ch : str) r += ToLower((unsigned char)ch);
    return r;
}

//!
//! \brief Returns number of seconds since the beginning of the Unix Epoch.
//! \return int64_t seconds.
//!
int64_t GetUnixEpochTime()
{
    // Get the current time point
    auto now = std::chrono::system_clock::now();

    // Convert the time point to a duration since the epoch
    auto duration = now.time_since_epoch();

    // Convert the duration to seconds
    int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

    return seconds;
}

//!
//! \brief Formats input unix epoch time in human readable format.
//! \param int64_t seconds.
//! \return ISO8601 conformant datetime string.
//!
std::string FormatISO8601DateTime(int64_t time) {
    struct tm ts;
    time_t time_val = time;
    if (gmtime_r(&time_val, &ts) == nullptr) {
         return {};
    }

    return strprintf("%04i-%02i-%02iT%02i:%02i:%02iZ", ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec);
}

template <typename... Args>
//!
//! \brief Creates a string with fmt specifier and variadic args.
//! \param fmt specifier
//! \param args... variadic
//! \return formatted std::string
//!
static inline std::string LogPrintStr(const char* fmt, const Args&... args)
{
    std::string log_msg = FormatISO8601DateTime(GetUnixEpochTime()) + " ";

    try {
        log_msg += tfm::format(fmt, args...);
    } catch (tinyformat::format_error& fmterr) {
        /* Original format string will have newline so don't add one here */
        log_msg += "Error \"" + std::string(fmterr.what()) + "\" while formatting log message: " + fmt;
    }

    log_msg += "\n";

    return log_msg;
}

template <typename... Args>
//!
//! \brief LogPrintStr directed to cout.
//! \param fmt
//! \param args
//!
void log(const char* fmt, const Args&... args)
{
    std::cout << LogPrintStr(fmt, args...);
}

template <typename... Args>
//!
//! \brief LogPrintStr directed to cout, conditioned on the debug setting.
//! \param fmt
//! \param args
//!
void debug_log(const char* fmt, const Args&... args)
{
    if (g_debug) {
        log(fmt, args...);
    }
}

template <typename... Args>
//!
//! \brief LogPrintStr directed to cerr
//! \param fmt
//! \param args
//!
void error_log(const char* fmt, const Args&... args)
{
    std::string error_fmt = "ERROR: ";
    error_fmt += fmt;

    std::cerr << LogPrintStr(error_fmt.c_str(), args...);
}

[[nodiscard]] int ParseStringToInt(const std::string& str){
    try{
        return std::stoi(str);
    }
    catch (const std::invalid_argument& e){
        error_log("%s: Invalid argument: %s",
                  __func__,
                  e.what());
        throw;
    }
    catch (const std::out_of_range& e){
        error_log("%s: Out of range: %s",
                  __func__,
                  e.what());
        throw;
    }
}

std::multimap<std::string, std::string> ReadConfig(const std::string& filename) {
    std::multimap<std::string, std::string> config;
    std::ifstream file(filename);

    if (!file.is_open()) {
        error_log("%s: Could not open the config file: %s",
                  __func__,
                  filename);
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and lines starting with '#'
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::vector line_elements = StringSplit(line, "=");

        if (line_elements.size() != 2) {
            continue;
        }

        config.insert(std::make_pair(StripQuotes(TrimString(line_elements[0])),
                                     StripQuotes(TrimString(line_elements[1]))));
    }

    file.close();
    return config;
}

//!
//! \brief Finds directory entries in the provided path that match the provided wildcard string.
//! \param directory
//! \param wildcard
//! \return std::vector of fs::paths that match.
//!
std::vector<fs::path> FindDirEntriesWithWildcard(const fs::path& directory, const std::string& wildcard)
{
    std::vector<fs::path> matching_entries;
    std::regex regex_wildcard(wildcard); //convert wildcard to regex.

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
         debug_log("WARNING: %s, directory %s to search for regex expression \"%s\""
                  "does not exist or is not a directory.",
                  __func__,
                  directory,
                  wildcard);

        return matching_entries;
    }

    for (const auto& entry : fs::directory_iterator(directory)) {
        if (std::regex_match(entry.path().filename().string(), regex_wildcard)) {
            matching_entries.push_back(entry.path());
        }
    }

    return matching_entries;
}

//!
//! \brief The EventMonitor class provides the framework for monitoring event activity recorded by the EventRecorders
//! class EventRecorder threads. It also monitors changes in the input event devices and resets the EventRecorder threads
//! if they change. It also updates the m_last_active_time. The class is a singleton and has one instantiated thread.
//! It uses locks to protect the event_monitor device paths and the thread. The m_last_active_time is an atomic and requires
//! no explicit locking.
//!
class EventMonitor
{
public:
    std::thread m_event_monitor_thread;
    std::condition_variable cv_monitor_thread;
    std::atomic<bool> m_interrupt_monitor;

    EventMonitor();

    EventMonitor(std::vector<fs::path> event_device_paths);

    std::vector<fs::path> GetEventDevices();

    void UpdateEventDevices();

    void EventActivityMonitorThread();

    bool IsInitialized();

    int64_t GetLastActiveTime();

private:
    static std::vector<fs::path> EnumerateEventDevices();

    mutable std::mutex mtx_event_monitor;
    mutable std::mutex mtx_event_monitor_thread;

    std::vector<fs::path> m_event_device_paths;

    std::atomic<int64_t> m_last_active_time;

    bool m_initialized;
};

//!
//! \brief The EventRecorders class provides the framework for recording event activity from each of the input event devices that
//! are classified as a pointing device (mouse). It is a singleton, but has multiple subordinate threads running, 1 thread for each
//! identified device to monitor.
//!
class EventRecorders
{
    friend EventMonitor;

public:
    std::condition_variable cv_recorder_threads;
    std::atomic<bool> m_interrupt_recorders;

    EventRecorders();

    EventRecorders(std::vector<fs::path> event_device_paths);

    class EventRecorder
    {
    public:
        std::thread m_event_recorder_thread;

        EventRecorder(fs::path event_device_path);

        fs::path GetEventDevicePath();

        int64_t GetEventCount();

        void EventActivityRecorderThread();

    private:
        mutable std::mutex mtx_event_recorder;

        fs::path m_event_device_path;

        std::atomic<int64_t> m_event_count;
    };

    int64_t GetTotalEventCount();

    std::vector<std::shared_ptr<EventRecorder>>& GetEventRecorders();

    void ResetEventRecorders();

private:
    mutable std::mutex mtx_event_recorders;
    mutable std::mutex mtx_event_recorder_threads;

    std::vector<std::shared_ptr<EventRecorder>> m_event_recorder_ptrs;
};

#endif // EVENT_DETECT_H
