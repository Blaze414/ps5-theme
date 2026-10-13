/* The running copy of the plugin, and taking its overlays out: for the
   plugin itself (a newer copy taking over, ShellUI exiting, stopping on
   request) and for ps5-theme-stop, which links this file with only the
   platform code. */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "theme.h"

#define RELEASE_MAX 1024


/* The process recorded in PID_FILE when it took the instance lock, if it is
   still that process (same start time: a reused pid is never taken for
   it), or else the one whose main thread is named TITLE_ID. Other than
   `self`; -1 if neither is found. Copies before 1.51 write no PID_FILE. */
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
  if(pid > 0 && pid != self && start && plat_process_start(pid) == start) return pid;
  return plat_find_named_thread(TITLE_ID, self);
}


/* A mount point inside /user, in a sandbox's view of it or not: only games'
   and apps' pictures are overlaid there. */
int
title_mount_point(const char *on) {
  if(!strncmp(on, "/mnt/sandbox/", 13)) {
    on = strchr(on + 13, '/');
    if(!on) return 0;
  }
  return !strncmp(on, "/user/", 6) || !strncmp(on, "/original_user/", 15);
}


/* Take this plugin's overlays (mounts of folders under BASE_DIR: stage
   folders and the stock layers put over overlays in use) out, newest first:
   those whose mount point starts with `under` and, with `match`, that it
   accepts. They are found in the console's mount table, so an overlay
   missing from the records comes out too; where the table can't be listed
   that way, `recorded` (mount points, oldest first) is used. Never forced:
   one in use stays and is counted in *stuck. The number taken out. */
int
release_overlays(const char *under, int (*match)(const char *on), char (*recorded)[1024], int nrecorded, int *stuck) {
  char (*live)[1024] = malloc(sizeof(*live) * RELEASE_MAX);   /* per call: two threads use this */
  int n = -1, out = 0;
  if(live) n = plat_our_mounts(BASE_DIR "/", under, live, RELEASE_MAX);
  char (*list)[1024] = n >= 0 ? live : recorded;
  if(n < 0) n = nrecorded;
  *stuck = 0;
  for(int i = n - 1; i >= 0; i--) {
    if(strncmp(list[i], under, strlen(under)) || (match && !match(list[i]))) continue;
    if(list == recorded && !plat_is_our_mount(list[i], BASE_DIR "/")) continue;
    if(!plat_unmount_once(list[i])) out++;
    else (*stuck)++;
  }
  free(live);
  return out;
}
