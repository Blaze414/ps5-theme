/* Who may open the manager.

   The manager changes what the console shows, so it answers only devices on the console's own network, and by default
   only the ones you have allowed. A phone or computer asks once for a code; the console shows it (a notification); the
   code typed on that device gives its browser a key of its own: a cookie, which the console keeps only as its SHA-256.
   The console's own browser (127.0.0.1) is always allowed. Who may open it (the devices you allow, any device on your
   network, or only this console) is kept in BASE_DIR/access; the allowed devices in BASE_DIR/devices.

   Whatever the choice:
   - an address outside the private ranges (the internet, through a forwarded port) is refused;
   - a request that names the console by an internet name (its Host) is refused, so a web page that points its own
     name at the console (DNS rebinding) can't have the browser treat it as the manager's page.
   Requests that change something also need the X-PS5-Theme header (routes.c), and the key's cookie is SameSite=Strict,
   so another site's page can send neither. */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "access.h"
#include "config.h"
#include "fsguard.h"
#include "log.h"
#include "platform.h"
#include "sha256.h"


#define ACCESS_FILE   BASE_DIR "/access"
#define DEVICES_FILE  BASE_DIR "/devices"
#define DEVICES_MAX   32
#define CODE_SECONDS  120     /* a code is good for two minutes */
#define CODE_TRIES    5       /* and five tries: one in 200,000 to guess it */
#define CODES_PER_10M 10      /* new codes, at most, in ten minutes */
#define SEEN_EVERY    86400   /* a device's last use is written down at most once a day */

typedef struct device {
  uint8_t hash[32];
  long long added, seen;      /* unix seconds */
  char addr[16];
  char name[48];
} device_t;

static pthread_mutex_t access_lock = PTHREAD_MUTEX_INITIALIZER;
static int mode = ACCESS_PAIRED, loaded;
static device_t devices[DEVICES_MAX];
static int ndevices;
static struct {
  char digits[7];
  uint32_t asker;             /* only the device that asked may use it */
  long long until;            /* mono_seconds */
  int tries;
  long long shown;            /* when it was last shown (mono_seconds) */
} code;
static long long asked[CODES_PER_10M];   /* when the last codes were made (mono_seconds) */
static int nasked;

static const char *const mode_names[] = { "paired", "network", "console" };


const char *
access_mode_name(int m) {
  return m >= 0 && m < 3 ? mode_names[m] : "paired";
}

int
access_mode_parse(const char *s) {
  for(int i = 0; i < 3; i++) if(s && !strcmp(s, mode_names[i])) return i;
  return -1;
}


static void
hex(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  for(size_t i = 0; i < n; i++) {
    out[2 * i] = d[b[i] >> 4];
    out[2 * i + 1] = d[b[i] & 15];
  }
  out[2 * n] = 0;
}

static int
unhex(const char *s, uint8_t *out, size_t n) {
  for(size_t i = 0; i < n; i++) {
    int v = 0;
    for(int j = 0; j < 2; j++) {
      char c = s[2 * i + j];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if(d < 0) return -1;
      v = v << 4 | d;
    }
    out[i] = (uint8_t)v;
  }
  return 0;
}


/* Called with access_lock. */
static void
load(void) {
  if(loaded) return;
  loaded = 1;
  char line[256];
  FILE *f = fopen(ACCESS_FILE, "r");
  if(f) {
    while(fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = 0;
      if(!strncmp(line, "mode=", 5) && access_mode_parse(line + 5) >= 0) mode = access_mode_parse(line + 5);
    }
    fclose(f);
  }
  f = fopen(DEVICES_FILE, "r");
  if(!f) return;
  while(ndevices < DEVICES_MAX && fgets(line, sizeof(line), f)) {
    device_t d;
    memset(&d, 0, sizeof(d));
    char h[65], addr[16] = "", name[48] = "";
    if(sscanf(line, "%64[0-9a-f]\t%lld\t%lld\t%15[0-9.]\t%47[^\t\r\n]", h, &d.added, &d.seen, addr, name) < 3 ||
       strlen(h) != 64 || unhex(h, d.hash, 32)) {
      continue;
    }
    snprintf(d.addr, sizeof(d.addr), "%s", addr);
    snprintf(d.name, sizeof(d.name), "%s", name);
    devices[ndevices++] = d;
  }
  fclose(f);
}

/* Called with access_lock. */
static int
save_devices(void) {
  char *buf = malloc((size_t)DEVICES_MAX * 200 + 1), h[65];
  if(!buf) return -1;
  size_t o = 0;
  for(int i = 0; i < ndevices; i++) {
    hex(devices[i].hash, 32, h);
    o += (size_t)snprintf(buf + o, 200, "%s\t%lld\t%lld\t%s\t%s\n", h, devices[i].added, devices[i].seen,
                          devices[i].addr, devices[i].name);
  }
  int rc = fs_write_file(DEVICES_FILE, buf, o);
  free(buf);
  return rc;
}


void
access_init(void) {
  pthread_mutex_lock(&access_lock);
  load();
  pthread_mutex_unlock(&access_lock);
}

int
access_mode(void) {
  pthread_mutex_lock(&access_lock);
  load();
  int m = mode;
  pthread_mutex_unlock(&access_lock);
  return m;
}

int
access_set_mode(int m) {
  char line[32];
  if(m < 0 || m > 2) return -1;
  int n = snprintf(line, sizeof(line), "mode=%s\n", mode_names[m]);
  pthread_mutex_lock(&access_lock);
  load();
  int rc = fs_write_file(ACCESS_FILE, line, (size_t)n);
  if(!rc) mode = m;
  pthread_mutex_unlock(&access_lock);
  if(!rc) log_msg("access: the manager now opens for %s", m == ACCESS_PAIRED ? "the devices allowed" :
                  m == ACCESS_NETWORK ? "any device on the network" : "this console only");
  return rc;
}


/* 10/8, 172.16/12, 192.168/16, link-local 169.254/16, loopback, and 100.64/10 (carrier networks, and VPNs such as
   Tailscale that give a device an address there). */
static int
private_v4(uint32_t a) {
  return (a >> 24) == 10 || (a >> 24) == 127 || (a & 0xfff00000u) == 0xac100000u || (a & 0xffff0000u) == 0xc0a80000u ||
         (a & 0xffff0000u) == 0xa9fe0000u || (a & 0xffc00000u) == 0x64400000u;
}

/* The name the browser used for the console (Host, without its port): an IPv4 address, localhost, or a name only a
   local network has (one word, or one ending in .local, .lan, .home, .home.arpa or .internal). Any other name is one
   on the internet, which a web page could have pointed at the console. */
static int
host_ok(const char *host) {
  char h[256];
  size_t n = 0;
  for(; *host && *host != ':' && n < sizeof(h) - 1; host++) h[n++] = (char)tolower((unsigned char)*host);
  h[n] = 0;
  if(!n || n == sizeof(h) - 1) return 0;
  if(*host == ':') {   /* then only a port */
    host++;
    if(!*host || strspn(host, "0123456789") != strlen(host) || strlen(host) > 5) return 0;
  }
  struct in_addr a;
  if(inet_pton(AF_INET, h, &a) == 1 || !strcmp(h, "localhost")) return 1;
  for(size_t i = 0; i < n; i++) if(!isalnum((unsigned char)h[i]) && h[i] != '-' && h[i] != '.') return 0;
  if(!strchr(h, '.')) return 1;
  static const char *const local[] = { ".local", ".lan", ".home", ".home.arpa", ".internal" };
  for(size_t i = 0; i < sizeof(local) / sizeof(*local); i++) {
    size_t k = strlen(local[i]);
    if(n > k && !strcmp(h + n - k, local[i])) return 1;
  }
  return 0;
}

/* The device's key, from the request's cookie, as its hash; 0 if it carries one. */
static int
cookie_hash(const char *cookie, uint8_t hash[32]) {
  const char *p = cookie;
  while(p && (p = strstr(p, ACCESS_COOKIE "="))) {
    if(p == cookie || p[-1] == ' ' || p[-1] == ';') {
      p += sizeof(ACCESS_COOKIE);
      size_t n = strspn(p, "0123456789abcdef");
      if(n != 64) return -1;
      sha256(p, 64, hash);
      return 0;
    }
    p++;
  }
  return -1;
}

/* Called with access_lock: the allowed device holding this key, or -1. Every device is compared in full, whatever
   matches, so how long it takes says nothing about the keys. */
static int
device_of(const uint8_t hash[32]) {
  int found = -1;
  for(int i = 0; i < ndevices; i++) {
    uint8_t diff = 0;
    for(int k = 0; k < 32; k++) diff |= (uint8_t)(devices[i].hash[k] ^ hash[k]);
    if(!diff) found = i;
  }
  return found;
}


int
access_gate(int fd, const char *host, const char *cookie, int open, access_who_t *who, char *why, size_t len) {
  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  memset(who, 0, sizeof(*who));
  who->device = -1;
  memset(&peer, 0, sizeof(peer));
  if(getpeername(fd, (struct sockaddr *)&peer, &plen) || peer.sin_family != AF_INET) {
    snprintf(why, len, "the manager only answers devices on the console's own network");
    return 403;
  }
  who->peer = ntohl(peer.sin_addr.s_addr);
  who->console = who->peer == INADDR_LOOPBACK;
  int priv = private_v4(who->peer);
#ifdef TEST_HOOKS
  const char *pub = getenv("PS5THEME_TEST_PUBLIC");   /* an address the tests can reach, taken as the internet's */
  struct in_addr t;
  if(pub && inet_pton(AF_INET, pub, &t) == 1 && ntohl(t.s_addr) == who->peer) priv = 0;
#endif
  if(!priv) {
    snprintf(why, len, "the manager only answers devices on the console's own network");
    return 403;
  }
  if(host && !host_ok(host)) {
    snprintf(why, len, "open the manager by the console's address, such as http://192.168.1.20:%d", WEB_PORT);
    return 403;
  }
  if(who->console) return 0;

  uint8_t hash[32];
  int has_key = cookie && !cookie_hash(cookie, hash);
  pthread_mutex_lock(&access_lock);
  load();
  int m = mode;
  if(has_key) {
    who->device = device_of(hash);
    long long now = (long long)time(NULL);
    if(who->device >= 0 && (now - devices[who->device].seen > SEEN_EVERY || now < devices[who->device].seen)) {
      devices[who->device].seen = now;
      save_devices();
    }
  }
  pthread_mutex_unlock(&access_lock);
  who->mode = m;
  if(m == ACCESS_CONSOLE && open != ACCESS_OPEN_PAGE) {   /* (the page itself still loads, to say so) */
    snprintf(why, len, "the manager is set to open only on the console itself");
    return 403;
  }
  if(m == ACCESS_NETWORK || who->device >= 0 || open) return 0;
  snprintf(why, len, "this device isn't allowed to open the manager yet");
  return 401;
}


static void
addr_text(uint32_t a, char *out, size_t n) {
  snprintf(out, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

/* A short name for a device, from its browser's User-Agent: "iPhone, Safari". */
static void
device_name(const char *ua, char *out, size_t n) {
  const char *kind = "Device", *browser = "";
  if(!ua) ua = "";
  if(strstr(ua, "PlayStation")) kind = "PlayStation";
  else if(strstr(ua, "iPhone")) kind = "iPhone";
  else if(strstr(ua, "iPad")) kind = "iPad";
  else if(strstr(ua, "Android")) kind = strstr(ua, "Mobile") ? "Android phone" : "Android tablet";
  else if(strstr(ua, "Macintosh")) kind = "Mac";
  else if(strstr(ua, "Windows")) kind = "Windows PC";
  else if(strstr(ua, "CrOS")) kind = "Chromebook";
  else if(strstr(ua, "Linux")) kind = "Linux computer";
  if(strstr(ua, "Edg/")) browser = "Edge";
  else if(strstr(ua, "Firefox/") || strstr(ua, "FxiOS/")) browser = "Firefox";
  else if(strstr(ua, "Chrome/") || strstr(ua, "CriOS/")) browser = "Chrome";
  else if(strstr(ua, "Safari/")) browser = "Safari";
  snprintf(out, n, "%s%s%s", kind, *browser ? ", " : "", browser);
}


int
access_pair_start(const access_who_t *who, char *why, size_t len) {
  char addr[16], msg[200];
  if(who->console || who->device >= 0) return 0;   /* (already allowed) */
  addr_text(who->peer, addr, sizeof(addr));
  long long now = mono_seconds();
  pthread_mutex_lock(&access_lock);
  load();
  if(mode == ACCESS_CONSOLE) {
    pthread_mutex_unlock(&access_lock);
    snprintf(why, len, "the manager is set to open only on the console itself");
    return 403;
  }
  if(mode != ACCESS_PAIRED) {
    pthread_mutex_unlock(&access_lock);
    return 0;
  }
  int fresh = code.digits[0] && code.asker == who->peer && now < code.until && code.tries < CODE_TRIES;
  if(!fresh) {
    int recent = 0;
    for(int i = 0; i < nasked; i++) recent += now - asked[i] < 600;
    if(recent >= CODES_PER_10M) {
      pthread_mutex_unlock(&access_lock);
      snprintf(why, len, "too many codes asked for: try again in a few minutes");
      return 429;
    }
    uint32_t v;
    do {
      if(plat_random(&v, sizeof(v))) {
        pthread_mutex_unlock(&access_lock);
        snprintf(why, len, "the console couldn't make a code");
        return 500;
      }
    } while(v >= 4294000000u);   /* (an even spread over the million codes) */
    snprintf(code.digits, sizeof(code.digits), "%06u", v % 1000000u);
    code.asker = who->peer;
    code.until = now + CODE_SECONDS;
    code.tries = 0;
    code.shown = 0;
    if(nasked == CODES_PER_10M) memmove(asked, asked + 1, sizeof(asked) - sizeof(*asked)), nasked--;
    asked[nasked++] = now;
  } else if(now - code.shown < 5) {   /* shown a moment ago */
    pthread_mutex_unlock(&access_lock);
    return 0;
  }
  code.shown = now;
  snprintf(msg, sizeof(msg), "PS5 Themes: %.3s %.3s is the code for the device at %s. It works for 2 minutes.",
           code.digits, code.digits + 3, addr);
  pthread_mutex_unlock(&access_lock);
  plat_notify(msg);
  log_msg("access: a code was shown on the console for the device at %s", addr);
  return 0;
}


int
access_pair_finish(const access_who_t *who, const char *digits, const char *ua, char key[65], char *why, size_t len) {
  char addr[16];
  addr_text(who->peer, addr, sizeof(addr));
  long long now = mono_seconds();
  pthread_mutex_lock(&access_lock);
  load();
  if(mode != ACCESS_PAIRED || who->console) {
    pthread_mutex_unlock(&access_lock);
    snprintf(why, len, "no code is needed: this device can already open the manager");
    return 409;
  }
  if(!code.digits[0] || code.asker != who->peer) {
    pthread_mutex_unlock(&access_lock);
    snprintf(why, len, "ask the console for a code first");
    return 403;
  }
  if(now >= code.until || code.tries >= CODE_TRIES) {
    memset(&code, 0, sizeof(code));
    pthread_mutex_unlock(&access_lock);
    snprintf(why, len, "that code is no longer valid: ask for a new one");
    return 403;
  }
  code.tries++;
  uint8_t diff = strlen(digits) != 6;
  for(int i = 0; i < 6; i++) diff |= (uint8_t)(digits[i] ^ code.digits[i]) | (uint8_t)!digits[i];
  if(diff) {
    int left = CODE_TRIES - code.tries;
    if(!left) memset(&code, 0, sizeof(code));
    pthread_mutex_unlock(&access_lock);
    log_msg("access: a wrong code from the device at %s", addr);
    snprintf(why, len, left ? "that isn't the code the console shows" : "too many wrong codes: ask for a new one");
    return 403;
  }
  memset(&code, 0, sizeof(code));
  uint8_t raw[32];
  if(plat_random(raw, sizeof(raw))) {
    pthread_mutex_unlock(&access_lock);
    snprintf(why, len, "the console couldn't make a key");
    return 500;
  }
  hex(raw, sizeof(raw), key);
  device_t d;
  memset(&d, 0, sizeof(d));
  sha256(key, 64, d.hash);
  d.added = d.seen = (long long)time(NULL);
  snprintf(d.addr, sizeof(d.addr), "%s", addr);
  device_name(ua, d.name, sizeof(d.name));
  if(ndevices == DEVICES_MAX) {   /* full: the one unused the longest makes room */
    int old = 0;
    for(int i = 1; i < ndevices; i++) if(devices[i].seen < devices[old].seen) old = i;
    devices[old] = devices[--ndevices];
  }
  devices[ndevices++] = d;
  int rc = save_devices();
  if(rc) ndevices--;
  pthread_mutex_unlock(&access_lock);
  if(rc) {
    snprintf(why, len, "the console couldn't keep the device's key");
    return 500;
  }
  log_msg("access: allowed %s at %s", d.name, addr);
  notify("PS5 Themes: %s at %s can now open the manager.", d.name, addr);
  return 0;
}


int
access_list(access_device_t *out, int max, const access_who_t *who) {
  pthread_mutex_lock(&access_lock);
  load();
  int n = 0;
  for(int i = 0; i < ndevices && n < max; i++, n++) {
    hex(devices[i].hash, 4, out[n].id);
    out[n].added = devices[i].added;
    out[n].seen = devices[i].seen;
    snprintf(out[n].addr, sizeof(out[n].addr), "%s", devices[i].addr);
    snprintf(out[n].name, sizeof(out[n].name), "%s", devices[i].name);
    out[n].this_one = who && who->device == i;
  }
  pthread_mutex_unlock(&access_lock);
  return n;
}


int
access_forget(const char *id) {
  char h[9];
  int gone = 0;
  pthread_mutex_lock(&access_lock);
  load();
  for(int i = 0; i < ndevices; ) {
    hex(devices[i].hash, 4, h);
    if(!strcmp(id, "all") || !strcmp(id, h)) {
      devices[i] = devices[--ndevices];
      gone++;
    } else {
      i++;
    }
  }
  int rc = gone ? save_devices() : 0;
  pthread_mutex_unlock(&access_lock);
  if(gone && !rc) log_msg("access: %d device(s) forgotten", gone);
  return rc ? -1 : gone;
}
