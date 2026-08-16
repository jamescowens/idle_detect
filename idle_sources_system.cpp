/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_sources_system.h>
#include <util.h>

#include <gio/gio.h>

#include <memory>
#include <optional>
#include <unistd.h>
#include <utility>
#include <vector>

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

//!
//! \brief Reads the DISPLAY assignments out of the systemd user manager's Environment property.
//!
//! This is the one discovery hint that describes the session as it is now rather than as it was when this
//! process was executed, which is why it is worth a synchronous D-Bus call on every discovery pass. The
//! property is annotated EmitsChangedSignal("false"), so there is no subscription available to replace
//! that call with; caching the first read instead would reintroduce exactly the staleness the read exists
//! to avoid.
//!
//! Every DISPLAY assignment found is returned rather than just one. systemd resolves duplicates by
//! letting the last assignment win, but discovery is over-inclusive by design and every candidate is
//! validated by connecting to it, so returning all of them costs at most one failed connect on a
//! duplicate that no longer exists, and avoids having to guess which assignment is the live one.
//!
//! Failure at any step is silent beyond a debug log: an absent or unresponsive user manager simply
//! contributes no hint, and the socket scans carry the discovery on their own.
//!
//! \return DISPLAY values, empty if the manager is unreachable or exports none.
//!
std::vector<std::string> ReadDisplaysFromSystemdUserManager()
{
    std::vector<std::string> displays;

    GError* connect_error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &connect_error);

    if (!connection) {
        if (connect_error) {
            debug_log("INFO: %s: Cannot connect to the session bus to read the systemd user manager "
                      "environment: %s",
                      __func__,
                      connect_error->message);
            g_error_free(connect_error);
        } else {
            debug_log("INFO: %s: Cannot connect to the session bus to read the systemd user manager "
                      "environment (unknown error).",
                      __func__);
        }

        return displays;
    }

    GError* call_error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.systemd1",
                                                   "/org/freedesktop/systemd1",
                                                   "org.freedesktop.DBus.Properties",
                                                   "Get",
                                                   g_variant_new("(ss)",
                                                                 "org.freedesktop.systemd1.Manager",
                                                                 "Environment"),
                                                   G_VARIANT_TYPE("(v)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   500, // Timeout (ms)
                                                   nullptr,
                                                   &call_error);

    if (call_error) {
        // No user manager, or one that does not expose this property, is an ordinary state rather than
        // an error. A tty-only login over ssh is the common case.
        debug_log("INFO: %s: Could not read the systemd user manager Environment property: %s",
                  __func__,
                  call_error->message);
        g_error_free(call_error);
    } else if (result) {
        GVariant* boxed = nullptr;
        g_variant_get(result, "(v)", &boxed);

        if (boxed != nullptr) {
            // The property is documented as "as", but this is data from another process, and a wrong
            // type would otherwise be a g_variant_get_strv() precondition failure rather than a missed
            // hint.
            if (g_variant_is_of_type(boxed, G_VARIANT_TYPE_STRING_ARRAY)) {
                const std::string prefix = "DISPLAY=";

                gsize count = 0;
                const gchar** entries = g_variant_get_strv(boxed, &count);

                if (entries != nullptr) {
                    for (gsize i = 0; i < count; ++i) {
                        const std::string entry(entries[i]);

                        if (entry.rfind(prefix, 0) != 0) {
                            continue;
                        }

                        const std::string value = entry.substr(prefix.size());

                        if (!value.empty()) {
                            debug_log("INFO: %s: systemd user manager environment provides DISPLAY=%s.",
                                      __func__,
                                      value.c_str());

                            displays.push_back(value);
                        }
                    }

                    // Shallow free. The strings themselves belong to the variant.
                    g_free(entries);
                }
            } else {
                debug_log("INFO: %s: systemd user manager Environment property is not a string array.",
                          __func__);
            }

            g_variant_unref(boxed);
        }

        g_variant_unref(result);
    } else {
        error_log("%s: Call to read the systemd user manager Environment property returned no result and "
                  "no error.",
                  __func__);
    }

    g_object_unref(connection);

    return displays;
}

//!
//! \brief Builds and validates the source for a Wayland endpoint candidate.
//!
//! Validation is the connect-and-bind that Start() performs. Its default budget of one attempt is taken
//! rather than the startup budget, because the pool re-offers a candidate that failed on every subsequent
//! reconcile tick, and a synchronous retry here would stall every other candidate behind it.
//!
//! \param socket_name Wayland socket to bind to.
//! \param notification_timeout_ms Idle notification threshold in milliseconds.
//! \return Started source, or nullptr if the candidate is not a usable endpoint right now.
//!
std::unique_ptr<IdleSource> MakeWaylandEndpointSource(const std::string& socket_name,
                                                      int notification_timeout_ms)
{
    auto source = std::make_unique<WaylandIdleSource>(socket_name);

    if (!source->Start(notification_timeout_ms)) {
        debug_log("INFO: %s: Wayland candidate %s did not start. It is either not a compositor socket or "
                  "the compositor does not implement ext_idle_notifier_v1.",
                  __func__,
                  source->Describe().c_str());

        return nullptr;
    }

    return source;
}

//!
//! \brief Builds and validates the source for an X11 endpoint candidate.
//!
//! X11IdleSource is stateless and has nothing to start, so the reading itself is the validation. This is
//! also the only thing that distinguishes a live display from a stale socket left behind by a dead X
//! server, which the socket scan cannot tell apart.
//!
//! \param display Canonical X display string.
//! \return Source, or nullptr if the display did not answer.
//!
std::unique_ptr<IdleSource> MakeX11EndpointSource(const std::string& display)
{
    auto source = std::make_unique<X11IdleSource>(display);

    if (source->ResolveIdleSeconds() == IDLE_ERROR) {
        debug_log("INFO: %s: X candidate %s did not answer an XScreenSaver query.",
                  __func__,
                  source->Describe().c_str());

        return nullptr;
    }

    return source;
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

bool WaylandIdleSource::IsAlive() const
{
    // The same flag ResolveIdleSeconds() gates on, asked as a different question. There it decides whether
    // this reading is usable; here it decides whether this source still has a future, which is what lets the
    // pool rebuild the connection instead of holding a source that can only ever return IDLE_ERROR.
    return m_monitor->IsAvailable();
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

bool X11IdleSource::IsAlive() const
{
    // Stateless by construction: the connection is opened and closed inside every query, so there is no
    // stale state for liveness to detect and a failed reading is reported as IDLE_ERROR rather than as
    // death. See the header for why evicting on those errors would be worse than keeping the source.
    return true;
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
    // KDE first, preserving the ordering the old IsKdeSession() branch had.
    if (SessionBusNameHasOwner("org.kde.ksmserver")) {
        return ShellKind::KDE;
    }

    // Mutter first among the GNOME names only because it is the common case and answers both questions,
    // so the second probe is usually skipped. It is not the authoritative one: see below.
    if (SessionBusNameHasOwner("org.gnome.Mutter.IdleMonitor")) {
        return ShellKind::GNOME;
    }

    // gnome-session without mutter. This is still a GNOME shell for inhibition purposes -- IsInhibited
    // talks to org.gnome.SessionManager, which is this very name -- and dropping it here would silently
    // lose inhibition on GNOME Flashback/Metacity and other gnome-session variants. See the ShellMonitor
    // class commentary in the header for why the two questions cannot share one probe.
    if (SessionBusNameHasOwner("org.gnome.SessionManager")) {
        debug_log("INFO: %s: org.gnome.SessionManager is present without org.gnome.Mutter.IdleMonitor. "
                  "Treating this as a GNOME shell that contributes inhibition but no idle value.",
                  __func__);

        return ShellKind::GNOME;
    }

    return ShellKind::NONE;
}

bool ShellMonitor::HasGnomeIdleMonitor() const
{
    return SessionBusNameHasOwner("org.gnome.Mutter.IdleMonitor");
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
    , m_gnome_idle_monitor_present(true)
{}

void ShellIdleSource::SetKind(ShellKind kind)
{
    m_kind = kind;
}

void ShellIdleSource::SetWaylandEndpointPresent(bool present)
{
    m_wayland_endpoint_present = present;
}

void ShellIdleSource::SetGnomeIdleMonitorPresent(bool present)
{
    m_gnome_idle_monitor_present = present;
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
        // Mutter's IdleMonitor answers on both GNOME X11 and GNOME Wayland, so the Wayland distinction
        // KDE needs does not apply. What does apply is whether mutter is the window manager at all: a
        // gnome-session desktop running Metacity owns org.gnome.SessionManager but not
        // org.gnome.Mutter.IdleMonitor, so it is a GNOME shell with no idle value to read. It then
        // contributes inhibition only, exactly like KDE on Wayland above, and its endpoint supplies the
        // idle value.
        if (!m_gnome_idle_monitor_present) {
            debug_log("INFO: %s: GNOME shell without mutter's IdleMonitor. No shell idle value; the "
                      "endpoint supplies it and the shell supplies inhibition.",
                      __func__);
            break;
        }

        return GetIdleTimeWaylandGnomeViaDBus();
    case ShellKind::NONE:
        break;
    }

    return IDLE_ERROR;
}

bool ShellIdleSource::IsAlive() const
{
    // Nothing is held between resolves, so nothing can die between them. A shell that goes away is handled
    // by the pool as a change of ShellKind, not as a liveness failure.
    return true;
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

// -----------------------------------------------------------------------------------------------------
// Production wiring for IdleSourcePool
// -----------------------------------------------------------------------------------------------------

DiscoveryHints BuildDiscoveryHints()
{
    DiscoveryHints hints;

    // An unset XDG_RUNTIME_DIR leaves the path empty, which DiscoverEndpoints() reads as "skip the
    // Wayland socket scan" rather than as "scan the current directory".
    std::optional<std::string> runtime_dir = GetEnvVariable("XDG_RUNTIME_DIR");

    if (runtime_dir.has_value() && !runtime_dir->empty()) {
        hints.m_xdg_runtime_dir = *runtime_dir;
    } else {
        debug_log("INFO: %s: XDG_RUNTIME_DIR is not set. No Wayland sockets will be discovered.", __func__);
    }

    hints.m_x11_socket_dir = "/tmp/.X11-unix";

    // This is what lets the X socket scan reject the other users' sockets and the display manager's
    // root-owned greeter socket that share that world-visible directory. Leaving it unset would not
    // disable the filter, it would invert it, so DiscoverEndpoints() skips the scan entirely without it.
    hints.m_uid = getuid();

    for (const std::string& display : ReadDisplaysFromSystemdUserManager()) {
        hints.m_env_displays.push_back(display);
    }

    // The process environment is a hint as well, and no more than a hint: it is frozen at exec, which is
    // the whole reason the manager environment is consulted above. It is still worth including, because a
    // daemon started from inside a graphical session has a correct DISPLAY here even on a setup where
    // nothing ever imported it into the systemd user manager.
    std::optional<std::string> env_display = GetEnvVariable("DISPLAY");

    if (env_display.has_value() && !env_display->empty()) {
        hints.m_env_displays.push_back(*env_display);
    }

    return hints;
}

EndpointSourceFactory MakeEndpointSourceFactory(int notification_timeout_ms)
{
    // The timeout is captured rather than threaded through Reconcile(), so that the pool never has to
    // carry a parameter that only one kind of source uses.
    return [notification_timeout_ms](const Endpoint& endpoint) -> std::unique_ptr<IdleSource> {
        switch (endpoint.m_kind) {
        case EndpointKind::WAYLAND:
            return MakeWaylandEndpointSource(endpoint.m_identifier, notification_timeout_ms);
        case EndpointKind::X11:
            return MakeX11EndpointSource(endpoint.m_identifier);
        }

        return nullptr;
    };
}

ShellSourceFactory MakeShellSourceFactory()
{
    // ShellMonitor holds no state and no connection, so capturing one by value costs nothing and keeps
    // the bus probe below reading as what it is: a question asked of the monitor rather than a free
    // function pulled out of the air.
    return [monitor = ShellMonitor()](ShellKind kind) -> std::unique_ptr<IdleSource> {
        if (kind == ShellKind::NONE) {
            return nullptr;
        }

        auto source = std::make_unique<ShellIdleSource>(kind);

        // Only GNOME reads this flag, and only this file can answer it. A KDE shell is left at the
        // source's default rather than paying a D-Bus round trip for a value nothing will look at.
        //
        // The other flag ShellIdleSource carries, whether a live Wayland endpoint exists, is
        // deliberately not set here: IdleSourcePool pushes it through ShellSourceContext at the end of
        // every Reconcile(), including this one. See MakeShellSourceFactory()'s header commentary for
        // why the factory must not try to answer that question itself.
        if (kind == ShellKind::GNOME) {
            const bool has_idle_monitor = monitor.HasGnomeIdleMonitor();

            if (!has_idle_monitor) {
                normal_log("INFO: %s: GNOME shell detected without org.gnome.Mutter.IdleMonitor. It will "
                           "contribute inhibition only, and its endpoints will supply the idle value.",
                           __func__);
            }

            source->SetGnomeIdleMonitorPresent(has_idle_monitor);
        }

        return source;
    };
}

InhibitionQuery MakeInhibitionQuery()
{
    return [monitor = ShellMonitor()](ShellKind kind) -> bool {
        return monitor.IsInhibited(kind);
    };
}

} // namespace IdleDetect
