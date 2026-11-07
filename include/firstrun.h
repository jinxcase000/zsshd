#ifndef ZSSHD_FIRSTRUN_H
#define ZSSHD_FIRSTRUN_H

#include <stddef.h>

/* First-run credential bootstrap. If password auth is enabled and there is no
 * credential configured (no password file and no authorized_keys), generate a
 * random root password, store it 0600, mark it as auto-generated, and append
 * it to a log under /data. If a password should be shown in the startup toast
 * (freshly generated, or still the unchanged auto one), copy it into `out` and
 * return 1; otherwise return 0 and leave `out` empty. */
int firstrun_password(char *out, size_t cap);

#endif /* ZSSHD_FIRSTRUN_H */
