#ifndef ZSSHD_SSH_SERVER_H
#define ZSSHD_SSH_SERVER_H

/* Bring up the SSH listener and serve sessions until ssh_server_request_stop()
 * is called. Blocks the calling thread. Returns 0 on clean shutdown, -1 if the
 * listener could not be established. */
int ssh_server_run(void);

/* Ask the accept loop to stop; safe to call from a signal or another thread. */
void ssh_server_request_stop(void);

#endif /* ZSSHD_SSH_SERVER_H */
