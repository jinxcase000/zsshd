#include "shell.h"
#include "zsshd_version.h"

#if defined(PLATFORM_PS5)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <strings.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define MAX_ARGS 32
#define LINE_MAX_LEN 1024
#define HIST_MAX 64
#define HIST_LINE 256
#define LS_COLS 80

/* ANSI color codes */
#define C_RESET "\033[0m"
#define C_DIR "\033[1;34m"   /* bold blue   */
#define C_USER "\033[1;32m"  /* bold green  */
#define C_PATH "\033[1;36m"  /* bold cyan   */
#define C_ERR "\033[1;31m"   /* bold red    */

struct shell_ctx {
  ssh_channel channel;
  char cwd[PATH_MAX];
  char host[64];
  int running;
  int color;
  int interactive;
  int rows;
  int out_fd; /* >=0 => redirect command stdout to this fd */
  const char *in_buf; /* stdin source for < and pipes; NULL = none */
  size_t in_len;
  char (*history)[HIST_LINE];
  int hist_count;
  int hist_pos;
};

/* --- output helpers: translate bare \n to \r\n for the client PTY ------ */

static void cout(struct shell_ctx *ctx, const char *s, size_t len) {
  if (ctx->out_fd >= 0) {
    ssize_t wr = write(ctx->out_fd, s, len); /* file: raw, no CRLF xlate */
    (void)wr;
    return;
  }
  size_t start = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\n') {
      if (i > start) {
        ssh_channel_write(ctx->channel, s + start, (uint32_t)(i - start));
      }
      ssh_channel_write(ctx->channel, "\r\n", 2);
      start = i + 1;
    }
  }
  if (len > start) {
    ssh_channel_write(ctx->channel, s + start, (uint32_t)(len - start));
  }
}

static void cputs(struct shell_ctx *ctx, const char *s) {
  cout(ctx, s, strlen(s));
}

/* raw write, no newline translation (for escape sequences) */
static void craw(struct shell_ctx *ctx, const char *s) {
  if (ctx->out_fd >= 0) {
    ssize_t wr = write(ctx->out_fd, s, strlen(s));
    (void)wr;
    return;
  }
  ssh_channel_write(ctx->channel, s, (uint32_t)strlen(s));
}

/* write a colour escape only when colour output is enabled (off for scripts) */
static void ccolor(struct shell_ctx *ctx, const char *code) {
  if (ctx->color) {
    ssh_channel_write(ctx->channel, code, (uint32_t)strlen(code));
  }
}

static void cprintf(struct shell_ctx *ctx, const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    cout(ctx, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
  }
}

static void cerror(struct shell_ctx *ctx, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  int saved = ctx->out_fd;
  ctx->out_fd = -1; /* errors go to the terminal, not a redirect target */
  ccolor(ctx, C_ERR);
  cputs(ctx, buf);
  ccolor(ctx, C_RESET);
  ctx->out_fd = saved;
}

/* --- path handling ----------------------------------------------------- */

static void normalize_path(const char *in, char *out, size_t cap) {
  const char *parts[PATH_MAX / 2];
  int depth = 0;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", in);

  char *save = NULL;
  for (char *tok = strtok_r(tmp, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
      continue;
    }
    if (strcmp(tok, "..") == 0) {
      if (depth > 0) {
        depth--;
      }
      continue;
    }
    if (depth < (int)(sizeof(parts) / sizeof(parts[0]))) {
      parts[depth++] = tok;
    }
  }

  size_t pos = 0;
  out[pos++] = '/';
  out[pos] = '\0';
  for (int i = 0; i < depth; i++) {
    size_t need = strlen(parts[i]) + (i > 0 ? 1 : 0);
    if (pos + need + 1 >= cap) {
      break;
    }
    if (i > 0) {
      out[pos++] = '/';
    }
    strcpy(out + pos, parts[i]);
    pos += strlen(parts[i]);
  }
  out[pos] = '\0';
}

static void resolve_path(struct shell_ctx *ctx, const char *arg, char *out,
                         size_t cap) {
  char joined[PATH_MAX];
  if (!arg || arg[0] == '\0') {
    snprintf(joined, sizeof(joined), "%s", ctx->cwd);
  } else if (arg[0] == '/') {
    snprintf(joined, sizeof(joined), "%s", arg);
  } else {
    snprintf(joined, sizeof(joined), "%s/%s", ctx->cwd, arg);
  }
  normalize_path(joined, out, cap);
}

/* --- commands ---------------------------------------------------------- */

static int cmd_pwd(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  cprintf(ctx, "%s\n", ctx->cwd);
  return 0;
}

static int cmd_cd(int argc, char **argv, struct shell_ctx *ctx) {
  const char *target = (argc > 1) ? argv[1] : "/";
  char path[PATH_MAX];
  resolve_path(ctx, target, path, sizeof(path));
  struct stat sb;
  if (stat(path, &sb) != 0) {
    cerror(ctx, "cd: ");
    cprintf(ctx, "%s: no such file or directory\n", target);
    return 1;
  }
  if (!S_ISDIR(sb.st_mode)) {
    cerror(ctx, "cd: ");
    cprintf(ctx, "%s: not a directory\n", target);
    return 1;
  }
  snprintf(ctx->cwd, sizeof(ctx->cwd), "%s", path);
  return 0;
}

/* qsort comparator for directory entry names */
static int name_cmp(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

static int cmd_ls(int argc, char **argv, struct shell_ctx *ctx) {
  char path[PATH_MAX];
  resolve_path(ctx, (argc > 1) ? argv[1] : "", path, sizeof(path));

  struct stat sb;
  if (stat(path, &sb) != 0) {
    cerror(ctx, "ls: ");
    cprintf(ctx, "%s: no such file or directory\n",
            (argc > 1) ? argv[1] : ctx->cwd);
    return 1;
  }

  enum { MAX_ENTRIES = 1024 };
  char (*names)[256] = malloc(sizeof(*names) * MAX_ENTRIES);
  char *ptrs[MAX_ENTRIES];
  if (!names) {
    cerror(ctx, "ls: out of memory\n");
    return 1;
  }
  int count = 0;

  if (S_ISDIR(sb.st_mode)) {
    DIR *d = opendir(path);
    if (!d) {
      free(names);
      cerror(ctx, "ls: ");
      cprintf(ctx, "%s: cannot open\n", path);
      return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < MAX_ENTRIES) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
        continue;
      }
      snprintf(names[count], sizeof(names[count]), "%s", e->d_name);
      ptrs[count] = names[count];
      count++;
    }
    closedir(d);
  } else {
    snprintf(names[0], sizeof(names[0]), "%s", (argc > 1) ? argv[1] : path);
    ptrs[0] = names[0];
    count = 1;
  }

  qsort(ptrs, (size_t)count, sizeof(ptrs[0]), name_cmp);

  for (int i = 0; i < count; i++) {
    char child[PATH_MAX];
    if (S_ISDIR(sb.st_mode)) {
      snprintf(child, sizeof(child), "%s/%s", path, ptrs[i]);
    } else {
      snprintf(child, sizeof(child), "%s", path);
    }
    struct stat cs;
    if (stat(child, &cs) != 0) {
      memset(&cs, 0, sizeof(cs));
    }
    int isdir = S_ISDIR(cs.st_mode);
    int isexec = (!isdir && (cs.st_mode & 0111));

    /* permission string, eza-style: '.' for a regular file, 'd' for dir */
    char perms[11];
    perms[0] = isdir ? 'd' : (S_ISLNK(cs.st_mode) ? 'l' : '.');
    const char *rwx = "rwxrwxrwx";
    for (int b = 0; b < 9; b++) {
      perms[1 + b] = (cs.st_mode & (1 << (8 - b))) ? rwx[b] : '-';
    }
    perms[10] = '\0';

    /* human-readable size; '-' for directories */
    char sizebuf[8];
    if (isdir) {
      snprintf(sizebuf, sizeof(sizebuf), "-");
    } else {
      double v = (double)cs.st_size;
      const char *units = "kMGT";
      int u = -1;
      while (v >= 1000.0 && u < 3) {
        v /= 1024.0;
        u++;
      }
      if (u < 0) {
        snprintf(sizebuf, sizeof(sizebuf), "%.0f", v);
      } else {
        snprintf(sizebuf, sizeof(sizebuf), v < 10.0 ? "%.1f%c" : "%.0f%c", v,
                 units[u]);
      }
    }

    char owner[16];
    if (cs.st_uid == 0) {
      snprintf(owner, sizeof(owner), "root");
    } else {
      snprintf(owner, sizeof(owner), "%u", (unsigned)cs.st_uid);
    }

    char datebuf[16] = "";
    time_t t = (time_t)cs.st_mtime;
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
      strftime(datebuf, sizeof(datebuf), "%e %b %H:%M", &tmv);
    }

    char head[128];
    snprintf(head, sizeof(head), "%s %4s %-5s %s ", perms, sizebuf, owner,
             datebuf);
    craw(ctx, head);

    /* Nerd Font glyph: folder (U+F07B) vs file (U+F15B). */
    craw(ctx, isdir ? "\xEF\x81\xBB" : "\xEF\x85\x9B");
    craw(ctx, " ");

    if (isdir) {
      ccolor(ctx, C_DIR);
    } else if (isexec) {
      ccolor(ctx, C_USER);
    }
    craw(ctx, ptrs[i]);
    if (isdir || isexec) {
      ccolor(ctx, C_RESET);
    }
    craw(ctx, "\r\n");
  }

  free(names);
  return 0;
}

/* --- input streams (for '<' redirection and pipes) -------------------- */

/* A read cursor over either a real file or an in-memory buffer (stdin). */
struct instream {
  FILE *fp;
  const char *buf;
  size_t len;
  size_t pos;
};

static void in_from_file(struct instream *in, FILE *fp) {
  in->fp = fp;
  in->buf = NULL;
  in->len = 0;
  in->pos = 0;
}

static void in_from_buf(struct instream *in, const char *buf, size_t len) {
  in->fp = NULL;
  in->buf = buf;
  in->len = len;
  in->pos = 0;
}

/* fread-style block read. */
static size_t in_read(struct instream *in, char *dst, size_t cap) {
  if (in->fp) {
    return fread(dst, 1, cap, in->fp);
  }
  size_t avail = in->len - in->pos;
  size_t n = avail < cap ? avail : cap;
  memcpy(dst, in->buf + in->pos, n);
  in->pos += n;
  return n;
}

/* fgets-style: one line incl. its trailing newline, NUL-terminated. Returns 1
 * if a line was read, 0 at EOF. */
static int in_gets(struct instream *in, char *dst, size_t cap) {
  if (cap == 0) {
    return 0;
  }
  if (in->fp) {
    return fgets(dst, (int)cap, in->fp) != NULL;
  }
  if (in->pos >= in->len) {
    return 0;
  }
  size_t i = 0;
  while (i + 1 < cap && in->pos < in->len) {
    char c = in->buf[in->pos++];
    dst[i++] = c;
    if (c == '\n') {
      break;
    }
  }
  dst[i] = '\0';
  return 1;
}

/* Read a whole file into a malloc'd buffer (caller frees); NULL on error. */
static char *read_file_all(const char *path, size_t *outlen) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 8192, len = 0;
  char *b = malloc(cap);
  if (!b) {
    fclose(f);
    return NULL;
  }
  size_t n;
  while ((n = fread(b + len, 1, cap - len, f)) > 0) {
    len += n;
    if (len == cap) {
      size_t ncap = cap * 2;
      char *nb = realloc(b, ncap);
      if (!nb) {
        free(b);
        fclose(f);
        return NULL;
      }
      b = nb;
      cap = ncap;
    }
  }
  fclose(f);
  *outlen = len;
  return b;
}

/* --- commands (continued) --------------------------------------------- */

static int cmd_cat(int argc, char **argv, struct shell_ctx *ctx) {
  char buf[4096];
  size_t n;
  if (argc < 2) {
    if (!ctx->in_buf) {
      cerror(ctx, "cat: missing file operand\n");
      return 1;
    }
    struct instream in;
    in_from_buf(&in, ctx->in_buf, ctx->in_len);
    while ((n = in_read(&in, buf, sizeof(buf))) > 0) {
      cout(ctx, buf, n);
    }
    return 0;
  }
  int rc = 0;
  for (int a = 1; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
      cerror(ctx, "cat: ");
      cprintf(ctx, "%s: cannot open\n", argv[a]);
      rc = 1;
      continue;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      cout(ctx, buf, n);
    }
    fclose(f);
  }
  return rc;
}

static int cmd_echo(int argc, char **argv, struct shell_ctx *ctx) {
  for (int a = 1; a < argc; a++) {
    cputs(ctx, argv[a]);
    if (a + 1 < argc) {
      cputs(ctx, " ");
    }
  }
  cputs(ctx, "\n");
  return 0;
}

static int cmd_whoami(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  cputs(ctx, "root\n");
  return 0;
}

static int cmd_uname(int argc, char **argv, struct shell_ctx *ctx) {
  struct utsname u;
  if (uname(&u) != 0) {
    cerror(ctx, "uname: cannot get system information\n");
    return 1;
  }
  int all = (argc > 1 && strcmp(argv[1], "-a") == 0);
  if (all) {
    cprintf(ctx, "%s %s %s %s %s\n", u.sysname, u.nodename, u.release,
            u.version, u.machine);
  } else {
    cprintf(ctx, "%s\n", u.sysname);
  }
  return 0;
}

static int cmd_clear(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  craw(ctx, "\033[2J\033[H");
  return 0;
}

static int cmd_help(int argc, char **argv, struct shell_ctx *ctx);
static int cmd_exit(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  ctx->running = 0;
  return 0;
}

/* --- filesystem helpers (stage 2b) ------------------------------------ */

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) {
    return -1;
  }
  struct stat st;
  mode_t mode = 0644;
  if (fstat(in, &st) == 0) {
    mode = st.st_mode & 0777;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (out < 0) {
    close(in);
    return -1;
  }
  char buf[8192];
  ssize_t r;
  int rc = 0;
  while ((r = read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = write(out, buf + off, (size_t)(r - off));
      if (w <= 0) {
        rc = -1;
        break;
      }
      off += w;
    }
    if (rc) {
      break;
    }
  }
  if (r < 0) {
    rc = -1;
  }
  close(in);
  close(out);
  return rc;
}

static int copy_recursive(const char *src, const char *dst) {
  struct stat st;
  if (stat(src, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    return copy_file(src, dst);
  }
  if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  DIR *d = opendir(src);
  if (!d) {
    return -1;
  }
  struct dirent *e;
  int rc = 0;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    char s2[PATH_MAX], d2[PATH_MAX];
    snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
    snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
    if (copy_recursive(s2, d2) != 0) {
      rc = -1;
    }
  }
  closedir(d);
  return rc;
}

static int remove_recursive(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    return unlink(path);
  }
  DIR *d = opendir(path);
  if (!d) {
    return -1;
  }
  struct dirent *e;
  int rc = 0;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    char c[PATH_MAX];
    snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
    if (remove_recursive(c) != 0) {
      rc = -1;
    }
  }
  closedir(d);
  if (rmdir(path) != 0) {
    rc = -1;
  }
  return rc;
}

static int mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

/* --- filesystem commands ---------------------------------------------- */

static int cmd_mkdir(int argc, char **argv, struct shell_ctx *ctx) {
  int parents = 0, start = 1, rc = 0;
  if (argc > 1 && strcmp(argv[1], "-p") == 0) {
    parents = 1;
    start = 2;
  }
  if (argc <= start) {
    cerror(ctx, "mkdir: missing operand\n");
    return 1;
  }
  for (int a = start; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    int r = parents ? mkdir_p(path) : mkdir(path, 0755);
    if (r != 0) {
      cerror(ctx, "mkdir: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}

static int cmd_rmdir(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc < 2) {
    cerror(ctx, "rmdir: missing operand\n");
    return 1;
  }
  int rc = 0;
  for (int a = 1; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    if (rmdir(path) != 0) {
      cerror(ctx, "rmdir: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}

static int cmd_touch(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc < 2) {
    cerror(ctx, "touch: missing operand\n");
    return 1;
  }
  int rc = 0;
  for (int a = 1; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
      cerror(ctx, "touch: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
      continue;
    }
    close(fd);
    (void)utimensat(AT_FDCWD, path, NULL, 0);
  }
  return rc;
}

static int cmd_rm(int argc, char **argv, struct shell_ctx *ctx) {
  int recursive = 0, force = 0, start = 1, rc = 0;
  while (start < argc && argv[start][0] == '-' && argv[start][1] != '\0') {
    for (const char *o = argv[start] + 1; *o; o++) {
      if (*o == 'r' || *o == 'R') {
        recursive = 1;
      } else if (*o == 'f') {
        force = 1;
      } else {
        cerror(ctx, "rm: unknown option\n");
        return 1;
      }
    }
    start++;
  }
  if (start >= argc) {
    if (force) {
      return 0;
    }
    cerror(ctx, "rm: missing operand\n");
    return 1;
  }
  for (int a = start; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
      if (!force) {
        cerror(ctx, "rm: ");
        cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
        rc = 1;
      }
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!recursive) {
        cerror(ctx, "rm: ");
        cprintf(ctx, "%s: is a directory\n", argv[a]);
        rc = 1;
        continue;
      }
      if (remove_recursive(path) != 0 && !force) {
        cerror(ctx, "rm: ");
        cprintf(ctx, "%s: failed to remove\n", argv[a]);
        rc = 1;
      }
    } else {
      if (unlink(path) != 0 && !force) {
        cerror(ctx, "rm: ");
        cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
        rc = 1;
      }
    }
  }
  return rc;
}

static int cmd_cp(int argc, char **argv, struct shell_ctx *ctx) {
  int recursive = 0, start = 1;
  while (start < argc && argv[start][0] == '-' && argv[start][1] != '\0') {
    for (const char *o = argv[start] + 1; *o; o++) {
      if (*o == 'r' || *o == 'R') {
        recursive = 1;
      } else {
        cerror(ctx, "cp: unknown option\n");
        return 1;
      }
    }
    start++;
  }
  if (argc - start < 2) {
    cerror(ctx, "cp: need source and destination\n");
    return 1;
  }
  char dst[PATH_MAX];
  resolve_path(ctx, argv[argc - 1], dst, sizeof(dst));
  struct stat dstat;
  int dst_is_dir = (stat(dst, &dstat) == 0 && S_ISDIR(dstat.st_mode));
  if ((argc - 1 - start) > 1 && !dst_is_dir) {
    cerror(ctx, "cp: destination must be a directory\n");
    return 1;
  }
  int rc = 0;
  for (int a = start; a < argc - 1; a++) {
    char src[PATH_MAX];
    resolve_path(ctx, argv[a], src, sizeof(src));
    struct stat st;
    if (stat(src, &st) != 0) {
      cerror(ctx, "cp: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
      continue;
    }
    char target[PATH_MAX];
    if (dst_is_dir) {
      snprintf(target, sizeof(target), "%s/%s", dst, base_name(src));
    } else {
      snprintf(target, sizeof(target), "%s", dst);
    }
    if (S_ISDIR(st.st_mode)) {
      if (!recursive) {
        cerror(ctx, "cp: ");
        cprintf(ctx, "%s: is a directory (use -r)\n", argv[a]);
        rc = 1;
        continue;
      }
      if (copy_recursive(src, target) != 0) {
        cerror(ctx, "cp: ");
        cprintf(ctx, "%s: copy failed\n", argv[a]);
        rc = 1;
      }
    } else {
      if (copy_file(src, target) != 0) {
        cerror(ctx, "cp: ");
        cprintf(ctx, "%s: copy failed\n", argv[a]);
        rc = 1;
      }
    }
  }
  return rc;
}

static int cmd_mv(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc < 3) {
    cerror(ctx, "mv: need source and destination\n");
    return 1;
  }
  char dst[PATH_MAX];
  resolve_path(ctx, argv[argc - 1], dst, sizeof(dst));
  struct stat dstat;
  int dst_is_dir = (stat(dst, &dstat) == 0 && S_ISDIR(dstat.st_mode));
  if ((argc - 2) > 1 && !dst_is_dir) {
    cerror(ctx, "mv: destination must be a directory\n");
    return 1;
  }
  int rc = 0;
  for (int a = 1; a < argc - 1; a++) {
    char src[PATH_MAX];
    resolve_path(ctx, argv[a], src, sizeof(src));
    char target[PATH_MAX];
    if (dst_is_dir) {
      snprintf(target, sizeof(target), "%s/%s", dst, base_name(src));
    } else {
      snprintf(target, sizeof(target), "%s", dst);
    }
    if (rename(src, target) == 0) {
      continue;
    }
    if (errno == EXDEV) {
      /* cross-device move: copy then remove the source */
      struct stat st;
      if (stat(src, &st) != 0) {
        cerror(ctx, "mv: ");
        cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
        rc = 1;
        continue;
      }
      int cr = S_ISDIR(st.st_mode) ? copy_recursive(src, target)
                                   : copy_file(src, target);
      if (cr != 0 || remove_recursive(src) != 0) {
        cerror(ctx, "mv: ");
        cprintf(ctx, "%s: move failed\n", argv[a]);
        rc = 1;
      }
    } else {
      cerror(ctx, "mv: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}

/* --- text commands (stage 2c) ----------------------------------------- */

static int head_tail_nlines(int argc, char **argv, int *start) {
  int nlines = 10;
  *start = 1;
  if (argc > 2 && strcmp(argv[1], "-n") == 0) {
    nlines = atoi(argv[2]);
    *start = 3;
  }
  if (nlines < 0) {
    nlines = 0;
  }
  return nlines;
}

static void head_stream(struct shell_ctx *ctx, struct instream *in,
                        int nlines) {
  char line[4096];
  int count = 0;
  while (count < nlines && in_gets(in, line, sizeof(line))) {
    cout(ctx, line, strlen(line));
    count++;
  }
}

static int cmd_head(int argc, char **argv, struct shell_ctx *ctx) {
  int start;
  int nlines = head_tail_nlines(argc, argv, &start);
  if (start >= argc) {
    if (!ctx->in_buf) {
      cerror(ctx, "head: missing file operand\n");
      return 1;
    }
    struct instream in;
    in_from_buf(&in, ctx->in_buf, ctx->in_len);
    head_stream(ctx, &in, nlines);
    return 0;
  }
  int multiple = (argc - start) > 1, rc = 0;
  for (int a = start; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      cerror(ctx, "head: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
      continue;
    }
    if (multiple) {
      cprintf(ctx, "==> %s <==\n", argv[a]);
    }
    struct instream in;
    in_from_file(&in, f);
    head_stream(ctx, &in, nlines);
    fclose(f);
    if (multiple && a + 1 < argc) {
      cputs(ctx, "\n");
    }
  }
  return rc;
}

static void tail_stream(struct shell_ctx *ctx, struct instream *in,
                        int nlines) {
  if (nlines <= 0) {
    return;
  }
  char **ring = calloc((size_t)nlines, sizeof(char *));
  if (!ring) {
    cerror(ctx, "tail: out of memory\n");
    return;
  }
  char line[4096];
  int idx = 0, filled = 0;
  while (in_gets(in, line, sizeof(line))) {
    free(ring[idx]);
    ring[idx] = strdup(line);
    idx = (idx + 1) % nlines;
    if (filled < nlines) {
      filled++;
    }
  }
  int begin = (filled < nlines) ? 0 : idx;
  for (int i = 0; i < filled; i++) {
    char *ln = ring[(begin + i) % nlines];
    if (ln) {
      cout(ctx, ln, strlen(ln));
    }
  }
  for (int i = 0; i < nlines; i++) {
    free(ring[i]);
  }
  free(ring);
}

static int cmd_tail(int argc, char **argv, struct shell_ctx *ctx) {
  int start;
  int nlines = head_tail_nlines(argc, argv, &start);
  if (start >= argc) {
    if (!ctx->in_buf) {
      cerror(ctx, "tail: missing file operand\n");
      return 1;
    }
    struct instream in;
    in_from_buf(&in, ctx->in_buf, ctx->in_len);
    tail_stream(ctx, &in, nlines);
    return 0;
  }
  int multiple = (argc - start) > 1, rc = 0;
  for (int a = start; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      cerror(ctx, "tail: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      rc = 1;
      continue;
    }
    if (multiple) {
      cprintf(ctx, "==> %s <==\n", argv[a]);
    }
    struct instream in;
    in_from_file(&in, f);
    tail_stream(ctx, &in, nlines);
    fclose(f);
    if (multiple && a + 1 < argc) {
      cputs(ctx, "\n");
    }
  }
  return rc;
}

static int grep_stream(struct shell_ctx *ctx, const regex_t *re,
                       struct instream *in, const char *label) {
  char line[4096];
  int matched = 0;
  while (in_gets(in, line, sizeof(line))) {
    size_t L = strlen(line);
    if (L > 0 && line[L - 1] == '\n') {
      line[L - 1] = '\0';
    }
    if (regexec(re, line, 0, NULL, 0) == 0) {
      if (label) {
        ccolor(ctx, C_PATH);
        craw(ctx, label);
        ccolor(ctx, C_RESET);
        craw(ctx, ":");
      }
      cputs(ctx, line);
      cputs(ctx, "\n");
      matched = 1;
    }
  }
  return matched;
}

static int cmd_grep(int argc, char **argv, struct shell_ctx *ctx) {
  int cflags = REG_EXTENDED | REG_NOSUB, start = 1;
  while (start < argc && argv[start][0] == '-' && argv[start][1] != '\0') {
    for (const char *o = argv[start] + 1; *o; o++) {
      if (*o == 'i') {
        cflags |= REG_ICASE;
      } else {
        cerror(ctx, "grep: unknown option\n");
        return 1;
      }
    }
    start++;
  }
  if (start >= argc) {
    cerror(ctx, "grep: usage: grep [-i] pattern [file...]\n");
    return 1;
  }
  const char *pat = argv[start++];
  regex_t re;
  if (regcomp(&re, pat, cflags) != 0) {
    cerror(ctx, "grep: invalid pattern\n");
    return 1;
  }
  int nfiles = argc - start, matched = 0;
  if (nfiles == 0) {
    if (!ctx->in_buf) {
      regfree(&re);
      cerror(ctx, "grep: no input\n");
      return 1;
    }
    struct instream in;
    in_from_buf(&in, ctx->in_buf, ctx->in_len);
    matched = grep_stream(ctx, &re, &in, NULL);
  } else {
    int multiple = nfiles > 1;
    for (int a = start; a < argc; a++) {
      char path[PATH_MAX];
      resolve_path(ctx, argv[a], path, sizeof(path));
      FILE *f = fopen(path, "r");
      if (!f) {
        cerror(ctx, "grep: ");
        cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
        continue;
      }
      struct instream in;
      in_from_file(&in, f);
      if (grep_stream(ctx, &re, &in, multiple ? argv[a] : NULL)) {
        matched = 1;
      }
      fclose(f);
    }
  }
  regfree(&re);
  return matched ? 0 : 1;
}

/* 'more'-style pager for `less`/`more`. Paginates only with a TTY; when the
 * session is scripted/piped it behaves like cat (matching real less). */
static int cmd_pager(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc < 2) {
    cerror(ctx, "less: missing file operand\n");
    return 1;
  }
  int rc = 0;
  for (int a = 1; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      cerror(ctx, "less: ");
      cprintf(ctx, "%s: cannot open\n", argv[a]);
      rc = 1;
      continue;
    }
    if (!ctx->interactive || ctx->out_fd >= 0) {
      /* no TTY (or redirected): dump like cat */
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        cout(ctx, buf, n);
      }
      fclose(f);
      continue;
    }
    int rows = ctx->rows > 1 ? ctx->rows : 24;
    int page = rows - 1; /* leave a row for the --More-- prompt */
    if (page < 1) {
      page = 1;
    }
    char line[4096];
    int shown = 0;
    int quit = 0;
    while (!quit && fgets(line, sizeof(line), f)) {
      size_t L = strlen(line);
      cout(ctx, line, L);
      if (L == 0 || line[L - 1] != '\n') {
        cout(ctx, "\n", 1);
      }
      shown++;
      if (shown >= page) {
        craw(ctx, "\033[7m--More--\033[0m"); /* reverse-video prompt */
        char k;
        int kn = ssh_channel_read(ctx->channel, &k, 1, 0);
        craw(ctx, "\r\033[K"); /* erase the prompt line */
        if (kn <= 0 || k == 'q' || k == 'Q') {
          quit = 1;
        } else if (k == '\r' || k == '\n') {
          shown = page - 1; /* advance one line */
        } else {
          shown = 0; /* space / anything else: next full page */
        }
      }
    }
    fclose(f);
    if (quit) {
      break;
    }
  }
  return rc;
}

/* --- process commands (ps / kill) ------------------------------------- */

/* kinfo_proc field offsets for the console's process records, mirroring the
 * layout instance_control.c already relies on (KINFO_PID_OFFSET /
 * KINFO_THREAD_OFFSET). Raw offsets are used instead of struct kinfo_proc
 * because the SDK header layout does not match the console's. */
#define PROC_PID_OFFSET 72U
#define PROC_NAME_OFFSET 447U
#define PROC_NAME_SIZE 20U

static int cmd_ps(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  int mib[4] = {1, 14, 8, 0}; /* CTL_KERN, KERN_PROC, KERN_PROC_PROC */
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
    cerror(ctx, "ps: cannot query process list\n");
    return 1;
  }
  unsigned char *buf = malloc(size);
  if (!buf) {
    cerror(ctx, "ps: out of memory\n");
    return 1;
  }
  if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    cerror(ctx, "ps: cannot read process list\n");
    return 1;
  }
  cprintf(ctx, "%6s  %s\n", "PID", "NAME");
  for (size_t off = 0; off + PROC_NAME_OFFSET + PROC_NAME_SIZE <= size;) {
    int rec = 0;
    memcpy(&rec, buf + off, sizeof(rec));
    if (rec <= 0 || off + (size_t)rec > size ||
        (size_t)rec <= PROC_NAME_OFFSET + PROC_NAME_SIZE) {
      break;
    }
    pid_t pid = -1;
    memcpy(&pid, buf + off + PROC_PID_OFFSET, sizeof(pid));
    char name[PROC_NAME_SIZE + 1];
    memcpy(name, buf + off + PROC_NAME_OFFSET, PROC_NAME_SIZE);
    name[PROC_NAME_SIZE] = '\0';
    if (pid > 0) {
      cprintf(ctx, "%6d  %s\n", (int)pid, name);
    }
    off += (size_t)rec;
  }
  free(buf);
  return 0;
}

struct sig_entry {
  const char *name;
  int sig;
};

static const struct sig_entry SIGNALS[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
    {"ILL", SIGILL},   {"ABRT", SIGABRT}, {"FPE", SIGFPE},
    {"KILL", SIGKILL}, {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},
    {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"USR1", SIGUSR1},
    {"USR2", SIGUSR2}, {"STOP", SIGSTOP}, {"CONT", SIGCONT},
};
static const int N_SIGNALS = (int)(sizeof(SIGNALS) / sizeof(SIGNALS[0]));

/* Resolve a signal token (without the leading '-'): a number, a bare name, or
 * a name with the SIG prefix. Returns -1 if unrecognised. */
static int sig_from_token(const char *t) {
  if (t[0] >= '0' && t[0] <= '9') {
    return atoi(t);
  }
  if (strncasecmp(t, "SIG", 3) == 0) {
    t += 3;
  }
  for (int i = 0; i < N_SIGNALS; i++) {
    if (strcasecmp(t, SIGNALS[i].name) == 0) {
      return SIGNALS[i].sig;
    }
  }
  return -1;
}

static int cmd_kill(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc > 1 && strcmp(argv[1], "-l") == 0) {
    for (int i = 0; i < N_SIGNALS; i++) {
      cprintf(ctx, "%2d) SIG%-6s%s", SIGNALS[i].sig, SIGNALS[i].name,
              (i % 4 == 3 || i == N_SIGNALS - 1) ? "\n" : " ");
    }
    return 0;
  }
  int sig = SIGTERM;
  int start = 1;
  if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
    const char *spec;
    if (strcmp(argv[1], "-s") == 0) {
      if (argc < 3) {
        cerror(ctx, "kill: option requires an argument -- 's'\n");
        return 1;
      }
      spec = argv[2];
      start = 3;
    } else {
      spec = argv[1] + 1;
      start = 2;
    }
    sig = sig_from_token(spec);
    if (sig < 0) {
      cerror(ctx, "kill: ");
      cprintf(ctx, "%s: invalid signal\n", spec);
      return 1;
    }
  }
  if (start >= argc) {
    cerror(ctx, "kill: usage: kill [-s signal | -signal] pid...\n");
    return 1;
  }
  int rc = 0;
  for (int a = start; a < argc; a++) {
    char *end = NULL;
    long pid = strtol(argv[a], &end, 10);
    if (end == argv[a] || *end != '\0') {
      cerror(ctx, "kill: ");
      cprintf(ctx, "%s: arguments must be process IDs\n", argv[a]);
      rc = 1;
      continue;
    }
    if (kill((pid_t)pid, sig) != 0) {
      cerror(ctx, "kill: ");
      cprintf(ctx, "(%ld): %s\n", pid, strerror(errno));
      rc = 1;
    }
  }
  return rc;
}

/* --- dispatch table ---------------------------------------------------- */

struct command {
  const char *name;
  int (*fn)(int, char **, struct shell_ctx *);
};

static const struct command COMMANDS[] = {
    {"pwd", cmd_pwd},     {"cd", cmd_cd},       {"ls", cmd_ls},
    {"cat", cmd_cat},     {"echo", cmd_echo},   {"whoami", cmd_whoami},
    {"uname", cmd_uname}, {"clear", cmd_clear}, {"help", cmd_help},
    {"mkdir", cmd_mkdir}, {"rmdir", cmd_rmdir}, {"touch", cmd_touch},
    {"rm", cmd_rm},       {"cp", cmd_cp},       {"mv", cmd_mv},
    {"head", cmd_head},   {"tail", cmd_tail},   {"grep", cmd_grep},
    {"less", cmd_pager},  {"more", cmd_pager},
    {"ps", cmd_ps},       {"kill", cmd_kill},
    {"exit", cmd_exit},   {"quit", cmd_exit},
};
static const int N_COMMANDS = (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]));

static int cmd_help(int argc, char **argv, struct shell_ctx *ctx) {
  (void)argc;
  (void)argv;
  cputs(ctx, "built-in commands:\n");
  for (int i = 0; i < N_COMMANDS; i++) {
    cprintf(ctx, "  %s\n", COMMANDS[i].name);
  }
  return 0;
}

/* --- line parsing + loop ----------------------------------------------- */

static int tokenize(char *line, char **argv, int max) {
  int argc = 0;
  char *p = line;
  while (*p && argc < max) {
    while (*p == ' ' || *p == '\t') {
      *p++ = '\0';
    }
    if (*p == '\0') {
      break;
    }
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') {
      p++;
    }
  }
  return argc;
}

struct redir {
  const char *out_path; /* non-NULL for > or >> */
  int append;           /* 1 for >>, 0 for > */
  const char *in_path;  /* non-NULL for < */
};

/* Strip '>' '>>' (output) and '<' (input) redirection tokens from argv,
 * filling *r. Each operator may be glued to its filename ('>f') or separate
 * ('>' 'f'). Returns 0 on success, -1 on an operator with no filename. */
static int parse_redirect(char **argv, int *argc, struct redir *r) {
  r->out_path = NULL;
  r->append = 0;
  r->in_path = NULL;
  int w = 0;
  for (int i = 0; i < *argc; i++) {
    char *t = argv[i];
    if (t[0] == '>') {
      int app = (t[1] == '>');
      const char *fname = t + (app ? 2 : 1);
      if (*fname == '\0') {
        if (i + 1 >= *argc) {
          return -1;
        }
        fname = argv[++i];
      }
      r->out_path = fname;
      r->append = app;
    } else if (t[0] == '<') {
      const char *fname = t + 1;
      if (*fname == '\0') {
        if (i + 1 >= *argc) {
          return -1;
        }
        fname = argv[++i];
      }
      r->in_path = fname;
    } else {
      argv[w++] = t;
    }
  }
  *argc = w;
  return 0;
}

static int dispatch_one(struct shell_ctx *ctx, char *line) {
  char *argv[MAX_ARGS];
  int argc = tokenize(line, argv, MAX_ARGS);
  if (argc == 0) {
    return 0;
  }

  struct redir r;
  if (parse_redirect(argv, &argc, &r) != 0) {
    cerror(ctx, "syntax error: expected filename after redirection\n");
    return 1;
  }
  if (argc == 0) {
    cerror(ctx, "syntax error: missing command before redirection\n");
    return 1;
  }

  const char *saved_in_buf = ctx->in_buf;
  size_t saved_in_len = ctx->in_len;
  char *in_data = NULL;
  int redirected_out = 0, saved_color = ctx->color;

  if (r.in_path) {
    char path[PATH_MAX];
    resolve_path(ctx, r.in_path, path, sizeof(path));
    size_t len = 0;
    in_data = read_file_all(path, &len);
    if (!in_data) {
      cerror(ctx, "cannot open ");
      cprintf(ctx, "%s: %s\n", r.in_path, strerror(errno));
      return 1;
    }
    ctx->in_buf = in_data;
    ctx->in_len = len;
  }

  if (r.out_path) {
    char path[PATH_MAX];
    resolve_path(ctx, r.out_path, path, sizeof(path));
    int flags = O_WRONLY | O_CREAT | (r.append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0) {
      cerror(ctx, "cannot open ");
      cprintf(ctx, "%s: %s\n", r.out_path, strerror(errno));
      if (in_data) {
        free(in_data);
        ctx->in_buf = saved_in_buf;
        ctx->in_len = saved_in_len;
      }
      return 1;
    }
    ctx->out_fd = fd;
    ctx->color = 0; /* no colour escapes in a file */
    redirected_out = 1;
  }

  int status = 127, found = 0;
  for (int i = 0; i < N_COMMANDS; i++) {
    if (strcmp(argv[0], COMMANDS[i].name) == 0) {
      status = COMMANDS[i].fn(argc, argv, ctx);
      found = 1;
      break;
    }
  }
  if (!found) {
    cerror(ctx, "%s: command not found\n", argv[0]);
    status = 127;
  }

  if (redirected_out) {
    close(ctx->out_fd);
    ctx->out_fd = -1;
    ctx->color = saved_color;
  }
  if (in_data) {
    free(in_data);
    ctx->in_buf = saved_in_buf;
    ctx->in_len = saved_in_len;
  }
  return status;
}

/* Run one or more commands separated by ';' or newlines. Returns the exit
 * status of the last command executed. */
/* Run a pipeline cmds[0] | cmds[1] | ... Each non-final stage's output is
 * captured to a temp file and fed as the next stage's stdin; the final stage
 * writes normally. Returns the last stage's exit status. */
static int run_pipeline(struct shell_ctx *ctx, char **cmds, int n) {
  const char *saved_in = ctx->in_buf;
  size_t saved_inlen = ctx->in_len;
  char *carry = NULL;
  size_t carry_len = 0;
  int status = 0;
  for (int i = 0; i < n && ctx->running; i++) {
    if (i > 0) {
      ctx->in_buf = carry;
      ctx->in_len = carry_len;
    }
    if (i < n - 1) {
      char tmp[PATH_MAX];
      snprintf(tmp, sizeof(tmp), "/data/zsshd/.pipe.%lx.%d",
               (unsigned long)(uintptr_t)ctx, i);
      char linebuf[LINE_MAX_LEN];
      snprintf(linebuf, sizeof(linebuf), "%s > %s", cmds[i], tmp);
      status = dispatch_one(ctx, linebuf);
      free(carry);
      carry = read_file_all(tmp, &carry_len);
      (void)unlink(tmp);
      if (!carry) {
        carry = malloc(1);
        carry_len = 0;
      }
    } else {
      status = dispatch_one(ctx, cmds[i]);
    }
  }
  free(carry);
  ctx->in_buf = saved_in;
  ctx->in_len = saved_inlen;
  return status;
}

/* Split a command segment on top-level '|'; run it as a pipeline, or as a
 * single command when there is no pipe. */
static int run_segment(struct shell_ctx *ctx, char *seg) {
  char *cmds[MAX_ARGS];
  int n = 0;
  char *start = seg;
  for (char *p = seg;; p++) {
    if (*p == '|' || *p == '\0') {
      char end = *p;
      *p = '\0';
      if (n < MAX_ARGS) {
        cmds[n++] = start;
      }
      if (end == '\0') {
        break;
      }
      start = p + 1;
    }
  }
  if (n <= 1) {
    return dispatch_one(ctx, seg);
  }
  return run_pipeline(ctx, cmds, n);
}

static int run_commands(struct shell_ctx *ctx, const char *text) {
  char work[LINE_MAX_LEN];
  snprintf(work, sizeof(work), "%s", text);
  int status = 0;
  char *seg = work;
  for (char *p = work;; p++) {
    if (*p == ';' || *p == '\n' || *p == '\0') {
      char end = *p;
      *p = '\0';
      status = run_segment(ctx, seg);
      if (end == '\0' || !ctx->running) {
        break;
      }
      seg = p + 1;
    }
  }
  return status;
}

/* Scripted/non-interactive session: no banner, prompt, echo or colour. Read
 * lines from the client and execute them until EOF (the client closes stdin). */
static void run_batch(struct shell_ctx *ctx) {
  char line[LINE_MAX_LEN];
  size_t len = 0;
  char buf[512];
  for (;;) {
    int n = ssh_channel_read(ctx->channel, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    for (int i = 0; i < n && ctx->running; i++) {
      char c = buf[i];
      if (c == '\n' || c == '\r') {
        line[len] = '\0';
        if (len > 0) {
          run_commands(ctx, line);
        }
        len = 0;
      } else if (len + 1 < sizeof(line)) {
        line[len++] = c;
      }
    }
    if (!ctx->running) {
      break;
    }
  }
  if (ctx->running && len > 0) {
    line[len] = '\0';
    run_commands(ctx, line);
  }
}

/* Erase the currently typed line on screen and replace it with `repl`,
 * leaving the cursor at the end. */
static void replace_line(struct shell_ctx *ctx, char *line, size_t *len,
                         size_t *pos, const char *repl) {
  /* move the cursor to the end of the current content first */
  if (*pos < *len) {
    ssh_channel_write(ctx->channel, line + *pos, (uint32_t)(*len - *pos));
  }
  for (size_t i = 0; i < *len; i++) {
    craw(ctx, "\b \b");
  }
  snprintf(line, LINE_MAX_LEN, "%s", repl);
  *len = strlen(line);
  *pos = *len;
  ssh_channel_write(ctx->channel, line, (uint32_t)*len);
}

/* Reprint line[pos..] after an insert/delete and park the cursor back at pos. */
static void redraw_tail(struct shell_ctx *ctx, const char *line, size_t len,
                        size_t pos, int erased_one) {
  if (len > pos) {
    ssh_channel_write(ctx->channel, line + pos, (uint32_t)(len - pos));
  }
  if (erased_one) {
    craw(ctx, " ");
  }
  size_t back = (len - pos) + (erased_one ? 1 : 0);
  for (size_t k = 0; k < back; k++) {
    craw(ctx, "\b");
  }
}

/* fish-style prompt: green user@host, cyan path, bold prompt glyph */
static void prompt(struct shell_ctx *ctx) {
  craw(ctx, C_USER);
  craw(ctx, "root@");
  craw(ctx, ctx->host);
  craw(ctx, C_RESET);
  craw(ctx, " ");
  craw(ctx, C_PATH);
  craw(ctx, ctx->cwd);
  craw(ctx, C_RESET);
  craw(ctx, " \342\235\257 "); /* UTF-8 for fish's ❯ */
}

/* Insert a string at the cursor (used by tab completion). */
static void shell_insert_str(struct shell_ctx *ctx, char *line, size_t *len,
                             size_t *pos, const char *str) {
  size_t slen = strlen(str);
  if (slen == 0) {
    return;
  }
  if (*len + slen >= LINE_MAX_LEN) {
    slen = LINE_MAX_LEN - 1 - *len;
  }
  if (slen == 0) {
    return;
  }
  memmove(line + *pos + slen, line + *pos, *len - *pos);
  memcpy(line + *pos, str, slen);
  *len += slen;
  ssh_channel_write(ctx->channel, line + *pos, (uint32_t)(*len - *pos));
  *pos += slen;
  for (size_t k = 0; k < *len - *pos; k++) {
    craw(ctx, "\b");
  }
}

/* Reprint the prompt + current line and park the cursor at pos (used after
 * listing completion candidates on their own line). */
static void reprint_line(struct shell_ctx *ctx, const char *line, size_t len,
                         size_t pos) {
  prompt(ctx);
  if (len) {
    ssh_channel_write(ctx->channel, line, (uint32_t)len);
  }
  for (size_t k = 0; k < len - pos; k++) {
    craw(ctx, "\b");
  }
}

/* Tab completion: completes the token ending at the cursor. Command names in
 * command position, filesystem paths elsewhere. */
static void handle_tab(struct shell_ctx *ctx, char *line, size_t *len,
                       size_t *pos) {
  size_t tstart = *pos;
  while (tstart > 0 && line[tstart - 1] != ' ' && line[tstart - 1] != '\t') {
    tstart--;
  }
  size_t plen = *pos - tstart;
  char partial[PATH_MAX];
  if (plen >= sizeof(partial)) {
    return;
  }
  memcpy(partial, line + tstart, plen);
  partial[plen] = '\0';

  int cmdpos = 1;
  for (size_t i = 0; i < tstart; i++) {
    if (line[i] != ' ' && line[i] != '\t') {
      cmdpos = 0;
      break;
    }
  }

  enum { MAXM = 256 };
  char (*names)[256] = malloc(sizeof(*names) * MAXM);
  if (!names) {
    return;
  }
  int count = 0;
  const char *base = partial;

  if (cmdpos && strchr(partial, '/') == NULL) {
    for (int i = 0; i < N_COMMANDS && count < MAXM; i++) {
      if (strncmp(COMMANDS[i].name, partial, plen) == 0) {
        snprintf(names[count], 256, "%s", COMMANDS[i].name);
        count++;
      }
    }
  } else {
    char dirbuf[PATH_MAX];
    char resolved[PATH_MAX];
    const char *slash = strrchr(partial, '/');
    if (slash) {
      size_t dl = (size_t)(slash - partial) + 1; /* keep the slash */
      if (dl >= sizeof(dirbuf)) {
        free(names);
        return;
      }
      memcpy(dirbuf, partial, dl);
      dirbuf[dl] = '\0';
      base = slash + 1;
      resolve_path(ctx, dirbuf, resolved, sizeof(resolved));
    } else {
      resolve_path(ctx, "", resolved, sizeof(resolved)); /* cwd */
    }
    size_t bl = strlen(base);
    DIR *d = opendir(resolved);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL && count < MAXM) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
          continue;
        }
        if (strncmp(e->d_name, base, bl) == 0) {
          snprintf(names[count], 256, "%s", e->d_name);
          count++;
        }
      }
      closedir(d);
    }
  }

  if (count == 0) {
    craw(ctx, "\a"); /* bell */
    free(names);
    return;
  }

  char lcp[256];
  snprintf(lcp, sizeof(lcp), "%s", names[0]);
  for (int i = 1; i < count; i++) {
    size_t j = 0;
    while (lcp[j] && names[i][j] && lcp[j] == names[i][j]) {
      j++;
    }
    lcp[j] = '\0';
  }

  size_t blen = strlen(base);
  if (strlen(lcp) > blen) {
    shell_insert_str(ctx, line, len, pos, lcp + blen);
  }

  if (count == 1) {
    int isdir = 0;
    if (!(cmdpos && strchr(partial, '/') == NULL)) {
      size_t ts = *pos;
      while (ts > 0 && line[ts - 1] != ' ' && line[ts - 1] != '\t') {
        ts--;
      }
      size_t tl = *pos - ts;
      char full[PATH_MAX], chk[PATH_MAX];
      if (tl < sizeof(full)) {
        memcpy(full, line + ts, tl);
        full[tl] = '\0';
        resolve_path(ctx, full, chk, sizeof(chk));
        struct stat sb;
        if (stat(chk, &sb) == 0 && S_ISDIR(sb.st_mode)) {
          isdir = 1;
        }
      }
    }
    shell_insert_str(ctx, line, len, pos, isdir ? "/" : " ");
  } else if (strlen(lcp) == blen) {
    /* ambiguous and no progress: list the candidates */
    craw(ctx, "\r\n");
    for (int i = 0; i < count; i++) {
      cputs(ctx, names[i]);
      craw(ctx, "  ");
    }
    craw(ctx, "\r\n");
    reprint_line(ctx, line, *len, *pos);
  }
  free(names);
}

void shell_run(ssh_channel channel, int interactive, int rows) {
  struct shell_ctx ctx;
  ctx.channel = channel;
  ctx.running = 1;
  snprintf(ctx.cwd, sizeof(ctx.cwd), "/");
  if (gethostname(ctx.host, sizeof(ctx.host)) != 0 || ctx.host[0] == '\0') {
    snprintf(ctx.host, sizeof(ctx.host), "ps5");
  }
  ctx.history = malloc(sizeof(*ctx.history) * HIST_MAX);
  ctx.hist_count = 0;
  ctx.hist_pos = 0;
  ctx.color = interactive;
  ctx.interactive = interactive;
  ctx.rows = rows;
  ctx.out_fd = -1;
  ctx.in_buf = NULL;
  ctx.in_len = 0;

  if (!interactive) {
    run_batch(&ctx);
    free(ctx.history);
    return;
  }

  craw(&ctx, C_USER);
  cputs(&ctx, "zsshd " ZSSHD_VERSION);
  craw(&ctx, C_RESET);
  cputs(&ctx, " ready. 'help' for commands, 'exit' to quit.\n");
  prompt(&ctx);

  char line[LINE_MAX_LEN];
  size_t len = 0; /* total chars in line   */
  size_t pos = 0; /* cursor index (0..len) */
  char buf[512];
  int esc = 0;      /* 0 normal, 1 saw ESC, 2 in CSI */
  char csi[8];
  int csilen = 0;
  while (ctx.running) {
    int n = ssh_channel_read(channel, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    for (int i = 0; i < n && ctx.running; i++) {
      char c = buf[i];

      /* --- escape-sequence (CSI) state machine --- */
      if (esc == 1) {
        if (c == '[') {
          esc = 2;
          csilen = 0;
        } else {
          esc = 0;
        }
        continue;
      }
      if (esc == 2) {
        if ((c >= '0' && c <= '9') || c == ';') {
          if (csilen < (int)sizeof(csi) - 1) {
            csi[csilen++] = c;
          }
          continue;
        }
        csi[csilen] = '\0';
        if (c == 'A' && ctx.history && ctx.hist_pos > 0) { /* up */
          ctx.hist_pos--;
          replace_line(&ctx, line, &len, &pos, ctx.history[ctx.hist_pos]);
        } else if (c == 'B' && ctx.history) { /* down */
          if (ctx.hist_pos < ctx.hist_count - 1) {
            ctx.hist_pos++;
            replace_line(&ctx, line, &len, &pos, ctx.history[ctx.hist_pos]);
          } else {
            ctx.hist_pos = ctx.hist_count;
            replace_line(&ctx, line, &len, &pos, "");
          }
        } else if (c == 'C') { /* right */
          if (pos < len) {
            ssh_channel_write(channel, &line[pos], 1);
            pos++;
          }
        } else if (c == 'D') { /* left */
          if (pos > 0) {
            pos--;
            craw(&ctx, "\b");
          }
        } else if (c == 'H') { /* home */
          while (pos > 0) {
            pos--;
            craw(&ctx, "\b");
          }
        } else if (c == 'F') { /* end */
          while (pos < len) {
            ssh_channel_write(channel, &line[pos], 1);
            pos++;
          }
        } else if (c == '~') {
          if (strcmp(csi, "3") == 0) { /* delete key: remove char at cursor */
            if (pos < len) {
              memmove(line + pos, line + pos + 1, len - pos - 1);
              len--;
              redraw_tail(&ctx, line, len, pos, 1);
            }
          } else if (strcmp(csi, "1") == 0 || strcmp(csi, "7") == 0) {
            while (pos > 0) {
              pos--;
              craw(&ctx, "\b");
            }
          } else if (strcmp(csi, "4") == 0 || strcmp(csi, "8") == 0) {
            while (pos < len) {
              ssh_channel_write(channel, &line[pos], 1);
              pos++;
            }
          }
        }
        esc = 0;
        continue;
      }
      if (c == 0x1b) {
        esc = 1;
        continue;
      }

      if (c == '\r' || c == '\n') {
        craw(&ctx, "\r\n");
        line[len] = '\0';
        if (len > 0 && ctx.history) {
          int dup = (ctx.hist_count > 0 &&
                     strcmp(ctx.history[ctx.hist_count - 1], line) == 0);
          if (!dup) {
            if (ctx.hist_count < HIST_MAX) {
              snprintf(ctx.history[ctx.hist_count], HIST_LINE, "%s", line);
              ctx.hist_count++;
            } else {
              memmove(ctx.history, ctx.history + 1,
                      sizeof(*ctx.history) * (HIST_MAX - 1));
              snprintf(ctx.history[HIST_MAX - 1], HIST_LINE, "%s", line);
            }
          }
          ctx.hist_pos = ctx.hist_count;
        }
        run_commands(&ctx, line);
        len = 0;
        pos = 0;
        if (ctx.running) {
          prompt(&ctx);
        }
      } else if (c == 0x7f || c == 0x08) { /* backspace: delete before cursor */
        if (pos > 0) {
          memmove(line + pos - 1, line + pos, len - pos);
          len--;
          pos--;
          craw(&ctx, "\b");
          redraw_tail(&ctx, line, len, pos, 1);
        }
      } else if (c == 0x03) { /* Ctrl-C */
        craw(&ctx, "^C\r\n");
        len = 0;
        pos = 0;
        if (ctx.history) {
          ctx.hist_pos = ctx.hist_count;
        }
        prompt(&ctx);
      } else if (c == '\t') {
        handle_tab(&ctx, line, &len, &pos);
      } else if (c >= 0x20 && c != 0x7f && len + 1 < sizeof(line)) {
        /* insert at cursor */
        memmove(line + pos + 1, line + pos, len - pos);
        line[pos] = c;
        len++;
        ssh_channel_write(channel, &line[pos], (uint32_t)(len - pos));
        pos++;
        for (size_t k = 0; k < len - pos; k++) {
          craw(&ctx, "\b");
        }
      }
    }
  }

  free(ctx.history);
}

int shell_exec(ssh_channel channel, const char *command) {
  struct shell_ctx ctx;
  ctx.channel = channel;
  ctx.running = 1;
  ctx.color = 0; /* scripted: clean, uncoloured output */
  ctx.interactive = 0;
  ctx.rows = 0;
  ctx.out_fd = -1;
  ctx.in_buf = NULL;
  ctx.in_len = 0;
  snprintf(ctx.cwd, sizeof(ctx.cwd), "/");
  if (gethostname(ctx.host, sizeof(ctx.host)) != 0 || ctx.host[0] == '\0') {
    snprintf(ctx.host, sizeof(ctx.host), "ps5");
  }
  ctx.history = NULL; /* no history for a one-shot exec */
  ctx.hist_count = 0;
  ctx.hist_pos = 0;
  return run_commands(&ctx, command ? command : "");
}

#else /* !PLATFORM_PS5 */

void shell_run(ssh_channel channel, int interactive, int rows) {
  (void)channel;
  (void)interactive;
  (void)rows;
}
int shell_exec(ssh_channel channel, const char *command) {
  (void)channel;
  (void)command;
  return 0;
}

#endif
