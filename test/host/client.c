/* host test client: key setup, then exec and report what the server sends */
#include <libssh/libssh.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
/* One connection, one exec of cmd, output discarded; returns its exit status
 * (255 when there is none, 2 when a setup step fails, 3 when a step waits
 * longer than 15 s). *stage names the last step reached. */
static int exec_once(int port, const char *keyfile, const char *cmd,
                     const char **stage) {
  ssh_key k = NULL;
  ssh_session s = ssh_new();
  long tmo = 15;
  ssh_options_set(s, SSH_OPTIONS_HOST, "127.0.0.1");
  ssh_options_set(s, SSH_OPTIONS_PORT, &port);
  ssh_options_set(s, SSH_OPTIONS_USER, "root");
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &tmo);
  int code = 2;
  ssh_channel c = NULL;
  *stage = "connect";
  if (ssh_connect(s) != SSH_OK) goto out;
  *stage = "auth";
  if (ssh_pki_import_privkey_file(keyfile, NULL, NULL, NULL, &k) != SSH_OK
      || ssh_userauth_publickey(s, NULL, k) != SSH_AUTH_SUCCESS) goto out;
  *stage = "open";
  c = ssh_channel_new(s);
  if (c == NULL || ssh_channel_open_session(c) != SSH_OK) goto out;
  *stage = "exec";
  if (ssh_channel_request_exec(c, cmd) != SSH_OK) goto out;
  *stage = "read";
  char buf[4096];
  int n;
  while ((n = ssh_channel_read_timeout(c, buf, sizeof buf, 0, 15000)) > 0) {}
  if (n == 0 && !ssh_channel_is_eof(c)) { code = 3; goto out; }
  if (n < 0) goto out;
  *stage = "exit-status";
  uint32_t st = 0; char *sig = NULL; int core = 0;
  code = ssh_channel_get_exit_state(c, &st, &sig, &core) == SSH_OK ? (int)st : 255;
  *stage = "close";
  ssh_channel_send_eof(c);
  ssh_channel_close(c);
out:
  if (c != NULL) ssh_channel_free(c);
  ssh_key_free(k);
  ssh_disconnect(s);
  ssh_free(s);
  return code;
}

int main(int argc, char **argv) {
  ssh_key k = NULL;
  /* client loop <port> <keyfile> <cmd> <n>: n sequential connections, each
   * running cmd once; stops at the first non-zero exit status. */
  if (argc >= 6 && !strcmp(argv[1], "loop")) {
    int n = atoi(argv[5]);
    for (int i = 0; i < n; i++) {
      const char *stage = "";
      int rc = exec_once(atoi(argv[2]), argv[3], argv[4], &stage);
      if ((i + 1) % 100 == 0) { fprintf(stderr, "LOOP %d done\n", i + 1); }
      if (rc != 0) { printf("LOOP exec %d of %d: rc=%d at %s\n", i + 1, n, rc, stage); return 1; }
    }
    printf("LOOP ok=%d\n", n);
    return 0;
  }
  /* client raw <port> <keyfile> <cmd>: one exec; the output bytes exactly as
   * sent go to stdout, and the exit status is ours (255 when there is none,
   * 2 when a setup step fails). */
  if (argc >= 5 && !strcmp(argv[1], "raw")) {
    ssh_session s = ssh_new();
    int port = atoi(argv[2]);
    ssh_options_set(s, SSH_OPTIONS_HOST, "127.0.0.1");
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s, SSH_OPTIONS_USER, "root");
    if (ssh_connect(s) != SSH_OK) { fprintf(stderr, "connect fail\n"); return 2; }
    if (ssh_pki_import_privkey_file(argv[3], NULL, NULL, NULL, &k) != SSH_OK
        || ssh_userauth_publickey(s, NULL, k) != SSH_AUTH_SUCCESS) { fprintf(stderr, "auth fail\n"); return 2; }
    ssh_channel c = ssh_channel_new(s);
    if (c == NULL || ssh_channel_open_session(c) != SSH_OK
        || ssh_channel_request_exec(c, argv[4]) != SSH_OK) { fprintf(stderr, "exec fail\n"); return 2; }
    char buf[4096]; int n;
    while ((n = ssh_channel_read(c, buf, sizeof buf, 0)) > 0) { fwrite(buf, 1, (size_t)n, stdout); }
    fflush(stdout);
    uint32_t code = 0; char *sig = NULL; int core = 0;
    int r = ssh_channel_get_exit_state(c, &code, &sig, &core);
    ssh_channel_send_eof(c);
    ssh_channel_close(c);
    ssh_channel_free(c);
    ssh_disconnect(s);
    ssh_free(s);
    return r == SSH_OK ? (int)code : 255;
  }
  /* client pty <port> <keyfile> <text>: an interactive session (PTY, 80x24,
   * then a shell); text is typed in one write, and every byte the server
   * sends goes to stdout until the channel ends (text should end with
   * "exit\n"). Returns 0, 2 when a setup step fails, 3 when the server is
   * silent for 15 s before the end. */
  if (argc >= 5 && !strcmp(argv[1], "pty")) {
    ssh_session s = ssh_new();
    int port = atoi(argv[2]);
    ssh_options_set(s, SSH_OPTIONS_HOST, "127.0.0.1");
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s, SSH_OPTIONS_USER, "root");
    if (ssh_connect(s) != SSH_OK) { fprintf(stderr, "connect fail\n"); return 2; }
    if (ssh_pki_import_privkey_file(argv[3], NULL, NULL, NULL, &k) != SSH_OK
        || ssh_userauth_publickey(s, NULL, k) != SSH_AUTH_SUCCESS) { fprintf(stderr, "auth fail\n"); return 2; }
    ssh_channel c = ssh_channel_new(s);
    if (c == NULL || ssh_channel_open_session(c) != SSH_OK
        || ssh_channel_request_pty_size(c, "xterm", 80, 24) != SSH_OK
        || ssh_channel_request_shell(c) != SSH_OK) { fprintf(stderr, "shell fail\n"); return 2; }
    size_t len = strlen(argv[4]);
    if (ssh_channel_write(c, argv[4], (uint32_t)len) != (int)len) { fprintf(stderr, "write fail\n"); return 2; }
    char buf[4096]; int n, code = 0;
    while ((n = ssh_channel_read_timeout(c, buf, sizeof buf, 0, 15000)) > 0) { fwrite(buf, 1, (size_t)n, stdout); }
    if (n == 0 && !ssh_channel_is_eof(c)) { code = 3; }
    fflush(stdout);
    ssh_channel_send_eof(c);
    ssh_channel_close(c);
    ssh_channel_free(c);
    ssh_disconnect(s);
    ssh_free(s);
    return code;
  }
  if (argc >= 3 && !strcmp(argv[1], "mkkey")) {
    ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &k);
    ssh_pki_export_privkey_file(k, NULL, NULL, NULL, argv[2]);
    char *b64; ssh_pki_export_pubkey_base64(k, &b64);
    printf("ssh-ed25519 %s test\n", b64);
    return 0;
  }
  /* client <port> <keyfile> <cmd> */
  ssh_session s = ssh_new();
  int port = atoi(argv[1]);
  ssh_options_set(s, SSH_OPTIONS_HOST, "127.0.0.1");
  ssh_options_set(s, SSH_OPTIONS_PORT, &port);
  ssh_options_set(s, SSH_OPTIONS_USER, "root");
  if (ssh_connect(s) != SSH_OK) { printf("connect fail\n"); return 2; }
  ssh_pki_import_privkey_file(argv[2], NULL, NULL, NULL, &k);
  if (ssh_userauth_publickey(s, NULL, k) != SSH_AUTH_SUCCESS) { printf("auth fail\n"); return 2; }
  ssh_channel c = ssh_channel_new(s);
  ssh_channel_open_session(c);
  if (ssh_channel_request_exec(c, argv[3]) != SSH_OK) { printf("exec fail\n"); return 2; }
  char buf[256]; int n, fail = 0;
  while ((n = ssh_channel_read(c, buf, sizeof buf - 1, 0)) > 0) { buf[n] = 0; printf("data: %s", buf); }
  printf("read ended n=%d eof=%d open=%d\n", n, ssh_channel_is_eof(c), ssh_channel_is_open(c));
  /* OpenSSH answers the server's close with its own and exits; a server that
   * drops the TCP connection first makes it print "closed by remote host".
   * Pump packets until the server's close arrives, idle 600 ms as a slow
   * client would, then pump again: the connection must still be up. */
  ssh_event ev = ssh_event_new(); ssh_event_add_session(ev, s);
  { int k2; for (k2 = 0; k2 < 40 && !ssh_channel_is_closed(c); k2++) ssh_event_dopoll(ev, 100);
    printf("remote close seen=%d connected=%d\n", ssh_channel_is_closed(c), ssh_is_connected(s));
    int was_up = ssh_is_connected(s);
    usleep(600000);
    ssh_event_dopoll(ev, 100); ssh_event_dopoll(ev, 100);
    int still = ssh_is_connected(s);
    printf("after idle: connected=%d\n", still);
    if (!ssh_channel_is_closed(c) || !was_up || !still) { printf("FAIL server dropped before client closed\n"); fail = 1; } }
  /* exit-status arrives before EOF; libssh records it on the channel */
  int st = -1; /* ssh_channel_get_exit_state is the 0.10+ API */
  uint32_t code = 0; char *sig = NULL; int core = 0;
  int r = ssh_channel_get_exit_state(c, &code, &sig, &core);
  printf("exit_state rc=%d code=%u signal=%s\n", r, code, sig ? sig : "(none)");
  (void)st;
  printf("RESULT code=%u have_status=%d\n", code, r == SSH_OK);
  if (fail) return 99;
  return r == SSH_OK ? (int)code : 255;
}
