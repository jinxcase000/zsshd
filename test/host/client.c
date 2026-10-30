/* host test client: key setup, then exec and report what the server sends */
#include <libssh/libssh.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  ssh_key k = NULL;
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
