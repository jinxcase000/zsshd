#!/bin/sh
# Host test for the `touch` builtin (shell.c cmd_touch) on an existing file:
# the mtime moves. Three builds of the real shell (lib.sh build_host_shell):
#   - plain: utimensat works;
#   - ZSSHD_TEST_UTIMENSAT_FAIL: utimensat fails (as on the PS5), so the
#     futimes/utimes fallback must still move the mtime;
#   - ZSSHD_TEST_UTIMES_FAIL as well: every call fails, so touch must exit
#     non-zero and print an error rather than silently succeed.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
P=$(libssh_prefix) || exit 1
W=$(mktemp -d)
trap 'kill "$(cat "$W/pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir "$W/dir"
cc -w -I"$P/include" -o "$W/client" "$here/client.c" -L"$P/lib" -lssh
"$W/client" mkkey "$W/key" > "$W/dir/authorized_keys"
fail=0

mtime() { python3 -c 'import os, sys; print(int(os.stat(sys.argv[1]).st_mtime))' "$1"; }

# run_case NAME WANT_MOVED WANT_RC CFLAGS...
run_case() {
  name=$1 want_moved=$2 want_rc=$3
  shift 3
  PORT=$(free_port)
  build_host_shell "$root" "$P" "$W/z" "$PORT" "$W/dir" "$@"
  "$W/z" > "$W/log" 2>&1 &
  echo $! > "$W/pid"
  i=0
  until port_open "$PORT"; do
    i=$((i + 1))
    [ $i -lt 50 ] || { echo "FAIL: $name: server did not start"; cat "$W/log"; exit 1; }
    sleep 0.1
  done
  f=$W/f
  : > "$f"
  touch -t 200001010000 "$f"
  before=$(mtime "$f")
  with_timeout 30 "$W/client" raw "$PORT" "$W/key" "touch $f" > "$W/out" 2>&1 && rc=0 || rc=$?
  after=$(mtime "$f")
  kill "$(cat "$W/pid")" 2> /dev/null || true
  wait "$(cat "$W/pid")" 2> /dev/null || true
  moved=0
  [ "$after" -gt "$before" ] && moved=1
  if [ "$moved" = "$want_moved" ] && [ "$rc" = "$want_rc" ]; then
    echo "ok: $name (moved=$moved rc=$rc)"
  else
    echo "FAIL: $name: moved=$moved (want $want_moved) rc=$rc (want $want_rc)"
    cat "$W/out"
    fail=1
  fi
  if [ "$want_rc" != 0 ] && ! grep -q 'touch: ' "$W/out"; then
    echo "FAIL: $name: no error message"
    fail=1
  fi
}

run_case "touch moves mtime of an existing file" 1 0
run_case "utimensat fails: fallback moves mtime" 1 0 -DZSSHD_TEST_UTIMENSAT_FAIL
run_case "every time call fails: error reported" 0 1 -DZSSHD_TEST_UTIMENSAT_FAIL -DZSSHD_TEST_UTIMES_FAIL
[ "$fail" = 0 ] && echo "PASS: touch moves mtime, falls back, reports failure"
exit "$fail"
