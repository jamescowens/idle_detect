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
#include <vector>

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

// ============================================================================
// FailureReportThrottle
//
// The ladder that keeps a once-per-second failure from becoming a once-per-second log line. Its shape is
// load bearing: reports must land on the onset and on each widening of the interval, and must stop
// widening at the ceiling, or a permanently broken input still produces unbounded output.
// ============================================================================

namespace {

//!
//! \brief Feeds consecutive failures to a throttle and returns the 1-based ordinals it reported on.
//!
std::vector<int> ReportedFailureOrdinals(FailureReportThrottle& throttle, int failures)
{
    std::vector<int> reported;

    for (int i = 1; i <= failures; ++i) {
        if (throttle.RecordFailure()) {
            reported.push_back(i);
        }
    }

    return reported;
}

} // namespace

TEST(FailureReportThrottle, ReportsTheFirstFailureImmediately)
{
    FailureReportThrottle throttle(1, 64);

    // The onset of a fault must never be suppressed: a throttle that swallowed it would hide a genuine
    // problem behind the mechanism meant to stop the noise from it.
    EXPECT_TRUE(throttle.RecordFailure());
    EXPECT_EQ(throttle.ConsecutiveFailures(), 1);
}

TEST(FailureReportThrottle, DoublesTheSuppressionIntervalOnTheDocumentedLadder)
{
    FailureReportThrottle throttle(1, 64);

    // The same ladder IdleSourcePool::RecordCandidateFailure() and ShellIdleSource::NoteQueryOutcome()
    // apply, and the documented sequence in both: 1st, 3rd, 6th, 11th, 20th, 37th, 70th.
    const std::vector<int> expected = {1, 3, 6, 11, 20, 37, 70};

    EXPECT_EQ(ReportedFailureOrdinals(throttle, 70), expected);
}

TEST(FailureReportThrottle, CapsTheIntervalAtTheCeiling)
{
    FailureReportThrottle throttle(1, 64);

    ReportedFailureOrdinals(throttle, 70);

    EXPECT_EQ(throttle.Interval(), 64);

    // Past the ceiling the interval must stay put rather than going on doubling. Doubling forever is the
    // failure this bound exists to prevent in the other direction: it would eventually stop reporting a
    // standing fault altogether. Exactly 64 more failures are suppressed and the 65th is reported, which
    // is the 135th of the run.
    const std::vector<int> expected = {65};

    EXPECT_EQ(ReportedFailureOrdinals(throttle, 65), expected);
    EXPECT_EQ(throttle.Interval(), 64);
    EXPECT_EQ(throttle.ConsecutiveFailures(), 135);
}

TEST(FailureReportThrottle, BoundsOutputForAPermanentlyBrokenInput)
{
    FailureReportThrottle throttle(1, 64);

    // 86400 failures is one per second for a day, which is exactly the measurement that motivated the
    // ladder. Unthrottled that is 86400 lines; the ceiling has to hold it to about one per minute.
    const std::vector<int> reported = ReportedFailureOrdinals(throttle, 86400);

    EXPECT_LT(reported.size(), 1400u);
}

TEST(FailureReportThrottle, ResetClearsTheLadderAndReturnsTheRunLength)
{
    FailureReportThrottle throttle(1, 64);

    ReportedFailureOrdinals(throttle, 10);

    // The count is what the caller puts in its recovery line, and it is the only place the size of the
    // outage appears at normal level, since the middle of the run was suppressed.
    EXPECT_EQ(throttle.Reset(), 10);
    EXPECT_EQ(throttle.ConsecutiveFailures(), 0);
    EXPECT_EQ(throttle.Interval(), 0);
}

TEST(FailureReportThrottle, ResetWithNoRunReportsNothingToRecoverFrom)
{
    FailureReportThrottle throttle(1, 64);

    // Called on every success, which is most ticks. It must be able to say "there was no outage" so the
    // caller does not announce a recovery from a fault that never happened.
    EXPECT_EQ(throttle.Reset(), 0);
}

TEST(FailureReportThrottle, AFailureAfterResetIsReportedImmediatelyAgain)
{
    FailureReportThrottle throttle(1, 64);

    ReportedFailureOrdinals(throttle, 70);
    throttle.Reset();

    // A new fault must not inherit a suppression window earned by the previous one. Leaving the ladder
    // standing would hide the onset of the next outage behind up to 64 silent ticks.
    EXPECT_TRUE(throttle.RecordFailure());
    EXPECT_EQ(throttle.ConsecutiveFailures(), 1);
    EXPECT_EQ(throttle.Interval(), 1);
}

TEST(FailureReportThrottle, ClampsADegenerateInitialInterval)
{
    // An interval of zero would report every failure and defeat the object entirely.
    FailureReportThrottle throttle(0, 64);

    const std::vector<int> expected = {1, 3, 6, 11, 20};

    EXPECT_EQ(ReportedFailureOrdinals(throttle, 20), expected);
}

TEST(FailureReportThrottle, ClampsACeilingBelowTheFloor)
{
    // A ceiling under the floor would make the ladder shrink on its second step instead of growing.
    FailureReportThrottle throttle(4, 1);

    const std::vector<int> expected = {1, 6, 11, 16};

    EXPECT_EQ(ReportedFailureOrdinals(throttle, 20), expected);
    EXPECT_EQ(throttle.Interval(), 4);
}
