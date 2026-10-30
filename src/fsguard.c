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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>

#include "platform.h"
#include "theme.h"


static const char *const roots[] = { BASE_DIR, TILE_DIR };
#define NROOTS (sizeof(roots) / sizeof(*roots))

static uint32_t crc_table[256];

/* A count of the writes made (bumped after each: one made during a read of the themes is seen by the next read), so
   what was worked out from the plugin's folders can be kept until it changes (the status's walk of each theme). */
static atomic_uint writes_made;
static int written(int rc) { atomic_fetch_add(&writes_made, 1); return rc; }
unsigned fs_generation(void) { return atomic_load(&writes_made); }
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


/* The one write outside the plugin's own folders: replacing an installed copy
   of ps5-theme in etaHEN's plugin folder or PS5 Payload Manager's payload
   folders with a newer one, so the next start runs that too. Only an existing
   regular file named .plugin or .elf directly in etaHEN's folder, or at most
   two folders deep in Payload Manager's, whose folder is really there (no
   symlink out). The new copy is written beside it, checked by `check`, then
   renamed over it, so a cut-off write never leaves a broken copy. The caller
   makes sure the file is an older ps5-theme build. */
static int deny(const char *op, const char *path);
static const char *const payload_roots[] = { "/data/etaHEN/plugins", "/data/etahen/plugins", "/data/pldmgr/payloads" };

int
fs_replace_payload(const char *path, const void *data, size_t len, int (*check)(const char *tmp)) {
  char tmp[PATH_MAX], parent[PATH_MAX], parent_real[PATH_MAX], root_real[PATH_MAX];
  const char *root = NULL;
  struct stat st;

  size_t n = path ? strlen(path) : 0;
  if(!n || !lexical_ok(path)) return deny("replace", path ? path : "(none)");
  int named = (n > 7 && !strcmp(path + n - 7, ".plugin")) || (n > 4 && !strcmp(path + n - 4, ".elf"));
  for(size_t i = 0; i < sizeof(payload_roots) / sizeof(*payload_roots) && !root; i++) {
    if(within(path, payload_roots[i]) && strcmp(path, payload_roots[i])) root = payload_roots[i];
  }
  int depth = 0;
  if(root) for(const char *p = path + strlen(root) + 1; *p; p++) depth += *p == '/';
  int deep_ok = root && depth <= (strstr(root, "pldmgr") ? 2 : 0);   /* Payload Manager: payloads/local/<name>/ */
  snprintf(parent, sizeof(parent), "%s", path);
  *strrchr(parent, '/') = 0;
  if(!named || !deep_ok || lstat(path, &st) || !S_ISREG(st.st_mode) ||
     !realpath(parent, parent_real) || !realpath(root, root_real) || !within(parent_real, root_real) ||
     snprintf(tmp, sizeof(tmp), "%s.ps5theme-new", path) >= (int)sizeof(tmp)) {
    return deny("replace", path);
  }
  struct stat old;
  if(!lstat(tmp, &old) && (!S_ISREG(old.st_mode) || unlink(tmp))) return deny("replace", tmp);   /* our own leftover only */
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 0777);
  if(fd < 0) return -1;
  const uint8_t *p = data;
  size_t left = len;
  int ok = 1;
  while(ok && left) {
    ssize_t w = write(fd, p, left);
    if(w < 0 && errno == EINTR) continue;
    if(w <= 0) ok = 0;
    else { p += w; left -= (size_t)w; }
  }
  if(fsync(fd)) ok = 0;
  if(close(fd)) ok = 0;
  if(!ok || (check && !check(tmp)) || rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  int dir = open(parent, O_RDONLY);
  if(dir >= 0) { fsync(dir); close(dir); }
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


/* Room for `need` more bytes under the plugin's folder, keeping FS_RESERVE free: the storage is the console's,
   shared with saves, app data and the system's own records, and a full disk is worse for them than a refused
   theme file. 0 when there is room (or the free space can't be told); -1 with errno ENOSPC, logged, when not. */
int
fs_room_for(uint64_t need) {
  uint64_t avail = plat_free_bytes(BASE_DIR);
  if(avail == UINT64_MAX || (avail > FS_RESERVE && need <= avail - FS_RESERVE)) return 0;
  log_err("storage nearly full: %llu MB free, %llu MB needed, %llu MB kept free for the console",
          (unsigned long long)(avail >> 20), (unsigned long long)((need + (1u << 20) - 1) >> 20),
          (unsigned long long)(FS_RESERVE >> 20));
  errno = ENOSPC;
  return -1;
}


int
fs_open_write(const char *path) {
  if(!fs_write_allowed(path)) return deny("write", path);
  return written(open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644));
}


/* One more line at the end of a file, in one write and without waiting for the disk: for records that matter only
   while the console runs (the overlays this plugin mounted go with a restart), where waiting for each was most of
   the time the home screen showed its own look during a switch. */
int
fs_append_file(const char *path, const void *data, size_t len) {
  if(!fs_write_allowed(path)) return deny("append", path);
  int fd = written(open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644));
  if(fd < 0) return -1;
  const char *p = data;
  while(len) {
    ssize_t n = write(fd, p, len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) { close(fd); return -1; }
    p += n;
    len -= (size_t)n;
  }
  return close(fd) ? -1 : 0;
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
      if(written(mkdir(tmp, 0755)) && errno != EEXIST) return -1;
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
  return written(rename(from, to));
}


int
fs_unlink(const char *path) {
  if(!fs_write_allowed(path)) return deny("unlink", path);
  return written(unlink(path));
}


int
fs_rmdir(const char *path) {
  if(!fs_write_allowed(path)) return deny("rmdir", path);
  return written(rmdir(path));
}


int
fs_symlink(const char *target, const char *path) {
  if(!fs_write_allowed(path)) return deny("symlink", path);
  return written(symlink(target, path));
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
  return written(link(existing, path));
}


/* A copy of a regular file inside the plugin's own folders, written through the guard; 0 on success. */
int
fs_copy_file(const char *existing, const char *path) {
  struct stat st;
  if(!fs_read_within(existing, BASE_DIR) || lstat(existing, &st) || !S_ISREG(st.st_mode)) {
    return deny("copy from", existing);
  }
  if(fs_room_for((uint64_t)st.st_size)) return -1;
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


/* Deeper than any folder the plugin makes or the system has. A tree deeper still (put there by hand, or a broken
   copy) is left as it is rather than walked: each level takes stack, and a web worker has 512 KB of it. */
#define RM_MAX_DEPTH 64

static int
rm_rf_at(const char *path, int depth) {
  struct stat st;

  if(depth > RM_MAX_DEPTH) return -2;   /* (said as ELOOP by fs_rm_rf_under, whatever fails after it) */
  if(lstat(path, &st)) return errno == ENOENT ? 0 : -1;
  if(!S_ISDIR(st.st_mode)) return fs_unlink(path);

  DIR *d = opendir(path);
  if(!d) return -1;
  char *child = malloc(PATH_MAX);   /* (from the heap: see RM_MAX_DEPTH) */
  if(!child) { closedir(d); return -1; }
  struct dirent *e;
  int ret = 0;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(snprintf(child, PATH_MAX, "%s/%s", path, e->d_name) >= PATH_MAX) {
      ret = -1;
      continue;
    }
    int r = rm_rf_at(child, depth + 1);
    if(r && ret != -2) ret = r;
  }
  closedir(d);
  free(child);
  return fs_rmdir(path) && ret != -2 ? -1 : ret;
}

/* 1 if a folder under `path` is deeper than RM_MAX_DEPTH: then nothing is deleted at all, rather than part of it
   (a theme's settings gone and its files left). */
static int
too_deep(const char *path, int depth) {
  struct stat st;
  if(depth > RM_MAX_DEPTH) return 1;
  if(lstat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
  DIR *d = opendir(path);
  char *child = d ? malloc(PATH_MAX) : NULL;
  int deep = 0;
  struct dirent *e;
  while(child && !deep && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) continue;   /* (a file: no stat needed to know) */
    if(snprintf(child, PATH_MAX, "%s/%s", path, e->d_name) < PATH_MAX) deep = too_deep(child, depth + 1);
  }
  if(d) closedir(d);
  free(child);
  return deep;
}

static int
rm_rf(const char *path) {
  if(too_deep(path, 0)) return -2;
  return rm_rf_at(path, 0);
}


int
fs_rm_rf_under(const char *root, const char *path) {
  size_t n = strlen(root);

  if(strncmp(path, root, n) || path[n] != '/' || !path[n + 1] || !lexical_ok(path)) {
    return deny("delete", path);
  }
  int r = rm_rf(path);
  if(r == -2) errno = ELOOP;
  return r ? -1 : 0;
}


/* Atomic replace: write to a temp file, fsync, rename over the target. Each write has a temporary name of its own:
   two threads writing one file (the chosen theme, from two requests at once) never write into each other's, and
   the last rename wins whole. The name ends in .part, which listings and applies skip and clear after an hour, so
   one cut off by a crash or a power loss is never taken for a theme's file. */
static atomic_uint temp_seq;

int
fs_write_file(const char *path, const void *data, size_t len) {
  char tmp[PATH_MAX];
  const char *p = data;

  if(snprintf(tmp, sizeof(tmp), "%s.%u.part", path, atomic_fetch_add(&temp_seq, 1)) >= (int)sizeof(tmp)) return -1;
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
copy_tree_into(const char *src, const char *dst, int links, int depth) {
  struct stat st;

  if(depth > RM_MAX_DEPTH) { errno = ELOOP; return -1; }   /* (deeper than anything the plugin makes: see there) */
  if(!lstat(dst, &st)) return deny("copy over existing", dst);
  if(fs_mkdirs(dst)) return -1;

  DIR *d = opendir(src);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  uint8_t *buf = malloc(64 * 1024);
  char *paths = malloc(2 * PATH_MAX);   /* this level's two paths, from the heap too */
  if(!buf || !paths) { free(buf); free(paths); closedir(d); return -1; }

  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    /* An upload being received (or one cut off) is never part of a theme: a copy made meanwhile, such as the copy
       Discard goes back to, would otherwise bring it back as a file of the theme. */
    size_t len = strlen(e->d_name);
    if(len > 5 && !strcmp(e->d_name + len - 5, ".part")) continue;
    char *s = paths, *t = paths + PATH_MAX;
    if(snprintf(s, PATH_MAX, "%s/%s", src, e->d_name) >= PATH_MAX ||
       snprintf(t, PATH_MAX, "%s/%s", dst, e->d_name) >= PATH_MAX) {
      ret = -1;
      break;
    }
    if(lstat(s, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      ret = copy_tree_into(s, t, links, depth + 1);
    } else if(S_ISREG(st.st_mode)) {
      if(links && !fs_link(s, t)) continue;
      if(fs_room_for((uint64_t)st.st_size)) { ret = -1; break; }   /* the console's share stays free (logged) */
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
  free(paths);
  closedir(d);
  return ret;
}


int
fs_copy_tree(const char *src, const char *dst) {
  return copy_tree_into(src, dst, 0, 0);
}


/* The same tree with every file hard-linked where the filesystem allows. Safe
   for theme folders: every write to a theme replaces a file (a new file is
   renamed over it) and never changes one in place. */
int
fs_link_tree(const char *src, const char *dst) {
  return copy_tree_into(src, dst, 1, 0);
}
