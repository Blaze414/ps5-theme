/* Guarded filesystem writes.

   The plugin must never persistently change the console. etaHEN remounts the system partitions read-write, so this
   is enforced here rather than trusted: a write is allowed only inside BASE_DIR or TILE_DIR, and it is made in the
   very folder that was checked. That folder is opened by walking down from its root one part at a time: each part
   opened as a folder (O_DIRECTORY), never through a link (O_NOFOLLOW). The write is then made relative to that open
   folder (openat, renameat, unlinkat ...), and the last part is never followed either. There is no second lookup of
   the path: a folder on the way renamed, or swapped for a link, after it was opened can't send the write anywhere
   else. A folder is deleted only if nothing is mounted at it or under it, by the mount table.

   On the PS5 the calls relative to an open folder (openat, mkdirat ...) go through platform_ps5.c: the SDK's own
   return a failure as if it were a result (see the end of platform.h). */

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

#include "fsguard.h"
#include "config.h"
#include "log.h"
#include "platform.h"


static const char *const roots[] = { BASE_DIR, TILE_DIR };
#define NROOTS (sizeof(roots) / sizeof(*roots))

#define DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

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
  if(n > 1 && path[n - 1] == '/') return 0;
  return 1;
}


static int
within(const char *path, const char *root) {
  size_t n = strlen(root);
  return !strncmp(path, root, n) && (path[n] == '/' || path[n] == 0);
}


static const char *
root_of(const char *path) {
  for(size_t i = 0; i < NROOTS; i++) if(within(path, roots[i])) return roots[i];
  return NULL;
}


/* A refusal: said in the log as `op` (unless NULL, for the log's own writes and quiet checks), errno EPERM. */
static int
refuse(const char *op, const char *path, const char *why) {
  if(op) log_err("write guard: refused %s %s (%s)", op, path ? path : "(none)", why);
  errno = EPERM;
  return -1;
}

static int deny(const char *op, const char *path) { return refuse(op, path, "outside the plugin's folders"); }

/* What an open with O_NOFOLLOW says about a link: ELOOP, or EMLINK on FreeBSD before 13 (the console's). */
static int
is_link_errno(int e) {
  return e == ELOOP || e == EMLINK;
}


/* `part`, missing in the open folder `dir`, made there and opened: the new folder's descriptor, or -1. */
static int
make_dir(int dir, const char *part) {
  if(mkdirat(dir, part, 0755)) {
    if(errno != EEXIST) return -1;
  } else {
    written(0);
  }
  return openat(dir, part, DIR_FLAGS);
}

/* The folder holding the last part of `path`, opened from `top` down one part at a time, and *leaf, that last part
   (in `path`). Each part is opened as a folder, never through a link: -1 with EPERM (said as `op`). Missing folders on the way are made when `make`, else -1 with ENOENT. `top` itself is
   followed only with `follow_top` (the payload folders, which the system may link); `path` is inside it, longer. */
static int
walk_to_parent(const char *top, int follow_top, const char *path, int make, const char *op, const char **leaf) {
  int fd = open(top, follow_top ? (O_RDONLY | O_DIRECTORY | O_CLOEXEC) : DIR_FLAGS);
  if(fd < 0) return is_link_errno(errno) ? refuse(op, path, "its folder is a link") : -1;
  const char *p = path + strlen(top) + 1;
  for(;;) {
    const char *slash = strchr(p, '/');
    if(!slash) {
      if(!*p) {
        close(fd);
        errno = EINVAL;
        return -1;
      }
      *leaf = p;
      return fd;
    }
    char part[NAME_MAX + 1];
    size_t n = (size_t)(slash - p);
    if(!n || n > NAME_MAX) {
      close(fd);
      errno = ENAMETOOLONG;
      return -1;
    }
    memcpy(part, p, n);
    part[n] = 0;
    int next = openat(fd, part, DIR_FLAGS);
    if(next < 0 && errno == ENOENT && make) next = make_dir(fd, part);
    int e = errno;
    close(fd);
    if(next < 0) {
      if(is_link_errno(e)) return refuse(op, path, "a folder on the way is a link");
      errno = e;
      return -1;
    }
    fd = next;
    p = slash + 1;
  }
}


/* The folder holding `path`'s last part, inside one of the roots (see walk_to_parent); with `make`, the root and the
   folders on the way are made when missing. For a root itself, the folder above it, as the system has it. */
static int
open_parent(const char *path, int make, const char *op, const char **leaf) {
  const char *root = path && lexical_ok(path) ? root_of(path) : NULL;
  if(!root) return deny(op, path);
  size_t n = strlen(root);
  if(!path[n]) {
    char above[PATH_MAX];
    size_t a = (size_t)(strrchr(root, '/') - root);
    snprintf(above, sizeof(above), "%.*s", (int)(a ? a : 1), root);
    *leaf = root + a + 1;
    return open(above, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  }
  if(make) {
    struct stat st;
    const char *l;
    if(lstat(root, &st) && errno == ENOENT) {
      int above = open_parent(root, 0, op, &l);
      if(above >= 0) {
        int fd = make_dir(above, l);
        if(fd >= 0) close(fd);
        close(above);
      }
    }
  }
  return walk_to_parent(root, 0, path, make, op, leaf);
}


/* `leaf` in the open folder `dir` as a plain file of its own: never a link (O_NOFOLLOW), never a device, a pipe or a
   folder (opened without waiting, then asked). -1 with EPERM, said as `op`, for those. */
static int
open_file_at(int dir, const char *leaf, int flags, const char *op, const char *path) {
  int fd = openat(dir, leaf, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644);
  if(fd < 0) return is_link_errno(errno) ? refuse(op, path, "it is a link") : -1;
  struct stat st;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return refuse(op, path, "not a plain file");
  }
  int fl = fcntl(fd, F_GETFL);
  if(fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  return fd;
}


/* errno kept across a close. */
static void
close_keep(int fd) {
  int e = errno;
  close(fd);
  errno = e;
}


int
fs_id_of(const struct stat *st, fs_id_t *id) {
  if(!S_ISREG(st->st_mode)) return -1;
  id->dev = st->st_dev;
  id->ino = st->st_ino;
  id->size = (unsigned long long)st->st_size;
  id->mtime_s = (unsigned long long)st->st_mtim.tv_sec;
  id->mtime_ns = (unsigned long long)st->st_mtim.tv_nsec;
  return 0;
}


int
fs_id_same(const fs_id_t *a, const fs_id_t *b) {
  return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
         a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns;
}


int
fs_guard_init(void) {
  pthread_once(&crc_once, crc_init);
  if(mkdir(BASE_DIR, 0755) && errno != EEXIST) return -1;   /* (by its name, under the system's own /data) */
  int fd = open(BASE_DIR, DIR_FLAGS);   /* a folder of its own, not a link to one */
  if(fd < 0) return -1;
  close(fd);
  return access(BASE_DIR, W_OK) ? -1 : 0;
}


/* The one write outside the plugin's own folders: replacing an installed copy
   of ps5-theme in etaHEN's plugin folder or PS5 Payload Manager's payload
   folders with a newer one, so the next start runs that too. Only an existing
   regular file named .plugin or .elf directly in etaHEN's folder, or at most
   two folders deep in Payload Manager's, reached without a link. The new copy
   is written beside it, checked by `check`, then renamed over it, so a
   cut-off write never leaves a broken copy; all in the folder that was opened
   and checked. The caller makes sure the file is an older ps5-theme build. */
static const char *const payload_roots[] = { "/data/etaHEN/plugins", "/data/etahen/plugins", "/data/pldmgr/payloads" };

int
fs_replace_payload(const char *path, const void *data, size_t len, int (*check)(const char *tmp)) {
  char tmp[PATH_MAX], tmp_leaf[NAME_MAX + 1];
  const char *root = NULL, *leaf;
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
  if(!named || !deep_ok || snprintf(tmp, sizeof(tmp), "%s.ps5theme-new", path) >= (int)sizeof(tmp)) {
    return deny("replace", path);
  }
  int dir = walk_to_parent(root, 1, path, 0, "replace", &leaf);
  if(dir < 0) return -1;
  if(fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
     snprintf(tmp_leaf, sizeof(tmp_leaf), "%s.ps5theme-new", leaf) >= (int)sizeof(tmp_leaf)) {
    close(dir);
    return deny("replace", path);
  }
  struct stat old;
  if(!fstatat(dir, tmp_leaf, &old, AT_SYMLINK_NOFOLLOW) && (!S_ISREG(old.st_mode) || unlinkat(dir, tmp_leaf, 0))) {
    close(dir);
    return deny("replace", tmp);   /* our own leftover only */
  }
  int fd = openat(dir, tmp_leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 0777);
  if(fd < 0) {
    close_keep(dir);
    return -1;
  }
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
  if(!ok || (check && !check(tmp)) || renameat(dir, tmp_leaf, dir, leaf)) {
    unlinkat(dir, tmp_leaf, 0);
    close(dir);
    return -1;
  }
  fsync(dir);
  close(dir);
  return 0;
}


/* `path` opened to read, a regular file inside `root`, reached the way a write is (see the top): never through a
   link. Quiet: -1 with errno (EPERM for a link or anything but a plain file). */
int
fs_open_read(const char *path, const char *root) {
  const char *leaf;
  if(!path || !root || !lexical_ok(path) || !lexical_ok(root) || !within(path, root) || !path[strlen(root)]) {
    errno = EPERM;
    return -1;
  }
  int dir = walk_to_parent(root, 0, path, 0, NULL, &leaf);
  if(dir < 0) return -1;
  int fd = open_file_at(dir, leaf, O_RDONLY, NULL, path);
  close_keep(dir);
  return fd;
}


/* For reads that must stay inside a directory, e.g. serving theme files. A read that follows should use
   fs_open_read, so that what was checked is what is read. */
int
fs_read_within(const char *path, const char *root) {
  int fd = fs_open_read(path, root);
  if(fd < 0) return 0;
  close(fd);
  return 1;
}


/* A folder inside the plugin's folders, opened the way a write's is, for writing into later with openat (the crash
   handler, which can do no more than that). */
int
fs_open_dir(const char *path) {
  const char *leaf;
  int dir = open_parent(path, 0, "open", &leaf);
  if(dir < 0) return -1;
  int fd = openat(dir, leaf, DIR_FLAGS);
  close_keep(dir);
  if(fd < 0 && is_link_errno(errno)) return refuse("open", path, "it is a link");
  return fd;
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


static int
open_in_parent(const char *path, int flags, const char *op) {
  const char *leaf;
  int dir = open_parent(path, 0, op, &leaf);
  if(dir < 0) return -1;
  int fd = written(open_file_at(dir, leaf, flags, op, path));
  close_keep(dir);
  return fd;
}


int
fs_open_write(const char *path) {
  return open_in_parent(path, O_WRONLY | O_CREAT | O_TRUNC, "write");
}


/* For adding to the end of a file; `quiet` for the log's own (a refusal said in the log would write the log). */
int
fs_open_append(const char *path, int quiet) {
  return open_in_parent(path, O_WRONLY | O_CREAT | O_APPEND, quiet ? NULL : "append");
}


/* One more line at the end of a file, in one write and without waiting for the disk: for records that matter only
   while the console runs (the overlays this plugin mounted go with a restart), where waiting for each was most of
   the time the home screen showed its own look during a switch. */
int
fs_append_file(const char *path, const void *data, size_t len) {
  int fd = fs_open_append(path, 0);
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


/* Every folder of `path` that is missing, made; each one checked as it is reached. Outside the plugin's folders, only
   a folder that is already there is accepted (nothing is made). */
int
fs_mkdirs(const char *path) {
  if(!path || !lexical_ok(path)) return refuse("mkdir", path, "not a plain path");
  if(!root_of(path)) {
    struct stat st;
    if(!stat(path, &st) && S_ISDIR(st.st_mode)) return 0;
    return deny("mkdir", path);
  }
  const char *leaf;
  int dir = open_parent(path, 1, "mkdir", &leaf);
  if(dir < 0) return -1;
  int fd = openat(dir, leaf, DIR_FLAGS);
  if(fd < 0 && errno == ENOENT) fd = make_dir(dir, leaf);
  close_keep(dir);
  if(fd < 0) return is_link_errno(errno) ? refuse("mkdir", path, "it is a link") : -1;
  close(fd);
  return 0;
}


int
fs_rename(const char *from, const char *to) {
  const char *lf, *lt;
  int df = open_parent(from, 0, "rename", &lf);
  if(df < 0) return -1;
  int dt = open_parent(to, 0, "rename", &lt);
  if(dt < 0) {
    close_keep(df);
    return -1;
  }
  int rc = written(renameat(df, lf, dt, lt));
  close_keep(df);
  close_keep(dt);
  return rc;
}


int
fs_unlink(const char *path) {
  const char *leaf;
  int dir = open_parent(path, 0, "unlink", &leaf);
  if(dir < 0) return -1;
  int rc = written(unlinkat(dir, leaf, 0));
  close_keep(dir);
  return rc;
}


int
fs_rmdir(const char *path) {
  const char *leaf;
  int dir = open_parent(path, 0, "rmdir", &leaf);
  if(dir < 0) return -1;
  int rc = written(unlinkat(dir, leaf, AT_REMOVEDIR));
  close_keep(dir);
  return rc;
}


int
fs_symlink(const char *target, const char *path) {
  const char *leaf;
  int dir = open_parent(path, 0, "symlink", &leaf);
  if(dir < 0) return -1;
  int rc = written(symlinkat(target, dir, leaf));
  close_keep(dir);
  return rc;
}


/* A second name for a regular file; both names stay inside the plugin's
   folder (staging links the stock mirror's files instead of copying them).
   The file linked is the one in the folder that was checked, never a link. */
int
fs_link(const char *existing, const char *path) {
  struct stat st;
  const char *le, *lp;
#ifdef TEST_HOOKS
  if(getenv("PS5THEME_TEST_NO_LINKS")) {   /* a filesystem without hard links, like the console's /data */
    errno = EPERM;
    return -1;
  }
#endif
  if(!existing || !lexical_ok(existing) || !within(existing, BASE_DIR)) return deny("link from", existing);
  int de = open_parent(existing, 0, "link from", &le);
  if(de < 0) return -1;
  if(fstatat(de, le, &st, AT_SYMLINK_NOFOLLOW) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || S_ISFIFO(st.st_mode) ||
     S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISSOCK(st.st_mode)) {
    close(de);
    return refuse("link from", existing, "not a plain file");
  }
  int dp = open_parent(path, 0, "link", &lp);
  if(dp < 0) {
    close_keep(de);
    return -1;
  }
  int rc = written(linkat(de, le, dp, lp, 0));
  close_keep(de);
  close_keep(dp);
  return rc;
}


/* Copies what can be read from `in` to `out`; 0 when exactly `size` bytes were. */
static int
copy_fd(int in, int out, uint64_t size) {
  char *buf = malloc(64 * 1024);
  if(!buf) return -1;
  uint64_t copied = 0;
  int ret = 0;
  for(;;) {
    ssize_t n = read(in, buf, 64 * 1024);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) { ret = -1; break; }
    if(n == 0) break;
    for(ssize_t off = 0; off < n; ) {
      ssize_t w = write(out, buf + off, (size_t)(n - off));
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) { ret = -1; break; }
      off += w;
    }
    if(ret) break;
    copied += (uint64_t)n;
  }
  free(buf);
  return ret || copied != size ? -1 : 0;
}


/* A copy of a regular file inside the plugin's own folders, written through the guard; 0 on success. */
int
fs_copy_file(const char *existing, const char *path) {
  struct stat st;
  int in = fs_open_read(existing, BASE_DIR);
  if(in < 0) return errno == ENOENT ? -1 : deny("copy from", existing);
  if(fstat(in, &st) || fs_room_for((uint64_t)st.st_size)) {
    close_keep(in);
    return -1;
  }
  int out = fs_open_write(path);
  if(out < 0) {
    close_keep(in);
    return -1;
  }
  int ret = copy_fd(in, out, (uint64_t)st.st_size);
  close(in);
  if(close(out)) ret = -1;
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

/* Deletes `name` in the open folder `dir`, with everything inside it: a link is deleted, never what it points to
   (fs_rm_rf_under has made sure nothing is mounted under it). -2 when deeper than RM_MAX_DEPTH (said as ELOOP by
   fs_rm_rf_under, whatever fails after it). */
static int
rm_at(int dir, const char *name, const char *path, int depth) {
  if(depth > RM_MAX_DEPTH) return -2;
  /* A folder is what opens as one, never through a link; anything else, a link included, is deleted as a name. */
  int fd = openat(dir, name, DIR_FLAGS);
  if(fd < 0) {
    if(errno == ENOENT) return 0;
    if(errno != ENOTDIR && !is_link_errno(errno)) return -1;
    return written(unlinkat(dir, name, 0)) && errno != ENOENT ? -1 : 0;
  }
  DIR *d = fdopendir(fd);
  if(!d) {
    close_keep(fd);
    return -1;
  }
  struct dirent *e;
  int ret = 0;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    int r = rm_at(dirfd(d), e->d_name, path, depth + 1);
    if(r && ret != -2) ret = r;
  }
  closedir(d);
  return written(unlinkat(dir, name, AT_REMOVEDIR)) && ret != -2 ? -1 : ret;
}

/* 1 if a folder inside the open folder `fd` (taken over and closed) is deeper than RM_MAX_DEPTH: then nothing is
   deleted at all, rather than part of it (a theme's settings gone and its files left). */
static int
too_deep(int fd, int depth) {
  if(depth > RM_MAX_DEPTH) {
    close(fd);
    return 1;
  }
  DIR *d = fdopendir(fd);
  if(!d) {
    close(fd);
    return 0;
  }
  int deep = 0;
  struct dirent *e;
  while(!deep && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) continue;   /* (a file: no need to open it to know) */
    int child = openat(dirfd(d), e->d_name, DIR_FLAGS);
    if(child >= 0) deep = too_deep(child, depth + 1);
  }
  closedir(d);
  return deep;
}


static uint64_t
bytes_at(int dir, const char *name, int depth) {
  struct stat st;
  if(depth > RM_MAX_DEPTH || fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) return 0;
  if(S_ISREG(st.st_mode)) return (uint64_t)st.st_size;
  if(!S_ISDIR(st.st_mode)) return 0;
  int fd = openat(dir, name, DIR_FLAGS);
  DIR *d = fd < 0 ? NULL : fdopendir(fd);
  if(!d) {
    if(fd >= 0) close(fd);
    return 0;
  }
  uint64_t n = 0;
  struct dirent *e;
  while((e = readdir(d))) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n += bytes_at(dirfd(d), e->d_name, depth + 1);
  }
  closedir(d);
  return n;
}

/* The bytes of the files under `path`, inside the plugin's folders, walked as a delete walks: never through a link,
   no deeper than RM_MAX_DEPTH. A file with two names counts twice. */
uint64_t
fs_tree_bytes(const char *path) {
  const char *leaf;
  int dir = open_parent(path, 0, NULL, &leaf);
  if(dir < 0) return 0;
  uint64_t n = bytes_at(dir, leaf, 0);
  close(dir);
  return n;
}


int
fs_rm_rf_under(const char *root, const char *path) {
  size_t n = strlen(root);
  const char *leaf;

  if(strncmp(path, root, n) || path[n] != '/' || !path[n + 1] || !lexical_ok(path)) {
    return deny("delete", path);
  }
  /* Nothing is deleted under a mount: what is mounted there isn't the plugin's (the mount table says). */
  if(plat_mounted_under(path) > 0) return refuse("delete", path, "something is mounted inside it");
  int dir = open_parent(path, 0, "delete", &leaf);
  if(dir < 0) return errno == ENOENT ? 0 : -1;
  int r, top = openat(dir, leaf, DIR_FLAGS);
  if(top >= 0 && too_deep(top, 0)) r = -2;
  else r = rm_at(dir, leaf, path, 0);
  close_keep(dir);
  if(r == -2) errno = ELOOP;
  return r ? -1 : 0;
}


/* Atomic replace: write to a temp file, fsync, rename over the target, both in the one folder opened and checked.
   Each write has a temporary name of its own: two threads writing one file (the chosen theme, from two requests at
   once) never write into each other's, and the last rename wins whole. The name ends in .part, which listings and
   applies skip and clear after an hour, so one cut off by a crash or a power loss is never taken for a theme's
   file. */
static atomic_uint temp_seq;

int
fs_write_file(const char *path, const void *data, size_t len) {
  char tmp[NAME_MAX + 1];
  const char *p = data, *leaf;

  int dir = open_parent(path, 0, "write", &leaf);
  if(dir < 0) return -1;
  if(snprintf(tmp, sizeof(tmp), "%s.%u.part", leaf, atomic_fetch_add(&temp_seq, 1)) >= (int)sizeof(tmp)) {
    close(dir);
    errno = ENAMETOOLONG;
    return -1;
  }
  int fd = written(open_file_at(dir, tmp, O_WRONLY | O_CREAT | O_TRUNC, "write", path));
  if(fd < 0) {
    close_keep(dir);
    return -1;
  }
  int failed = 0;
  while(len && !failed) {
    ssize_t n = write(fd, p, len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) failed = 1;
    else { p += n; len -= (size_t)n; }
  }
  /* Both always run: a failed fsync must not leave the file open. */
  if(!failed && fsync(fd)) failed = 1;
#ifdef TEST_HOOKS
  if(!access("/tmp/ps5-theme-fail-fsync", F_OK)) failed = 1;   /* a disk that fails to sync */
#endif
  if(close(fd)) failed = 1;
  if(!failed && written(renameat(dir, tmp, dir, leaf))) failed = 1;
  if(failed) {
    int e = errno;
    unlinkat(dir, tmp, 0);
    errno = e;
  }
  close_keep(dir);
  return failed ? -1 : 0;
}


int
fs_crc32_file(const char *path, uint32_t *crc, uint64_t *size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
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


/* Copy plain directories and regular files (never symlinks) from src, inside the plugin's folder, to dst, e.g. to
   duplicate a theme. dst must not exist yet. Each file is read from the folder that was listed, never through a
   link. */
static int
copy_tree_into(const char *src, const char *dst, int links, int depth) {
  struct stat st;

  if(depth > RM_MAX_DEPTH) { errno = ELOOP; return -1; }   /* (deeper than anything the plugin makes: see there) */
  if(!lstat(dst, &st)) return refuse("copy over existing", dst, "it is there already");
  if(!src || !lexical_ok(src) || !within(src, BASE_DIR) || !src[strlen(BASE_DIR)]) return deny("copy from", src);
  if(fs_mkdirs(dst)) return -1;

  const char *leaf;
  int above = walk_to_parent(BASE_DIR, 0, src, 0, "copy from", &leaf);
  if(above < 0) return -1;
  int sfd = openat(above, leaf, DIR_FLAGS);
  close_keep(above);
  DIR *d = sfd < 0 ? NULL : fdopendir(sfd);
  if(!d) {
    if(sfd >= 0) close_keep(sfd);
    return -1;
  }
  struct dirent *e;
  int ret = 0;
  char *paths = malloc(2 * PATH_MAX);   /* this level's two paths, from the heap (see RM_MAX_DEPTH) */
  if(!paths) { closedir(d); return -1; }

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
    int sub = openat(dirfd(d), e->d_name, DIR_FLAGS);   /* (a folder is what opens as one: see rm_at) */
    if(sub >= 0) {
      close(sub);
      ret = copy_tree_into(s, t, links, depth + 1);
    } else if(errno == ENOTDIR) {
      if(fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
      if(links && !fs_link(s, t)) continue;
      if(fs_room_for((uint64_t)st.st_size)) { ret = -1; break; }   /* (the console's share stays free) */
      int in = open_file_at(dirfd(d), e->d_name, O_RDONLY, "copy from", s);
      struct stat ist;
      int out = in < 0 ? -1 : fs_open_write(t);
      if(in < 0 || out < 0 || fstat(in, &ist) || copy_fd(in, out, (uint64_t)ist.st_size)) ret = -1;
      if(in >= 0) close(in);
      if(out >= 0 && close(out)) ret = -1;
    }
  }
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
