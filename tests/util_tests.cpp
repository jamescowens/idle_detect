/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <util.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// StringSplit
// ============================================================================

TEST(StringSplit, BasicSplit)
{
    auto result = StringSplit("a:b:c", ":");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "c");
}

TEST(StringSplit, MultiCharDelimiter)
{
    auto result = StringSplit("one::two::three", "::");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "one");
    EXPECT_EQ(result[1], "two");
    EXPECT_EQ(result[2], "three");
}

TEST(StringSplit, NoDelimiterFound)
{
    auto result = StringSplit("nodelimiter", ":");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "nodelimiter");
}

TEST(StringSplit, EmptyString)
{
    auto result = StringSplit("", ":");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "");
}

TEST(StringSplit, TrailingDelimiter)
{
    auto result = StringSplit("a:b:", ":");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "");
}

TEST(StringSplit, LeadingDelimiter)
{
    auto result = StringSplit(":a:b", ":");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "");
    EXPECT_EQ(result[1], "a");
    EXPECT_EQ(result[2], "b");
}

TEST(StringSplit, ConsecutiveDelimiters)
{
    auto result = StringSplit("a::b", ":");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "");
    EXPECT_EQ(result[2], "b");
}

// ============================================================================
// TrimString
// ============================================================================

TEST(TrimString, WhitespaceTrimming)
{
    EXPECT_EQ(TrimString("  hello  "), "hello");
    EXPECT_EQ(TrimString("\t\nhello\r\n"), "hello");
}

TEST(TrimString, NoTrimmingNeeded)
{
    EXPECT_EQ(TrimString("hello"), "hello");
}

TEST(TrimString, CustomPattern)
{
    EXPECT_EQ(TrimString("xxhelloxx", "x"), "hello");
}

TEST(TrimString, EmptyString)
{
    EXPECT_EQ(TrimString(""), "");
}

TEST(TrimString, AllWhitespace)
{
    EXPECT_EQ(TrimString("   \t\n  "), "");
}

TEST(TrimString, InternalWhitespacePreserved)
{
    EXPECT_EQ(TrimString("  hello world  "), "hello world");
}

// ============================================================================
// StripQuotes
// ============================================================================

TEST(StripQuotes, DoubleQuotes)
{
    EXPECT_EQ(StripQuotes("\"hello\""), "hello");
}

TEST(StripQuotes, SingleQuotes)
{
    EXPECT_EQ(StripQuotes("'hello'"), "hello");
}

TEST(StripQuotes, MixedQuotes)
{
    EXPECT_EQ(StripQuotes("\"hello'"), "hello");
    EXPECT_EQ(StripQuotes("'hello\""), "hello");
}

TEST(StripQuotes, NoQuotes)
{
    EXPECT_EQ(StripQuotes("hello"), "hello");
}

TEST(StripQuotes, EmptyString)
{
    EXPECT_EQ(StripQuotes(""), "");
}

TEST(StripQuotes, QuotesOnly)
{
    EXPECT_EQ(StripQuotes("\"\""), "");
    EXPECT_EQ(StripQuotes("''"), "");
}

TEST(StripQuotes, SingleCharacterQuote)
{
    EXPECT_EQ(StripQuotes("\""), "");
    EXPECT_EQ(StripQuotes("'"), "");
}

// ============================================================================
// ToLower
// ============================================================================

TEST(ToLower, CharUppercase)
{
    EXPECT_EQ(ToLower(std::string("A")), "a");
    EXPECT_EQ(ToLower(std::string("Z")), "z");
}

TEST(ToLower, CharLowercase)
{
    EXPECT_EQ(ToLower(std::string("a")), "a");
}

TEST(ToLower, CharNonAlpha)
{
    EXPECT_EQ(ToLower(std::string("1")), "1");
    EXPECT_EQ(ToLower(std::string("!")), "!");
}

TEST(ToLower, StringMixedCase)
{
    EXPECT_EQ(ToLower(std::string("Hello World")), "hello world");
}

TEST(ToLower, StringEmpty)
{
    EXPECT_EQ(ToLower(std::string("")), "");
}

TEST(ToLower, StringAllUpper)
{
    EXPECT_EQ(ToLower(std::string("ABCXYZ")), "abcxyz");
}

// ============================================================================
// GetUnixEpochTime
// ============================================================================

TEST(GetUnixEpochTime, ReasonableValue)
{
    int64_t now = GetUnixEpochTime();

    // Must be after 2024-01-01 (1704067200)
    EXPECT_GT(now, 1704067200);

    // Must not be more than 1 second in the future relative to a second call
    int64_t now2 = GetUnixEpochTime();
    EXPECT_LE(now, now2 + 1);
}

// ============================================================================
// FormatISO8601DateTime
// ============================================================================

TEST(FormatISO8601DateTime, Epoch)
{
    EXPECT_EQ(FormatISO8601DateTime(0), "1970-01-01T00:00:00Z");
}

TEST(FormatISO8601DateTime, KnownTimestamp)
{
    // 2001-09-09T01:46:40Z = 1000000000
    EXPECT_EQ(FormatISO8601DateTime(1000000000), "2001-09-09T01:46:40Z");
}

TEST(FormatISO8601DateTime, AnotherKnownTimestamp)
{
    // 2024-01-01T00:00:00Z = 1704067200
    EXPECT_EQ(FormatISO8601DateTime(1704067200), "2024-01-01T00:00:00Z");
}

// ============================================================================
// IsValidTimestamp
// ============================================================================

TEST(IsValidTimestamp, CurrentTimeIsValid)
{
    EXPECT_TRUE(IsValidTimestamp(GetUnixEpochTime()));
}

TEST(IsValidTimestamp, RecentPastIsValid)
{
    EXPECT_TRUE(IsValidTimestamp(GetUnixEpochTime() - 3600));
}

TEST(IsValidTimestamp, FarFutureIsInvalid)
{
    // 100 years from now
    EXPECT_FALSE(IsValidTimestamp(GetUnixEpochTime() + 100LL * 365 * 86400));
}

TEST(IsValidTimestamp, FarPastIsInvalid)
{
    // 20 years before now (limit is 10 years)
    EXPECT_FALSE(IsValidTimestamp(GetUnixEpochTime() - 20LL * 365 * 86400));
}

TEST(IsValidTimestamp, ZeroIsInvalid)
{
    EXPECT_FALSE(IsValidTimestamp(0));
}

TEST(IsValidTimestamp, SlightlyFutureIsValid)
{
    // 30 seconds in the future (limit is 60)
    EXPECT_TRUE(IsValidTimestamp(GetUnixEpochTime() + 30));
}

TEST(IsValidTimestamp, TooFarFutureBySeconds)
{
    // 120 seconds in the future (limit is 60)
    EXPECT_FALSE(IsValidTimestamp(GetUnixEpochTime() + 120));
}

// ============================================================================
// ParseStringToInt
// ============================================================================

TEST(ParseStringToInt, ValidInt)
{
    EXPECT_EQ(ParseStringToInt("42"), 42);
}

TEST(ParseStringToInt, Negative)
{
    EXPECT_EQ(ParseStringToInt("-10"), -10);
}

TEST(ParseStringToInt, Zero)
{
    EXPECT_EQ(ParseStringToInt("0"), 0);
}

TEST(ParseStringToInt, InvalidString)
{
    EXPECT_THROW((void)ParseStringToInt("abc"), std::invalid_argument);
}

TEST(ParseStringToInt, EmptyString)
{
    EXPECT_THROW((void)ParseStringToInt(""), std::invalid_argument);
}

TEST(ParseStringToInt, Overflow)
{
    EXPECT_THROW((void)ParseStringToInt("99999999999999999999"), std::out_of_range);
}

// ============================================================================
// ParseStringtoInt64
// ============================================================================

TEST(ParseStringtoInt64, ValidInt64)
{
    EXPECT_EQ(ParseStringtoInt64("1000000000000"), 1000000000000LL);
}

TEST(ParseStringtoInt64, Negative)
{
    EXPECT_EQ(ParseStringtoInt64("-5000000000"), -5000000000LL);
}

TEST(ParseStringtoInt64, Zero)
{
    EXPECT_EQ(ParseStringtoInt64("0"), 0);
}

TEST(ParseStringtoInt64, LargeValue)
{
    EXPECT_EQ(ParseStringtoInt64("9223372036854775807"), INT64_MAX);
}

TEST(ParseStringtoInt64, InvalidString)
{
    EXPECT_THROW((void)ParseStringtoInt64("not_a_number"), std::invalid_argument);
}

TEST(ParseStringtoInt64, Overflow)
{
    EXPECT_THROW((void)ParseStringtoInt64("99999999999999999999999"), std::out_of_range);
}

// ============================================================================
// FindDirEntriesWithWildcard
// ============================================================================

class FindDirEntriesTest : public ::testing::Test
{
protected:
    fs::path m_test_dir;

    void SetUp() override
    {
        m_test_dir = fs::temp_directory_path() / "idle_detect_test_finddir";
        fs::create_directories(m_test_dir);

        // Create test files
        std::ofstream(m_test_dir / "test_file_1.txt").close();
        std::ofstream(m_test_dir / "test_file_2.txt").close();
        std::ofstream(m_test_dir / "other.log").close();
    }

    void TearDown() override
    {
        fs::remove_all(m_test_dir);
    }
};

TEST_F(FindDirEntriesTest, NonExistentDirectory)
{
    auto result = FindDirEntriesWithWildcard("/nonexistent_dir_xyz", ".*");
    EXPECT_TRUE(result.empty());
}

TEST_F(FindDirEntriesTest, MatchingFiles)
{
    auto result = FindDirEntriesWithWildcard(m_test_dir, "test_file_.*\\.txt");
    EXPECT_EQ(result.size(), 2u);
}

TEST_F(FindDirEntriesTest, NoMatchingFiles)
{
    auto result = FindDirEntriesWithWildcard(m_test_dir, "nonexistent_.*");
    EXPECT_TRUE(result.empty());
}

TEST_F(FindDirEntriesTest, MatchAllFiles)
{
    auto result = FindDirEntriesWithWildcard(m_test_dir, ".*");
    EXPECT_EQ(result.size(), 3u);
}

TEST_F(FindDirEntriesTest, MatchSpecificExtension)
{
    auto result = FindDirEntriesWithWildcard(m_test_dir, ".*\\.log");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].filename(), "other.log");
}

// ============================================================================
// GetEnvVariable
// ============================================================================

TEST(GetEnvVariable, SetVariable)
{
    setenv("IDLE_DETECT_TEST_VAR", "test_value", 1);
    auto result = GetEnvVariable("IDLE_DETECT_TEST_VAR");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "test_value");
    unsetenv("IDLE_DETECT_TEST_VAR");
}

TEST(GetEnvVariable, UnsetVariable)
{
    unsetenv("IDLE_DETECT_NONEXISTENT_VAR");
    auto result = GetEnvVariable("IDLE_DETECT_NONEXISTENT_VAR");
    EXPECT_FALSE(result.has_value());
}

TEST(GetEnvVariable, EmptyValue)
{
    setenv("IDLE_DETECT_EMPTY_VAR", "", 1);
    auto result = GetEnvVariable("IDLE_DETECT_EMPTY_VAR");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "");
    unsetenv("IDLE_DETECT_EMPTY_VAR");
}

// ============================================================================
// Exception classes
// ============================================================================

TEST(EventIdleDetectException, ConstructionAndWhat)
{
    EventIdleDetectException ex("test error message");
    EXPECT_STREQ(ex.what(), "test error message");
}

TEST(EventIdleDetectException, ConstructFromCString)
{
    EventIdleDetectException ex("c-string error");
    EXPECT_STREQ(ex.what(), "c-string error");
}

TEST(EventIdleDetectException, IsStdException)
{
    EventIdleDetectException ex("test");
    const std::exception& base_ref = ex;
    EXPECT_STREQ(base_ref.what(), "test");
}

TEST(FileSystemException, ConstructionAndWhat)
{
    FileSystemException ex("file error", "/tmp/test.txt");
    std::string what_str = ex.what();
    EXPECT_NE(what_str.find("file error"), std::string::npos);
    EXPECT_NE(what_str.find("/tmp/test.txt"), std::string::npos);
}

TEST(FileSystemException, PathAccessor)
{
    FileSystemException ex("error", "/tmp/test.txt");
    EXPECT_EQ(ex.path(), fs::path("/tmp/test.txt"));
}

TEST(ThreadException, ConstructionAndWhat)
{
    ThreadException ex("thread error message");
    EXPECT_STREQ(ex.what(), "thread error message");
}

TEST(ThreadException, IsEventIdleDetectException)
{
    ThreadException ex("test");
    const EventIdleDetectException& base_ref = ex;
    EXPECT_STREQ(base_ref.what(), "test");
}
