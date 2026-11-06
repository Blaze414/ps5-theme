/* shellui.c: the home screen's lifecycle: our overlays out of its sandbox as it exits (rest mode, a crash), and
   its successor's views covered as soon as it is found. */
#ifndef PS5_THEME_SHELLUI_H
#define PS5_THEME_SHELLUI_H

#include <stddef.h>

#define SHELLUI_PROCESS  "SceShellUI"
#define SHELLUI_SANDBOX  "/mnt/sandbox/NPXS40087_"
#define SANDBOX_ROOT     "/mnt/sandbox"
/* Apps that start with the home screen and close with it at every rest (13.42): their sandboxes go with its. */
#define SHELLUI_COMPANIONS { "NPXS40101", "NPXS40102" }

void    *shellui_guard(void *arg);
void     crash_release(void);                  /* on a fatal signal: our overlays out of its sandbox */
int      sandbox_of(const char *on, char *out, size_t size);
int      sandbox_closed(const char *on);       /* in a sandbox whose app has exited: never covered */
int      restart_hold(const char *on);         /* the home screen is restarting: no sandbox is touched */
int      shellui_box_unwatched(const char *on);/* a home screen sandbox other than the watched one's */
int      shellui_found(void);                  /* its process has been found at least once */
int      shellui_looked(void);                 /* the guard's first look is done */
int      shellui_hunting(void);                /* it exited moments ago: the next one is looked for often */
int      shellui_records_stale(void);          /* overlays were taken out: tidy the records (once) */
int      shellui_replan_due(void);             /* a new home screen: cover its views now (once) */
int      shellui_replan_pending(void);
unsigned shellui_up_exits(void);               /* home screens that closed after being up (rest mode, not a crash) */
extern int shellui_up_ms;                      /* how long counts as up (tests shorten it) */
extern int shellui_left_ms;                    /* when what closed with it is looked at, after (tests shorten it) */

#endif
