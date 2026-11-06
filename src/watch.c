/* The view watcher. The system can make new views of a themed folder after the theme was
   applied: after rest mode, some of ShellUI's apps restart in new sandboxes,
   whose views show the stock files. While a theme is applied, the watcher
   overlays any such view from the same stage folders and checks it, within a
   second of the mount table changing.

   The same views show when a system app keeps crashing as it starts: each
   restart is a new sandbox (NPXS40087_002, _003, ...). If one app restarts
   CRASH_RESTARTS times within CRASH_WINDOW seconds while a theme is applied,
   the theme is the likely cause and is taken down, before the console gives
   up on the app. So is a slower loop, CRASH_SLOW_RESTARTS within
   CRASH_SLOW_WINDOW: on 13.42 ShellUI once restarted about once a minute
   for five minutes, and apps wouldn't start meanwhile. A rest mode cycle
   restarts each app once, and the count starts again after a home screen
   that was up shellui_up_ms before it closed (see shellui.c): rests one
   after another came 30 s apart on the console (1.87), and three in a
   minute were taken for a crash loop. A loop in which the home screen
   stays up that long is no longer caught; the one seen (1.39) had a cause
   gone since. Sandboxes of
   earlier restarts can stay in the mount table (ShellUI's _000, _001 and
   _002 at once on the console), so only sandboxes newer than any seen at the
   first look count. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "watch.h"
#include "apply_internal.h"
#include "config.h"
#include "crash.h"
#include "diag.h"
#include "log.h"
#include "mounts.h"
#include "platform.h"
#include "shellui.h"

#define WATCH_FULL_SECONDS 30     /* look again even if the mount count is the same */
#define WATCH_RETRY_SECONDS 60    /* before trying a view that refused the overlay again */

static struct {
  char on[PATH_MAX];
  long long until;   /* (forward-only clock) */
} watch_refused[16];

static int
watch_waiting(const char *on, long long now) {
  for(size_t i = 0; i < sizeof(watch_refused) / sizeof(*watch_refused); i++) {
    if(watch_refused[i].until > now && !strcmp(watch_refused[i].on, on)) return 1;
  }
  return 0;
}

#define CRASH_RESTARTS      3
#define CRASH_WINDOW        60
#define CRASH_SLOW_RESTARTS 5
#define CRASH_SLOW_WINDOW   600

static struct {
  char   title[16];
  int    instance;                /* the newest sandbox seen, e.g. 2 for _002 */
  time_t at[CRASH_SLOW_RESTARTS]; /* the latest restarts, newest first */
  int    restarts;
  unsigned ups;                   /* shellui_up_exits() at its last restart */
  unsigned first_look;            /* the look at the mount table that found it */
} apps_seen[64];
static int napps_seen;
static unsigned looks;
static char crash_app[16];        /* the app that kept restarting (main thread) */
static int crash_count, crash_window;   /* how often, within how many seconds */
static int crash_fast_window = CRASH_WINDOW, crash_slow_window = CRASH_SLOW_WINDOW;

/* Note a view of a sandbox (/mnt/sandbox/<title>_<instance>/...). 1 if its
   app has now restarted too often. Every sandbox of an app at the first look
   that finds it sets its baseline, the newest of them. */
static int
note_view(const char *on, time_t now) {
  static const char prefix[] = "/mnt/sandbox/";
  if(strncmp(on, prefix, sizeof(prefix) - 1)) return 0;
  const char *t = on + sizeof(prefix) - 1, *us = strchr(t, '_'), *slash = us ? strchr(us, '/') : NULL;
  if(!us || !slash || us == t || us - t >= 16 || slash - us < 2) return 0;
  int instance = atoi(us + 1);
  int k = 0;
  while(k < napps_seen && (strncmp(apps_seen[k].title, t, us - t) || apps_seen[k].title[us - t])) k++;
  if(k == napps_seen) {
    if(napps_seen == (int)(sizeof(apps_seen) / sizeof(*apps_seen))) return 0;
    memset(&apps_seen[k], 0, sizeof(apps_seen[k]));
    memcpy(apps_seen[k].title, t, us - t);
    apps_seen[k].instance = instance;
    apps_seen[k].first_look = looks;
    apps_seen[k].ups = shellui_up_exits();
    napps_seen++;
    return 0;
  }
  if(instance <= apps_seen[k].instance) return 0;
  if(apps_seen[k].first_look == looks) {
    apps_seen[k].instance = instance;
    return 0;
  }
  apps_seen[k].instance = instance;
  unsigned ups = shellui_up_exits();
  if(ups != apps_seen[k].ups) {   /* a home screen was up and closed since: rest mode, a new count */
    apps_seen[k].ups = ups;
    apps_seen[k].restarts = 0;
  }
  memmove(apps_seen[k].at + 1, apps_seen[k].at, sizeof(time_t) * (CRASH_SLOW_RESTARTS - 1));
  apps_seen[k].at[0] = now;
  if(apps_seen[k].restarts < CRASH_SLOW_RESTARTS) apps_seen[k].restarts++;
  int r = apps_seen[k].restarts;
  /* A gap below zero means the clock was set back meanwhile (the console sets it from the network): never within. */
  time_t fast = now - apps_seen[k].at[CRASH_RESTARTS - 1], slow = now - apps_seen[k].at[CRASH_SLOW_RESTARTS - 1];
  if(r >= CRASH_RESTARTS && fast >= 0 && fast <= crash_fast_window) {
    crash_count = CRASH_RESTARTS;
    crash_window = crash_fast_window;
  } else if(r >= CRASH_SLOW_RESTARTS && slow >= 0 && slow <= crash_slow_window) {
    crash_count = CRASH_SLOW_RESTARTS;
    crash_window = crash_slow_window;
  } else {
    return 0;
  }
  snprintf(crash_app, sizeof(crash_app), "%s", apps_seen[k].title);
  return 1;
}

/* 1 if what the watcher has to say about `on` was said already since the theme was applied: a view that stays
   as it is gets looked at again every minute, but said once (on the console, a game's views left under another
   mount wrote the same lines every minute, 5,800 a day, pushing what mattered out of the log). */
static char watch_said_on[64][PATH_MAX];
static int nwatch_said;

static int
watch_said(const char *on) {
  for(int i = 0; i < nwatch_said; i++) if(!strcmp(watch_said_on[i], on)) return 1;
  if(nwatch_said < (int)(sizeof(watch_said_on) / sizeof(*watch_said_on))) {
    snprintf(watch_said_on[nwatch_said++], PATH_MAX, "%s", on);
  }
  return 0;
}

/* A view in an app's sandbox is covered only once it has been there VIEW_SETTLE_MS: the system builds a sandbox
   view by view as the app starts, and an overlay put in while it does can keep the app from starting (1.67 with the
   home screen's font; 1.79 with two apps that start with the home screen, and the console didn't wake). The home
   screen's own views are the exception: they go in as soon as its process is watched, before it reads the home
   row's icons, and its sandbox is whole by then (it starts after it). */
#define VIEW_SETTLE_MS 2000
static struct { char on[PATH_MAX]; long long first; } young[64];
static int nyoung;

static int
view_settling(const char *on, long long now_ms) {
  if(strncmp(on, "/mnt/sandbox/", 13)) return 0;
  if(!strncmp(on, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1) && !strstr(on, "/sce_sbx/")) return 0;
  int oldest = 0;
  for(int i = 0; i < nyoung; i++) {
    if(!strcmp(young[i].on, on)) return now_ms - young[i].first < VIEW_SETTLE_MS;
    if(young[i].first < young[oldest].first) oldest = i;
  }
  int k = nyoung < (int)(sizeof(young) / sizeof(*young)) ? nyoung++ : oldest;
  snprintf(young[k].on, PATH_MAX, "%s", on);
  young[k].first = now_ms;
  return 1;
}

static void
watch_refuse(const char *on, long long now) {
  static size_t next;
  snprintf(watch_refused[next].on, PATH_MAX, "%s", on);
  watch_refused[next].until = now + WATCH_RETRY_SECONDS;
  next = (next + 1) % (sizeof(watch_refused) / sizeof(*watch_refused));
}

static int watch_force;   /* plan again now, though the mount table looks the same */

void
watch_look_now(void) {
  watch_force = 1;
}

const char *
watch_crash_app(void) {
  return crash_app;
}

#ifdef TEST_HOOKS
void
watch_set_windows(int fast, int slow) {
  crash_fast_window = fast;
  crash_slow_window = slow;
}
#endif

/* Cover the views made since the last look. 1 if an app keeps restarting while the theme is applied (see above). */
int
watch_views(int reset) {
  static int last_count = -1;
  static long long last_full;
  static job_t jobs[MAX_JOBS];
  time_t now = time(NULL);   /* app restarts: by the wall clock, so rest mode counts between them */
  long long tick = mono_seconds();   /* waits */

  if(reset || !live_stage_count()) {
    last_count = -1;
    napps_seen = 0;
    nwatch_said = 0;
    nyoung = 0;
    return 0;
  }
  int count = plat_mount_count();
  if(count == last_count && tick - last_full < WATCH_FULL_SECONDS && !watch_force) return 0;
  watch_force = 0;
  last_count = count;
  last_full = tick;

  int failed = 0, added = 0, settling = 0;
  long long now_ms = mono_ms();
  int njobs = plan_jobs(jobs, &failed, 1);
  looks++;
  /* Our overlays by the mount table, read once a look and only when needed: one of ours may be under another
     mount at the same point, where the path no longer shows it. Never a second one there. */
  static char ours[MAX_RECORDS][1024];
  int nours = -2;
  for(int i = 0; i < njobs; i++) {
    if(note_view(jobs[i].on, now)) {
      log_err("theme %s: the system app %s restarted %d times within %d s, as when it crashes on start; taking the theme down",
              live_theme_name(), crash_app, crash_count, crash_window);
      return 1;
    }
  }
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    const stage_t *st = live_stage_for(j->real);
    if(!st || plat_is_our_mount(j->on, STAGE_DIR "/") || watch_waiting(j->on, tick)) continue;
    if(nours == -2) nours = plat_our_mounts(STAGE_DIR "/", "/", ours, MAX_RECORDS);
    int stacked = 0;
    for(int k = 0; k < nours && !stacked; k++) stacked = !strcmp(ours[k], j->on);
    if(stacked) {
      if(!watch_said(j->on)) {
        log_msg("new view %s: an overlay of ours is already there, under another mount; left as it is "
                "(the theme shows there again after rest mode)", j->on);
      }
      watch_refuse(j->on, tick);
      continue;
    }

    if(view_settling(j->on, now_ms)) {   /* (looked at again within a second) */
      settling++;
      continue;
    }
    crumb(CRUMB_MAIN, "overlaying a new view %s", j->on);
    int r = mount_overlay(st->dir, j->on);
    if(r > 0) continue;
    if(r) {
      if(!watch_said(j->on)) log_err("new view %s: overlay refused (%s), trying again every %d s", j->on, strerror(errno), WATCH_RETRY_SECONDS);
      watch_refuse(j->on, tick);
      continue;
    }
    if(record_mount(j->on, st->dir, st->real)) log_err("could not record mount %s", j->on);
    /* Against the stage folder: the theme's own files may have unsaved edits. */
    if(verify_plan(&st->plan, st->dir, j->on, "new view")) {
      log_err("new view %s: verification failed, removing overlay", j->on);
      if(plat_unmount(j->on)) log_err("unmount %s failed: %s", j->on, strerror(errno));
      watch_refuse(j->on, tick);
      continue;
    }
    log_msg("overlaid %s, a view the system made after the theme was applied (%d file(s) verified)",
            j->on, st->plan.count);
    added++;
  }
  if(settling) last_count = -1;   /* a view still being built: looked at again on the next round */
  if(added) {
    log_msg("theme %s: %d new view(s) covered", live_theme_name(), added);
    dump_mount_table();
    /* Look once more on the next round rather than taking the count now: our own
       mounts changed it, but so may a view the system made while these were being
       covered (a sandbox's views come one after another), which taking the count
       would hide until the next full look, up to WATCH_FULL_SECONDS later. Our own
       mounts are skipped on that look. */
    last_count = -1;
  }
  crumb(CRUMB_MAIN, "idle");
  return 0;
}
