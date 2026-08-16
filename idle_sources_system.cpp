/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_sources_system.h>
#include <util.h>

#include <gio/gio.h>

#include <utility>

namespace IdleDetect {

namespace {

//!
//! \brief Checks whether a well-known name currently has an owner on the session bus.
//!
//! This is how shell presence is determined. The shell lives under user@UID.service and cannot be
//! attributed to a logind session, and only one process can own a well-known name, so ownership of the
//! name is both the most direct and the only reliable signal available.
//!
//! \param name Well-known bus name to check, e.g. "org.kde.ksmserver".
//! \return true if the name has an owner, false if it does not or if the bus could not be queried.
//!
bool SessionBusNameHasOwner(const char* name)
{
    GError* connect_error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &connect_error);

    if (!connection) {
        if (connect_error) {
            debug_log("INFO: %s: Cannot connect to session bus to check owner of %s: %s",
                      __func__,
                      name,
                      connect_error->message);
            g_error_free(connect_error);
        } else {
            debug_log("INFO: %s: Cannot connect to session bus to check owner of %s (unknown error).",
                      __func__,
                      name);
        }

        return false;
    }

    GError* call_error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "NameHasOwner",
                                                   g_variant_new("(s)", name),
                                                   G_VARIANT_TYPE("(b)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   500, // Timeout (ms)
                                                   nullptr,
                                                   &call_error);

    gboolean has_owner = FALSE;

    if (call_error) {
        debug_log("INFO: %s: Error checking D-Bus owner for %s: %s", __func__, name, call_error->message);
        g_error_free(call_error);
    } else if (result) {
        g_variant_get(result, "(b)", &has_owner);
        g_variant_unref(result);
    } else {
        error_log("%s: Call to NameHasOwner for %s returned no result and no error.", __func__, name);
    }

    g_object_unref(connection);

    debug_log("INFO: %s: D-Bus name %s owned? %s", __func__, name, (has_owner == TRUE) ? "Yes" : "No");

    return has_owner == TRUE;
}

} // anonymous namespace

// -----------------------------------------------------------------------------------------------------
// WaylandIdleSource
// -----------------------------------------------------------------------------------------------------

WaylandIdleSource::WaylandIdleSource(std::string socket_name)
    : m_socket_name(std::move(socket_name))
    , m_monitor(std::make_unique<WaylandIdleMonitor>())
    , m_started(false)
{}

WaylandIdleSource::~WaylandIdleSource()
{
    Stop();
}

bool WaylandIdleSource::Start(int notification_timeout_ms, int max_init_retries)
{
    // A failed Start() has already torn down everything it constructed, so m_started stays false and Stop()
    // correctly has nothing to do.
    m_started = m_monitor->Start(m_socket_name, notification_timeout_ms, max_init_retries);

    return m_started;
}

void WaylandIdleSource::Stop()
{
    if (!m_started) {
        return;
    }

    m_monitor->Stop();
    m_started = false;
}

int64_t WaylandIdleSource::ResolveIdleSeconds()
{
    // IsAvailable() goes false when the monitor thread exits on its own, which is how a compositor hangup
    // surfaces. Reporting IDLE_ERROR rather than the last known value keeps a dead endpoint out of the
    // aggregate instead of freezing it at whatever the compositor last said.
    if (!m_monitor->IsAvailable()) {
        return IDLE_ERROR;
    }

    return m_monitor->GetIdleSeconds();
}

std::string WaylandIdleSource::Describe() const
{
    return "wayland:" + m_socket_name;
}

// -----------------------------------------------------------------------------------------------------
// X11IdleSource
// -----------------------------------------------------------------------------------------------------

X11IdleSource::X11IdleSource(std::string display)
    : m_display(std::move(display))
{}

int64_t X11IdleSource::ResolveIdleSeconds()
{
    // One attempt, not the six that GetIdleTimeXss() defaults to. That default was sized for a single startup
    // connection to the process's own DISPLAY, where waiting 2.5 seconds for an X server that is still coming
    // up happens once. Here the question is narrower: a display that is not answering right now is simply not
    // an endpoint right now. Discovery re-offers it on the next reconcile tick and this runs again, so the
    // reconcile loop already supplies the retries. Keeping them inside the call would instead make every dead
    // display stall the whole pool for 2.5 seconds, on every tick, since this call is synchronous and N
    // displays are resolved in sequence.
    return GetIdleTimeXss(m_display, 1);
}

std::string X11IdleSource::Describe() const
{
    return "x11:" + m_display;
}

// -----------------------------------------------------------------------------------------------------
// ShellMonitor
// -----------------------------------------------------------------------------------------------------

ShellKind ShellMonitor::DetectShellKind() const
{
    if (SessionBusNameHasOwner("org.kde.ksmserver")) {
        return ShellKind::KDE;
    }

    if (SessionBusNameHasOwner("org.gnome.Mutter.IdleMonitor")) {
        return ShellKind::GNOME;
    }

    return ShellKind::NONE;
}

bool ShellMonitor::IsInhibited(ShellKind kind) const
{
    switch (kind) {
    case ShellKind::KDE:
        return CheckKdeInhibition();
    case ShellKind::GNOME:
        return CheckGnomeInhibition();
    case ShellKind::NONE:
        break;
    }

    // No shell means nothing can be inhibiting, which is correct rather than a gap.
    return false;
}

// -----------------------------------------------------------------------------------------------------
// ShellIdleSource
// -----------------------------------------------------------------------------------------------------

ShellIdleSource::ShellIdleSource(ShellKind kind)
    : m_kind(kind)
    , m_wayland_endpoint_present(false)
{}

void ShellIdleSource::SetKind(ShellKind kind)
{
    m_kind = kind;
}

void ShellIdleSource::SetWaylandEndpointPresent(bool present)
{
    m_wayland_endpoint_present = present;
}

int64_t ShellIdleSource::ResolveIdleSeconds()
{
    switch (m_kind) {
    case ShellKind::KDE:
        // Plasma 6 removed ksmserver's GetSessionIdleTime on Wayland, where the Wayland endpoint's
        // ext_idle_notifier_v1 is the idle source instead and this shell contributes inhibition only.
        // On X11 ksmserver still answers, and it folds inhibition into the value it returns, so no
        // separate inhibition check is added here.
        if (m_wayland_endpoint_present) {
            debug_log("INFO: %s: KDE shell alongside a live Wayland endpoint. No shell idle value; the "
                      "Wayland endpoint supplies it and the shell supplies inhibition.",
                      __func__);
            break;
        }

        return GetIdleTimeKdeDBus();
    case ShellKind::GNOME:
        // Mutter's IdleMonitor answers on both GNOME X11 and GNOME Wayland, so no such distinction is
        // needed here.
        return GetIdleTimeWaylandGnomeViaDBus();
    case ShellKind::NONE:
        break;
    }

    return IDLE_ERROR;
}

std::string ShellIdleSource::Describe() const
{
    switch (m_kind) {
    case ShellKind::KDE:
        return "shell:kde";
    case ShellKind::GNOME:
        return "shell:gnome";
    case ShellKind::NONE:
        break;
    }

    return "shell:none";
}

} // namespace IdleDetect
