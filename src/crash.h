/* crash.c: a report of what each thread was doing (its last breadcrumb) when the plugin crashes. */
#ifndef PS5_THEME_CRASH_H
#define PS5_THEME_CRASH_H

#define WEB_WORKERS 4   /* the web server's threads, each with its own breadcrumb slot */
enum { CRUMB_MAIN = 0, CRUMB_LISTEN, CRUMB_TILE, CRUMB_GUARD, CRUMB_WEB, CRUMB_COUNT = CRUMB_WEB + WEB_WORKERS };

void crash_install(void);
void crash_ready(void);   /* after log_init: the folder crash reports go in */
/* Called first on a fatal signal, from the signal handler: system calls and static memory only. */
void crash_on_fatal(void (*fn)(void));
void crash_thread_init(int slot);
void crash_thread_exit(int slot);
void crumb(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void crash_report_previous(void);

#endif
