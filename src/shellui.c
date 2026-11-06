/* The home screen's lifecycle: its process found and watched by a thread of its own (shellui_guard), so its exit is
   handled at once even while the main thread is busy applying a theme, and the sandboxes it and other apps leave
   behind. What the rest of the engine asks of it: may an overlay go into this sandbox now (shellui_box_unwatched,
   restart_hold, sandbox_closed), and has anything happened that it should act on (the shellui_* questions below).

   The rules, each found on a console (see docs/challenges.md):
   - nothing goes into the home screen's sandbox until its process runs and is watched;
   - everything of ours comes out of that sandbox as the process begins to exit, before the system takes the sandbox
     down (an overlay left there stopped every app from starting, 1.44; on 2026-11-04 the system removed empty
     folders of the real /user through a view of it that an overlay of ours kept it from taking down), and out of
     the sandboxes of the apps that close with it, in the order the system takes their views down (1.89);
   - from that exit until the next home screen is watched, nothing goes into any sandbox (two apps' views covered as
     the system made them kept the console from waking, 1.79);
   - a sandbox whose app has exited is never covered again.

   Shared with the main thread through atomics only, and one small list under its own lock (dead_lock). */

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "shellui.h"
#include "config.h"
#include "crash.h"
#include "log.h"
#include "mounts.h"
#include "platform.h"
#include "running.h"

static atomic_int shellui_seen;     /* found at least once (for the apply's hints) */
/* The ShellUI sandbox whose process the guard is watching (/mnt/sandbox/NPXS40087_<n>), -1 when none. Nothing of
   ours goes into a ShellUI sandbox but this one (see mount_overlay in apply.c). */
static atomic_int watched_box = -1;
static atomic_int guard_hunting;    /* ShellUI exited moments ago: its successor is looked for, and covered, at once */
static atomic_int shellui_between;  /* from ShellUI's exit until the next one is watched: no sandbox is touched */

static atomic_int guard_looked;     /* the guard's first look is done */
static atomic_int records_stale;    /* the guard took overlays out: the main thread tidies the records */
static atomic_int replan_now;       /* the guard found a new ShellUI: cover its views at once */
/* Home screens that closed after being up SHELLUI_UP_MS or more: a rest mode (or a restart of the console's own), not
   a crash as it started. The main thread's crash check starts its count again on each (note_view). */
static atomic_uint up_exits;


/* 1 while the home screen is restarting (from its exit until the next one is found and watched, as when the console
   goes into rest mode or wakes): then nothing goes into any app's sandbox. The system builds the next home screen's
   sandbox and those of the apps that start with it meanwhile, and on 13.42 the next home screen never started, so
   the console couldn't wake, after the plugin covered two such apps' views just as they were made (1.79). Their views
   are covered once the new home screen runs (the guard asks for that). */
int
restart_hold(const char *on) {
  return !strncmp(on, "/mnt/sandbox/", 13) && atomic_load(&shellui_between);
}

/* Is `on` in a ShellUI sandbox other than the one whose process the guard is watching? Nothing of ours goes into
   one (mount_overlay): a sandbox the system is still building (its process not started), one it is taking down,
   or one whose ShellUI is gone. On 13.42 (2026-10-29), the font overlaid in each new ShellUI sandbox while the
   system was still building it, as it woke from rest mode: two instances in a row never started, the system was
   left unable to take their sandboxes down (our overlay on its font folder), and no ShellUI started again until
   the console was restarted. Watched, every overlay in it comes out the moment it exits (shellui_guard). */
int
shellui_box_unwatched(const char *on) {
  if(strncmp(on, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1)) return 0;
  const char *n = on + sizeof(SHELLUI_SANDBOX) - 1;
  if(*n < '0' || *n > '9') return 1;
  return atoi(n) != atomic_load(&watched_box);
}


/* Sandboxes (/mnt/sandbox/<app>_<n>/) whose app has exited: never covered
   again, nor any older one of the app, so the launcher's teardown of them
   finds nothing of ours (see shellui_guard). Marked by the guard thread,
   read by both. */
static pthread_mutex_t dead_lock = PTHREAD_MUTEX_INITIALIZER;
static char dead_sandboxes[32][40];
static int ndead, dead_next;

/* The sandbox a mount point is in, "/mnt/sandbox/<app>_<n>/" written to
   `out`; 0 if it is in none. */
int
sandbox_of(const char *on, char *out, size_t size) {
  static const char prefix[] = "/mnt/sandbox/";
  if(strncmp(on, prefix, sizeof(prefix) - 1)) return 0;
  const char *name = on + sizeof(prefix) - 1, *slash = strchr(name, '/');
  if(!slash || !strchr(name, '_') || strchr(name, '_') > slash) return 0;
  return snprintf(out, size, "%.*s/", (int)(slash - on), on) < (int)size;
}

static void
sandbox_mark_dead(const char *box) {
  pthread_mutex_lock(&dead_lock);
  snprintf(dead_sandboxes[dead_next], sizeof(dead_sandboxes[0]), "%s", box);
  dead_next = (dead_next + 1) % 32;
  if(ndead < 32) ndead++;
  pthread_mutex_unlock(&dead_lock);
}

/* Is `on` in a sandbox not newer than one whose app was seen exiting? */
int
sandbox_closed(const char *on) {
  char box[64];
  int closed = 0;
  if(!sandbox_of(on, box, sizeof(box))) return 0;
  const char *us = strrchr(box, '_');
  size_t app = (size_t)(us - box);
  int mine = atoi(us + 1);
  pthread_mutex_lock(&dead_lock);
  for(int i = 0; i < ndead && !closed; i++) {
    closed = !strncmp(dead_sandboxes[i], box, app + 1) && atoi(dead_sandboxes[i] + app + 1) >= mine;
  }
  pthread_mutex_unlock(&dead_lock);
  return closed;
}


/* The newest sandbox of each of `apps` (/mnt/sandbox/<app>_<n>; the system keeps older ones' folders), which is
   its running instance's, in newest[] (-1: none), from one look at the folder. */
static void
newest_boxes(const char *const *apps, int napps, int *newest) {
  DIR *d = opendir(SANDBOX_ROOT);
  struct dirent *e;
  for(int a = 0; a < napps; a++) newest[a] = -1;
  while(d && (e = readdir(d))) {
    for(int a = 0; a < napps; a++) {
      size_t len = strlen(apps[a]);
      if(strncmp(e->d_name, apps[a], len) || e->d_name[len] != '_') continue;
      int inst = atoi(e->d_name + len + 1);
      if(inst > newest[a]) newest[a] = inst;
    }
  }
  if(d) closedir(d);
}

/* The newest ShellUI sandbox there is (/mnt/sandbox/NPXS40087_<n>, the
   system keeps older ones' folders), which is the running instance's.
   -1 if there is none. */
static int
newest_shellui_box(void) {
  DIR *d = opendir(SANDBOX_ROOT);
  struct dirent *e;
  int newest = -1;
  const char *name = &SHELLUI_SANDBOX[sizeof(SANDBOX_ROOT)];   /* "NPXS40087_" */
  while(d && (e = readdir(d))) {
    if(strncmp(e->d_name, name, strlen(name))) continue;
    int inst = atoi(e->d_name + strlen(name));
    if(inst > newest) newest = inst;
  }
  if(d) closedir(d);
  return newest;
}


static long long
clock_ms(clockid_t which) {
  struct timespec t;
  clock_gettime(which, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long long now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }

/* ShellUI's sandbox `inst` as "/mnt/sandbox/NPXS40087_<n>/" in `box`; if not known, the newest with an overlay of
   ours. 0 if there is none. */
static int
shellui_box(int inst, char *box, size_t len) {
  static char points[MAX_RECORDS][1024];   /* guard thread only */
  if(inst < 0) {
    int n = plat_our_mounts(BASE_DIR "/", SHELLUI_SANDBOX, points, MAX_RECORDS);
    for(int i = 0; i < n; i++) {
      int k = atoi(points[i] + sizeof(SHELLUI_SANDBOX) - 1);
      if(k > inst) inst = k;
    }
    if(inst < 0) return 0;
  }
  snprintf(box, len, "%s%03d/", SHELLUI_SANDBOX, inst);
  return 1;
}

#define EXIT_LOOK_MS   10      /* how often a watched ShellUI is asked whether its exit has begun */
#define EXIT_GONE_MS   10000   /* the longest it is waited for to be gone, after its exit began */
#define RESTART_SAY_S  20      /* no new ShellUI this long after one exited: said, with what may hold it up */
#define LEFT_LOOK_MS   5000    /* when the sandboxes that closed with it are looked at, after it exited */
int shellui_left_ms = LEFT_LOOK_MS;
/* A home screen up this long before it closed had come up and been used: rest mode only follows one that has. One
   crashing as it started was replaced every 9 s on 13.42 (1.31); rests one after another, as quickly as a person
   could, came 30 s apart (1.87), and three of them in a minute took the theme down as a crash loop; the quickest,
   with 1.88, left a home screen up 24 s. */
#define SHELLUI_UP_MS  15000
int shellui_up_ms = SHELLUI_UP_MS;

/* The sandboxes closing with the home screen: its own and, the apps that close with it (SHELLUI_COMPANIONS), their
   newest. Guard thread only. */
static const char *const companions[] = SHELLUI_COMPANIONS;
#define NCOMPANIONS ((int)(sizeof(companions) / sizeof(*companions)))
static char closing[1 + NCOMPANIONS][48];
static int nclosing;
static long long left_look;   /* when to look at what the system left of them (now_ms); 0: done */

static int
in_closing(const char *on) {
  for(int i = 0; i < nclosing; i++) if(!strncmp(on, closing[i], strlen(closing[i]))) return 1;
  return 0;
}

/* ShellUI is exiting (rest mode, or a crash): its sandbox (see shellui_box) is closed, and our overlays in it come
   out at once, by the mount table, before the launcher (SceLncService) takes it down. An overlay of ours left inside
   its views of folders the system can write then keeps it from taking them down: on 13.42 no game or app would start
   until a restart (CE-105733-9, 1.44), and on 2026-11-04 the system removed empty folders of the real /user through
   such a view, /user/bgft among them, which every sandbox needs, so no home screen started again until a restart.
   So they are taken out as its exit begins (`began`, ms), not once it is gone (the launcher starts on its sandbox
   then), a few at a time, in the order the system takes the views down (release_watching); what it still has open
   then comes out once it is gone. The apps that close with it at every rest lose their sandboxes then too, and ours
   in those come out with these: left there, the views they were in stayed up for good, after every rest (1.88). A
   companion that didn't close (the home screen alone crashed) shows the console's own vsh_asset until the next rest.
   None of these sandboxes is covered again; their successors' views are covered as soon as they appear. The records
   are tidied by the main thread. */
static void
release_sandbox(int pid, int inst, long long began, long long up) {
  char box[40], also[128] = "", more_said[96] = "";
  if(!shellui_box(inst, box, sizeof(box))) return;
  int newest[NCOMPANIONS];
  newest_boxes(companions, NCOMPANIONS, newest);
  nclosing = 0;
  snprintf(closing[nclosing++], sizeof(closing[0]), "%s", box);
  for(int a = 0; a < NCOMPANIONS; a++) {
    if(newest[a] < 0) continue;
    snprintf(closing[nclosing++], sizeof(closing[0]), SANDBOX_ROOT "/%s_%03d/", companions[a], newest[a]);
    size_t at = strlen(also);
    snprintf(also + at, sizeof(also) - at, "%s%s_%03d", at ? ", " : " and of ", companions[a], newest[a]);
  }
  for(int i = 0; i < nclosing; i++) sandbox_mark_dead(closing[i]);   /* first: see mount_overlay */
  const char *name = box + strlen("/mnt/sandbox/");
  int len = (int)strlen(name) - 1, stuck;
  long long t0 = now_ms(), gone = 0;
  int out = release_watching(SANDBOX_ROOT "/", in_closing, began ? pid : 0, &gone, &stuck);
  long long done = now_ms();
  log_msg("ShellUI exited: %d overlay(s) taken out of its sandbox %.*s%s%s in %lld ms%s; up %lld s", out, len, name,
          also, *also ? " (closing with it)" : "", done - t0, began ? ", as its exit began" : "", up / 1000);
  if(began) {
    int before = gone != 0;   /* gone while ours were still coming out */
    while(!gone) {
      if(plat_wait_exit(pid, EXIT_LOOK_MS)) gone = now_ms();
      else if(now_ms() - began >= EXIT_GONE_MS) break;
    }
    int more = release_watching(SANDBOX_ROOT "/", in_closing, 0, NULL, &stuck);
    if(more) snprintf(more_said, sizeof(more_said), "; %d more overlay(s) taken out (in use until then)", more);
    if(!gone) log_msg("ShellUI still there %d s after its exit began%s", EXIT_GONE_MS / 1000, more_said);
    else if(before) log_msg("ShellUI gone %lld ms after its exit began, before ours were all out%s", gone - began, more_said);
    else log_msg("ShellUI gone %lld ms after its exit began, %lld ms after ours were out%s", gone - began, gone - done, more_said);
  }
  atomic_store(&records_stale, 1);
  if(stuck) log_err("ShellUI exited: %d overlay(s) in its sandbox could not be taken out", stuck);
  left_look = now_ms() + shellui_left_ms;
}

/* LEFT_LOOK_MS (shellui_left_ms) after the home screen exited: which of the system's views are still up in the sandboxes that closed
   with it, for the log. One the system couldn't take down (an overlay of ours still in it as it tried) stays up until
   the console restarts. Each sandbox's view of the console's data folder stays whatever was in it (every old
   sandbox keeps one on 13.42, also of apps the plugin never touches), so that one isn't counted. */
static void
look_at_left(void) {
  static char up[64][1024];   /* guard thread only */
  char said[640] = "", names[160] = "";
  int left = 0, ours_left = 0;
  for(int b = 0; b < nclosing; b++) {
    int ours, n = plat_others_under(BASE_DIR "/", closing[b], up, 64, &ours);
    if(n < 0) return;   /* (the table can't be listed: emulated mounts) */
    const char *box = closing[b] + strlen(SANDBOX_ROOT "/");
    size_t at = strlen(names);
    snprintf(names + at, sizeof(names) - at, "%s%.*s", at ? ", " : "", (int)strlen(box) - 1, box);
    ours_left += ours;
    for(int i = 0; i < n; i++) {
      if(i >= 64) {   /* (counted, not named) */
        left++;
        continue;
      }
      const char *rel = up[i] + strlen(closing[b]);
      if(!strcmp(rel, "data")) continue;
      left++;
      at = strlen(said);
      if(at < sizeof(said) - 80) snprintf(said + at, sizeof(said) - at, "%s%.*s%s", at ? ", " : "", (int)strlen(box), box, rel);
    }
  }
  if(!left && !ours_left) {
    log_msg("%d s after the home screen exited, the system had taken down the sandboxes that closed with it (%s)",
            shellui_left_ms / 1000, names);
  } else {
    log_msg("%d s after the home screen exited, %d of the system's view(s) in the sandboxes that closed with it were "
            "still up, which stay until a restart: %s%s", shellui_left_ms / 1000, left, *said ? said : "none",
            ours_left ? " (with overlays of ours still in them)" : "");
  }
}

/* No new ShellUI RESTART_SAY_S after one exited (the console doesn't wake): what may hold the system up, said once,
   for a report. */
static void
say_no_restart(int inst, long long secs) {
  char box[40], user[64];
  static char left[MAX_RECORDS][1024];   /* guard thread only */
  struct stat st;
  int ours = shellui_box(inst, box, sizeof(box)) ? plat_our_mounts(BASE_DIR "/", box, left, MAX_RECORDS) : -1;
  snprintf(user, sizeof(user), "%suser", box);
  log_err("the home screen hasn't started again %lld s after it exited (its old sandbox: %d overlay(s) of ours left, "
          "the system's view of /user %s; /user/bgft %s): if the console doesn't wake, hold its power button until it "
          "beeps twice", secs, ours < 0 ? 0 : ours, ours >= 0 && plat_mounted_under(user) > 0 ? "still there" : "taken down",
          stat("/user/bgft", &st) ? "missing" : "there");
}


/* On a fatal signal (crash.c's handler: system calls and static memory only): our overlays out of ShellUI's
   sandbox, newest first, as the guard takes them out when ShellUI exits. With no copy running, one left there
   would stop the system taking the sandbox down at rest mode (no home screen, or no app starting, until the
   console restarts). From the records, read whole into a static buffer; only one the path shows as ours comes
   off, never a mount of the system's. */
void
crash_release(void) {
  static char buf[512 << 10];
  static char *ons[MAX_RECORDS];
  int fd = open(MOUNTS_FILE, O_RDONLY), n = 0;
  if(fd < 0) return;
  ssize_t len = 0, r;
  while(len < (ssize_t)sizeof(buf) - 1 && (r = read(fd, buf + len, sizeof(buf) - 1 - (size_t)len)) > 0) len += r;
  close(fd);
  buf[len] = 0;
  for(char *p = buf; *p && n < MAX_RECORDS;) {
    char *end = strchr(p, '\n'), *tab = strchr(p, '\t');
    if(end) *end = 0;
    if(tab && (!end || tab < end)) *tab = 0;
    if(!strncmp(p, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1)) ons[n++] = p;
    if(!end) break;
    p = end + 1;
  }
  atomic_store(&watched_box, -1);   /* and nothing more goes in */
  for(int i = n - 1; i >= 0; i--) if(plat_is_our_mount(ons[i], BASE_DIR "/")) plat_unmount_once(ons[i]);
}


void *
shellui_guard(void *arg) {
  int logged = -1, after_exit = 0, last_inst = -1, said = 0;
  long long exited = 0;
  (void)arg;
  crash_thread_init(CRUMB_GUARD);
  for(;;) {
    crumb(CRUMB_GUARD, "looking for ShellUI");
    int pid = plat_find_process(SHELLUI_PROCESS);
    if(left_look && now_ms() >= left_look) {
      left_look = 0;
      look_at_left();
    }
    if(pid <= 0) {
      atomic_store(&guard_looked, 1);
      /* Soon after it exits, a new one starts: looked for often then, so its views are covered as soon as its
         process is there (see watched_box), early enough for what it reads as it starts. */
      int soon = after_exit && mono_seconds() - exited < 30;
      atomic_store(&guard_hunting, soon);
      if(after_exit && !said && mono_seconds() - exited >= RESTART_SAY_S) {
        said = 1;
        say_no_restart(last_inst, mono_seconds() - exited);
      }
      usleep(soon ? 10 * 1000 : 1000 * 1000);
      continue;
    }
    /* Its process runs in the newest of its sandboxes: the system builds one before starting it, and only one
       ShellUI runs at a time. Only now does anything of ours go in. */
    int inst = newest_shellui_box();
    atomic_store(&watched_box, inst);
    atomic_store(&shellui_between, 0);   /* (after: its own sandbox is the watched one first) */
    atomic_store(&guard_hunting, 0);
    atomic_store(&guard_looked, 1);
    if(pid != logged) log_msg("watching ShellUI (pid %d, sandbox _%03d) to take overlays out of its sandbox when it exits", pid, inst);
    logged = pid;
    if(!atomic_exchange(&shellui_seen, 1) || after_exit) atomic_store(&replan_now, 1);
    after_exit = 0;
    said = 0;
    crumb(CRUMB_GUARD, "watching ShellUI");
    long long watched = now_ms();   /* about when it started (but the first, there before the plugin was) */
    long long watched_wall = clock_ms(CLOCK_REALTIME);
    long long began = 0;   /* when its exit was seen beginning; 0: seen only once it was gone */
    for(int look = 0; !plat_wait_exit(pid, EXIT_LOOK_MS); look++) {
      if(plat_process_exiting(pid)) {
        began = now_ms();
        break;
      }
      if(left_look && now_ms() >= left_look) {
        left_look = 0;
        look_at_left();
      }
      if(inst < 0 && !(look % (5000 / EXIT_LOOK_MS))) atomic_store(&watched_box, inst = newest_shellui_box());
    }
    atomic_store(&shellui_between, 1);   /* first: nothing goes into any sandbox until the next one runs */
    atomic_store(&watched_box, -1);      /* nor more into this one (see mount_overlay in apply.c) */
    /* How long it was up, by the longer of two clocks: the steady one may stand still while the console sleeps in
       rest mode, and the console's own is set from the network after it starts (set back, it says less). */
    long long up = (began ? began : now_ms()) - watched, up_wall = clock_ms(CLOCK_REALTIME) - watched_wall;
    if(up_wall > up) up = up_wall;
    if(up >= shellui_up_ms) atomic_fetch_add(&up_exits, 1);   /* before its successor's sandbox can be seen */
    crumb(CRUMB_GUARD, "taking overlays out of ShellUI's sandbox");
    release_sandbox(pid, inst, began, up);
    last_inst = inst;
    after_exit = 1;
    exited = mono_seconds();
    atomic_store(&guard_hunting, 1);
  }
  return NULL;
}


/* What the main thread asks. */
int shellui_found(void)          { return atomic_load(&shellui_seen); }
int shellui_looked(void)         { return atomic_load(&guard_looked); }
int shellui_hunting(void)        { return atomic_load(&guard_hunting); }
int shellui_records_stale(void)  { return atomic_exchange(&records_stale, 0); }
int shellui_replan_due(void)     { return atomic_exchange(&replan_now, 0); }
int shellui_replan_pending(void) { return atomic_load(&replan_now); }
unsigned shellui_up_exits(void)  { return atomic_load(&up_exits); }
