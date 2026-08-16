/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <idle_source.h>

using namespace IdleDetect;

//
// Sentinel contract
//

TEST(IdleAggregation, NoSourcesAtAllReportsNoGuiSession)
{
    EXPECT_EQ(AggregateIdleSeconds({}, false, false), IDLE_NO_GUI_SESSION);
}

TEST(IdleAggregation, InhibitedShortCircuitsToZero)
{
    EXPECT_EQ(AggregateIdleSeconds({500, 900}, true, true), 0);
}

TEST(IdleAggregation, InhibitedWinsOverErrorSources)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR}, true, true), 0);
}

TEST(IdleAggregation, SourcesPresentButAllErrorReportsError)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR, IDLE_ERROR}, false, true), IDLE_ERROR);
}

TEST(IdleAggregation, SourcesPresentButNoneResolvedReportsError)
{
    EXPECT_EQ(AggregateIdleSeconds({}, false, true), IDLE_ERROR);
}

//
// min() behavior
//

TEST(IdleAggregation, TakesMinimumOfResolvedValues)
{
    EXPECT_EQ(AggregateIdleSeconds({900, 12, 500}, false, true), 12);
}

TEST(IdleAggregation, ZeroIsAValidMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({900, 0}, false, true), 0);
}

//
// Sentinels must never enter the minimum. min(-1, 300) would be -1, silently
// converting one source's error into a global error.
//

TEST(IdleAggregation, ErrorSentinelExcludedFromMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR, 300}, false, true), 300);
}

TEST(IdleAggregation, NoGuiSessionSentinelExcludedFromMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_NO_GUI_SESSION, 300}, false, true), 300);
}

TEST(IdleAggregation, BothSentinelsExcludedTogether)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_NO_GUI_SESSION, IDLE_ERROR, 42}, false, true), 42);
}

//
// The multi-endpoint regression this design exists to prevent: an active
// endpoint alongside an idle one must report the active one.
//

TEST(IdleAggregation, ActiveEndpointBeatsIdleEndpoint)
{
    // Console idle for 20 minutes, VNC session active 3 seconds ago.
    EXPECT_EQ(AggregateIdleSeconds({1200, 3}, false, true), 3);
}

//
// Endpoint value semantics
//

TEST(Endpoint, OrdersByKindThenIdentifier)
{
    Endpoint wayland{EndpointKind::WAYLAND, "wayland-0"};
    Endpoint x11_one{EndpointKind::X11, ":1"};
    Endpoint x11_two{EndpointKind::X11, ":2"};

    EXPECT_LT(wayland, x11_one);
    EXPECT_LT(x11_one, x11_two);
}

TEST(Endpoint, EqualityComparesKindAndIdentifier)
{
    EXPECT_EQ((Endpoint{EndpointKind::X11, ":1"}), (Endpoint{EndpointKind::X11, ":1"}));
    EXPECT_FALSE((Endpoint{EndpointKind::X11, ":1"}) == (Endpoint{EndpointKind::WAYLAND, ":1"}));
}

TEST(Endpoint, ToStringIsHumanReadable)
{
    EXPECT_EQ((Endpoint{EndpointKind::WAYLAND, "wayland-0"}).ToString(), "wayland:wayland-0");
    EXPECT_EQ((Endpoint{EndpointKind::X11, ":1"}).ToString(), "x11::1");
}
