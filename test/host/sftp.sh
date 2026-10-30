#!/bin/sh
# SFTP host test (macOS or Linux): builds the vendored libssh against the
# host's OpenSSL, builds
# ssh_server.c against it with test/host/stub.c, runs it on 127.0.0.1:2296 and
# drives it with test/host/sftp.py (paramiko, installed into a throwaway venv).
# Needs cmake, a C compiler, OpenSSL headers (set OPENSSL_ROOT_DIR if not
# Homebrew's openssl@3), python3 and network access for pip.
# Also checks that the server process is still running after every session.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
OSSL=${OPENSSL_ROOT_DIR:-$(brew --prefix openssl@3 2>/dev/null || echo /usr)}
W=$(mktemp -d)
trap 'kill "$(cat "$W/pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$W"' EXIT
link="$root/external/libssh-0.12.2/compile_commands.json"
old=$(readlink "$link" 2>/dev/null || true)
cmake -S "$root/external/libssh-0.12.2" -B "$W/libssh" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DBUILD_SHARED_LIBS=OFF \
  -DOPENSSL_ROOT_DIR="$OSSL" -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF \
  -DWITH_ZLIB=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF \
  -DWITH_SYMBOL_VERSIONING=OFF > "$W/build.log" 2>&1
# libssh's configure re-points compile_commands.json in its source tree at
# $W, which is gone after the test; put the original link back.
if [ -n "$old" ]; then ln -sfn "$old" "$link"; elif [ -L "$link" ]; then rm -f "$link"; fi
cmake --build "$W/libssh" -j4 >> "$W/build.log" 2>&1
cmake --install "$W/libssh" --prefix "$W/prefix" >> "$W/build.log" 2>&1
mkdir "$W/dir" "$W/work"
cc -Wall -Werror -DPLATFORM_PS5 -DZSSHD_PORT=2296 -DZSSHD_DIR="\"$W/dir\"" \
  -I"$root/include" -I"$root/include/platform" \
  -I"$W/prefix/include" -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" \
  "$root/src/app/ssh_server.c" "$W/prefix/lib/libssh.a" \
  -L"$OSSL/lib" -lcrypto -lpthread
python3 -m venv "$W/venv"
"$W/venv/bin/pip" install -q paramiko
ssh-keygen -q -t ed25519 -N '' -f "$W/key"
cp "$W/key.pub" "$W/dir/authorized_keys"
head -c 1048576 /dev/urandom > "$W/work/src.bin"
"$W/zsshd-host" > "$W/server.log" 2>&1 &
echo $! > "$W/pid"
sleep 2
rc=0
perl -e 'alarm 300; exec @ARGV' "$W/venv/bin/python" -I "$here/sftp.py" 2296 \
  "$W/key" "$W/work" || rc=$?
sleep 1
if ! kill -0 "$(cat "$W/pid")" 2>/dev/null; then
  echo "FAIL server process exited"; rc=1
fi
[ "$rc" = 0 ] && echo "PASS: 3 SFTP sessions verified, client closing first, server alive"
exit "$rc"
