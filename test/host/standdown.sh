#!/bin/sh
# Stand-down host test (macOS, FreeBSD or Linux; needs cmake, a C compiler with
# AddressSanitizer and python3). A replacement payload stands the running
# daemon down (instance_control.c -> ssh_server_request_stop). Session threads
# still running then (one the accept loop took just before the stop, others
# part way through a key exchange) went on using libssh's crypto state while
# it was freed (ssh_finalize, or libssh's destructor at process exit: the
# mbedtls CTR-DRBG). Their next random draw ran AES with a zeroed key schedule
# and read off the end of it (SIGSEGV in mbedtls_aesni_crypt_ecb under
# ssh_handle_key_exchange on the console).
#
# Builds the vendored mbedtls and libssh (the console's crypto backend) and
# ssh_server.c with -fsanitize=address, then RUNS times (default 20): starts
# the server, keeps key exchanges in flight against it (kex_flood.py), sends
# SIGUSR1 (test/host/main.c: the same stop call) at a varying moment, and
# requires a clean exit: status 0, the "Shutting down" line, and no
# AddressSanitizer report or crash. The host process exits in microseconds,
# so main.c frees libssh as exit does and then lives on EXIT_DELAY_MS
# (default 3000) ms, the window in which the console crashed.
#
# mbedtls is built with MBEDTLS_THREADING_C as on the console. A zeroed
# FreeBSD pthread mutex is a static initializer, so on the console the draw
# after the free locks the zeroed DRBG mutex and goes on into AES; macOS
# refuses a zeroed mutex (EINVAL) and would hide the bad draw, so the server
# links test/host/libthr_mutex.c, which gives mbedtls the console's behaviour.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/lib.sh"
RUNS=${RUNS:-20}
EXIT_DELAY_MS=${EXIT_DELAY_MS:-3000}
W=$(mktemp -d)
# KEEP=<dir>: copy the server logs there at the end.
trap '[ -n "${KEEP:-}" ] && cp "$W"/server-*.log "$KEEP"/ 2>/dev/null; kill "$(cat "$W/pid" 2>/dev/null)" "$(cat "$W/floodpid" 2>/dev/null)" 2>/dev/null || true; wait 2>/dev/null; rm -rf "$W"' EXIT
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
SAN="-fsanitize=address -fno-omit-frame-pointer -g -O1"
# The console build turns mbedtls threading on (.github/workflows/release.yml); every
# compile that sees the mbedtls headers must agree on it.
printf '#define MBEDTLS_THREADING_C\n#define MBEDTLS_THREADING_PTHREAD\n' > "$W/threading.h"
CRYPTO="$SAN -include $W/threading.h"
# step CMD...: run a build step into build.log; on failure show its tail.
step() {
  "$@" >> "$W/build.log" 2>&1 || {
    echo "FAIL: build step failed: $*"
    tail -30 "$W/build.log"
    exit 1
  }
}
if [ ! -f "$root/external/mbedtls/framework/CMakeLists.txt" ]; then
  git -C "$root" submodule update --init --recursive external/mbedtls
fi
step cmake -S "$root/external/mbedtls" -B "$W/mbedtls" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="$CRYPTO" -DCMAKE_INSTALL_PREFIX="$W/prefix" \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF
step cmake --build "$W/mbedtls" -j"$JOBS"
step cmake --install "$W/mbedtls"
step cmake -S "$root/external/libssh-0.12.2" -B "$W/libssh" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="$CRYPTO" -DCMAKE_INSTALL_PREFIX="$W/prefix" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DBUILD_SHARED_LIBS=OFF -DWITH_MBEDTLS=ON \
  -DMBEDTLS_INCLUDE_DIR="$W/prefix/include" \
  -DMBEDTLS_SSL_LIBRARY="$W/prefix/lib/libmbedtls.a" \
  -DMBEDTLS_CRYPTO_LIBRARY="$W/prefix/lib/libmbedcrypto.a" \
  -DMBEDTLS_X509_LIBRARY="$W/prefix/lib/libmbedx509.a" \
  -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF -DWITH_ZLIB=OFF \
  -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DWITH_SYMBOL_VERSIONING=OFF
# libssh's configure links compile_commands.json into its source tree; it
# would dangle once $W is gone. Restore the link if git tracks it, else drop it.
link="$root/external/libssh-0.12.2/compile_commands.json"
if git -C "$root" ls-files --error-unmatch "$link" >/dev/null 2>&1; then
  git -C "$root" checkout -- "$link"
elif [ -L "$link" ]; then
  rm -f "$link"
fi
step cmake --build "$W/libssh" -j"$JOBS"
step cmake --install "$W/libssh"
mkdir "$W/dir"
PORT=$(free_port)
cc -Wall -Werror $CRYPTO -DPLATFORM_PS5 -DZSSHD_PORT=$PORT -DZSSHD_DIR="\"$W/dir\"" \
  -DZSSHD_CONFIG_DIR="\"$W/dir\"" -I"$root/include" -I"$root/include/platform" \
  -I"$W/prefix/include" -o "$W/zsshd-host" "$here/main.c" "$here/stub.c" \
  "$here/libthr_mutex.c" "$root/src/app/ssh_server.c" "$root/src/app/zsshd_config.c" \
  "$W/prefix/lib/libssh.a" "$W/prefix/lib/libmbedcrypto.a" -lpthread
# halt_on_error (the default) ends the server at the first report.
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
i=1
inflight=0
while [ "$i" -le "$RUNS" ]; do
  log=$W/server-$i.log
  ZSSHD_TEST_EXIT_DELAY_MS=$EXIT_DELAY_MS "$W/zsshd-host" > "$log" 2>&1 &
  echo $! > "$W/pid"
  pid=$(cat "$W/pid")
  n=0
  until port_open "$PORT"; do
    n=$((n + 1))
    if [ "$n" -ge 300 ] || ! kill -0 "$pid" 2> /dev/null; then
      echo "FAIL: run $i: server is not listening on 127.0.0.1:$PORT; server log:"
      cat "$log"
      exit 1
    fi
    sleep 0.1
  done
  python3 -I "$here/kex_flood.py" "$PORT" 8 3 > "$W/flood.log" 2>&1 &
  echo $! > "$W/floodpid"
  # Stand down 0.5 to 1.4 s into the flood, a different moment each run.
  sleep "$(python3 -c 'import random; print(round(random.uniform(0.5, 1.4), 2))')"
  kill -USR1 "$pid"
  n=0
  while kill -0 "$pid" 2> /dev/null && [ "$n" -lt 150 ]; do
    sleep 0.1
    n=$((n + 1))
  done
  if kill -0 "$pid" 2> /dev/null; then
    kill -9 "$pid" 2> /dev/null || true
    wait "$pid" 2> /dev/null || true
    echo "FAIL: run $i: server still running 15 s after the stand-down; log tail:"
    tail -20 "$log"
    exit 1
  fi
  wait "$pid" && rc=0 || rc=$?
  wait "$(cat "$W/floodpid")" 2> /dev/null || true
  rm -f "$W/pid" "$W/floodpid"
  if [ "$rc" != 0 ] || grep -q 'AddressSanitizer' "$log" \
    || ! grep -q '^\[zsshd\] Shutting down' "$log"; then
    echo "FAIL: run $i: stand-down exit status $rc (EXIT_DELAY_MS=$EXIT_DELAY_MS); first report lines:"
    grep -n -A30 'ERROR: AddressSanitizer' "$log" | head -40 || true
    echo "-- log tail:"
    tail -8 "$log"
    exit 1
  fi
  # A session thread that logs after the accept loop returned outlived it.
  late=$(awk '/^\[zsshd\] Shutting down/ { p = 1; next } p' "$log")
  if [ -n "$late" ]; then
    echo "FAIL: run $i: a session thread was still running after the server returned:"
    echo "$late" | head -5
    exit 1
  fi
  ended=$(sed -n 's/^\[zsshd\] Stand-down: ending \([0-9]*\) session.*/\1/p' "$log")
  inflight=$((inflight + ${ended:-0}))
  i=$((i + 1))
done
echo "PASS: $RUNS stand-downs during a key-exchange flood ($inflight in-flight sessions ended), all exited 0 with no AddressSanitizer report"
