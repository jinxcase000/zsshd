/* Host-test fault injection for link(2). test/host/sftp.sh builds the
 * vendored libssh with -include of this file, so the SFTP server's link()
 * calls go to zsshd_test_link() (test/host/link_fault.c), which can make
 * link() report success without creating anything. */
#ifndef ZSSHD_TEST_LINK_FAULT_H
#define ZSSHD_TEST_LINK_FAULT_H
#include <unistd.h>
int zsshd_test_link(const char *oldpath, const char *newpath);
#define link(oldpath, newpath) zsshd_test_link((oldpath), (newpath))
#endif
