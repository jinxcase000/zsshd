#include "scp.h"

#if defined(PLATFORM_PS5)

#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define SCP_CHUNK 32768 /* streaming block size; never buffer a whole file */
#define SCP_MAXDEPTH 64 /* max directory nesting for recursive scp */

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

/* Apply atime/mtime from a T line (best effort). */
static void scp_apply_times(const char *path, long atime, long mtime) {
  struct timeval tv[2];
  tv[0].tv_sec = (time_t)atime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = (time_t)mtime;
  tv[1].tv_usec = 0;
  (void)utimes(path, tv);
}

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
      /* -t (sink), -r (recursive) and -p (times) are all handled below */
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

  /* Directory stack for recursive uploads (D pushes, E pops). Heap-allocated
   * to keep the per-session thread stack small. */
  char(*dirstack)[PATH_MAX] = malloc(sizeof(*dirstack) * SCP_MAXDEPTH);
  if (!dirstack) {
    scp_fatal(channel, "scp: out of memory");
    return 1;
  }
  int depth = 0;
  char curdir[PATH_MAX];
  snprintf(curdir, sizeof(curdir), "%s", destbuf);
  int at_top = 1;

  /* Pending timestamps from a T line, applied to the next file or directory. */
  int have_time = 0;
  long t_atime = 0, t_mtime = 0;

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

    if (op == 'T') {
      long mt, mtu, at, atu;
      if (sscanf(line, "T%ld %ld %ld %ld", &mt, &mtu, &at, &atu) >= 3) {
        t_mtime = mt;
        t_atime = at;
        have_time = 1;
      }
      scp_ack(channel);
      continue;
    }

    if (op == 'D') {
      unsigned mode = 0;
      long long zero = 0;
      char name[256] = {0};
      if (sscanf(line, "D%o %lld %255[^\n]", &mode, &zero, name) < 3) {
        scp_fatal(channel, "scp: bad directory header");
        rc = 1;
        break;
      }
      char newdir[PATH_MAX];
      if (at_top && !dest_is_dir) {
        snprintf(newdir, sizeof(newdir), "%s", destbuf);
      } else {
        snprintf(newdir, sizeof(newdir), "%s/%s", curdir, name);
      }
      if (mkdir(newdir, mode ? (mode & 0777) : 0755) != 0 && errno != EEXIST) {
        scp_fatal(channel, "scp: cannot create directory");
        rc = 1;
        break;
      }
      if (have_time) {
        scp_apply_times(newdir, t_atime, t_mtime);
        have_time = 0;
      }
      if (depth < SCP_MAXDEPTH) {
        snprintf(dirstack[depth], PATH_MAX, "%s", curdir);
        depth++;
      }
      snprintf(curdir, sizeof(curdir), "%s", newdir);
      at_top = 0;
      scp_ack(channel);
      continue;
    }

    if (op == 'E') {
      if (depth > 0) {
        depth--;
        snprintf(curdir, sizeof(curdir), "%s", dirstack[depth]);
      }
      scp_ack(channel);
      continue;
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
    if (sscanf(line, "C%o %lld %255[^\n]", &mode, &size, name) != 3 ||
        size < 0) {
      scp_fatal(channel, "scp: bad file header");
      rc = 1;
      break;
    }

    char path[PATH_MAX];
    if (at_top && !dest_is_dir) {
      snprintf(path, sizeof(path), "%s", destbuf);
    } else {
      snprintf(path, sizeof(path), "%s/%s", curdir, name);
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
    if (have_time) {
      scp_apply_times(path, t_atime, t_mtime);
      have_time = 0;
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
  free(dirstack);
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

/* Send a control line and read the sink's ack (0 = ok). */
static int scp_send_ctrl(ssh_channel ch, const char *line) {
  if (chan_write_all(ch, line, strlen(line)) != 0) {
    return -1;
  }
  char ack;
  if (!chan_read_byte(ch, &ack) || ack != 0) {
    return -1;
  }
  return 0;
}

struct scp_send_ctx {
  ssh_channel ch;
  int preserve;  /* -p: emit T lines */
  int recursive; /* -r: descend directories */
  char *buf;     /* SCP_CHUNK streaming buffer (heap, shared) */
};

/* Send one path to the sink: a regular file, or (with -r) a directory tree as
 * D <contents> E. `depth` guards runaway recursion. 0 on success, -1 on
 * protocol/IO failure. */
static int scp_send_path(struct scp_send_ctx *c, const char *path,
                         const char *name, int depth) {
  struct stat sb;
  if (lstat(path, &sb) != 0) {
    scp_fatal(c->ch, "scp: cannot stat source");
    return -1;
  }

  if (S_ISDIR(sb.st_mode)) {
    if (!c->recursive) {
      scp_fatal(c->ch, "scp: is a directory (use -r)");
      return -1;
    }
    if (depth > SCP_MAXDEPTH) {
      return 0; /* too deep: skip */
    }
    char line[300];
    if (c->preserve) {
      snprintf(line, sizeof(line), "T%lld 0 %lld 0\n", (long long)sb.st_mtime,
               (long long)sb.st_atime);
      if (scp_send_ctrl(c->ch, line) != 0) {
        return -1;
      }
    }
    snprintf(line, sizeof(line), "D%04o 0 %s\n", (unsigned)(sb.st_mode & 0777),
             name);
    if (scp_send_ctrl(c->ch, line) != 0) {
      return -1;
    }
    DIR *d = opendir(path);
    int rc = 0;
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
          continue;
        }
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (scp_send_path(c, child, e->d_name, depth + 1) != 0) {
          rc = -1;
          break;
        }
      }
      closedir(d);
    }
    if (rc != 0) {
      return -1;
    }
    return scp_send_ctrl(c->ch, "E\n");
  }

  if (!S_ISREG(sb.st_mode)) {
    return 0; /* skip sockets/fifos/devices */
  }

  if (c->preserve) {
    char tline[80];
    snprintf(tline, sizeof(tline), "T%lld 0 %lld 0\n", (long long)sb.st_mtime,
             (long long)sb.st_atime);
    if (scp_send_ctrl(c->ch, tline) != 0) {
      return -1;
    }
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    scp_fatal(c->ch, "scp: cannot open source file");
    return -1;
  }
  char hdr[300];
  snprintf(hdr, sizeof(hdr), "C%04o %lld %s\n", (unsigned)(sb.st_mode & 0777),
           (long long)sb.st_size, name);
  if (scp_send_ctrl(c->ch, hdr) != 0) {
    close(fd);
    return -1;
  }
  long long remaining = (long long)sb.st_size;
  int ok = 1;
  while (remaining > 0) {
    size_t want = remaining < (long long)SCP_CHUNK ? (size_t)remaining
                                                   : (size_t)SCP_CHUNK;
    ssize_t r = read(fd, c->buf, want);
    if (r <= 0) {
      ok = 0;
      break;
    }
    if (chan_write_all(c->ch, c->buf, (size_t)r) != 0) {
      ok = 0;
      break;
    }
    remaining -= r;
  }
  close(fd);
  if (!ok) {
    return -1;
  }
  scp_ack(c->ch); /* end-of-file marker */
  char fin;
  if (!chan_read_byte(c->ch, &fin) || fin != 0) {
    return -1;
  }
  return 0;
}

int scp_source(ssh_channel channel, const char *command) {
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", command);
  char *argv[64];
  int argc = scp_tokenize(tmp, argv, 64);

  struct scp_send_ctx c;
  c.ch = channel;
  c.preserve = 0;
  c.recursive = 0;
  const char *src = NULL;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strchr(argv[i], 'p')) {
        c.preserve = 1;
      }
      if (strchr(argv[i], 'r')) {
        c.recursive = 1;
      }
    } else {
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

  c.buf = malloc(SCP_CHUNK);
  if (!c.buf) {
    scp_fatal(channel, "scp: out of memory");
    return 1;
  }
  int rc = scp_send_path(&c, path, scp_basename(path), 0);
  free(c.buf);
  return rc == 0 ? 0 : 1;
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
