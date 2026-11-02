#include <signal.h>
#include <stdio.h>

#include "ssh_server.h"

int main(void) {
  /* Line-buffer the log so a test that kills the server still sees it. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  /* A write to a broken pipe or FIFO should fail with EPIPE, not kill the process. */
  signal(SIGPIPE, SIG_IGN);
  return ssh_server_run();
}
