#ifndef ZSSHD_SHELL_H
#define ZSSHD_SHELL_H

#include <libssh/libssh.h>

/* Run the interactive command interpreter on an authenticated session channel.
 * Returns when the client sends 'exit' or the channel closes. */
void shell_run(ssh_channel channel);

#endif /* ZSSHD_SHELL_H */
