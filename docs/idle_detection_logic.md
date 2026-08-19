# Idle Detection Logic — Architecture and Decision Flow

This document describes how idle_detect determines whether the user is idle,
how it interacts with event_detect, and how different desktop environments
and session types are handled.

## Two-Process Model

```
+-------------------------------+       +-------------------------------+
|  event_detect (system daemon) |       |  idle_detect (per-user daemon)|
|  Runs as event_detect:        |       |  One instance per login       |
|  event_detect user            |       |  session                      |
|                               |       |                               |
|  Monitors:                    |       |  Monitors:                    |
|  - /dev/input/event* (mice)   |       |  - GUI session idle time      |
|  - pts/tty access times       |       |    (method varies by DE)      |
|  - Named pipe from idle_detect|       |  - Idle inhibition state      |
|                               |       |  - Control overrides          |
|  Exports:                     |       |    (FORCED_ACTIVE/FORCED_IDLE)|
|  - POSIX shmem                |       |                               |
|    (/idle_detect_shmem)       |       |  Sends to event_detect:       |
|  - File (last_active_time.dat)|       |  - USER_ACTIVE messages       |
|    (optional)                 |       |    via named pipe              |
+-------------------------------+       +-------------------------------+
```

### Data Flow

1. **event_detect** monitors hardware input devices directly via libevdev.
   Each pointing device gets its own `EventRecorder` thread. The `Monitor`
   thread aggregates event counts, tty access times, and messages from
   idle_detect instances to determine the overall `last_active_time`.

2. **idle_detect** queries the GUI session's idle time using desktop-specific
   methods (see below). It combines this with the `last_active_time` from
   event_detect's shared memory (via `std::min` of both idle durations) to
   determine the final idle state.

3. When idle_detect determines the user is active, it sends a timestamped
   `USER_ACTIVE` message to event_detect via the named pipe. This ensures
   event_detect's shared memory reflects GUI-level activity even when the
   user is interacting through methods that don't generate `/dev/input`
   events (e.g., touchpad gestures handled by the compositor).

### Combining idle_detect and event_detect

In the main loop of idle_detect (`main()` in `idle_detect.cpp`), the idle
time from `GetIdleTimeSeconds()` (GUI session) is combined with the idle
time derived from event_detect's shared memory timestamp:

```
idle_seconds = min(gui_idle_seconds, current_time - shmem_last_active_time)
```

This means the system is considered active if **either** source shows
activity. The `std::min` ensures the shortest idle duration wins.


## GetIdleTimeSeconds() and the idle source pool

Prior to 0.9.2.0 this was a decision tree keyed on environment variables read
once at startup (`IsTtySession()`, `IsWaylandSession()`), which froze the
daemon's view of the session at process start. A daemon that outlived a GUI
session, or started before one existed, never recovered. That was the cause of
issue #12 and both functions are gone.

`GetIdleTimeSeconds()` now delegates to `g_idle_source_pool`, which maintains a
set of live sources and re-derives them on a reconcile tick.

### Two layers

| layer | count | supplies |
|---|---|---|
| desktop shell source | 0 or 1 | inhibition, and on some shells an idle value |
| graphical endpoints | 0..N | an idle value per reachable endpoint |

`ShellKind` is `NONE`, `KDE`, or `GNOME`. `EndpointKind` is `WAYLAND` or `X11`.
A session can legitimately present several endpoints at once — a Wayland
compositor plus its XWayland display, for instance — so endpoints are a set,
not a choice.

### Discovery

`DiscoverEndpoints()` builds candidates from `DiscoveryHints`, whose values come
from the **systemd user manager environment** read at reconcile time, not from
the daemon's own environment: `WAYLAND_DISPLAY`, `DISPLAY`, `XAUTHORITY`. logind
is optional enrichment only — it has proven unreliable for this, so hints from
any source are distrusted and every candidate must prove itself by connecting.

A candidate that fails validation is not retried immediately. It backs off
`ENDPOINT_RETRY_BACKOFF_INITIAL_TICKS`, then doubles, capped at
`ENDPOINT_RETRY_BACKOFF_MAX_TICKS` (64). This matters in practice: an XWayland
display advertised in the environment that does not answer an XScreenSaver query
is quarantined rather than retried hot, and never contributes a value.

A live source is dropped once it both fails `IsAlive()` and has resolved
`IDLE_ERROR` `DEAD_SOURCE_ERROR_THRESHOLD` times consecutively. A shell that
disappears must be absent for `SHELL_ABSENCE_OBSERVATIONS_REQUIRED` consecutive
reconciles before its source is removed, so a momentary D-Bus hiccup does not
tear down a working session.

### Aggregation

Each source resolves to exactly one value; the pool never looks inside a
source's internal priority chain. `AggregateIdleSeconds()` then applies:

```
inhibited                -> 0                     (inhibition wins over everything)
no source present        -> IDLE_NO_GUI_SESSION   (-2)
otherwise                -> min(values >= 0)
all values negative      -> IDLE_ERROR            (-1)
```

The sentinels are negative precisely so they can be excluded from the minimum.
Letting `IDLE_ERROR` participate would convert one source's failure into a
global error, and `min(-1, 300)` would report the machine as active forever.

The two sentinels are distinct because the caller treats them differently:
`IDLE_NO_GUI_SESSION` means "there is no GUI session here, defer to
`event_detect`", while `IDLE_ERROR` means "there should be one and it is not
answering".

### Per-shell roles

The shell does not always supply an idle value. On KDE under Wayland the
compositor's `ext_idle_notifier_v1` endpoint supplies idle time and the shell
supplies only inhibition; on KDE under X11 `ksmserver` supplies both, since its
`GetSessionIdleTime` already accounts for inhibitors. GNOME supplies inhibition
via `org.gnome.SessionManager.IsInhibited` and idle time via
`org.gnome.Mutter.IdleMonitor`.

| session | idle value from | inhibition from |
|---|---|---|
| KDE Wayland (Plasma 6) | `ext_idle_notifier_v1` endpoint | KDE shell (PolicyAgent) |
| KDE X11 (Plasma 5/6) | KDE shell (`GetSessionIdleTime`) | handled inside ksmserver |
| GNOME | Mutter `IdleMonitor` | `org.gnome.SessionManager` |
| other Wayland | `ext_idle_notifier_v1` endpoint | none available |
| other X11 | XScreenSaver (`libXss`) | none available |
| tty only | *(no endpoints, no shell)* | — |

A tty-only session is not a special case in the code any more. It simply
produces no endpoints and no shell, so aggregation returns
`IDLE_NO_GUI_SESSION` and the caller falls back to `event_detect`, which is
where tty monitoring lives.

### Dependency separation

`idle_source`, `session_discovery` and `idle_source_pool` are dependency-free
and unit-testable. Every call into D-Bus, X11 and Wayland lives in
`idle_sources_system`, which is deliberately excluded from the test binary.
Factories are injected into the pool, so its behavior can be tested without a
display server.

## Inhibition Handling

Inhibition prevents the system from being considered idle even when there
is no user input (e.g., while watching a video). Each path handles this
differently:

| Path              | Inhibition Method                                      | Notes                                    |
|-------------------|--------------------------------------------------------|------------------------------------------|
| KDE Wayland       | `PolicyAgent.HasInhibition(1)` D-Bus call              | Explicit check, separate from idle query |
| KDE X11           | Built into `GetSessionIdleTime` return value           | ksmserver resets idle time when inhibited|
| GNOME Wayland/X11 | `org.gnome.SessionManager.IsInhibited` D-Bus call      | Explicit check before idle query         |
| Non-KDE X11       | Same GNOME check (fails gracefully if not GNOME)       | Falls through to XSS if not inhibited    |

### KDE PolicyAgent Inhibition Types

The `HasInhibition(uint types)` method uses a bitmask:
- **Type 1** — `ChangeScreenSettings`: prevents screen idle, blanking, lock
- **Type 2** — `InterruptSession`: prevents system suspend

idle_detect checks type 1 only, as we care about screen idle inhibition.


## ext_idle_notifier_v1 Protocol

Unlike X11's `XScreenSaverQueryInfo()` which returns the current idle time
on demand, the Wayland `ext_idle_notifier_v1` protocol is **callback-based**:

1. Client creates a notification with a timeout (idle_detect uses 1000ms)
2. Compositor sends `idled` event when user has been idle for that duration
3. Compositor sends `resumed` event when user becomes active again
4. Client tracks state transitions and calculates idle time from timestamps

The `WaylandIdleMonitor` class (`idle_detect.h/cpp`) runs its own thread
with a Wayland event loop, tracking `m_is_idle` and `m_idle_start_time`
as atomics. `GetIdleSeconds()` returns `current_time - m_idle_start_time`
when idle, or 0 when active.

### Compositor-specific behavior

The `ext_idle_notifier_v1` spec deliberately leaves "user activity"
compositor-defined. This means:
- Physical input (keyboard, mouse) always resets idle on all compositors
- `zwp_idle_inhibit_manager_v1` inhibitors are respected (Wayland-native)
- D-Bus-level inhibitors (`org.freedesktop.ScreenSaver.Inhibit`) may or
  may not be reflected — depends on compositor implementation. This is why
  idle_detect performs a separate D-Bus inhibition check on KDE Wayland.


## Device Re-enumeration (event_detect)

The `Monitor::EventActivityMonitorThread()` in event_detect re-enumerates
input devices every second by scanning `/sys/class/input/event*` for
entries with a `device/mouse*` child.

If the set of device paths changes (not just the count — paths are
compared directly), the monitor sends SIGHUP to the main thread, which
tears down all `EventRecorder` threads and restarts them with the new
device set.

Additionally, each `EventRecorder` sets a `m_device_lost` flag if it
receives `-ENODEV` from libevdev (device disconnected). The monitor
thread checks this flag each iteration as an additional trigger for
re-enumeration, so device loss is detected even when the device count
hasn't changed yet (e.g., between a disconnect and reconnect).


## Known Gaps and Platform Issues

### Plasma 5 to Plasma 6 Migration

- `org.kde.ksmserver` no longer implements `/ScreenSaver` or the
  `org.freedesktop.ScreenSaver.GetSessionIdleTime` method on Wayland
- `org.freedesktop.ScreenSaver.GetSessionIdleTime` exists on Plasma 6
  but returns "not supported on this platform" on Wayland sessions
- The ksmserver service still exists (so `IsKdeSession()` still works),
  but its idle-related functionality has moved to KWin

### VNC Virtual Input (KWin)

w0vncserver (TigerVNC 1.16.0) uses `org.freedesktop.portal.RemoteDesktop`
for input injection on KWin (since KWin doesn't implement the wlr virtual
input protocols `zwlr_virtual_pointer_v1` / `zwp_virtual_keyboard_v1`).

On KDE Plasma 6 (confirmed on openSUSE Leap 16.0), KWin correctly resets
its idle timer for portal-injected input. This means `ext_idle_notifier_v1`
accurately reflects VNC input activity — moving the mouse in VNC resets
idle, stopping movement allows idle to accumulate. No special VNC handling
is needed in idle_detect.

Note that event_detect's `/dev/input` monitoring does not see VNC input
(virtual input doesn't create `/dev/input` events), but this is handled
correctly because idle_detect's GUI idle time (via `ext_idle_notifier_v1`)
is combined with event_detect's hardware idle time via `std::min`.

Older Plasma versions may not reset idle on portal input — this was a
known KWin gap that appears to have been fixed upstream in Plasma 6.

### systemd-logind IdleSinceHint

The `org.freedesktop.login1` `IdleSinceHint` property is deliberately
not used. It is unreliable across session types and often does not
reflect actual user input activity, especially on Wayland or with
mixed session configurations.
