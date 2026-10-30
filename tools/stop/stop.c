/* ps5-theme-stop: stops the running ps5-theme and leaves its theme on the
   console, for when the manager can't be reached: start this, then start
   ps5-theme again (it carries on with the theme). Start it from a payload
   loader (port 9021) or PS5 Payload Manager.

   Whether a copy runs is decided by the plugin's instance lock alone, and
   overlays are only touched while this program holds that lock, so never
   under a running copy. A running copy is asked to stop over its web port
   first (since 1.73: nothing to look for), else, found by running_copy, by
   its signal (SIGUSR2): it takes out only the overlays of games' and apps'
   pictures, which must not stay in the home screen's view without a copy
   running to take them out at rest mode (no game or app would start after
   it), and exits. The rest of the theme stays; the next copy carries on and
   puts the pictures back. A copy that doesn't stop within two minutes is
   ended, and so is a copy from before 1.50 (it ends on the signal at once):
   the pictures' overlays are then taken out here. One that can't be found
   is left alone, with what this program saw written to DUMP_DIR/stop.txt. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>

#include "platform.h"
#include "theme.h"

#define STOP_WAIT_SECONDS 120
#define KILL_WAIT_SECONDS 10
#define MAX_LINES         2048


/* What the platform code logs goes to standard output here. */
static void
vlog(const char *level, const char *fmt, va_list ap) {
  printf("[ps5-theme-stop] %s", level);
  vprintf(fmt, ap);
  putchar('\n');
  fflush(stdout);
}

void
log_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog("", fmt, ap);
  va_end(ap);
}

void
log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog("error: ", fmt, ap);
  va_end(ap);
}


static void
say(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  plat_notify(msg);
  log_msg("%s", msg);
}


/* 0 once this program holds the lock, trying for up to `seconds`. */
static int
lock_within(int fd, int seconds) {
  for(int i = 0; i <= seconds * 5; i++) {
    if(!flock(fd, LOCK_EX | LOCK_NB)) return 0;
    if(i < seconds * 5) usleep(200 * 1000);
  }
  return -1;
}


/* Holding the lock: the overlays of games' and apps' pictures come out,
   and their records with them (MOUNTS_FILE: "<mount point>\t<source>\t
   <folder it shows>" per line, oldest first); the other records stay. */
static int
release_titles(int *stuck) {
  static char lines[MAX_LINES][PATH_MAX + 800], points[MAX_LINES][1024];
  int n = 0, npoints = 0, left = 0;
  FILE *f = fopen(MOUNTS_FILE, "r");
  while(f && n < MAX_LINES && fgets(lines[n], sizeof(lines[n]), f)) {
    lines[n][strcspn(lines[n], "\r\n")] = 0;
    if(!lines[n][0]) continue;
    snprintf(points[npoints++], sizeof(points[0]), "%.*s", (int)strcspn(lines[n], "\t"), lines[n]);
    n++;
  }
  if(f) fclose(f);
  int out = release_overlays("", title_mount_point, points, npoints, stuck);
  for(int i = 0; i < n; i++) {
    int keep = !title_mount_point(points[i]) || plat_is_our_mount(points[i], BASE_DIR "/");
    if(keep && left != i) memcpy(lines[left], lines[i], sizeof(lines[0]));
    left += keep;
  }
  if(left == n) return out;
  if(!left) {
    if(unlink(MOUNTS_FILE) && errno != ENOENT) log_err("could not update %s", MOUNTS_FILE);
    return out;
  }
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.tmp", MOUNTS_FILE);
  int ok = (f = fopen(tmp, "w")) != NULL;
  for(int i = 0; ok && i < left; i++) ok = fprintf(f, "%s\n", lines[i]) > 0;
  if(f) ok = !fclose(f) && ok;
  if(!ok || rename(tmp, MOUNTS_FILE)) {
    unlink(tmp);
    log_err("could not update %s", MOUNTS_FILE);
  }
  return out;
}


/* What this program saw, for when the running copy can't be found. */
static void
write_report(void) {
  mkdir(DUMP_DIR, 0755);
  FILE *f = fopen(DUMP_DIR "/stop.txt", "w");
  if(!f) return;
  char line[128] = "";
  FILE *pid = fopen(PID_FILE, "r");
  if(pid && !fgets(line, sizeof(line), pid)) line[0] = 0;
  if(pid) fclose(pid);
  fprintf(f, "# %s: %s", PID_FILE, line[0] ? line : "(none)\n");
  int recorded = atoi(line);   /* that process's start time as the console says it now, to compare */
  if(recorded > 0) fprintf(f, "# pid %d starts now at %llu\n", recorded, plat_process_start(recorded));
  char reply[256];
  fprintf(f, "# web port: %s\n", local_request("GET", "/api/status", reply, sizeof(reply)) ? "no answer" : "answers");
  fprintf(f, "# threads (pid tid name / process name)\n");
  plat_dump_threads(f);
  fclose(f);
}


int
main(void) {
  int stuck = 0, pid = -1, ended = 0;

  if(plat_init()) {
    say("ps5-theme-stop: could not get privileges. Is the console jailbroken?");
    return 1;
  }
  int lock = open(INSTANCE_LOCK, O_RDWR | O_CREAT, 0644);
  if(lock < 0) {
    say("ps5-theme-stop: cannot open %s (%s)", INSTANCE_LOCK, strerror(errno));
    return 1;
  }
  int asked = 0;
  if(flock(lock, LOCK_EX | LOCK_NB)) {
    /* A copy runs: asked over its web port first, which needs no looking for its process. */
    char reply[256];
    int web = 1;
#ifdef TEST_HOOKS
    web = !getenv("PS5THEME_TEST_NOT_FOUND") && !getenv("PS5THEME_TEST_NO_WEB");
#endif
    if(web && !local_request("POST", "/api/stop", reply, sizeof(reply))) {
      log_msg("asked ps5-theme to stop over its web port");
      asked = !lock_within(lock, STOP_WAIT_SECONDS);
      if(!asked) log_msg("ps5-theme did not stop within %d s of being asked", STOP_WAIT_SECONDS);
    }
  }
  if(!asked && flock(lock, LOCK_EX | LOCK_NB)) {
    pid = running_copy(getpid());
    if(pid <= 0) {
      write_report();
      say("ps5-theme-stop: ps5-theme is running but couldn't be found, so nothing was changed. "
          "Details are in %s/stop.txt.", DUMP_DIR);
      return 1;
    }
    log_msg("asking ps5-theme (pid %d) to stop", pid);
    if(kill(pid, SIGUSR2) || lock_within(lock, STOP_WAIT_SECONDS)) {
      log_msg("ps5-theme (pid %d) did not stop; ending it", pid);
      kill(pid, SIGKILL);
      ended = 1;
      if(lock_within(lock, KILL_WAIT_SECONDS)) {
        say("ps5-theme-stop: ps5-theme could not be stopped, so nothing was changed.");
        return 1;
      }
    }
  }
  /* This program holds the lock now: no copy runs, and none starts until it
     ends. What one that ended without stopping left comes out. */
  int out = release_titles(&stuck);
  if(out) log_msg("took %d overlay(s) of games' and apps' pictures out", out);
  if(stuck) {
    say("ps5-theme-stop: %d game picture(s) are still in use. Start ps5-theme again before rest mode.", stuck);
  } else if(pid <= 0 && !asked) {
    say("ps5-theme isn't running. Start it to open the manager.");
  } else {
    say("ps5-theme %s. The theme stays; start ps5-theme again to open the manager.", ended ? "was ended" : "stopped");
  }
  return 0;
}
