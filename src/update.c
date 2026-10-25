/* Updating in place: one running copy at a time, handing over to a newer one.

   etaHEN starts plugins at boot. After rest mode its Toolbox is often gone,
   so a plugin file copied in then would wait for the next restart. Instead
   the running copy looks for a newer copy of itself: a .plugin file (version
   in etaHEN's header) or a plain .elf (version in the marker every build
   carries), in etaHEN's plugin folders or in UPDATE_DIR, which works without
   etaHEN. It starts the newer one through the ELF loader on port 9021 (the
   standard PS5 payload loader, which etaHEN also runs). The new copy finds
   the old one holding the instance lock, asks it to step aside over the
   local web API, and carries on with the overlays that are already in place.

   Only one copy may run: the lock is an flock() on a file in BASE_DIR, held
   for the life of the process and released by the kernel when it ends. */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "theme.h"


#define ELF_LOADER_PORT  9021
#define PLUGIN_MAX_BYTES (32u << 20)
#define HEADER_BYTES     29          /* "etaHEN_PLUGIN\0" + 9-char title + "\0" + "x.xx\0" */
#define RETRY_SECONDS    600         /* before starting the same file again */
#define PLDMGR_PAYLOADS  "/data/pldmgr/payloads"   /* PS5 Payload Manager's payloads */

static atomic_int handoff, handoff_pid;

/* In every build, so a plain .elf of the plugin shows which version it is. */
#define MARKER "PS5THEME-VERSION:"
__attribute__((used)) static const char version_marker[] = MARKER PLUGIN_VERSION ":" TITLE_ID;


/* "1.23" -> 123; -1 if it isn't a version. */
int
version_value(const char *v) {
  int major = 0, minor = 0, n = 0;
  if(sscanf(v, "%d.%2d%n", &major, &minor, &n) != 2 || n != 4 || major < 0 || major > 9) return -1;
  return major * 100 + minor;
}


int
my_version(void) {
#ifdef TEST_HOOKS
  const char *o = getenv("PS5THEME_TEST_VERSION");
  if(o) return version_value(o);
#endif
  return version_value(PLUGIN_VERSION);
}


int
instance_lock(int wait_ms) {
  static int fd = -1;
  if(fd < 0) fd = fs_open_write(INSTANCE_LOCK);
  if(fd < 0) return -1;
  for(int waited = 0;; waited += 200) {
    if(!flock(fd, LOCK_EX | LOCK_NB)) return 0;
    if(waited >= wait_ms) return -1;
    usleep(200 * 1000);
  }
}


/* One request to the copy answering on this console's web port. The reply
   (headers and body) goes to `out`. 0 on a 200 reply. */
static int
local_request(const char *method, const char *path, char *out, size_t size) {
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(WEB_PORT) };
  struct timeval tv = { .tv_sec = 3 };
  char req[256];
  size_t got = 0;

  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return -1;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  plat_socket_nosigpipe(fd);
  int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-PS5-Theme: 1\r\n"
                   "Content-Length: 0\r\nConnection: close\r\n\r\n", method, path);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) || send(fd, req, n, 0) != n) {
    close(fd);
    return -1;
  }
  while(got + 1 < size) {
    ssize_t r = recv(fd, out + got, size - 1 - got, 0);
    if(r < 0 && errno == EINTR) continue;
    if(r <= 0) break;
    got += r;
  }
  close(fd);
  out[got] = 0;
  return strncmp(out, "HTTP/1.1 200", 12) ? -1 : 0;
}


int
running_version(void) {
  char reply[4096];
  if(local_request("GET", "/api/status", reply, sizeof(reply))) return -1;
  const char *v = strstr(reply, "\"version\":\"");
  return v ? version_value(v + 11) : -1;
}


/* The pid lets the running copy check that this one is still waiting when
   it gets round to stepping aside. */
int
ask_handoff(void) {
  char reply[1024], path[64];
  snprintf(path, sizeof(path), "/api/handoff?pid=%d", (int)getpid());
  return local_request("POST", path, reply, sizeof(reply));
}


void
request_handoff(int pid) {
  atomic_store(&handoff_pid, pid > 0 ? pid : 0);
  atomic_store(&handoff, 1);
}


int handoff_requested(void) { return atomic_load(&handoff); }


static atomic_int stop_flag;

void request_stop(void) { atomic_store(&stop_flag, 1); }

int stop_requested(void) { return atomic_load(&stop_flag); }


/* For another program to find this copy (running_copy): its pid and start
   time, written once it holds the lock. */
void
record_running(void) {
  char line[64];
  int n = snprintf(line, sizeof(line), "%d %llu\n", (int)getpid(), plat_process_start(getpid()));
  if(fs_write_file(PID_FILE, line, (size_t)n)) log_err("could not write %s", PID_FILE);
}


/* Step aside only for a newer copy that is still there: one that gave up
   waiting (this copy was busy applying for longer than it waits) has
   exited, and stepping aside then would leave nothing running. A request
   without a pid (from 1.23 to 1.28) is taken as it is. */
int
handoff_due(void) {
  if(!atomic_load(&handoff)) return 0;
  int pid = atomic_load(&handoff_pid);
  /* "No such process" counts only where this copy can see other processes
     at all (the first process, pid 1, always runs). */
  int visible = !kill(1, 0) || errno != ESRCH;
  if(pid > 0 && visible && kill(pid, 0) && errno == ESRCH) {
    atomic_store(&handoff, 0);
    log_msg("the newer copy that asked to take over (pid %d) is gone; this copy carries on", pid);
    return 0;
  }
  return 1;
}


/* The version in a ps5-theme plugin file's header, or -1. */
static int
plugin_file_version(const char *path, off_t *size) {
  unsigned char h[HEADER_BYTES + 4];
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > HEADER_BYTES + 64 &&
           (uint64_t)st.st_size <= PLUGIN_MAX_BYTES && read(fd, h, sizeof(h)) == (ssize_t)sizeof(h);
  close(fd);
  if(!ok || memcmp(h, "etaHEN_PLUGIN", 14) || memcmp(h + 14, TITLE_ID, 10) || h[28] ||
     memcmp(h + HEADER_BYTES, "\x7f" "ELF", 4)) {
    return -1;
  }
  *size = st.st_size;
  return version_value((const char *)h + 24);
}


/* `needle` in `hay` (the console's C library has no memmem). */
static const uint8_t *
find_bytes(const uint8_t *hay, size_t n, const char *needle, size_t m) {
  for(size_t i = 0; m && i + m <= n; i++) {
    const uint8_t *p = memchr(hay + i, (unsigned char)needle[0], n - m + 1 - i);
    if(!p) return NULL;
    if(!memcmp(p, needle, m)) return p;
    i = (size_t)(p - hay);
  }
  return NULL;
}


/* The version in the marker of a ps5-theme program in memory (an ELF), or -1. */
static int
marker_version(const uint8_t *buf, size_t n) {
  int version = -1;
  if(n <= 64 || memcmp(buf, "\x7f" "ELF", 4)) return -1;
  /* The prefix also appears on its own (it is what this code searches
     for): only "<prefix>x.xx:<title ID>" counts. */
  size_t from = 0;
  while(version < 0 && from < n) {
    const uint8_t *m = find_bytes(buf + from, n - from, MARKER, sizeof(MARKER) - 1);
    if(!m) break;
    size_t at = (size_t)(m - buf) + sizeof(MARKER) - 1;
    from = at;
    if(at + 5 + sizeof(TITLE_ID) <= n && buf[at + 4] == ':' && !memcmp(buf + at + 5, TITLE_ID, sizeof(TITLE_ID))) {
      char v[5];
      memcpy(v, buf + at, 4);
      v[4] = 0;
      version = version_value(v);
    }
  }
  return version;
}


/* The version in a plain .elf of ps5-theme (its marker), or -1. Files are
   remembered by size and time, so each is read once. */
static int
elf_file_version(const char *path, const struct stat *st) {
  static struct { char path[PATH_MAX]; off_t size; time_t mtime; int version; } seen[64];
  static size_t next;
  for(size_t i = 0; i < sizeof(seen) / sizeof(*seen); i++) {
    if(!strcmp(seen[i].path, path) && seen[i].size == st->st_size && seen[i].mtime == st->st_mtime) {
      return seen[i].version;
    }
  }
  int version = -1;
  uint8_t *buf = NULL;
  int fd = -1;
  if(S_ISREG(st->st_mode) && st->st_size > 64 && (uint64_t)st->st_size <= PLUGIN_MAX_BYTES &&
     (buf = malloc(st->st_size)) && (fd = open(path, O_RDONLY)) >= 0 && read(fd, buf, st->st_size) == st->st_size) {
    version = marker_version(buf, (size_t)st->st_size);
  }
  if(fd >= 0) close(fd);
  free(buf);
  snprintf(seen[next].path, PATH_MAX, "%s", path);
  seen[next].size = st->st_size;
  seen[next].mtime = st->st_mtime;
  seen[next].version = version;
  next = (next + 1) % (sizeof(seen) / sizeof(*seen));
  return version;
}


/* Hand a program (the file from `offset` on: after a .plugin header, or a
   whole .elf) to the ELF loader, which starts it as a new process. */
static int
start_elf(const char *path, off_t offset, off_t size) {
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(ELF_LOADER_PORT) };
  struct timeval tv = { .tv_sec = 10 };
  uint8_t *buf = malloc(size);
  int ret = -1;

  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int in = open(path, O_RDONLY);
  if(!buf || in < 0 || read(in, buf, size) != size) {
    log_err("update: could not read %s", path);
    goto done;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) goto done;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  plat_socket_nosigpipe(fd);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    log_err("update: the ELF loader (port %d) is not answering: %s", ELF_LOADER_PORT, strerror(errno));
    close(fd);
    goto done;
  }
  const uint8_t *p = buf + offset;
  size_t left = size - offset;
  while(left) {
    ssize_t n = send(fd, p, left, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) break;
    p += n;
    left -= n;
  }
  shutdown(fd, SHUT_WR);
  /* The loader may keep the connection as the new copy's output: don't wait
     on it, so a hand-over request is answered at once. */
  struct timeval brief = { .tv_sec = 1 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &brief, sizeof(brief));
  char sink[256];
  while(recv(fd, sink, sizeof(sink), 0) > 0) { /* what the loader says, briefly */ }
  close(fd);
  ret = left ? -1 : 0;
  if(ret) log_err("update: sending %s to the ELF loader failed", path);

done:
  if(in >= 0) close(in);
  free(buf);
  return ret;
}


/* PS5 Payload Manager keeps each payload in a folder of its own: under payloads/ in older versions, and
   under payloads/local/ (installed from a USB drive or the console) and payloads/repo/ (downloaded) in
   newer ones, so two levels deep. Folders that are links are not followed. */
static void
each_pm_folder(void (*fn)(const char *dir, void *ctx), void *ctx) {
  fn(PLDMGR_PAYLOADS, ctx);
  DIR *top = opendir(PLDMGR_PAYLOADS);
  if(!top) return;
  struct dirent *e;
  while((e = readdir(top))) {
    char sub[PATH_MAX];
    if(e->d_name[0] == '.' || snprintf(sub, sizeof(sub), "%s/%s", PLDMGR_PAYLOADS, e->d_name) >= (int)sizeof(sub) ||
       !fs_is_dir(sub)) continue;
    fn(sub, ctx);
    DIR *mid = opendir(sub);
    if(!mid) continue;
    struct dirent *f;
    while((f = readdir(mid))) {
      char deep[PATH_MAX];
      if(f->d_name[0] == '.' || snprintf(deep, sizeof(deep), "%s/%s", sub, f->d_name) >= (int)sizeof(deep) ||
         !fs_is_dir(deep)) continue;
      fn(deep, ctx);
    }
    closedir(mid);
  }
  closedir(top);
}


/* The newest copy of ps5-theme found so far. */
typedef struct candidate {
  char path[PATH_MAX];
  int version;
  off_t size, offset;
  time_t mtime;
} candidate_t;


static void
consider_dir(const char *dirpath, candidate_t *best) {
  DIR *dir = opendir(dirpath);
  if(!dir) return;
  struct dirent *e;
  while((e = readdir(dir))) {
    size_t len = strlen(e->d_name);
    char path[PATH_MAX];
    struct stat st;
    off_t size = 0, offset = 0;
    int plugin = len > 7 && !strcmp(e->d_name + len - 7, ".plugin");
    int elf = len > 4 && !strcmp(e->d_name + len - 4, ".elf");
    if(!plugin && !elf) continue;
    if(snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
    int v;
    if(plugin) {
      v = plugin_file_version(path, &size);
      offset = HEADER_BYTES;
    } else {
      v = elf_file_version(path, &st);
      size = st.st_size;
    }
    if(v > best->version) {
      best->version = v;
      best->size = size;
      best->offset = offset;
      best->mtime = st.st_mtime;
      snprintf(best->path, sizeof(best->path), "%s", path);
    }
  }
  closedir(dir);
}


/* ---- the installed copies ----
   A copy of ps5-theme brings the installed copies up to its version once
   each time it starts (at boot, or after taking over in an update): the ones
   in etaHEN's plugin folders and PS5 Payload Manager's payload folders, which
   the console starts it from. Otherwise a restart would start the older copy
   again, and with the update folder emptied nothing would bring this one
   back. Only once per start, so a copy put back on purpose afterwards (going
   back to an older version) is left alone. */

static const char *const installed_dirs[] = { "/data/etaHEN/plugins", "/data/etahen/plugins" };

typedef void (*build_fn)(const char *path, int version, int plugin, void *ctx);

/* Each ps5-theme build (.plugin or .elf) directly in `dirpath`. */
static void
each_build_in(const char *dirpath, build_fn fn, void *ctx) {
  DIR *dir = opendir(dirpath);
  if(!dir) return;
  struct dirent *e;
  while((e = readdir(dir))) {
    size_t len = strlen(e->d_name);
    char path[PATH_MAX];
    struct stat st;
    off_t size = 0;
    int plugin = len > 7 && !strcmp(e->d_name + len - 7, ".plugin");
    int elf = len > 4 && !strcmp(e->d_name + len - 4, ".elf");
    if((!plugin && !elf) || snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name) >= (int)sizeof(path) ||
       lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
    int v = plugin ? plugin_file_version(path, &size) : elf_file_version(path, &st);
    if(v > 0) fn(path, v, plugin, ctx);
  }
  closedir(dir);
}


/* The installed copies: etaHEN's plugin folders, and Payload Manager's payload folders (see each_pm_folder). */
typedef struct { build_fn fn; void *ctx; } walk_t;

static void
builds_in_pm_dir(const char *dir, void *ctx) {
  walk_t *w = ctx;
  each_build_in(dir, w->fn, w->ctx);
}


static void
each_installed_build(build_fn fn, void *ctx) {
  for(size_t i = 0; i < sizeof(installed_dirs) / sizeof(*installed_dirs); i++) each_build_in(installed_dirs[i], fn, ctx);
  walk_t w = { fn, ctx };
  each_pm_folder(builds_in_pm_dir, &w);
}


#define MAX_INSTALLED 16
typedef struct { int mine, count, plugin[MAX_INSTALLED]; char path[MAX_INSTALLED][PATH_MAX]; } copies_t;

static void
note_older(const char *path, int version, int plugin, void *ctx) {
  copies_t *c = ctx;
  if(version < c->mine && c->count < MAX_INSTALLED) {
    snprintf(c->path[c->count], PATH_MAX, "%s", path);
    c->plugin[c->count++] = plugin;
  }
}


static void
note_same(const char *path, int version, int plugin, void *ctx) {
  copies_t *c = ctx;
  if(version == c->mine && !c->count) {
    snprintf(c->path[0], PATH_MAX, "%s", path);
    c->plugin[c->count++] = plugin;
  }
}


/* The program of a ps5-theme file from `skip` on (after a .plugin's header), only if its marker is this version. */
static uint8_t *
read_program(const char *path, off_t skip, size_t *len) {
  struct stat st;
  uint8_t *buf = NULL;
  int fd = open(path, O_RDONLY);
  int ok = fd >= 0 && !fstat(fd, &st) && st.st_size > skip + 64 && (uint64_t)st.st_size <= PLUGIN_MAX_BYTES &&
           (buf = malloc(st.st_size)) && read(fd, buf, st.st_size) == st.st_size &&
           marker_version(buf + skip, (size_t)(st.st_size - skip)) == my_version();
  if(fd >= 0) close(fd);
  if(!ok) { free(buf); return NULL; }
  memmove(buf, buf + skip, (size_t)(st.st_size - skip));
  *len = (size_t)(st.st_size - skip);
  return buf;
}


/* What a replacement must be before it is put in place: this version, header and program both. */
static int
plugin_copy_ok(const char *tmp) {
  off_t size;
  size_t len;
  uint8_t *prog = plugin_file_version(tmp, &size) == my_version() ? read_program(tmp, HEADER_BYTES, &len) : NULL;
  free(prog);
  return prog != NULL;
}


static int
elf_copy_ok(const char *tmp) {
  size_t len;
  uint8_t *prog = read_program(tmp, 0, &len);
  free(prog);
  return prog != NULL;
}


/* 1 when no installed copy is older than this one, after putting this version in place of any that is. */
static int
refresh_installed(void) {
  static copies_t older, same;
  int mine = my_version();
  older.mine = same.mine = mine;
  older.count = same.count = 0;
  each_installed_build(note_older, &older);
  if(!older.count) return 1;
  /* This version's program: from a copy of it here (the update folder first). */
  each_build_in(UPDATE_DIR, note_same, &same);
  if(!same.count) each_installed_build(note_same, &same);
  size_t len = 0;
  uint8_t *prog = same.count ? read_program(same.path[0], same.plugin[0] ? HEADER_BYTES : 0, &len) : NULL;
  if(!prog) {
    log_msg("update: %d installed cop(ies) older than %d.%02d, and no copy of %d.%02d here to put in their place",
            older.count, mine / 100, mine % 100, mine / 100, mine % 100);
    return 0;
  }
  uint8_t *plugin = malloc(HEADER_BYTES + len);
  if(plugin) {
    memset(plugin, 0, HEADER_BYTES);
    memcpy(plugin, "etaHEN_PLUGIN", 14);                       /* with its terminating zero, as etaHEN reads it */
    memcpy(plugin + 14, TITLE_ID, sizeof(TITLE_ID));
    snprintf((char *)plugin + 24, 5, "%d.%02d", mine / 100, mine % 100);
    memcpy(plugin + HEADER_BYTES, prog, len);
  }
  int all = 1;
  for(int i = 0; i < older.count; i++) {
    const char *path = older.path[i];
    int ok = older.plugin[i] ? plugin && !fs_replace_payload(path, plugin, HEADER_BYTES + len, plugin_copy_ok)
                             : !fs_replace_payload(path, prog, len, elf_copy_ok);
    if(ok) log_msg("update: %s is now %d.%02d, so the console starts this version from there too", path, mine / 100, mine % 100);
    else {
      log_err("update: could not put %d.%02d in place of %s; the update folder keeps its copy", mine / 100, mine % 100, path);
      all = 0;
    }
  }
  free(plugin);
  free(prog);
  return all;
}


/* UPDATE_DIR is only a drop box: a copy of ps5-theme there that isn't newer
   than this one has done its job (this copy came from it, or it is older), so
   it goes, once nothing has written to it for TIDY_SECONDS (not one still
   being copied in). Anything that isn't a ps5-theme build stays, and so do
   the copies in etaHEN's and PS5 Payload Manager's folders: those are the
   installed ones. */
#define TIDY_SECONDS 60

static void
tidy_update_dir(void) {
  DIR *dir = opendir(UPDATE_DIR);
  if(!dir) return;
  int mine = my_version();
  time_t now = time(NULL);
  struct dirent *e;
  while((e = readdir(dir))) {
    size_t len = strlen(e->d_name);
    char path[PATH_MAX];
    struct stat st;
    off_t size = 0;
    int plugin = len > 7 && !strcmp(e->d_name + len - 7, ".plugin");
    int elf = len > 4 && !strcmp(e->d_name + len - 4, ".elf");
    if(!plugin && !elf) continue;
    if(snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, e->d_name) >= (int)sizeof(path) || lstat(path, &st) ||
       !S_ISREG(st.st_mode) || now - st.st_mtime < TIDY_SECONDS) continue;
    int v = plugin ? plugin_file_version(path, &size) : elf_file_version(path, &st);
    if(v <= 0 || v > mine) continue;
    if(!fs_unlink(path)) log_msg("update: removed %s from the update folder (%d.%02d, not newer than this copy)", e->d_name, v / 100, v % 100);
  }
  closedir(dir);
}


static void
consider_pm_dir(const char *dir, void *ctx) { consider_dir(dir, ctx); }


/* A newer copy of ps5-theme where people keep payloads: etaHEN's plugin
   folders, UPDATE_DIR, and PS5 Payload Manager's payload folders (two levels,
   see each_pm_folder). Start the newest; 1 if it was started. */
int
update_check(void) {
  static const char *const dirs[] = { "/data/etaHEN/plugins", "/data/etahen/plugins", UPDATE_DIR };
  static int tries = 5, installed_current;
  if(tries > 0) {
    installed_current = refresh_installed();
    tries = installed_current ? 0 : tries - 1;
  }
  if(installed_current) tidy_update_dir();     /* its copy may still be what brings this version back after a restart */
  static char tried[PATH_MAX];
  static time_t tried_at, tried_mtime;
  candidate_t best = { .version = my_version() };

  for(size_t d = 0; d < sizeof(dirs) / sizeof(*dirs); d++) consider_dir(dirs[d], &best);
  each_pm_folder(consider_pm_dir, &best);
  char *best_path = best.path;
  int best_version = best.version;
  off_t best_size = best.size, best_offset = best.offset;
  time_t best_mtime = best.mtime;
  if(!best_path[0]) return 0;

  time_t now = time(NULL);
  if(!strcmp(best_path, tried) && best_mtime == tried_mtime && now - tried_at < RETRY_SECONDS) return 0;
  snprintf(tried, sizeof(tried), "%s", best_path);
  tried_mtime = best_mtime;
  tried_at = now;

  log_msg("update: found ps5-theme %d.%02d in %s, starting it", best_version / 100, best_version % 100, best_path);
  if(start_elf(best_path, best_offset, best_size)) {
    notify("ps5-theme: found version %d.%02d but could not start it. Restart the console to update.",
           best_version / 100, best_version % 100);
    return 0;
  }
  notify("ps5-theme: updating to %d.%02d...", best_version / 100, best_version % 100);
  return 1;
}
