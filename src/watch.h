/* watch.c: views of the themed folders that the system makes after an apply (apps starting again in new sandboxes,
   after rest mode), covered from the same stage folders; and an app that keeps restarting with the theme on. */
#ifndef PS5_THEME_WATCH_H
#define PS5_THEME_WATCH_H

int         watch_views(int reset);   /* 1: an app kept restarting (watch_crash_app): take the theme down */
void        watch_look_now(void);     /* at the next look, though the mount table looks the same */
const char *watch_crash_app(void);
#ifdef TEST_HOOKS
void        watch_set_windows(int fast, int slow);   /* the crash-loop windows, seconds */
#endif

#endif
