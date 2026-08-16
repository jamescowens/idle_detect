/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <idle_source_pool.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace IdleDetect;

//
// These tests exist because the multi-endpoint regression this whole design prevents is not
// observable one layer down. AggregateIdleSeconds() takes a plain vector<int64_t> and structurally
// cannot express an endpoint, a shell, or a fallback chain, so "the shell's idle time must not
// stand in for an endpoint's" cannot even be stated there. It can be stated here, and
// MultipleEndpointsReportActiveOneDespiteIdleShell is that statement.
//
// The pool reaches system state only through injected factories, which is what lets this file
// exist: real WaylandIdleSource/X11IdleSource/ShellIdleSource construction needs D-Bus, X11 and
// libwayland, none of which idle_detect_tests links. The fakes below stand in for them and report
// scripted values, so every reconciliation, eviction, retry and aggregation path is exercised
// against known inputs.
//

namespace {

//!
//! \brief Idle source returning a scripted value, recording its resolves, and reporting its own
//! destruction through a caller-supplied callback.
//!
//! The destruction callback is how eviction is asserted. Eviction has no return value and no
//! observable side effect other than the source being torn down, and "torn down" is exactly what
//! matters: a Wayland source that is dropped from the map but not destroyed keeps its compositor
//! thread alive. A callback rather than a flag, so that endpoint sources can report per-endpoint
//! while shell sources, which are replaced in place, can be counted.
//!
class FakeIdleSource : public IdleSource
{
public:
    FakeIdleSource(std::string description, int64_t value, std::function<void()> on_destroy)
        : m_description(std::move(description))
        , m_value(value)
        , m_alive(true)
        , m_resolve_count(0)
        , m_on_destroy(std::move(on_destroy))
    {}

    ~FakeIdleSource() override
    {
        if (m_on_destroy) {
            m_on_destroy();
        }
    }

    FakeIdleSource(const FakeIdleSource&) = delete;
    FakeIdleSource& operator=(const FakeIdleSource&) = delete;

    int64_t ResolveIdleSeconds() override
    {
        ++m_resolve_count;

        return m_value;
    }

    bool IsAlive() const override
    {
        return m_alive;
    }

    std::string Describe() const override
    {
        return m_description;
    }

    //! \brief Changes what this source reports from the next resolve onwards.
    void SetValue(int64_t value)
    {
        m_value = value;
    }

    //!
    //! \brief Kills or revives this source, standing in for a compositor hangup that leaves the
    //! endpoint's socket in place while the monitor behind it stops working.
    //!
    void SetAlive(bool alive)
    {
        m_alive = alive;
    }

    //! \brief Number of times the pool has resolved this source.
    int ResolveCount() const
    {
        return m_resolve_count;
    }

private:
    std::string m_description;
    int64_t m_value;
    bool m_alive;
    int m_resolve_count;
    std::function<void()> m_on_destroy;
};

//!
//! \brief Shell source fake. Also implements ShellSourceContext, so the tests can assert that the
//! pool pushes the live-Wayland-endpoint fact that only it knows.
//!
class FakeShellSource : public FakeIdleSource, public ShellSourceContext
{
public:
    FakeShellSource(ShellKind kind, int64_t value, std::function<void()> on_destroy)
        : FakeIdleSource(kind == ShellKind::KDE ? "shell:kde" : "shell:gnome", value, std::move(on_destroy))
        , m_kind(kind)
        , m_wayland_endpoint_present(false)
        , m_context_updates(0)
    {}

    void SetWaylandEndpointPresent(bool present) override
    {
        m_wayland_endpoint_present = present;
        ++m_context_updates;
    }

    ShellKind Kind() const
    {
        return m_kind;
    }

    bool WaylandEndpointPresent() const
    {
        return m_wayland_endpoint_present;
    }

    int ContextUpdates() const
    {
        return m_context_updates;
    }

private:
    ShellKind m_kind;
    bool m_wayland_endpoint_present;
    int m_context_updates;
};

//!
//! \brief Scripted factories plus the pool they drive.
//!
//! An endpoint is "real" only if a value has been scripted for it; anything else makes the factory
//! return nullptr, which is how a candidate that fails validation is modelled. The pool is declared
//! last so that it is destroyed first, while the bookkeeping its sources report destruction into is
//! still alive.
//!
class PoolHarness
{
public:
    PoolHarness()
        : m_shell_value(IDLE_ERROR)
        , m_shell_factory_declines(false)
        , m_shell_source(nullptr)
        , m_shell_destructions(0)
        , m_live_shell_sources(0)
        , m_max_live_shell_sources(0)
        , m_inhibited(false)
        , m_pool([this](const Endpoint& endpoint) { return MakeEndpointSource(endpoint); },
                 [this](ShellKind kind) { return MakeShellSource(kind); },
                 [this](ShellKind kind) { return QueryInhibition(kind); })
    {}

    IdleSourcePool& Pool()
    {
        return m_pool;
    }

    //! \brief Makes an endpoint validate, reporting the given idle seconds.
    void ScriptEndpoint(const Endpoint& endpoint, int64_t value)
    {
        m_endpoint_values[endpoint] = value;
    }

    //! \brief Makes an endpoint fail validation again, as a compositor that has gone away would.
    void ClearEndpointScript(const Endpoint& endpoint)
    {
        m_endpoint_values.erase(endpoint);
    }

    //! \brief Live source for an endpoint, or nullptr if there is none. Never dangling: a source
    //! deregisters itself as it is destroyed.
    FakeIdleSource* EndpointSource(const Endpoint& endpoint) const
    {
        auto iter = m_endpoint_sources.find(endpoint);

        return (iter == m_endpoint_sources.end()) ? nullptr : iter->second;
    }

    bool EndpointDestroyed(const Endpoint& endpoint) const
    {
        auto iter = m_endpoint_destroyed.find(endpoint);

        return (iter == m_endpoint_destroyed.end()) ? false : iter->second;
    }

    //!
    //! \brief How many sources for this endpoint have been destroyed over the pool's life.
    //!
    //! EndpointDestroyed() cannot answer the rebuild case: the replacement source registers itself as
    //! not-destroyed, so a teardown immediately followed by a rebuild leaves that flag false. A count
    //! is what distinguishes "torn down and rebuilt" from "never touched".
    //!
    int EndpointDestructions(const Endpoint& endpoint) const
    {
        auto iter = m_endpoint_destructions.find(endpoint);

        return (iter == m_endpoint_destructions.end()) ? 0 : iter->second;
    }

    int EndpointFactoryCalls(const Endpoint& endpoint) const
    {
        auto iter = m_endpoint_factory_calls.find(endpoint);

        return (iter == m_endpoint_factory_calls.end()) ? 0 : iter->second;
    }

    void SetShellValue(int64_t value)
    {
        m_shell_value = value;
    }

    void SetShellFactoryDeclines(bool declines)
    {
        m_shell_factory_declines = declines;
    }

    FakeShellSource* ShellSource() const
    {
        return m_shell_source;
    }

    //! \brief How many shell sources have been destroyed over the pool's life.
    int ShellDestructions() const
    {
        return m_shell_destructions;
    }

    //!
    //! \brief High-water mark of simultaneously live shell sources. There is one shell, so a
    //! replacement must release its predecessor before its successor is built.
    //!
    int MaxLiveShellSources() const
    {
        return m_max_live_shell_sources;
    }

    const std::vector<ShellKind>& ShellFactoryKinds() const
    {
        return m_shell_factory_kinds;
    }

    void SetInhibited(bool inhibited)
    {
        m_inhibited = inhibited;
    }

    const std::vector<ShellKind>& InhibitionKinds() const
    {
        return m_inhibition_kinds;
    }

private:
    std::unique_ptr<IdleSource> MakeEndpointSource(const Endpoint& endpoint)
    {
        ++m_endpoint_factory_calls[endpoint];

        auto script = m_endpoint_values.find(endpoint);

        if (script == m_endpoint_values.end()) {
            return nullptr;
        }

        m_endpoint_destroyed[endpoint] = false;

        auto source = std::make_unique<FakeIdleSource>(endpoint.ToString(),
                                                       script->second,
                                                       [this, endpoint]() { OnEndpointDestroyed(endpoint); });

        m_endpoint_sources[endpoint] = source.get();

        return source;
    }

    std::unique_ptr<IdleSource> MakeShellSource(ShellKind kind)
    {
        m_shell_factory_kinds.push_back(kind);

        if (kind == ShellKind::NONE || m_shell_factory_declines) {
            return nullptr;
        }

        auto source = std::make_unique<FakeShellSource>(kind, m_shell_value, [this]() { OnShellDestroyed(); });

        ++m_live_shell_sources;

        if (m_live_shell_sources > m_max_live_shell_sources) {
            m_max_live_shell_sources = m_live_shell_sources;
        }

        m_shell_source = source.get();

        return source;
    }

    void OnEndpointDestroyed(const Endpoint& endpoint)
    {
        m_endpoint_destroyed[endpoint] = true;
        ++m_endpoint_destructions[endpoint];

        // Deregistered rather than left behind, so EndpointSource() can never hand a test a pointer
        // to a source the pool has already torn down.
        m_endpoint_sources.erase(endpoint);
    }

    void OnShellDestroyed()
    {
        ++m_shell_destructions;
        --m_live_shell_sources;
        m_shell_source = nullptr;
    }

    bool QueryInhibition(ShellKind kind)
    {
        m_inhibition_kinds.push_back(kind);

        return m_inhibited && kind != ShellKind::NONE;
    }

    std::map<Endpoint, int64_t> m_endpoint_values;
    std::map<Endpoint, FakeIdleSource*> m_endpoint_sources;
    std::map<Endpoint, bool> m_endpoint_destroyed;
    std::map<Endpoint, int> m_endpoint_destructions;
    std::map<Endpoint, int> m_endpoint_factory_calls;

    int64_t m_shell_value;
    bool m_shell_factory_declines;
    FakeShellSource* m_shell_source;
    int m_shell_destructions;
    int m_live_shell_sources;
    int m_max_live_shell_sources;
    std::vector<ShellKind> m_shell_factory_kinds;

    bool m_inhibited;
    std::vector<ShellKind> m_inhibition_kinds;

    IdleSourcePool m_pool;
};

const Endpoint g_wayland_zero{EndpointKind::WAYLAND, "wayland-0"};
const Endpoint g_x11_one{EndpointKind::X11, ":1"};
const Endpoint g_x11_two{EndpointKind::X11, ":2"};

//!
//! \brief Runs the given number of reconcile ticks against an unchanging candidate set and shell.
//! \param harness harness owning the pool
//! \param ticks number of reconciles to run
//! \param endpoints candidate set offered on each tick
//!
void ReconcileTicks(PoolHarness& harness, int ticks, const std::set<Endpoint>& endpoints)
{
    for (int tick = 0; tick < ticks; ++tick) {
        harness.Pool().Reconcile(endpoints, ShellKind::NONE);
    }
}

//!
//! \brief Runs the given number of reconcile ticks against an unchanging candidate set and a shell that
//! stays present.
//!
//! Separate from ReconcileTicks() rather than a defaulted argument on it, because the two are used for
//! opposite purposes: the endpoint tests want a shell that is out of the way, while the shell-context tests
//! need one that survives the run, and a shell observed as NONE for three consecutive ticks is dropped.
//!
//! \param harness harness owning the pool
//! \param ticks number of reconciles to run
//! \param endpoints candidate set offered on each tick
//! \param shell shell kind observed on each tick
//!
void ReconcileTicksWithShell(PoolHarness& harness,
                             int ticks,
                             const std::set<Endpoint>& endpoints,
                             ShellKind shell)
{
    for (int tick = 0; tick < ticks; ++tick) {
        harness.Pool().Reconcile(endpoints, shell);
    }
}

//!
//! \brief Reconciles until the endpoint is offered to the factory again, and reports how many ticks
//! that took.
//!
//! The count includes the tick on which the offer happened, so an endpoint offered on every tick
//! returns 1 and an endpoint serving an N-tick backoff returns N + 1. Written as a search rather
//! than as an absolute tick number because the ladder's later rungs are 32 and 64 ticks long, and
//! spelling those out as running totals would obscure the very intervals the test is about.
//!
//! \param harness harness owning the pool
//! \param endpoint endpoint to watch
//! \param endpoints candidate set offered on each tick
//! \param limit tick ceiling, guarding against a hang if the backoff never elapses
//! \return ticks consumed, or -1 if the limit was reached without an offer
//!
int TicksUntilNextFactoryCall(PoolHarness& harness,
                              const Endpoint& endpoint,
                              const std::set<Endpoint>& endpoints,
                              int limit = 1000)
{
    const int calls_before = harness.EndpointFactoryCalls(endpoint);

    for (int ticks = 1; ticks <= limit; ++ticks) {
        harness.Pool().Reconcile(endpoints, ShellKind::NONE);

        if (harness.EndpointFactoryCalls(endpoint) > calls_before) {
            return ticks;
        }
    }

    return -1;
}

} // anonymous namespace

//
// The multi-endpoint regression.
//

TEST(IdleSourcePool, MultipleEndpointsReportActiveOneDespiteIdleShell)
{
    // A console session idle for twenty minutes, a VNC session active three seconds ago, and a
    // desktop shell that has been idle for twenty-five minutes because it is the console's shell.
    // The user is demonstrably at the keyboard, so the pool must say three seconds.
    //
    // Every way of getting this wrong produces 1200 or 1500: fusing the shell into each endpoint's
    // chain, letting the shell win because it was resolved last, or taking a maximum. Only keeping
    // the sources independent and taking the minimum produces 3.
    PoolHarness harness;

    harness.ScriptEndpoint(g_wayland_zero, 3);
    harness.ScriptEndpoint(g_x11_one, 1200);
    harness.SetShellValue(1500);

    harness.Pool().Reconcile({g_wayland_zero, g_x11_one}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 2u);
    ASSERT_TRUE(harness.Pool().HasShellSource());

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 3);

    // Both endpoints and the shell were consulted, so the answer is a real aggregate rather than a
    // short circuit that happened to land on the right number.
    EXPECT_EQ(harness.EndpointSource(g_wayland_zero)->ResolveCount(), 1);
    EXPECT_EQ(harness.EndpointSource(g_x11_one)->ResolveCount(), 1);
    EXPECT_EQ(harness.ShellSource()->ResolveCount(), 1);
}

TEST(IdleSourcePool, ActiveEndpointStillWinsAfterTheIdleOneIsAddedLater)
{
    // Same regression from the other direction: the idle endpoint arrives second, so a bug that
    // let the most recently added source overwrite the aggregate would show up here and not above.
    PoolHarness harness;

    harness.ScriptEndpoint(g_wayland_zero, 5);
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);
    ASSERT_EQ(harness.Pool().GetIdleSeconds(), 5);

    harness.ScriptEndpoint(g_x11_one, 900);
    harness.Pool().Reconcile({g_wayland_zero, g_x11_one}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 5);
}

TEST(IdleSourcePool, OneEndpointErrorDoesNotMaskAnotherEndpointsReading)
{
    // A dead display must not convert a live one's reading into a global error.
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, IDLE_ERROR);
    harness.ScriptEndpoint(g_x11_two, 42);

    harness.Pool().Reconcile({g_x11_one, g_x11_two}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 42);
}

//
// Inhibition.
//

TEST(IdleSourcePool, InhibitionShortCircuitsToZero)
{
    // Inhibition means "do not let this machine go idle" and outranks every reading, however large.
    PoolHarness harness;

    harness.ScriptEndpoint(g_wayland_zero, 3600);
    harness.ScriptEndpoint(g_x11_one, 7200);
    harness.SetShellValue(9000);
    harness.SetInhibited(true);

    harness.Pool().Reconcile({g_wayland_zero, g_x11_one}, ShellKind::GNOME);

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 0);
}

TEST(IdleSourcePool, InhibitionIsQueriedWithTheDetectedShellKind)
{
    // The query is per-shell: KDE and GNOME answer through entirely different bus interfaces, so
    // handing it the wrong kind silently returns "not inhibited" forever.
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, 100);
    harness.Pool().Reconcile({g_x11_one}, ShellKind::GNOME);
    harness.Pool().GetIdleSeconds();

    ASSERT_EQ(harness.InhibitionKinds().size(), 1u);
    EXPECT_EQ(harness.InhibitionKinds().front(), ShellKind::GNOME);
}

TEST(IdleSourcePool, InhibitionStillAppliesWhenTheShellSourceCouldNotBeCreated)
{
    // A GNOME shell without mutter has no idle value, so its factory may decline. It still inhibits:
    // inhibition is answered by gnome-session, not by the window manager, and losing it here would
    // resume the compute the user explicitly asked to hold off.
    PoolHarness harness;

    harness.SetShellFactoryDeclines(true);
    harness.SetInhibited(true);
    harness.ScriptEndpoint(g_x11_one, 5000);

    harness.Pool().Reconcile({g_x11_one}, ShellKind::GNOME);

    ASSERT_FALSE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 0);
    ASSERT_EQ(harness.InhibitionKinds().size(), 1u);
    EXPECT_EQ(harness.InhibitionKinds().front(), ShellKind::GNOME);
}

//
// Sentinels.
//

TEST(IdleSourcePool, NoEndpointsAndNoShellReportsNoGuiSession)
{
    // This is the tty and headless case, and it must be distinguishable from a failure: the main
    // loop overrides use_event_detect on IDLE_NO_GUI_SESSION and does not on IDLE_ERROR.
    PoolHarness harness;

    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
    EXPECT_NE(harness.Pool().GetIdleSeconds(), IDLE_ERROR);
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_FALSE(harness.Pool().HasShellSource());
}

TEST(IdleSourcePool, EndpointsPresentButAllErrorReportsError)
{
    // Sources exist, so this is a failure to read rather than an absence of a GUI session, and the
    // main loop must not silently fall back to event_detect on it.
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, IDLE_ERROR);
    harness.ScriptEndpoint(g_x11_two, IDLE_ERROR);

    harness.Pool().Reconcile({g_x11_one, g_x11_two}, ShellKind::NONE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 2u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_ERROR);
    EXPECT_NE(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, ShellAloneCountsAsASource)
{
    // KDE on X11 with no endpoint the pool could validate. The shell is a source in its own right,
    // so its value is the aggregate -- not IDLE_NO_GUI_SESSION, which would hand the session to
    // event_detect while a perfectly good ksmserver reading was available.
    PoolHarness harness;

    harness.SetShellValue(700);

    harness.Pool().Reconcile({}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    ASSERT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 700);
}

//
// Endpoint lifecycle.
//

TEST(IdleSourcePool, FailedCandidateIsNotAddedAndIsRetriedOnALaterReconcile)
{
    // Discovery is deliberately over-inclusive, so a candidate that does not validate is normal.
    // Caching that failure as a verdict is the bug: a compositor that is still starting up would
    // then be locked out for the life of the daemon.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // Still failing on the retry: still offered, still not added. The retry is one tick later than
    // the failure rather than on the very next tick, because a first failure buys a one-tick
    // backoff; see the ladder tests below.
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // The compositor finishes coming up. It is picked up on the next offer, which is what makes the
    // backoff a delay rather than the lockout it is guarding against.
    harness.ScriptEndpoint(g_wayland_zero, 17);

    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 3);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 17);

    // And having succeeded, it is not rebuilt on the tick after that.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);
    EXPECT_FALSE(harness.EndpointDestroyed(g_wayland_zero));
}

//
// Validation backoff.
//
// The defect these pin down is a permanently-bad candidate -- a stale Wayland socket with no
// listener, or any Wayland candidate at all in a GNOME session, which advertises no
// ext_idle_notifier_v1 -- being rebuilt, restarted and re-logged on every single reconcile tick
// forever. Measured at 87 journal lines in 8 seconds with debug logging off, which is roughly
// 604,000 lines a day from one dead socket.
//

TEST(IdleSourcePool, RepeatedValidationFailuresAreBackedOffRatherThanRetriedEveryTick)
{
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    // Tick 1: offered, fails. One tick of backoff.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // Tick 2: deferred. This is the assertion the whole fix is about -- the factory is not called,
    // so nothing is built, nothing is started against a socket nobody is listening on, and nothing
    // is logged about it.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // Tick 3: interval elapsed, offered again, fails again. Interval doubles to two ticks.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // Ticks 4 and 5: deferred.
    ReconcileTicks(harness, 2, candidates);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // Tick 6: offered, fails. Interval doubles to four ticks.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // Ticks 7 through 10: deferred.
    ReconcileTicks(harness, 4, candidates);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // Tick 11: offered.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 4);

    // Eleven ticks, four validation attempts. Without the backoff it would be eleven.
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
}

TEST(IdleSourcePool, BackoffIntervalDoublesAndThenHoldsAtItsCeiling)
{
    // The ladder in full, and the ceiling it stops at. Unbounded doubling would eventually stop
    // retrying a candidate in any useful sense at all, which is the failure mode on the other side
    // of the one being fixed: a compositor that comes up an hour after the daemon did must still be
    // picked up within a bounded time.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // Each value is the interval just earned, plus the tick the retry itself lands on.
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 1 + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2 + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 4 + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 8 + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 16 + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 32 + 1);

    // Capped from here on rather than continuing to 128.
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates),
              ENDPOINT_RETRY_BACKOFF_MAX_TICKS + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates),
              ENDPOINT_RETRY_BACKOFF_MAX_TICKS + 1);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates),
              ENDPOINT_RETRY_BACKOFF_MAX_TICKS + 1);
}

TEST(IdleSourcePool, SuccessfulValidationClearsTheBackoffLadder)
{
    // A candidate that finally comes up must not carry the interval it earned while it was down.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.Pool().Reconcile(candidates, ShellKind::NONE);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // Two failures deep, so the ladder stands at two ticks. The compositor comes up and the next
    // offer succeeds.
    harness.ScriptEndpoint(g_wayland_zero, 11);

    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 3);
    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 11);

    // A live source is left alone, so no further offers are made while it survives.
    ReconcileTicks(harness, 5, candidates);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // The compositor goes away, and comes back as a candidate that is failing again. The first
    // failure after the success buys one tick, not the two the ladder stood at before it, which is
    // what "cleared" means here.
    harness.Pool().Reconcile({}, ShellKind::NONE);
    harness.ClearEndpointScript(g_wayland_zero);

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 4);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
}

TEST(IdleSourcePool, LeavingAndReenteringTheCandidateSetClearsTheBackoff)
{
    // Leaving the candidate set is the pool's only evidence that the thing behind the endpoint has
    // changed. A socket that is replugged, or a compositor restarted, therefore gets a full-speed
    // attempt on the tick it reappears rather than serving out an interval earned by its
    // predecessor -- which at the ceiling would be a 64-tick wait for a session that is up now.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.Pool().Reconcile(candidates, ShellKind::NONE);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 3);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // Three failures deep, so the ladder stands at four ticks. The endpoint disappears while that
    // interval is still running.
    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // It comes back, and is offered on the very tick it returns rather than after the remainder of
    // an interval it is no longer serving.
    harness.ScriptEndpoint(g_wayland_zero, 21);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 4);
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 21);
}

TEST(IdleSourcePool, OneCandidatesBackoffDoesNotDelayAnother)
{
    // The ladder is per endpoint. A dead Wayland socket sitting at the bottom of its ladder must not
    // hold back the X display that appears next to it, which is the multi-endpoint case the whole
    // pool exists for.
    PoolHarness harness;

    const std::set<Endpoint> wayland_only{g_wayland_zero};

    harness.Pool().Reconcile(wayland_only, ShellKind::NONE);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, wayland_only), 2);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, wayland_only), 3);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // The Wayland candidate is three failures deep and serving a four-tick interval. A live X
    // display appears and is validated on the very tick it is first offered.
    harness.ScriptEndpoint(g_x11_one, 64);
    harness.Pool().Reconcile({g_wayland_zero, g_x11_one}, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_x11_one), 1);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 64);
}

TEST(IdleSourcePool, EndpointDisappearingDestroysItsSource)
{
    // Dropping the source from the map without destroying it would leave a Wayland monitor thread
    // running against a compositor that is gone, and eviction is what stops it.
    PoolHarness harness;

    harness.ScriptEndpoint(g_wayland_zero, 8);
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    ASSERT_FALSE(harness.EndpointDestroyed(g_wayland_zero));

    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_TRUE(harness.EndpointDestroyed(g_wayland_zero));
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);

    // The evicted endpoint's reading is gone from the aggregate too, rather than frozen at 8.
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, UnrelatedChurnPreservesSurvivingSourceIdentity)
{
    // Add A, add B, drop A, keep B. B must be the same object throughout: rebuilding it would drop
    // and re-establish a working compositor connection every time some other endpoint appeared.
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, 100);
    harness.Pool().Reconcile({g_x11_one}, ShellKind::NONE);

    harness.ScriptEndpoint(g_wayland_zero, 200);
    harness.Pool().Reconcile({g_x11_one, g_wayland_zero}, ShellKind::NONE);

    FakeIdleSource* wayland_source = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(wayland_source, nullptr);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_x11_one), 1);
    EXPECT_FALSE(harness.EndpointDestroyed(g_x11_one));

    // Mutate the live object. A rebuilt source would come back from the factory reporting the
    // scripted 200 instead, so this pins identity by behavior and not just by pointer value.
    wayland_source->SetValue(6);

    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    EXPECT_TRUE(harness.EndpointDestroyed(g_x11_one));
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.EndpointSource(g_wayland_zero), wayland_source);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 6);
}

//
// Liveness of retained sources.
//
// The defect these pin down is Reconcile() treating "the endpoint key is still in the candidate set"
// as proof that the source behind it works. A Wayland socket outlives the compositor's ability to
// serve it, so a monitor whose thread has exited leaves the candidate looking exactly as it did when
// it was healthy. Retained on key alone, such a source is kept forever: it can only return
// IDLE_ERROR, and its existence keeps any_source_present true, so IDLE_NO_GUI_SESSION can never fire
// and the daemon neither reads the compositor nor falls back to event_detect.
//

TEST(IdleSourcePool, DeadRetainedSourceIsTornDownAndRebuilt)
{
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.ScriptEndpoint(g_wayland_zero, 12);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    FakeIdleSource* original = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // The compositor hangs up and the monitor thread exits. The socket is untouched, so discovery goes
    // on offering the same candidate: nothing in the endpoint set can distinguish this from health.
    //
    // The dead source is left reporting a distinctive value, so that a pool which kept it would be
    // caught by the aggregate below rather than only by a pointer comparison.
    original->SetValue(999);
    original->SetAlive(false);

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointDestructions(g_wayland_zero), 1);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);
    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);

    FakeIdleSource* rebuilt = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_TRUE(rebuilt->IsAlive());

    // The rebuilt source reports the scripted value. 999 here would mean the dead one survived.
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 12);
}

TEST(IdleSourcePool, LiveRetainedSourceIsNeverRebuilt)
{
    // The other half of the rule. Liveness must not become an excuse to rebuild a working source,
    // which would drop and re-establish a compositor connection on every tick.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.ScriptEndpoint(g_wayland_zero, 12);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    FakeIdleSource* source = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(source, nullptr);

    // Mutated, so a rebuilt source would be caught by behavior and not only by pointer identity.
    source->SetValue(77);

    ReconcileTicks(harness, 10, candidates);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);
    EXPECT_EQ(harness.EndpointDestructions(g_wayland_zero), 0);
    EXPECT_EQ(harness.EndpointSource(g_wayland_zero), source);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 77);
}

TEST(IdleSourcePool, DeadSourceThatCannotBeRebuiltStopsCountingAsASource)
{
    // This is the whole point of the fix. A compositor that is gone for good, behind a socket that
    // lingers, must stop suppressing IDLE_NO_GUI_SESSION -- otherwise the daemon reports a GUI session
    // it cannot read, and the main loop never hands the session to event_detect.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.ScriptEndpoint(g_wayland_zero, 12);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    FakeIdleSource* source = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(source, nullptr);
    ASSERT_EQ(harness.Pool().GetIdleSeconds(), 12);

    // The compositor dies and does not come back. The stale socket keeps the candidate alive.
    source->SetAlive(false);
    harness.ClearEndpointScript(g_wayland_zero);

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointDestructions(g_wayland_zero), 1);
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);

    // Retaining the dead source would report 12 forever. Retaining it as a mere presence would report
    // IDLE_ERROR forever. Both mask a session that has no readable idle source at all.
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
    EXPECT_NE(harness.Pool().GetIdleSeconds(), IDLE_ERROR);
}

TEST(IdleSourcePool, FailedRebuildOfADeadSourceIsBackedOff)
{
    // A dead compositor behind a lingering socket is a permanently-failing candidate like any other,
    // so the rebuild goes through the same ladder rather than reconnecting on every tick.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.ScriptEndpoint(g_wayland_zero, 12);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    FakeIdleSource* source = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(source, nullptr);

    source->SetAlive(false);
    harness.ClearEndpointScript(g_wayland_zero);

    // Teardown and the failed rebuild happen on the same tick: the endpoint is newly-seen again by
    // the time the add pass runs.
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // One tick of backoff, then the retry.
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 3);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 4);
}

TEST(IdleSourcePool, SuccessfulRebuildResetsTheBackoff)
{
    // The strict form of "success resets the ladder": the endpoint never leaves the candidate set, so
    // the reset can only have come from the successful validation itself.
    PoolHarness harness;

    const std::set<Endpoint> candidates{g_wayland_zero};

    harness.ScriptEndpoint(g_wayland_zero, 12);
    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    FakeIdleSource* source = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(source, nullptr);

    // The compositor dies, and the rebuild fails twice, taking the ladder to two ticks.
    source->SetAlive(false);
    harness.ClearEndpointScript(g_wayland_zero);

    harness.Pool().Reconcile(candidates, ShellKind::NONE);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);
    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
    ASSERT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);

    // The compositor comes back and the next attempt succeeds.
    harness.ScriptEndpoint(g_wayland_zero, 5);

    ASSERT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 3);
    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    ASSERT_EQ(harness.Pool().GetIdleSeconds(), 5);

    // It dies again. The rebuild attempt is made on that very tick rather than being deferred by the
    // two-tick interval the ladder stood at before the success.
    FakeIdleSource* rebuilt = harness.EndpointSource(g_wayland_zero);
    ASSERT_NE(rebuilt, nullptr);

    rebuilt->SetAlive(false);
    harness.ClearEndpointScript(g_wayland_zero);

    harness.Pool().Reconcile(candidates, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 5);
    EXPECT_EQ(harness.EndpointDestructions(g_wayland_zero), 2);

    // And the ladder restarts at one tick rather than resuming at two.
    EXPECT_EQ(TicksUntilNextFactoryCall(harness, g_wayland_zero, candidates), 2);
}

//
// Shell lifecycle.
//

TEST(IdleSourcePool, ShellKindTransitionsCreateReplaceAndDrop)
{
    PoolHarness harness;

    harness.SetShellValue(300);

    // NONE: no shell source, and the factory is not called for a shell that does not exist.
    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_FALSE(harness.Pool().HasShellSource());
    EXPECT_TRUE(harness.ShellFactoryKinds().empty());

    // NONE -> KDE: created.
    harness.Pool().Reconcile({}, ShellKind::KDE);

    ASSERT_TRUE(harness.Pool().HasShellSource());
    FakeShellSource* kde_source = harness.ShellSource();
    ASSERT_NE(kde_source, nullptr);
    EXPECT_EQ(kde_source->Kind(), ShellKind::KDE);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 300);
    ASSERT_EQ(harness.ShellFactoryKinds().size(), 1u);
    EXPECT_EQ(harness.ShellFactoryKinds().back(), ShellKind::KDE);

    // KDE -> KDE: left alone. The shell source is stateful in production and rebuilding it every
    // tick would discard that state for nothing.
    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_EQ(harness.ShellFactoryKinds().size(), 1u);
    EXPECT_EQ(harness.ShellSource(), kde_source);
    EXPECT_EQ(harness.ShellDestructions(), 0);

    // KDE -> GNOME: replaced, old one destroyed. SetKind() cannot be used through the abstract
    // IdleSource interface, and a KDE source left answering for a GNOME shell would query
    // ksmserver on a machine where nothing owns that name.
    //
    // The replacement is asserted by destruction count and reported kind rather than by pointer
    // identity: the old source is released before the new one is built, so the allocator is quite
    // likely to hand back the same address.
    harness.SetShellValue(450);
    harness.Pool().Reconcile({}, ShellKind::GNOME);

    ASSERT_EQ(harness.ShellFactoryKinds().size(), 2u);
    EXPECT_EQ(harness.ShellFactoryKinds().back(), ShellKind::GNOME);
    ASSERT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 1);
    ASSERT_NE(harness.ShellSource(), nullptr);
    EXPECT_EQ(harness.ShellSource()->Kind(), ShellKind::GNOME);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 450);

    // There is one shell, so the predecessor is released before the successor is built rather than
    // the two overlapping.
    EXPECT_EQ(harness.MaxLiveShellSources(), 1);

    // GNOME -> NONE: dropped and destroyed, once the absence has been observed often enough to be
    // acted on. See the debounce tests below for why disappearance is not acted on immediately.
    ReconcileTicks(harness, SHELL_ABSENCE_OBSERVATIONS_REQUIRED, {});

    EXPECT_FALSE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 2);
    EXPECT_EQ(harness.ShellFactoryKinds().size(), 2u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, DecliningShellFactoryIsRetriedOnALaterReconcile)
{
    // Same non-caching rule as endpoints: a shell whose source could not be built this tick must
    // get another chance, not be written off.
    PoolHarness harness;

    harness.SetShellFactoryDeclines(true);
    harness.SetShellValue(600);

    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_FALSE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellFactoryKinds().size(), 1u);

    harness.SetShellFactoryDeclines(false);
    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellFactoryKinds().size(), 2u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 600);
}

TEST(IdleSourcePool, WaylandEndpointPresenceIsPushedToTheShellSource)
{
    // The shell source cannot see the endpoint set, and the answer changes while the daemon runs,
    // so the pool has to tell it. Getting this wrong makes a Plasma 6 Wayland session spend a D-Bus
    // timeout per tick on a ksmserver method that was removed.
    PoolHarness harness;

    harness.SetShellValue(50);
    harness.ScriptEndpoint(g_x11_one, 100);

    harness.Pool().Reconcile({g_x11_one}, ShellKind::KDE);

    ASSERT_NE(harness.ShellSource(), nullptr);
    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());
    EXPECT_GT(harness.ShellSource()->ContextUpdates(), 0);

    harness.ScriptEndpoint(g_wayland_zero, 100);
    harness.Pool().Reconcile({g_x11_one, g_wayland_zero}, ShellKind::KDE);

    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());

    // The compositor goes away. The flag goes back down rather than latching, so a KDE shell that
    // survives its Wayland endpoint resumes asking ksmserver for a value.
    harness.Pool().Reconcile({g_x11_one}, ShellKind::KDE);

    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());
}

TEST(IdleSourcePool, AWaylandCandidateThatFailsValidationStillMarksTheShellAsWayland)
{
    // The defect this pins down, and the reason the flag is derived from candidates rather than from live
    // sources. "Is this a Wayland session?" and "do we have a working Wayland idle source?" are different
    // questions, and only the first one is the shell's.
    //
    // A Wayland candidate that never validates is the ORDINARY case, not an exotic one: the compositor may
    // not advertise ext_idle_notifier_v1, the candidate may be serving out a validation backoff, or the
    // source may have died and not yet been rebuilt. Reading any of those as "not a Wayland session" put a
    // Plasma 6 Wayland shell on the X11 arm, where it called a D-Bus method Plasma 6 had removed on every
    // single tick.
    PoolHarness harness;

    harness.SetShellValue(50);

    // Deliberately not scripted, so the factory rejects it exactly as a compositor without
    // ext_idle_notifier_v1 would.
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    ASSERT_NE(harness.ShellSource(), nullptr);
    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());

    // And it stays marked while the candidate goes on failing, including once the backoff has stopped the
    // factory from even being offered it. A flag that decayed with the retry ladder would produce the
    // original flood on a slower clock.
    ReconcileTicksWithShell(harness, 20, {g_wayland_zero}, ShellKind::KDE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());
}

TEST(IdleSourcePool, AnAllX11CandidateSetDoesNotMarkTheShellAsWayland)
{
    // The other half of the rule. Candidates are a wider signal than live sources, so it has to be shown
    // that they are not so wide as to be useless: a session with no Wayland candidate at all must leave the
    // shell on the X11 arm, where a KDE shell really does have a GetSessionIdleTime to call.
    PoolHarness harness;

    harness.SetShellValue(50);
    harness.ScriptEndpoint(g_x11_one, 100);

    // A validated X11 endpoint and an X11 candidate that fails validation. Neither is Wayland, and a
    // failing candidate must not be mistaken for one.
    harness.Pool().Reconcile({g_x11_one, g_x11_two}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    ASSERT_NE(harness.ShellSource(), nullptr);
    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());

    ReconcileTicksWithShell(harness, 10, {g_x11_one, g_x11_two}, ShellKind::KDE);

    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());

    // The shell keeps its value, which is what the X11 arm exists for.
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 50);
}

TEST(IdleSourcePool, TheWaylandFlagFollowsCandidatesAcrossReconciles)
{
    // The flag tracks the candidate set on every tick rather than latching in either direction, and it does
    // so without any endpoint ever validating. A user who logs out of a Wayland session into an X11 one
    // must get a shell that resumes querying ksmserver, and vice versa.
    PoolHarness harness;

    harness.SetShellValue(50);
    harness.Pool().Reconcile({g_x11_one}, ShellKind::KDE);

    ASSERT_NE(harness.ShellSource(), nullptr);
    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());

    // A compositor socket appears. Nothing validates, and the answer changes anyway.
    harness.Pool().Reconcile({g_x11_one, g_wayland_zero}, ShellKind::KDE);

    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());

    // The socket goes away.
    harness.Pool().Reconcile({g_x11_one}, ShellKind::KDE);

    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());

    // And back, this time as the only candidate there is.
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::KDE);

    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());

    // An empty candidate set is not a Wayland session either. Reconcile() must recompute rather than leave
    // the last answer standing.
    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_FALSE(harness.ShellSource()->WaylandEndpointPresent());
}

TEST(IdleSourcePool, AShellSourceBuiltThisTickAlreadyKnowsTheSessionIsWayland)
{
    // The ordering that makes the fix effective in production. The pool pushes context at the END of
    // Reconcile(), so a shell source created on the same tick a Wayland candidate first appeared must be
    // told before the main loop can resolve it. Getting this wrong costs one misrouted query per shell
    // creation, which is once per session -- small, but it is the very first query the daemon makes.
    PoolHarness harness;

    harness.SetShellValue(50);
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::KDE);

    ASSERT_NE(harness.ShellSource(), nullptr);
    EXPECT_TRUE(harness.ShellSource()->WaylandEndpointPresent());
    EXPECT_GT(harness.ShellSource()->ContextUpdates(), 0);
}

//
// Shell disappearance debounce.
//
// The defect these pin down is a single failed D-Bus probe emptying the pool. The shell kind is
// re-derived from live NameHasOwner calls on every tick with nothing to smooth it, and in a
// shell-only session -- KDE on X11, or GNOME with no validated endpoint -- the shell is the ONLY
// source. One hiccuped probe therefore took the pool to no sources at all, reported
// IDLE_NO_GUI_SESSION, and flipped the daemon's source of truth to event_detect for a tick.
//

TEST(IdleSourcePool, OneMissedShellObservationDoesNotDropTheShell)
{
    // A shell-only session, which is the configuration where this is not merely noisy but wrong.
    PoolHarness harness;

    harness.SetShellValue(700);
    harness.Pool().Reconcile({}, ShellKind::KDE);

    ASSERT_TRUE(harness.Pool().HasShellSource());
    ASSERT_EQ(harness.Pool().GetIdleSeconds(), 700);

    FakeShellSource* source = harness.ShellSource();
    ASSERT_NE(source, nullptr);

    // One probe fails. The shell is still there; the bus was simply not answering this instant.
    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 0);
    EXPECT_EQ(harness.ShellSource(), source);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 700);
    EXPECT_NE(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, MissedShellObservationStillReportsInhibition)
{
    // Inhibition is evaluated from the tracked kind, so a tick that let the observation through would
    // resume the compute the user explicitly asked to hold off -- and it would do so while still
    // holding the very shell source that answers for it.
    PoolHarness harness;

    harness.SetShellValue(700);
    harness.SetInhibited(true);
    harness.Pool().Reconcile({}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().GetIdleSeconds(), 0);

    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 0);
    ASSERT_FALSE(harness.InhibitionKinds().empty());
    EXPECT_EQ(harness.InhibitionKinds().back(), ShellKind::KDE);
}

TEST(IdleSourcePool, ConsecutiveMissedShellObservationsEventuallyDropTheShell)
{
    // Debouncing is a delay, not a veto. A shell that has really exited must still be dropped, or the
    // pool would report a source that cannot answer and would never fall back to event_detect.
    PoolHarness harness;

    harness.SetShellValue(700);
    harness.Pool().Reconcile({}, ShellKind::KDE);

    ASSERT_TRUE(harness.Pool().HasShellSource());

    // One short of the threshold: still held.
    ReconcileTicks(harness, SHELL_ABSENCE_OBSERVATIONS_REQUIRED - 1, {});

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 0);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 700);

    // The observation that confirms it.
    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_FALSE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 1);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, AnInterveningShellObservationResetsTheAbsenceCount)
{
    // The threshold counts CONSECUTIVE absences. A run broken by a successful probe starts over,
    // otherwise a session whose bus is merely flaky would accumulate unrelated misses over hours and
    // eventually drop a shell that was answering all along.
    PoolHarness harness;

    harness.SetShellValue(700);
    harness.Pool().Reconcile({}, ShellKind::KDE);

    FakeShellSource* source = harness.ShellSource();
    ASSERT_NE(source, nullptr);

    // Two misses, then the probe answers again.
    ReconcileTicks(harness, SHELL_ABSENCE_OBSERVATIONS_REQUIRED - 1, {});
    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 0);

    // The shell that was held is the same object, not a rebuild: the debounce keeps the source, it
    // does not tear it down and put it back.
    EXPECT_EQ(harness.ShellSource(), source);
    EXPECT_EQ(harness.ShellFactoryKinds().size(), 1u);

    // Two more misses. Without the reset this would be the fourth consecutive absence and the shell
    // would already be gone.
    ReconcileTicks(harness, SHELL_ABSENCE_OBSERVATIONS_REQUIRED - 1, {});

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 700);

    // And the run completes on its own terms.
    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_FALSE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.ShellDestructions(), 1);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

TEST(IdleSourcePool, AShellAppearingIsActedOnImmediately)
{
    // Only disappearance is debounced. Delaying an appearance would leave a session that has just
    // come up reporting IDLE_NO_GUI_SESSION for no reason, and the risk the debounce exists to
    // manage does not run in this direction.
    PoolHarness harness;

    harness.SetShellValue(700);

    harness.Pool().Reconcile({}, ShellKind::NONE);

    ASSERT_FALSE(harness.Pool().HasShellSource());

    harness.Pool().Reconcile({}, ShellKind::KDE);

    EXPECT_TRUE(harness.Pool().HasShellSource());
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 700);
    EXPECT_EQ(harness.ShellFactoryKinds().size(), 1u);
}

TEST(IdleSourcePool, EndpointsAreNotDebounced)
{
    // Endpoints are discovered by a filesystem scan and validated by a connect, neither of which has
    // the probe-hiccup failure mode, and delaying their eviction would leave a source reporting
    // against a compositor that is already gone.
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, 42);
    harness.Pool().Reconcile({g_x11_one}, ShellKind::NONE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 1u);

    harness.Pool().Reconcile({}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_TRUE(harness.EndpointDestroyed(g_x11_one));
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}

//
// Shutdown.
//

TEST(IdleSourcePool, ShutdownDestroysEverythingAndReportsNoGuiSession)
{
    PoolHarness harness;

    harness.ScriptEndpoint(g_wayland_zero, 10);
    harness.ScriptEndpoint(g_x11_one, 20);
    harness.SetShellValue(30);
    harness.Pool().Reconcile({g_wayland_zero, g_x11_one}, ShellKind::KDE);

    ASSERT_EQ(harness.Pool().EndpointSourceCount(), 2u);
    ASSERT_TRUE(harness.Pool().HasShellSource());

    harness.Pool().Shutdown();

    EXPECT_TRUE(harness.EndpointDestroyed(g_wayland_zero));
    EXPECT_TRUE(harness.EndpointDestroyed(g_x11_one));
    EXPECT_EQ(harness.ShellDestructions(), 1);
    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_FALSE(harness.Pool().HasShellSource());

    // The shell kind is cleared along with the sources, so nothing lingers to report inhibition for
    // a shell that is no longer being tracked.
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);

    // Shutdown is not a one-way door; it is called on SIGTERM and on config reload alike.
    harness.Pool().Reconcile({g_x11_one}, ShellKind::KDE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 20);
}

TEST(IdleSourcePool, ShutdownIsIdempotent)
{
    PoolHarness harness;

    harness.ScriptEndpoint(g_x11_one, 20);
    harness.Pool().Reconcile({g_x11_one}, ShellKind::NONE);

    harness.Pool().Shutdown();
    harness.Pool().Shutdown();

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
}
