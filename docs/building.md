# Building from Source

Detailed build and install guide for people who want to compile
idle_detect themselves. If you just want to install it, see the
[Packages section of the README](../README.md#a-packages-preferred)
first — pre-built packages exist for openSUSE, Fedora, Arch Linux,
Debian, and Ubuntu.

## Prerequisites

idle_detect is C++17 and needs a reasonably modern toolchain. Minimum
compiler: GCC 9 or Clang 10.

### Dependencies by distribution

All package names below are for the **-dev / -devel** packages needed
to build. Runtime-only packages will usually be pulled in as
dependencies of the devel packages.

#### Debian / Ubuntu

```bash
sudo apt-get install \
    build-essential cmake ninja-build pkg-config \
    libevdev-dev libxss-dev libdbus-1-dev libglib2.0-dev libx11-dev \
    libwayland-dev libwayland-bin wayland-protocols \
    libgtest-dev
```

- `libwayland-bin` provides `wayland-scanner`, needed to compile the
  vendored `ext-idle-notify-v1.xml`.
- `libgtest-dev` is preferred over the CMake `FetchContent` fallback —
  avoids a network fetch during configuration and shows consistent
  test output across distros.

#### Fedora / openSUSE

```bash
# Fedora
sudo dnf install \
    gcc-c++ cmake ninja-build pkgconf \
    libevdev-devel libXScrnSaver-devel dbus-devel glib2-devel \
    wayland-devel wayland-protocols-devel \
    gtest-devel

# openSUSE
sudo zypper install \
    gcc-c++ cmake ninja pkgconf \
    libevdev-devel libXScrnSaver-devel dbus-1-devel glib2-devel \
    wayland-devel wayland-protocols-devel \
    gtest
```

Package name differences between the two (`ninja` vs `ninja-build`,
`gtest` vs `gtest-devel`) are accounted for in the OBS spec file; as a
local builder you just need whichever your distro calls them.

#### Arch Linux

```bash
sudo pacman -S --needed \
    base-devel cmake ninja pkgconf \
    libevdev libxss dbus glib2 libx11 \
    wayland wayland-protocols \
    gtest
```

The base `wayland` package already provides `wayland-scanner` on Arch.

## Quick build (common case)

```bash
git clone https://github.com/jamescowens/idle_detect.git
cd idle_detect
sudo ./install.sh
./user_install.sh             # as your regular user, not root
```

`install.sh` does a clean CMake configure + build + install into
`/usr/local` and starts the system service. `user_install.sh` handles
the per-user systemd unit.

That's it for the common case. The rest of this document is for people
who want to do something non-default.

## `install.sh` flags

```
./install.sh [--prefix=<dir>] [--cxx-compiler=<path>]
```

- `--prefix=<dir>` — install root. Default `/usr/local`. Common
  alternatives: `/usr` (distro-style), `/opt/idle_detect` (sidecar
  install).
- `--cxx-compiler=<path>` — force a specific compiler. CMake runs a
  feature-check compile to verify C++17 support; some compilers
  advertise C++17 without fully implementing it (GCC 7 is a known
  case). Use this flag if CMake's autodetection picks a compiler that
  fails that check.

Behaviorally, `install.sh`:

1. Creates the `event_detect` system user and group if missing.
2. Configures the build in `build/cmake/` (or `build/<prefix>/` if
   `--prefix` is non-default).
3. Runs the build (as the invoking user via `sudo -u $SUDO_USER`, so
   build artifacts stay user-owned — important if you later want to
   re-run CMake without `sudo`).
4. Installs everything via `cmake --install`.
5. Reloads systemd and starts `dc_event_detection.service`.

**Prerequisite:** you must invoke `install.sh` with `sudo`, not as a
direct root login. It reads `$SUDO_USER` to chown the build directory
back; direct root gives no meaningful value there.

## `user_install.sh`

Installs and enables the per-user systemd unit. Run as your regular
user (not `sudo`):

```bash
./user_install.sh
```

Behaviorally:

1. Copies the user systemd unit file into `~/.config/systemd/user/`.
2. Runs `systemctl --user daemon-reload`.
3. Enables and starts `dc_idle_detection.service`.
4. Triggers first-run config copy from
   `/usr/share/idle_detect/idle_detect.conf.default` to
   `~/.config/idle_detect.conf`.

The split between system and user install is intentional — the user
unit can't be enabled by root for an arbitrary user in a way that
survives re-login cleanly, so per-user setup is a separate step.

## Manual CMake build (no `install.sh`)

For contributors, or for non-standard workflows:

```bash
# Configure
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local

# Build
cmake --build build -j$(nproc)

# Test (see next section)
ctest --test-dir build --output-on-failure

# Install (usually needs sudo for /usr/local or /usr)
sudo cmake --install build
```

### Relevant CMake options

| Option | Default | What it does |
|---|---|---|
| `CMAKE_BUILD_TYPE` | *(empty)* | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel`. Affects optimization and debug symbol flags. |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Install root. Debian packaging uses `/usr`. |
| `CMAKE_INSTALL_SYSCONFDIR` | `etc` (relative) | Where `/etc`-style config files go. Debian packaging passes `/etc` (absolute); local builds use the relative default, which resolves to `/etc` either way via the CMakeLists logic. |
| `CMAKE_CXX_COMPILER` | autodetected | Override if the default is too old. Can also be set via `CXX=/path/to/g++-13 cmake -S . -B build`. |
| `BUILD_TESTING` | `ON` | Build the GoogleTest unit test suite. Set `-DBUILD_TESTING=OFF` for a minimal build. |

### Generator choice: Ninja vs Makefiles

Ninja is faster for incremental rebuilds and is the default for this
project's shipping packages. Makefiles work fine if you prefer them:

```bash
cmake -S . -B build          # omit -G Ninja to use Unix Makefiles
cmake --build build
```

## Running tests

The project ships 101 unit tests across three files (`util`,
`EventMessage`, `Config`). The test binary is deliberately built
against `util.cpp` only — no D-Bus, Wayland, X11, or libevdev — so it
runs in any CI environment.

```bash
ctest --test-dir build --output-on-failure
```

Common invocations:

```bash
# Single test binary, verbose
./build/idle_detect_tests --gtest_verbose

# Filter to a specific test
./build/idle_detect_tests --gtest_filter='ConfigTest.*'

# List all tests
./build/idle_detect_tests --gtest_list_tests
```

Core-logic coverage (Monitor aggregation, IPC state machine, seqlock
semantics) is scheduled for 1.0 — see `docs/ROADMAP_1.0.md`.

### GoogleTest source

By default CMake uses `find_package(GTest)` to locate the system
`libgtest-dev` / `gtest-devel` / `gtest` package. If not found, it
falls back to `FetchContent` which clones GoogleTest 1.15.2 from
GitHub at configure time. For offline or reproducible builds, make
sure your system GoogleTest package is installed — then no network
fetch happens.

## Developer workflow

### Debug build with sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
      -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan
./build-asan/idle_detect_tests
```

Threads + sanitizers: if you want TSan, use a separate build dir with
`-fsanitize=thread` (incompatible with ASan in the same build).

### Incremental rebuilds during development

The project installs into systemd paths, so iterating against a
running service is a little clunky. Two approaches:

1. **Install each iteration.** `cmake --build build && sudo cmake
   --install build && sudo systemctl restart dc_event_detection` —
   slow but safe.
2. **Run the binary directly.** `sudo ./build/event_detect
   /etc/event_detect.conf` — bypasses the service for one-off
   testing. For `idle_detect` the path is
   `./build/idle_detect ~/.config/idle_detect.conf` as your user.
   The daemons can be interrupted with `Ctrl+C` cleanly.

### Viewing logs

```bash
journalctl -u dc_event_detection -f              # system daemon
journalctl --user -u dc_idle_detection -f        # user daemon
```

Enable `debug=1` in the relevant config for verbose output during
iteration.

## Building a package locally

Occasionally useful for testing packaging changes before pushing to
OBS or triggering GHA.

### Debian / Ubuntu `.deb`

Builds inside a container matching your target codename. The
`debian/` directory in the repo is what `dpkg-buildpackage` expects.

```bash
docker run --rm -v "$PWD:/src" -w /src debian:bookworm bash -c '
    apt-get update && apt-get install -y --no-install-recommends \
        devscripts equivs fakeroot build-essential dpkg-dev
    mk-build-deps --install --remove --tool "apt-get -y --no-install-recommends" debian/control
    dpkg-buildpackage -b -us -uc -rfakeroot
'
ls ../*.deb
```

This is exactly what the GitHub Actions workflow does per codename.

### Fedora / openSUSE `.rpm`

```bash
# Inside a Fedora or openSUSE host / container
rpmbuild -bb idle_detect.spec         # uses the spec from your OBS checkout
```

You'll need to supply the source tarball at
`~/rpmbuild/SOURCES/idle_detect-<version>.tar.gz` — easiest is to
generate it with `git archive --format=tar.gz --prefix=idle_detect-<version>/ HEAD`.

### Arch `PKGBUILD`

```bash
makepkg -si                           # from the directory with PKGBUILD
```

The OBS checkout has a working `PKGBUILD`; the main repo does not
ship one directly.

## Troubleshooting

### CMake says C++17 not supported

Your autodetected compiler is too old or broken. Override:

```bash
./install.sh --cxx-compiler=/usr/bin/g++-13
# or manually:
CXX=/usr/bin/g++-13 cmake -S . -B build
```

### `wayland-scanner` not found

Install the package that provides it (`libwayland-bin` on Debian/Ubuntu,
`wayland-devel` on Fedora/openSUSE, `wayland` on Arch).

### `find_package(GTest)` fails, `FetchContent` also fails

If you're offline and don't have GoogleTest installed, disable the
tests entirely:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
```

### `last_active_time.dat` not being written

`write_last_active_time_to_file=1` must be set in `event_detect.conf`,
and the `event_detect` user must have write access to
`event_count_files_path`. If the systemd service has
`RuntimeDirectory=event_detect` set (default), the path is managed
correctly. For non-systemd / manual runs, create the directory with
`event_detect:event_detect` ownership.

### User service fails to start on first login

Race between session startup and D-Bus registration. See
`dc_idle_detection.service.in` — the service uses `ExecStartPre=/bin/sleep 5`
to mitigate this. If the delay isn't enough, increase it and reload:

```bash
systemctl --user daemon-reload
systemctl --user restart dc_idle_detection
```
