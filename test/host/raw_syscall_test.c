/* Host test of src/platform/ps5_syscall.S (built by test/host/raw_syscall.sh):
 * each wrapper returns -1 with errno set when the kernel reports failure, and
 * the call's own result when it succeeds. The PS5 SDK's raw stubs returned the
 * positive errno instead. Run in an empty scratch directory given as argv[1]. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int __wrap_link(const char *oldpath, const char *newpath);
int __wrap_symlink(const char *target, const char *linkpath);
long __wrap_readlink(const char *path, char *buf, size_t size);
int __wrap_chown(const char *path, uid_t uid, gid_t gid);
int __wrap_fchown(int fd, uid_t uid, gid_t gid);
pid_t __wrap_getpgrp(void);
long __wrap_syscall(long number, ...);
long __wrap___syscall(long number, ...);

static int failed;

static void expect(const char *what, long got, long want, int want_errno) {
  int e = errno;
  int ok = got == want && (want_errno == 0 || e == want_errno);
  printf("%s %s: returned %ld errno %d, want %ld%s%d\n", ok ? "ok  " : "FAIL",
         what, got, want_errno ? e : 0, want, want_errno ? " errno " : " ",
         want_errno);
  if (!ok) {
    failed = 1;
  }
}

int main(int argc, char **argv) {
  char buf[64];
  if (argc != 2 || chdir(argv[1]) != 0) {
    fprintf(stderr, "usage: raw_syscall_test <empty scratch dir>\n");
    return 2;
  }
  int fd = open("file", O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0 || write(fd, "data", 4) != 4) {
    perror("file");
    return 2;
  }

  errno = 0;
  expect("link, source missing", __wrap_link("nosuch", "l1"), -1, ENOENT);
  errno = 0;
  expect("link, target exists", __wrap_link("file", "file"), -1, EEXIST);
  errno = 0;
  expect("link", __wrap_link("file", "l2"), 0, 0);
  struct stat st;
  expect("link made the name", stat("l2", &st) == 0 && st.st_nlink == 2, 1, 0);

  errno = 0;
  expect("symlink, name exists", __wrap_symlink("file", "file"), -1, EEXIST);
  errno = 0;
  expect("symlink", __wrap_symlink("file", "sl"), 0, 0);

  errno = 0;
  expect("readlink, regular file", __wrap_readlink("file", buf, sizeof(buf)),
         -1, EINVAL);
  errno = 0;
  expect("readlink, missing", __wrap_readlink("nosuch", buf, sizeof(buf)), -1,
         ENOENT);
  errno = 0;
  expect("readlink", __wrap_readlink("sl", buf, sizeof(buf)), 4, 0);
  expect("readlink text", memcmp(buf, "file", 4) == 0, 1, 0);

  errno = 0;
  expect("chown, missing", __wrap_chown("nosuch", (uid_t)-1, (gid_t)-1), -1,
         ENOENT);
  errno = 0;
  expect("chown, no change", __wrap_chown("file", (uid_t)-1, (gid_t)-1), 0, 0);
  errno = 0;
  expect("fchown, bad fd", __wrap_fchown(-1, (uid_t)-1, (gid_t)-1), -1, EBADF);

  expect("getpgrp", __wrap_getpgrp(), getpgrp(), 0);

  errno = 0;
  expect("syscall(SYS_close, -1)", __wrap_syscall(SYS_close, -1), -1, EBADF);
  expect("syscall(SYS_getpid)", __wrap_syscall(SYS_getpid), getpid(), 0);
  errno = 0;
  expect("__syscall(SYS_close, -1)", __wrap___syscall(SYS_close, -1), -1,
         EBADF);
  /* six arguments after the number: the last is read from the stack */
  errno = 0;
  expect("syscall, six arguments", __wrap_syscall(SYS_close, -1, 2, 3, 4, 5, 6),
         -1, EBADF);

  close(fd);
  if (failed) {
    return 1;
  }
  printf("PASS: raw system call wrappers report failure as -1 with errno\n");
  return 0;
}
