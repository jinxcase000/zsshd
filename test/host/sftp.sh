#!/bin/sh
# SFTP host test (macOS, FreeBSD or Linux): builds the vendored libssh (with the
# zsshd changes to its SFTP server) against the host's OpenSSL, builds
# ssh_server.c against it with test/host/stub.c, runs it on 127.0.0.1 on a free port
# chosen per run (so parallel runs do not collide) and
# drives it with test/host/sftp.py (paramiko).
# Needs cmake, a C compiler, OpenSSL headers (set OPENSSL_ROOT_DIR if not
# Homebrew's openssl@3) and python3 with paramiko (preinstalled, or from pip:
# network, or PIP_FIND_LINKS/PIP_NO_INDEX for a local wheel dir).
# Also checks that the server process is still running after every session.
# Then builds a second libssh + server with -DSFTP_NO_HARDLINKS (what the PS5
# build gets from __PROSPERO__) and runs sftp.py's nohardlinks checks on it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
OSSL=$(openssl_prefix)
W=$(mktemp -d)
trap 'for p in "$W/pid" "$W/pid2"; do kill "$(cat "$p" 2>/dev/null)" 2>/dev/null || true; done; wait 2>/dev/null; rm -rf "$W"' EXIT
# paramiko: use a preinstalled one if python3 can import it; otherwise install it
# into a throwaway venv (pip honours PIP_FIND_LINKS / PIP_NO_INDEX for a local
# wheel dir or cache, so no network is needed then). Skip when neither works.
if python3 -I -c 'import paramiko' 2>/dev/null; then
  PY=python3
else
  python3 -m venv "$W/venv"
  "$W/venv/bin/pip" install -q paramiko > "$W/pip.log" 2>&1 || {
    echo "SKIP: paramiko is not installed and pip could not install it (offline?)."
    echo "      Install paramiko for python3, or set PIP_FIND_LINKS=<wheel dir> and PIP_NO_INDEX=1."
    exit 0
  }
  PY=$W/venv/bin/python
fi
# build_server NAME EXTRA_CFLAGS PORT: libssh into $W/libssh-NAME, installed
# to $W/prefix-NAME, and the host server $W/zsshd-NAME listening on PORT.
# -include link_fault.h routes the SFTP server's link() calls through
# test/host/link_fault.c, so a link() that reports success without creating
# anything can be simulated (ZSSHD_TEST_LINK_NOOP_DIR, set below).
build_server() {
  bs_name=$1 bs_cflags=$2 bs_port=$3
  cmake -S "$root/external/libssh-0.12.2" -B "$W/libssh-$bs_name" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-include $here/link_fault.h $bs_cflags" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DOPENSSL_ROOT_DIR="$OSSL" -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF \
    -DWITH_ZLIB=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF \
    -DWITH_SYMBOL_VERSIONING=OFF > "$W/build-$bs_name.log" 2>&1
  # libssh's configure links compile_commands.json into its source tree; it
  # would dangle once $W is gone. Restore the link if git tracks it, else drop it.
  link="$root/external/libssh-0.12.2/compile_commands.json"
  if git -C "$root" ls-files --error-unmatch "$link" >/dev/null 2>&1; then
    git -C "$root" checkout -- "$link"
  elif [ -L "$link" ]; then
    rm -f "$link"
  fi
  cmake --build "$W/libssh-$bs_name" -j4 >> "$W/build-$bs_name.log" 2>&1
  cmake --install "$W/libssh-$bs_name" --prefix "$W/prefix-$bs_name" \
    >> "$W/build-$bs_name.log" 2>&1
  cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=$bs_port -DZSSHD_DIR="\"$W/dir\"" \
    -DZSSHD_CONFIG_DIR="\"$W/dir\"" -I"$root/include" -I"$root/include/platform" \
    -I"$W/prefix-$bs_name/include" -o "$W/zsshd-$bs_name" "$here/main.c" "$here/stub.c" \
    "$here/link_fault.c" \
    "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" \
    "$W/prefix-$bs_name/lib/libssh.a" \
    -L"$OSSL/lib" -lcrypto -lpthread
}
# start_server NAME PORT PIDFILE: run $W/zsshd-NAME (inside work/nolink/ its
# link() returns 0 and creates nothing). Fail fast: the port was picked before
# the server could bind it, so another process may have taken it in between.
start_server() {
  ZSSHD_TEST_LINK_NOOP_DIR="$W/work/nolink/" "$W/zsshd-$1" > "$W/server-$1.log" 2>&1 &
  echo $! > "$3"
  sleep 2
  if ! kill -0 "$(cat "$3")" 2>/dev/null \
    || ! port_open "$2"; then
    echo "FAIL: server $1 is not listening on 127.0.0.1:$2 (port clash or server exited); server log:"
    cat "$W/server-$1.log"
    exit 1
  fi
}
mkdir "$W/dir" "$W/work" "$W/work2"
PORT=$(free_port)
build_server links "" "$PORT"
ssh-keygen -q -t ed25519 -N '' -f "$W/key"
cp "$W/key.pub" "$W/dir/authorized_keys"
head -c 104857600 /dev/urandom > "$W/work/src.bin"
head -c 4096 /dev/urandom > "$W/work2/src.bin"
start_server links "$PORT" "$W/pid"
rc=0
with_timeout 300 "$PY" -I "$here/sftp.py" "$PORT" \
  "$W/key" "$W/work" || rc=$?
# The console check tool, where the checkout has one, against this build.
csc="$root/tools/console-sftp-check.py"
if [ -f "$csc" ]; then
  echo "== console-sftp-check.py suite"
  with_timeout 120 "$PY" -I "$csc" suite 127.0.0.1 "$PORT" \
    "$W/key" "$W/work/console-check" 1 || rc=1
fi
sleep 1
if ! kill -0 "$(cat "$W/pid")" 2>/dev/null; then
  echo "FAIL server process exited"; rc=1
fi
kill "$(cat "$W/pid")" 2>/dev/null || true
wait "$(cat "$W/pid")" 2>/dev/null || true
PORT2=$(free_port)
build_server nohardlinks -DSFTP_NO_HARDLINKS "$PORT2"
start_server nohardlinks "$PORT2" "$W/pid2"
rc2=0
with_timeout 120 "$PY" -I "$here/sftp.py" "$PORT2" \
  "$W/key" "$W/work2" nohardlinks || rc2=$?
if [ -f "$csc" ]; then
  echo "== console-sftp-check.py suite (SFTP_NO_HARDLINKS)"
  with_timeout 120 "$PY" -I "$csc" suite 127.0.0.1 "$PORT2" \
    "$W/key" "$W/work2/console-check" 1 || rc2=1
fi
sleep 1
if ! kill -0 "$(cat "$W/pid2")" 2>/dev/null; then
  echo "FAIL server process (SFTP_NO_HARDLINKS) exited"; rc2=1
fi
[ "$rc" = 0 ] && echo "PASS: 3 SFTP sessions verified, oversized read answered short, server alive"
[ "$rc2" = 0 ] && echo "PASS: SFTP_NO_HARDLINKS build: rename without link(), hardlink not offered"
[ "$rc" = 0 ] && [ "$rc2" = 0 ]
