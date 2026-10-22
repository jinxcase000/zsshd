#ifndef ZSSHD_SHELL_H
#define ZSSHD_SHELL_H

#include <libssh/libssh.h>

/* Run the command interpreter on an authenticated session channel.
 * interactive != 0 gives the banner, prompt, echo, colour and line-editing;
 * 0 is a clean scripted mode that reads lines until EOF (no PTY). */
void shell_run(ssh_channel channel, int interactive, int rows);

/* Run a single command string (one or more commands separated by ';' or
 * newlines) for an SSH 'exec' request. Returns the last command's exit
 * status. */
int shell_exec(ssh_channel channel, const char *command);

#endif /* ZSSHD_SHELL_H */
