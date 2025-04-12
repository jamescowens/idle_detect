/*
 * Copyright (C) 2025 James C. Owens
 * Portions Copyright (c) 2019 The Bitcoin Core developers
 * Portions Copyright (c) 2025 The Gridcoin developers
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <util.h>
#include <chrono>
#include <cstddef>
#include <regex>
#include <fstream>

//!
//! \brief This to support early use of the log utility functions before the config is read to get the
//! debug flag.
//!
std::atomic<bool> g_debug = false;

//!
//! \brief The flag controls the logging of timestamps by the log functions. This is used to suppress
//! timestamp output when run under systemd, where the journal appends a high resolution timestamp.
//!
std::atomic<bool> g_log_timestamps = true;

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

[[nodiscard]] std::string TrimString(const std::string& str, const std::string& pattern)
{
    std::string::size_type front = str.find_first_not_of(pattern);
    if (front == std::string::npos) {
        return std::string();
    }
    std::string::size_type end = str.find_last_not_of(pattern);
    return str.substr(front, end - front + 1);
}

[[nodiscard]] std::string StripQuotes(const std::string& str)
{
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

constexpr char ToLower(char c)
{
    return (c >= 'A' && c <= 'Z' ? (c - 'A') + 'a' : c);
}

std::string ToLower(const std::string& str)
{
    std::string r;
    for (auto ch : str) r += ToLower((unsigned char)ch);
    return r;
}

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

std::string FormatISO8601DateTime(int64_t time)
{
    struct tm ts;
    time_t time_val = time;
    if (gmtime_r(&time_val, &ts) == nullptr) {
        return {};
    }

    return strprintf("%04i-%02i-%02iT%02i:%02i:%02iZ",
                     ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec);
}

bool IsValidTimestamp(const int64_t& timestamp)
{
    int64_t now = GetUnixEpochTime();

    int64_t future_limit = now + 60; // No more than 60 seconds in the future.
    int64_t past_limit = now - 10 * 86400 * 365; // No more than ten years in the past.

    return timestamp >= past_limit && timestamp <= future_limit;
}

[[nodiscard]] int ParseStringToInt(const std::string& str)
{
    try {
        return std::stoi(str);
    } catch (const std::invalid_argument& e){
        error_log("%s: Invalid argument: %s",
                  __func__,
                  e.what());
        throw;
    } catch (const std::out_of_range& e){
        error_log("%s: Out of range: %s",
                  __func__,
                  e.what());
        throw;
    }
}

[[nodiscard]] int64_t ParseStringtoInt64(const std::string& str)
{
    try {
        return static_cast<int64_t>(std::stoll(str));
    } catch (const std::invalid_argument& e){
        error_log("%s: Invalid argument: %s",
                  __func__,
                  e.what());
        throw;
    } catch (const std::out_of_range& e){
        error_log("%s: Out of range: %s",
                  __func__,
                  e.what());
        throw;
    }
}

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

// Class Config

Config::Config()
{}

void Config::ReadAndUpdateConfig(const fs::path& config_file) {
    std::unique_lock<std::mutex> lock(mtx_config);

    std::multimap<std::string, std::string> config;

    try {
        std::ifstream file(config_file);

        if (!file.is_open()) {
            error_log("%s: Could not open the config file: %s",
                      __func__,
                      config_file);
            return;
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

        // Do this all at once so the result of the config read is essentially "atomic".
        m_config_in.swap(config);
    } catch (FileSystemException& e) {
        error_log("%s: Reading config file failed, so defaults will be used: %s",
                  __func__,
                  e.what());
    }

    // If the config file read failed, we will process args anyway, which will result in defaults being chosen.
    ProcessArgs();
}

config_variant Config::GetArg(const std::string& arg)
{
    std::unique_lock<std::mutex> lock(mtx_config);

    auto iter = m_config.find(arg);

    if (iter != m_config.end()) {
        return iter->second;
    } else {
        return std::string {};
    }
}

std::string Config::GetArgString(const std::string& arg, const std::string& default_value) const
{
    auto iter = m_config_in.find(arg);

    if (iter != m_config_in.end()) {
        return iter->second;
    } else {
        return default_value;
    }
}

EventMessage::EventMessage()
    : m_timestamp(0)
    , m_event_type(UNKNOWN)
{}

EventMessage::EventType EventMessage::EventTypeStringToEnum(const std::string& event_type_str)
{
    if (event_type_str == std::string {"USER_ACTIVE"}) {
        return USER_ACTIVE;
    }

    return UNKNOWN;
}

EventMessage::EventMessage(std::string timestamp_str, std::string event_type_str)
{
    m_timestamp = ParseStringtoInt64(timestamp_str);

    m_event_type = EventTypeStringToEnum(event_type_str);
}

std::string EventMessage::EventTypeToString()
{
    return EventTypeToString(m_event_type);
}

std::string EventMessage::EventTypeToString(const EventType& event_type)
{
    std::string out;

    switch (event_type) {
    case UNKNOWN:
        out = "UNKNOWN";
        break;
    case USER_ACTIVE:
        out = "USER_ACTIVE";
        break;
    }

    return out;
}

bool EventMessage::IsValid()
{
    return m_event_type != UNKNOWN && IsValidTimestamp(m_timestamp);
}

std::string EventMessage::ToString()
{
    std::string out = ::ToString(m_timestamp) + ":" + EventTypeToString(m_event_type);

    return out;
}
