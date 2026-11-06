/* The plugin itself: who may open the manager, its settings, what it keeps on the console's storage, and handing
   over to a newer copy or stopping. */

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "access.h"
#include "api.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "platform.h"
#include "prefs.h"
#include "status.h"
#include "theme.h"
#include "update.h"

/* The shown picture, or the one named by `kind`. */
/* Whether the request comes from a process on the console itself. */
static int
from_console(request_t *req) {
  struct sockaddr_in peer = {0};
  socklen_t len = sizeof(peer);
  return !getpeername(req->fd, (struct sockaddr *)&peer, &len) && peer.sin_family == AF_INET &&
         ntohl(peer.sin_addr.s_addr) == INADDR_LOOPBACK;
}

/* A newer copy of the plugin asks this one to step aside. Only a process on
   the console itself may ask. */
void
api_handoff(request_t *req) {
  if(!from_console(req)) return respond_error(req->fd, 403, "only the console itself can ask for this");
  char pid[16] = "";
  get_param(req->query, "pid", pid, sizeof(pid));
  log_msg("a newer copy of the plugin asked to take over");
  respond_ok(req->fd);
  request_handoff(atoi(pid));
}

/* ps5-theme-stop asks this copy to stop and leave the theme, as its signal (SIGUSR2) does, here: no looking for the
   process, which the console doesn't always allow. Only a process on the console itself may ask. */
void
api_stop(request_t *req) {
  if(!from_console(req)) return respond_error(req->fd, 403, "only the console itself can ask for this");
  log_msg("ps5-theme-stop asked this copy to stop");
  respond_ok(req->fd);
  request_stop();
}

/* Who may open the manager, and (to a device allowed to) the devices allowed. */
void
api_access(request_t *req) {
  int m = access_mode();
  int allowed = req->who.console || m == ACCESS_NETWORK || (m == ACCESS_PAIRED && req->who.device >= 0);
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"mode\":\"%s\",\"here\":\"%s\",\"allowed\":%s", access_mode_name(m),
            req->who.console ? "console" : "device", allowed ? "true" : "false");
  if(allowed) {
    access_device_t d[32];
    int n = access_list(d, 32, &req->who);
    SB_LIT(&sb, ",\"devices\":[");
    for(int i = 0; i < n; i++) {
      sb_printf(&sb, "%s{\"id\":\"%s\",\"added\":%lld,\"seen\":%lld,\"addr\":\"%s\",\"name\":", i ? "," : "",
                d[i].id, d[i].added, d[i].seen, d[i].addr);
      sb_json_str(&sb, d[i].name);
      sb_printf(&sb, ",\"thisOne\":%s}", d[i].this_one ? "true" : "false");
    }
    SB_LIT(&sb, "]");
  }
  SB_LIT(&sb, "}");
  respond_json(req->fd, 200, &sb);
}

void
api_pair_start(request_t *req) {
  char why[160];
  int st = access_pair_start(&req->who, why, sizeof(why));
  if(st) return respond_error(req->fd, st, why);
  static const char ok[] = "{\"ok\":true,\"seconds\":120}";
  respond(req->fd, 200, "application/json", ok, sizeof(ok) - 1);
}

/* The code from the console: the device gets its key, as a cookie only the manager's own requests send. */
void
api_pair_finish(request_t *req) {
  char digits[16] = "", key[65], why[160], head[512];   /* (zeroed: the check reads six, whatever was sent) */
  get_param(req->query, "code", digits, sizeof(digits));
  int st = access_pair_finish(&req->who, digits, req->ua, key, why, sizeof(why));
  if(st) return respond_error(req->fd, st, why);
  static const char body[] = "{\"ok\":true}";
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                   "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                   "Set-Cookie: " ACCESS_COOKIE "=%s; Path=/; Max-Age=34560000; HttpOnly; SameSite=Strict\r\n"
                   "Connection: close\r\n\r\n", sizeof(body) - 1, key);
  if(!send_all(req->fd, head, (size_t)n)) send_all(req->fd, body, sizeof(body) - 1);
}

void
api_set_access(request_t *req) {
  char mode[16];
  get_param(req->query, "mode", mode, sizeof(mode));
  int m = access_mode_parse(mode);
  if(m < 0) return respond_error(req->fd, 400, "mode must be paired, network or console");
  if(access_set_mode(m)) return respond_error(req->fd, 500, "could not keep the setting");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"mode\":\"%s\"}", access_mode_name(m));
  respond_json(req->fd, 200, &sb);
}

void
api_forget_device(request_t *req) {
  char id[16];
  get_param(req->query, "id", id, sizeof(id));
  if(strcmp(id, "all") && (strlen(id) != 8 || strspn(id, "0123456789abcdef") != 8)) {
    return respond_error(req->fd, 400, "no such device");
  }
  int n = access_forget(id);
  if(n < 0) return respond_error(req->fd, 500, "could not forget the device");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"forgotten\":%d}", n);
  respond_json(req->fd, 200, &sb);
}

/* ---- settings that aren't a theme's (prefs.c) ---- */

void
api_settings(request_t *req) {
  strbuf_t sb = {0};
  SB_LIT(&sb, "{");
  for(int i = 0; pref_name(i); i++) sb_printf(&sb, "%s\"%s\":%s", i ? "," : "", pref_name(i), pref_on(pref_name(i)) ? "true" : "false");
  SB_LIT(&sb, "}");
  respond_json(req->fd, 200, &sb);
}

void
api_set_setting(request_t *req) {
  char name[32], on[8];
  get_param(req->query, "name", name, sizeof(name));
  get_param(req->query, "on", on, sizeof(on));
  if(strcmp(on, "0") && strcmp(on, "1")) return respond_error(req->fd, 400, "on must be 0 or 1");
  int known = 0;
  for(int i = 0; pref_name(i); i++) known |= !strcmp(name, pref_name(i));
  if(!known) return respond_error(req->fd, 400, "no such setting");
  if(pref_set(name, on[0] == '1')) return respond_error(req->fd, 500, "could not keep the setting");
  log_msg("settings: %s %s", name, on[0] == '1' ? "on" : "off");
  api_settings(req);
}

/* ---- what the plugin keeps on the console's storage ---- */

/* Is a recorded overlay, or a mounted one, showing a folder under `prefix`? Such a folder is never removed. */
static int
in_use_under(const char *prefix) {
  char (*points)[1024] = malloc(sizeof(*points) * 64);
  int n = points ? plat_our_mounts(prefix, "/", points, 64) : -1;
  free(points);
  if(n > 0) return 1;
  FILE *f = fopen(MOUNTS_FILE, "r");   /* (what the records say, where the mount table can't tell: emulated mounts) */
  char line[PATH_MAX + 800];
  int used = 0;
  while(f && !used && fgets(line, sizeof(line), f)) {
    char *src = strchr(line, '\t');
    used = src && !strncmp(src + 1, prefix, strlen(prefix));
  }
  if(f) fclose(f);
  return used;
}

/* The copies of the system's folders for firmware other than this console's: left after a firmware update, never
   used again while it runs this one. Their names, and their bytes in all. */
static int
other_firmware(char names[][16], int max, uint64_t *bytes) {
  char fw[16];
  fw_string(fw, sizeof(fw));
  DIR *d = opendir(STOCK_DIR);
  struct dirent *e;
  int n = 0;
  *bytes = 0;
  while(d && n < max && (e = readdir(d))) {
    char p[PATH_MAX];
    if(e->d_name[0] == '.' || !strcmp(e->d_name, fw) || strlen(e->d_name) >= 16) continue;
    snprintf(p, sizeof(p), "%s/%s", STOCK_DIR, e->d_name);
    if(!fs_is_dir(p)) continue;
    snprintf(names[n++], 16, "%s", e->d_name);
    *bytes += fs_tree_bytes(p);
  }
  if(d) closedir(d);
  return n;
}

void
api_storage(request_t *req) {
  char fw[16], here[PATH_MAX], names[16][16];
  uint64_t other;
  fw_string(fw, sizeof(fw));
  snprintf(here, sizeof(here), "%s/%s", STOCK_DIR, fw);
  int n = other_firmware(names, 16, &other);
  uint64_t avail = plat_free_bytes(BASE_DIR);
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"firmware\":\"%s\",\"themes\":%llu,\"copies\":%llu,\"ready\":%llu,\"other\":%llu,\"logs\":%llu,"
            "\"room\":%lld,\"otherFirmware\":[", fw,
            (unsigned long long)(fs_tree_bytes(THEMES_DIR) + fs_tree_bytes(SNAP_DIR) + fs_tree_bytes(INCOMING_DIR)),
            (unsigned long long)fs_tree_bytes(here), (unsigned long long)fs_tree_bytes(STAGE_DIR), (unsigned long long)other,
            (unsigned long long)(fs_tree_bytes(LOG_DIR) + fs_tree_bytes(DUMP_DIR)),
            avail == UINT64_MAX ? -1LL : avail > FS_RESERVE ? (long long)(avail - FS_RESERVE) : 0LL);
  for(int i = 0; i < n; i++) sb_printf(&sb, "%s\"%s\"", i ? "," : "", names[i]);
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}

/* Remove the copies for other firmware (under the themes lock, as an apply holds it), never one an overlay uses. */
void
api_storage_clean(request_t *req) {
  char what[16], names[16][16];
  uint64_t bytes, freed = 0;
  get_param(req->query, "what", what, sizeof(what));
  if(strcmp(what, "firmware")) return respond_error(req->fd, 400, "only the copies for other firmware can be removed here");
  themes_lock();
  int n = other_firmware(names, 16, &bytes), removed = 0, kept = 0;
  for(int i = 0; i < n; i++) {
    char p[PATH_MAX], prefix[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", STOCK_DIR, names[i]);
    snprintf(prefix, sizeof(prefix), "%s/", p);
    if(in_use_under(prefix)) {
      kept++;
      continue;
    }
    uint64_t b = fs_tree_bytes(p);
    if(!fs_rm_rf_under(STOCK_DIR, p)) {
      freed += b;
      removed++;
    }
  }
  themes_unlock();
  log_msg("storage: removed the copies for %d other firmware, %llu MB%s", removed, (unsigned long long)(freed >> 20),
          kept ? " (some are in use and stay)" : "");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"removed\":%d,\"kept\":%d,\"freed\":%llu}", removed, kept, (unsigned long long)freed);
  respond_json(req->fd, 200, &sb);
}
