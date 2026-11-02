#include "zsshd_config.h"

#if defined(PLATFORM_PS5)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read the first line of /data/zsshd/<name> into `line`, stripped of any
 * trailing CR/LF. Returns 0 on success, -1 if the file is missing or empty. */
static int read_setting(const char *name, char *line, size_t cap) {
  char path[256];
  int n = snprintf(path, sizeof(path), "%s/%s", ZSSHD_CONFIG_DIR, name);
  if (n <= 0 || (size_t)n >= sizeof(path)) {
    return -1;
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    return -1;
  }
  char *got = fgets(line, (int)cap, f);
  fclose(f);
  if (!got) {
    return -1;
  }
  line[strcspn(line, "\r\n")] = '\0';
  return 0;
}

int zsshd_config_int(const char *name, int def, int min, int max) {
  char line[64];
  if (read_setting(name, line, sizeof(line)) != 0 || line[0] == '\0') {
    return def;
  }
  char *end = NULL;
  long v = strtol(line, &end, 10);
  if (end == line) {
    return def;
  }
  if (v < (long)min || v > (long)max) {
    return def;
  }
  return (int)v;
}

int zsshd_config_str(const char *name, char *out, size_t cap) {
  char line[256];
  if (read_setting(name, line, sizeof(line)) != 0 || line[0] == '\0') {
    return 0;
  }
  snprintf(out, cap, "%s", line);
  return 1;
}

#else /* !PLATFORM_PS5 */

int zsshd_config_int(const char *name, int def, int min, int max) {
  (void)name;
  (void)min;
  (void)max;
  return def;
}

int zsshd_config_str(const char *name, char *out, size_t cap) {
  (void)name;
  (void)out;
  (void)cap;
  return 0;
}

#endif
