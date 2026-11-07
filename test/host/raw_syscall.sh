#!/bin/sh
# Host test for src/platform/ps5_syscall.S, the PS5 build's wrappers around the
# SDK libc's raw system call stubs: each must return -1 and set errno when the
# kernel reports failure. The PS5 kernel uses the FreeBSD amd64 convention
# (carry flag set, errno in %rax), which x86_64 macOS and FreeBSD share, so the
# wrappers are built and run here; elsewhere the test is skipped. Needs only a
# C compiler. What it cannot show: that the console kernel sets the carry flag
# the same way (only a console run of a failing call can).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
if [ ! -f "$root/src/platform/ps5_syscall.S" ]; then
  echo "SKIP: src/platform/ps5_syscall.S is not in this tree"
  exit 0
fi
case "$(uname -s)/$(uname -m)" in
  Darwin/x86_64 | FreeBSD/amd64) ;;
  *)
    echo "SKIP: raw system call wrappers need x86_64 macOS or FreeBSD (this is $(uname -s)/$(uname -m))"
    exit 0
    ;;
esac
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/scratch"
cc -Wall -Werror -o "$W/raw-syscall-test" \
  "$here/raw_syscall_test.c" "$root/src/platform/ps5_syscall.S"
"$W/raw-syscall-test" "$W/scratch"
