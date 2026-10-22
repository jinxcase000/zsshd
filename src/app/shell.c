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
  char (*history)[HIST_LINE];
  int hist_count;
  int hist_pos;
};

/* --- output helpers: translate bare \n to \r\n for the client PTY ------ */

static void cout(struct shell_ctx *ctx, const char *s, size_t len) {
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
  ccolor(ctx, C_ERR);
  cputs(ctx, buf);
  ccolor(ctx, C_RESET);
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

static int cmd_cat(int argc, char **argv, struct shell_ctx *ctx) {
  if (argc < 2) {
    cerror(ctx, "cat: missing file operand\n");
    return 1;
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
    char buf[4096];
    size_t n;
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

static int cmd_head(int argc, char **argv, struct shell_ctx *ctx) {
  int start;
  int nlines = head_tail_nlines(argc, argv, &start);
  if (start >= argc) {
    cerror(ctx, "head: missing file operand\n");
    return 1;
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
    char line[4096];
    int count = 0;
    while (count < nlines && fgets(line, sizeof(line), f)) {
      cout(ctx, line, strlen(line));
      count++;
    }
    fclose(f);
    if (multiple && a + 1 < argc) {
      cputs(ctx, "\n");
    }
  }
  return rc;
}

static int cmd_tail(int argc, char **argv, struct shell_ctx *ctx) {
  int start;
  int nlines = head_tail_nlines(argc, argv, &start);
  if (start >= argc) {
    cerror(ctx, "tail: missing file operand\n");
    return 1;
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
    if (nlines > 0) {
      /* keep only the last N lines in a ring of heap strings */
      char **ring = calloc((size_t)nlines, sizeof(char *));
      if (!ring) {
        fclose(f);
        cerror(ctx, "tail: out of memory\n");
        rc = 1;
        continue;
      }
      char line[4096];
      int idx = 0, filled = 0;
      while (fgets(line, sizeof(line), f)) {
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
    fclose(f);
    if (multiple && a + 1 < argc) {
      cputs(ctx, "\n");
    }
  }
  return rc;
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
  if (argc - start < 2) {
    cerror(ctx, "grep: usage: grep [-i] pattern file...\n");
    return 1;
  }
  const char *pat = argv[start++];
  regex_t re;
  if (regcomp(&re, pat, cflags) != 0) {
    cerror(ctx, "grep: invalid pattern\n");
    return 1;
  }
  int multiple = (argc - start) > 1, matched = 0;
  for (int a = start; a < argc; a++) {
    char path[PATH_MAX];
    resolve_path(ctx, argv[a], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
      cerror(ctx, "grep: ");
      cprintf(ctx, "%s: %s\n", argv[a], strerror(errno));
      continue;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
      size_t L = strlen(line);
      if (L > 0 && line[L - 1] == '\n') {
        line[L - 1] = '\0';
      }
      if (regexec(&re, line, 0, NULL, 0) == 0) {
        if (multiple) {
          ccolor(ctx, C_PATH);
          craw(ctx, argv[a]);
          ccolor(ctx, C_RESET);
          craw(ctx, ":");
        }
        cputs(ctx, line);
        cputs(ctx, "\n");
        matched = 1;
      }
    }
    fclose(f);
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
    if (!ctx->interactive) {
      /* no TTY: dump like cat */
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

static int dispatch_one(struct shell_ctx *ctx, char *line) {
  char *argv[MAX_ARGS];
  int argc = tokenize(line, argv, MAX_ARGS);
  if (argc == 0) {
    return 0;
  }
  for (int i = 0; i < N_COMMANDS; i++) {
    if (strcmp(argv[0], COMMANDS[i].name) == 0) {
      return COMMANDS[i].fn(argc, argv, ctx);
    }
  }
  cerror(ctx, "%s: command not found\n", argv[0]);
  return 127;
}

/* Run one or more commands separated by ';' or newlines. Returns the exit
 * status of the last command executed. */
static int run_commands(struct shell_ctx *ctx, const char *text) {
  char work[LINE_MAX_LEN];
  snprintf(work, sizeof(work), "%s", text);
  int status = 0;
  char *seg = work;
  for (char *p = work;; p++) {
    if (*p == ';' || *p == '\n' || *p == '\0') {
      char end = *p;
      *p = '\0';
      status = dispatch_one(ctx, seg);
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
