/* Crash handling: on a fatal signal, write a report to LOG_DIR and exit
   quietly instead of letting the system's crash reporter take the process.

   The handler only uses async-signal-safe calls (open/write/close/_exit,
   clock_gettime) and formats numbers itself. Each thread has its own
   alternate signal stack so a stack overflow can still be reported.
   Breadcrumbs record what each thread was doing when it died. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "theme.h"


#define ALTSTACK_SIZE (64 * 1024)
#define CRUMB_SIZE    256
#define LAST_CRASH    LOG_DIR "/last-crash"

static char crumbs[CRUMB_COUNT][CRUMB_SIZE];
static pthread_t threads[CRUMB_COUNT];
static int thread_known[CRUMB_COUNT];
static volatile sig_atomic_t in_crash;

/* Present when the linker puts the ELF header in a loaded segment (lld does). */
extern const char __ehdr_start[] __attribute__((weak));


void
crumb(int slot, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(crumbs[slot], CRUMB_SIZE, fmt, ap);
  va_end(ap);
}


static const char *
slot_name(int slot) {
  static const char *const names[CRUMB_COUNT] = {
    [CRUMB_MAIN] = "main (theme engine)", [CRUMB_LISTEN] = "web listener", [CRUMB_TILE] = "tile installer",
    [CRUMB_WEB] = "web worker 1", [CRUMB_WEB + 1] = "web worker 2",
    [CRUMB_WEB + 2] = "web worker 3", [CRUMB_WEB + 3] = "web worker 4",
  };
  return slot >= 0 && slot < CRUMB_COUNT && names[slot] ? names[slot] : "other";
}


/* ---- async-signal-safe output ---- */

typedef struct out { int fd; } out_t;

static void
put(out_t *o, const char *s) {
  size_t n = strlen(s);
  while(n) {
    ssize_t w = write(o->fd, s, n);
    if(w <= 0) return;
    s += w;
    n -= w;
  }
}

static void
put_hex(out_t *o, uint64_t v) {
  char buf[19] = "0x";
  for(int i = 0; i < 16; i++) buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
  buf[18] = 0;
  put(o, buf);
}

static void
put_dec(out_t *o, uint64_t v) {
  char buf[24];
  int i = sizeof(buf) - 1;
  buf[i] = 0;
  do { buf[--i] = '0' + v % 10; v /= 10; } while(v && i);
  put(o, buf + i);
}

static void
put_addr(out_t *o, uint64_t addr) {
  uint64_t base = (uint64_t)(uintptr_t)__ehdr_start;
  put_hex(o, addr);
  if(base && addr >= base && addr - base < 0x10000000) {
    put(o, "  (ps5-theme.elf+");
    put_hex(o, addr - base);
    put(o, ")");
  }
  put(o, "\n");
}

static const char *
signal_name(int sig) {
  switch(sig) {
  case SIGSEGV: return "SIGSEGV (invalid memory access)";
  case SIGBUS:  return "SIGBUS (bus error)";
  case SIGILL:  return "SIGILL (illegal instruction)";
  case SIGFPE:  return "SIGFPE (arithmetic error)";
  case SIGABRT: return "SIGABRT (abort)";
  case SIGSYS:  return "SIGSYS (bad system call)";
  default:      return "fatal signal";
  }
}


static void
fatal_handler(int sig, siginfo_t *info, void *ucontext) {
  if(in_crash) _exit(71);
  in_crash = 1;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  /* LOG_DIR "/crash-<unix time>.txt" built without stdio. */
  char path[160], num[24];
  int i = sizeof(num) - 1;
  uint64_t t = ts.tv_sec;
  num[i] = 0;
  do { num[--i] = '0' + t % 10; t /= 10; } while(t && i);
  strcpy(path, LOG_DIR "/crash-");
  strcat(path, num + i);
  strcat(path, ".txt");

  out_t o = { open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644) };
  if(o.fd < 0) o.fd = 2;

  uint64_t rip = 0, rsp = 0, rbp = 0;
  plat_crash_regs(ucontext, &rip, &rsp, &rbp);

  pthread_t self = pthread_self();
  const char *thread = "other";
  for(int i = 0; i < CRUMB_COUNT; i++) {
    if(thread_known[i] && pthread_equal(self, threads[i])) thread = slot_name(i);
  }

  put(&o, "ps5-theme crash report\n");
  put(&o, "version:   " PLUGIN_VERSION "\n");
  put(&o, "time:      "); put_dec(&o, ts.tv_sec); put(&o, " (unix)\n");
  put(&o, "pid:       "); put_dec(&o, getpid()); put(&o, "\n");
  put(&o, "thread:    "); put(&o, thread); put(&o, "\n");
  put(&o, "signal:    "); put(&o, signal_name(sig)); put(&o, ", code ");
  put_dec(&o, info ? (uint64_t)info->si_code : 0); put(&o, "\n");
  put(&o, "fault at:  "); put_hex(&o, info ? (uint64_t)(uintptr_t)info->si_addr : 0); put(&o, "\n");
  put(&o, "image:     "); put_hex(&o, (uint64_t)(uintptr_t)__ehdr_start); put(&o, "\n");
  put(&o, "rip:       "); put_addr(&o, rip);
  put(&o, "rsp:       "); put_hex(&o, rsp); put(&o, "\n");
  put(&o, "\nlast actions:");
  for(int i = 0; i < CRUMB_COUNT; i++) {
    put(&o, "\n  "); put(&o, slot_name(i)); put(&o, ": ");
    put(&o, crumbs[i][0] ? crumbs[i] : "-");
  }
  put(&o, "\n\nbacktrace (symbolize with: addr2line -e ps5-theme.elf <offset>):\n");
  put(&o, "  #0  "); put_addr(&o, rip);

  /* Frame-pointer walk (built with -fno-omit-frame-pointer). Stops on anything
     implausible; a bad pointer here re-faults into the in_crash exit. */
  uint64_t fp = rbp;
  for(int depth = 1; depth < 32; depth++) {
    if(!fp || (fp & 7) || fp < rsp || fp - rsp > (8u << 20)) break;
    uint64_t next = ((uint64_t *)(uintptr_t)fp)[0];
    uint64_t ret = ((uint64_t *)(uintptr_t)fp)[1];
    if(!ret) break;
    put(&o, "  #"); put_dec(&o, depth); put(&o, depth < 10 ? "  " : " "); put_addr(&o, ret);
    if(next <= fp) break;
    fp = next;
  }
  put(&o, "\nThe theme mounts are read-only and disappear on reboot; the console is unchanged.\n");
  if(o.fd != 2) close(o.fd);

  /* Tell the next start which report to announce. */
  int m = open(LAST_CRASH, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if(m >= 0) {
    out_t mo = { m };
    put(&mo, path);
    close(m);
  }

  out_t so = { 1 };
  put(&so, "[ps5-theme] crashed, report written to ");
  put(&so, path);
  put(&so, "\n");

  /* Exit without a core dump so the system crash reporter is not triggered. */
  _exit(70);
}


static void
stop_handler(int sig) {
  out_t so = { 1 };
  (void)sig;
  put(&so, "[ps5-theme] stopping on request\n");
  _exit(0);
}


static stack_t previous_stack[CRUMB_COUNT];
static void *own_stack[CRUMB_COUNT];


/* Per thread: an alternate stack, so stack overflows can still be reported. */
void
crash_thread_init(int slot) {
  stack_t ss;

  threads[slot] = pthread_self();
  thread_known[slot] = 1;
  ss.ss_sp = malloc(ALTSTACK_SIZE);
  ss.ss_size = ALTSTACK_SIZE;
  ss.ss_flags = 0;
  if(!ss.ss_sp) return;
  if(sigaltstack(&ss, &previous_stack[slot])) {
    log_err("crash: sigaltstack failed: %s", strerror(errno));
    free(ss.ss_sp);
    return;
  }
  own_stack[slot] = ss.ss_sp;
}


/* For threads that finish: put back the stack they had and free ours. */
void
crash_thread_exit(int slot) {
  thread_known[slot] = 0;
  if(!own_stack[slot]) return;
  stack_t off = previous_stack[slot];
  if(!off.ss_sp) {
    memset(&off, 0, sizeof(off));
    off.ss_flags = SS_DISABLE;
  }
  if(!sigaltstack(&off, NULL)) {
    free(own_stack[slot]);
    own_stack[slot] = NULL;
  }
}


void
crash_install(void) {
  static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS };
  struct sigaction sa;

  crash_thread_init(CRUMB_MAIN);

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fatal_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for(size_t i = 0; i < sizeof(fatal) / sizeof(*fatal); i++) sigaction(fatal[i], &sa, NULL);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = stop_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  /* A browser closing its connection must never kill the plugin. */
  signal(SIGPIPE, SIG_IGN);
}


/* Announce a crash from the previous run, once. */
void
crash_report_previous(void) {
  char path[256] = {0};
  int fd = open(LAST_CRASH, O_RDONLY);

  if(fd < 0) return;
  ssize_t n = read(fd, path, sizeof(path) - 1);
  close(fd);
  fs_unlink(LAST_CRASH);
  if(n <= 0) return;

  const char *name = strrchr(path, '/');
  log_err("previous run crashed, report: %s", path);
  notify("ps5-theme recovered from a crash. Report saved as %s (see Activity).", name ? name + 1 : path);
}
