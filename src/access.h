/* access.c: who may open the manager. */
#ifndef PS5_THEME_ACCESS_H
#define PS5_THEME_ACCESS_H

#include <stddef.h>
#include <stdint.h>

enum { ACCESS_PAIRED, ACCESS_NETWORK, ACCESS_CONSOLE };
#define ACCESS_COOKIE "ps5theme"

typedef struct access_who {
  uint32_t peer;      /* the asking device's IPv4 address */
  int console;        /* it is the console itself */
  int device;         /* an allowed device (its slot), or -1 */
  int mode;
} access_who_t;

typedef struct access_device {
  char id[9];         /* the first 4 bytes of its key's hash, in hex */
  long long added, seen;
  char addr[16], name[48];
  int this_one;       /* the device asking */
} access_device_t;

void        access_init(void);
int         access_mode(void);
int         access_set_mode(int mode);
const char *access_mode_name(int mode);
int         access_mode_parse(const char *s);
/* 0 when the request may go on; else the HTTP status (401: a code is needed; 403) and why. `open`: what a device not
   yet allowed may still ask for: the page itself (which then says what to do), or a code. */
enum { ACCESS_OPEN_NOT = 0, ACCESS_OPEN_PAIR = 1, ACCESS_OPEN_PAGE = 2 };
int         access_gate(int fd, const char *host, const char *cookie, int open, access_who_t *who, char *why, size_t len);
int         access_pair_start(const access_who_t *who, char *why, size_t len);
int         access_pair_finish(const access_who_t *who, const char *digits, const char *ua, char key[65], char *why,
                               size_t len);
int         access_list(access_device_t *out, int max, const access_who_t *who);
int         access_forget(const char *id);   /* an id, or "all": how many, or -1 */

#endif
