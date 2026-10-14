/* Guarded filesystem writes.

   The plugin must never persistently change the console. etaHEN remounts the
   system partitions read-write, so this is enforced here rather than trusted:
   a write is allowed only when the path is lexically inside BASE_DIR or
   TILE_DIR *and* its deepest existing ancestor, with symlinks resolved, is
   inside the same root. The final component is never followed (O_NOFOLLOW,
   unlink/rename/mkdir semantics), so a planted symlink cannot redirect a write
   into /system_ex. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>

#include "theme.h"


static const char *const roots[] = { BASE_DIR, TILE_DIR };
#define NROOTS (sizeof(roots) / sizeof(*roots))

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void
crc_init(void) {
  for(uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for(int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}


int
fs_guard_init(void) {
  pthread_once(&crc_once, crc_init);
  if(mkdir(BASE_DIR, 0755) && errno != EEXIST) return -1;
  return access(BASE_DIR, W_OK) ? -1 : 0;
}


/* The table is made on first use, so the checks work in any program that
   links them (the fuzzer and test tools too), not only after start-up. */
uint32_t
crc32_update(uint32_t crc, const void *buf, size_t len) {
  const uint8_t *p = buf;
  pthread_once(&crc_once, crc_init);
  crc = ~crc;
  while(len--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}


static int
lexical_ok(const char *path) {
  size_t n = strlen(path);
  if(path[0] != '/' || n >= PATH_MAX - 1) return 0;
  if(strstr(path, "//") || strstr(path, "/./") || strstr(path, "/../")) return 0;
  if(n >= 2 && !strcmp(path + n - 2, "/.")) return 0;
  if(n >= 3 && !strcmp(path + n - 3, "/..")) return 0;
  return 1;
}


static int
within(const char *path, const char *root) {
  size_t n = strlen(root);
  return !strncmp(path, root, n) && (path[n] == '/' || path[n] == 0);
}


/* Real path of the deepest existing ancestor of `path` (excluding path itself). */
static int
existing_parent(const char *path, char *out) {
  char dir[PATH_MAX];

  strcpy(dir, path);
  for(;;) {
    char *slash = strrchr(dir, '/');
    if(!slash) return -1;
    if(slash == dir) { dir[1] = 0; }
    else *slash = 0;
    if(realpath(dir, out)) return 0;
    if(errno != ENOENT && errno != ENOTDIR) return -1;
    if(!strcmp(dir, "/")) return -1;
  }
}


int
fs_write_allowed(const char *path) {
  char parent[PATH_MAX], root_real[PATH_MAX], root_parent[PATH_MAX];

  if(!path || !lexical_ok(path)) return 0;
  for(size_t i = 0; i < NROOTS; i++) {
    if(!within(path, roots[i])) continue;
    if(existing_parent(path, parent)) return 0;

    if(!strcmp(path, roots[i])) {
      /* Creating the root itself: its parent must be where we expect. */
      return !existing_parent(roots[i], root_parent) && !strcmp(parent, root_parent);
    }
    if(!realpath(roots[i], root_real)) return 0;
    return within(parent, root_real);
  }
  return 0;
}


/* For reads that must stay inside a directory, e.g. serving theme files. */
int
fs_read_within(const char *path, const char *root) {
  char real[PATH_MAX], root_real[PATH_MAX];
  return lexical_ok(path) && realpath(path, real) && realpath(root, root_real) &&
         within(real, root_real);
}


static int
deny(const char *op, const char *path) {
  log_err("write guard: refused %s %s", op, path);
  errno = EPERM;
  return -1;
}


int
fs_open_write(const char *path) {
  if(!fs_write_allowed(path)) return deny("write", path);
  return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
}


int
fs_mkdirs(const char *path) {
  char tmp[PATH_MAX];
  struct stat st;

  if(!lexical_ok(path)) return deny("mkdir", path);
  strcpy(tmp, path);
  for(char *p = tmp + 1; ; p++) {
    if(*p != '/' && *p != 0) continue;
    char saved = *p;
    *p = 0;
    if(stat(tmp, &st) || !S_ISDIR(st.st_mode)) {
      if(!fs_write_allowed(tmp)) return deny("mkdir", tmp);
      if(mkdir(tmp, 0755) && errno != EEXIST) return -1;
    }
    *p = saved;
    if(!saved) break;
  }
  return 0;
}


int
fs_rename(const char *from, const char *to) {
  if(!fs_write_allowed(from)) return deny("rename", from);
  if(!fs_write_allowed(to)) return deny("rename", to);
  return rename(from, to);
}


int
fs_unlink(const char *path) {
  if(!fs_write_allowed(path)) return deny("unlink", path);
  return unlink(path);
}


int
fs_rmdir(const char *path) {
  if(!fs_write_allowed(path)) return deny("rmdir", path);
  return rmdir(path);
}


int
fs_symlink(const char *target, const char *path) {
  if(!fs_write_allowed(path)) return deny("symlink", path);
  return symlink(target, path);
}


/* A second name for a regular file; both names stay inside the plugin's
   folder (staging links the stock mirror's files instead of copying them). */
int
fs_link(const char *existing, const char *path) {
  struct stat st;
#ifdef TEST_HOOKS
  if(getenv("PS5THEME_TEST_NO_LINKS")) {   /* a filesystem without hard links, like the console's /data */
    errno = EPERM;
    return -1;
  }
#endif
  if(!fs_write_allowed(path)) return deny("link", path);
  if(!fs_read_within(existing, BASE_DIR) || lstat(existing, &st) || !S_ISREG(st.st_mode)) {
    return deny("link from", existing);
  }
  return link(existing, path);
}


/* A copy of a regular file inside the plugin's own folders, written through the guard; 0 on success. */
int
fs_copy_file(const char *existing, const char *path) {
  struct stat st;
  if(!fs_read_within(existing, BASE_DIR) || lstat(existing, &st) || !S_ISREG(st.st_mode)) {
    return deny("copy from", existing);
  }
  int in = open(existing, O_RDONLY);
  if(in < 0) return -1;
  int out = fs_open_write(path);
  if(out < 0) {
    close(in);
    return -1;
  }
  char buf[16 * 1024];
  int ret = 0;
  uint64_t copied = 0;
  for(;;) {
    ssize_t n = read(in, buf, sizeof(buf));
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) { ret = -1; break; }
    if(n == 0) break;
    for(ssize_t off = 0; off < n; ) {
      ssize_t w = write(out, buf + off, n - off);
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) { ret = -1; break; }
      off += w;
    }
    if(ret) break;
    copied += (uint64_t)n;
  }
  close(in);
  if(close(out) || copied != (uint64_t)st.st_size) ret = -1;
  if(ret) fs_unlink(path);
  return ret;
}


int
fs_is_dir(const char *path) {
  struct stat st;
  return !lstat(path, &st) && S_ISDIR(st.st_mode);
}


static int
rm_rf(const char *path) {
  struct stat st;

  if(lstat(path, &st)) return errno == ENOENT ? 0 : -1;
  if(!S_ISDIR(st.st_mode)) return fs_unlink(path);

  DIR *d = opendir(path);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[PATH_MAX];
    if(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) {
      ret = -1;
      continue;
    }
    if(rm_rf(child)) ret = -1;
  }
  closedir(d);
  return fs_rmdir(path) ? -1 : ret;
}


int
fs_rm_rf_under(const char *root, const char *path) {
  size_t n = strlen(root);

  if(strncmp(path, root, n) || path[n] != '/' || !path[n + 1] || !lexical_ok(path)) {
    return deny("delete", path);
  }
  return rm_rf(path);
}


/* Atomic replace: write to a temp file, fsync, rename over the target. */
int
fs_write_file(const char *path, const void *data, size_t len) {
  char tmp[PATH_MAX];
  const char *p = data;

  if(snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
  int fd = fs_open_write(tmp);
  if(fd < 0) return -1;
  while(len) {
    ssize_t n = write(fd, p, len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) { close(fd); fs_unlink(tmp); return -1; }
    p += n;
    len -= n;
  }
  /* Both always run: a failed fsync must not leave the file open. */
  int failed = fsync(fd) != 0;
#ifdef TEST_HOOKS
  if(!access("/tmp/ps5-theme-fail-fsync", F_OK)) failed = 1;   /* a disk that fails to sync */
#endif
  if(close(fd)) failed = 1;
  if(failed) { fs_unlink(tmp); return -1; }
  return fs_rename(tmp, path);
}


int
fs_crc32_file(const char *path, uint32_t *crc, uint64_t *size) {
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;

  uint8_t *buf = malloc(64 * 1024);
  if(!buf) { close(fd); return -1; }

  uint32_t c = 0;
  uint64_t total = 0;
  int ret = 0;
  for(;;) {
    ssize_t n = read(fd, buf, 64 * 1024);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) { ret = -1; break; }
    if(n == 0) break;
    c = crc32_update(c, buf, n);
    total += n;
  }
  free(buf);
  close(fd);
  if(!ret) {
    *crc = c;
    if(size) *size = total;
  }
  return ret;
}


/* Copy plain directories and regular files (never symlinks) from src to dst,
   e.g. to duplicate a theme. dst must not exist yet. */
static int
copy_tree_into(const char *src, const char *dst, int links) {
  struct stat st;

  if(!lstat(dst, &st)) return deny("copy over existing", dst);
  if(fs_mkdirs(dst)) return -1;

  DIR *d = opendir(src);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  uint8_t *buf = malloc(64 * 1024);
  if(!buf) { closedir(d); return -1; }

  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[PATH_MAX], t[PATH_MAX];
    if(snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
       snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) {
      ret = -1;
      break;
    }
    if(lstat(s, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      ret = copy_tree_into(s, t, links);
    } else if(S_ISREG(st.st_mode)) {
      if(links && !fs_link(s, t)) continue;
      int in = open(s, O_RDONLY);
      int out = in < 0 ? -1 : fs_open_write(t);
      if(in < 0 || out < 0) ret = -1;
      while(!ret) {
        ssize_t n = read(in, buf, 64 * 1024);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) ret = -1;
        if(n <= 0) break;
        for(ssize_t off = 0; !ret && off < n; ) {
          ssize_t w = write(out, buf + off, n - off);
          if(w < 0 && errno == EINTR) continue;
          if(w <= 0) ret = -1;
          else off += w;
        }
      }
      if(in >= 0) close(in);
      if(out >= 0 && close(out)) ret = -1;
    }
  }
  free(buf);
  closedir(d);
  return ret;
}


int
fs_copy_tree(const char *src, const char *dst) {
  return copy_tree_into(src, dst, 0);
}


/* The same tree with every file hard-linked where the filesystem allows. Safe
   for theme folders: every write to a theme replaces a file (a new file is
   renamed over it) and never changes one in place. */
int
fs_link_tree(const char *src, const char *dst) {
  return copy_tree_into(src, dst, 1);
}
