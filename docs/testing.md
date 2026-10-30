# Testing record — 0.9.2.0, 0.9.2.1 and 0.9.2.2

What was tested before the 0.9.2.0 release, on what, and what it found. This is a
record of work actually performed, not a test plan: everything below was run and
its result observed. Where something was *not* covered, it is listed under
[Gaps](#gaps).

0.9.2.1 is a patch release with two changes, each recorded in its own place
below: `dc_fah_v8` now selects the Folding@home v8 API by client version (see
[Folding@home v8 control](#foldinghome-v8-control)), and `event_detect` no longer
logs "No pointing devices" once a second on a machine without a mouse (see
[event_detect without pointing devices](#event_detect-without-pointing-devices)).

0.9.2.2 is a patch release with two fixes: `idle_detect` counts only
screen-idle inhibitions (see
[Desktop inhibition type](#desktop-inhibition-type-0922)), and both daemons
flush every log line (see [Log line flushing](#log-line-flushing-0922)). The
release itself is recorded under
[0.9.2.2 release verification](#0922-release-verification).

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

### Issue #11 reproduced and fixed, under container

The reported crash (segfault in `libwayland-client` on Ubuntu 26.04) was
reproduced deliberately rather than inferred. The trigger is a compositor that
advertises `wl_seat` but **not** `ext_idle_notifier_v1` — which is what Mutter
looks like to this code, and why the reporter's patch skipped GNOME.

An Ubuntu 26.04 container (libwayland-client 1.24.0, matching the reporter's
`libwayland-client.so.0.24.0`) with a ~60-line Wayland server providing exactly
that global set. Weston headless was tried first and does **not** reproduce it,
because with no input devices it advertises no `wl_seat` at all and the faulty
path is never entered.

Against that server, 0.9.1.0 crashes:

```
#4  wl_seat_release (wl_seat=0x5562a2375be0)
#5  WaylandIdleMonitor::CleanupWayland      idle_detect.cpp:1322
#6  WaylandIdleMonitor::InitializeWayland   idle_detect.cpp:1236
#7  WaylandIdleMonitor::Start               idle_detect.cpp:1132
```

The seat binds, the required-globals check fails because the notifier is absent,
and the failure path calls `wl_seat_release()` on a proxy that is no longer
valid.

| version | result against the same server |
|---|---|
| 0.9.1.0 | exit 139 — SIGSEGV, core dumped |
| 0.9.2.0 | ran to timeout, no crash; endpoint failed validation and went on the backoff ladder |

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

#### 0.9.2.1: the paused flag moved in 8.3

The 0.9.2.0 `dc_fah_v8` read the flag at `config.paused`. From 8.3 the client has
**no top-level `config.paused`**: the flag lives per resource group, at
`groups.<name>.config.paused`. Against those clients 0.9.2.0 reported failure on
`pause` although the client had paused, and reported success on `unpause` from a
check that could not fail — the silent no-op the tool exists to prevent. Resource
groups were added in 8.1.4, removed in 8.2.1 and returned in 8.3.0 (client
`CHANGELOG.md`), so the client's reported version decides which API it speaks,
not the presence of a `groups` key.

Measured per version, each client sandboxed as an ordinary user (`--cpus=0`, GPUs
hidden, its own loopback port), the flag read from the raw state after each verb:

| client | paused flag lives at | `{"cmd":"pause"/"unpause"}` | `{"cmd":"state","state":"pause"/"fold"}` |
|---|---|---|---|
| 8.1.18 | `config.paused` (no `groups` key) | works | **ignored** |
| 8.3.18 | `groups[""].config.paused` only | works | works |
| 8.4.9 | same as 8.3.18 | works | works |
| 8.5.6 | same as 8.3.18 | works | works |

`tests/integration/test_dc_fah_v8_versions.sh` downloads all four, starts each
sandboxed on a free port, runs six verified pause/unpause steps per version with
the flag read independently of the tool, then a control in which the verb is
suppressed and `apply_paused` must refuse to confirm. It needs network access and
about 16 MB of downloads, so it is a manual test, not part of CTest. Run on the
handoff's side on 2026-10-04 and reproduced here on 2026-10-07:

| script | result |
|---|---|
| 0.9.2.1 `dc_fah_v8` | ALL PASS on all four versions; every control refuses to confirm |
| 0.9.2.0 `dc_fah_v8` | `BAD pause confirmed=False actual={'': True}` on 8.3.18, 8.4.9 and 8.5.6; 8.1.18 passes |

So the test can fail, and it fails on the actual defect. (Against the 0.9.2.0
script the harness also dies in its control step, which calls `paused_flag`, a
function that script does not have — a limitation of the harness, separate from
the defect it catches.) In production, idle_detect drove one real cycle on each
API generation on 2026-10-04: media-2 on 8.1.18 (legacy) and media-3 on 8.5.6
(groups), both printing `Folding@home v8 paused.` / `resumed.` with no
`could not confirm`.

**Download-channel trap.** Upstream's `debian-stable-64bit` channel is stale and
still serves 8.1.18. Current Linux builds — 8.5.6 at the time of writing,
including an RPM — are on `debian-10-64bit`. The 0.9.2.0 note that "only 8.1.18
was available" was true of that channel only.

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

### event_detect without pointing devices

`event_detect` re-scans `/sys/class/input` every second from its monitor thread
so hotplugged devices are picked up. On media-2 — a headless media box with no
pointing device at all, so no `/dev/input/by-id/` either — every scan logged
`ERROR: EnumerateEventDevices: No pointing devices identified to monitor.`:
**86,269 lines in the 24 hours before the fix** (measured 2026-10-07). media-3,
which has a mouse, logged none.

0.9.2.1 puts the report on the repo's existing `FailureReportThrottle` ladder:
the first scan of a run at error level, later scans at normal level with
doubling spacing capped at 3600 scans (about hourly), everything in between at
debug level, and one normal-level line when a device appears again.

Verified in an Ubuntu 26.04 container with an empty directory bind-mounted over
`/sys/class/input`, running the 0.9.2.1 `event_detect` for 20 seconds (one scan
per second):

```
lines about pointing devices: 5   (0.9.2.0 prints one per scan: ~20)
  ERROR ... No pointing devices identified to monitor. Scanning continues every second; ...
  INFO  ... Still no pointing devices after 3 scans. Next report in 2 scans.
  INFO  ... Still no pointing devices after 6 scans. Next report in 4 scans.
  INFO  ... Still no pointing devices after 11 scans. Next report in 8 scans.
  INFO  ... Still no pointing devices after 20 scans. Next report in 16 scans.
```

No other error lines. The recovery line (a device appearing after a run of empty
scans) was not exercised in the container, since nothing can be hotplugged into
it; it is the `Reset()` branch of the same ladder that the shared-memory and pipe
paths in `idle_detect` already use. The real confirmation is media-2's journal
after it picks up this release.

### Desktop inhibition type (0.9.2.2)

On jco-monster-ng (Plasma 6.4.2, Wayland) a Thunderbird download held BOINC at
`never` and Folding@home paused from 2026-10-27 18:05 until Thunderbird was
restarted at 14:38 the next day. PolicyAgent listed one inhibition, reason
"Download in progress"; `HasInhibition(1)` was true and `HasInhibition(4)`
false. `CheckKdeInhibition()` queried type 1 under a comment calling it
`ChangeScreenSettings`. PowerDevil's enum (`daemon/powerdevilpolicyagent.h`,
v6.4.2) is `InterruptSession = 1`, `ChangeScreenSettings = 4`: type 1 is the
sleep inhibit, which xdg-desktop-portal-kde raises for portal flag 4 (Suspend),
the flag a browser or mail client holds for a background download. The admin
session reports the same mechanism behind a udisks2 mdraid check pinning DC
paused on 2026-07-05.

Controls on the live session, a persistent D-Bus client holding each lock for
14 s and PolicyAgent probed at 8 s:

| lock | `HasInhibition(1)` | `HasInhibition(4)` |
|---|---|---|
| none | false | false |
| `AddInhibition` type 1 | true | false |
| `AddInhibition` type 4 | true | true |
| portal `Inhibit` flags=4 (Suspend) | true | false |
| portal `Inhibit` flags=8 (Idle) | true | true |
| `org.freedesktop.ScreenSaver.Inhibit` | true | true |

PolicyAgent adds `InterruptSession` implicitly behind every
`ChangeScreenSettings` lock, so querying type 4 keeps every screen-idle lock
and drops the sleep-only ones. A first run of the same controls with 5 s holds
registered nothing at all: PolicyAgent enforces a new inhibition only after a
5 s single-shot timer, which is also why `kde-inhibit --power` had looked like
it registered nothing.

`tests/integration/test_kde_inhibition.sh` takes each lock through
`org.freedesktop.portal.Inhibit` and runs the daemon for 10 s with an isolated
config (no control scripts, no `event_detect`). Against the 0.9.2.1 binary:

```
PASS: baseline: HasInhibition(type=1) not inhibited over 10 ticks, expected not inhibited
FAIL: sleep-only: HasInhibition(type=1) inhibited (10 of 10 ticks) over 10 ticks, expected not inhibited
PASS: screen-idle: HasInhibition(type=1) inhibited (10 of 10 ticks), expected inhibited
```

After the fix:

```
PASS: baseline: HasInhibition(type=4) not inhibited over 10 ticks, expected not inhibited
PASS: sleep-only: HasInhibition(type=4) not inhibited over 10 ticks, expected not inhibited
PASS: screen-idle: HasInhibition(type=4) inhibited (10 of 10 ticks), expected inhibited
```

The GNOME query had the same shape of defect: `IsInhibited(15)` matches
logout, switch-user and suspend inhibitors as well as idle
(`gsm-manager.c`, `inhibitor_has_flag` returns `flags & flag`). It now passes
the idle flag (8) alone, the flag Mozilla's `InhibitGNOME` raises for
playback. Verified against the sources only; see Gaps.

Installed on jco-monster-ng from `development` at `9c5c951` on 2026-10-28
(`sudo ./install.sh --prefix=/usr/local`, then `./user_install.sh`, which
restarted the user service at 15:03:29). The installed binary is the one the
install built (md5 `4f9867f29fd3`); `~/.config/idle_detect.conf`, the helper
scripts and `monitor_ttys=0` in `/etc/event_detect.conf` were untouched. The
integration test run against `/usr/local/bin/idle_detect` passed all three
cases. A sleep-only portal lock was then held for 15 minutes against the live
daemon; the user was active for nearly the whole window, so the daemon never
reached its 300 s idle threshold and the resume-despite-lock transition was not
observed. The daemon's sends to `event_detect` were confirmed working after the
restart by the pipe's write timestamps (one per second while the user was
active, none while idle, and matching the shared-memory last-active time at
15:05:17 and 15:08:29), since the daemon's own "working again" line never
appeared; see the next section.

### Log line flushing (0.9.2.2)

`normal_log` wrote to `std::cout` without a flush. Stdout under systemd is a
socket to the journal, which stdio buffers fully, so a daemon's own lines left
the process only when an `error_log` line (`cerr` is tied to `cout`), a full
4 KiB buffer or exit pushed them out, and the journal stamped the whole batch
with that later moment. On jco-monster-ng all 29 "User became idle/active"
lines from the 2026-10-24 to 10-28 run of the user daemon carry the single
timestamp 2026-10-28 15:03:24, the moment the restart's pipe error flushed
them; the 29 matching "Set BOINC client to run mode" lines from `dc_unpause`
sit at their true times, since each script is its own process. `event_detect`
uses the same logger.

Reproduced with the daemon's stdout redirected to a file, which stdio buffers
the same way, under an isolated config for an 8 s run, reading the file at 5 s
while the daemon was still running:

| logger | log level | in the file at 5 s | at exit |
|---|---|---|---|
| unflushed (0.9.2.1) | normal | 0 bytes | 22 lines, 1708 bytes |
| unflushed (0.9.2.1) | debug | 12288 bytes, exactly three 4 KiB buffers | 232 lines, 19312 bytes |
| flushed (0.9.2.2) | normal | 13 lines, 1256 bytes | 22 lines, 1708 bytes |
| flushed (0.9.2.2) | debug | 176 lines, 14896 bytes | 232 lines, 19312 bytes |

The fix is a `std::flush` after each line in `normal_log`, which `debug_log`
shares; `error_log` already went through the unit-buffered `cerr`. Two unit
tests, `NormalLog.FlushesEveryLine` and
`DebugLog.FlushesWhenEnabledAndWritesNothingWhenDisabled`, point `cout` at a
streambuf that counts sync calls; both fail against the unflushed header. The
cost is one `write` per log line, and at normal level the daemons log a few
lines per state change.

### 0.9.2.2 release verification

Cut from `development` at `ce34327` on 2026-10-28, merged to `master` as
`0e433d1` and tagged. CI was green on the tagged commit for gcc and clang. The
`.deb` workflow built all 15 packages and attached 22 assets in 16 minutes,
with no stall on `jammy / amd64` this time. OBS revision 120 built all 22
`idle_detect` targets (Leap 15.6 and 16.0, Tumbleweed, Arch, Fedora 38 through
Rawhide) in three minutes; every row reported `succeeded`, leaving only the
known `Fedora_38 armv7l _repository broken` row, which names the repository,
not the package. The release was published as latest at 20:56 UTC.

`tests/integration/test_kde_inhibition.sh` was run against the binary built
from the tagged sources once Chrome had released the audio and video locks it
held for most of the afternoon (the test skips while PolicyAgent holds any
inhibition): all three cases pass. The 0.9.2.2 RPM (`0.9.2.2-lp160.1.1`) was
visible from media-2's `home_jamescowens` repository nine seconds after the
16.0 build published. jco upgraded it by hand at 17:18 (`zypper up
idle_detect`, then a restart of the user service): the package's own scriptlet
restarted the system service at 17:18:59 and the user service came back at
17:19:02, `zypper ps -s` found no process using deleted files, and
`read_shmem_timestamps` reports 0.9.2.2. The journal there shows the flush on
a host with no pointing device: the "No pointing devices" ladder lines from
0.9.2.1 are stamped at 1, 3, 6 and 11 seconds after start, as the ladder
specifies, rather than in a batch. Rebooted on 2026-10-30 at 11:53: back on
a new boot id after 122 s, kernel up at 11:55:09, `dc_event_detection`
active at 11:55:23 with its first journal lines 60 ms later, and
`read_shmem_timestamps` reporting 0.9.2.2. The user service was inactive
after the reboot because the box came up at the sddm greeter: the unit is
`WantedBy=graphical-session.target` and media-2 has no autologin, so it
starts at the next desktop login, as it did after the previous boot (booted
2026-10-17, first desktop login 2026-10-24). jco-linux2 through jco-linux7
were unreachable at the time.

Installed on jco-monster-ng from the tagged sources at 17:16 the same day
(`sudo ./install.sh --prefix=/usr/local`, ten seconds including the rebuild,
then `./user_install.sh`). `read_shmem_timestamps` reports 0.9.2.2 - 20261028;
the helper scripts, `~/.config/idle_detect.conf` and `monitor_ttys=0` were
untouched. The system service restarted at 17:16:22 and the user service at
17:16:57, and this time the user daemon started after the registration pipe
existed, so the pipe error seen on the 15:03 restart did not recur. The flush
shows directly in the journal: `event_detect`'s four startup lines are stamped
within 60 ms of the unit's start, and the user daemon's fifteen lines in its
first ten seconds carry their own times, 100 ms apart at startup and then one
to five seconds apart. The daemon it replaced showed the old behavior one last
time on its way out: its four "User became idle/active" lines from the
afternoon all carry 17:16:17, the moment the system service's stop broke the
pipe and an error line flushed them. The shared-memory segment was recreated
at 17:16:22 and is updating once a second.

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
| idle_detect | KDE inhibition query asked for the sleep policy (type 1), so any download pinned DC paused; GNOME query matched logout, switch-user and suspend inhibitors too |
| both daemons | log lines buffered under systemd, so a run's state changes reached the journal as one batch stamped with the time of the next error line or exit |

## Gaps

Stated explicitly rather than implied by omission.

- **Only media-2 and jco-monster-ng run 0.9.2.2.** The rest of the fleet was
  unreachable when the release was cut and still runs 0.9.2.1.
- **GNOME inhibition flag.** The change from `IsInhibited(15)` to `IsInhibited(8)`
  is verified against the gnome-session and Mozilla sources, not against a live
  GNOME session. No host in the fleet runs GNOME.
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
- **Non-x86_64 architectures** were not tested locally.
- **`dc_fah_v8` against a client that reports no version** falls back to the
  shape of the state (a `groups` key means the groups API). That path is covered
  by the unit-style edge cases in the handoff (no version with groups, no version
  without), not by a real client, since every client tested reports one.
