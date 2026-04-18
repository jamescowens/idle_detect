# Configuration Reference

idle_detect uses two separate configuration files, one per daemon:

| File | Scope | Read by | Typical install path |
|---|---|---|---|
| `event_detect.conf` | System | `event_detect` (system daemon) | `/etc/event_detect.conf` |
| `idle_detect.conf`  | User   | `idle_detect` (per-user daemon) | `~/.config/idle_detect.conf` |

The system config ships as part of the package. The user config is
auto-generated on first run from a template at
`/usr/share/idle_detect/idle_detect.conf.default`, then treated as
user-owned thereafter.

## Syntax

Both files use the same simple format:

```
# Comments start with '#' at the beginning of a line.
key=value
string_key="value with spaces"
```

- One key per line.
- Whitespace around `=` is tolerated.
- Values can be quoted with `"..."` when they contain spaces.
- Boolean values accept `0` / `1` or `false` / `true` (case-insensitive).
- Unknown keys are ignored silently.

## Reloading

Neither daemon watches its config file. Restart the service after editing:

```bash
sudo systemctl restart dc_event_detection       # system
systemctl --user restart dc_idle_detection      # user
```

## Upgrade behavior

Both config files are marked as configuration files in all supported
package formats (RPM `%config(noreplace)`, Debian `conffiles`, Arch
`backup=(...)`). On package upgrade, your existing values are
preserved — the shipped defaults only apply to fresh installs. If a
future release changes a default and you want the new behavior, edit
the file manually.

---

## `event_detect.conf` — system daemon settings

### `debug`

- **Type:** boolean
- **Default (code):** `true`
- **Shipped value:** `0` (disabled)
- **Controls:** verbose debug logging to stderr/journal.

Emits a large volume of log output — roughly one block per second
describing each monitor thread's state. Useful when diagnosing device
enumeration or idle-source selection; noisy otherwise.

> **Gotcha:** if you delete this line entirely from your config, the
> code default (`true`) kicks in and debug logging is enabled. Keep
> `debug=0` explicitly if you want it off.

### `event_count_files_path`

- **Type:** string (directory path)
- **Default:** `/run/event_detect`
- **Controls:** the runtime directory where `event_detect` creates the
  named pipe (`event_registration_pipe`) and optionally the
  last-active-time file.

Must be writable by the `event_detect` user. On systemd systems the
default path is managed by the service via `RuntimeDirectory=`, so you
generally don't need to change this.

### `write_last_active_time_to_file`

- **Type:** boolean
- **Default (code):** `0` (disabled)
- **Shipped value:** `1` (enabled)
- **Controls:** whether `event_detect` periodically writes the aggregated
  last-active timestamp to a plain-text file.

When enabled, the file lives at
`<event_count_files_path>/<last_active_time_cpp_filename>`
(default `/run/event_detect/last_active_time.dat`) and contains a single
Unix epoch seconds value. Intended for consumers that prefer a file
read over a shared-memory attach. The shared-memory segment is
published independently — this setting only affects the file.

### `last_active_time_cpp_filename`

- **Type:** string (basename only, no path separators)
- **Default:** `last_active_time.dat`
- **Controls:** basename of the file written when
  `write_last_active_time_to_file=1`. The file is placed inside
  `event_count_files_path`. No reason to change this unless a consumer
  insists on a specific filename.

### `monitor_ttys`

- **Type:** boolean
- **Default:** `true`
- **Controls:** whether `event_detect` polls `/dev/pts/*` and
  `/dev/tty*` atime to detect terminal and SSH activity.

Useful on headless systems and for catching SSH activity on desktops.
Two reasons you might want to disable it:

- You want DC to keep running while you're using terminals or SSH —
  treat only GUI input as activity, so coding / sysadmin work in a
  terminal doesn't pause the DC client.
- You want to minimize the syscalls the daemon makes.

Has no side effects on pure GUI-only workloads.

### `monitor_idle_detect_events`

- **Type:** boolean
- **Default (code):** `false`
- **Shipped value:** `1` (enabled)
- **Controls:** whether `event_detect` listens on its named pipe for
  activity messages sent by per-user `idle_detect` daemons.

Must be enabled for the per-user daemons to contribute to the
aggregated idle time — without it, `event_detect` only sees raw input
devices and tty atime. Disabling isolates the system daemon from the
user-side entirely.

### `use_shared_memory`

- **Type:** boolean
- **Default:** `true`
- **Controls:** whether `event_detect` creates and updates the
  `/idle_detect_shmem` POSIX shared memory segment.

The shared memory segment is the fastest way for other processes (e.g.
BOINC) to read the current `last_active_time`. Disable only if you
have a reason to avoid the `/dev/shm` entry — you'll lose the primary
consumer interface; the file-based fallback
(`write_last_active_time_to_file`) remains if enabled.

---

## `idle_detect.conf` — user daemon settings

### `debug`

- **Type:** boolean
- **Default (code):** `true`
- **Shipped value:** `0` (disabled)
- **Controls:** verbose debug logging from `idle_detect` to journal.

Same gotcha as `event_detect.conf`: if you remove the line entirely,
the code default enables debug. Keep `debug=0` explicitly to suppress.

### `event_count_files_path`

- **Type:** string (directory path)
- **Default:** `/run/event_detect`
- **Controls:** where `idle_detect` looks for the named pipe created
  by `event_detect`. Must match `event_detect.conf`'s value for the
  two daemons to communicate.

### `use_event_detect`

- **Type:** boolean
- **Default:** `true`
- **Controls:** whether `idle_detect` reads the `event_detect`
  shared-memory segment as part of its idle-time calculation.

When enabled, `idle_detect` takes the *minimum* of its session-local
idle sources (D-Bus / Wayland / XScreenSaver) and the `event_detect`
aggregated value. This catches activity that session-local sources
miss — notably SSH and TTY activity on the same machine. Disable only
if you want `idle_detect` to ignore cross-session input entirely.

### `update_event_detect`

- **Type:** boolean
- **Default:** `true`
- **Controls:** whether `idle_detect` sends `timestamp:USER_ACTIVE`
  messages to `event_detect` via its named pipe.

Required for `event_detect` to aggregate activity from the GUI session
into the shared-memory value. If you disable this, `event_detect` will
not know about D-Bus / Wayland / X11 idle state — it will only see raw
input devices and ttys.

Interacts with `event_detect.conf`'s `monitor_idle_detect_events` —
both must be enabled for the pipe to carry meaningful traffic.

### `execute_dc_control_scripts`

- **Type:** boolean
- **Default:** `false`
- **Shipped value:** `0` (disabled)
- **Controls:** whether `idle_detect` directly runs `active_command` and
  `idle_command` on user-active / user-idle state transitions.

Default is **off** — the assumption is that a DC client polls
`/idle_detect_shmem` itself and makes its own run/pause decision. Turn
this on if your DC client does not read the shared-memory segment and
you want `idle_detect` to drive it via the configured scripts. See
`active_command` and `idle_command` below.

### `last_active_time_cpp_filename`

- **Type:** string (basename only)
- **Default:** `last_active_time.dat`
- **Controls:** basename for the file-based fallback, matches the
  `event_detect.conf` entry of the same name.

Used only if `idle_detect` falls back to reading the file (rather than
the shared-memory segment). No reason to change unless aligning with a
non-default `event_detect.conf`.

### `shmem_name`

- **Type:** string
- **Default:** `/idle_detect_shmem`
- **Controls:** POSIX shared-memory segment name that `idle_detect`
  opens to read the aggregated `last_active_time`.

Must match `event_detect`'s output segment. The leading `/` is required
by POSIX `shm_open` semantics.

### `inactivity_time_trigger`

- **Type:** integer (seconds)
- **Default:** `300` (five minutes)
- **Controls:** idle threshold, in seconds. When `last_active_time` is
  more than this many seconds old, the state is considered `IDLE`;
  otherwise `ACTIVE`.

State transitions fire `active_command` / `idle_command` when
`execute_dc_control_scripts=1`.

### `active_command`

- **Type:** string (command line)
- **Default (code):** empty string
- **Shipped value:** `/usr/bin/dc_pause`
- **Controls:** command executed when `idle_detect` transitions from
  idle to active (user returns to the machine).

Fires only when `execute_dc_control_scripts=1`. The default script
calls `boinccmd --set_run_mode never` to pause BOINC. Edit the script
at `/usr/bin/dc_pause`, or point this setting at a different
executable, to control other DC clients.

### `idle_command`

- **Type:** string (command line)
- **Default (code):** empty string
- **Shipped value:** `/usr/bin/dc_unpause`
- **Controls:** command executed when `idle_detect` transitions from
  active to idle (user has been inactive for `inactivity_time_trigger`
  seconds).

Fires only when `execute_dc_control_scripts=1`. The default script
calls `boinccmd --set_run_mode always` to resume BOINC.

---

## Common configurations

The snippets below show **only the settings that differ from the
shipped defaults** for each scenario — they are *edits to make*, not
complete config files. Keep all the other default lines in your
`idle_detect.conf` or `event_detect.conf` as shipped; do not
copy-paste these as replacements for the whole file.

### BOINC workstation, run-mode controlled by idle_detect

Required for **BOINC versions older than 8.2.10**, which do not read
`/idle_detect_shmem` and therefore need idle_detect to drive them
via `boinccmd`. In `~/.config/idle_detect.conf`, change:

```ini
execute_dc_control_scripts=1
```

`active_command` and `idle_command` already default (in the shipped
config) to `dc_pause` / `dc_unpause`, which call
`boinccmd --set_run_mode`. Adjust `inactivity_time_trigger` from the
default `300` if you want a different idle threshold.

The shipped scripts probe a handful of common BOINC data directories
(`/var/lib/boinc`, `/var/lib/boinc-client`, `/var/snap/boinc/common`,
`$HOME/BOINC`, `$HOME/.BOINC`, and the Flatpak path), so most
installations work without any configuration. If your BOINC data
directory is somewhere else, set `BOINC_DATA_DIR` in the environment;
see *Customizing dc_pause / dc_unpause* below.

### Customizing dc_pause / dc_unpause

The shipped `/usr/bin/dc_pause` and `/usr/bin/dc_unpause` scripts are
**replaced on every package upgrade and every `install.sh` run**. Do
not edit them in place — your edits will be lost.

To customize permanently, copy the scripts to a location outside the
idle_detect install prefix and point the config at the copies.
`~/.local/bin/` is the recommended destination because it is never
touched by any install path (package or local build), and it matches
the execution context of the user service that runs these scripts.

```bash
mkdir -p ~/.local/bin
cp /usr/bin/dc_pause /usr/bin/dc_unpause ~/.local/bin/
chmod +x ~/.local/bin/dc_pause ~/.local/bin/dc_unpause
# edit ~/.local/bin/dc_pause and ~/.local/bin/dc_unpause as needed
```

Then edit `~/.config/idle_detect.conf` — use the fully-expanded path
(`~/` does **not** expand in the config file):

```ini
active_command=/home/USER/.local/bin/dc_pause
idle_command=/home/USER/.local/bin/dc_unpause
```

Finally:

```bash
systemctl --user restart dc_idle_detection
```

`/usr/local/bin/` can also work as a system-wide location, but only
for package installs — a local build via `install.sh` defaults to
`--prefix=/usr/local` and will overwrite anything you place there.

**Environment-variable override (no file edits).** If the only thing
you need to change is the BOINC data directory, set `BOINC_DATA_DIR`
on the user service instead of copying the scripts:

```bash
mkdir -p ~/.config/systemd/user/dc_idle_detection.service.d
cat > ~/.config/systemd/user/dc_idle_detection.service.d/override.conf <<'EOF'
[Service]
Environment=BOINC_DATA_DIR=/path/to/your/boinc/data
EOF
systemctl --user daemon-reload
systemctl --user restart dc_idle_detection
```

### DC client reads shmem directly (idle_detect is passive)

No changes needed — this is the shipped default. Your client opens
`/idle_detect_shmem` (two `int64_t`: update_time, last_active_time,
both Unix epoch seconds UTC) and makes its own decision. **BOINC
8.2.10 and later** read the segment directly, so this is the correct
configuration for current BOINC installations.

### DC continues during terminal / SSH work

In `/etc/event_detect.conf`, change:

```ini
monitor_ttys=0
```

This stops `event_detect` from counting pts/tty atime as activity, so
using a terminal or SSH session doesn't pause the DC client. Only GUI
input and (if enabled) D-Bus / Wayland / X11 idle state drive the
idle calculation.

### Headless server / SSH-only workloads

No user service needed — only `event_detect` runs. TTY monitoring
(`monitor_ttys=1`, the shipped default) catches SSH session activity.
DC clients read the shared-memory segment as usual. No config changes
needed beyond the defaults.
