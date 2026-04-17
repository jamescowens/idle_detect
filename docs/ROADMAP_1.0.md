# Roadmap toward idle_detect 1.0

Curated from external technical reviews plus internal observations
accumulated during the 0.9.0.0 release cycle. The tiers reflect what
would actually block a "1.0 / stable" label rather than strict
chronological phases. Items can land in any order within a tier.

---

## Tier 1 — Correctness (must-have for 1.0)

Real bugs or gaps that would be awkward to ship under a stable label.

### 1. Shared-memory torn-read mitigation

The `SharedMemoryTimestampExporter` writes two `int64_t` values
non-atomically. The shmem data contract (`int64_t[2]` =
`{update_time, last_active_time}`) is frozen — BOINC's reader depends
on it and cannot be changed. However, *store order* is not part of
the contract, so we can still meaningfully improve the failure mode
without breaking any existing reader.

**Writer-side change:**

- Write `last_active_time` first, emit a
  `std::atomic_thread_fence(std::memory_order_release)`, then write
  `update_time` last. `update_time` becomes the implicit publication
  marker.
- Result for naive readers (BOINC): a torn read now yields
  `{stale update_time, fresh last_active_time}` — freshness check
  shows ≤1s old (prior update cycle), idle calculation uses the
  correct current value. Much safer failure mode than the current
  ordering (which can produce a false-idle observation during a tear).

**Reader-side opt-in (for consumers we control):**

- `read_shmem_timestamps` (and any future first-party consumer) does a
  seqlock-style read: read `update_time_1`, fence, read
  `last_active_time`, fence, read `update_time_2`; retry while the
  two `update_time` reads differ.
- Gets fully torn-read-free behavior with zero contract change.
  BOINC's path is unaffected.

### 2. Core-logic test coverage

The codebase is I/O-heavy and system-dependent — D-Bus, Wayland,
XScreenSaver, libevdev against `/dev/input`, pts/tty access times — and
much of that can only be exercised on a real running desktop session.
The existing test binary deliberately links only `util.cpp` to keep it
CI-runnable. 1.0 doesn't aim for broad unit test coverage; it aims for
a *pragmatic* coverage target with the scope below.

**Target:** roughly 60% line coverage, with close to 100% of the
aggregation, state-machine, and parsing logic covered. Explicitly not
chasing total coverage.

**In scope — pure logic, tested in the `util.cpp`-only test binary:**

Most of the decision logic is algorithmically pure, it just doesn't
currently live in a pure place. The plan is to extract it first, then
test it:

- Aggregation math in `Monitor::EventActivityMonitorThread` (max of
  the three sources, `FORCED_ACTIVE` / `FORCED_IDLE` overrides):
  extract into a free function or small struct that takes
  `{event_time, tty_time, idle_detect_time, state}` and returns the
  computed `last_active_time`.
- Device-path-change detection (the `event_devices !=
  event_devices_prev` comparison) — extract; test with synthetic path
  vectors.
- Seqlock reader and writer from Item 1 — pure algorithm; correctness
  provable with a concurrent-write test.
- Pipe-message parser: the existing `EventMessage` tests, extended
  with malformed inputs, oversized payloads, boundary timestamps.

**In scope — real OS primitives, tested in CI against the actual
kernel interfaces:**

POSIX shm and named pipes work fine in any Linux CI container; they
are filesystem-like, not session-dependent. Real round-trips against
`/dev/shm` and a temp FIFO are feasible. Only the clock needs to be
faked.

**In scope — end-to-end smoke harness, runnable locally or in a VM
before cutting a release:**

A small script (bash or Python) versioned in the repo that spins up
`event_detect` in a tmp prefix, fakes a session for `idle_detect`,
writes synthetic activity via the named pipe, reads the shmem, and
asserts on the values. Not run automatically in CI — formalizes the
manual pre-release check.

**Out of scope — accepted as manual-integration-only:**

- D-Bus idle sources (`GetIdleTimeKdeDBus`, Mutter, GNOME session
  inhibit) — would need a mock session bus or real desktop.
- `ext_idle_notifier_v1` — needs a real Wayland compositor implementing
  the protocol.
- XScreenSaver — needs a real X server with the extension.
- `libevdev` against actual `/dev/input/event*` — needs real devices.

These stay in a release checklist, exercised manually on real
desktops (KDE Wayland, KDE X11, GNOME, other Wayland, X11-only) before
tagging.

**Hard dependency:** this work requires the refactor items first. As
the code currently stands, extracting the aggregation math means
untangling it from D-Bus/Wayland/libevdev-linked code in 90KB
translation units. The file split (Item 5) and the encapsulation work
(Item 3) are preconditions — not nice-to-haves — for the pure-logic
extraction to be tractable.

### 3. Encapsulation of thread / CV / atomic members

Thread handles (`m_monitor_thread`, `m_event_recorder_thread`, ...),
condition variables, and interrupt atomics are currently `public` on
their owning classes. Callers can `.join()` a thread, `.notify_all()`
a CV, or flip an interrupt flag directly — bypassing shutdown
invariants and making it harder to reason about class lifetime.

- Make them private; expose `Start()`, `Stop()`, `IsRunning()` on the
  owning class.
- `WaylandIdleMonitor`'s C-callback publics (`m_seat`,
  `m_idle_notifier`, `m_is_idle`, `m_idle_start_time`) need a
  different treatment — the conventional fix is a tiny opaque struct
  passed as `user_data`, or a `friend` function for the callbacks.

---

## Tier 2 — Maintainability (should-have for 1.0)

Not bugs, but "1.0" implies the code is something a new contributor can
navigate without first internalizing 90KB of prose.

### 5. Split monolithic source files and de-flatten the repo

Bundle two related restructures into a single PR so the tree only
reshuffles once.

**Source-file split.** `idle_detect.cpp` at ~90KB and `event_detect.cpp`
at ~53KB are too large. Proposed decomposition (names approximate):

- `event_detect.cpp` → `event_detect_main.cpp`, `monitor.cpp`,
  `event_recorder.cpp`, `tty_monitor.cpp`, `idle_detect_monitor.cpp`
- `idle_detect.cpp` → `idle_detect_main.cpp`,
  `wayland_idle_monitor.cpp`, `idle_detect_control_monitor.cpp`,
  `dbus_idle_sources.cpp`

The `goto start_failed:` label in `WaylandIdleMonitor` gets eliminated
as part of this — the natural split point is a clean time to
restructure the init control flow with RAII cleanup instead of a
label. (Don't introduce new `goto`s elsewhere in the restructure;
project preference is to use RAII / early return / small helpers.)

**Repo de-flatten.** Move from the current flat-root layout to the
structure below. Packaging tools (`dpkg-buildpackage`, OBS, makepkg)
have path conventions that require `debian/`, `.github/`,
`CMakeLists.txt`, `README.md`, and `LICENSE.md` to stay at the root;
install scripts stay at root for discoverability.

```
/
├── src/
│   ├── common/          # util.{cpp,h}, tinyformat.h, release.h
│   ├── event_detect/    # event_detect, monitor, event_recorder,
│   │                    #   tty_monitor, idle_detect_monitor
│   ├── idle_detect/     # idle_detect, wayland_idle_monitor,
│   │                    #   idle_detect_control_monitor,
│   │                    #   dbus_idle_sources
│   └── tools/           # read_shmem_timestamps
├── scripts/             # dc_pause, dc_unpause, force_state.sh,
│                        #   event_detect_control.sh,
│                        #   *_resources.sh,
│                        #   idle_detect_new_user_setup.sh
├── systemd/             # *.service.in, *.preset
├── config/              # *.conf.in,
│                        #   idle-detect-autostart.desktop.in,
│                        #   idle_detect_wrapper.sh.in
├── cmake/               # uninstall.cmake.in
├── tests/               # already exists
├── docs/                # Doxyfile moves here
├── protocols/           # already exists
├── .github/             # already exists
├── debian/              # must stay at root
├── CMakeLists.txt
├── install.sh
├── user_install.sh
├── README.md
├── LICENSE.md
└── CLAUDE.md
```

`deprecated/` and `popular_distribution_required_modules_to_build.txt`
get deleted in the same PR (formerly a Tier 3 line item — promoted
here since we're restructuring anyway).

**Impact surface:**

- `CMakeLists.txt`: every `configure_file`, `install`, source-list,
  and include path needs updating. Large but mechanical.
- Packaging: zero impact. `debian/rules`, the `.spec`, and the
  `PKGBUILD` all drive CMake and don't reference internal repo paths.
- Documentation: `README.md` and `docs/idle_detection_logic.md` may
  reference a handful of source paths that need touching up.

### 6. Unify duplicated `State` enum

`IdleDetectMonitor::State` (in `event_detect.h`) and
`IdleDetectControlMonitor::State` (in `idle_detect.h`) are identical
enums with identical `StateToString()` methods. Hoist to `util.h` (or
a new `state.h`) to prevent drift.

### 7. `std::regex` on the device-enumeration hot path

`FindDirEntriesWithWildcard` in `util.cpp` constructs a `std::regex`
per call, and the monitor thread invokes it every second.
`std::regex` construction is notoriously expensive. Replace with
`fnmatch(3)` — faster, fewer allocations, and its glob semantics are
exactly what's wanted.

### 8. Config parser robustness

`Config::ReadConfigFile` splits on `=` and requires exactly two
elements. Values containing `=` (base64 strings, escaped data, URL
parameters) are silently dropped. Switch to
`std::string::find_first_of('=')` and split into key + remainder.

---

## Tier 3 — Hygiene (bundle into one cleanup PR)

Low-effort, high-polish items that land well together.

- Add `[[nodiscard]]` on class status getters: `IsValid`,
  `IsInitialized`, `GetLastActiveTime`, etc.
- Add `.clang-format` + `.clang-tidy`; run once across the tree.
- CI sanitizer jobs: one matrix entry each for
  `-fsanitize=address,undefined` and `-fsanitize=thread`.
- Document or enforce matched quoting in `StripQuotes`.
- Replace vendored `tinyformat.h` with FetchContent or a submodule
  for upstream tracking.

---

## Tier 4 — Post-1.0 (tracked but not blocking)

- Mock-based integration tests for D-Bus / Wayland idle sources.
- Structured logging (key=value or JSON) — helps user bug reports.
- Manpages for the daemons and config files (debhelper installs them
  automatically if present).
- Coverage reporting in CI (codecov or equivalent).
- `osc service remoterun` automation from GHA so OBS rebuilds fire on
  each release tag without manual `osc service runall`.
- Signed apt repo published somewhere (BOINC-style aptly publish)
  rather than only `.deb` attachments on GitHub releases.

---

## Suggested ordering

1. **Items 1 (seqlock) and 7 (fnmatch) first** — small, focused, low
   risk. Good warm-up that demonstrates 1.0 work is moving.
2. **Item 5 (file split)** — large but mechanical diff, independent
   of the correctness work. Precondition for Item 2's pure-logic
   extraction.
3. **Item 3 (encapsulation)** — often done alongside the split;
   private members and `Start`/`Stop` interfaces become natural once
   classes live in their own files. Also a precondition for Item 2.
4. **Item 2 (test coverage)** after the refactor groundwork is in
   place — extract pure-logic components, test them in the
   `util.cpp`-only test binary, add the real-OS-primitive round-trips
   and the smoke harness.
5. **Item 6 (enum unify)** — small; fold into any adjacent PR.
6. **Tier 3 hygiene bundle** near the end — cheap, satisfying, good
   closeout.
