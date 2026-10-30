#!/bin/sh
# Host test for ssh_server.c's session ending (macOS or Linux, needs libssh 0.10+
# headers and library; set LIBSSH_PREFIX if not Homebrew's). Builds ssh_server.c
# against test/host/stub.c (no PS5 shell), runs it on 127.0.0.1:2299 and checks
# with a libssh client that a finished exec: reports its exit status, gets the
# server's channel close, and the server still holds the connection afterwards
# (OpenSSH prints "Connection to <host> closed by remote host." and exits
# non-zero when the server drops the TCP stream first). A failing exec must
# report its non-zero status.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
P=${LIBSSH_PREFIX:-$(brew --prefix libssh)}
W=$(mktemp -d)
trap 'kill "$(cat "$W/pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir "$W/dir"
cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=2299 -DZSSHD_DIR="\"$W/dir\"" \
  -I"$root/include" -I"$root/include/platform" -I"$P/include" \
  -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" "$root/src/app/ssh_server.c" \
  -L"$P/lib" -lssh -lpthread
cc -w -I"$P/include" -o "$W/client" "$here/client.c" -L"$P/lib" -lssh
"$W/client" mkkey "$W/key" > "$W/dir/authorized_keys"
"$W/zsshd-host" > "$W/server.log" 2>&1 &
echo $! > "$W/pid"
sleep 2
fail=0
for i in 1 2 3 4 5; do
  out=$(perl -e 'alarm 10; exec @ARGV' "$W/client" 2299 "$W/key" "echo hi" 2>&1) && rc=0 || rc=$?
  [ "$rc" = 0 ] || { echo "exec $i: rc=$rc"; echo "$out"; fail=1; }
done
perl -e 'alarm 10; exec @ARGV' "$W/client" 2299 "$W/key" "fail now" > "$W/o" 2>&1 && rc=0 || rc=$?
[ "$rc" = 3 ] || { echo "failing exec: rc=$rc, want 3"; cat "$W/o"; fail=1; }
[ "$fail" = 0 ] && echo "PASS: 5 good execs rc=0 with the connection held, failing exec rc=3"
exit "$fail"
