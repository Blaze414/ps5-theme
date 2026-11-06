/* Logging to LOG_FILE with rotation, plus console notifications. */

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "log.h"
#include "config.h"
#include "fsguard.h"
#include "platform.h"


#define LOG_ROTATE_BYTES (512 * 1024)

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int writing_log;   /* this thread holds log_lock: anything said meanwhile would wait on itself */


void
log_init(void) {
  fs_mkdirs(LOG_DIR);
}


/* log.txt -> log.1.txt -> log.2.txt, oldest dropped. Caller holds log_lock. */
static void
rotate_if_needed(void) {
  struct stat st;

  if(lstat(LOG_FILE, &st) || st.st_size < LOG_ROTATE_BYTES) return;
  fs_unlink(LOG_DIR "/log.2.txt");
  fs_rename(LOG_DIR "/log.1.txt", LOG_DIR "/log.2.txt");
  fs_rename(LOG_FILE, LOG_DIR "/log.1.txt");
}


static void
vlog(const char *level, const char *fmt, va_list ap) {
  char line[1200];
  struct timespec ts;
  struct tm tm;

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d %s ",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec, level);
  if(n < 0) return;
  int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  if(m < 0) return;
  n += m;
  if(n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';

  if(!writing_log) {   /* (a refusal by the write guard while rotating: on stdout only) */
    pthread_mutex_lock(&log_lock);
    writing_log = 1;
    rotate_if_needed();
    int fd = fs_open_append(LOG_FILE, 1);   /* through the write guard, which says nothing here */
    if(fd >= 0) {
      if(write(fd, line, n) < 0) { /* nothing better to do */ }
      close(fd);
    }
    writing_log = 0;
    pthread_mutex_unlock(&log_lock);
  }

  /* Also to stdout, which etaHEN connects to its log socket. */
  fwrite("[ps5-theme] ", 1, 12, stdout);
  fwrite(line, 1, n, stdout);
  fflush(stdout);
}


long long
mono_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec;
}


void
log_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog("INFO ", fmt, ap);
  va_end(ap);
}


void
log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog("ERROR", fmt, ap);
  va_end(ap);
}


/* A notification of two lines with the plugin's icon (plat_toast), said in the log as one. */
void
toast(const char *title, const char *fmt, ...) {
  char detail[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);

  log_msg("notify: %s%s%s", title, *detail ? ": " : "", detail);
  plat_toast(title, detail, TOAST_ICON);
}


void
notify(const char *fmt, ...) {
  char msg[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  log_msg("notify: %s", msg);
  plat_notify(msg);
}
