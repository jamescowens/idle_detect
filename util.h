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
#include <tinyformat.h>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

extern std::atomic<bool> g_debug;

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

//!
//! \brief Finds directory entries in the provided path that match the provided wildcard string.
//! \param directory
//! \param wildcard
//! \return std::vector of fs::paths that match.
//!
std::vector<fs::path> FindDirEntriesWithWildcard(const fs::path& directory, const std::string& wildcard);

#endif // UTIL_H
