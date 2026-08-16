/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_SOURCES_SYSTEM_H
#define IDLE_SOURCES_SYSTEM_H

#include <idle_detect.h>
#include <idle_source.h>
#include <idle_source_pool.h>
#include <session_discovery.h>

#include <memory>
#include <string>

//
// This file, and only this file, makes actual system contact for idle detection: D-Bus (via GIO),
// Wayland, and X11. idle_source.* and session_discovery.* are deliberately free of those
// dependencies so they can link into idle_detect_tests, which links neither GLib nor X11 nor
// libwayland. Nothing declared here may therefore be pulled into that test target.
//

namespace IdleDetect {

//!
//! \brief The WaylandIdleSource class is the idle source for one Wayland endpoint, backed by
//! ext_idle_notifier_v1.
//!
//! It owns exactly one WaylandIdleMonitor bound to a specific socket, which is what allows more than one
//! Wayland endpoint to be monitored from a single process. There is no inhibition handling here:
//! inhibition is a per-user desktop shell concern and is evaluated separately, because the shell cannot be
//! attributed to any particular endpoint.
//!
class WaylandIdleSource : public IdleSource
{
public:
    //!
    //! \brief Constructor. Does not connect to the compositor; call Start() for that.
    //! \param socket_name Wayland socket to bind to, e.g. "wayland-0". Empty means use the WAYLAND_DISPLAY
    //!        environment variable.
    //!
    explicit WaylandIdleSource(std::string socket_name);

    //! \brief Destructor. Stops the monitor if it is running.
    ~WaylandIdleSource() override;

    //! \brief Deleted copy constructor and assignment operator. This owns a thread and a Wayland connection.
    WaylandIdleSource(const WaylandIdleSource&) = delete;
    WaylandIdleSource& operator=(const WaylandIdleSource&) = delete;

    //!
    //! \brief Connects to the socket and starts the monitor thread. A failure here means the candidate
    //! endpoint is not real, which is how discovery candidates are validated.
    //!
    //! The retry budget defaults to a single attempt, unlike WaylandIdleMonitor::Start(), whose default
    //! preserves the fifteen-attempt startup budget. That budget is wrong for a validator in two ways. The
    //! pool re-runs discovery on every reconcile tick and calls this again for any candidate that is still
    //! offered, so the reconcile loop already IS the retry loop and an internal one only duplicates it. And
    //! because this call is synchronous, every second spent retrying one candidate is a second in which no
    //! other endpoint is validated or read. The failure that makes this concrete is a compositor which does
    //! not implement ext_idle_notifier_v1 at all: the connect and both roundtrips succeed, the globals never
    //! appear, and the entire budget is spent on every tick forever rather than once.
    //!
    //! Everything this path logs about a failure is at debug level, which follows from the same reasoning. A
    //! failed Start() here does not mean something went wrong; it means a candidate produced by a deliberately
    //! over-inclusive discovery turned out not to be an endpoint, which is what validation is for. Such a
    //! candidate offers itself on every reconcile tick, so anything emitted here at normal or error level is
    //! emitted forever: 87 journal lines in 8 seconds were measured from one stale socket with no listener.
    //! The rejection is reported once, by IdleSourcePool, which is also what decides when to try again.
    //!
    //! \param notification_timeout_ms Idle notification threshold in milliseconds.
    //! \param max_init_retries Connect-and-bind attempts before declaring the candidate unusable. Exposed so
    //!        the pool can tune it without reaching into the monitor.
    //! \return true if the monitor was started.
    //!
    bool Start(int notification_timeout_ms, int max_init_retries = 1);

    //!
    //! \brief Stops the monitor and releases its Wayland resources. Idempotent, and a no-op if Start() was
    //! never called or did not succeed.
    //!
    void Stop();

    //!
    //! \brief Resolves the idle time reported by this endpoint's compositor.
    //! \return Idle seconds >= 0, or IDLE_ERROR if the monitor is not available.
    //!
    int64_t ResolveIdleSeconds() override;

    //!
    //! \brief Human-readable description, "wayland:<socket>".
    //! \return string representation
    //!
    std::string Describe() const override;

private:
    //! \brief Wayland socket this source is bound to.
    std::string m_socket_name;

    //! \brief The monitor implementing ext_idle_notifier_v1 against that socket.
    std::unique_ptr<WaylandIdleMonitor> m_monitor;

    //!
    //! \brief Whether Start() succeeded and Stop() therefore still has work to do.
    //!
    //! This is deliberately not IsAvailable(): a monitor whose thread exited on its own clears the monitor's
    //! initialized flag but leaves the thread joinable and the interrupt pipe open, and that state still has
    //! to be torn down. Gating Stop() on availability would skip exactly the failure case that matters.
    //!
    bool m_started;
};

//!
//! \brief The X11IdleSource class is the idle source for one X display, backed by XScreenSaver.
//!
//! It is stateless: the X connection is opened and closed within each query, so this source cannot go stale
//! and needs no liveness eviction. A display that has gone away simply starts returning IDLE_ERROR.
//!
class X11IdleSource : public IdleSource
{
public:
    //!
    //! \brief Constructor.
    //! \param display Canonical X display string, e.g. ":1". Empty uses the DISPLAY environment variable.
    //!
    explicit X11IdleSource(std::string display);

    //!
    //! \brief Resolves the idle time reported by XScreenSaver on this display.
    //! \return Idle seconds >= 0, or IDLE_ERROR.
    //!
    int64_t ResolveIdleSeconds() override;

    //!
    //! \brief Human-readable description, "x11:<display>".
    //! \return string representation
    //!
    std::string Describe() const override;

private:
    //! \brief X display this source queries.
    std::string m_display;
};

//!
//! \brief The ShellMonitor class determines which desktop shell owns the per-user session bus and whether
//! that shell reports idle inhibition.
//!
//! The desktop shell is a per-user singleton living under user@UID.service, and cannot be attributed to any
//! logind session, so it is detected by bus name ownership rather than by session enumeration. Only one
//! process can own org.kde.ksmserver or org.gnome.Mutter.IdleMonitor, so there is at most one shell per user
//! regardless of how many graphical endpoints exist.
//!
//! PRESENCE FOR INHIBITION AND PRESENCE FOR AN IDLE VALUE ARE DIFFERENT QUESTIONS, AND MUST NOT SHARE ONE
//! PROBE. On GNOME they are answered by two different bus names owned by two different processes:
//!
//!   - org.gnome.SessionManager is gnome-session, and it is what answers IsInhibited. It is present in
//!     every gnome-session desktop.
//!   - org.gnome.Mutter.IdleMonitor is mutter, and it is what answers the idle time. It is present only
//!     when the window manager actually is mutter.
//!
//! GNOME Flashback with Metacity, and other gnome-session variants, have the first and not the second.
//! Probing only for Mutter would classify those as ShellKind::NONE and silently drop their inhibition,
//! which is a regression against the behavior this replaced: the old GetIdleTimeSeconds() ran
//! CheckGnomeInhibition() on every non-KDE session unconditionally, precisely because inhibition does not
//! depend on the window manager. So DetectShellKind() answers the inhibition question and reports GNOME
//! for either name, while HasGnomeIdleMonitor() answers the idle-value question separately.
//!
class ShellMonitor
{
public:
    //!
    //! \brief Detects which shell's INHIBITION rules apply, by checking session bus name ownership.
    //!
    //! KDE is checked first, matching the ordering the old IsKdeSession() branch had. GNOME is reported
    //! when either org.gnome.Mutter.IdleMonitor or org.gnome.SessionManager has an owner; see the class
    //! commentary for why the second name has to count. A shell reported on the strength of
    //! org.gnome.SessionManager alone contributes inhibition only, exactly as KDE does on Wayland.
    //!
    //! \return ShellKind::KDE, ShellKind::GNOME, or ShellKind::NONE.
    //!
    ShellKind DetectShellKind() const;

    //!
    //! \brief Reports whether mutter's IdleMonitor is on the bus, i.e. whether a GNOME shell has an idle
    //! value to give at all.
    //!
    //! Deliberately separate from DetectShellKind(). Folding it in would recreate the regression the
    //! class commentary describes, because a single answer cannot serve both questions.
    //!
    //! \return true if org.gnome.Mutter.IdleMonitor has an owner.
    //!
    bool HasGnomeIdleMonitor() const;

    //!
    //! \brief Checks whether the given shell reports idle inhibition. This is a global override applied
    //! ahead of every source, not a property of any one endpoint: there is one shell, and inhibition means
    //! "do not let this machine go idle".
    //! \param kind Shell to query.
    //! \return true if inhibited, false if not inhibited, if the query failed, or if there is no shell.
    //!
    bool IsInhibited(ShellKind kind) const;
};

//!
//! \brief The ShellIdleSource class is the idle source for the per-user desktop shell.
//!
//! It is deliberately NOT tied to any endpoint. With N endpoints and one shell, at most one endpoint is the
//! shell's screen and there is no reliable way to determine which, so fusing the shell into an endpoint's
//! chain would make every endpoint report the shell's idle time and silently discard a second endpoint's
//! real activity. Inhibition is likewise not resolved here; ShellMonitor evaluates it separately.
//!
class ShellIdleSource : public IdleSource, public ShellSourceContext
{
public:
    //!
    //! \brief Constructor.
    //! \param kind Shell kind as detected by ShellMonitor.
    //!
    explicit ShellIdleSource(ShellKind kind);

    //!
    //! \brief Updates the shell kind, e.g. after a NameOwnerChanged transition.
    //! \param kind New shell kind.
    //!
    void SetKind(ShellKind kind);

    //!
    //! \brief Records whether any Wayland endpoint is currently live, which is how this source learns that a
    //! KDE shell is a Wayland shell.
    //!
    //! Plasma 6 removed ksmserver's GetSessionIdleTime on Wayland, so a KDE shell has an idle value on X11
    //! and none on Wayland. The old code made that distinction with getenv("WAYLAND_DISPLAY"), which is the
    //! frozen-at-exec environment read this design exists to eliminate. The owner of this source knows the
    //! live endpoint set, so it tells the source instead: a live Wayland endpoint means the compositor this
    //! shell is driving is a Wayland compositor. That is what ShellSourceContext, which this class
    //! implements for the purpose, is for: IdleSourcePool calls this on every reconcile with the
    //! state of its live endpoint sources.
    //!
    //! Both misclassifications are benign, which is why this indirect signal is acceptable. A KDE X11
    //! session sharing the machine with an unrelated Wayland compositor (a nested weston, a headless remote
    //! desktop) suppresses a shell value whose reading is XSS-derived and therefore duplicated by the X11
    //! endpoint that is already reporting it. In the other direction the shell would contribute IDLE_ERROR,
    //! which aggregation excludes. Inhibition is unaffected either way, since it does not run through here.
    //!
    //! \param present true if at least one live Wayland endpoint exists.
    //!
    void SetWaylandEndpointPresent(bool present) override;

    //!
    //! \brief Records whether mutter's IdleMonitor is on the bus, which is how this source learns that a
    //! GNOME shell is a GNOME shell WITHOUT an idle value.
    //!
    //! This is the GNOME counterpart of SetWaylandEndpointPresent(), and exists for the same reason: the
    //! shell kind alone does not determine whether there is a value to read. A gnome-session desktop whose
    //! window manager is not mutter -- GNOME Flashback with Metacity, and other variants -- is a GNOME
    //! shell for inhibition purposes but has no IdleMonitor to query. See the ShellMonitor commentary.
    //!
    //! Without this the source would still behave correctly, because the Mutter D-Bus call fails and
    //! returns IDLE_ERROR by itself. It would just pay a 500 ms D-Bus timeout to discover that on every
    //! resolve, on a session where the answer never changes.
    //!
    //! \param present true if org.gnome.Mutter.IdleMonitor has an owner.
    //!
    void SetGnomeIdleMonitorPresent(bool present);

    //!
    //! \brief Resolves the shell's idle time per the source-chain table: KDE on X11 uses ksmserver, which
    //! handles inhibition internally so no separate check is added; GNOME uses Mutter's IdleMonitor; KDE on
    //! Wayland, and GNOME without mutter, have no idle value and contribute inhibition only.
    //! \return Idle seconds >= 0, or IDLE_ERROR when this shell supplies no value.
    //!
    int64_t ResolveIdleSeconds() override;

    //!
    //! \brief Human-readable description, "shell:kde", "shell:gnome", or "shell:none".
    //! \return string representation
    //!
    std::string Describe() const override;

private:
    //! \brief Shell kind this source resolves for.
    ShellKind m_kind;

    //!
    //! \brief Whether a live Wayland endpoint exists. See SetWaylandEndpointPresent().
    //!
    //! Defaults to false, so a shell source constructed before its owner has run discovery attempts the
    //! ksmserver call once rather than suppressing it. That call returns an error on Plasma 6 Wayland, which
    //! aggregation excludes.
    //!
    bool m_wayland_endpoint_present;

    //!
    //! \brief Whether mutter's IdleMonitor is on the bus. See SetGnomeIdleMonitorPresent().
    //!
    //! Defaults to true for the same reason m_wayland_endpoint_present defaults to false: a source
    //! constructed before its owner has probed the bus should attempt the call once rather than suppress
    //! it, and a wrong guess costs one excluded IDLE_ERROR.
    //!
    bool m_gnome_idle_monitor_present;
};

// -----------------------------------------------------------------------------------------------------
// Production wiring for IdleSourcePool
// -----------------------------------------------------------------------------------------------------
//
// IdleSourcePool is deliberately dependency-free and takes everything that touches the system through
// injected callables. The four functions below are the production implementations of those callables and
// of the discovery inputs, and they are the only place where the pool's abstract types are bound to real
// D-Bus, Wayland and X11 contact. A test builds the same pool with its own lambdas and links none of it.
//

//!
//! \brief Gathers endpoint discovery hints from the running system.
//!
//! Every hint is independently unreliable and none of them is load-bearing: DiscoverEndpoints() takes
//! their union and each resulting candidate is validated by connecting to it. Four are gathered here --
//! $XDG_RUNTIME_DIR for the Wayland socket scan, /tmp/.X11-unix for the X socket scan, our uid so that
//! scan can reject other users' sockets, and the DISPLAY assignments the systemd user manager exports.
//!
//! The last of those is what makes a D-Bus call worth making. The user manager's Environment property is
//! updated when the graphical session starts, by the display manager or by an equivalent
//! "systemctl --user import-environment DISPLAY", so it describes the session that exists NOW. The
//! process's own DISPLAY is added as one more hint, but only as a hint: it is frozen at exec, and a
//! daemon started before its GUI session -- the failure this whole design exists to fix -- does not have
//! one at all.
//!
//! That property is annotated EmitsChangedSignal("false"), so it is READ ON DEMAND on every call rather
//! than subscribed to once and cached. A PropertiesChanged subscription would simply never fire, leaving
//! the cache frozen at whatever the first read saw, which is the same staleness bug in a new place.
//!
//! An unreachable or absent systemd user manager is an ordinary outcome rather than an error: whatever
//! hints were gathered are returned, and the socket scans carry discovery by themselves.
//!
//! DiscoveryHints::m_logind_displays is deliberately left empty. It is documented as optional enrichment,
//! and every display a logind graphical session could name is already reachable through the X socket scan
//! or the manager environment.
//!
//! \return Populated hints. Any field may be empty.
//!
DiscoveryHints BuildDiscoveryHints();

//!
//! \brief Creates the production endpoint source factory for IdleSourcePool.
//!
//! Validation lives in the factory because what "validated" means is protocol-specific:
//!
//!   - A Wayland candidate is validated by connecting to its socket and binding ext_idle_notifier_v1,
//!     which is what WaylandIdleSource::Start() does. It is called with that method's one-attempt
//!     default rather than the fifteen-attempt startup budget, because the pool re-runs discovery on
//!     every reconcile tick and calls this factory again for any candidate still being offered. The
//!     reconcile loop is the retry loop; see the commentary on WaylandIdleSource::Start().
//!
//!   - An X11 candidate is validated by taking one reading, since X11IdleSource is stateless and has
//!     nothing to start. A reading of IDLE_ERROR rejects the candidate.
//!
//! A rejected candidate yields nullptr, which the pool does not cache: the candidate is offered again on
//! the next tick and built again then, so a compositor or X server that was still starting up is picked
//! up shortly afterwards rather than being locked out for the life of the process.
//!
//! \param notification_timeout_ms Idle notification threshold handed to each new Wayland source.
//! \return Factory suitable for IdleSourcePool's constructor.
//!
EndpointSourceFactory MakeEndpointSourceFactory(int notification_timeout_ms);

//!
//! \brief Creates the production shell source factory for IdleSourcePool.
//!
//! ShellIdleSource carries two flags that its ShellKind alone does not determine, and they are set from
//! two different places on purpose, because two different things know them:
//!
//!   - Whether mutter's IdleMonitor is on the bus is a session bus fact, and this file is the only place
//!     that can answer it. The factory probes it with ShellMonitor::HasGnomeIdleMonitor() and stamps it
//!     onto the source before returning it. The probe is made only for a GNOME shell, because that flag
//!     is read only by the GNOME arm of ShellIdleSource::ResolveIdleSeconds(); paying a D-Bus round trip
//!     to answer a question nobody asks would be pure cost.
//!
//!   - Whether a live Wayland endpoint exists -- which is what separates KDE on Wayland, where Plasma 6
//!     ksmserver has no GetSessionIdleTime, from KDE on X11, where it does -- is NOT set here, and the
//!     omission is deliberate rather than an oversight. IdleSourcePool pushes it through
//!     ShellSourceContext at the end of every Reconcile(), including the Reconcile() that created the
//!     source, so it is already correct before any GetIdleSeconds() can observe it.
//!
//! The second one is not something the factory could do better if it tried. It is handed a ShellKind and
//! nothing else, and the fact in question is not "was a Wayland endpoint discovered" but "did a Wayland
//! endpoint validate, and is it live right now" -- which only the pool knows, because only the pool owns
//! the sources. The two answers a factory could reach for are both wrong: the candidate set counts
//! endpoints that failed to validate, and getenv("WAYLAND_DISPLAY") is precisely the frozen-at-exec read
//! this design exists to eliminate. Setting the flag in both places would also make the pool's push and
//! the factory's guess two sources of truth for one fact, with the staler one winning whenever the
//! factory ran last.
//!
//! \return Factory suitable for IdleSourcePool's constructor.
//!
ShellSourceFactory MakeShellSourceFactory();

//!
//! \brief Creates the production inhibition query for IdleSourcePool, wrapping ShellMonitor::IsInhibited().
//!
//! The pool calls this on every GetIdleSeconds() with the shell kind it currently tracks, including
//! ShellKind::NONE. That case is answered without any bus traffic, since nothing that does not exist can
//! be inhibiting.
//!
//! \return Query suitable for IdleSourcePool's constructor.
//!
InhibitionQuery MakeInhibitionQuery();

//!
//! \brief The single process-wide pool of live idle sources, defined in idle_detect.cpp and built from the
//! production factories above.
//!
//! It is declared here rather than in idle_detect.h because it is only meaningful in combination with those
//! factories, and because idle_detect.h is included BY this header: an IdleSourcePool object declared there
//! would make the lower-level header depend on the pool, inverting the layering this split exists to
//! establish. GetIdleTimeSeconds() is the reader; main() is the only thing that reconciles or shuts it down.
//!
//! Construction takes no action and makes no system contact, so it is safe at static initialization time. The
//! pool holds no sources until the first Reconcile(), which is what makes "the GUI session does not exist
//! yet" an ordinary starting state rather than a startup failure.
//!
extern IdleSourcePool g_idle_source_pool;

} // namespace IdleDetect

#endif // IDLE_SOURCES_SYSTEM_H
