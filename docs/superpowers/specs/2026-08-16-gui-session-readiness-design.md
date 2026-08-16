# GUI Session Readiness and Multi-Endpoint Idle Detection

**Date:** 2026-08-16
**Status:** Design approved, pending implementation plan
**Related issues:** [#12](https://github.com/jamescowens/idle_detect/issues/12) (root cause), [#11](https://github.com/jamescowens/idle_detect/issues/11) (prerequisite)

## Problem

`idle_detect` permanently reports TTY-only idle detection when the user service starts
before the GUI session is established. Users observe that only terminal typing registers
as activity; desktop input is ignored for the lifetime of the process.

### Root cause

Session type is determined from the **process environment**, which is frozen at `exec`:

```cpp
// idle_detect.cpp
static bool IsTtySession() {
    const char* display = getenv("DISPLAY");
    const char* wayland_display = getenv("WAYLAND_DISPLAY");
    return (display == nullptr || strlen(display) == 0) &&
           (wayland_display == nullptr || strlen(wayland_display) == 0);
}
```

`GetIdleTimeSeconds()` re-evaluates `IsTtySession()` / `IsKdeSession()` / `IsWaylandSession()`
on every poll, so routing is already dynamic. But a running process can never observe
`DISPLAY` or `WAYLAND_DISPLAY` appearing later. `IsTtySession()` returns true forever,
`GetIdleTimeSeconds()` returns `-2` forever, and the tty monitor in `event_detect` becomes
the only live source.

### Why the existing wrapper does not save us

`idle_detect_wrapper.sh.in` polls `systemctl --user show-environment` for
`DBUS_SESSION_BUS_ADDRESS` and `DISPLAY`/`WAYLAND_DISPLAY`, logs them on success, and then:

```sh
exec @CMAKE_INSTALL_FULL_BINDIR@/idle_detect
```

It never imports them. The wrapper's own environment was fixed when systemd spawned the
unit; variables entering the user manager environment afterward are visible to
`show-environment` but are **not** retroactively injected into the already-running wrapper.
The wrapper correctly waits for the GUI session, correctly detects it, and then execs the
binary with a stale environment. Detection works; propagation is missing.

### Ordering precondition

The failure requires the user service to start before the GUI session. This happens when
any user service activates the logind user context ahead of GUI initialization, so
`dc_idle_detection.service` starts with no window manager in context.

Rejected remedies:

- **Demote to an autostart `.desktop` entry** — fragile in other ways; explicitly out of scope.
- **Rely on `graphical-session.target` ordering** — unreliable in `--user` space.

## Environment findings

Verified on `jco-monster-ng` (openSUSE, Plasma 6 Wayland) on 2026-08-16. These constrain
the design and several of them invalidate otherwise-obvious approaches.

### `systemd1.Manager.Environment` emits no change signal

```xml
<property name="Environment" type="as" access="read">
 <annotation name="org.freedesktop.DBus.Property.EmitsChangedSignal" value="false"/>
</property>
```

It must be polled or read on demand. A signal-driven subscription to the user manager
environment is not available.

### logind emits usable signals, but is not the authority we need

`org.freedesktop.login1.Manager` emits `SessionNew`, `SessionRemoved`, and
`PropertiesChanged`. `login1.Session.Type` / `.Active` / `.Display` and `login1.User.Display`
carry no suppressing annotation and emit normally. `login1.User.Sessions` and `.State` are
annotated `false`.

### The desktop shell cannot be attributed to a logind session

```
ksmserver     → /user.slice/user-1000.slice/user@1000.service/session.slice/plasma-ksmserver.service
kwin_wayland  → /user.slice/user-1000.slice/user@1000.service/session.slice/plasma-kwin_wayland.service
plasmashell   → /user.slice/user-1000.slice/user@1000.service/session.slice/plasma-plasmashell.service
```

`GetSessionByPID(ksmserver)` returns `org.freedesktop.login1.NoSessionForPID`.

On a systemd-managed desktop the shell **and the compositor** live under
`user@UID.service`, not in the logind session cgroup. The desktop shell is structurally a
**per-user singleton**. It cannot be correlated to a logind session, and the design must
not try.

### D-Bus idle sources are per-user, not per-session

The session bus is the per-user bus at `/run/user/UID/bus`. Only one process can own
`org.kde.ksmserver` or `org.gnome.Mutter.IdleMonitor`. Concurrent GUI sessions for the same
user cannot each have a shell-provided idle source.

### Endpoint enumeration hazards

- `$XDG_RUNTIME_DIR` contains both `wayland-0` and `wayland-0.lock`. Lock files must be filtered.
- `/tmp/.X11-unix` is world-visible and contains **other users'** sockets (`X0` root-owned
  greeter alongside user-owned `X1`). Filesystem enumeration must filter on
  `st_uid == getuid()`, and even then is incomplete for display-manager-started X servers.
- A user's logind session list is mostly noise: three `tty` sessions and one `Class=manager`
  session alongside the single `Type=wayland` session. Filtering on `Class=user` and
  `Type ∈ {wayland, x11}` is mandatory if logind is consulted.

### Deployment context

There is exactly **one** `dc_idle_detection.service` process per user. The systemd user
manager is per-user, not per-session, so one process must serve all of that user's GUI
sessions.

## Governing principle

**Every discovery source is assumed unreliable. Nothing is trusted until a connection to it
succeeds. Discovery is a union of hints, validated by connecting.**

No single source — not logind, not the systemd user manager environment, not `getenv` — is
load-bearing on its own. The current bug is precisely a violation of this: a single source,
captured once, never revalidated.

This is not a general-purpose principle imported from elsewhere. This project exists because
logind's own idle detector was demonstrated not to work, and the codebase already bypasses
several such facilities. The design encodes that operational experience.

## Architecture

Two layers, asymmetric by nature. This is **not** N symmetric sessions; it is **one shell and
N endpoints**.

```
┌─ Layer 1: Desktop Shell (per-user singleton, D-Bus) ───────────┐
│  org.kde.ksmserver / org.gnome.Mutter.IdleMonitor              │
│  Lifecycle: user@UID.service     Trigger: NameOwnerChanged     │
│  Supplies: inhibition (global override), idle on X11/GNOME     │
└────────────────────────────────────────────────────────────────┘
┌─ Layer 2: Graphical Endpoints (0..N, protocol) ────────────────┐
│  wayland-* sockets in $XDG_RUNTIME_DIR   →  ext_idle_notifier  │
│  X displays (union of hints)             →  XScreenSaver       │
│  Trigger: inotify on $XDG_RUNTIME_DIR + periodic reconcile     │
└────────────────────────────────────────────────────────────────┘
                            ↓
     inhibited ? 0 : min(resolved values of all live sources)
```

`NameOwnerChanged` on the user bus is the "GUI session established" trigger. It is
signal-driven, requires no polling, and directly observes the event that matters — the
desktop shell claiming its well-known name.

### Layer overlap is intentional and safe

On GNOME Wayland, Mutter's `IdleMonitor` and the local compositor's `ext_idle_notifier_v1`
describe the same screen. Rather than correlating them — which the cgroup finding says is
not reliably possible — both report and the aggregate takes the minimum. `min()` of two
readings of one screen is that screen. This removes the correlation problem entirely.

## Aggregation

### Priority within a source, `min()` across sources

The existing fallback chain is a **priority list**: first working source wins. `min()` is
not priority — it is biased toward "active", so any single misbehaving source reporting a
low idle value pins the machine to active forever and DC never runs. Priority tolerates a
broken source; `min()` is dominated by one.

Therefore each source independently resolves to exactly one idle value using its own
priority chain, reproducing today's logic. Only those single resolved values are
aggregated. **`min()` never sees the inside of a fallback chain.**

### Sentinel contract

`-1` and `-2` never enter `min()`. `min(-1, 300)` is `-1`, which would silently convert one
endpoint's error into a global error. `-2` must remain distinct from `-1`: it means "defer
to `event_detect` unconditionally, overriding the `use_event_detect` config setting", not
"failure". This distinction is load-bearing in `main()`.

| Condition | Result |
|---|---|
| Shell reports inhibited | `0` — short-circuit, before any source is consulted |
| ≥1 source yields a value ≥ 0 | `min()` over those values only |
| Sources exist but all yield `-1` | `-1` (error) |
| No endpoints and no shell | `-2` (defer to `event_detect`) |

"No endpoints and no shell" is the only path to `-2`. A shell present with no endpoints, or
endpoints present with no shell, both yield a real result or `-1`.

### The shell is a source, not a property of an endpoint

The shell must **not** be fused into an endpoint's chain. With N X11 endpoints and one
shell, at most one endpoint is the shell's screen, and the cgroup finding establishes that
we cannot determine which. Applying `GetIdleTimeKdeDBus()` to every X11 endpoint would make
them all report the console's idle time and silently discard a VNC endpoint's real
activity — defeating the multi-endpoint requirement.

The shell is therefore its own source with its own chain, contributing one value to the
aggregate alongside the endpoints. Endpoints are protocol-only.

### Source chains

Each chain resolves to exactly one value. Only these resolved values enter `min()`.

| Source | Chain |
|---|---|
| Shell — KDE on X11 | `GetIdleTimeKdeDBus()` — inhibition handled internally, **no** separate check |
| Shell — GNOME (X11 or Wayland) | Mutter D-Bus `IdleMonitor` → `-1` |
| Shell — KDE on Wayland | No idle value (`ksmserver GetSessionIdleTime` is gone on Plasma 6); contributes inhibition only |
| Shell — absent | No idle value, no inhibition |
| Endpoint — Wayland | `ext_idle_notifier_v1` → `-1` |
| Endpoint — X11 | `GetIdleTimeXss()` → `-1` |

Inhibition is evaluated separately from the chains and is a **global override**:
`CheckKdeInhibition()` or `CheckGnomeInhibition()` per shell kind, short-circuiting the
whole aggregate to `0`. There is one shell, and inhibition means "do not let this machine go
idle". Shell-less endpoints get no inhibition awareness, which is correct rather than a
gap — there is no shell to inhibit.

### Behavioral equivalence to today

This structure reproduces current behavior in every case:

- **KDE Wayland** — today: `CheckKdeInhibition()` → `ext_idle_notifier_v1`. Now: shell
  supplies the inhibition override, the Wayland endpoint supplies `ext_idle_notifier_v1`.
  Identical.
- **GNOME Wayland** — today: inhibition → Mutter → `ext_idle_notifier_v1` fallback. Now:
  `min(Mutter, ext_idle_notifier_v1)`. These describe the same screen, so the minimum is
  that screen. Per the #11 report GNOME does not advertise `ext_idle_notifier_v1` at all, in
  which case only Mutter contributes.
- **Non-KDE X11** — today: `CheckGnomeInhibition()` → `GetIdleTimeXss()`. Now: same, with
  Mutter additionally contributing if a GNOME shell is present.
- **KDE X11** — today: `GetIdleTimeKdeDBus()` only; XSS is never consulted. Now XSS
  participates as a corroborating peer. This is the **one intentional behavioral delta**.
  It is benign: `ksmserver`'s idle time is XSS-derived on X11 so the two agree, and when KDE
  inhibits, `GetIdleTimeKdeDBus()` returns `0`, which wins the `min()` and correctly reports
  active regardless of what XSS says.

## Components

New files, since `idle_detect.cpp` is already 2054 lines.

### `EndpointDiscovery` — `session_discovery.h/cpp`

Maintains the endpoint set from a union of individually-distrusted hints:

- `wayland-*` sockets in `$XDG_RUNTIME_DIR`, excluding `*.lock`
- `DISPLAY` from the systemd user manager `Environment` property (read on demand)
- `/tmp/.X11-unix/X*` filtered by `st_uid == getuid()`
- logind `Session.Display` for `Class=user` ∧ `Type ∈ {wayland,x11}` — **optional enrichment**

Union, dedupe, then **validate by connecting**. No hint is required to be present. Triggered
by inotify on `$XDG_RUNTIME_DIR` plus the reconcile timer.

The X11 hints are unreliable in complementary ways, which is why they are unioned: a
user-started `Xvnc` leaves a user-owned socket the `st_uid` filter finds, while a
display-manager-started console X may leave a root-owned socket the filter misses but which
the `Environment` `DISPLAY` hint supplies.

### `ShellMonitor` — `session_discovery.h/cpp`

Watches `NameOwnerChanged` for `org.kde.ksmserver` and `org.gnome.Mutter.IdleMonitor`.
Exposes shell kind, inhibition state, and the D-Bus idle value where one exists.

### `IdleSource` — `idle_source.h/cpp`

Abstract interface; each instance owns its full priority chain and resolves to one value.

- **`WaylandIdleSource`** — one per Wayland endpoint. Today's `WaylandIdleMonitor`, made
  instance-clean. One `wl_display`, one thread, one proxy graph per instance. No globals.
- **`X11IdleSource`** — one per X display. Wraps XScreenSaver. Stateless: `GetIdleTimeXss()`
  already does `XOpenDisplay` → query → `XCloseDisplay` on every call, so the connection
  does not outlive the call.
- **`ShellIdleSource`** — at most one, driven by `ShellMonitor`. Resolves per the shell rows
  of the source-chain table. Not tied to any endpoint.

### `IdleSourcePool`

Owns all sources, reconciles endpoint sources against `EndpointDiscovery` and the shell
source against `ShellMonitor`, applies the aggregation rules, and evaluates the inhibition
override ahead of them.

The reconcile backstop interval is a named constant, `RECONCILE_INTERVAL_SECONDS = 30`.

### `GetIdleTimeSeconds()`

Becomes a thin delegation to the pool, preserving the exact sentinel contract.

## Multiplicity decisions

**N X11 endpoints.** A user at an X11 console plus a TigerVNC/xrdp remote administration
login is a real and common configuration — `vncserver` creates a genuine second display
(`:2`, user-owned socket) concurrent with the console's `:1`. Multi-instance X11 is nearly
free and carries no lifetime hazard, because `X11IdleSource` is stateless.

Note the distinction: VNC **sharing** an existing display (x11vnc, krfb) is *not* a second
endpoint — it injects input via XTEST, which resets the X server's idle counter, so the
existing endpoint already observes that activity. Only VNC with a **virtualized screen**
creates a second endpoint.

**N Wayland endpoints, N=1 in practice.** A Wayland compositor generally owns a DRM device
and seat, so a second concurrent compositor requires deliberate headless configuration.
Confirmed behavior on Plasma 6: VNC into a Wayland session works through the *existing*
`ext_idle_notifier_v1` because KWin resets the idle timer on RemoteDesktop portal input —
one compositor, one socket, one endpoint.

The single-Wayland assumption is nonetheless **not hard-coded**, because not hard-coding it
is free. `WaylandIdleMonitor` must be de-globalized regardless to fix #11, and once
instance-clean, a pool holding one source versus N is the same code. Hard-coding would
require an explicit "pick one socket" selection heuristic — strictly more code. Exceptions
that would produce N>1: GNOME Remote Desktop headless mode, and nested compositors.

**One shell**, per the per-user-bus finding.

## Session lifecycle

**VT switch is not teardown.** On a VT switch away, the logind session persists with
`Type=wayland`; only `Active` flips to false. `Session.Active` is therefore **not** a
teardown trigger.

**Switch-user requires no code.** Switching to a different user leaves our session alive
with `Active=false` while its idle counter climbs naturally, so DC correctly resumes — the
user genuinely is not typing. Same-user second sessions are degenerate: display managers
reattach rather than spawn a second session, and if one appeared its shell could not own the
bus name, so it lands in Layer 2 with no special case.

**Teardown** means the endpoint is gone: socket removed, or connection error. Crash and
logout-to-greeter are the cases that matter.

**TTY/VT sessions are out of scope** — handled by `event_detect`'s tty detector, which is
optional and which some users deliberately disable so terminal access does not stop DC. This
mechanism reasons about tty only as "no GUI endpoints found → return `-2`".

## Triggers and data flow

No polling in steady state.

| Event | Source | Action |
|---|---|---|
| Shell name acquired/lost | `NameOwnerChanged` on user bus | Re-evaluate shell kind; rebuild affected chains |
| Wayland socket created/removed | inotify on `$XDG_RUNTIME_DIR` | Add/evict Wayland source |
| Connection error on a source | The source itself | Immediate teardown and evict |
| Reconcile tick (~30 s) | Timer | Self-heal backstop; re-run full discovery union |

The reconcile tick is a backstop, not the mechanism. If every signal source fails, the
daemon still converges within ~30 s rather than hanging in tty mode forever. That is the
property the current wrapper lacks.

**Startup is not special.** The daemon starts, runs discovery, and finds either zero or some
endpoints. Zero is a normal state returning `-2` — not a failure, not a reason to exit. The
"started before the GUI" case is the ordinary path with a later trigger. This removes the
wrapper's reason to exist, and with it the cause of #12.

## Error handling

- Per-endpoint failures are contained to that endpoint: evict, do not propagate.
- Sentinels stay out of `min()` per the sentinel contract.
- The pool never exits the process on discovery failure.
- X11 sources cannot go stale by construction (open/query/close per call).
- Wayland sources are evicted on the first protocol error.

**Residual risk:** a *live but wrong* source — a stale `ext_idle_notifier` on a dead
compositor, or an endpoint whose notifier never fires so idle reads 0 forever. Under `min()`
either pins DC to paused permanently. Mitigation is strict liveness: any endpoint whose
connection errors or whose socket disappears is torn down and evicted immediately rather
than left reporting. This is the same discipline #11 concerns.

## Removals

- `idle_detect_wrapper.sh.in` — retired. `ExecStart` points at the binary directly. Requires
  updating `dc_idle_detection.service.in`, `CMakeLists.txt`, `install.sh`, and packaging.
- `getenv()`-based session typing (`IsTtySession`, `IsWaylandSession`).
- The `g_wayland_idle_monitor` global.

`ExecStartPre=/bin/sleep 5` should also be reconsidered once startup is no longer
order-sensitive.

## Testing

The registry-free design leaves a clean seam: the aggregation rule and the discovery union
are both pure logic, testable without D-Bus, Wayland, or X11. This matters because the
existing test binary deliberately links only `util.cpp` for that reason.

**Unit, no dependencies:**

- Aggregation table: every row, plus `min()` ignoring `-1`, `-2` surviving distinct from
  `-1`, inhibition short-circuiting ahead of all sources, empty-set behavior.
- Source chain fidelity with injected fake sources: each row of the source-chain table,
  asserting today's ordering is preserved — especially KDE-X11 taking `GetIdleTimeKdeDBus()`
  with no separate inhibition call.
- The multi-endpoint regression this design exists to prevent: two X11 endpoints with a KDE
  shell present, where one endpoint is active and the other idle, must report the active
  one. This fails if the shell is ever fused into endpoint chains.
- Discovery union with temp dirs: `*.lock` filtered, foreign-uid X sockets rejected,
  complementary hints merging, dedupe.

**Manual / integration:**

- Cold start before GUI (the #12 reproduction).
- Compositor kill and restart.
- `vncserver :2` alongside a console session.
- Nested weston for the N>1 Wayland path.

The test binary links the two new files. They depend on D-Bus/Wayland/X11 only behind the
`IdleSource` interface, so the pure-logic parts remain linkable on their own.

## Sequencing

1. **Land #11's lifetime fixes first**, as a standalone bugfix against `master`. Penumbra69's
   patch is independently correct and the crash is live for users now. Deferring it until
   this refactor lands makes affected users wait for all of it, and entangles a targeted fix
   in a large change. The GNOME eager-startup skip in that patch is a separate behavioral
   decision and should be evaluated on its own merits.
2. Build the multi-endpoint work on top, starting from a de-globalized `WaylandIdleMonitor`.

## Out of scope

- Changes to `event_detect`, its tty monitoring, or the shared-memory contract.
- The BOINC shmem layout, which is frozen at `int64_t[2]`.
- Non-systemd init systems. The project already ships systemd units exclusively.
- Adding `event_count_files_path` to `idle_detect.conf.in`. That is a real consistency gap
  reported in #12 and should be fixed separately, but it is not the cause of the reported
  symptom: `ProcessArgs()` defaults it to `/run/event_detect` and `Config::GetArgString`
  returns the default cleanly on a missing key.
