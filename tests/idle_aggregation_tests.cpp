/*
 * Copyright (C) 2025-2026 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <idle_source.h>

using namespace IdleDetect;

//
// Sentinel wire contract
//
// Every other assertion in this file uses the symbolic constant on both sides of the
// comparison and is therefore blind to a change in the underlying value: setting IDLE_ERROR to
// -7 and IDLE_NO_GUI_SESSION to -9 would leave all of them passing. The three cases below are
// the only ones that pin the numbers themselves, so they compare against literals on purpose.
// The values are what a reader of a debug log sees, and what the daemon's behavior has always
// been described in terms of, so they are held fixed here even though idle_detect.cpp's main
// loop now compares against the names rather than the bare numbers.
//

TEST(IdleAggregation, ErrorSentinelIsMinusOne)
{
    EXPECT_EQ(IDLE_ERROR, -1);
    EXPECT_EQ(AggregateIdleSeconds({}, false, true), -1);
}

TEST(IdleAggregation, NoGuiSessionSentinelIsMinusTwo)
{
    EXPECT_EQ(IDLE_NO_GUI_SESSION, -2);
    EXPECT_EQ(AggregateIdleSeconds({}, false, false), -2);
}

TEST(IdleAggregation, SentinelsAreDistinct)
{
    // A caller that cannot tell "no GUI session at all" from "a source failed" cannot decide
    // whether to override use_event_detect.
    EXPECT_NE(IDLE_ERROR, IDLE_NO_GUI_SESSION);
}

//
// Sentinel selection
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

TEST(IdleAggregation, InhibitedTakesPrecedenceOverNoSourcePresent)
{
    // This input is expected to be unreachable in practice: inhibition is reported by a
    // shell, and a shell's presence means a source is present, so inhibited implies
    // any_source_present. The relative order of the two checks is load-bearing if that
    // invariant is ever broken, though, so it is pinned deliberately rather than left to
    // whichever branch happens to be written first.
    EXPECT_EQ(AggregateIdleSeconds({}, true, false), 0);
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
// This case is deliberately weak and is named for what it actually tests.
//
// The regression this design exists to prevent is a multi-endpoint one: a console session
// idle for twenty minutes alongside a VNC session active three seconds ago must report the
// active one, and a shell's inhibit state must not fuse with an unrelated endpoint's
// reading. AggregateIdleSeconds() takes a plain vector<int64_t> and structurally cannot
// express an endpoint, a shell, or a fallback chain, so that regression is not observable at
// this layer at all. What remains below is a second min() case whose winner sits in the last
// position; it shares its kill set with TakesMinimumOfResolvedValues and ZeroIsAValidMinimum,
// which the '<' -> '>' mutation kills together, so it has no independent kill power.
//
// The real multi-endpoint and shell-fusion regression test belongs at the IdleSourcePool
// layer, and lives there: see IdleSourcePool.MultipleEndpointsReportActiveOneDespiteIdleShell
// in tests/idle_source_pool_tests.cpp.
//

TEST(IdleAggregation, MinimumCanBeTheLastValue)
{
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
