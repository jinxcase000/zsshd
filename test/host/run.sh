#!/bin/sh
# Host test for ssh_server.c's session ending (macOS, FreeBSD or Linux, needs
# libssh 0.10+ headers and library, python3 and a C compiler; see lib.sh's
# libssh_prefix for where libssh is looked for). Builds ssh_server.c
# against test/host/stub.c (no PS5 shell), runs it on 127.0.0.1 on a free port chosen per run just before the server is built
# (so parallel runs do not collide) and checks
# with a libssh client that a finished exec: reports its exit status, gets the
# server's channel close, and the server still holds the connection afterwards
# (OpenSSH prints "Connection to <host> closed by remote host." and exits
# non-zero when the server drops the TCP stream first). A failing exec must
# report its non-zero status.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
P=$(libssh_prefix) || exit 1
W=$(mktemp -d)
trap 'kill "$(cat "$W/pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir "$W/dir"
PORT=$(free_port)
# The compile-time default differs from the port the test uses, so the server
# only listens on PORT by reading it from the config dir.
DEFPORT=$(free_port)
while [ "$DEFPORT" = "$PORT" ]; do DEFPORT=$(free_port); done
echo "$PORT" > "$W/dir/port"
cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=$DEFPORT -DZSSHD_DIR="\"$W/dir\"" -DZSSHD_CONFIG_DIR="\"$W/dir\"" \
  -I"$root/include" -I"$root/include/platform" -I"$P/include" \
  -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" \
  -L"$P/lib" -lssh -lpthread
cc -w -I"$P/include" -o "$W/client" "$here/client.c" -L"$P/lib" -lssh
"$W/client" mkkey "$W/key" > "$W/dir/authorized_keys"
"$W/zsshd-host" > "$W/server.log" 2>&1 &
echo $! > "$W/pid"
sleep 2
# Fail fast: the port was picked before the server could bind it, so another
# process may have taken it in between.
if ! kill -0 "$(cat "$W/pid")" 2>/dev/null \
  || ! port_open "$PORT"; then
  echo "FAIL: server is not listening on 127.0.0.1:$PORT (port clash or server exited); server log:"
  cat "$W/server.log"
  exit 1
fi
fail=0
for i in 1 2 3 4 5; do
  out=$(with_timeout 10 "$W/client" "$PORT" "$W/key" "echo hi" 2>&1) && rc=0 || rc=$?
  [ "$rc" = 0 ] || { echo "exec $i: rc=$rc"; echo "$out"; fail=1; }
done
with_timeout 10 "$W/client" "$PORT" "$W/key" "fail now" > "$W/o" 2>&1 && rc=0 || rc=$?
[ "$rc" = 3 ] || { echo "failing exec: rc=$rc, want 3"; cat "$W/o"; fail=1; }
# A bad port file falls back to the compile-time default.
kill "$(cat "$W/pid")" 2>/dev/null || true
sleep 1
echo 99999 > "$W/dir/port"
"$W/zsshd-host" > "$W/server2.log" 2>&1 &
echo $! > "$W/pid"
sleep 2
if ! port_open "$DEFPORT"; then
  echo "FAIL: bad port file did not fall back to default port $DEFPORT; server log:"
  cat "$W/server2.log"
  fail=1
fi
# A bindaddr that is not loopback: the self-check probe must connect to that
# address, not to loopback (where nothing listens), or the idle listener looks
# dead and is re-created over and over. Needs a non-loopback IPv4 address on
# this host; skipped without one.
kill "$(cat "$W/pid")" 2>/dev/null || true
LANIP=$(python3 -c 'import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.connect(("192.0.2.1", 9))
    a = s.getsockname()[0]
    print("" if a.startswith("127.") else a)
except OSError:
    print("")' 2> /dev/null) || LANIP=
if [ -n "$LANIP" ]; then
  BPORT=$(free_port)
  mkdir "$W/bdir"
  echo "$BPORT" > "$W/bdir/port"
  echo "$LANIP" > "$W/bdir/bindaddr"
  cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=$DEFPORT -DZSSHD_DIR="\"$W/bdir\"" -DZSSHD_CONFIG_DIR="\"$W/bdir\"" \
    -DZSSHD_PROBE_INTERVAL_S=1 -DZSSHD_PROBE_DEADLINE_S=1 \
    -I"$root/include" -I"$root/include/platform" -I"$P/include" \
    -o "$W/zsshd-bind" "$here/main.c" "$here/stub.c" "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" \
    -L"$P/lib" -lssh -lpthread
  "$W/zsshd-bind" > "$W/server3.log" 2>&1 &
  echo $! > "$W/pid"
  sleep 7
  if ! python3 -c 'import socket, sys; socket.create_connection((sys.argv[1], int(sys.argv[2])), 2).close()' "$LANIP" "$BPORT" 2> /dev/null; then
    echo "FAIL: server is not listening on $LANIP:$BPORT; server log:"
    cat "$W/server3.log"
    fail=1
  elif grep -q "is dead" "$W/server3.log"; then
    echo "FAIL: idle listener bound to $LANIP was re-created (probe ignored bindaddr); server log:"
    cat "$W/server3.log"
    fail=1
  fi
else
  echo "SKIP: no non-loopback IPv4 address for the bindaddr probe check"
fi
[ "$fail" = 0 ] && echo "PASS: 5 good execs rc=0 with the connection held, failing exec rc=3"
exit "$fail"
