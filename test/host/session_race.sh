#!/bin/sh
# Host test for a session-setup race in ssh_server.c (macOS, Linux or
# FreeBSD, needs libssh 0.10+ and python3; lib.sh's libssh_prefix finds libssh,
# or set LIBSSH_PREFIX). A libssh client sends SERVICE_REQUEST right after its
# NEWKEYS. When both reach the server in one read, libssh handles the request
# inside ssh_handle_key_exchange; if the server callbacks were only set after the key
# exchange, the request was queued and never answered, and the client hung at
# auth (about 1 in 6000 direct loopback connections). Here every exec goes
# through coalesce_proxy.py, which holds client bytes for 50 ms and forwards
# them together, so the two packets always arrive in one read. Each exec must
# finish with rc=0 within 10 s.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
P=$(libssh_prefix) || exit 1
W=$(mktemp -d)
trap 'kill $(cat "$W/pid" "$W/proxypid" 2>/dev/null) 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir "$W/dir"
PORT=$(free_port)
cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=$PORT -DZSSHD_DIR="\"$W/dir\"" -DZSSHD_CONFIG_DIR="\"$W/dir\"" \
  -I"$root/include" -I"$root/include/platform" -I"$P/include" \
  -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" \
  -L"$P/lib" -lssh -lpthread
cc -w -I"$P/include" -o "$W/client" "$here/client.c" -L"$P/lib" -lssh
"$W/client" mkkey "$W/key" > "$W/dir/authorized_keys"
"$W/zsshd-host" > "$W/server.log" 2>&1 &
echo $! > "$W/pid"
PPORT=$(free_port)
python3 -I "$here/coalesce_proxy.py" "$PPORT" "$PORT" 50 > "$W/proxy.log" 2>&1 &
echo $! > "$W/proxypid"
sleep 2
if ! kill -0 "$(cat "$W/pid")" 2> /dev/null || ! port_open "$PORT"; then
  echo "FAIL: server is not listening on 127.0.0.1:$PORT (port clash or server exited); server log:"
  cat "$W/server.log"
  exit 1
fi
if ! kill -0 "$(cat "$W/proxypid")" 2> /dev/null || ! port_open "$PPORT"; then
  echo "FAIL: proxy is not listening on 127.0.0.1:$PPORT; proxy log:"
  cat "$W/proxy.log"
  exit 1
fi
fail=0
for i in 1 2 3 4 5; do
  out=$(perl -e 'alarm 10; exec @ARGV' "$W/client" "$PPORT" "$W/key" "echo hi" 2>&1) && rc=0 || rc=$?
  [ "$rc" = 0 ] || { echo "exec $i through the coalescing proxy: rc=$rc (142 = timed out)"; echo "$out"; fail=1; }
done
[ "$fail" = 0 ] && echo "PASS: 5 execs with NEWKEYS and SERVICE_REQUEST in one read, all rc=0"
exit "$fail"
