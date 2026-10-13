/* ps5-klog: the console's kernel log on TCP port 3232, for finding out what
   the system is doing (ShellUI, the launcher and other system processes print
   their debug messages there). Read-only: it reads /dev/klog and changes
   nothing. The last few megabytes are kept, so a client that connects after
   something happened still gets it, then everything new as it comes.

     nc <console-ip> 3232 > klog.txt

   Start it from a payload loader (port 9021) or PS5 Payload Manager. Only one
   process can read /dev/klog at a time: if etaHEN's own log server is running,
   this one says so and exits. */

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <ps5/kernel.h>

#define PORT        3232
#define RING_BYTES  (4u << 20)
#define MAX_CLIENTS 4
#define LISTEN_CHECK_SECONDS 5

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static char ring[RING_BYTES];
static size_t head, filled;


static void
note(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, msg, sizeof(req.message) - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}


/* Root, full capabilities, and the real filesystem rather than a sandbox
   view (as the theme plugin does), so /dev/klog can be opened. */
static int
raise_privileges(void) {
  uint8_t caps[16];
  pid_t pid = getpid();
  memset(caps, 0xff, sizeof(caps));
  if(kernel_set_ucred_authid(pid, 0x4800000000010003l) || kernel_set_ucred_caps(pid, caps) ||
     kernel_set_ucred_uid(pid, 0) || kernel_set_ucred_ruid(pid, 0)) return -1;
  intptr_t root = kernel_get_root_vnode();
  if(!root || kernel_set_proc_rootdir(pid, root) || kernel_set_proc_jaildir(pid, root)) return -1;
  return 0;
}


static void
ring_put(const char *p, size_t n) {
  while(n) {
    size_t step = RING_BYTES - head < n ? RING_BYTES - head : n;
    memcpy(ring + head, p, step);
    head = (head + step) % RING_BYTES;
    filled = filled + step > RING_BYTES ? RING_BYTES : filled + step;
    p += step;
    n -= step;
  }
}


static int
send_all(int fd, const char *p, size_t n) {
  while(n) {
    ssize_t w = send(fd, p, n, 0);
    if(w < 0 && errno == EINTR) continue;
    if(w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}


/* What is kept, oldest first. */
static int
send_ring(int fd) {
  size_t start = (head + RING_BYTES - filled) % RING_BYTES;
  size_t first = RING_BYTES - start < filled ? RING_BYTES - start : filled;
  return send_all(fd, ring + start, first) || send_all(fd, ring, filled - first) ? -1 : 0;
}


/* The listening socket, non-blocking. Rest mode can leave it failing every
   accept() for good (errno 0xA3, as etaHEN's servers see): it is then
   opened again. Up to `attempts` tries a second apart (0: until it works). */
static int
open_listener(int attempts) {
  struct sockaddr_in addr;
  int one = 1;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  for(int tries = 0; !attempts || tries < attempts; tries++) {
    if(tries) sleep(1);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv < 0) continue;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(!bind(srv, (struct sockaddr *)&addr, sizeof(addr)) && !listen(srv, MAX_CLIENTS)) {
      fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);
      return srv;
    }
    int saved = errno;
    close(srv);
    errno = saved;
  }
  return -1;
}


int
main(void) {
  int clients[MAX_CLIENTS];
  char buf[16384];

  signal(SIGPIPE, SIG_IGN);
  if(raise_privileges()) {
    note("ps5-klog: could not get privileges. Is the console jailbroken?");
    return 1;
  }
  int klog = open("/dev/klog", O_RDONLY | O_NONBLOCK);
  if(klog < 0) {
    snprintf(buf, sizeof(buf), "ps5-klog: cannot read the kernel log (%s). Is another log server running?", strerror(errno));
    note(buf);
    return 1;
  }

  int srv = open_listener(5);
  if(srv < 0) {
    snprintf(buf, sizeof(buf), "ps5-klog: port %d is not available (%s)", PORT, strerror(errno));
    note(buf);
    return 1;
  }
  time_t checked = time(NULL);
  for(int i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;
  note("ps5-klog: kernel log on port 3232");

  for(;;) {
    /* Polled with a short timeout and a non-blocking read each time, so it
       works whether or not /dev/klog supports poll. */
    struct pollfd pf[2] = {{klog, POLLIN, 0}, {srv, POLLIN, 0}};
    poll(pf, 2, 200);

    ssize_t n;
    while((n = read(klog, buf, sizeof(buf))) > 0) {
      ring_put(buf, (size_t)n);
      for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i] >= 0 && send_all(clients[i], buf, (size_t)n)) {
          close(clients[i]);
          clients[i] = -1;
        }
      }
    }

    int c = -1;
    if(pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      errno = EPIPE;
    } else if((pf[1].revents & POLLIN) || time(NULL) - checked >= LISTEN_CHECK_SECONDS) {
      /* Also when nothing seems to arrive: a broken socket may not say so. */
      checked = time(NULL);
      c = accept(srv, NULL, NULL);
      if(c < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)) continue;
    } else {
      continue;
    }
    if(c < 0) {
      close(srv);
      srv = open_listener(0);
      continue;
    }
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
    struct timeval tv = {2, 0};   /* a client that stops reading is dropped */
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int slot = -1;
    for(int i = 0; i < MAX_CLIENTS && slot < 0; i++) if(clients[i] < 0) slot = i;
    if(slot < 0 || send_ring(c)) {
      close(c);
      continue;
    }
    clients[slot] = c;
  }
}
