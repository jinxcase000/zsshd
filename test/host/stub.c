#include "shell.h"
#include "pal_notification.h"
#include <string.h>
#include <unistd.h>
/* host stubs: the PS5 shell is not built on macOS */
void shell_run(ssh_channel c, int i, int r) { (void)c; (void)i; (void)r; }
int shell_exec(ssh_channel c, const char *cmd) {
  ssh_channel_write(c, "out:", 4);
  ssh_channel_write(c, cmd, (uint32_t)strlen(cmd));
  ssh_channel_write(c, "\n", 1);
  return strncmp(cmd, "fail", 4) == 0 ? 3 : 0;
}
int pal_notification_init(void) { return 0; }
void pal_notification_shutdown(void) {}
void pal_notification_send(const char *m) { (void)m; }
void pal_notification_send_ex(const char *m, const char *i) { (void)m; (void)i; }
