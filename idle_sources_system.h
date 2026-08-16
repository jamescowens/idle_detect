/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_SOURCES_SYSTEM_H
#define IDLE_SOURCES_SYSTEM_H

#include <idle_detect.h>
#include <idle_source.h>

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
    //! \param notification_timeout_ms Idle notification threshold in milliseconds.
    //! \return true if the monitor was started.
    //!
    bool Start(int notification_timeout_ms);

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
class ShellMonitor
{
public:
    //!
    //! \brief Detects the current desktop shell by checking session bus name ownership.
    //! \return ShellKind::KDE, ShellKind::GNOME, or ShellKind::NONE.
    //!
    ShellKind DetectShellKind() const;

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
class ShellIdleSource : public IdleSource
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
    //! shell is driving is a Wayland compositor.
    //!
    //! Both misclassifications are benign, which is why this indirect signal is acceptable. A KDE X11
    //! session sharing the machine with an unrelated Wayland compositor (a nested weston, a headless remote
    //! desktop) suppresses a shell value whose reading is XSS-derived and therefore duplicated by the X11
    //! endpoint that is already reporting it. In the other direction the shell would contribute IDLE_ERROR,
    //! which aggregation excludes. Inhibition is unaffected either way, since it does not run through here.
    //!
    //! \param present true if at least one live Wayland endpoint exists.
    //!
    void SetWaylandEndpointPresent(bool present);

    //!
    //! \brief Resolves the shell's idle time per the source-chain table: KDE on X11 uses ksmserver, which
    //! handles inhibition internally so no separate check is added; GNOME uses Mutter's IdleMonitor; KDE on
    //! Wayland has no idle value and contributes inhibition only.
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
};

} // namespace IdleDetect

#endif // IDLE_SOURCES_SYSTEM_H
