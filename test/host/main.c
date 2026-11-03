#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libssh/libssh.h>

#include "ssh_server.h"

/* SIGUSR1 stands the server down the way a replacement payload does: the
 * console's control listener (instance_control.c) and src/main.c's signal
 * handler both end up in ssh_server_request_stop(). test/host/standdown.sh
 * uses it. */
static void on_standdown(int sig) {
  (void)sig;
  ssh_server_request_stop();
}

int main(void) {
  /* Line-buffer the log so a test that kills the server still sees it. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  /* A write to a broken pipe or FIFO should fail with EPIPE, not kill the process. */
  signal(SIGPIPE, SIG_IGN);
  signal(SIGUSR1, on_standdown);
  int rc = ssh_server_run();
  /* As src/main.c: the process exits once the accept loop returns. */
  printf("[zsshd] Shutting down\n");
  /* ZSSHD_TEST_EXIT_DELAY_MS: do what process exit does to libssh first,
   * then stay alive that long, so session threads still running then show.
   * At exit libssh's destructor finalizes it (frees the mbedtls RNG); with
   * its constructor run, ssh_server_run's own ssh_finalize() only dropped a
   * reference, so this one frees. The console's exit takes time; on the host
   * the process would be gone before a session thread got to run. */
  const char *delay = getenv("ZSSHD_TEST_EXIT_DELAY_MS");
  if (delay != NULL && atoi(delay) > 0) {
    ssh_finalize();
    usleep((useconds_t)atoi(delay) * 1000U);
  }
  return rc;
}
