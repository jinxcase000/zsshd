#ifndef ZSSHD_CONFIG_H
#define ZSSHD_CONFIG_H

#include <stddef.h>

/* Tiny settings reader. Each option is a single small file under
 * /data/zsshd/<name>, matching the convention already used by `password` and
 * `authorized_keys` (presence/contents of a file change behaviour). Only the
 * first line is read; a trailing newline/CR is ignored. */

#ifndef ZSSHD_CONFIG_DIR
#define ZSSHD_CONFIG_DIR "/data/zsshd"
#endif

/* First line of /data/zsshd/<name> parsed as a base-10 int. Returns `def`
 * when the file is absent, empty or unparseable, or when the value falls
 * outside [min, max]. */
int zsshd_config_int(const char *name, int def, int min, int max);

/* Copy the first line of /data/zsshd/<name> into `out` (NUL-terminated).
 * Returns 1 when a non-empty value was read, 0 otherwise (out is left
 * untouched on 0, so callers can pre-seed a default). */
int zsshd_config_str(const char *name, char *out, size_t cap);

#endif /* ZSSHD_CONFIG_H */
