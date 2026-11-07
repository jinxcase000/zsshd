/* Host stand-ins for the console-only calls ssh_server.c makes, so the real
 * shell (shell.c) runs on a host (test/host/touch.sh). */
#include "pal_notification.h"

int pal_notification_init(void) { return 0; }
void pal_notification_shutdown(void) {}
void pal_notification_send(const char *m) { (void)m; }
void pal_notification_send_ex(const char *m, const char *i) {
  (void)m;
  (void)i;
}

/* touch test hooks: make the libc time-setting calls fail the way the PS5's
 * utimensat does, so cmd_touch's fallbacks run. The executable's own
 * definition wins over libc's at link time. */
#if defined(ZSSHD_TEST_UTIMENSAT_FAIL) || defined(ZSSHD_TEST_UTIMES_FAIL)
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif
#ifdef ZSSHD_TEST_UTIMENSAT_FAIL
int utimensat(int fd, const char *path, const struct timespec times[2],
              int flag) {
  (void)fd;
  (void)path;
  (void)times;
  (void)flag;
  errno = ENOSYS;
  return -1;
}
#endif
#ifdef ZSSHD_TEST_UTIMES_FAIL
int futimes(int fd, const struct timeval times[2]) {
  (void)fd;
  (void)times;
  errno = EPERM;
  return -1;
}
int utimes(const char *path, const struct timeval times[2]) {
  (void)path;
  (void)times;
  errno = EPERM;
  return -1;
}
#endif
