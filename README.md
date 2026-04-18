# idle_detect

A compact C++17 idle-detection service for Linux, built to solve the
long-standing idle-detection issues in BOINC and other distributed
computing clients. Runs as a two-tier service: a system-level daemon
aggregates raw input activity, and a per-user daemon handles
session-aware idle time with desktop inhibit propagation (KDE, GNOME,
Wayland via `ext_idle_notifier_v1`, X11 via XScreenSaver).

Status: stable for daily use. Please file issues on GitHub for bugs
or unsupported desktop configurations.

## Installation

Two paths: install a pre-built package for your distribution (preferred)
or compile locally.

### A. Packages (preferred)

#### openSUSE, Fedora, Arch Linux

Pick your distribution from the one-click install page:

https://software.opensuse.org//download.html?project=home%3Ajamescowens&package=idle_detect

Coverage: openSUSE Leap 15.6, Leap 16.0, Tumbleweed; Fedora 38–43 and
Rawhide; Arch Linux. Multiple architectures depending on distribution
(x86_64, aarch64, i586, armv7l, ppc64le).

#### Debian, Ubuntu

Download the appropriate `.deb` for your codename and architecture from
the latest GitHub release:

https://github.com/jamescowens/idle_detect/releases

Coverage: Debian bookworm, trixie; Ubuntu jammy (22.04), noble (24.04),
plucky (25.04). Architectures: amd64, arm64, armhf. Example:

```bash
sudo dpkg -i idle-detect_<version>.<codename>_<arch>.deb
sudo apt-get install -f   # resolve any dependencies dpkg didn't pull
```

Signature verification: `SHA256SUMS.txt` is published alongside each
release.

### Post-install activation

After the package is installed, the **system service** (`dc_event_detection`)
starts automatically via the system preset — no action needed.

The **user service** (`dc_idle_detection`) is enabled via the user preset,
but only takes effect for user sessions that start *after* package
install. To light it up in your current session without logging out:

```bash
systemctl --user daemon-reload
systemctl --user enable --now dc_idle_detection.service
```

To verify both services:

```bash
systemctl status dc_event_detection          # system
systemctl --user status dc_idle_detection    # user
```

### Configuration

- System config: `/etc/event_detect.conf` (edit as root, then
  `sudo systemctl restart dc_event_detection`)
- User config: `~/.config/idle_detect.conf` (auto-created on first run,
  then `systemctl --user restart dc_idle_detection` after edits)

By default idle_detect does **not** call the DC-control scripts. It
publishes activity state to `event_detect`, which exports
`/idle_detect_shmem` and `/run/event_detect/last_active_time.dat`. A DC
client that reads either of those can decide for itself when to pause.

If your DC client does not read the shared memory segment directly
(e.g. the current BOINC client), you can have idle_detect drive it by
enabling the bundled scripts:

1. Edit `~/.config/idle_detect.conf` and set
   `execute_dc_control_scripts=1`.
2. `systemctl --user restart dc_idle_detection`.

The default `/usr/bin/dc_pause` and `/usr/bin/dc_unpause` scripts
target BOINC via `boinccmd --set_run_mode never` / `always`. For other
DC clients, edit those scripts, or point `active_command` /
`idle_command` in `idle_detect.conf` at your own.

> **Note for upgraders from 0.9.0.0.** That release shipped with
> `execute_dc_control_scripts=1` enabled by default. Package managers
> preserve `idle_detect.conf` across upgrades (it's marked as a
> configuration file), so existing installs keep their prior setting —
> your behavior does not change on upgrade. If you want the new
> default-off behavior, edit `~/.config/idle_detect.conf` manually.

## Architecture

Two processes, coordinated via a named pipe and POSIX shared memory:

**`event_detect`** (system daemon, `event_detect:event_detect` user):
- Monitors `/dev/input/event*` pointing devices (catches physical mouse
  and keyboard activity)
- Monitors pts/tty atime (catches terminal and SSH activity)
- Receives messages from per-user `idle_detect` instances via named pipe
- Exports aggregated `last_active_time` as `/idle_detect_shmem` (two
  `int64_t`: update timestamp and last-active timestamp, Unix epoch
  seconds UTC) and optionally `/run/event_detect/last_active_time.dat`

**`idle_detect`** (per-user daemon, one per session):
- Detects session type and uses the most appropriate idle source:
  - **KDE X11 / Plasma 5**: `org.kde.ksmserver` `GetSessionIdleTime`
    (includes inhibit propagation)
  - **KDE Wayland / Plasma 6**: `ext_idle_notifier_v1` + D-Bus
    `PowerManagement` inhibit check
  - **GNOME**: `org.gnome.SessionManager.IsInhibited` + Mutter idle time
  - **Other Wayland**: `ext_idle_notifier_v1`
  - **X11 fallback**: XScreenSaver (`libXss`)
  - **TTY**: falls back to `event_detect`
- Reports activity to `event_detect` via named pipe as
  `timestamp:USER_ACTIVE` messages

A DC client polls `/idle_detect_shmem` once a second to decide whether
to run. This resolves ≈99% of the idle-detection issues that were seen
with BOINC directly.

## Local compilation

If no package is available for your distribution, build from source.

### Dependencies (development packages)

Debian / Ubuntu:
```
libevdev-dev libxss-dev libdbus-1-dev libglib2.0-dev libx11-dev
libwayland-dev libwayland-bin wayland-protocols libgtest-dev
cmake ninja-build pkg-config
```

Fedora / openSUSE:
```
libevdev-devel libXScrnSaver-devel dbus-devel glib2-devel
wayland-devel wayland-protocols-devel gtest-devel
cmake ninja-build pkgconfig
```

A C++17-capable compiler is required (GCC 9+ or Clang 10+).

### Build and install

```bash
git clone https://github.com/jamescowens/idle_detect.git
cd idle_detect
sudo ./install.sh
```

Supported arguments:
- `--prefix=<dir>` (default `/usr/local`)
- `--cxx-compiler=<path>` — force a specific compiler. CMake runs a
  feature-check compile; some compilers advertise C++17 without fully
  implementing it (GCC 7 is a known case).

`install.sh` handles the system side: building and installing
`event_detect`, the system service file, and the system config. Then as
your regular user (not sudo):

```bash
./user_install.sh
```

This installs the user service. Enable and start it:

```bash
systemctl --user daemon-reload
systemctl --user enable --now dc_idle_detection.service
```

## Branching and releases

- `development` — active development
- `master` — releases and pre-release tags

Packages published to OBS and attached to GitHub releases are built
from `master` tags.

## License

MIT. See `LICENSE.md`.
