# Shared helpers for the host test scripts; source it, do not run it.
# POSIX sh, for macOS (Homebrew) and FreeBSD (pkg). Callers use set -eu.

# Print a libssh install prefix (with include/ and lib/): $LIBSSH_PREFIX,
# else Homebrew's, else pkg-config's, else /usr/local or /usr when it holds
# the libssh headers (FreeBSD's pkg installs under /usr/local). Fails with a
# message when none is found.
libssh_prefix() {
  if [ -n "${LIBSSH_PREFIX:-}" ]; then
    echo "$LIBSSH_PREFIX"
    return 0
  fi
  if command -v brew > /dev/null 2>&1 && brew --prefix libssh > /dev/null 2>&1; then
    brew --prefix libssh
    return 0
  fi
  if command -v pkg-config > /dev/null 2>&1 && pkg-config --exists libssh; then
    pkg-config --variable=prefix libssh
    return 0
  fi
  for d in /usr/local /usr; do
    if [ -f "$d/include/libssh/libssh.h" ]; then
      echo "$d"
      return 0
    fi
  done
  echo "FAIL: libssh not found; install it (brew install libssh / pkg install libssh) or set LIBSSH_PREFIX" >&2
  return 1
}

# Print an OpenSSL prefix: $OPENSSL_ROOT_DIR, else Homebrew's openssl@3,
# else /usr (FreeBSD's base OpenSSL, most Linux distributions).
openssl_prefix() {
  if [ -n "${OPENSSL_ROOT_DIR:-}" ]; then
    echo "$OPENSSL_ROOT_DIR"
  elif command -v brew > /dev/null 2>&1 && brew --prefix openssl@3 > /dev/null 2>&1; then
    brew --prefix openssl@3
  else
    echo /usr
  fi
}

# with_timeout SECONDS CMD...: run CMD, killed after SECONDS. Uses timeout(1)
# where it exists (FreeBSD base, GNU coreutils), else perl's alarm (macOS).
with_timeout() {
  wt_s=$1
  shift
  if command -v timeout > /dev/null 2>&1; then
    timeout "$wt_s" "$@"
  else
    perl -e 'alarm shift; exec @ARGV' "$wt_s" "$@"
  fi
}

# Print a free TCP port on 127.0.0.1 (a race remains until the server binds).
free_port() {
  python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# port_open PORT: succeed when 127.0.0.1:PORT accepts a connection.
port_open() {
  python3 -c 'import socket, sys; socket.create_connection(("127.0.0.1", int(sys.argv[1])), 2).close()' "$1" 2> /dev/null
}
