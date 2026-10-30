/* The running copy of the plugin, and taking its overlays out: for the
   plugin itself (a newer copy taking over, ShellUI exiting, stopping on
   request) and for ps5-theme-stop, which links this file with only the
   platform code. */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform.h"
#include "theme.h"

#define RELEASE_MAX 1024


/* The process recorded in PID_FILE when it took the instance lock, if it is
   still that process (its start time the same, give or take START_SLACK: a
   reused pid is never taken for it), or else the one whose main thread is
   named TITLE_ID. Other than `self`; -1 if neither is found. Copies before
   1.51 write no PID_FILE.
   The start time is the kernel's boot time plus how long after boot the
   process started, and the boot time moves whenever the console sets its
   clock: matched to the microsecond (before 1.73), the copy that had run
   for 43 minutes wasn't recognised (2026-10-29), and 13.42 lists no thread
   names to find it by. */
#define START_SLACK_US (120ull * 1000000ull)

int
running_copy(int self) {
  int pid = 0;
  unsigned long long start = 0;
#ifdef TEST_HOOKS
  if(getenv("PS5THEME_TEST_NOT_FOUND")) return -1;
#endif
  FILE *f = fopen(PID_FILE, "r");
  if(f) {
    if(fscanf(f, "%d %llu", &pid, &start) != 2) pid = 0;
    fclose(f);
  }
  unsigned long long now = pid > 0 && pid != self && start ? plat_process_start(pid) : 0;
  if(now && (now > start ? now - start : start - now) <= START_SLACK_US) return pid;
  return plat_find_named_thread(TITLE_ID, self);
}

/* One request to the copy answering on this console's web port. The reply
   (headers and body) goes to `out`. 0 on a 200 reply. */
int
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



/* A mount point that is overlaid only in the home screen's own views: inside
   /user (games' and apps' pictures), in a sandbox's view of it or not, or a
   sandbox's view of the fonts folder (FONT_DIR is never overlaid itself, so
   only the sandbox form counts). */
int
title_mount_point(const char *on) {
  int in_sandbox = !strncmp(on, "/mnt/sandbox/", 13);
  if(in_sandbox) {
    on = strchr(on + 13, '/');
    if(!on) return 0;
  }
  static const char fonts[] = "/common/font";
  size_t n = strlen(on);
  if(in_sandbox && n >= sizeof(fonts) - 1 && !strcmp(on + n - (sizeof(fonts) - 1), fonts)) return 1;
  return !strncmp(on, "/user/", 6) || !strncmp(on, "/original_user/", 15);
}


/* Take this plugin's overlays (mounts of folders under BASE_DIR: stage
   folders and the stock layers put over overlays in use) out, newest first:
   those whose mount point starts with `under` and, with `match`, that it
   accepts. They are found in the console's mount table, so an overlay
   missing from the records comes out too; where the table can't be listed
   that way, `recorded` (mount points, oldest first) is used. Never forced:
   one in use stays and is counted in *stuck. Only the newest mount at its
   point is taken off: an unmount by path takes the newest there, which for
   one of ours under a mount the system made later would be the system's, so
   such a one stays (counted in *stuck too). The number taken out. */
int
release_overlays(const char *under, int (*match)(const char *on), char (*recorded)[1024], int nrecorded, int *stuck) {
  char (*live)[1024] = malloc(sizeof(*live) * RELEASE_MAX);   /* per call: two threads use this */
  int n = -1, out = 0, buried = 0;
  if(live) n = plat_our_top_mounts(BASE_DIR "/", under, live, RELEASE_MAX, &buried);
  char (*list)[1024] = n >= 0 ? live : recorded;
  if(n < 0) n = nrecorded, buried = 0;
  *stuck = buried;
  for(int i = n - 1; i >= 0; i--) {
    if(strncmp(list[i], under, strlen(under)) || (match && !match(list[i]))) continue;
    if(list == recorded && !plat_is_our_mount(list[i], BASE_DIR "/")) continue;
    if(!plat_unmount_once(list[i])) out++;
    else (*stuck)++;
  }
  free(live);
  return out;
}
