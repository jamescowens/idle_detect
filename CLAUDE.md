# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

idle_detect is a C++17 Linux idle detection service for BOINC and other distributed computing programs. It uses a two-tier architecture: a system-level daemon (`event_detect`) and per-user daemons (`idle_detect`) that coordinate via named pipes and POSIX shared memory.

## Build Commands

```bash
# Configure (Debug)
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug

# Configure (Release, with Ninja)
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release -G Ninja

# Build
cmake --build build/cmake -j$(nproc)

# Full install (as root)
sudo ./install.sh [--prefix=/usr/local] [--cxx-compiler=/path/to/g++]

# User-level install (as regular user, after system install)
./user_install.sh
```

### Dependencies (Ubuntu)

```
libevdev-dev libxss-dev libdbus-1-dev libglib2.0-dev libwayland-dev wayland-protocols
```

## Architecture

### Two-Process Model

**event_detect** (system daemon, runs as `event_detect:event_detect` user):
- Monitors `/dev/input/event*` devices for mouse/input activity
- Monitors pts/tty access times for terminal/SSH activity
- Receives updates from user-level `idle_detect` instances via named pipe
- Exports aggregated `last_active_time` via POSIX shared memory (`/idle_detect_shmem`) and/or file (`/run/event_detect/last_active_time.dat`)

**idle_detect** (user daemon, one per user session):
- Detects session type and uses appropriate idle detection:
  - KDE: `org.kde.ksmserver.GetSessionIdleTime` (includes inhibit support)
  - GNOME: `org.gnome.SessionManager.IsInhibited` + mutter idle time
  - Wayland (other): `ext_idle_notifier_v1` protocol
  - X11 (fallback): XScreenSaver (`libXss`)
  - TTY: falls back to event_detect
- Sends `timestamp:USER_ACTIVE` messages to event_detect via named pipe
- Optionally runs dc_pause/dc_unpause control scripts

### Source Files

- `event_detect.h/cpp` — System-level daemon (namespace `EventDetect`)
- `idle_detect.h/cpp` — User-level daemon (namespace `IdleDetect`)
- `util.h/cpp` — Shared utilities: `Config` base class, `EventMessage`, string/time helpers, logging
- `release.h` — Version macros (version is sourced from here by CMakeLists.txt)
- `read_shmem_timestamps.cpp` — Standalone utility to read the shared memory segment
- `tinyformat.h` — Header-only formatting library

### Key Classes

- `EventDetect::Monitor` — Aggregates input events, writes shared memory
- `EventDetect::InputEventRecorders` — Thread pool, one `EventRecorder` per input device
- `EventDetect::TtyMonitor` — Polls tty/pts access times
- `EventDetect::IdleDetectMonitor` — Reads named pipe from user-level instances
- `EventDetect::SharedMemoryTimestampExporter` — RAII wrapper for POSIX shm (`int64_t[2]`)
- `IdleDetect::IdleDetectControlMonitor` — Manages override states (NORMAL, FORCED_ACTIVE, FORCED_IDLE)
- `IdleDetect::WaylandIdleMonitor` — `ext_idle_notifier_v1` Wayland protocol handler
- `Config` / `EventDetectConfig` / `IdleDetectConfig` — Singleton config with `config_variant` typed values

### Threading Model

Both daemons are multi-threaded. Synchronization uses `std::atomic` for flags/timestamps, `std::mutex` for complex state, and `std::condition_variable` for event-driven threads. Classes are generally non-copyable singletons.

### Naming Conventions

- `m_` prefix: member variables
- `g_` prefix: globals
- `mtx_` prefix: mutexes
- `cv_` prefix: condition variables
- `m_interrupt_*`: atomic shutdown flags

## Systemd Services

- `dc_event_detection.service` — system service for event_detect
- `dc_idle_detection.service` — user service for idle_detect
- Service files are generated from `.in` templates by CMake

## Branching

- `development` — active development branch
- `master` — releases and pre-release tags (0.x)

## CI

GitHub Actions (`.github/workflows/cmake-multi-platform.yml`): builds on Ubuntu with both GCC and Clang using Ninja, runs CTest.
