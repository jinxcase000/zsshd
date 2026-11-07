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

# build_host_shell ROOT LIBSSH OUT PORT DIR [CFLAGS...]: build the daemon at
# OUT with the real shell (shell.c; console-only calls stubbed by
# test/host/shell_host.c), listening on 127.0.0.1:PORT with DIR as its data
# and config folder. ROOT is the checkout, LIBSSH the libssh prefix
# (libssh_prefix). CFLAGS are added, e.g. -DZSSHD_TEST_UTIMENSAT_FAIL.
build_host_shell() {
  bh_root=$1 bh_p=$2 bh_out=$3 bh_port=$4 bh_dir=$5
  shift 5
  # Helper sources shell.c links against on branches that have them.
  bh_extra=
  for bh_h in cmdline diskfmt timeutil; do
    if [ -f "$bh_root/src/app/$bh_h.c" ]; then
      bh_extra="$bh_extra $bh_root/src/app/$bh_h.c"
    fi
  done
  # The console has CLOCK_UPTIME; macOS calls it CLOCK_UPTIME_RAW and Linux
  # has CLOCK_BOOTTIME. FreeBSD already defines it.
  case $(uname -s) in
    Darwin) bh_clk=-DCLOCK_UPTIME=CLOCK_UPTIME_RAW ;;
    Linux) bh_clk=-DCLOCK_UPTIME=CLOCK_BOOTTIME ;;
    *) bh_clk=-DZSSHD_HOST_NO_CLOCK_SHIM ;;
  esac
  # bh_extra is deliberately unquoted: a word list of source paths.
  cc -Wall -Werror -DPLATFORM_PS5 "$bh_clk" "$@" -DZSSHD_PORT="$bh_port" \
    -DZSSHD_DIR="\"$bh_dir\"" -DZSSHD_CONFIG_DIR="\"$bh_dir\"" \
    -I"$bh_root/include" -I"$bh_root/include/platform" -I"$bh_p/include" \
    -o "$bh_out" "$bh_root/test/host/main.c" "$bh_root/test/host/shell_host.c" \
    "$bh_root/src/app/ssh_server.c" "$bh_root/src/app/zsshd_config.c" "$bh_root/src/app/firstrun.c" \
    "$bh_root/src/app/shell.c" $bh_extra -L"$bh_p/lib" -lssh -lpthread
}
