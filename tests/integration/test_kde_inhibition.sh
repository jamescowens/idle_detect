#!/bin/bash
# Live check of idle_detect's KDE inhibition query against PowerDevil, run safely as an ordinary user.
#
#   ./test_kde_inhibition.sh [path/to/idle_detect]
#
# Needs a running Plasma session (org.kde.Solid.PowerManagement on the session bus) and
# xdg-desktop-portal-kde. Each case takes an inhibition through org.freedesktop.portal.Inhibit the way
# real applications do, waits past PowerDevil's 5 s enforcement delay, runs idle_detect for 10 s with a
# throwaway config that executes no dc control scripts and talks to no event_detect, and reads the
# per-tick "HasInhibition(type=N) returned:" debug line:
#   baseline     no lock                                            -> must NOT report inhibited
#   sleep-only   portal flag 4 (Suspend), e.g. a download running   -> must NOT report inhibited
#   screen-idle  portal flag 8 (Idle), e.g. a video playing         -> MUST report inhibited
# 0.9.2.1 and earlier queried PolicyAgent type 1 (InterruptSession, the sleep inhibit) and fail the
# sleep-only case. Exit status 0 only if every case passed.
#
# Each 10 s daemon run shares $XDG_RUNTIME_DIR/idle_detect_control_pipe with any idle_detect already running
# for this user, so do not send force_state commands while the test runs.
set -u
BIN=$(readlink -f "${1:-./idle_detect}")
[ -x "$BIN" ] || { echo "usage: $0 [path/to/idle_detect] (not executable: $BIN)"; exit 2; }
PA=(org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent)
busctl --user call "${PA[@]}" HasInhibition u 4 >/dev/null 2>&1 || { echo "SKIP: no PowerDevil PolicyAgent on the session bus (not a Plasma session?)"; exit 2; }
[ "$(busctl --user call "${PA[@]}" ListInhibitions | awk '{print $2}')" = 0 ] || { echo "SKIP: PolicyAgent already holds an inhibition; release it first"; busctl --user call "${PA[@]}" ListInhibitions; exit 2; }

W=$(mktemp -d /tmp/idle_detect_kde_inhibit.XXXXXX)
HOLDER_PID=
cleanup() { [ -n "$HOLDER_PID" ] && kill "$HOLDER_PID" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

cat > "$W/idle_detect.conf" <<'CONF'
debug=1
use_event_detect=0
use_xprintidle=0
update_event_detect=0
execute_dc_control_scripts=0
inactivity_time_trigger="20"
active_command=
idle_command=
CONF

cat > "$W/hold.py" <<'PY'
import sys, time
import gi; gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
flags, secs = int(sys.argv[1]), float(sys.argv[2])
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Inhibit",
              "Inhibit", GLib.Variant("(sua{sv})", ("", flags, {"reason": GLib.Variant("s", "idle_detect test, portal flags %d" % flags)})),
              GLib.VariantType("(o)"), Gio.DBusCallFlags.NONE, 2000, None)
time.sleep(secs)   # the inhibition lives as long as this connection does
PY

fail=0
run_case() {  # name, portal flags (0 = none), expected "inhibited" | "not inhibited"
  local name=$1 flags=$2 expect=$3
  if [ "$flags" != 0 ]; then
    python3 -I "$W/hold.py" "$flags" 30 & HOLDER_PID=$!
    sleep 7   # PowerDevil enforces a new inhibition only after a 5 s grace period
    local held; held=$(busctl --user call "${PA[@]}" ListInhibitions | awk '{print $2}')
    [ "$held" = 1 ] || { echo "ERROR: $name: PolicyAgent lists $held inhibitions while the portal lock is held"; fail=1; }
  fi
  timeout 10 "$BIN" "$W/idle_detect.conf" > "$W/$name.log" 2>&1
  local total yes; total=$(grep -c "HasInhibition(type=[0-9]*) returned:" "$W/$name.log"); yes=$(grep -c "HasInhibition(type=[0-9]*) returned: true" "$W/$name.log")
  local got="not inhibited"; [ "$yes" -gt 0 ] && got="inhibited ($yes of $total ticks)"
  local type; type=$(grep -o "HasInhibition(type=[0-9]*)" "$W/$name.log" | head -1)
  if [ "$total" -lt 3 ]; then echo "ERROR: $name: only $total HasInhibition queries in 10 s (no KDE shell source?)"; fail=1
  elif [ "$yes" -gt 0 ] && [ "$expect" = "inhibited" ] && [ "$yes" -eq "$total" ]; then echo "PASS: $name: $type $got, expected $expect"
  elif [ "$yes" -eq 0 ] && [ "$expect" = "not inhibited" ]; then echo "PASS: $name: $type $got over $total ticks, expected $expect"
  else echo "FAIL: $name: $type $got over $total ticks, expected $expect"; fail=1; fi
  if [ -n "$HOLDER_PID" ]; then kill "$HOLDER_PID" 2>/dev/null; wait "$HOLDER_PID" 2>/dev/null; HOLDER_PID=; sleep 2; fi
}

run_case baseline    0 "not inhibited"
run_case sleep-only  4 "not inhibited"
run_case screen-idle 8 "inhibited"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit $fail
