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
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
OSSL=$(openssl_prefix)
W=$(mktemp -d)
trap 'kill "$(cat "$W/pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$W"' EXIT
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
# -include link_fault.h routes the SFTP server's link() calls through
# test/host/link_fault.c, so a link() that reports success without creating
# anything can be simulated (ZSSHD_TEST_LINK_NOOP_DIR, set below).
cmake -S "$root/external/libssh-0.12.2" -B "$W/libssh" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-include $here/link_fault.h" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DBUILD_SHARED_LIBS=OFF \
  -DOPENSSL_ROOT_DIR="$OSSL" -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF \
  -DWITH_ZLIB=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF \
  -DWITH_SYMBOL_VERSIONING=OFF > "$W/build.log" 2>&1
# libssh's configure links compile_commands.json into its source tree; it
# would dangle once $W is gone. Restore the link if git tracks it, else drop it.
link="$root/external/libssh-0.12.2/compile_commands.json"
if git -C "$root" ls-files --error-unmatch "$link" >/dev/null 2>&1; then
  git -C "$root" checkout -- "$link"
elif [ -L "$link" ]; then
  rm -f "$link"
fi
cmake --build "$W/libssh" -j4 >> "$W/build.log" 2>&1
cmake --install "$W/libssh" --prefix "$W/prefix" >> "$W/build.log" 2>&1
mkdir "$W/dir" "$W/work"
PORT=$(free_port)
cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=$PORT -DZSSHD_DIR="\"$W/dir\"" -DZSSHD_CONFIG_DIR="\"$W/dir\"" \
  -I"$root/include" -I"$root/include/platform" \
  -I"$W/prefix/include" -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" \
  "$here/link_fault.c" \
  "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" "$W/prefix/lib/libssh.a" \
  -L"$OSSL/lib" -lcrypto -lpthread
ssh-keygen -q -t ed25519 -N '' -f "$W/key"
cp "$W/key.pub" "$W/dir/authorized_keys"
head -c 104857600 /dev/urandom > "$W/work/src.bin"
# Inside work/nolink/ the server's link() returns 0 and creates nothing.
ZSSHD_TEST_LINK_NOOP_DIR="$W/work/nolink/" "$W/zsshd-host" > "$W/server.log" 2>&1 &
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
rc=0
with_timeout 300 "$PY" -I "$here/sftp.py" "$PORT" \
  "$W/key" "$W/work" || rc=$?
sleep 1
if ! kill -0 "$(cat "$W/pid")" 2>/dev/null; then
  echo "FAIL server process exited"; rc=1
fi
[ "$rc" = 0 ] && echo "PASS: 3 SFTP sessions verified, oversized read answered short, server alive"
exit "$rc"
