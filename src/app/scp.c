#include "scp.h"

#if defined(PLATFORM_PS5)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define SCP_CHUNK 32768 /* streaming block size; never buffer a whole file */

/* --- argument parsing ------------------------------------------------- */

/* Split the exec command into whitespace tokens in place (no quoting). argv[]
 * point into `buf`. Returns the token count. */
static int scp_tokenize(char *buf, char **argv, int max) {
  int n = 0;
  char *p = buf;
  while (*p && n < max) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (!*p) {
      break;
    }
    argv[n++] = p;
    while (*p && *p != ' ' && *p != '\t') {
      p++;
    }
    if (*p) {
      *p++ = '\0';
    }
  }
  return n;
}

int scp_is_sink(const char *command) {
  if (!command || strncmp(command, "scp ", 4) != 0) {
    return 0;
  }
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", command);
  char *argv[64];
  int argc = scp_tokenize(tmp, argv, 64);
  if (argc < 2 || strcmp(argv[0], "scp") != 0) {
    return 0;
  }
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && strchr(argv[i], 't')) {
      return 1;
    }
  }
  return 0;
}

/* --- channel I/O ------------------------------------------------------ */

static int chan_write_all(ssh_channel ch, const void *data, size_t len) {
  const char *p = (const char *)data;
  while (len > 0) {
    int w = ssh_channel_write(ch, p, (uint32_t)len);
    if (w <= 0) {
      return -1;
    }
    p += w;
    len -= (size_t)w;
  }
  return 0;
}

/* Read exactly one byte; returns 1 on success, 0 on EOF/error. */
static int chan_read_byte(ssh_channel ch, char *c) {
  return ssh_channel_read(ch, c, 1, 0) == 1 ? 1 : 0;
}

static void scp_ack(ssh_channel ch) {
  char zero = 0;
  (void)chan_write_all(ch, &zero, 1);
}

static void scp_fatal(ssh_channel ch, const char *msg) {
  char code = 0x02;
  (void)chan_write_all(ch, &code, 1);
  (void)chan_write_all(ch, msg, strlen(msg));
  (void)chan_write_all(ch, "\n", 1);
}

/* Read one control line (up to and excluding '\n', opcode byte included) into
 * buf. Returns the length, or 0 on EOF. */
static int scp_read_line(ssh_channel ch, char *buf, size_t cap) {
  size_t i = 0;
  while (i + 1 < cap) {
    char c;
    if (!chan_read_byte(ch, &c)) {
      return 0;
    }
    if (c == '\n') {
      break;
    }
    buf[i++] = c;
  }
  buf[i] = '\0';
  return (int)i;
}

static int is_dir(const char *path) {
  struct stat sb;
  return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

/* --- sink ------------------------------------------------------------- */

int scp_sink(ssh_channel channel, const char *command) {
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", command);
  char *argv[64];
  int argc = scp_tokenize(tmp, argv, 64);

  int target_is_dir_flag = 0;
  const char *dest = NULL;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strchr(argv[i], 'd')) {
        target_is_dir_flag = 1;
      }
      /* -t (sink), -r, -p are tolerated; -r/-p behaviour is limited below */
    } else {
      dest = argv[i]; /* last non-flag token is the destination */
    }
  }
  if (!dest) {
    scp_fatal(channel, "scp: no destination");
    return 1;
  }

  char destbuf[PATH_MAX];
  if (dest[0] == '/') {
    snprintf(destbuf, sizeof(destbuf), "%s", dest);
  } else {
    snprintf(destbuf, sizeof(destbuf), "/%s", dest);
  }
  int dest_is_dir = target_is_dir_flag || is_dir(destbuf);

  scp_ack(channel); /* tell the client we are ready */

  int rc = 0;
  for (;;) {
    char line[PATH_MAX + 64];
    int n = scp_read_line(channel, line, sizeof(line));
    if (n <= 0) {
      break; /* client closed its side: transfer complete */
    }
    char op = line[0];
    if (op == 0x01 || op == 0x02) {
      rc = 1; /* client reported an error */
      break;
    }
    if (op == 'T' || op == 'E') {
      scp_ack(channel); /* timestamps / leave-dir: accept and ignore */
      continue;
    }
    if (op == 'D') {
      scp_fatal(channel, "scp: recursive upload not supported yet");
      rc = 1;
      break;
    }
    if (op != 'C') {
      scp_fatal(channel, "scp: protocol error");
      rc = 1;
      break;
    }

    /* C<mode> <size> <name> */
    unsigned mode = 0;
    long long size = 0;
    char name[256] = {0};
    if (sscanf(line, "C%o %lld %255[^\n]", &mode, &size, name) != 3 || size < 0) {
      scp_fatal(channel, "scp: bad file header");
      rc = 1;
      break;
    }

    char path[PATH_MAX];
    if (dest_is_dir) {
      snprintf(path, sizeof(path), "%s/%s", destbuf, name);
    } else {
      snprintf(path, sizeof(path), "%s", destbuf);
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                  mode ? (mode & 0777) : 0644);
    if (fd < 0) {
      scp_fatal(channel, "scp: cannot create destination file");
      rc = 1;
      break;
    }

    scp_ack(channel); /* ready to receive the file body */

    long long remaining = size;
    char buf[SCP_CHUNK];
    int ok = 1;
    while (remaining > 0) {
      size_t want = remaining < (long long)sizeof(buf) ? (size_t)remaining
                                                       : sizeof(buf);
      int r = ssh_channel_read(channel, buf, (uint32_t)want, 0);
      if (r <= 0) {
        ok = 0;
        break;
      }
      ssize_t off = 0;
      while (off < r) {
        ssize_t w = write(fd, buf + off, (size_t)(r - off));
        if (w <= 0) {
          ok = 0;
          break;
        }
        off += w;
      }
      if (!ok) {
        break;
      }
      remaining -= r;
    }
    close(fd);
    if (!ok) {
      scp_fatal(channel, "scp: write failed");
      rc = 1;
      break;
    }

    /* client's trailing status byte for this file (0 = ok) */
    char tb;
    if (!chan_read_byte(channel, &tb)) {
      rc = 1;
      break;
    }
    if (tb != 0) {
      rc = 1;
      break;
    }
    scp_ack(channel); /* acknowledge the file */
  }
  return rc;
}

static const char *scp_basename(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

int scp_is_source(const char *command) {
  if (!command || strncmp(command, "scp ", 4) != 0) {
    return 0;
  }
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", command);
  char *argv[64];
  int argc = scp_tokenize(tmp, argv, 64);
  if (argc < 2 || strcmp(argv[0], "scp") != 0) {
    return 0;
  }
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && strchr(argv[i], 'f')) {
      return 1;
    }
  }
  return 0;
}

int scp_source(ssh_channel channel, const char *command) {
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", command);
  char *argv[64];
  int argc = scp_tokenize(tmp, argv, 64);
  const char *src = NULL;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      src = argv[i]; /* last non-flag token is the source path */
    }
  }
  if (!src) {
    scp_fatal(channel, "scp: no source path");
    return 1;
  }

  char path[PATH_MAX];
  if (src[0] == '/') {
    snprintf(path, sizeof(path), "%s", src);
  } else {
    snprintf(path, sizeof(path), "/%s", src);
  }

  /* wait for the sink's go-ahead before sending anything */
  char go;
  if (!chan_read_byte(channel, &go) || go != 0) {
    return 1;
  }

  struct stat sb;
  if (stat(path, &sb) != 0) {
    scp_fatal(channel, "scp: no such file");
    return 1;
  }
  if (!S_ISREG(sb.st_mode)) {
    scp_fatal(channel,
              "scp: not a regular file (recursive download not supported yet)");
    return 1;
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    scp_fatal(channel, "scp: cannot open source file");
    return 1;
  }

  char hdr[300];
  int hn = snprintf(hdr, sizeof(hdr), "C%04o %lld %s\n",
                    (unsigned)(sb.st_mode & 0777), (long long)sb.st_size,
                    scp_basename(path));
  if (hn < 0 || chan_write_all(channel, hdr, (size_t)hn) != 0) {
    close(fd);
    return 1;
  }

  char ack;
  if (!chan_read_byte(channel, &ack) || ack != 0) {
    close(fd);
    return 1;
  }

  char buf[SCP_CHUNK];
  long long remaining = (long long)sb.st_size;
  int ok = 1;
  while (remaining > 0) {
    size_t want = remaining < (long long)sizeof(buf) ? (size_t)remaining
                                                     : sizeof(buf);
    ssize_t r = read(fd, buf, want);
    if (r <= 0) {
      ok = 0;
      break;
    }
    if (chan_write_all(channel, buf, (size_t)r) != 0) {
      ok = 0;
      break;
    }
    remaining -= r;
  }
  close(fd);
  if (!ok) {
    return 1;
  }

  scp_ack(channel); /* end-of-file marker */
  char fin;
  if (!chan_read_byte(channel, &fin) || fin != 0) {
    return 1;
  }
  return 0;
}

#else /* !PLATFORM_PS5 */

int scp_is_sink(const char *command) {
  (void)command;
  return 0;
}
int scp_sink(ssh_channel channel, const char *command) {
  (void)channel;
  (void)command;
  return 0;
}
int scp_is_source(const char *command) {
  (void)command;
  return 0;
}
int scp_source(ssh_channel channel, const char *command) {
  (void)channel;
  (void)command;
  return 0;
}

#endif
