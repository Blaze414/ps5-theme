/* Updating in place: one running copy at a time, handing over to a newer one.

   etaHEN starts plugins at boot. After rest mode its Toolbox is often gone,
   so a plugin file copied in then would wait for the next restart. Instead
   the running copy looks for a newer ps5-theme plugin file in etaHEN's
   plugin folders and starts it through etaHEN's ELF loader (port 9021, the
   loader etaHEN itself uses). The new copy finds the old one holding the
   instance lock, asks it to step aside over the local web API, and carries
   on with the overlays that are already in place.

   Only one copy may run: the lock is an flock() on a file in BASE_DIR, held
   for the life of the process and released by the kernel when it ends. */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
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


#define INSTANCE_LOCK    BASE_DIR "/instance.lock"
#define ELF_LOADER_PORT  9021
#define PLUGIN_MAX_BYTES (32u << 20)
#define HEADER_BYTES     29          /* "etaHEN_PLUGIN\0" + 9-char title + "\0" + "x.xx\0" */
#define RETRY_SECONDS    600         /* before starting the same file again */

static atomic_int handoff;


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


int
ask_handoff(void) {
  char reply[1024];
  return local_request("POST", "/api/handoff", reply, sizeof(reply));
}


void request_handoff(void) { atomic_store(&handoff, 1); }
int  handoff_requested(void) { return atomic_load(&handoff); }


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


/* Hand the ELF inside a plugin file to etaHEN's loader, which starts it as a
   new process. */
static int
start_plugin_file(const char *path, off_t size) {
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
    log_err("update: etaHEN's ELF loader (port %d) is not answering: %s", ELF_LOADER_PORT, strerror(errno));
    close(fd);
    goto done;
  }
  const uint8_t *p = buf + HEADER_BYTES;
  size_t left = size - HEADER_BYTES;
  while(left) {
    ssize_t n = send(fd, p, left, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) break;
    p += n;
    left -= n;
  }
  shutdown(fd, SHUT_WR);
  char sink[256];
  while(recv(fd, sink, sizeof(sink), 0) > 0) { /* the loader's output, until it closes */ }
  close(fd);
  ret = left ? -1 : 0;
  if(ret) log_err("update: sending %s to the ELF loader failed", path);

done:
  if(in >= 0) close(in);
  free(buf);
  return ret;
}


/* A newer ps5-theme plugin file in etaHEN's plugin folders: start it. */
void
update_check(void) {
  static const char *const dirs[] = { "/data/etaHEN/plugins", "/data/etahen/plugins" };
  static char tried[PATH_MAX];
  static time_t tried_at, tried_mtime;
  char best[PATH_MAX] = "";
  int best_version = my_version();
  off_t best_size = 0;
  time_t best_mtime = 0;

  for(size_t d = 0; d < sizeof(dirs) / sizeof(*dirs); d++) {
    DIR *dir = opendir(dirs[d]);
    if(!dir) continue;
    struct dirent *e;
    while((e = readdir(dir))) {
      size_t len = strlen(e->d_name);
      char path[PATH_MAX];
      struct stat st;
      off_t size;
      if(len < 8 || strcmp(e->d_name + len - 7, ".plugin")) continue;
      if(snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
      int v = plugin_file_version(path, &size);
      if(v > best_version) {
        best_version = v;
        best_size = size;
        best_mtime = st.st_mtime;
        snprintf(best, sizeof(best), "%s", path);
      }
    }
    closedir(dir);
  }
  if(!best[0]) return;

  time_t now = time(NULL);
  if(!strcmp(best, tried) && best_mtime == tried_mtime && now - tried_at < RETRY_SECONDS) return;
  snprintf(tried, sizeof(tried), "%s", best);
  tried_mtime = best_mtime;
  tried_at = now;

  log_msg("update: found ps5-theme %d.%02d in %s, starting it", best_version / 100, best_version % 100, best);
  if(start_plugin_file(best, best_size)) {
    notify("ps5-theme: found version %d.%02d but could not start it. Restart the console to update.",
           best_version / 100, best_version % 100);
    return;
  }
  notify("ps5-theme: updating to %d.%02d...", best_version / 100, best_version % 100);
}
