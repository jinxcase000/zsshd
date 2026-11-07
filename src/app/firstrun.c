#include "firstrun.h"

#if defined(PLATFORM_PS5)

#include "zsshd_config.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FR_DIR ZSSHD_CONFIG_DIR
#define FR_PASSWORD FR_DIR "/password"
#define FR_AUTHKEYS FR_DIR "/authorized_keys"
#define FR_AUTOPW FR_DIR "/.autopw"
#define FR_LOG FR_DIR "/zsshd.log"

/* Fill buf with n random bytes: /dev/urandom when available, otherwise a
 * SplitMix64 mix of clock + pid + address (weaker, fallback only). */
static void fr_random(unsigned char *buf, size_t n) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t got = 0;
    while (got < n) {
      ssize_t r = read(fd, buf + got, n - got);
      if (r <= 0) {
        break;
      }
      got += (size_t)r;
    }
    close(fd);
    if (got == n) {
      return;
    }
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t x = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^
               ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)buf;
  for (size_t i = 0; i < n; i++) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    buf[i] = (unsigned char)z;
  }
}

/* Generate a (cap-1)-char password from an unambiguous charset. */
static void fr_gen(char *out, size_t cap) {
  static const char cs[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  size_t n = cap > 1 ? cap - 1 : 0;
  unsigned char rnd[64];
  if (n > sizeof(rnd)) {
    n = sizeof(rnd);
  }
  fr_random(rnd, n);
  for (size_t i = 0; i < n; i++) {
    out[i] = cs[rnd[i] % (sizeof(cs) - 1)];
  }
  out[n] = '\0';
}

static int fr_read_line(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return -1;
  }
  char *got = fgets(out, (int)cap, f);
  fclose(f);
  if (!got) {
    return -1;
  }
  out[strcspn(out, "\r\n")] = '\0';
  return 0;
}

static int fr_write(const char *path, const char *data, mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) {
    return -1;
  }
  (void)fchmod(fd, mode); /* enforce mode despite umask */
  size_t len = strlen(data);
  int ok = (write(fd, data, len) == (ssize_t)len) &&
           (write(fd, "\n", 1) == 1);
  close(fd);
  return ok ? 0 : -1;
}

static void fr_log(const char *pw) {
  int fd = open(FR_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    return;
  }
  (void)fchmod(fd, 0600);
  char line[128];
  int n = snprintf(line, sizeof(line), "zsshd: generated root password: %s\n",
                   pw);
  if (n > 0) {
    ssize_t w = write(fd, line, (size_t)n);
    (void)w;
  }
  close(fd);
}

int firstrun_password(char *out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';

  if (!zsshd_config_int("passwordauth", 1, 0, 1)) {
    return 0; /* key-only mode: never auto-generate a password */
  }
  if (access(FR_AUTHKEYS, F_OK) == 0) {
    return 0; /* operator is using public keys */
  }

  if (access(FR_PASSWORD, F_OK) != 0) {
    /* no credential at all: generate one so the console stays reachable */
    (void)mkdir(FR_DIR, 0700);
    char pw[17];
    fr_gen(pw, sizeof(pw));
    if (fr_write(FR_PASSWORD, pw, 0600) != 0) {
      return 0; /* could not persist; leave it to "no login" */
    }
    (void)fr_write(FR_AUTOPW, pw, 0600);
    fr_log(pw);
    snprintf(out, cap, "%s", pw);
    return 1;
  }

  /* a password file exists: keep showing it only while it is still the auto
   * one (i.e. the password file still matches the marker). */
  char cur[128], marker[128];
  if (fr_read_line(FR_PASSWORD, cur, sizeof(cur)) == 0 && cur[0] &&
      fr_read_line(FR_AUTOPW, marker, sizeof(marker)) == 0 &&
      strcmp(cur, marker) == 0) {
    snprintf(out, cap, "%s", cur);
    return 1;
  }
  (void)unlink(FR_AUTOPW); /* user set their own password: stop showing it */
  return 0;
}

#else /* !PLATFORM_PS5 */

int firstrun_password(char *out, size_t cap) {
  (void)out;
  (void)cap;
  return 0;
}

#endif
