/* apply.c: a theme applied: its files staged under STAGE_DIR, checked, and mounted read-only over the system's
   folders; and taken down again. Main thread; an apply holds themes_lock. */
#ifndef PS5_THEME_APPLY_H
#define PS5_THEME_APPLY_H

int  apply_theme(const char *name);   /* 0 once applied; the outcome is said (status.h) */
void apply_none(void);                /* the console's own look: every overlay out, stage folders kept for later */
void apply_after_reboot(void);        /* at start, when adopt_live fails: any overlays out, stage folders kept for reuse */
int  adopt_live(const char *active);  /* carry on with a previous copy's overlays: 0 if all are there for `active` */
int  release_titles(int *stuck);      /* on stop: games' and apps' pictures and fonts out of ShellUI's view */
void tidy_records(void);              /* the records of overlays the home screen's guard took out */

/* The folders a theme may change files in, at most (the manager checks a theme against it). */
int  max_theme_folders(void);
/* Titles the applied theme has pictures for that weren't installed when it was applied: how many, up to max. */
int  titles_awaiting_install(char (*out)[16], int max);
/* The files a theme had when it was last applied (with themes_lock held). */
void applied_files_record(const char *name);
void applied_files_forget(void);
int  applied_files_unchanged(const char *name);
int  theme_has_files(const char *name);

#endif
