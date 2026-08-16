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

    std::string Describe() const override
    {
        return m_description;
    }

    //! \brief Changes what this source reports from the next resolve onwards.
    void SetValue(int64_t value)
    {
        m_value = value;
    }

    //! \brief Number of times the pool has resolved this source.
    int ResolveCount() const
    {
        return m_resolve_count;
    }

private:
    std::string m_description;
    int64_t m_value;
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
    // Caching that failure is the bug: a compositor that is still starting up would then be locked
    // out for the life of the daemon.
    PoolHarness harness;

    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), IDLE_NO_GUI_SESSION);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 1);

    // Still failing on the next tick: still retried, still not added.
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 0u);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 2);

    // The compositor finishes coming up.
    harness.ScriptEndpoint(g_wayland_zero, 17);
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    EXPECT_EQ(harness.Pool().EndpointSourceCount(), 1u);
    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);
    EXPECT_EQ(harness.Pool().GetIdleSeconds(), 17);

    // And having succeeded, it is not rebuilt on the tick after that.
    harness.Pool().Reconcile({g_wayland_zero}, ShellKind::NONE);

    EXPECT_EQ(harness.EndpointFactoryCalls(g_wayland_zero), 3);
    EXPECT_FALSE(harness.EndpointDestroyed(g_wayland_zero));
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

    // GNOME -> NONE: dropped and destroyed.
    harness.Pool().Reconcile({}, ShellKind::NONE);

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
