/* update.c: one running copy at a time, and handing over to a newer one. */
#ifndef PS5_THEME_UPDATE_H
#define PS5_THEME_UPDATE_H

int  version_value(const char *v);   /* "1.23" -> 123, or -1 */
int  my_version(void);
int  instance_lock(int wait_ms);     /* 0 once this copy holds the lock */
void record_running(void);           /* PID_FILE, once this copy holds the lock */
int  running_version(void);          /* of the copy answering locally, or -1 */
int  take_over(int running);         /* ask the running copy to step aside: 0 once this copy holds the lock */
int  update_check(void);             /* 1 if a newer copy was started from UPDATE_DIR */
int  ask_handoff(void);
void request_handoff(int pid);       /* pid of the newer copy, or 0 if it didn't say */
int  handoff_requested(void);
int  handoff_due(void);              /* asked, and the newer copy is still there */
void request_stop(void);             /* ps5-theme-stop asked (async-signal-safe) */
int  stop_requested(void);

#endif
