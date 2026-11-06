/* status.c: what the engine is doing, for the manager and the console: the theme applied and the one being
   applied, how far it has got, the last outcome, and the theme chosen (ACTIVE_FILE). The main thread applies;
   the web thread asks and chooses. */
#ifndef PS5_THEME_STATUS_H
#define PS5_THEME_STATUS_H

#include <stddef.h>

#define RESULT_MAX 1536   /* an apply's outcome with all its hints (pictures, font, artwork, music) */

/* Held by the engine for a whole apply and by the web thread for any change to a theme's files, so a theme never
   changes underneath staging and verification. */
void themes_lock(void);
void themes_unlock(void);

int  applied_theme(char *out, size_t size);
int  applying_theme(char *out, size_t size);
int  apply_progress(char *text, size_t size);   /* percent, or -1 when idle */
void last_result(char *out, size_t size);

void read_active(char *out, size_t size);       /* the chosen theme, "none" for the console's own look */
int  write_active(const char *name);
/* Apply the chosen theme again if it is still `expect`, said on the console once it is done with `note` (a line, or
   several), not step by step: for icons styled by themselves while a game may be open. -1 if another is chosen. */
int  reapply_quietly(const char *expect, const char *note);

/* The main thread's side of an apply. */
int  status_take_request(void);                       /* 1 once for each apply asked for since */
void status_apply_begins(const char *theme, int shown); /* theme "" for none; shown: one was applied before */
int  status_apply_quiet(void);                        /* this apply is said only once it is done */
void status_apply_ends(const char *theme, int ok);
void status_adopted(const char *theme);               /* a previous copy's overlays carried on with */
void set_progress(int pct, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* pct -1: idle */
void set_result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say_apply_note(void);                            /* the notes asked for with this apply, on the console */

#endif
