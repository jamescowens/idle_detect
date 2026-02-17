/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <util.h>

// ============================================================================
// Default Constructor
// ============================================================================

TEST(EventMessage, DefaultConstructor)
{
    EventMessage msg;
    EXPECT_EQ(msg.m_timestamp, 0);
    EXPECT_EQ(msg.m_event_type, EventMessage::UNKNOWN);
}

// ============================================================================
// Parameterized Constructor
// ============================================================================

TEST(EventMessage, ParameterizedConstructor)
{
    int64_t ts = GetUnixEpochTime();
    EventMessage msg(ts, EventMessage::USER_ACTIVE);

    EXPECT_EQ(msg.m_timestamp, ts);
    EXPECT_EQ(msg.m_event_type, EventMessage::USER_ACTIVE);
}

TEST(EventMessage, ParameterizedConstructorAllTypes)
{
    EventMessage msg1(100, EventMessage::UNKNOWN);
    EXPECT_EQ(msg1.m_event_type, EventMessage::UNKNOWN);

    EventMessage msg2(100, EventMessage::USER_UNFORCE);
    EXPECT_EQ(msg2.m_event_type, EventMessage::USER_UNFORCE);

    EventMessage msg3(100, EventMessage::USER_FORCE_ACTIVE);
    EXPECT_EQ(msg3.m_event_type, EventMessage::USER_FORCE_ACTIVE);

    EventMessage msg4(100, EventMessage::USER_FORCE_IDLE);
    EXPECT_EQ(msg4.m_event_type, EventMessage::USER_FORCE_IDLE);
}

// ============================================================================
// String Constructor
// ============================================================================

TEST(EventMessage, StringConstructorUserActive)
{
    int64_t ts = GetUnixEpochTime();
    EventMessage msg(std::to_string(ts), "USER_ACTIVE");

    EXPECT_EQ(msg.m_timestamp, ts);
    EXPECT_EQ(msg.m_event_type, EventMessage::USER_ACTIVE);
}

TEST(EventMessage, StringConstructorUserUnforce)
{
    EventMessage msg("1000000000", "USER_UNFORCE");

    EXPECT_EQ(msg.m_timestamp, 1000000000);
    EXPECT_EQ(msg.m_event_type, EventMessage::USER_UNFORCE);
}

TEST(EventMessage, StringConstructorUserForceActive)
{
    EventMessage msg("1000000000", "USER_FORCE_ACTIVE");
    EXPECT_EQ(msg.m_event_type, EventMessage::USER_FORCE_ACTIVE);
}

TEST(EventMessage, StringConstructorUserForceIdle)
{
    EventMessage msg("1000000000", "USER_FORCE_IDLE");
    EXPECT_EQ(msg.m_event_type, EventMessage::USER_FORCE_IDLE);
}

TEST(EventMessage, StringConstructorUnknownType)
{
    EventMessage msg("1000000000", "BOGUS_TYPE");
    EXPECT_EQ(msg.m_event_type, EventMessage::UNKNOWN);
}

TEST(EventMessage, StringConstructorInvalidTimestamp)
{
    EXPECT_THROW(EventMessage("not_a_number", "USER_ACTIVE"), std::invalid_argument);
}

// ============================================================================
// EventTypeToString (member)
// ============================================================================

TEST(EventMessage, EventTypeToStringMember)
{
    EventMessage msg1(0, EventMessage::UNKNOWN);
    EXPECT_EQ(msg1.EventTypeToString(), "UNKNOWN");

    EventMessage msg2(0, EventMessage::USER_ACTIVE);
    EXPECT_EQ(msg2.EventTypeToString(), "USER_ACTIVE");

    EventMessage msg3(0, EventMessage::USER_UNFORCE);
    EXPECT_EQ(msg3.EventTypeToString(), "USER_UNFORCE");

    EventMessage msg4(0, EventMessage::USER_FORCE_ACTIVE);
    EXPECT_EQ(msg4.EventTypeToString(), "USER_FORCE_ACTIVE");

    EventMessage msg5(0, EventMessage::USER_FORCE_IDLE);
    EXPECT_EQ(msg5.EventTypeToString(), "USER_FORCE_IDLE");
}

// ============================================================================
// EventTypeToString (static)
// ============================================================================

TEST(EventMessage, EventTypeToStringStatic)
{
    EXPECT_EQ(EventMessage::EventTypeToString(EventMessage::UNKNOWN), "UNKNOWN");
    EXPECT_EQ(EventMessage::EventTypeToString(EventMessage::USER_ACTIVE), "USER_ACTIVE");
    EXPECT_EQ(EventMessage::EventTypeToString(EventMessage::USER_UNFORCE), "USER_UNFORCE");
    EXPECT_EQ(EventMessage::EventTypeToString(EventMessage::USER_FORCE_ACTIVE), "USER_FORCE_ACTIVE");
    EXPECT_EQ(EventMessage::EventTypeToString(EventMessage::USER_FORCE_IDLE), "USER_FORCE_IDLE");
}

// ============================================================================
// IsValid
// ============================================================================

TEST(EventMessage, IsValidWithCurrentTimestamp)
{
    EventMessage msg(GetUnixEpochTime(), EventMessage::USER_ACTIVE);
    EXPECT_TRUE(msg.IsValid());
}

TEST(EventMessage, IsInvalidWithUnknownType)
{
    EventMessage msg(GetUnixEpochTime(), EventMessage::UNKNOWN);
    EXPECT_FALSE(msg.IsValid());
}

TEST(EventMessage, IsInvalidWithBadTimestamp)
{
    EventMessage msg(0, EventMessage::USER_ACTIVE);
    EXPECT_FALSE(msg.IsValid());
}

TEST(EventMessage, IsInvalidDefault)
{
    EventMessage msg;
    EXPECT_FALSE(msg.IsValid());
}

TEST(EventMessage, IsValidAllNonUnknownTypes)
{
    int64_t ts = GetUnixEpochTime();

    EventMessage msg1(ts, EventMessage::USER_UNFORCE);
    EXPECT_TRUE(msg1.IsValid());

    EventMessage msg2(ts, EventMessage::USER_FORCE_ACTIVE);
    EXPECT_TRUE(msg2.IsValid());

    EventMessage msg3(ts, EventMessage::USER_FORCE_IDLE);
    EXPECT_TRUE(msg3.IsValid());
}

// ============================================================================
// ToString
// ============================================================================

TEST(EventMessage, ToStringFormat)
{
    EventMessage msg(1000000000, EventMessage::USER_ACTIVE);
    EXPECT_EQ(msg.ToString(), "1000000000:USER_ACTIVE");
}

TEST(EventMessage, ToStringAllTypes)
{
    EXPECT_EQ(EventMessage(100, EventMessage::UNKNOWN).ToString(), "100:UNKNOWN");
    EXPECT_EQ(EventMessage(100, EventMessage::USER_UNFORCE).ToString(), "100:USER_UNFORCE");
    EXPECT_EQ(EventMessage(100, EventMessage::USER_FORCE_ACTIVE).ToString(), "100:USER_FORCE_ACTIVE");
    EXPECT_EQ(EventMessage(100, EventMessage::USER_FORCE_IDLE).ToString(), "100:USER_FORCE_IDLE");
}

// ============================================================================
// Round-trip: construct from ToString output
// ============================================================================

TEST(EventMessage, RoundTrip)
{
    int64_t ts = GetUnixEpochTime();
    EventMessage original(ts, EventMessage::USER_ACTIVE);
    std::string serialized = original.ToString();

    // Parse back: split on ':'
    auto parts = StringSplit(serialized, ":");
    ASSERT_EQ(parts.size(), 2u);

    EventMessage reconstructed(parts[0], parts[1]);
    EXPECT_EQ(reconstructed.m_timestamp, original.m_timestamp);
    EXPECT_EQ(reconstructed.m_event_type, original.m_event_type);
    EXPECT_EQ(reconstructed.ToString(), serialized);
}

TEST(EventMessage, RoundTripAllTypes)
{
    int64_t ts = 1700000000;
    std::vector<EventMessage::EventType> types = {
        EventMessage::USER_ACTIVE,
        EventMessage::USER_UNFORCE,
        EventMessage::USER_FORCE_ACTIVE,
        EventMessage::USER_FORCE_IDLE
    };

    for (auto type : types) {
        EventMessage original(ts, type);
        std::string serialized = original.ToString();
        auto parts = StringSplit(serialized, ":");
        ASSERT_EQ(parts.size(), 2u);
        EventMessage reconstructed(parts[0], parts[1]);
        EXPECT_EQ(reconstructed.m_timestamp, ts);
        EXPECT_EQ(reconstructed.m_event_type, type);
    }
}
