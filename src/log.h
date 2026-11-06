/* log.c: the plugin's log, and what it says on the console. */
#ifndef PS5_THEME_LOG_H
#define PS5_THEME_LOG_H

void log_init(void);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A notification on the console, one line. */
void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Two lines with the plugin's icon beside them, as etaHEN's own (TOAST_ICON). */
void toast(const char *title, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Seconds on a clock that only goes forward, for waits and intervals: the console sets its own clock from the
   network after it starts, and a wall-clock interval measured across that jump stalls or ends early. */
long long mono_seconds(void);

#endif
