/* link(2) as some filesystems implement it: when ZSSHD_TEST_LINK_NOOP_DIR is
 * set and newpath starts with it, return 0 and create nothing. Otherwise the
 * real link(). See link_fault.h; this file is built without it. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int zsshd_test_link(const char *oldpath, const char *newpath);

int
zsshd_test_link(const char *oldpath, const char *newpath)
{
    const char *dir = getenv("ZSSHD_TEST_LINK_NOOP_DIR");

    if (dir != NULL && *dir != '\0' && strncmp(newpath, dir, strlen(dir)) == 0) {
        return 0;
    }
    return link(oldpath, newpath);
}
