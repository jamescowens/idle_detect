# GUI Session Readiness and Multi-Endpoint Idle Detection

**Date:** 2026-08-16
**Status:** Implemented on `gui_session_readiness`. Corrected in place where the implementation
disproved the design; the corrections are marked "as implemented" and are load-bearing, not
editorial.
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
│  org.gnome.SessionManager (inhibition without an idle value)   │
│  Lifecycle: user@UID.service     Trigger: NameHasOwner probe   │
│  Supplies: inhibition (global override), idle on X11/GNOME     │
└────────────────────────────────────────────────────────────────┘
┌─ Layer 2: Graphical Endpoints (0..N, protocol) ────────────────┐
│  wayland-* sockets in $XDG_RUNTIME_DIR   →  ext_idle_notifier  │
│  X displays (union of hints)             →  XScreenSaver       │
│  Trigger: full discovery union, every reconcile                │
└────────────────────────────────────────────────────────────────┘
                            ↓
     inhibited ? 0 : min(resolved values of all live sources)
```

Ownership of the shell's well-known name on the user bus is the "GUI session established"
signal. It directly observes the event that matters — the desktop shell claiming its name — and
it is the same question whether it is subscribed to or asked. As implemented it is asked, once
per reconcile, alongside endpoint discovery; see "Triggers and data flow" for why, and for the
debounce that asking rather than subscribing turned out to need.

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

**When `-2` can and cannot fire.** `-2` is a statement about the *source set*, not about the
readings: it fires only when the pool holds nothing at all. `-1` is a statement about the readings:
sources exist, none produced a value. `main()` treats them differently on purpose. `-2` overrides
the `use_event_detect` config setting, because in that state the only activity there is to see is
tty and ssh and only `event_detect` can see it. `-1` does not override anything — a GUI session
demonstrably exists and only the reading failed, so whether to fall back to `event_detect` stays
the config's decision.

It follows that there are legitimate, correctly-working sessions where `-2` can never fire, and
they are not a fault:

- **KDE on Wayland with no validated endpoint.** The shell is a source. It contributes inhibition
  and no idle value, so it resolves `-1` on every tick, and the aggregate is `-1` forever. This is
  the "contributes inhibition only" row of the source-chain table below, working exactly as
  designed.
- **GNOME without mutter**, for the same reason.

In both, the pool is right to say `-1` rather than `-2`: there IS a GUI session, its shell is
answering, and the user has a working configuration in which `use_event_detect` — true by
default — supplies the reading. Nothing is being suppressed.

See "Error handling" for the narrower case that IS a fault: a source that is retained after it has
stopped working keeps the source set non-empty, so a session that genuinely has no readable source
reports `-1` when it should report `-2`, and the override never fires for the user who disabled
`use_event_detect`.

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
| Shell — GNOME with mutter (X11 or Wayland) | Mutter D-Bus `IdleMonitor` → `-1` |
| Shell — GNOME without mutter | No idle value; contributes inhibition only |
| Shell — KDE on Wayland | No idle value (`ksmserver GetSessionIdleTime` is gone on Plasma 6); contributes inhibition only |
| Shell — absent | No idle value, no inhibition |
| Endpoint — Wayland | `ext_idle_notifier_v1` → `-1` |
| Endpoint — X11 | `GetIdleTimeXss()` → `-1` |

Which of the two KDE rows applies is decided by whether discovery offered a **Wayland endpoint
candidate**, not by whether a Wayland source is live. The two are different questions, and only
the first one is about the session's protocol.

The two "contributes inhibition only" rows resolve to `-1`, which aggregation excludes, and that
is a correct outcome rather than a fault. Logging must keep them apart even though the sentinel
cannot: an arm that supplies no value by design says so at debug level, while an arm that
attempted a query and failed is reported on a throttled ladder.

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

New files. The stated reason was that `idle_detect.cpp` was already 2054 lines; **that reason was
not achieved, and it was the wrong reason.** `idle_detect.cpp` is 3194 lines on this branch, half
again as large as it started, with about 3,000 further lines in the eight new files beside it. It
grew because the work that landed in it — reconcile wiring, the discovery-input plumbing, the
XAUTHORITY hint, the log throttles — is new behaviour rather than behaviour moved out, and because
the Wayland monitor and the X11 query stayed put. Nothing was extracted from it to make it smaller.

The split that did get made was drawn on a different line, and it is the one worth keeping:
anything that makes system contact — D-Bus via GIO, Wayland, X11 — is confined to
`idle_sources_system.h/cpp`, and everything else is kept free of those dependencies so it links
into `idle_detect_tests`, which links none of those libraries. That is why `ShellMonitor` and the
three concrete `IdleSource` implementations live in the system file while the interface, the
discovery union, and the pool do not. Testability is the criterion; line count never was, and the
original rationale should be read as a placeholder for it rather than as a goal that was met.

The file attributions below are as implemented.

### `DiscoverEndpoints()` — `session_discovery.h/cpp`

Produces the endpoint set from a union of individually-distrusted hints:

- `wayland-*` sockets in `$XDG_RUNTIME_DIR`, excluding `*.lock`
- `DISPLAY` **and `WAYLAND_DISPLAY`** from the systemd user manager `Environment` property (read
  on demand), plus this process's own two as further hints. `WAYLAND_DISPLAY` earns its place
  because the socket scan matches a naming convention rather than a rule: `weston
  --socket=mysession`, a nested compositor, or a socket outside the runtime directory is named by
  nothing else. `XAUTHORITY` comes out of the same single `Properties.Get` result, since a
  discovered `DISPLAY` is useless without credentials that are not the ones the process was
  executed with.
- `/tmp/.X11-unix/X*` filtered by `st_uid == getuid()`
- logind `Session.Display` for `Class=user` ∧ `Type ∈ {wayland,x11}` — **optional enrichment**,
  and as implemented not gathered at all: every display it could name is already reachable
  through the X socket scan or the manager environment, so the field exists and is left empty.

Union, dedupe, then **validate by connecting**. No hint is required to be present. Validation is
performed by the pool's endpoint factory rather than here, since what "validated" means is
protocol-specific; this function is a pure, dependency-free set operation over hints, which is
what makes it unit-testable against temp dirs.

Re-run in full on every reconcile, which as implemented is every main-loop iteration. The inotify
watch on `$XDG_RUNTIME_DIR` was not built: at a one-second reconcile it would save a directory
scan and add a second code path with its own failure modes.

The X11 hints are unreliable in complementary ways, which is why they are unioned: a
user-started `Xvnc` leaves a user-owned socket the `st_uid` filter finds, while a
display-manager-started console X may leave a root-owned socket the filter misses but which
the `Environment` `DISPLAY` hint supplies.

### `ShellMonitor` — `idle_sources_system.h/cpp`

Detects the shell by bus name ownership and exposes shell kind and inhibition state. As
implemented it **polls** rather than subscribing to `NameOwnerChanged`; see "Triggers and data
flow" for why, and for the debounce that a polled probe turned out to need. It holds no state and
no connection and is constructed per call, GIO's shared session bus connection being what is
actually reused.

`DetectShellKind()` knows three names — `org.kde.ksmserver`, `org.gnome.Mutter.IdleMonitor` and
`org.gnome.SessionManager` — but does **not** probe all three per reconcile. It short-circuits on
the first owner it finds, in that order, so the per-tick cost is one `NameHasOwner` round trip on
KDE, two on GNOME with mutter, and three only on GNOME without mutter and on a session with no
shell at all. Ordering is therefore load-bearing for cost as well as for classification: KDE is
first because the old `IsKdeSession()` branch was, and mutter precedes gnome-session because it is
the common GNOME case and answers both questions at once. The GNOME shell factory issues one
further probe, `HasGnomeIdleMonitor()`, and only when a GNOME shell source is being built.

`org.gnome.SessionManager` is a third name the original sketch did not have. Presence for
inhibition and presence for an idle value are different questions: gnome-session answers
`IsInhibited` and is present in every gnome-session desktop, while mutter answers the idle time
and is present only when the window manager actually is mutter. GNOME Flashback with Metacity has
the first and not the second, and probing only for Mutter would silently drop its inhibition.

### `IdleSource` — `idle_source.h/cpp`, implementations in `idle_sources_system.h/cpp`

Abstract interface; each instance owns its full priority chain and resolves to one value. The
interface and the aggregation rule are dependency-free and live in `idle_source.h/cpp`; the three
implementations below all make system contact and therefore live in the system file.

- **`WaylandIdleSource`** — one per Wayland endpoint. Today's `WaylandIdleMonitor`, made
  instance-clean. One `wl_display`, one thread, one proxy graph per instance. No globals.
- **`X11IdleSource`** — one per X display. Wraps XScreenSaver. Stateless: `GetIdleTimeXss()`
  already does `XOpenDisplay` → query → `XCloseDisplay` on every call, so the connection
  does not outlive the call. Note that this makes the source unable to detect its own death, not
  immune to dying; see "Error handling".
- **`ShellIdleSource`** — at most one, driven by `ShellMonitor`. Resolves per the shell rows
  of the source-chain table. Not tied to any endpoint. Which shell row applies is not determined
  by shell kind alone, so the pool pushes the two facts it cannot know — whether this is a
  Wayland session, and whether mutter's `IdleMonitor` is on the bus — through a small
  `ShellSourceContext` interface. The Wayland one is derived from the endpoint **candidates**,
  not from live Wayland sources: a compositor we cannot read an idle time out of is still a
  compositor, and inferring the protocol from whether a source validated sent a Plasma 6 Wayland
  shell down the KDE-on-X11 arm, to a D-Bus method Plasma 6 had removed, once per tick.

### `IdleSourcePool` — `idle_source_pool.h/cpp`

Owns all sources, reconciles endpoint sources against `DiscoverEndpoints()` and the shell
source against `ShellMonitor`, applies the aggregation rules, and evaluates the inhibition
override ahead of them.

It has its own file, and that file is dependency-free: every piece of system contact arrives
through factories injected by `idle_detect.cpp` and implemented in `idle_sources_system.cpp`. The
pool's logic is the part of this design most likely to break in a way no compiler catches — a
source torn down and rebuilt on unrelated churn, a validation failure cached so an endpoint never
comes back, a shell value leaking into an endpoint's reading. Put in the system file, none of that
could be tested; put here, all of it is.

Reconcile runs **every main-loop iteration**, which is `check_interval_seconds`, currently fixed
at 1 s and not configurable. There is no `RECONCILE_INTERVAL_SECONDS`, and no separate reconcile
timer: the main loop reconciles and then resolves, so every reading comes from the sources that
exist now. See "Triggers and data flow" for what bounds the cost of that.

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
mechanism does not reason about tty at all; it has no concept of one. What it reasons about is an
empty source set — **no endpoints *and* no shell** — which is what returns `-2`, and a tty-only
session is simply the commonest way to arrive there. Getting this wrong in the earlier wording
mattered, because "no GUI endpoints found → `-2`" is a different and incorrect rule: a KDE session
on Wayland with no validated endpoint has no endpoints either, and it must yield `-1` rather than
`-2` because its shell is a live source. See the sentinel contract.

## Triggers and data flow

Everything is driven by the reconcile pass, and the reconcile pass runs on the main loop.

| Event | Detected by | Action |
|---|---|---|
| Shell name acquired/lost | `NameHasOwner` probe, each reconcile | Re-evaluate shell kind; create, replace or drop the shell source |
| Wayland socket created/removed | `$XDG_RUNTIME_DIR` scan, each reconcile | Add/evict Wayland source |
| X socket or `DISPLAY` appearing/disappearing | Socket scan and manager environment, each reconcile | Add/evict X11 source |
| Source reports itself dead | `IdleSource::IsAlive()`, each reconcile | Immediate teardown; endpoint becomes newly-seen |
| Source silently stops working | `DEAD_SOURCE_ERROR_THRESHOLD` consecutive `-1` resolutions | Teardown; endpoint becomes newly-seen |
| Reconcile tick (1 s) | Main loop | Re-run the full discovery union and the shell probe |

**The reconcile tick is the mechanism, not a backstop.** The signal-driven design above it —
`NameOwnerChanged` subscriptions, an inotify watch on `$XDG_RUNTIME_DIR` — was not built, and the
polled equivalent is better here rather than merely simpler. Convergence is one second instead of
thirty, there is one code path rather than a fast one plus a self-heal one that is exercised only
when the fast one has already failed, and a daemon that starts before its GUI session — the case
this design exists to fix — needs no special handling because the state it starts in is the same
state it is in on every other tick.

What was thirty seconds' job, bounding the cost of a session that never comes up, is done instead
by **per-candidate exponential backoff**. A candidate that fails validation is retried after 1
reconcile tick, then 2, 4, 8, and so on to a ceiling of `ENDPOINT_RETRY_BACKOFF_MAX_TICKS` = 64,
with the ladder cleared the moment it validates or leaves the candidate set. That bounds a
permanently-bad candidate at one attempt per 64 ticks forever, which is the case that actually
costs something: a stale `wayland-9` socket with no listener produced 87 journal lines in 8
seconds before this existed, and GNOME is not exotic — it advertises no `ext_idle_notifier_v1` at
all, so every Wayland candidate in a GNOME session is a permanent validation failure. A slow
global tick would have throttled the healthy path to pay for the broken one; the backoff charges
the broken candidate alone.

Two things a polled probe needed that a signal would not have:

- **Shell disappearance is debounced.** `DetectShellKind()` is a synchronous `NameHasOwner`, and a
  bus hiccup, a timeout under load or a session bus restart answers `NONE` for a session whose
  shell is fine. In a shell-only session that is the only source there is, so acting on one such
  answer empties the pool for a tick and flips the daemon to `event_detect`. An absence must be
  observed `SHELL_ABSENCE_OBSERVATIONS_REQUIRED` = 3 times consecutively before it is acted on.
  Only the disappearing direction is debounced; a shell appearing is acted on at once.
- **Repeated-failure logging is throttled everywhere it can recur.** Anything that logs
  unconditionally in this path logs once per second for the life of the process. Three ladders
  cover it, all with the same shape — report the first failure of a run, then double the number
  suppressed between reports to a ceiling of 64, and clear the whole thing on the first success:

  1. Candidate validation failures, on the backoff ladder that also defers the retry itself.
  2. The shell source, which is not a candidate and never passes through that ladder, counting its
     own consecutive failures. Only the logging is throttled here, never the query: a shell that
     starts answering again must be read on the tick it recovers.
  3. The main loop body, which was the last place holding out. Reconciling and resolving are the
     loop's job, so the loop is where "no source produced a value" and "event_detect could not be
     read" are decided, and both were reported unconditionally — one `ERROR` per second each, on
     any session where nothing resolves and on any machine without `event_detect` respectively.
     Sends to the `event_detect` pipe share a third instance of the ladder, since a pipe that is
     not there stays not there.

  The shape is written once, as `FailureReportThrottle` in `util.*`, which is in the test binary
  and therefore has tests; the two older ladders predate it and still carry their own copies. The
  first occurrence of each condition stays at `ERROR` so a genuine fault is immediately visible,
  the reports after it are follow-ups at normal level, and everything suppressed is debug material
  rather than discarded.

  Throttling is not the only tool. Per-tick helpers whose caller already reports a counted,
  throttled summary log their own detail at debug level instead — `GetIdleTimeKdeDBus()`,
  `GetIdleTimeWaylandGnomeViaDBus()`, `ReadTimestampViaShmem()` and `ReadLastActiveTimeFile()` all
  keep that contract. The rule is one operator-visible line per condition per ladder, owned by
  whoever can count the run.

**Startup is not special.** The daemon starts, runs discovery, and finds either zero or some
sources. An empty pool — no endpoints and no shell — is a normal state returning `-2`, not a
failure and not a reason to exit. The "started before the GUI" case is the ordinary path with a
later trigger. This removes the wrapper's reason to exist, and with it the cause of #12.

## Error handling

- Per-endpoint failures are contained to that endpoint: evict, do not propagate.
- Sentinels stay out of `min()` per the sentinel contract.
- The pool never exits the process on discovery failure.
- Wayland sources report their own death through `IsAlive()`: the monitor thread clears its
  availability flag on a hangup or on the removal of a global it depends on, and the pool tears
  the source down and rebuilds it through validation.
- X11 sources **cannot report** their own death, which is not the same as not having one. The
  connection is stateless, so there is no stale state for `IsAlive()` to inspect and it answers
  true forever. The endpoint underneath it is another matter, and the original claim that such a
  source "needs no liveness eviction" was wrong: SIGKILLing an X server unlinks nothing, so its
  socket stays in `/tmp/.X11-unix`, discovery goes on offering the candidate, and the source is
  retained on the strength of the key alone. Measured on `:47`: 14 consecutive `-1` resolutions
  with no teardown, and it would have continued for the life of the process.
- The generic rule that covers both, and that lives in the pool rather than in any source: after
  `DEAD_SOURCE_ERROR_THRESHOLD` = 3 consecutive `-1` resolutions an endpoint's source is torn
  down and its endpoint treated as newly-seen, subject to the backoff ladder. Any reading clears
  the run. Three rather than one, because a single failed reading is an ordinary transient and
  the response to it is to try again. The two mechanisms are complementary: `IsAlive()` catches
  sources that know they are dead, the error count catches sources that cannot tell.

**Residual risk:** a *live but wrong* source — an endpoint whose notifier never fires so idle
reads 0 forever. Under `min()` that pins DC to paused permanently, and neither mechanism above
sees it, because a source reporting 0 is by every available test working. Mitigation for the
cases that ARE detectable is strict liveness: an endpoint whose socket disappears, whose source
reports itself dead, or whose source stops producing readings is torn down rather than left
reporting. This is the same discipline #11 concerns.

**A retained dead source suppressing `-2` is the failure mode to watch.** Every one of the above
matters less for the readings it saves than for what a retained dead source does to
`any_source_present`: while it exists the pool reports `-1` where it should report `-2`, so
`main()` never overrides `use_event_detect`, and a user who set `use_event_detect=0` — the only
user for whom the override is load-bearing — is served by nothing at all.

Note the precision this needs, because the naive version of the claim contradicts the aggregation
rules. `-2` failing to fire is not in itself a fault: per "When `-2` can and cannot fire" above,
there are healthy sessions — KDE on Wayland with no validated endpoint, GNOME without mutter —
where a live, correctly-behaving shell source resolves `-1` forever and `-2` correctly never fires.
The fault is narrower than "`-2` did not fire". It is **a source that is counted while it is dead**:
the source set is non-empty only because nothing noticed that one of its members stopped working.
That is what `IsAlive()` and `DEAD_SOURCE_ERROR_THRESHOLD` exist to catch, and it is why the second
of them had to be added — an `X11IdleSource` against a `SIGKILL`ed X server answers `IsAlive()` true
forever and is exactly this case.

## Removals

- `idle_detect_wrapper.sh.in` — retired. `ExecStart` points at the binary directly. Requires
  updating `dc_idle_detection.service.in`, `CMakeLists.txt`, `install.sh`, and packaging.
- `getenv()`-based session typing (`IsTtySession`, `IsWaylandSession`).
- The `g_wayland_idle_monitor` global.

`ExecStartPre=/bin/sleep 5` should also be reconsidered once startup is no longer
order-sensitive. It was, and is gone: with reconcile running every second and an empty pool as a
normal starting state, there is nothing for the delay to wait for.

## Testing

The registry-free design leaves a clean seam: the aggregation rule and the discovery union
are both pure logic, testable without D-Bus, Wayland, or X11. This matters because the test
binary, which at the time linked only `util.cpp`, deliberately links no system libraries.

**Unit, no dependencies:**

- Aggregation table: every row, plus `min()` ignoring `-1`, `-2` surviving distinct from
  `-1`, inhibition short-circuiting ahead of all sources, empty-set behavior.
- Source chain fidelity with injected fake sources: each row of the source-chain table,
  asserting today's ordering is preserved — especially KDE-X11 taking `GetIdleTimeKdeDBus()`
  with no separate inhibition call. **Not built, and cannot be, as written.** The chains belong to
  `ShellIdleSource`, which lives in `idle_sources_system.cpp` because it makes D-Bus contact, and
  that file is deliberately excluded from `idle_detect_tests`. An injected fake source can stand in
  for a chain but cannot *be* one, so a fake proves nothing about which D-Bus method a real arm
  calls. Splitting the arms out from the calls to make them injectable would move the untested
  boundary rather than remove it.

  What covers it instead, in three parts:

  - **The half about inhibition is now structural rather than per-arm, and is tested.** Inhibition
    is evaluated once by the pool through an injected `InhibitionQuery` and short-circuits ahead of
    every source, so no chain contains an inhibition call for a fidelity test to catch.
    `InhibitionShortCircuitsToZero`, `InhibitionIsQueriedWithTheDetectedShellKind` and
    `InhibitionStillAppliesWhenTheShellSourceCouldNotBeCreated` assert it.
  - **The half that actually broke — *which* row applies — is tested in the pool**, because the
    inputs that select the row are pushed in by the pool through `ShellSourceContext`. The KDE row
    is selected by whether a Wayland endpoint *candidate* exists, and that was got wrong once, in
    exactly the direction that sent a Plasma 6 Wayland shell down the KDE-on-X11 arm. Five tests
    pin it: `WaylandEndpointPresenceIsPushedToTheShellSource`,
    `AWaylandCandidateThatFailsValidationStillMarksTheShellAsWayland`,
    `AnAllX11CandidateSetDoesNotMarkTheShellAsWayland`,
    `TheWaylandFlagFollowsCandidatesAcrossReconciles` and
    `AShellSourceBuiltThisTickAlreadyKnowsTheSessionIsWayland`.
  - **The remainder — which D-Bus method each arm issues — is covered only by review and by the
    manual runs below.** That is an honest gap, and it is the price of the isolation rule. It is a
    tolerable one: each arm is a handful of straight-line lines with no branching left in it once
    the row is chosen, and a wrong method fails loudly and immediately on a live session rather
    than subtly.
- The multi-endpoint regression this design exists to prevent: two X11 endpoints with a KDE
  shell present, where one endpoint is active and the other idle, must report the active
  one. This fails if the shell is ever fused into endpoint chains.
- Discovery union with temp dirs: `*.lock` filtered, foreign-uid X sockets rejected,
  complementary hints merging, dedupe.
- Pool reconciliation, which the seam turned out to make testable in full and which is where the
  defects actually were: eviction on a disappeared endpoint, retention of a live one across
  unrelated churn, teardown and rebuild on `IsAlive()` false and on a run of errors, the backoff
  ladder and every way it resets, the shell disappearance debounce, and the Wayland-session flag
  following candidates rather than live sources.

**Manual / integration:**

- Cold start before GUI (the #12 reproduction).
- Compositor kill and restart.
- `vncserver :2` alongside a console session.
- Nested weston for the N>1 Wayland path.
- A **crashed** X server, `SIGKILL`ed so its socket is left behind. Distinct from a clean exit,
  and the case that disproved "X11 sources need no liveness eviction".

The test binary links `idle_source.cpp`, `session_discovery.cpp` and `idle_source_pool.cpp`, none
of which include D-Bus, X11, Wayland or GLib headers; `idle_sources_system.cpp` holds everything
that does and is never linked into it. The dependency-free three are where the logic worth
asserting on lives, which is the point of drawing the line there rather than at the `IdleSource`
interface alone.

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
