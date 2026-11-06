/* What the engine is doing, for the manager and the console. */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "status.h"
#include "config.h"
#include "fsguard.h"
#include "log.h"
#include "prefs.h"

/* The main thread applies; the web threads ask how it is going and choose themes. state_lock guards what both
   read; themes_mutex is themes_lock (status.h). */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t themes_mutex = PTHREAD_MUTEX_INITIALIZER;
static char applied[128];        /* theme currently mounted, "" if stock */
static char applying[128];       /* theme being applied, "" when idle */
static int  progress_pct = -1;   /* how far the apply is, -1 when idle */
static char progress_text[128];  /* what it is doing, in plain words */
static char result[RESULT_MAX];  /* outcome of the last apply, for the UI */
static atomic_int reapply_requested;
/* An apply asked for by the icon styler or the page's own styling of new icons (reapply_quietly): said once on the
   console when it is done, with what it was for, and not step by step as one you start: a game may be open. One you
   ask for meanwhile (write_active) is said as usual, the note after it. */
static char quiet_note[1024];    /* (state_lock) said once the next apply of quiet_theme is done */
static char quiet_theme[128];    /* (state_lock) */
static int  loud_wanted;         /* (state_lock) an apply asked for by you since the last one began */
static atomic_int apply_quiet;   /* this apply is said only once it is done */
static char apply_note[1024];    /* (main thread) and with this */

/* Console notifications queue up and each stays a few seconds, so a long
   apply gets at most one progress notification every PROGRESS_NOTE_SECONDS
   (and none in its first seconds), on top of the start and the result. */
#define PROGRESS_NOTE_SECONDS 8
static long long progress_started, progress_noted;   /* main thread only (forward-only clock) */

/* What the apply is doing, for the manager and the console. The percentage
   never goes back. */
void
set_progress(int pct, const char *fmt, ...) {
  va_list ap;
  char text[sizeof(progress_text)];
  int shown = -1;
  pthread_mutex_lock(&state_lock);
  if(pct < 0) {
    progress_pct = -1;
    progress_text[0] = 0;
  } else {
    if(pct > 100) pct = 100;
    if(pct > progress_pct) progress_pct = pct;
    va_start(ap, fmt);
    vsnprintf(progress_text, sizeof(progress_text), fmt, ap);
    va_end(ap);
    shown = progress_pct;
    snprintf(text, sizeof(text), "%s", progress_text);
  }
  pthread_mutex_unlock(&state_lock);

  long long now = mono_seconds();
  if(shown < 0) {
    progress_started = 0;
  } else if(!progress_started) {
    progress_started = progress_noted = now;
  } else if(now - progress_noted >= PROGRESS_NOTE_SECONDS && shown < 100 && !atomic_load(&apply_quiet) && pref_on("progress")) {
    progress_noted = now;
    notify("ps5-theme: %s... %d%%", text, shown);
  }
}

int
apply_progress(char *text, size_t size) {
  pthread_mutex_lock(&state_lock);
  int pct = progress_pct;
  snprintf(text, size, "%s", progress_text);
  pthread_mutex_unlock(&state_lock);
  return pct;
}

void
set_result(const char *fmt, ...) {
  va_list ap;
  pthread_mutex_lock(&state_lock);
  va_start(ap, fmt);
  vsnprintf(result, sizeof(result), fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&state_lock);
}

void themes_lock(void)   { pthread_mutex_lock(&themes_mutex); }
void themes_unlock(void) { pthread_mutex_unlock(&themes_mutex); }

void
last_result(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", result);
  pthread_mutex_unlock(&state_lock);
}

int
applied_theme(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", applied);
  pthread_mutex_unlock(&state_lock);
  return out[0] != 0;
}

int
applying_theme(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", applying);
  pthread_mutex_unlock(&state_lock);
  return out[0] != 0;
}

/* ---------------- selection ---------------- */

void
read_active(char *out, size_t size) {
  out[0] = 0;
  FILE *f = fopen(ACTIVE_FILE, "r");
  if(f) {
    if(fgets(out, size, f)) out[strcspn(out, "\r\n \t")] = 0;
    fclose(f);
  }
  if(!out[0]) snprintf(out, size, "none");
}

/* Apply again the theme that is chosen, if it is still `expect`: for the icon styler, which works for a while
   after reading what was applied, and the page's own styling of new icons. It never writes the choice, so a theme
   chosen meanwhile is never undone; if one is chosen between the check and the request, the request applies that
   one, as it was going to be anyway, and the note, about this one, is dropped. Notes asked for before the apply
   begins are said together. */
int
reapply_quietly(const char *expect, const char *note) {
  char cur[128];
  read_active(cur, sizeof(cur));
  if(strcmp(cur, expect)) return -1;
  pthread_mutex_lock(&state_lock);
  size_t have = strlen(quiet_note);
  if(strcmp(quiet_theme, expect)) have = 0;   /* (one left for a theme no longer chosen) */
  snprintf(quiet_theme, sizeof(quiet_theme), "%s", expect);
  if(!have) snprintf(quiet_note, sizeof(quiet_note), "%s", note);
  else if(!strstr(quiet_note, note) && have + 1 + strlen(note) < sizeof(quiet_note)) snprintf(quiet_note + have, sizeof(quiet_note) - have, "\n%s", note);
  pthread_mutex_unlock(&state_lock);
  atomic_store(&reapply_requested, 1);
  return 0;
}

/* Called from the web thread; the main loop picks the change up. */
int
write_active(const char *name) {
  char line[128];
  int n = snprintf(line, sizeof(line), "%s\n", name);
  if(fs_write_file(ACTIVE_FILE, line, n)) return -1;
  pthread_mutex_lock(&state_lock);
  loud_wanted = 1;
  pthread_mutex_unlock(&state_lock);
  atomic_store(&reapply_requested, 1);
  return 0;
}

/* ---------------- the main thread's side of an apply ---------------- */

int
status_take_request(void) {
  return atomic_exchange(&reapply_requested, 0);
}

/* A note asked for with this theme's apply is said once it is done; quietly, unless you asked for one too. The
   console says nothing step by step for a quiet apply, and nothing of one at start (`shown`: a theme was applied
   before by this copy). */
void
status_apply_begins(const char *theme, int shown) {
  pthread_mutex_lock(&state_lock);
  applied[0] = 0;
  snprintf(applying, sizeof(applying), "%s", theme);
  progress_pct = -1;
  int noted = theme[0] && quiet_note[0] && !strcmp(quiet_theme, theme);
  if(quiet_note[0] && !noted) log_msg("not said, as '%s' is applied instead: %s", theme[0] ? theme : "none", quiet_note);
  snprintf(apply_note, sizeof(apply_note), "%s", noted ? quiet_note : "");
  atomic_store(&apply_quiet, noted && !loud_wanted && shown);
  quiet_note[0] = quiet_theme[0] = 0;
  loud_wanted = 0;
  pthread_mutex_unlock(&state_lock);
}

int
status_apply_quiet(void) {
  return atomic_load(&apply_quiet);
}

void
status_apply_ends(const char *theme, int ok) {
  pthread_mutex_lock(&state_lock);
  if(ok) snprintf(applied, sizeof(applied), "%s", theme);
  applying[0] = 0;
  pthread_mutex_unlock(&state_lock);
  if(!ok && apply_note[0]) log_msg("not said, as the apply didn't finish: %s", apply_note);
  apply_note[0] = 0;
  atomic_store(&apply_quiet, 0);
  set_progress(-1, "%s", "");
}

void
status_adopted(const char *theme) {
  pthread_mutex_lock(&state_lock);
  snprintf(applied, sizeof(applied), "%s", theme);
  pthread_mutex_unlock(&state_lock);
}

/* Each line of the note taken for this apply, on the console. */
void
say_apply_note(void) {
  for(char *line = apply_note, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if(next) *next++ = 0;
    notify("%s", line);
  }
  apply_note[0] = 0;
}
