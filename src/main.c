#include "zsshd_instance.h"
#include "instance_control.h"
#include "ssh_server.h"
#include "pal_notification.h"

#include <signal.h>
#include <stdio.h>
#include <pthread.h>

/* FreeBSD/PS5 thread naming. Weak so the payload still links if the SDK lacks
 * it. Without this the loader names our main thread "payload.elf", which
 * instance_control's thread scan would not recognize. */
__attribute__((weak)) void pthread_set_name_np(pthread_t, const char *);

/* Resolves the symbol the control listener calls (instance_control.c:328)
 * when a replacement payload asks this instance to stand down. Also our
 * SIGINT/SIGTERM landing point. */
void zsshd_shutdown_request(void) {
  ssh_server_request_stop();
}

static void on_signal(int sig) {
  (void)sig;
  ssh_server_request_stop();
}

int main(void) {
  if (pthread_set_name_np) {
    pthread_set_name_np(pthread_self(), "zsshd.elf");
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  zsshd_instance_init();

  /* If a previous zsshd still holds the console, step aside rather than
   * fighting it for the control port. */
  if (instance_control_replace_previous() != 0) {
    printf("[zsshd] A previous instance is still alive; aborting start\n");
    return 1;
  }

  if (instance_control_start() != 0) {
    printf("[zsshd] Failed to start control listener\n");
    return 1;
  }

  printf("[zsshd] Up. instance_id=%llx\n",
         (unsigned long long)zsshd_daemon_instance_id());

  pal_notification_init();

  /* Blocks here running the SSH accept loop until shutdown is requested. */
  int rc = ssh_server_run();

  printf("[zsshd] Shutting down\n");
  pal_notification_shutdown();
  instance_control_stop();
  return rc;
}
