#!/bin/bash
# Version matrix for dc_fah_v8 against real Folding@home v8 clients, run safely as an ordinary user.
#
#   ./test_dc_fah_v8_versions.sh [path/to/dc_fah_v8] [workdir]
#
# Downloads 8.1.18 (debian-stable channel) and 8.3.18 / 8.4.9 / 8.5.6 (debian-10 channel), starts each
# in its own directory on a free loopback port with --cpus=0 and GPUs hidden (a bare 8.5.6 defaults to
# cpus=31 and would fold anonymously), then for each client: verified pause/unpause with the ACTUAL
# flag read independently; CONTROL 1 = with the verb suppressed, apply_paused must NOT confirm.
# Exit status 0 only if every check passed. Clients are stopped by PID after their exe is checked.
set -u
TOOL=$(readlink -f "${1:-./dc_fah_v8}")
W=$(readlink -f "${2:-$(mktemp -d /tmp/dc_fah_v8_matrix.XXXXXX)}"); mkdir -p "$W"
U=https://download.foldingathome.org/releases/public/fah-client
declare -A SRC=([8.1.18]=debian-stable-64bit [8.3.18]=debian-10-64bit [8.4.9]=debian-10-64bit [8.5.6]=debian-10-64bit)
declare -A PORT PID
fail=0; port=17396
cleanup() {
  for v in "${!PID[@]}"; do
    p=${PID[$v]}
    if [ "$(readlink /proc/$p/exe 2>/dev/null)" = "$W/fah-client_$v-64bit-release/fah-client" ]; then kill "$p"; fi
  done
}
trap cleanup EXIT

for v in 8.1.18 8.3.18 8.4.9 8.5.6; do
  t=fah-client_$v-64bit-release.tar.bz2
  [ -f "$W/$t" ] || curl -sSf --max-time 300 -o "$W/$t" "$U/${SRC[$v]}/release/$t" || { echo "download $v FAILED"; exit 2; }
  tar xjf "$W/$t" -C "$W"
  while ss -ltnH "( sport = :$port )" | grep -q .; do port=$((port + 1)); done
  mkdir -p "$W/run-$v"
  ( cd "$W/run-$v" && exec env CUDA_VISIBLE_DEVICES=-1 OCL_ICD_VENDORS=/nonexistent \
      "$W/fah-client_$v-64bit-release/fah-client" --cpus=0 --http-addresses=127.0.0.1:$port \
      --log=log.txt --log-to-screen=false --open-web-control=false >out.txt 2>&1 ) &
  PID[$v]=$!; PORT[$v]=$port; port=$((port + 1))
done
sleep 8

for v in 8.1.18 8.3.18 8.4.9 8.5.6; do
  echo "== $v: $(python3 "$TOOL" -a 127.0.0.1:${PORT[$v]} api)"
  python3 - "$TOOL" "${PORT[$v]}" <<'PY' || fail=1
import sys, json
from importlib.machinery import SourceFileLoader
d = SourceFileLoader("d", sys.argv[1]).load_module(); port = int(sys.argv[2]); H = "127.0.0.1"
def actual():                                   # independent of dc_fah_v8's own flag reader
    s = d.read_state(H, port, 5.0)
    if "groups" in s: return {g: v["config"].get("paused") for g, v in s["groups"].items()}
    return {"config.paused": s["config"].get("paused")}
bad = 0
for verb in ("pause", "pause", "unpause", "unpause", "pause", "unpause"):
    want = verb == "pause"
    ok = d.apply_paused(H, port, 5.0, want); a = actual(); flags = set(a.values())
    good = ok and flags == {want}; bad += not good
    print(f"   {'ok ' if good else 'BAD'} {verb:8} confirmed={ok} actual={a}")
real = d.send_command; d.send_command = lambda *x, **k: None
st = d.read_state(H, port, 5.0); cur = d.paused_flag(st, d.api_generation(st))
ok = d.apply_paused(H, port, 5.0, not cur); bad += ok
print(f"   {'ok ' if not ok else 'BAD'} CONTROL verb suppressed -> confirmed={ok} (must be False)")
sys.exit(1 if bad else 0)
PY
done
echo; [ $fail = 0 ] && echo "ALL PASS ($W)" || echo "FAILURES ($W)"; exit $fail
