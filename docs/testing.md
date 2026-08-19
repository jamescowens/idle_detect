# Testing record — 0.9.2.0

What was tested before the 0.9.2.0 release, on what, and what it found. This is a
record of work actually performed, not a test plan: everything below was run and
its result observed. Where something was *not* covered, it is listed under
[Gaps](#gaps).

0.9.2.0 is largely a correctness release. The GUI-session-readiness rework touched
session discovery, endpoint lifetime, and the control-script layer, so testing
concentrated on real desktops and real reboots rather than synthetic cases.

## Test environment

Seven machines, chosen to span the filesystem layouts, security modules, desktops
and display servers the project has to work on.

| host | distro | desktop / server | security module | install | role |
|---|---|---|---|---|---|
| jco-monster-ng | openSUSE Leap 16.0 | KDE Plasma, Wayland | — | source | primary workstation, live BOINC + FAH v7 |
| media-2 | openSUSE Leap 16.0 | KDE Plasma, Wayland | **SELinux enforcing** | source | SELinux reference, BOINC 8.2.15 + FAH v8 |
| jco-linux2 | openSUSE Tumbleweed | — | AppArmor | source | long-running install, BOINC shmem contract |
| jco-linux3 | Ubuntu 22.04 | — | AppArmor | source | clean-slate installs, older systemd |
| jco-linux4 | Ubuntu 24.04 | — | AppArmor | **.deb** | package build and install |
| jco-linux5 | Ubuntu 24.04 | GNOME | AppArmor | source | GNOME / Mutter idle path |
| jco-linux6 | Fedora 37 | — | SELinux | source | RPM-family layout |

Compilers exercised: GCC 13.3, 15.3 and 16.2, and Clang, via the build matrix and
local builds.

## Unit tests

214 tests across `tests/util_tests.cpp`, `tests/event_message_tests.cpp` and
`tests/config_tests.cpp`, all passing. The test binary links only `util.cpp` plus
the dependency-free layers (`idle_source`, `session_discovery`,
`idle_source_pool`), so it needs no D-Bus, X11, Wayland or libevdev at test time.
`idle_sources_system.cpp`, which holds every call into those libraries, is
deliberately excluded.

## Functional testing

### Session discovery and GUI readiness (issue #12)

The failure being fixed: `idle_detect` froze its view of the session at process
start via `getenv()`, so a daemon that outlived a GUI session, or started before
one existed, never recovered and fell back to tty-only detection.

Verified on jco-monster-ng (KDE Wayland) that discovery reads the systemd user
manager environment at reconcile time rather than process start, obtaining
`DISPLAY`, `WAYLAND_DISPLAY` and `XAUTHORITY` live, binding `ext_idle_notifier_v1`
and resolving `org.kde.ksmserver`.

The decisive test was a **real logout and login** on media-2:

| | before login | after login |
|---|---|---|
| user manager environment | *empty* | `DISPLAY=:1`, `WAYLAND_DISPLAY=wayland-0`, `XAUTHORITY=/run/user/1000/xauth_zByyij` |
| GUI sockets held | none | `fd 8 -> /run/user/1000/wayland-0` |
| threads | 5 | 6 |

The same process (pid unchanged across the transition) lost its compositor and
recovered on its own. `XAUTHORITY` pointed at a *different* file after the
relogin, which is what makes the point: a daemon holding values from process start
would have been referencing a deleted path and a dead socket.

On compositor death the Wayland thread exited cleanly
(`Error/Hangup on Wayland display FD`), the monitor was marked unavailable, and no
stale socket was retained.

Endpoint validation was observed rejecting a bad candidate rather than trusting a
hint: an XWayland `DISPLAY=:1` advertised by the user manager failed its
XScreenSaver probe and was placed on the backoff ladder (1, 2, … 64 reconcile
ticks) **without contributing to the aggregate**.

media-2 exercised the hardest variant, with several `closing` Wayland sessions and
an SDDM X11 greeter session present alongside the live session. Discovery selected
the live one.

### Control scripts

`dc_pause` / `dc_unpause` drive BOINC and Folding@home independently: each is
attempted only if present, and neither the absence nor the failure of one prevents
the other. Verified round-trip in both directions on:

- **jco-monster-ng** — BOINC `never`/`always` plus FAH v7 pause/resume, confirmed
  by FahCore processes starting and stopping.
- **media-2** — BOINC 8.2.15 plus FAH v8, under SELinux enforcing.
- **jco-linux4** — FAH v8 with BOINC deliberately unreadable, confirming the
  independence property and the diagnostic below.

An unattended, naturally triggered transition was observed on jco-monster-ng: with
`inactivity_time_trigger=300`, `FS01:Unpaused` appeared 5 minutes after the last
input, and `FS01:Paused` on return. This is the path that matters in normal use
and had not previously been observed end to end.

Where BOINC is installed but `gui_rpc_auth.cfg` is unreadable — the default
`root:boinc 0640` on Debian and openSUSE — the scripts now report it instead of
skipping silently. Confirmed the advice they print is correct: adding the user to
the `boinc` group changed the same invocation from a reported failure to driving
both clients.

### Folding@home v8 control

v8 ships **no command-line control at all**. The official `fahctl` is not installed
by the package, requires the third-party `websocket-client` module, and against
client 8.1.18 sends a message shape the client ignores while still exiting 0 — it
reports success while doing nothing. Measured on jco-linux4:

| message | effect on 8.1.18 |
|---|---|
| `{"cmd":"state","state":"pause"}` — what `fahctl` sends | none |
| `{"cmd":"pause"}` | `paused = true` |
| `{"cmd":"unpause"}` | `paused = false` |

`dc_fah_v8` therefore speaks the WebSocket API directly using only the Python 3
standard library, and **re-reads client state to confirm** rather than trusting an
exit code. Verified with `python3-websocket` deliberately uninstalled, and
verified to exit non-zero against a stopped client.

### BOINC shared-memory contract

The `int64_t[2]` layout is frozen. On jco-linux2, the raw 16 bytes were decoded
independently as two little-endian `int64_t` and matched `read_shmem_timestamps`
exactly. BOINC 8.2.15 was confirmed consuming it, holding a read-only shared
mapping of `/dev/shm/idle_detect_shmem`, with no legacy-fallback warning logged.

### SELinux

On media-2 (enforcing), BOINC 8.2.15 could **not** read the segment. The denial is
`dontaudit`-suppressed, so `ausearch` reports nothing and it is only visible after
`semodule -DB` or by grepping the audit log directly:

```
avc: denied { read } for comm="boinc" name="idle_detect_shmem" dev="tmpfs"
  scontext=system_u:system_r:boinc_t:s0
  tcontext=system_u:object_r:tmpfs_t:s0 tclass=file permissive=0
```

The user-visible symptom is BOINC recommending that you install idle_detect while
idle_detect is installed and running. File permissions (0644) are irrelevant and
mislead the investigation.

`boinc_selinux_shmem_policy.sh` was verified from a clean state — module removed,
mapping confirmed gone, script run, mapping restored and the legacy warning
stopped — along with its idempotent re-run, `--remove`, non-root refusal, and its
no-op path on a system without SELinux. The module survives reboot.

### systemd unit placement and boot start

Found during testing: **`event_detect` had never started at boot from a source
install.** It was only ever running because `install.sh` starts it, and no machine
had been rebooted between installing and testing.

On distributions where `/usr/local` is a separate subvolume — all three openSUSE
machines here — systemd resolves the boot transaction before that filesystem is
mounted. Captured with `systemd.log_level=debug` on media-2:

```
+6.713s  dc_event_detection.service: Failed to load configuration: No such file or directory
+6.716s  Cannot add dependency job, ignoring: Unit dc_event_detection.service not found.
+7.835s  unit_file_build_name_map: normal unit file: /usr/local/lib/systemd/system/...
```

systemd loses by about 1.1 seconds, drops the job, and never re-resolves. The
message is debug-level, so nothing appears at default log level: the unit reports
`enabled`, resolves a valid `FragmentPath` when queried, starts by hand, and simply
never runs at boot. No ordering directive can help, because the job never enters
the transaction.

The installer now picks the location by comparing the filesystem holding the prefix
with the one holding `/`. Both branches verified by reboot:

| host | `/usr/local` | chosen location | result |
|---|---|---|---|
| media-2 | separate fs | `/etc/systemd/system` | active 14s after boot |
| jco-linux3 | same fs | `/usr/local/lib/systemd/system` | active 29s after boot |
| jco-linux4 | package build | `/usr/lib/systemd/system` | active |

### Install and upgrade behavior

Exercised against real, messy pre-existing installs rather than clean machines.

- **Upgrade over a 2026-04 install self-reporting 0.8.3.0** (jco-monster-ng), which
  had accumulated three hazards at once: a retired `idle_detect_wrapper.sh`, a
  hand-written unit in `/etc/systemd/system` shadowing the installed one, and both
  services already running. All three were handled: wrapper removed, shadowing unit
  backed up as `.superseded-<timestamp>` and removed, services restarted so no
  process was left running against a deleted inode.
- **Config preservation.** `cmake --install` rewrites `/etc/event_detect.conf`
  unconditionally, which silently reset a deliberate `monitor_ttys=0` to the
  default during the first 0.9.2.0 install on jco-monster-ng. `install.sh` now
  keeps the existing file and writes the incoming default alongside as `.new`.
  Verified on jco-linux3 that a hand-edited setting and an added comment both
  survive a reinstall with mode and ownership unchanged, and confirmed on the
  production host during the final reinstall.
- **Fresh install** on a clean box (jco-linux3, and media-2 which had no prior
  install) completed with no warnings.
- **Fleet reinstall.** All seven machines were brought to 0.9.2.0 and each
  independently selected the correct unit location for its layout — three
  `/etc`, three prefix, one package.

### Packaging

`dpkg-buildpackage` on Ubuntu 24.04 produced `idle-detect_0.9.2.0-1_amd64.deb`
cleanly. Verified the package contains `dc_fah_v8` and
`boinc_selinux_shmem_policy.sh` at `/usr/bin/` with mode 0755, the unit at
`/usr/lib/systemd/system/`, and that `/etc/event_detect.conf` is registered as a
**conffile** so dpkg preserves local edits. Installed from the package and
confirmed `dc_pause` locates `dc_fah_v8` as a sibling and drives the v8 client.

## Defects found and fixed during this testing

| area | defect |
|---|---|
| systemd | system unit never started at boot where the prefix is on a separate filesystem |
| install.sh | `/etc/event_detect.conf` silently overwritten on upgrade, discarding local settings |
| control scripts | Folding@home support absent entirely; BOINC-only |
| control scripts | a missing BOINC aborted the script under `set -e`, skipping Folding@home on a FAH-only host |
| control scripts | BOINC skipped without a word when its credentials were unreadable |
| FAH v8 | `fahctl` reports success while doing nothing on 8.1.18 |
| install.sh | a shadowing unit elsewhere on the system silently won over the installed one |
| install.sh | an already-running service was left executing a deleted binary after upgrade |

## Gaps

Stated explicitly rather than implied by omission.

- **Cold user-manager start.** The logout/login test on media-2 kept `user@1000`
  alive through an ssh session, so it exercised rediscovery by a *surviving*
  daemon. A login with no pre-existing session for that user — where the user
  manager itself starts fresh — has not been separately tested. It is the easier
  case, since discovery then begins with a fully populated environment.
- **Boot verification is not fleet-wide.** Placement is verified everywhere;
  boot-start after the final change was verified by reboot on media-2 and
  jco-linux3 only.
- **KDE X11 and GNOME** were exercised during development but not re-verified
  against the final build.
- **RPM packaging** is built by OBS and was not rebuilt as part of this round;
  the `.deb` path was.
- **`dc_fah_v8` against v8.3+** is untested. Only 8.1.18 was available, which is
  what the stable Debian channel serves. The newer message shape is implemented as
  a fallback but has not been exercised against a client that requires it.
- **Non-x86_64 architectures** were not tested locally.
