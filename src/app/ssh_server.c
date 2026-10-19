#include "ssh_server.h"

#if defined(PLATFORM_PS5)

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include "shell.h"
#include "pal_notification.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef ZSSHD_PORT
#define ZSSHD_PORT 22
#endif
#ifndef ZSSHD_VERSION
#define ZSSHD_VERSION "0.0.3"
#endif
#ifndef ZSSHD_DIR
#define ZSSHD_DIR "/data/zsshd"
#endif
#define ZSSHD_HOSTKEY  ZSSHD_DIR "/hostkey"
#define ZSSHD_PASSWORD ZSSHD_DIR "/password"
#define ZSSHD_AUTHKEYS ZSSHD_DIR "/authorized_keys"

static ssh_bind g_bind = NULL;
static volatile sig_atomic_t g_running = 0;

/* Per-session state threaded through the libssh callbacks. */
struct session_state {
  int authenticated;
  int channel_wanted_shell;
  ssh_channel channel;
};

/* --- optional-auth helpers -------------------------------------------- */

static int password_auth_configured(void) {
  return access(ZSSHD_PASSWORD, F_OK) == 0;
}
static int pubkey_auth_configured(void) {
  return access(ZSSHD_AUTHKEYS, F_OK) == 0;
}
/* Passwordless root is the default, but only while the operator has not opted
 * into a real method by dropping in a password or authorized_keys file. */
static int none_auth_allowed(void) {
  return !password_auth_configured() && !pubkey_auth_configured();
}

/* --- auth callbacks --------------------------------------------------- */

static int auth_none_cb(ssh_session session, const char *user, void *userdata) {
  (void)session;
  (void)user;
  struct session_state *st = (struct session_state *)userdata;
  if (!none_auth_allowed()) {
    return SSH_AUTH_DENIED;
  }
  st->authenticated = 1;
  return SSH_AUTH_SUCCESS;
}

static int auth_password_cb(ssh_session session, const char *user,
                            const char *password, void *userdata) {
  (void)session;
  (void)user;
  struct session_state *st = (struct session_state *)userdata;

  FILE *f = fopen(ZSSHD_PASSWORD, "r");
  if (!f) {
    return SSH_AUTH_DENIED; /* password method not configured */
  }
  char stored[256];
  char *line = fgets(stored, sizeof(stored), f);
  fclose(f);
  if (!line) {
    return SSH_AUTH_DENIED;
  }
  stored[strcspn(stored, "\r\n")] = '\0';

  if (strcmp(stored, password) == 0) {
    st->authenticated = 1;
    return SSH_AUTH_SUCCESS;
  }
  return SSH_AUTH_DENIED;
}

/* Returns 1 if the offered public key matches a line in authorized_keys. */
static int pubkey_in_authorized(ssh_key offered) {
  FILE *f = fopen(ZSSHD_AUTHKEYS, "r");
  if (!f) {
    return 0;
  }
  char line[4096];
  int matched = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }
    /* authorized_keys lines are "<type> <base64> [comment]"; libssh wants
     * just the base64 blob with its type prefix. */
    char *type = strtok(line, " \t");
    char *b64 = type ? strtok(NULL, " \t") : NULL;
    if (!type || !b64) {
      continue;
    }
    ssh_key candidate = NULL;
    if (ssh_pki_import_pubkey_base64(b64, ssh_key_type_from_name(type),
                                     &candidate) != SSH_OK) {
      continue;
    }
    if (ssh_key_cmp(offered, candidate, SSH_KEY_CMP_PUBLIC) == 0) {
      matched = 1;
    }
    ssh_key_free(candidate);
    if (matched) {
      break;
    }
  }
  fclose(f);
  return matched;
}

static int auth_pubkey_cb(ssh_session session, const char *user,
                          struct ssh_key_struct *pubkey, char signature_state,
                          void *userdata) {
  (void)session;
  (void)user;
  struct session_state *st = (struct session_state *)userdata;

  if (!pubkey_auth_configured()) {
    return SSH_AUTH_DENIED;
  }
  /* Probe phase: tell the client the key is acceptable so it will sign. */
  if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
    return pubkey_in_authorized(pubkey) ? SSH_AUTH_SUCCESS : SSH_AUTH_DENIED;
  }
  /* Signature phase: only accept a cryptographically valid signature from a
   * key we actually trust. */
  if (signature_state == SSH_PUBLICKEY_STATE_VALID &&
      pubkey_in_authorized(pubkey)) {
    st->authenticated = 1;
    return SSH_AUTH_SUCCESS;
  }
  return SSH_AUTH_DENIED;
}

/* --- channel callbacks ------------------------------------------------ */

static int pty_request_cb(ssh_session session, ssh_channel channel,
                          const char *term, int cols, int rows, int px, int py,
                          void *userdata) {
  (void)session;
  (void)channel;
  (void)term;
  (void)cols;
  (void)rows;
  (void)px;
  (void)py;
  (void)userdata;
  return 0; /* accept the pty */
}

static int shell_request_cb(ssh_session session, ssh_channel channel,
                            void *userdata) {
  (void)session;
  (void)channel;
  struct session_state *st = (struct session_state *)userdata;
  st->channel_wanted_shell = 1;
  return 0; /* accept the shell */
}

static ssh_channel channel_open_cb(ssh_session session, void *userdata) {
  struct session_state *st = (struct session_state *)userdata;
  if (st->channel) {
    return NULL; /* one session channel is enough for now */
  }
  st->channel = ssh_channel_new(session);
  return st->channel;
}

/* --- per-connection handler ------------------------------------------- */

static void *handle_session(void *arg) {
  ssh_session session = (ssh_session)arg;
  struct session_state st;
  memset(&st, 0, sizeof(st));

  if (ssh_handle_key_exchange(session) != SSH_OK) {
    printf("[zsshd] key exchange failed: %s\n", ssh_get_error(session));
    goto done;
  }

  struct ssh_server_callbacks_struct server_cb;
  memset(&server_cb, 0, sizeof(server_cb));
  server_cb.userdata = &st;
  server_cb.auth_none_function = auth_none_cb;
  server_cb.auth_password_function = auth_password_cb;
  server_cb.auth_pubkey_function = auth_pubkey_cb;
  server_cb.channel_open_request_session_function = channel_open_cb;
  ssh_callbacks_init(&server_cb);
  ssh_set_server_callbacks(session, &server_cb);

  int methods = SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_PUBLICKEY;
  if (none_auth_allowed()) {
    methods |= SSH_AUTH_METHOD_NONE;
  }
  ssh_set_auth_methods(session, methods);

  struct ssh_channel_callbacks_struct channel_cb;
  memset(&channel_cb, 0, sizeof(channel_cb));
  channel_cb.userdata = &st;
  channel_cb.channel_pty_request_function = pty_request_cb;
  channel_cb.channel_shell_request_function = shell_request_cb;
  ssh_callbacks_init(&channel_cb);

  ssh_event event = ssh_event_new();
  ssh_event_add_session(event, session);

  /* Pump the event loop until the client authenticates and asks for a shell
   * on a session channel. */
  int set_channel_cb = 0;
  while (g_running && !(st.authenticated && st.channel_wanted_shell)) {
    if (ssh_event_dopoll(event, 500) == SSH_ERROR) {
      break;
    }
    if (st.channel && !set_channel_cb) {
      ssh_set_channel_callbacks(st.channel, &channel_cb);
      set_channel_cb = 1;
    }
  }

  if (st.authenticated && st.channel && st.channel_wanted_shell) {
    shell_run(st.channel);
    ssh_channel_send_eof(st.channel);
    ssh_channel_close(st.channel);
  }

  ssh_event_free(event);

done:
  ssh_disconnect(session);
  ssh_free(session);
  return NULL;
}

/* --- host key --------------------------------------------------------- */

static int ensure_hostkey(void) {
  (void)mkdir(ZSSHD_DIR, 0700);
  if (access(ZSSHD_HOSTKEY, F_OK) == 0) {
    return 0;
  }
  ssh_key key = NULL;
  /* RSA, not ed25519: the mbedtls crypto backend is unreliable for ed25519. */
  if (ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, &key) != SSH_OK) {
    return -1;
  }
  int rc = ssh_pki_export_privkey_file(key, NULL, NULL, NULL, ZSSHD_HOSTKEY);
  ssh_key_free(key);
  return (rc == SSH_OK) ? 0 : -1;
}

/* --- public entry points --------------------------------------------- */

/* Best-effort local LAN IP: connect a UDP socket toward a public address so
 * the kernel picks the outbound interface, then read it back. No packets are
 * actually sent. */
static void local_ip(char *out, size_t n) {
  snprintf(out, n, "0.0.0.0");
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    return;
  }
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(53);
  a.sin_addr.s_addr = inet_addr("8.8.8.8");
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
    struct sockaddr_in local;
    socklen_t ll = sizeof(local);
    if (getsockname(s, (struct sockaddr *)&local, &ll) == 0) {
      snprintf(out, n, "%s", inet_ntoa(local.sin_addr));
    }
  }
  close(s);
}

void ssh_server_request_stop(void) {
  g_running = 0;
}

int ssh_server_run(void) {
  if (ssh_init() != SSH_OK) {
    return -1;
  }
  if (ensure_hostkey() != 0) {
    printf("[zsshd] could not create host key\n");
    return -1;
  }

  g_bind = ssh_bind_new();
  if (!g_bind) {
    return -1;
  }
  int port = ZSSHD_PORT;
  ssh_bind_options_set(g_bind, SSH_BIND_OPTIONS_BINDPORT, &port);
  ssh_bind_options_set(g_bind, SSH_BIND_OPTIONS_HOSTKEY, ZSSHD_HOSTKEY);

  if (ssh_bind_listen(g_bind) != SSH_OK) {
    printf("[zsshd] bind/listen failed: %s\n", ssh_get_error(g_bind));
    ssh_bind_free(g_bind);
    g_bind = NULL;
    return -1;
  }

  g_running = 1;
  printf("[zsshd] SSH listening on port %d\n", port);

  {
    char ip[64];
    local_ip(ip, sizeof(ip));
    char toast[192];
    snprintf(toast, sizeof(toast),
             "zSSHd [ver.%s] by JinxCase Started\n%s:%d", ZSSHD_VERSION, ip,
             port);
    pal_notification_send(toast);
  }

  int bind_fd = ssh_bind_get_fd(g_bind);
  while (g_running) {
    struct pollfd pfd;
    pfd.fd = bind_fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 500);
    if (pr <= 0) {
      continue; /* timeout: re-check g_running */
    }

    ssh_session session = ssh_new();
    if (!session) {
      continue;
    }
    if (ssh_bind_accept(g_bind, session) != SSH_OK) {
      ssh_free(session);
      continue;
    }

    pthread_t t;
    pthread_attr_t attr;
    int have_attr = (pthread_attr_init(&attr) == 0);
    if (have_attr) {
      (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      /* libssh's handshake plus the shell's recursive cp/rm need more than the
       * small default thread stack; a too-small stack segfaults the whole
       * payload, since every connection shares this process. */
      (void)pthread_attr_setstacksize(&attr, 1024 * 1024);
    }
    if (pthread_create(&t, have_attr ? &attr : NULL, handle_session,
                       session) != 0) {
      ssh_disconnect(session);
      ssh_free(session);
    }
    if (have_attr) {
      (void)pthread_attr_destroy(&attr);
    }
  }

  ssh_bind_free(g_bind);
  g_bind = NULL;
  ssh_finalize();
  return 0;
}

#else /* !PLATFORM_PS5 */

void ssh_server_request_stop(void) {}
int ssh_server_run(void) { return -1; }

#endif
