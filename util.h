/*
 * Copyright (C) 2025 James C. Owens
 * Portions Copyright (c) 2019 The Bitcoin Core developers
 * Portions Copyright (c) 2025 The Gridcoin developers
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef UTIL_H
#define UTIL_H

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <tinyformat.h>
#include <variant>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

extern std::atomic<bool> g_debug;
extern std::atomic<bool> g_log_timestamps;

//!
//! /brief Locale-independent version of std::to_string
//!
template <typename T>
std::string ToString(const T& t)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << t;
    return oss.str();
}

//!
//! \brief Utility function to split string by the provided delimiter. Note that no trimming is done to remove white space.
//! \param s: the string to split
//! \param delim: the delimiter string
//! \return std::vector of string parts
//!
[[nodiscard]] std::vector<std::string> StringSplit(const std::string& s, const std::string& delim);

//!
//! \brief Utility function to trim whitespace from the beginning and end of a string.
//! \param str: the string to trim
//! \param pattern: the pattern to trim, defaulting to " \f\n\r\t\v"
//! \return trimmed string
//!
[[nodiscard]] std::string TrimString(const std::string& str, const std::string& pattern = " \f\n\r\t\v");

//!
//! \brief Utility function to remove enclosing single or double quotes from a string. This is especially useful when
//! dealing with quoted values in a config file.
//! \param str: the input string with potential quotes to remove
//! \return the string with any enclosing quotes removed
//!
[[nodiscard]] std::string StripQuotes(const std::string& str);

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
constexpr char ToLower(char c);

/**
 * Returns the lowercase equivalent of the given string.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * This is a feature, not a limitation.
 *
 * @param[in] str   the string to convert to lowercase.
 * @returns         lowercased equivalent of str
 */
std::string ToLower(const std::string& str);

//!
//! \brief Returns number of seconds since the beginning of the Unix Epoch.
//! \return int64_t seconds.
//!
int64_t GetUnixEpochTime();

//!
//! \brief Formats input unix epoch time in human readable format.
//! \param int64_t seconds.
//! \return ISO8601 conformant datetime string.
//!
std::string FormatISO8601DateTime(int64_t time);

//!
//! \brief Validate timestamp for purposes of idle_detect and event_detect.
//! \param timestamp
//! \return boolean flag of whether the timestamp is valid.
//!
bool IsValidTimestamp(const int64_t& timestamp);

template <typename... Args>
//!
//! \brief Creates a string with fmt specifier and variadic args.
//! \param fmt specifier
//! \param args... variadic
//! \return formatted std::string
//!
static inline std::string LogPrintStr(const char* fmt, const Args&... args)
{
    std::string log_msg;

    // Conditionally add timestamp prefix based on g_log_timestamps flag.
    if (g_log_timestamps.load(std::memory_order_relaxed)) { // Check the flag
        log_msg = FormatISO8601DateTime(GetUnixEpochTime()) + " ";
    }

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
    if (g_debug.load()) {
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

[[nodiscard]] int ParseStringToInt(const std::string& str);

[[nodiscard]] int64_t ParseStringtoInt64(const std::string& str);

//!
//! \brief Finds directory entries in the provided path that match the provided wildcard string.
//! \param directory
//! \param wildcard
//! \return std::vector of fs::paths that match.
//!
std::vector<fs::path> FindDirEntriesWithWildcard(const fs::path& directory, const std::string& wildcard);

//!
//! \brief Safely get an enviroment variable value from the provided name
//! \param std::string of the name of the variable to retrieve
//! \return std::string of the value of the requested variable. std::nullopt if not found.
//!
std::optional<std::string> GetEnvVariable(const std::string& var_name);

//!
//! \brief The EventDetectException class is a customized exception handling class for the event_detect application.
//!
class EventIdleDetectException : public std::exception
{
public:
    EventIdleDetectException(const std::string& message) : m_message(message) {}
    EventIdleDetectException(const char* message) : m_message(message) {}

    const char* what() const noexcept override {
        return m_message.c_str();
    }

protected:
    std::string m_message;
};

//! File system related exceptions
class FileSystemException : public EventIdleDetectException
{
public:
    FileSystemException(const std::string& message, const std::filesystem::path& path)
        : EventIdleDetectException(message + " Path: " + path.string()), m_path(path) {}

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

//! Threading Related Exceptions
class ThreadException : public EventIdleDetectException
{
public:
    ThreadException(const std::string& message) : EventIdleDetectException(message) {}
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

protected:
    //!
    //! \brief Private version of GetArg that operates on m_config_in and also selects the provided default value
    //! if the arg is not found. This is how default values for parameters are established.
    //! \param arg (key) to look up value as string.
    //! \param default_value if arg is not found.
    //! \return string value found in lookup, default value if not found.
    //!
    std::string GetArgString(const std::string& arg, const std::string& default_value) const;

    //!
    //! \brief Holds the processed parameter-values, which are strongly typed and in a config_variant union, and where
    //! default values are populated if not found in the config file (m_config_in).
    //!
    std::multimap<std::string, config_variant> m_config;

private:
    //!
    //! \brief Private helper method used by ReadAndUpdateConfig. Note this is pure virtual. It must be implemented
    //! in a specialization of a derived class for use by a specific application.
    //!
    virtual void ProcessArgs() = 0;


    //!
    //! \brief This is the mutex member that provides lock control for the config object. This is used to ensure the
    //! config object is thread-safe.
    //!
    mutable std::mutex mtx_config;

    //!
    //! \brief Holds the raw parsed parameter-values from the config file.
    //!
    std::multimap<std::string, std::string> m_config_in;

};

//!
//! \brief The EventMessage class is a small class that encapsulates the "event message", which is a message sent
//! from the local idle_detect instance to event_detect indicating an event that updates the last active time. The
//! message is effectively timestamp:m_event_type in string format (to make use of pipe easier).
//!
//! The class stores these in their native format.
//!
//! A validation method and conversion to string format are provided.
//!
class EventMessage
{
public:
    //!
    //! \brief The EventType enum defines event types for the EventMessage class. Currently there is only
    //! one event type in use, USER_ACTIVE.
    //!
    enum EventType { // Note that if this enum is expanded, EventTypeToString must also be updated.
        UNKNOWN,
        USER_ACTIVE,
        USER_UNFORCE,
        USER_FORCE_ACTIVE,
        USER_FORCE_IDLE
    };

    //!
    //! \brief Constructs an "empty" EventMessage with timestamp of 0 and EventType of UNKNOWN.
    //!
    EventMessage();

    //!
    //! \brief Constructs an EventMessage from the provided parameters
    //! \param timestamp
    //! \param event_type
    //!
    EventMessage(int64_t timestamp, EventType event_type);

    //!
    //! \brief Constructs an EventMessage from the provided strings.
    //! \param timestamp_str
    //! \param event_type_str
    //!
    EventMessage(std::string timestamp_str, std::string event_type_str);

    //!
    //! \brief Converts m_event_type member variable in the EventMessage object to a string.
    //! \return string representation of enum value
    //!
    std::string EventTypeToString();

    //!
    //! \brief Static version that takes the event_type enum as a parameter and provides the string representation.
    //! \param event_type
    //! \return string representation of enum value
    //!
    static std::string EventTypeToString(const EventType& event_type);

    //!
    //! \brief Validates the EventMessage object.
    //! \return true if EventMessage is valid.
    //!
    bool IsValid();

    //!
    //! \brief Returns the string message format of the EventMessage object. This is meant to go on the pipe. This
    //! is in lieu of a full serialization approach, which is overkill here.
    //! \return std::string in the format of <timestamp>:<event_type string>
    //!
    std::string ToString();

    int64_t m_timestamp;
    EventType m_event_type;

private:
    //!
    //! \brief This converts the event type string to the proper enum value. It is the converse of EventTypeToString().
    //! \param event_type_str
    //! \return EventType enum value
    //!
    EventType EventTypeStringToEnum(const std::string& event_type_str);
};


#endif // UTIL_H
