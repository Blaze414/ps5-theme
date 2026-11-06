/* What the apply engine (apply.c) shares with the code beside it on the main thread: the view watcher (watch.c)
   and the diagnostics (diag.c), and nothing else. */
#ifndef PS5_THEME_APPLY_INTERNAL_H
#define PS5_THEME_APPLY_INTERNAL_H

#include <limits.h>

#include "fsguard.h"
#include "platform.h"

/* The folders a theme may change files in. An icon style takes two a game (its metadata and its sce_sys) and some
   thirty for the system tiles; 256 holds a library of over a hundred. Each costs about 2 KB of the engine's
   tables for every view of it. */
#define MAX_TARGETS 256
#define MAX_JOBS    (MAX_TARGETS * 6)   /* (a game's folders have two or three views; the system's about a dozen) */
#define MAX_VIEWS   256                 /* views that can show a themable folder */

/* The theme files that go into one folder, as paths relative to it ("/sub/file.png"), and the identity of each
   that passed its check. */
typedef struct plan {
  char (*rel)[PATH_MAX];
  fs_id_t *id;   /* the file that passed its check (ino 0: none taken), what is staged is checked against */
  int count, cap, skipped;
} plan_t;

/* A place to mount a themed folder: the folder itself or one of the system's views of it. */
typedef struct job {
  char on[PATH_MAX];      /* mount point */
  char real[PATH_MAX];    /* real folder whose themed copy is mounted there */
  int  view;              /* 1 if on is a nullfs view of real, not real itself */
} job_t;

/* A themed copy of a folder, staged under STAGE_DIR. */
typedef struct stage {
  char   real[PATH_MAX];
  char   dir[PATH_MAX];
  plan_t plan;
  int    state;           /* 0 staged and verified, -1 failed */
  int    counted;
  unsigned long long fp_inputs;   /* the stock copy and the theme files it was built from */
  unsigned long long fp_staged;   /* the theme files as they are in dir */
} stage_t;

long long      mono_ms(void);
int            plan_jobs(job_t *jobs, int *failed, int quiet);
int            mount_overlay(const char *src, const char *on);
int            relevant_views(plat_view_t *out, int max);   /* every view of a themable folder */
int            verify_plan(const plan_t *p, const char *theme_dir, const char *other_dir, const char *what);
/* The applied theme: its name ("" when none), and its verified stage folder for a real folder (NULL if none). */
const char    *live_theme_name(void);
const stage_t *live_stage_for(const char *real);
int            live_stage_count(void);

#endif
