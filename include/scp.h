#ifndef ZSSHD_SCP_H
#define ZSSHD_SCP_H

#include <libssh/libssh.h>

/* Classic scp protocol support (the `scp -t`/`scp -f` exec path some clients
 * use instead of the SFTP subsystem). Only the sink (`scp -t`, upload to the
 * console) is implemented for now. */

/* Returns 1 if `command` (from an SSH exec request) is a classic scp sink
 * invocation, i.e. "scp ... -t <dest>", otherwise 0. */
int scp_is_sink(const char *command);

/* Handle a classic scp sink over `channel`: receive file(s) from the client
 * and stream them to disk under the requested destination. Returns the exit
 * status to report to the client (0 on success). */
int scp_sink(ssh_channel channel, const char *command);

/* Returns 1 if `command` is a classic scp source invocation
 * ("scp ... -f <path>"), otherwise 0. */
int scp_is_source(const char *command);

/* Handle a classic scp source over `channel`: stream the requested file to the
 * client. Returns the exit status to report (0 on success). */
int scp_source(ssh_channel channel, const char *command);

#endif /* ZSSHD_SCP_H */
