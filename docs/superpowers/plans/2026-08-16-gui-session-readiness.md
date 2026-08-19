# GUI Session Readiness and Multi-Endpoint Idle Detection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `getenv()`-based session typing in `idle_detect` with runtime GUI session discovery that re-evaluates as sessions appear and disappear, and support multiple concurrent graphical endpoints.

**Architecture:** Two layers. A per-user desktop *shell* source triggered by D-Bus `NameOwnerChanged`, and 0..N graphical *endpoints* discovered from a union of individually-distrusted hints and validated by connecting. Each source resolves to exactly one idle value via its own priority chain; the aggregate is `min()` across sources with `-1`/`-2` sentinels excluded.

**Tech Stack:** C++17, GLib/GIO (D-Bus), libwayland-client, libXss, GoogleTest 1.15.2, CMake, systemd user units.

**Spec:** `docs/superpowers/specs/2026-08-16-gui-session-readiness-design.md`

**Branch:** `gui_session_readiness`

## Global Constraints

- C++17. No `goto` — restructure with RAII, early return, or a helper. This is a standing project preference and Task 3 exists specifically to remove the one existing instance.
- Naming conventions: `m_` members, `g_` globals, `mtx_` mutexes, `cv_` condition variables, `m_interrupt_*` atomic shutdown flags.
- Doxygen `//!` comment blocks on all public classes and methods, matching `idle_detect.h`.
- Copyright header on every new file, copied verbatim from `tests/config_tests.cpp` lines 1-5 with the year left as 2025.
- The test binary `idle_detect_tests` must remain linkable **without** D-Bus, X11, Wayland, or libevdev. Pure-logic code goes in files that do not include those headers; system access sits behind the `IdleSource` interface.
- Sentinel contract, exact values: `-1` = error, `-2` = no GUI session (defer to `event_detect`, overriding the `use_event_detect` config setting). These are distinct and `-2` must never be collapsed into `-1`.
- The BOINC shared-memory contract is frozen at `int64_t[2]`. Nothing in this plan touches it.
- Build directory: use `build/dev`, **not** `build/cmake` — the latter contains root-owned files from a prior `sudo` build.

**Standard commands** used throughout:

```bash
cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug
cmake --build build/dev -j$(nproc)
ctest --test-dir build/dev --output-on-failure
```

## File Structure

| File | Responsibility |
|---|---|
| `idle_source.h` / `.cpp` | **Create.** `Endpoint`, `ShellKind`, `IdleSource` interface, `AggregateIdleSeconds()`. Pure logic — no system headers. |
| `session_discovery.h` / `.cpp` | **Create.** `NormalizeX11Display()`, `DiscoveryHints`, `DiscoverEndpoints()`. Pure logic over injected hints — no system headers. |
| `idle_sources_system.h` / `.cpp` | **Create.** `WaylandIdleSource`, `X11IdleSource`, `ShellIdleSource`, `ShellMonitor`, `IdleSourcePool`. All D-Bus/Wayland/X11 contact lives here. |
| `idle_detect.h` / `.cpp` | **Modify.** De-globalize `WaylandIdleMonitor`; replace `GetIdleTimeSeconds()` internals; delete `IsTtySession`/`IsWaylandSession`; wire the pool into `main()`. |
| `tests/idle_aggregation_tests.cpp` | **Create.** Sentinel contract and `min()` behavior. |
| `tests/session_discovery_tests.cpp` | **Create.** Hint union, lock-file filtering, uid filtering, display normalization. |
| `dc_idle_detection.service.in` | **Modify.** `ExecStart` → binary directly. |
| `CMakeLists.txt` | **Modify.** New sources, new test files, drop wrapper generation/install. |
| `idle_detect_wrapper.sh.in` | **Delete.** |
| `idle_detect.conf.in` | **Modify.** Add the missing `event_count_files_path` line (issue #12 consistency gap). |

The split is deliberate: `idle_source.*` and `session_discovery.*` contain zero system dependencies so they can link into `idle_detect_tests`, which currently links only `util.cpp`. `idle_sources_system.*` holds everything that cannot.

---

## Phase 0 — Issue #11 Wayland lifetime fixes

Independently mergeable. Land this even if the rest slips. Based on Penumbra69's patch from issue #11, **excluding** its GNOME eager-startup skip, which is a separate behavioral decision and is superseded by Phase 3's discovery logic.

> **Status: COMPLETE (13 code commits, `a36c839`..`0a02914`).** Adversarial review during execution surfaced nine further defects, all pre-existing, fixed in the same phase because they sit in the code being restructured and several undermine the design's goals directly. Beyond Tasks 1-3 below, Phase 0 also delivered:
>
> - `45eb6cf` — `wl_display_cancel_read()` on the `EINTR` path. The poll loop restarted without releasing the read lock, driving libwayland's `reader_count` to 2 and deadlocking the next `wl_display_read_events()` in `pthread_cond_wait`.
> - `1ebd1a8` — `HandleGlobalRemove` destroys the proxies instead of only nulling them. `CleanupWayland()`'s destroys are gated on non-null, so an orphaned proxy was skipped forever, and `wl_display_disconnect()` does not free live proxies.
> - `38966ce` — null-check `wl_registry_bind()` results. `debug_log` is a function template, so its `wl_proxy_get_version()` argument was evaluated unconditionally regardless of the debug flag.
> - `f9402d3` — clear `m_initialized` when the monitor thread exits unexpectedly. It was cleared in exactly one place, so a compositor hangup left `IsAvailable()` true and `GetIdleSeconds()` serving a frozen value for the daemon's life, pinning DC permanently paused or permanently running.
> - `4615fef` — do not enter the evdev read loop without a device handle. `g_exit_code` is not monotonic, so a recorder thread could skip initialization and still enter the loop.
> - `e45e09b` — preserve the failure exit code. `Shutdown(const int& = 0)` reset `g_exit_code` to 0, so four error paths exited successfully and defeated `Restart=on-failure`.
> - `73eda70`, `999e7d9`, `0a02914` — three regressions the above introduced, found by a second review pass: an `m_globals_lost` latch across a successful init, a `std::terminate` from move-assigning onto a joinable `std::thread` on restart, and an unbounded reap join.
> - `a02f2e0`, `50a7886` — restart-loop containment. `Shutdown(1)` is semantically right, but `RestartSec=5s` plus `ExecStartPre=/bin/sleep 5` against systemd's default 10s window meant the burst limit never tripped. Bounded via `StartLimitIntervalSec`/`StartLimitBurst`, and "no pointing devices" was made **non-fatal** — it is re-evaluated every second from the monitor thread, tty monitoring does not depend on it, and the downstream consumers are empty-safe.
>
> Verified throughout: clean build with zero warnings, 101/101 tests, zero `goto` in the tracked codebase, and **valgrind reporting 0 errors on both the normal and the issue-#11 failure path**.

### Task 1: Centralize failed-init cleanup and destroy Wayland proxies

**Files:**
- Modify: `idle_detect.h:186-190` (add `ResetWaylandState()` declaration)
- Modify: `idle_detect.cpp:1228-1300` (`InitializeWayland`), `idle_detect.cpp:1315-1345` (`CleanupWayland`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `void WaylandIdleMonitor::ResetWaylandState();` — private, clears `m_seat`, `m_idle_notifier`, `m_seat_id`, `m_idle_notifier_id`, `m_idle_notification` to null/0 without destroying anything.

- [ ] **Step 1: Add the `ResetWaylandState()` declaration**

In `idle_detect.h`, immediately after the `CleanupWayland()` declaration:

```cpp
    //! \brief Private method to clean up Wayland resources.
    void CleanupWayland();

    //! \brief Private method to clear cached Wayland object pointers and IDs without destroying them.
    void ResetWaylandState();
```

- [ ] **Step 2: Implement `ResetWaylandState()`**

In `idle_detect.cpp`, immediately before `bool WaylandIdleMonitor::InitializeWayland()`:

```cpp
void WaylandIdleMonitor::ResetWaylandState() {
    m_seat = nullptr;
    m_idle_notifier = nullptr;
    m_seat_id = 0;
    m_idle_notifier_id = 0;
    m_idle_notification = nullptr;
}
```

- [ ] **Step 3: Route all three failed-init paths through `CleanupWayland()`**

In `InitializeWayland()`, replace the three ad-hoc cleanup sites.

Replace at the failed-registry branch:

```cpp
                error_log("%s: Failed to get Wayland registry (attempt %d).", __func__, attempt);
                wl_display_disconnect(m_display); m_display = nullptr;
```

with:

```cpp
                error_log("%s: Failed to get Wayland registry (attempt %d).", __func__, attempt);
                CleanupWayland();
```

Replace the stale-globals reset:

```cpp
                // Reset potential stale globals found from previous failed attempts
                m_seat = nullptr; m_idle_notifier = nullptr;
                m_seat_id = 0; m_idle_notifier_id = 0;
```

with:

```cpp
                // Reset potential stale globals found from previous failed attempts
                ResetWaylandState();
```

Replace the globals-not-found branch:

```cpp
                        wl_registry_destroy(m_registry); m_registry = nullptr;
                        wl_display_disconnect(m_display); m_display = nullptr;
```

with:

```cpp
                        CleanupWayland();
```

Replace the roundtrip-failed branch:

```cpp
                    if (m_registry) { wl_registry_destroy(m_registry); m_registry = nullptr; }
                    wl_display_disconnect(m_display); m_display = nullptr;
```

with:

```cpp
                    CleanupWayland();
```

- [ ] **Step 4: Actually destroy proxies in `CleanupWayland()`**

Replace:

```cpp
    if (m_idle_notifier) { m_idle_notifier = nullptr; } // Global, no destroy in spec
    if (m_seat) {
        // Check version before calling release (available since v5)
        if (wl_proxy_get_version((struct wl_proxy *)m_seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(m_seat);
        }
        // Even without release, we destroy the proxy reference below implicitly or explicitly?
        // Wayland client library usually handles proxy destruction when display is disconnected/destroyed.
        // Setting pointer to null is sufficient here.
        m_seat = nullptr;
    }
```

with:

```cpp
    if (m_idle_notifier) {
        ext_idle_notifier_v1_destroy(m_idle_notifier);
        m_idle_notifier = nullptr;
    }
    if (m_seat) {
        // Check version before calling release (available since v5). Below v5 there is no
        // release request, so destroy the proxy directly rather than leaking it until
        // wl_display_disconnect().
        if (wl_proxy_get_version((struct wl_proxy *)m_seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(m_seat);
        } else {
            wl_proxy_destroy((struct wl_proxy *)m_seat);
        }
        m_seat = nullptr;
    }
```

Then, immediately before the trailing `// Note: Interrupt pipe FDs are NOT closed here.` comment, add:

```cpp
    m_seat_id = 0;
    m_idle_notifier_id = 0;
```

- [ ] **Step 5: Build**

Run: `cmake --build build/dev -j$(nproc)`
Expected: builds clean, no new warnings.

- [ ] **Step 6: Verify the daemon still starts on this machine**

Run: `./build/dev/idle_detect 2>&1 | head -40`
Expected: log lines showing Wayland init succeeding and an idle time being reported. Ctrl-C to exit.

- [ ] **Step 7: Commit**

```bash
git add idle_detect.h idle_detect.cpp
git commit -m "Fix Wayland proxy lifetime on failed init retries

Route all failed-initialization paths in InitializeWayland() through
CleanupWayland() instead of ad-hoc disconnect pairs that left m_seat and
m_idle_notifier dangling. Destroy ext_idle_notifier_v1 and pre-v5 wl_seat
proxies explicitly rather than only nulling the pointers.

Addresses part of #11.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Guard against duplicate registry binds

**Files:**
- Modify: `idle_detect.cpp` — `WaylandIdleMonitor_HandleGlobal` (search for `WaylandIdleMonitor_HandleGlobal`, near line 1500)

**Interfaces:**
- Consumes: `ResetWaylandState()` from Task 1 (relied on to null the pointers between attempts so the new guard does not permanently block rebinding).
- Produces: nothing new.

- [ ] **Step 1: Add the bind guards**

Replace:

```cpp
    if (strcmp(interface, wl_seat_interface.name) == 0) {
```

with:

```cpp
    if (strcmp(interface, wl_seat_interface.name) == 0 && monitor->m_seat == nullptr) {
```

and replace:

```cpp
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
```

with:

```cpp
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0 &&
               monitor->m_idle_notifier == nullptr) {
```

- [ ] **Step 2: Build**

Run: `cmake --build build/dev -j$(nproc)`
Expected: builds clean.

- [ ] **Step 3: Verify rebinding still works across a retry**

Run: `WAYLAND_DISPLAY=nonexistent ./build/dev/idle_detect 2>&1 | head -30`
Expected: repeated "Failed to connect to Wayland display" retry messages, no crash, and the process exits or falls back rather than hanging. This exercises the retry loop that Task 1 changed.

- [ ] **Step 4: Commit**

```bash
git add idle_detect.cpp
git commit -m "Bind wl_seat and ext_idle_notifier_v1 at most once per attempt

A compositor advertising multiple seats would overwrite m_seat and leak the
first binding. Guard both binds on the cached pointer being null;
ResetWaylandState() clears them between initialization attempts so retries
still rebind.

Addresses part of #11.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Remove the `goto` from `WaylandIdleMonitor::Start()`

**Files:**
- Modify: `idle_detect.cpp:1110-1171` (`WaylandIdleMonitor::Start`)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool WaylandIdleMonitor::StartInternal(int notification_timeout_ms);` — private helper returning success, with **no** cleanup responsibility. `Start()` owns cleanup on failure.

Rationale: this file's only `goto` (`start_failed:`) is against project style. Task 6 rewrites this function's signature anyway, so removing it now keeps that later change small.

- [ ] **Step 1: Add the `StartInternal()` declaration**

In `idle_detect.h`, immediately after the `InitializeWayland()` declaration:

```cpp
    //! \brief Private method to initialize Wayland and set up the idle notification.
    bool InitializeWayland();

    //! \brief Private helper performing the fallible portion of Start(). Performs no cleanup;
    //! Start() owns teardown on failure.
    bool StartInternal();
```

- [ ] **Step 2: Extract the fallible body into `StartInternal()`**

Add to `idle_detect.cpp`, immediately before `void WaylandIdleMonitor::Stop() {`:

```cpp
bool WaylandIdleMonitor::StartInternal() {
    // Initialize Wayland connection, get initial state, and subscribe. Includes retries internally.
    if (!InitializeWayland()) {
        error_log("%s: Failed to initialize Wayland or find required protocols after retries.", __func__);
        return false;
    }

    if (!m_seat || !m_idle_notifier) {
        error_log("%s: Required Wayland interfaces not bound even after InitializeWayland success (logic error?).",
                  __func__);
        return false;
    }

    // Create the specific idle notification request object
    CreateIdleNotification();
    if (!m_idle_notification) {
        error_log("%s: Failed to create Wayland idle notification object.", __func__);
        return false;
    }

    try {
        m_monitor_thread = std::thread(&WaylandIdleMonitor::WaylandMonitorThread, this);
    } catch (const std::system_error& e) {
        error_log("%s: Failed to start Wayland monitor thread: %s", __func__, e.what());
        return false;
    } catch (...) {
        error_log("%s: Unknown error starting Wayland monitor thread.", __func__);
        return false;
    }

    return true;
}
```

- [ ] **Step 3: Replace the `goto` body in `Start()`**

Replace everything in `Start()` from `// Initialize Wayland connection` through the closing `return false;` of the `start_failed:` label with:

```cpp
    if (!StartInternal()) {
        CleanupWayland();
        if (m_interrupt_pipe_fd[0] != -1) { close(m_interrupt_pipe_fd[0]); m_interrupt_pipe_fd[0] = -1; }
        if (m_interrupt_pipe_fd[1] != -1) { close(m_interrupt_pipe_fd[1]); m_interrupt_pipe_fd[1] = -1; }
        m_initialized.store(false);
        return false;
    }

    m_initialized.store(true); // Set initialized only after thread starts successfully
    normal_log("INFO: %s: Wayland idle monitor started successfully.", __func__);
    return true;
}
```

- [ ] **Step 4: Verify no `goto` remains**

Run: `git ls-files -z '*.cpp' '*.h' | xargs -0 grep -nw "goto"`
Expected: no output.

**Scope correction.** An earlier revision of this task covered only the five `goto start_failed`
jumps in `Start()` and asserted the grep above would then come back clean. It would not have.
`idle_detect.cpp` had **eleven** gotos in two clusters, and `event_detect.cpp` had a twelfth:

- 5 × `goto start_failed` in `Start()` — the extraction above.
- 6 × `goto thread_exit` in `WaylandMonitorThread()`. Five sit directly in the outer `while`
  body, where `break` is exactly equivalent because `thread_exit:` only logs before the function
  ends. **The sixth is inside the nested `while (wl_display_prepare_read(...) != 0)` loop, where
  `break` is a bug** — it would exit only the inner loop and fall through to `wl_display_flush()`
  and `poll()` on a display that just failed to dispatch. That one became a `PrepareRead()`
  helper returning `bool`, called as `if (!PrepareRead()) { break; }`.
- 1 × `goto cleanup` in `event_detect.cpp`'s libevdev read loop, replaced by RAII
  (`ScopedFileDescriptor` + a `unique_ptr<libevdev, decltype(&libevdev_free)>`). This also
  removed a latent **double `close(fd)`**: the old libevdev-init failure path closed the
  descriptor and then fell through to the `cleanup:` label, which closed it again.

- [ ] **Step 5: Build and smoke test**

Run: `cmake --build build/dev -j$(nproc) && ./build/dev/idle_detect 2>&1 | head -20`
Expected: builds clean; monitor starts as before. Ctrl-C to exit.

- [ ] **Step 6: Commit**

```bash
git add idle_detect.h idle_detect.cpp
git commit -m "Remove goto from WaylandIdleMonitor::Start()

Extract the fallible portion into StartInternal() and let Start() own the
failure teardown. Matches the RAII/early-return style used elsewhere.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Phase 1 — Pure logic foundations

No system dependencies. Fully TDD.

### Task 4: Endpoint types and idle aggregation

**Files:**
- Create: `idle_source.h`, `idle_source.cpp`
- Create: `tests/idle_aggregation_tests.cpp`
- Modify: `CMakeLists.txt:110-118` (idle_detect sources), `CMakeLists.txt:346-351` (test sources)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `constexpr int64_t IdleDetect::IDLE_ERROR = -1;`
  - `constexpr int64_t IdleDetect::IDLE_NO_GUI_SESSION = -2;`
  - `enum class IdleDetect::EndpointKind { WAYLAND, X11 };`
  - `struct IdleDetect::Endpoint { EndpointKind m_kind; std::string m_identifier; };` with `operator<`, `operator==`, `std::string ToString() const;`
  - `enum class IdleDetect::ShellKind { NONE, KDE, GNOME };`
  - `class IdleDetect::IdleSource` with `virtual int64_t ResolveIdleSeconds() = 0;` and `virtual std::string Describe() const = 0;`
  - `int64_t IdleDetect::AggregateIdleSeconds(const std::vector<int64_t>& resolved, bool inhibited, bool any_source_present);`

- [ ] **Step 1: Write the failing test**

Create `tests/idle_aggregation_tests.cpp`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <idle_source.h>

using namespace IdleDetect;

//
// Sentinel contract
//

TEST(IdleAggregation, NoSourcesAtAllReportsNoGuiSession)
{
    EXPECT_EQ(AggregateIdleSeconds({}, false, false), IDLE_NO_GUI_SESSION);
}

TEST(IdleAggregation, InhibitedShortCircuitsToZero)
{
    EXPECT_EQ(AggregateIdleSeconds({500, 900}, true, true), 0);
}

TEST(IdleAggregation, InhibitedWinsOverErrorSources)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR}, true, true), 0);
}

TEST(IdleAggregation, SourcesPresentButAllErrorReportsError)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR, IDLE_ERROR}, false, true), IDLE_ERROR);
}

TEST(IdleAggregation, SourcesPresentButNoneResolvedReportsError)
{
    EXPECT_EQ(AggregateIdleSeconds({}, false, true), IDLE_ERROR);
}

//
// min() behavior
//

TEST(IdleAggregation, TakesMinimumOfResolvedValues)
{
    EXPECT_EQ(AggregateIdleSeconds({900, 12, 500}, false, true), 12);
}

TEST(IdleAggregation, ZeroIsAValidMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({900, 0}, false, true), 0);
}

//
// Sentinels must never enter the minimum. min(-1, 300) would be -1, silently
// converting one source's error into a global error.
//

TEST(IdleAggregation, ErrorSentinelExcludedFromMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_ERROR, 300}, false, true), 300);
}

TEST(IdleAggregation, NoGuiSessionSentinelExcludedFromMinimum)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_NO_GUI_SESSION, 300}, false, true), 300);
}

TEST(IdleAggregation, BothSentinelsExcludedTogether)
{
    EXPECT_EQ(AggregateIdleSeconds({IDLE_NO_GUI_SESSION, IDLE_ERROR, 42}, false, true), 42);
}

//
// The multi-endpoint regression this design exists to prevent: an active
// endpoint alongside an idle one must report the active one.
//

TEST(IdleAggregation, ActiveEndpointBeatsIdleEndpoint)
{
    // Console idle for 20 minutes, VNC session active 3 seconds ago.
    EXPECT_EQ(AggregateIdleSeconds({1200, 3}, false, true), 3);
}

//
// Endpoint value semantics
//

TEST(Endpoint, OrdersByKindThenIdentifier)
{
    Endpoint wayland{EndpointKind::WAYLAND, "wayland-0"};
    Endpoint x11_one{EndpointKind::X11, ":1"};
    Endpoint x11_two{EndpointKind::X11, ":2"};

    EXPECT_LT(wayland, x11_one);
    EXPECT_LT(x11_one, x11_two);
}

TEST(Endpoint, EqualityComparesKindAndIdentifier)
{
    EXPECT_EQ((Endpoint{EndpointKind::X11, ":1"}), (Endpoint{EndpointKind::X11, ":1"}));
    EXPECT_FALSE((Endpoint{EndpointKind::X11, ":1"}) == (Endpoint{EndpointKind::WAYLAND, ":1"}));
}

TEST(Endpoint, ToStringIsHumanReadable)
{
    EXPECT_EQ((Endpoint{EndpointKind::WAYLAND, "wayland-0"}).ToString(), "wayland:wayland-0");
    EXPECT_EQ((Endpoint{EndpointKind::X11, ":1"}).ToString(), "x11::1");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/dev -j$(nproc)`
Expected: FAIL — `idle_source.h: No such file or directory`.

- [ ] **Step 3: Create the header**

Create `idle_source.h`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_SOURCE_H
#define IDLE_SOURCE_H

#include <cstdint>
#include <string>
#include <vector>

namespace IdleDetect {

//!
//! \brief Returned when a source exists but could not produce a reading.
//!
constexpr int64_t IDLE_ERROR = -1;

//!
//! \brief Returned when no GUI session exists at all. Instructs the caller to defer to
//! event_detect regardless of the use_event_detect config setting. Must remain distinct
//! from IDLE_ERROR.
//!
constexpr int64_t IDLE_NO_GUI_SESSION = -2;

//!
//! \brief The display server protocol a graphical endpoint speaks.
//!
enum class EndpointKind {
    WAYLAND,
    X11
};

//!
//! \brief A single reachable graphical endpoint.
//!
struct Endpoint {
    //! \brief Protocol spoken by this endpoint.
    EndpointKind m_kind;

    //! \brief Wayland socket name (e.g. "wayland-0") or canonical X display (e.g. ":1").
    std::string m_identifier;

    bool operator==(const Endpoint& other) const;
    bool operator<(const Endpoint& other) const;

    //!
    //! \brief Human-readable form for logging, e.g. "wayland:wayland-0" or "x11::1".
    //! \return string representation
    //!
    std::string ToString() const;
};

//!
//! \brief Which desktop shell owns the per-user session bus, if any.
//!
enum class ShellKind {
    NONE,
    KDE,
    GNOME
};

//!
//! \brief Abstract idle source. Each instance owns its own priority chain and resolves to
//! exactly one value. Aggregation never sees the inside of a chain.
//!
class IdleSource
{
public:
    virtual ~IdleSource() = default;

    //!
    //! \brief Resolves this source's idle time using its own internal fallback chain.
    //! \return idle seconds >= 0, or IDLE_ERROR
    //!
    virtual int64_t ResolveIdleSeconds() = 0;

    //!
    //! \brief Human-readable description for logging.
    //! \return string representation
    //!
    virtual std::string Describe() const = 0;
};

//!
//! \brief Combines resolved source values into the single value returned to the main loop.
//!
//! Inhibition short-circuits to zero. Sentinels are excluded from the minimum, because
//! min(IDLE_ERROR, 300) would silently convert one source's error into a global error.
//!
//! \param resolved values returned by every live source, each >= 0 or a sentinel
//! \param inhibited whether the shell reports idle inhibition
//! \param any_source_present whether any endpoint or shell exists at all
//! \return 0 if inhibited, IDLE_NO_GUI_SESSION if nothing exists, IDLE_ERROR if sources
//!         exist but none resolved, otherwise the minimum resolved value
//!
int64_t AggregateIdleSeconds(const std::vector<int64_t>& resolved,
                             bool inhibited,
                             bool any_source_present);

} // namespace IdleDetect

#endif // IDLE_SOURCE_H
```

- [ ] **Step 4: Create the implementation**

Create `idle_source.cpp`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_source.h>

#include <algorithm>
#include <tuple>

namespace IdleDetect {

bool Endpoint::operator==(const Endpoint& other) const
{
    return m_kind == other.m_kind && m_identifier == other.m_identifier;
}

bool Endpoint::operator<(const Endpoint& other) const
{
    return std::tie(m_kind, m_identifier) < std::tie(other.m_kind, other.m_identifier);
}

std::string Endpoint::ToString() const
{
    const std::string prefix = (m_kind == EndpointKind::WAYLAND) ? "wayland:" : "x11:";

    return prefix + m_identifier;
}

int64_t AggregateIdleSeconds(const std::vector<int64_t>& resolved,
                             bool inhibited,
                             bool any_source_present)
{
    // Inhibition means "do not let this machine go idle" and wins over everything, including
    // sources that failed to resolve.
    if (inhibited) {
        return 0;
    }

    if (!any_source_present) {
        return IDLE_NO_GUI_SESSION;
    }

    bool found = false;
    int64_t minimum = 0;

    for (const int64_t value : resolved) {
        // Sentinels are negative and must never participate in the minimum.
        if (value < 0) {
            continue;
        }

        if (!found || value < minimum) {
            minimum = value;
            found = true;
        }
    }

    return found ? minimum : IDLE_ERROR;
}

} // namespace IdleDetect
```

- [ ] **Step 5: Wire into CMake**

In `CMakeLists.txt`, in `SOURCES_IDLE_DETECT`, after `"idle_detect.h"`:

```cmake
    "idle_detect.h"
    "idle_source.h"
    "idle_source.cpp"
```

In the `add_executable(idle_detect_tests ...)` block, after `tests/config_tests.cpp`:

```cmake
        tests/config_tests.cpp
        tests/idle_aggregation_tests.cpp
        util.cpp
        idle_source.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug && cmake --build build/dev -j$(nproc) && ctest --test-dir build/dev --output-on-failure -R "IdleAggregation|Endpoint"`
Expected: PASS, 14 tests.

- [ ] **Step 7: Confirm the full suite still passes**

Run: `ctest --test-dir build/dev --output-on-failure`
Expected: PASS, 115 tests (101 existing + 14 new).

- [ ] **Step 8: Commit**

```bash
git add idle_source.h idle_source.cpp tests/idle_aggregation_tests.cpp CMakeLists.txt
git commit -m "Add endpoint types and idle aggregation with sentinel contract

AggregateIdleSeconds() takes min() across resolved source values, excluding
the -1 and -2 sentinels so one source's error cannot become a global error,
and preserving -2 as distinct from -1. No system dependencies, so it links
into the test binary alongside util.cpp.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Endpoint discovery from a union of hints

**Files:**
- Create: `session_discovery.h`, `session_discovery.cpp`
- Create: `tests/session_discovery_tests.cpp`
- Modify: `CMakeLists.txt` (same two blocks as Task 4)

**Interfaces:**
- Consumes: `Endpoint`, `EndpointKind` from Task 4 (`idle_source.h`).
- Produces:
  - `std::optional<std::string> IdleDetect::NormalizeX11Display(const std::string& raw);`
  - `struct IdleDetect::DiscoveryHints { std::filesystem::path m_xdg_runtime_dir; std::filesystem::path m_x11_socket_dir; std::vector<std::string> m_env_displays; std::vector<std::string> m_logind_displays; uid_t m_uid; };`
  - `std::set<IdleDetect::Endpoint> IdleDetect::DiscoverEndpoints(const DiscoveryHints& hints);`

- [ ] **Step 1: Write the failing test**

Create `tests/session_discovery_tests.cpp`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <session_discovery.h>

#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace IdleDetect;

namespace {

//!
//! \brief Creates a real unix domain socket at the given path so discovery's socket check
//! sees the same file type it will see in production.
//!
void MakeSocket(const fs::path& path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    close(fd);
}

//!
//! \brief RAII temporary directory.
//!
class TempDir
{
public:
    TempDir()
    {
        m_path = fs::temp_directory_path() / fs::path("idle_detect_test_" + std::to_string(getpid())
                                                      + "_" + std::to_string(++s_counter));
        fs::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
    static int s_counter;
};

int TempDir::s_counter = 0;

} // namespace

//
// X11 display normalization
//

TEST(NormalizeX11Display, AcceptsCanonicalForm)
{
    EXPECT_EQ(NormalizeX11Display(":1").value(), ":1");
}

TEST(NormalizeX11Display, StripsScreenSuffix)
{
    EXPECT_EQ(NormalizeX11Display(":1.0").value(), ":1");
}

TEST(NormalizeX11Display, AcceptsSocketBasename)
{
    EXPECT_EQ(NormalizeX11Display("X2").value(), ":2");
}

TEST(NormalizeX11Display, RejectsGarbage)
{
    EXPECT_FALSE(NormalizeX11Display("").has_value());
    EXPECT_FALSE(NormalizeX11Display("nonsense").has_value());
    EXPECT_FALSE(NormalizeX11Display(":").has_value());
    EXPECT_FALSE(NormalizeX11Display(":abc").has_value());
}

TEST(NormalizeX11Display, RejectsRemoteDisplays)
{
    // host:0 is a TCP display; XScreenSaver against it is not this daemon's business.
    EXPECT_FALSE(NormalizeX11Display("somehost:0").has_value());
}

//
// Wayland socket discovery
//

TEST(DiscoverEndpoints, FindsWaylandSocket)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, IgnoresWaylandLockFiles)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");
    std::ofstream(runtime.Path() / "wayland-0.lock").put('\n');

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, FindsMultipleWaylandSockets)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");
    MakeSocket(runtime.Path() / "wayland-1");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    EXPECT_EQ(DiscoverEndpoints(hints).size(), 2u);
}

TEST(DiscoverEndpoints, IgnoresNonSocketFilesNamedLikeWayland)
{
    TempDir runtime;
    std::ofstream(runtime.Path() / "wayland-9").put('\n');

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, MissingRuntimeDirIsNotAnError)
{
    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = "/nonexistent/path/for/test";
    hints.m_uid = getuid();

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

//
// X11 discovery: the hints are unreliable in complementary ways, so they are unioned.
//

TEST(DiscoverEndpoints, FindsX11SocketOwnedByUs)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::X11);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, RejectsX11SocketOwnedByAnotherUser)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X0");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    // Pretend we are a different user than the socket's owner. /tmp/.X11-unix is world
    // visible and holds the root-owned greeter socket alongside ours.
    hints.m_uid = getuid() + 1;

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, UsesEnvDisplayHintWhenSocketIsNotOurs)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid() + 1;      // socket filter rejects it
    hints.m_env_displays = {":1"};   // but the systemd manager environment knows about it

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, UnionsHintsAndDeduplicates)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":1"};       // duplicate of the socket
    hints.m_logind_displays = {":2"};    // additional, from optional enrichment

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":1"})), 1u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":2"})), 1u);
}

TEST(DiscoverEndpoints, IgnoresUnparseableHints)
{
    DiscoveryHints hints;
    hints.m_uid = getuid();
    hints.m_env_displays = {"", "garbage", ":3"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":3");
}

TEST(DiscoverEndpoints, CombinesWaylandAndX11)
{
    TempDir runtime;
    TempDir x11;
    MakeSocket(runtime.Path() / "wayland-0");
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    EXPECT_EQ(DiscoverEndpoints(hints).size(), 2u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/dev -j$(nproc)`
Expected: FAIL — `session_discovery.h: No such file or directory`.

- [ ] **Step 3: Create the header**

Create `session_discovery.h`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef SESSION_DISCOVERY_H
#define SESSION_DISCOVERY_H

#include <idle_source.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace IdleDetect {

//!
//! \brief Normalizes an X display specifier to the canonical ":N" form.
//!
//! Accepts ":N", ":N.S", and the socket basename form "XN". Rejects empty input, remote
//! (host-qualified) displays, and anything without a numeric display number.
//!
//! \param raw display specifier from a discovery hint
//! \return canonical ":N" form, or std::nullopt if unparseable
//!
std::optional<std::string> NormalizeX11Display(const std::string& raw);

//!
//! \brief Inputs to endpoint discovery. Every field is an independently unreliable hint;
//! none is required to be populated.
//!
struct DiscoveryHints {
    //! \brief Directory searched for wayland-* sockets. Typically $XDG_RUNTIME_DIR.
    std::filesystem::path m_xdg_runtime_dir;

    //! \brief Directory searched for X sockets. Typically /tmp/.X11-unix.
    std::filesystem::path m_x11_socket_dir;

    //! \brief DISPLAY values read from the systemd user manager Environment property.
    std::vector<std::string> m_env_displays;

    //! \brief Display values from logind graphical sessions. Optional enrichment only.
    std::vector<std::string> m_logind_displays;

    //! \brief Our uid. X socket candidates not owned by this uid are rejected, because
    //! /tmp/.X11-unix is world visible and contains other users' sockets.
    uid_t m_uid = 0;
};

//!
//! \brief Builds the candidate endpoint set from the union of all hints.
//!
//! Discovery is deliberately over-inclusive; callers validate each candidate by connecting
//! to it. No single hint is load-bearing.
//!
//! \param hints discovery inputs
//! \return deduplicated candidate endpoints
//!
std::set<Endpoint> DiscoverEndpoints(const DiscoveryHints& hints);

} // namespace IdleDetect

#endif // SESSION_DISCOVERY_H
```

- [ ] **Step 4: Create the implementation**

Create `session_discovery.cpp`:

```cpp
/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <session_discovery.h>

#include <cctype>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace IdleDetect {

namespace {

//!
//! \brief Returns true if every character is an ASCII digit and the string is non-empty.
//!
bool IsAllDigits(const std::string& value)
{
    if (value.empty()) {
        return false;
    }

    for (const unsigned char c : value) {
        if (!std::isdigit(c)) {
            return false;
        }
    }

    return true;
}

//!
//! \brief Returns true if the path exists and is a unix domain socket.
//!
bool IsSocket(const fs::path& path)
{
    struct stat st{};

    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }

    return S_ISSOCK(st.st_mode);
}

//!
//! \brief Returns true if the path exists and is owned by the given uid.
//!
bool IsOwnedBy(const fs::path& path, uid_t uid)
{
    struct stat st{};

    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }

    return st.st_uid == uid;
}

//!
//! \brief Adds an X display hint to the set if it normalizes successfully.
//!
void InsertX11Candidate(std::set<Endpoint>& endpoints, const std::string& raw)
{
    std::optional<std::string> normalized = NormalizeX11Display(raw);

    if (normalized.has_value()) {
        endpoints.insert(Endpoint{EndpointKind::X11, *normalized});
    }
}

} // anonymous namespace

std::optional<std::string> NormalizeX11Display(const std::string& raw)
{
    if (raw.empty()) {
        return std::nullopt;
    }

    std::string number;

    if (raw[0] == ':') {
        // ":N" or ":N.S" -- take everything up to an optional screen suffix.
        const size_t dot = raw.find('.', 1);
        number = raw.substr(1, (dot == std::string::npos) ? std::string::npos : dot - 1);
    } else if (raw[0] == 'X') {
        // Socket basename form, "XN".
        number = raw.substr(1);
    } else {
        // Host-qualified or otherwise not a local display.
        return std::nullopt;
    }

    if (!IsAllDigits(number)) {
        return std::nullopt;
    }

    return ":" + number;
}

std::set<Endpoint> DiscoverEndpoints(const DiscoveryHints& hints)
{
    std::set<Endpoint> endpoints;

    // --- Wayland: sockets named wayland-* in the runtime directory. ---
    if (!hints.m_xdg_runtime_dir.empty()) {
        std::error_code ec;
        fs::directory_iterator iter(hints.m_xdg_runtime_dir, ec);

        if (!ec) {
            for (const fs::directory_entry& entry : iter) {
                const std::string name = entry.path().filename().string();

                if (name.rfind("wayland-", 0) != 0) {
                    continue;
                }

                // wayland-0.lock sits beside wayland-0 and is a regular file, not a socket.
                if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".lock") == 0) {
                    continue;
                }

                if (!IsSocket(entry.path())) {
                    continue;
                }

                endpoints.insert(Endpoint{EndpointKind::WAYLAND, name});
            }
        }
    }

    // --- X11: union of socket scan, systemd manager environment, and logind. ---
    if (!hints.m_x11_socket_dir.empty()) {
        std::error_code ec;
        fs::directory_iterator iter(hints.m_x11_socket_dir, ec);

        if (!ec) {
            for (const fs::directory_entry& entry : iter) {
                const std::string name = entry.path().filename().string();

                if (name.empty() || name[0] != 'X') {
                    continue;
                }

                if (!IsSocket(entry.path())) {
                    continue;
                }

                // This directory is world visible and holds other users' sockets, including
                // the display manager's root-owned greeter socket.
                if (!IsOwnedBy(entry.path(), hints.m_uid)) {
                    continue;
                }

                InsertX11Candidate(endpoints, name);
            }
        }
    }

    for (const std::string& display : hints.m_env_displays) {
        InsertX11Candidate(endpoints, display);
    }

    for (const std::string& display : hints.m_logind_displays) {
        InsertX11Candidate(endpoints, display);
    }

    return endpoints;
}

} // namespace IdleDetect
```

- [ ] **Step 5: Wire into CMake**

In `SOURCES_IDLE_DETECT`, after `"idle_source.cpp"`:

```cmake
    "idle_source.cpp"
    "session_discovery.h"
    "session_discovery.cpp"
```

In the test executable block, after `tests/idle_aggregation_tests.cpp`:

```cmake
        tests/idle_aggregation_tests.cpp
        tests/session_discovery_tests.cpp
        util.cpp
        idle_source.cpp
        session_discovery.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug && cmake --build build/dev -j$(nproc) && ctest --test-dir build/dev --output-on-failure -R "NormalizeX11Display|DiscoverEndpoints"`
Expected: PASS, 17 tests.

- [ ] **Step 7: Confirm the full suite still passes**

Run: `ctest --test-dir build/dev --output-on-failure`
Expected: PASS, 132 tests.

- [ ] **Step 8: Commit**

```bash
git add session_discovery.h session_discovery.cpp tests/session_discovery_tests.cpp CMakeLists.txt
git commit -m "Add endpoint discovery from a union of distrusted hints

DiscoverEndpoints() unions wayland-* sockets, systemd manager environment
DISPLAY values, uid-filtered /tmp/.X11-unix sockets, and optional logind
enrichment, then deduplicates. Filters wayland lock files and other users'
X sockets. No hint is required to be present; callers validate by connecting.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Phase 2 — System-backed sources

### Task 6: Make `WaylandIdleMonitor` instance-clean and socket-addressable

**Files:**
- Modify: `idle_detect.h:122-212` (`WaylandIdleMonitor`)
- Modify: `idle_detect.cpp` — `Start()`, `InitializeWayland()`, and the `g_wayland_idle_monitor` global definition (search `g_wayland_idle_monitor`)

**Interfaces:**
- Consumes: `StartInternal()` from Task 3.
- Produces:
  - `bool WaylandIdleMonitor::Start(const std::string& socket_name, int notification_timeout_ms);` — `socket_name` empty means "use `WAYLAND_DISPLAY`", i.e. `wl_display_connect(nullptr)`.
  - `const std::string& WaylandIdleMonitor::GetSocketName() const;`

- [ ] **Step 1: Add the socket member and change the `Start()` signature**

In `idle_detect.h`, replace:

```cpp
    //! \brief Initializes the Wayland display and registry, and starts the idle notifier.
    bool Start(int notification_timeout_ms);
```

with:

```cpp
    //!
    //! \brief Initializes the Wayland display and registry, and starts the idle notifier.
    //! \param socket_name Wayland socket to connect to (e.g. "wayland-0"). Empty means use
    //!        the WAYLAND_DISPLAY environment variable.
    //! \param notification_timeout_ms idle notification threshold in milliseconds
    //! \return true on success
    //!
    bool Start(const std::string& socket_name, int notification_timeout_ms);

    //!
    //! \brief Returns the Wayland socket this monitor is bound to.
    //! \return socket name, empty if environment-derived
    //!
    const std::string& GetSocketName() const;
```

Add to the private members, after `m_notification_timeout_ms`:

```cpp
    //! \brief Timeout for idle notification in milliseconds
    int m_notification_timeout_ms;

    //! \brief Wayland socket name this instance connects to. Empty means use WAYLAND_DISPLAY.
    std::string m_socket_name;
```

Add `#include <string>` to the header includes if not already present.

- [ ] **Step 2: Store the socket name and use it when connecting**

In `idle_detect.cpp`, in `Start()`, at the point where `m_notification_timeout_ms` is assigned, add the socket assignment alongside it:

```cpp
    m_socket_name = socket_name;
```

In `InitializeWayland()`, replace:

```cpp
        m_display = wl_display_connect(nullptr);
```

with:

```cpp
        m_display = wl_display_connect(m_socket_name.empty() ? nullptr : m_socket_name.c_str());
```

Add the accessor, immediately after `bool WaylandIdleMonitor::IsAvailable() const {...}`:

```cpp
const std::string& WaylandIdleMonitor::GetSocketName() const
{
    return m_socket_name;
}
```

- [ ] **Step 3: Update the call site in `main()`**

In `idle_detect.cpp` `main()`, replace:

```cpp
        if (g_wayland_idle_monitor.Start(notification_timeout_ms)) {
```

with:

```cpp
        if (g_wayland_idle_monitor.Start(std::string {}, notification_timeout_ms)) {
```

This is temporary; Task 9 removes the global entirely.

- [ ] **Step 4: Build and smoke test**

Run: `cmake --build build/dev -j$(nproc) && ./build/dev/idle_detect 2>&1 | head -20`
Expected: builds clean; monitor starts as before against the environment socket. Ctrl-C to exit.

- [ ] **Step 5: Verify explicit socket addressing works**

Run: `./build/dev/idle_detect 2>&1 | grep -i "wayland" | head -5`
Expected: Wayland init succeeds. Then confirm the negative case still fails cleanly rather than crashing:
Run: `WAYLAND_DISPLAY=wayland-99 ./build/dev/idle_detect 2>&1 | head -10`
Expected: connect failures and retries, no segfault.

- [ ] **Step 6: Commit**

```bash
git add idle_detect.h idle_detect.cpp
git commit -m "Allow WaylandIdleMonitor to target an explicit socket

Start() takes a socket name and passes it to wl_display_connect() instead of
always deriving the socket from WAYLAND_DISPLAY. An empty name preserves the
old environment-derived behavior. Prerequisite for running one monitor per
discovered Wayland endpoint.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: `WaylandIdleSource`, `X11IdleSource`, `ShellMonitor`, `ShellIdleSource`

**Files:**
- Create: `idle_sources_system.h`, `idle_sources_system.cpp`
- Modify: `CMakeLists.txt` (`SOURCES_IDLE_DETECT` only — **not** the test binary; this file touches D-Bus/X11/Wayland)
- Modify: `idle_detect.h` — expose the existing static helpers so the new file can call them

**Interfaces:**
- Consumes: `IdleSource`, `Endpoint`, `ShellKind`, `IDLE_ERROR` from Task 4; `WaylandIdleMonitor::Start(socket, timeout)` and `GetSocketName()` from Task 6.
- Produces:
  - `class IdleDetect::WaylandIdleSource : public IdleSource` — ctor `explicit WaylandIdleSource(std::string socket_name);` plus `bool Start(int notification_timeout_ms);` and `void Stop();`
  - `class IdleDetect::X11IdleSource : public IdleSource` — ctor `explicit X11IdleSource(std::string display);`
  - `class IdleDetect::ShellMonitor` — `ShellKind DetectShellKind() const;` and `bool IsInhibited(ShellKind kind) const;`
  - `class IdleDetect::ShellIdleSource : public IdleSource` — ctor `explicit ShellIdleSource(ShellKind kind);` plus `void SetKind(ShellKind kind);`

The five existing static helpers in `idle_detect.cpp` — `GetIdleTimeKdeDBus()`, `GetIdleTimeWaylandGnomeViaDBus()`, `CheckKdeInhibition()`, `CheckGnomeInhibition()`, `GetIdleTimeXss()` — must lose `static` and gain declarations so this file can use them. `GetIdleTimeXss()` additionally needs a display parameter.

- [ ] **Step 1: Un-static the D-Bus helpers and give `GetIdleTimeXss()` a display parameter**

In `idle_detect.h`, inside `namespace IdleDetect`, immediately after `int64_t GetIdleTimeSeconds();`:

```cpp
//!
//! \brief Queries KDE ksmserver for session idle time. Handles inhibition internally by
//! resetting the reported idle time, so callers must not add a separate inhibition check.
//! \return idle seconds >= 0, or -1 on error
//!
int64_t GetIdleTimeKdeDBus();

//!
//! \brief Queries GNOME Mutter's IdleMonitor for input idle time.
//! \return idle seconds >= 0, or -1 on error
//!
int64_t GetIdleTimeWaylandGnomeViaDBus();

//!
//! \brief Checks whether KDE reports screen idle inhibition via the PowerManagement PolicyAgent.
//! \return true if inhibited
//!
bool CheckKdeInhibition();

//!
//! \brief Checks whether GNOME reports session idle inhibition.
//! \return true if inhibited
//!
bool CheckGnomeInhibition();

//!
//! \brief Queries XScreenSaver for idle time on a specific display.
//! \param display X display string, e.g. ":1". Empty uses the DISPLAY environment variable.
//! \return idle seconds >= 0, or -1 on error
//!
int64_t GetIdleTimeXss(const std::string& display);
```

In `idle_detect.cpp`, remove the `static` keyword from each of those five definitions and change `GetIdleTimeXss`:

```cpp
int64_t GetIdleTimeXss(const std::string& display) {
    debug_log("INFO: %s: Using XScreenSaver on display '%s'.",
              __func__,
              display.empty() ? "<default>" : display.c_str());
    Display* x_display = nullptr;
    const char* display_name = display.empty() ? nullptr : display.c_str();

    for (int attempt = 1; attempt <= MAX_X_CONNECT_RETRIES; ++attempt) {
        x_display = XOpenDisplay(display_name);
        if (x_display) break;
        if (attempt < MAX_X_CONNECT_RETRIES) {
            error_log("WARNING: %s: Could not open X display (attempt %d/%d). Retrying...",
                      __func__, attempt, MAX_X_CONNECT_RETRIES);
            std::this_thread::sleep_for(std::chrono::milliseconds(X_RETRY_DELAY_MS));
        } else {
            error_log("%s: Could not open X display after %d attempts.", __func__, MAX_X_CONNECT_RETRIES);
            return -1;
        }
    }
```

The remainder of the function body is unchanged except that every subsequent use of the local `display` variable must be renamed to `x_display`. There are five: `XScreenSaverQueryExtension(x_display, ...)`, two `XCloseDisplay(x_display)` calls in the error paths, `DefaultRootWindow(x_display)`, `XScreenSaverQueryInfo(x_display, ...)`, and the final `XCloseDisplay(x_display)`.

Update the existing call site inside `GetIdleTimeSeconds()` from `GetIdleTimeXss()` to `GetIdleTimeXss(std::string {})`.

- [ ] **Step 2: Build to confirm the refactor is clean**

Run: `cmake --build build/dev -j$(nproc)`
Expected: builds clean, behavior unchanged.

- [ ] **Step 3: Create the header**

Create `idle_sources_system.h`:

```cpp
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

namespace IdleDetect {

//!
//! \brief Idle source for one Wayland endpoint, backed by ext_idle_notifier_v1.
//!
//! Owns exactly one WaylandIdleMonitor bound to a specific socket. No inhibition handling:
//! inhibition is a per-user shell concern, evaluated by IdleSourcePool.
//!
class WaylandIdleSource : public IdleSource
{
public:
    //! \brief Constructor. Does not connect; call Start().
    explicit WaylandIdleSource(std::string socket_name);

    //! \brief Destructor. Stops the monitor if running.
    ~WaylandIdleSource() override;

    WaylandIdleSource(const WaylandIdleSource&) = delete;
    WaylandIdleSource& operator=(const WaylandIdleSource&) = delete;

    //!
    //! \brief Connects to the socket and starts the monitor thread.
    //! \param notification_timeout_ms idle notification threshold
    //! \return true on success
    //!
    bool Start(int notification_timeout_ms);

    //! \brief Stops the monitor and releases Wayland resources.
    void Stop();

    int64_t ResolveIdleSeconds() override;
    std::string Describe() const override;

private:
    std::string m_socket_name;
    std::unique_ptr<WaylandIdleMonitor> m_monitor;
};

//!
//! \brief Idle source for one X display, backed by XScreenSaver.
//!
//! Stateless: the X connection is opened and closed per query, so this source cannot go
//! stale and needs no liveness eviction.
//!
class X11IdleSource : public IdleSource
{
public:
    //! \brief Constructor.
    explicit X11IdleSource(std::string display);

    int64_t ResolveIdleSeconds() override;
    std::string Describe() const override;

private:
    std::string m_display;
};

//!
//! \brief Determines which desktop shell owns the per-user session bus and whether it
//! reports idle inhibition.
//!
//! The shell is a per-user singleton living under user@UID.service, not attributable to any
//! logind session, so it is detected by bus name ownership rather than session enumeration.
//!
class ShellMonitor
{
public:
    //!
    //! \brief Detects the current shell by checking bus name ownership.
    //! \return ShellKind::KDE, ShellKind::GNOME, or ShellKind::NONE
    //!
    ShellKind DetectShellKind() const;

    //!
    //! \brief Checks whether the given shell reports idle inhibition.
    //! \param kind shell to query
    //! \return true if inhibited, false if not or if the query failed
    //!
    bool IsInhibited(ShellKind kind) const;
};

//!
//! \brief Idle source for the per-user desktop shell.
//!
//! Deliberately not tied to any endpoint. With N endpoints and one shell, at most one
//! endpoint is the shell's screen and we cannot determine which, so fusing the shell into
//! an endpoint's chain would make every endpoint report the shell's idle time and discard
//! the others' real activity.
//!
class ShellIdleSource : public IdleSource
{
public:
    //! \brief Constructor.
    explicit ShellIdleSource(ShellKind kind);

    //! \brief Updates the shell kind after a NameOwnerChanged transition.
    void SetKind(ShellKind kind);

    int64_t ResolveIdleSeconds() override;
    std::string Describe() const override;

private:
    ShellKind m_kind;
};

} // namespace IdleDetect

#endif // IDLE_SOURCES_SYSTEM_H
```

- [ ] **Step 4: Create the implementation**

Create `idle_sources_system.cpp`:

```cpp
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
bool SessionBusNameHasOwner(const char* name)
{
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);

    if (!connection) {
        return false;
    }

    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "NameHasOwner",
                                                   g_variant_new("(s)", name),
                                                   G_VARIANT_TYPE("(b)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   500,
                                                   nullptr, &error);
    bool has_owner = false;

    if (error) {
        debug_log("INFO: %s: Error checking D-Bus owner for %s: %s", __func__, name, error->message);
        g_error_free(error);
    } else if (result) {
        g_variant_get(result, "(b)", &has_owner);
        g_variant_unref(result);
    }

    g_object_unref(connection);

    return has_owner;
}

} // anonymous namespace

// --- WaylandIdleSource ---

WaylandIdleSource::WaylandIdleSource(std::string socket_name)
    : m_socket_name(std::move(socket_name))
    , m_monitor(std::make_unique<WaylandIdleMonitor>())
{}

WaylandIdleSource::~WaylandIdleSource()
{
    Stop();
}

bool WaylandIdleSource::Start(int notification_timeout_ms)
{
    return m_monitor->Start(m_socket_name, notification_timeout_ms);
}

void WaylandIdleSource::Stop()
{
    if (m_monitor && m_monitor->IsAvailable()) {
        m_monitor->Stop();
    }
}

int64_t WaylandIdleSource::ResolveIdleSeconds()
{
    if (!m_monitor->IsAvailable()) {
        return IDLE_ERROR;
    }

    return m_monitor->GetIdleSeconds();
}

std::string WaylandIdleSource::Describe() const
{
    return "wayland:" + m_socket_name;
}

// --- X11IdleSource ---

X11IdleSource::X11IdleSource(std::string display)
    : m_display(std::move(display))
{}

int64_t X11IdleSource::ResolveIdleSeconds()
{
    return GetIdleTimeXss(m_display);
}

std::string X11IdleSource::Describe() const
{
    return "x11:" + m_display;
}

// --- ShellMonitor ---

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

    return false;
}

// --- ShellIdleSource ---

ShellIdleSource::ShellIdleSource(ShellKind kind)
    : m_kind(kind)
{}

void ShellIdleSource::SetKind(ShellKind kind)
{
    m_kind = kind;
}

int64_t ShellIdleSource::ResolveIdleSeconds()
{
    switch (m_kind) {
    case ShellKind::KDE:
        // ksmserver's GetSessionIdleTime is gone on Plasma 6 Wayland but still present on
        // X11. It handles inhibition internally, so no separate check is added here.
        return GetIdleTimeKdeDBus();
    case ShellKind::GNOME:
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
```

- [ ] **Step 5: Wire into CMake**

In `SOURCES_IDLE_DETECT` only, after `"session_discovery.cpp"`:

```cmake
    "session_discovery.cpp"
    "idle_sources_system.h"
    "idle_sources_system.cpp"
```

Do **not** add these to `idle_detect_tests` — they pull in GIO, X11, and Wayland.

- [ ] **Step 6: Build**

Run: `cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug && cmake --build build/dev -j$(nproc)`
Expected: builds clean.

- [ ] **Step 7: Confirm the test binary still links without system deps**

Run: `ctest --test-dir build/dev --output-on-failure`
Expected: PASS, 132 tests. If linking fails here, a system header leaked into `idle_source.cpp` or `session_discovery.cpp`.

- [ ] **Step 8: Commit**

```bash
git add idle_sources_system.h idle_sources_system.cpp idle_detect.h idle_detect.cpp CMakeLists.txt
git commit -m "Add per-endpoint and shell idle sources

WaylandIdleSource wraps one socket-bound WaylandIdleMonitor. X11IdleSource
wraps XScreenSaver for one display and is stateless, so it cannot go stale.
ShellIdleSource represents the per-user desktop shell and is deliberately not
tied to an endpoint. Un-statics the D-Bus helpers and parameterizes
GetIdleTimeXss() by display.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Phase 3 — Wiring

### Task 8: `IdleSourcePool` with reconciliation

**Files:**
- Modify: `idle_sources_system.h`, `idle_sources_system.cpp`

**Interfaces:**
- Consumes: everything from Tasks 4, 5, 7.
- Produces:
  - `class IdleDetect::IdleSourcePool` with:
    - `void Reconcile(const std::set<Endpoint>& endpoints, ShellKind shell, int notification_timeout_ms);`
    - `int64_t GetIdleSeconds();`
    - `void Shutdown();`

- [ ] **Step 1: Declare the pool**

Append to `idle_sources_system.h` inside `namespace IdleDetect`, before the closing brace:

```cpp
//!
//! \brief Owns all live idle sources and produces the single aggregate idle value.
//!
//! Endpoint sources are reconciled against discovery: newly-seen endpoints are started,
//! endpoints that disappeared are torn down and evicted. Eviction is immediate rather than
//! deferred, because a source left reporting against a dead compositor would pin the
//! aggregate to "active" indefinitely.
//!
class IdleSourcePool
{
public:
    IdleSourcePool();
    ~IdleSourcePool();

    IdleSourcePool(const IdleSourcePool&) = delete;
    IdleSourcePool& operator=(const IdleSourcePool&) = delete;

    //!
    //! \brief Brings the live source set in line with the given endpoints and shell.
    //! \param endpoints candidate endpoints from discovery
    //! \param shell currently detected shell kind
    //! \param notification_timeout_ms idle notification threshold for new Wayland sources
    //!
    void Reconcile(const std::set<Endpoint>& endpoints, ShellKind shell, int notification_timeout_ms);

    //!
    //! \brief Resolves every live source and aggregates the results.
    //! \return 0 if inhibited, IDLE_NO_GUI_SESSION if no sources exist, IDLE_ERROR if none
    //!         resolved, otherwise the minimum resolved value
    //!
    int64_t GetIdleSeconds();

    //! \brief Tears down every source.
    void Shutdown();

private:
    //! \brief Guards the source maps against concurrent reconcile and query.
    mutable std::mutex mtx_pool;

    //! \brief One source per live endpoint, keyed by endpoint.
    std::map<Endpoint, std::unique_ptr<IdleSource>> m_endpoint_sources;

    //! \brief The single shell source, null when no shell is present.
    std::unique_ptr<ShellIdleSource> m_shell_source;

    //! \brief Currently detected shell kind.
    ShellKind m_shell_kind;

    //! \brief Used to evaluate the inhibition override.
    ShellMonitor m_shell_monitor;
};
```

Add `#include <map>`, `#include <mutex>`, and `#include <set>` to the header includes.

- [ ] **Step 2: Implement the pool**

Append to `idle_sources_system.cpp` inside `namespace IdleDetect`:

```cpp
// --- IdleSourcePool ---

IdleSourcePool::IdleSourcePool()
    : m_shell_kind(ShellKind::NONE)
{}

IdleSourcePool::~IdleSourcePool()
{
    Shutdown();
}

void IdleSourcePool::Reconcile(const std::set<Endpoint>& endpoints,
                               ShellKind shell,
                               int notification_timeout_ms)
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    // --- Evict sources whose endpoint disappeared. ---
    for (auto iter = m_endpoint_sources.begin(); iter != m_endpoint_sources.end();) {
        if (endpoints.count(iter->first) == 0) {
            normal_log("INFO: %s: Evicting idle source %s.", __func__, iter->second->Describe().c_str());
            iter = m_endpoint_sources.erase(iter);
        } else {
            ++iter;
        }
    }

    // --- Add sources for newly-seen endpoints. ---
    for (const Endpoint& endpoint : endpoints) {
        if (m_endpoint_sources.count(endpoint) > 0) {
            continue;
        }

        if (endpoint.m_kind == EndpointKind::WAYLAND) {
            auto source = std::make_unique<WaylandIdleSource>(endpoint.m_identifier);

            // Validate by connecting. A candidate that will not connect is not an endpoint.
            if (!source->Start(notification_timeout_ms)) {
                error_log("%s: Could not start Wayland idle source for %s; not adding.",
                          __func__,
                          endpoint.ToString().c_str());
                continue;
            }

            normal_log("INFO: %s: Added idle source %s.", __func__, source->Describe().c_str());
            m_endpoint_sources.emplace(endpoint, std::move(source));
        } else {
            auto source = std::make_unique<X11IdleSource>(endpoint.m_identifier);

            // X11 sources are stateless, so validate by taking one reading.
            if (source->ResolveIdleSeconds() == IDLE_ERROR) {
                error_log("%s: Could not query X display %s; not adding.",
                          __func__,
                          endpoint.ToString().c_str());
                continue;
            }

            normal_log("INFO: %s: Added idle source %s.", __func__, source->Describe().c_str());
            m_endpoint_sources.emplace(endpoint, std::move(source));
        }
    }

    // --- Reconcile the shell source. ---
    if (shell == ShellKind::NONE) {
        if (m_shell_source) {
            normal_log("INFO: %s: Desktop shell went away; dropping shell idle source.", __func__);
            m_shell_source.reset();
        }
    } else if (!m_shell_source) {
        normal_log("INFO: %s: Desktop shell appeared; adding shell idle source.", __func__);
        m_shell_source = std::make_unique<ShellIdleSource>(shell);
    } else if (shell != m_shell_kind) {
        normal_log("INFO: %s: Desktop shell changed; updating shell idle source.", __func__);
        m_shell_source->SetKind(shell);
    }

    m_shell_kind = shell;
}

int64_t IdleSourcePool::GetIdleSeconds()
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    const bool any_source_present = !m_endpoint_sources.empty() || m_shell_source != nullptr;

    // Inhibition is a global override: there is one shell, and inhibition means "do not let
    // this machine go idle".
    const bool inhibited = (m_shell_kind != ShellKind::NONE) && m_shell_monitor.IsInhibited(m_shell_kind);

    std::vector<int64_t> resolved;
    resolved.reserve(m_endpoint_sources.size() + 1);

    for (auto& entry : m_endpoint_sources) {
        const int64_t value = entry.second->ResolveIdleSeconds();

        debug_log("INFO: %s: Source %s resolved to %lld.",
                  __func__,
                  entry.second->Describe().c_str(),
                  (long long)value);

        resolved.push_back(value);
    }

    if (m_shell_source) {
        const int64_t value = m_shell_source->ResolveIdleSeconds();

        debug_log("INFO: %s: Source %s resolved to %lld.",
                  __func__,
                  m_shell_source->Describe().c_str(),
                  (long long)value);

        resolved.push_back(value);
    }

    return AggregateIdleSeconds(resolved, inhibited, any_source_present);
}

void IdleSourcePool::Shutdown()
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    m_endpoint_sources.clear();
    m_shell_source.reset();
    m_shell_kind = ShellKind::NONE;
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build/dev -j$(nproc)`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add idle_sources_system.h idle_sources_system.cpp
git commit -m "Add IdleSourcePool with endpoint reconciliation

Starts sources for newly-discovered endpoints, validating each by connecting,
and evicts sources whose endpoint disappeared. Applies the inhibition override
and delegates to AggregateIdleSeconds().

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: Replace `GetIdleTimeSeconds()` and wire the pool into `main()`

**Files:**
- Modify: `idle_detect.cpp` — `GetIdleTimeSeconds()` (lines 710-785), `IsTtySession()`, `IsWaylandSession()`, `IsKdeSession()`, `g_wayland_idle_monitor` global, `main()` (lines 1798-1833)
- Modify: `idle_detect.h` — remove `WaylandIdleMonitor` from the public surface if no longer referenced outside `WaylandIdleSource`

**Interfaces:**
- Consumes: `IdleSourcePool`, `ShellMonitor` from Task 8; `DiscoverEndpoints`, `DiscoveryHints` from Task 5.
- Produces:
  - `IdleDetect::DiscoveryHints IdleDetect::BuildDiscoveryHints();` — gathers live hints from the running system.
  - `g_idle_source_pool` global replacing `g_wayland_idle_monitor`.

- [ ] **Step 1: Add the live hint gatherer**

Add to `idle_sources_system.h` inside `namespace IdleDetect`:

```cpp
//!
//! \brief Gathers discovery hints from the running system.
//!
//! Reads $XDG_RUNTIME_DIR, /tmp/.X11-unix, and the DISPLAY value from the systemd user
//! manager Environment property. That property is annotated EmitsChangedSignal("false"), so
//! it is read on demand rather than subscribed to.
//!
//! \return populated hints
//!
DiscoveryHints BuildDiscoveryHints();
```

Add `#include <session_discovery.h>` to that header.

Add to `idle_sources_system.cpp`, in the anonymous namespace:

```cpp
//!
//! \brief Reads DISPLAY out of the systemd user manager Environment property.
//! \return DISPLAY value, or empty if unavailable
//!
std::string ReadDisplayFromSystemdUserManager()
{
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);

    if (!connection) {
        return std::string {};
    }

    GError* error = nullptr;
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
                                                   500,
                                                   nullptr, &error);
    std::string display;

    if (error) {
        debug_log("INFO: %s: Could not read systemd user manager Environment: %s", __func__, error->message);
        g_error_free(error);
        g_object_unref(connection);

        return display;
    }

    if (result) {
        GVariant* boxed = nullptr;
        g_variant_get(result, "(v)", &boxed);

        if (boxed) {
            gsize count = 0;
            const gchar** entries = g_variant_get_strv(boxed, &count);

            if (entries) {
                for (gsize i = 0; i < count; ++i) {
                    const std::string entry(entries[i]);

                    if (entry.rfind("DISPLAY=", 0) == 0) {
                        display = entry.substr(8);
                        break;
                    }
                }

                g_free(entries);
            }

            g_variant_unref(boxed);
        }

        g_variant_unref(result);
    }

    g_object_unref(connection);

    return display;
}
```

Add the public function to `idle_sources_system.cpp`:

```cpp
DiscoveryHints BuildDiscoveryHints()
{
    DiscoveryHints hints;

    std::optional<std::string> runtime_dir = GetEnvVariable("XDG_RUNTIME_DIR");
    if (runtime_dir.has_value()) {
        hints.m_xdg_runtime_dir = *runtime_dir;
    }

    hints.m_x11_socket_dir = "/tmp/.X11-unix";
    hints.m_uid = getuid();

    const std::string display = ReadDisplayFromSystemdUserManager();
    if (!display.empty()) {
        hints.m_env_displays.push_back(display);
    }

    // The process environment is a hint too, but only a hint: it is frozen at exec and is
    // exactly what this whole mechanism exists to stop depending on.
    std::optional<std::string> env_display = GetEnvVariable("DISPLAY");
    if (env_display.has_value() && !env_display->empty()) {
        hints.m_env_displays.push_back(*env_display);
    }

    return hints;
}
```

Add `#include <unistd.h>` and `#include <optional>` to `idle_sources_system.cpp`.

`GetEnvVariable()` is declared at `util.h:188` as
`std::optional<std::string> GetEnvVariable(const std::string& var_name);` — verified, use as written.

- [ ] **Step 2: Replace `GetIdleTimeSeconds()`**

In `idle_detect.cpp`, replace the entire body of `int64_t GetIdleTimeSeconds() {...}` (lines 710-785) with:

```cpp
int64_t GetIdleTimeSeconds() {
    return g_idle_source_pool.GetIdleSeconds();
}
```

Delete `static bool IsTtySession()` and `static bool IsWaylandSession()` entirely — they read `getenv()` and are the root cause of the bug. Keep `IsKdeSession()` only if something still references it; otherwise delete it too, since `ShellMonitor::DetectShellKind()` supersedes it.

Run `grep -n "IsTtySession\|IsWaylandSession\|IsKdeSession" idle_detect.cpp idle_detect.h` and remove every remaining reference.

- [ ] **Step 3: Replace the global**

Replace the `g_wayland_idle_monitor` definition with:

```cpp
//!
//! \brief Global pool of live idle sources, reconciled from discovery.
//!
IdleDetect::IdleSourcePool g_idle_source_pool;
```

Declare it in `idle_detect.h` inside `namespace IdleDetect` as `extern IdleSourcePool g_idle_source_pool;` — or, if that creates an include cycle with `idle_sources_system.h`, declare it in `idle_sources_system.h` instead and include that from `idle_detect.cpp`. Prefer the latter.

- [ ] **Step 4: Replace the startup block in `main()`**

Replace lines 1798-1810 (the `// Start Wayland Monitor AFTER control monitor` block) with:

```cpp
    // Perform an initial discovery pass. Finding nothing is a normal state, not a failure:
    // the GUI session may not exist yet, and the reconcile in the main loop will pick it up
    // when it appears.
    const int notification_timeout_ms = 1000;
    IdleDetect::ShellMonitor shell_monitor;

    g_idle_source_pool.Reconcile(IdleDetect::DiscoverEndpoints(IdleDetect::BuildDiscoveryHints()),
                                 shell_monitor.DetectShellKind(),
                                 notification_timeout_ms);
```

- [ ] **Step 5: Reconcile on each main loop iteration**

Immediately before `int64_t idle_seconds = IdleDetect::GetIdleTimeSeconds();` in the main loop, add:

```cpp
        // Reconcile discovery every iteration. This is the self-heal path that lets a GUI
        // session appearing after startup be picked up without restarting the daemon.
        g_idle_source_pool.Reconcile(IdleDetect::DiscoverEndpoints(IdleDetect::BuildDiscoveryHints()),
                                     shell_monitor.DetectShellKind(),
                                     notification_timeout_ms);

```

- [ ] **Step 6: Fix the `using_event_detect_as_only_source` latch**

The existing flag is set once and never cleared, so a process that starts tty-only stays in that mode forever even after a GUI appears. Replace:

```cpp
        } else if (idle_seconds == -2 && !use_event_detect) {
            debug_log("INFO: %s: Tty session. Overriding use_event_detect and using event_detect anyway.",
                      __func__);
            using_event_detect_as_only_source = true;
        }
```

with:

```cpp
        } else if (idle_seconds == -2 && !use_event_detect) {
            debug_log("INFO: %s: No GUI session. Overriding use_event_detect and using event_detect anyway.",
                      __func__);
            using_event_detect_as_only_source = true;
        } else {
            // Clear the override once a GUI session exists, so a daemon that started before
            // the GUI session does not stay pinned to event_detect for its whole lifetime.
            using_event_detect_as_only_source = false;
        }
```

- [ ] **Step 7: Shut the pool down cleanly**

Find the shutdown sequence near the end of `main()` (where `IdleDetectControlMonitor` is stopped) and add before it:

```cpp
    g_idle_source_pool.Shutdown();
```

- [ ] **Step 8: Build**

Run: `cmake --build build/dev -j$(nproc)`
Expected: builds clean. Any remaining reference to `g_wayland_idle_monitor`, `IsTtySession`, or `IsWaylandSession` will surface here.

- [ ] **Step 9: Verify normal operation**

Run: `./build/dev/idle_detect 2>&1 | head -40`
Expected: log lines showing "Added idle source wayland:wayland-0" (and/or an x11 source), a detected shell, and a plausible idle time. Ctrl-C to exit.

- [ ] **Step 10: Verify the #12 reproduction is fixed**

This is the whole point of the change. In a VT or SSH session with no GUI variables:

```bash
env -u DISPLAY -u WAYLAND_DISPLAY ./build/dev/idle_detect 2>&1 | head -40
```

Expected: the daemon still discovers `wayland:wayland-0` from `$XDG_RUNTIME_DIR` and reports GUI idle time, rather than falling to tty-only. Before this change the same command reports "TTY session detected" forever.

- [ ] **Step 11: Commit**

```bash
git add idle_detect.h idle_detect.cpp idle_sources_system.h idle_sources_system.cpp
git commit -m "Replace getenv() session typing with runtime endpoint discovery

GetIdleTimeSeconds() now delegates to IdleSourcePool, which is reconciled
against discovery every main loop iteration. Deletes IsTtySession() and
IsWaylandSession(), which read the process environment frozen at exec and
could never observe a GUI session appearing later. Also clears the
using_event_detect_as_only_source latch once a GUI session shows up.

Fixes the root cause reported in #12.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: Retire `idle_detect_wrapper.sh`

**Files:**
- Delete: `idle_detect_wrapper.sh.in`
- Modify: `dc_idle_detection.service.in:8`
- Modify: `CMakeLists.txt:146`, `CMakeLists.txt:259`, `CMakeLists.txt:370`
- Modify: `docs/ROADMAP_1.0.md:178`

**Interfaces:**
- Consumes: Task 9 (the binary must handle late GUI sessions before the wrapper can go).
- Produces: nothing.

- [ ] **Step 1: Point the unit at the binary**

In `dc_idle_detection.service.in`, replace:

```ini
ExecStartPre=/bin/sleep 5
ExecStart=@CMAKE_INSTALL_FULL_BINDIR@/idle_detect_wrapper.sh
```

with:

```ini
ExecStart=@CMAKE_INSTALL_FULL_BINDIR@/idle_detect
```

The `ExecStartPre=/bin/sleep 5` goes away with it: startup is no longer order-sensitive, because finding no endpoints is a normal state that resolves on a later reconcile.

- [ ] **Step 2: Remove the CMake generation, install, and dev-copy rules**

Delete `CMakeLists.txt:146`:

```cmake
configure_file(idle_detect_wrapper.sh.in ${CMAKE_CURRENT_BINARY_DIR}/idle_detect_wrapper.sh @ONLY)
```

Delete the `install(FILES ...)` block at `CMakeLists.txt:259` that installs `idle_detect_wrapper.sh`.

Delete the line at `CMakeLists.txt:370`:

```cmake
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/idle_detect_wrapper.sh" "${INSTALL_BIN_DIR}/idle_detect_wrapper.sh"
```

- [ ] **Step 3: Delete the script and update the roadmap reference**

```bash
git rm idle_detect_wrapper.sh.in
```

In `docs/ROADMAP_1.0.md:178`, remove the `idle_detect_wrapper.sh.in` entry from the file listing.

- [ ] **Step 4: Verify nothing still references it**

Run: `grep -rn "idle_detect_wrapper" --include='*.txt' --include='*.in' --include='*.sh' --include='*.md' . | grep -v '^./build/' | grep -v '^./docs/html/' | grep -v '^./docs/superpowers/'`
Expected: no output. (The spec and this plan legitimately mention it in prose; `docs/superpowers/` is excluded for that reason.)

- [ ] **Step 5: Verify a clean configure and build**

Run: `rm -rf build/dev && cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug && cmake --build build/dev -j$(nproc)`
Expected: configures and builds clean with no reference to the removed script.

- [ ] **Step 6: Verify the generated unit file**

Run: `grep -A3 "\[Service\]" build/dev/dc_idle_detection.service`
Expected: `ExecStart` points at `.../bin/idle_detect`, and there is no `ExecStartPre`.

- [ ] **Step 7: Commit**

```bash
git add -A dc_idle_detection.service.in CMakeLists.txt docs/ROADMAP_1.0.md
git commit -m "Retire idle_detect_wrapper.sh

The wrapper polled systemctl --user show-environment for DISPLAY and
WAYLAND_DISPLAY, then exec'd idle_detect without importing them, so the binary
received a stale environment anyway. Its job now happens in-process where it
can be re-run instead of being a one-shot gate. ExecStartPre=/bin/sleep 5 goes
with it, since startup is no longer order-sensitive.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: Add the missing `event_count_files_path` config line

**Files:**
- Modify: `idle_detect.conf.in`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

This is the surface-level report in issue #12. It is a real consistency gap — `event_detect.conf.in:2` has the line and `idle_detect.conf.in` does not — but it is **not** the cause of the reporter's symptom, since `IdleDetectConfig::ProcessArgs()` defaults it to `/run/event_detect` and `Config::GetArgString()` returns the default cleanly on a missing key. Task 9 fixes the actual cause. This task closes the documentation gap so the file is self-describing.

- [ ] **Step 1: Add the line**

In `idle_detect.conf.in`, after `debug=0`:

```
debug=0
event_count_files_path="/run/event_detect"
use_event_detect=1
```

- [ ] **Step 2: Verify the default is unchanged**

Run: `grep -n 'event_count_files_path' idle_detect.cpp`
Expected: line ~97 shows `GetArgString("event_count_files_path", "/run/event_detect")` — the config file value and the code default now agree.

- [ ] **Step 3: Build and confirm the generated config**

Run: `cmake --build build/dev -j$(nproc) && cat build/dev/idle_detect.conf`
Expected: the generated file contains the new line.

- [ ] **Step 4: Commit**

```bash
git add idle_detect.conf.in
git commit -m "Add event_count_files_path to idle_detect.conf

event_detect.conf has always carried this line; idle_detect.conf did not,
which made the file look incomplete. Behavior is unchanged, since
ProcessArgs() already defaults it to /run/event_detect.

Reported in #12.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Manual verification

Run these after Task 11. They cover behavior the unit tests cannot reach.

- [ ] **Cold start before the GUI (the #12 reproduction).** Stop the user service, log out to the greeter, log back in, and confirm the service picks up the GUI session:

```bash
systemctl --user stop dc_idle_detection.service
# log out to the greeter, log back in
systemctl --user start dc_idle_detection.service
journalctl --user -u dc_idle_detection.service -n 50
```

Expected: "Added idle source wayland:..." and real GUI idle times, not tty-only.

- [ ] **Compositor restart.** With the daemon running, restart the compositor (or kill and restart a nested one). Expected: the old source is evicted, a new one is added on the next reconcile, and no crash appears in `journalctl --user -u dc_idle_detection.service`.

- [ ] **Second X11 endpoint.** With the console session running, start a virtual display and confirm both are monitored:

```bash
vncserver :2
journalctl --user -u dc_idle_detection.service -f
```

Expected: log shows both `x11::2` (or `wayland:...` for the console) and the second endpoint added. Move the mouse in the VNC session while the console sits idle; the reported idle time should drop, proving `min()` across endpoints works.

Then `vncserver -kill :2` and confirm the source is evicted.

- [ ] **Nested Wayland compositor (N>1 Wayland).** Run `weston` nested inside the current session, confirm `wayland-1` appears in `$XDG_RUNTIME_DIR` and a second Wayland source is added, then close it and confirm eviction.

- [ ] **No regression in the packaged install.** Run `sudo ./install.sh --prefix=/usr/local` and `./user_install.sh` on a scratch machine or VM, and confirm the service starts without the wrapper.

---

## Self-Review

**Spec coverage.** Every spec section maps to a task:

| Spec section | Task |
|---|---|
| Root cause / `getenv()` latch | 9 |
| Wrapper propagation failure | 10 |
| Governing principle (union of hints) | 5 |
| Two-layer architecture | 7, 8 |
| Priority within a source, `min()` across | 4, 7 |
| Sentinel contract | 4 |
| Shell is a source, not an endpoint property | 7 (`ShellIdleSource`), 4 (regression test) |
| `EndpointDiscovery` | 5, 9 (`BuildDiscoveryHints`) |
| `ShellMonitor` | 7 |
| `IdleSource` / `WaylandIdleSource` / `X11IdleSource` | 6, 7 |
| `IdleSourcePool` | 8 |
| N X11 endpoints | 5, 7, 8 |
| N Wayland endpoints, not hard-coded to 1 | 6, 8 |
| Teardown on socket loss / connection error | 8 (eviction) |
| VT switch is not teardown | Satisfied by construction: nothing reads `Session.Active` |
| Triggers and reconcile backstop | 9 — **see deviation below** |
| Error handling / containment | 8 |
| Removals | 9, 10 |
| Testing | 4, 5, manual section |
| Sequencing (#11 first) | Phase 0 |
| `event_count_files_path` gap | 11 |

**One deliberate deviation from the spec.** The spec specifies inotify on `$XDG_RUNTIME_DIR` plus `NameOwnerChanged` subscriptions as the primary triggers, with a ~30 s reconcile as a backstop. This plan implements **only the reconcile**, running it every main loop iteration (the loop already ticks on `check_interval_seconds`). Rationale: it is materially simpler, it delivers the entire user-visible fix, and the polling cost is one `readdir` plus two short D-Bus calls per tick against a loop that already makes D-Bus calls. Signal-driven triggers are a latency optimization — they would shorten worst-case detection from one tick to near-instant — and should be a follow-up once this is proven in the field. Flagged rather than silently dropped; the spec's `RECONCILE_INTERVAL_SECONDS` constant is correspondingly not introduced, since the main loop interval serves that role.

**Placeholder scan.** No `TBD`/`TODO`/"handle edge cases"/"similar to Task N" present. One step directs the implementer to choose between two include arrangements (Task 9 Step 3); it states the preferred option and the fallback, which is a real instruction rather than a placeholder.

**Type consistency.** Checked across tasks: `Endpoint{EndpointKind, std::string}` used identically in Tasks 4, 5, 8; `ShellKind` in 4, 7, 8; `IDLE_ERROR`/`IDLE_NO_GUI_SESSION` in 4, 7, 8; `WaylandIdleMonitor::Start(const std::string&, int)` defined in 6 and called in 7; `GetIdleTimeXss(const std::string&)` redefined in 7 and called in 7; `DiscoveryHints` defined in 5 and populated in 9; `AggregateIdleSeconds(vector, bool, bool)` defined in 4 and called in 8; `IdleSourcePool::Reconcile(set, ShellKind, int)` defined in 8 and called in 9 with matching arity.
