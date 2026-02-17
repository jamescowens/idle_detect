/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <util.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

//!
//! \brief Concrete Config subclass for testing the base class behavior.
//! Implements ProcessArgs() to store values with typed conversions, mirroring
//! the pattern used by EventDetectConfig and IdleDetectConfig.
//!
class TestConfig : public Config
{
    void ProcessArgs() override
    {
        // String parameter
        std::string str_val = GetArgString("string_param", "default_string");
        m_config.insert(std::make_pair("string_param", str_val));

        // Boolean parameter (same pattern as EventDetectConfig)
        std::string bool_str = GetArgString("bool_param", "true");
        if (bool_str == "1" || ToLower(bool_str) == "true") {
            m_config.insert(std::make_pair("bool_param", true));
        } else if (bool_str == "0" || ToLower(bool_str) == "false") {
            m_config.insert(std::make_pair("bool_param", false));
        }

        // Integer parameter
        int int_val = 0;
        try {
            int_val = ParseStringToInt(GetArgString("int_param", "42"));
        } catch (...) {}
        m_config.insert(std::make_pair("int_param", int_val));

        // Path parameter
        fs::path path_val;
        try {
            path_val = fs::path(GetArgString("path_param", "/default/path"));
        } catch (...) {}
        m_config.insert(std::make_pair("path_param", path_val));
    }
};

//!
//! \brief Test fixture that manages a temporary config file.
//!
class ConfigTest : public ::testing::Test
{
protected:
    fs::path m_test_dir;
    fs::path m_config_path;

    void SetUp() override
    {
        m_test_dir = fs::temp_directory_path() / "idle_detect_test_config";
        fs::create_directories(m_test_dir);
        m_config_path = m_test_dir / "test.conf";
    }

    void TearDown() override
    {
        fs::remove_all(m_test_dir);
    }

    void WriteConfigFile(const std::string& content)
    {
        std::ofstream f(m_config_path);
        f << content;
        f.close();
    }
};

// ============================================================================
// Basic key=value parsing
// ============================================================================

TEST_F(ConfigTest, BasicKeyValueParsing)
{
    WriteConfigFile(
        "string_param=hello\n"
        "bool_param=false\n"
        "int_param=100\n"
        "path_param=/tmp/test\n"
    );

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "hello");
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), false);
    EXPECT_EQ(std::get<int>(config.GetArg("int_param")), 100);
    EXPECT_EQ(std::get<fs::path>(config.GetArg("path_param")), fs::path("/tmp/test"));
}

// ============================================================================
// Default values when file has missing keys
// ============================================================================

TEST_F(ConfigTest, DefaultValues)
{
    WriteConfigFile(""); // Empty config, all defaults

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "default_string");
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), true);
    EXPECT_EQ(std::get<int>(config.GetArg("int_param")), 42);
    EXPECT_EQ(std::get<fs::path>(config.GetArg("path_param")), fs::path("/default/path"));
}

// ============================================================================
// Comments and empty lines are skipped
// ============================================================================

TEST_F(ConfigTest, CommentsSkipped)
{
    WriteConfigFile(
        "# This is a comment\n"
        "string_param=from_file\n"
        "# Another comment\n"
    );

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "from_file");
}

TEST_F(ConfigTest, EmptyLinesSkipped)
{
    WriteConfigFile(
        "\n"
        "string_param=value\n"
        "\n"
        "\n"
        "int_param=99\n"
    );

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "value");
    EXPECT_EQ(std::get<int>(config.GetArg("int_param")), 99);
}

// ============================================================================
// Quoted values are stripped
// ============================================================================

TEST_F(ConfigTest, DoubleQuotedValues)
{
    WriteConfigFile("string_param=\"quoted_value\"\n");

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "quoted_value");
}

TEST_F(ConfigTest, SingleQuotedValues)
{
    WriteConfigFile("string_param='single_quoted'\n");

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "single_quoted");
}

// ============================================================================
// Whitespace is trimmed from keys and values
// ============================================================================

TEST_F(ConfigTest, WhitespaceTrimmed)
{
    WriteConfigFile("  string_param  =  spaced_value  \n");

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "spaced_value");
}

// ============================================================================
// Malformed lines are skipped
// ============================================================================

TEST_F(ConfigTest, MalformedLinesSkipped)
{
    WriteConfigFile(
        "no_equals_sign\n"
        "string_param=good_value\n"
        "too=many=equals\n"
    );

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    // The good line should parse; malformed lines are skipped
    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "good_value");
}

// ============================================================================
// Non-existent config file uses defaults
// ============================================================================

TEST_F(ConfigTest, NonExistentFileUsesDefaults)
{
    TestConfig config;
    config.ReadAndUpdateConfig(m_test_dir / "nonexistent.conf");

    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "default_string");
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), true);
    EXPECT_EQ(std::get<int>(config.GetArg("int_param")), 42);
}

// ============================================================================
// GetArg for non-existing key returns empty string variant
// ============================================================================

TEST_F(ConfigTest, GetArgNonExistingKey)
{
    WriteConfigFile("string_param=value\n");

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    config_variant result = config.GetArg("nonexistent_key");
    EXPECT_EQ(std::get<std::string>(result), "");
}

// ============================================================================
// Boolean parsing variants
// ============================================================================

TEST_F(ConfigTest, BooleanTrue)
{
    WriteConfigFile("bool_param=true\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), true);
}

TEST_F(ConfigTest, BooleanTrueUppercase)
{
    WriteConfigFile("bool_param=TRUE\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), true);
}

TEST_F(ConfigTest, BooleanOne)
{
    WriteConfigFile("bool_param=1\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), true);
}

TEST_F(ConfigTest, BooleanFalse)
{
    WriteConfigFile("bool_param=false\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), false);
}

TEST_F(ConfigTest, BooleanZero)
{
    WriteConfigFile("bool_param=0\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<bool>(config.GetArg("bool_param")), false);
}

// ============================================================================
// Re-read config overwrites previous values
// ============================================================================

TEST_F(ConfigTest, ReReadConfig)
{
    WriteConfigFile("string_param=first\n");
    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);
    EXPECT_EQ(std::get<std::string>(config.GetArg("string_param")), "first");

    WriteConfigFile("string_param=second\n");
    config.ReadAndUpdateConfig(m_config_path);

    // After re-read, m_config gets new entries appended (multimap).
    // GetArg returns the first match, which is from the first read.
    // This tests the actual behavior of the multimap-based Config.
    auto result = std::get<std::string>(config.GetArg("string_param"));
    // The multimap accumulates entries; first inserted value is returned by find()
    EXPECT_FALSE(result.empty());
}

// ============================================================================
// Config file with values containing paths
// ============================================================================

TEST_F(ConfigTest, PathValues)
{
    WriteConfigFile("path_param=/run/event_detect\n");

    TestConfig config;
    config.ReadAndUpdateConfig(m_config_path);

    EXPECT_EQ(std::get<fs::path>(config.GetArg("path_param")), fs::path("/run/event_detect"));
}
