/* Home-screen tile that opens the theme manager in the console's web browser.

   A deep-link app: only param.json + icon0.png under TILE_DIR, registered with
   AppInstUtil. A URL in deeplinkUri opens the internal WebBrowser (psdevwiki
   Param.json). Mirrors etaHEN's Toolbox card installer: an ownership marker,
   a receipt to skip re-registration, and the authid restored afterwards. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "platform.h"
#include "theme.h"


#define STR_(x) #x
#define STR(x) STR_(x)

#define TILE_OWNER   TILE_DIR "/.ps5-theme-tile"
#define TILE_RECEIPT TILE_DIR "/.ps5-theme-registered"
#define TILE_PARAM   TILE_DIR "/sce_sys/param.json"
#define TILE_ICON    TILE_DIR "/sce_sys/icon0.png"

static const char marker[] = "ps5-theme tile v1\n";

static const char param[] =
  "{\"applicationCategoryType\":0,\"titleId\":\"" TITLE_ID "\","
  "\"deeplinkUri\":\"http://127.0.0.1:" STR(WEB_PORT) "/\","
  "\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
  "\"en-US\":{\"titleName\":\"Themes\"}}}";

extern const char web_icon[], web_icon_end[];


static int
equals_file(const char *path, const void *data, size_t len) {
  char buf[4096];
  const char *p = data;
  int equal = 1;
  FILE *f = fopen(path, "rb");

  if(!f) return 0;
  while(equal && len) {
    size_t want = len < sizeof(buf) ? len : sizeof(buf);
    if(fread(buf, 1, want, f) != want || memcmp(buf, p, want)) equal = 0;
    p += want;
    len -= want;
  }
  if(equal) equal = fgetc(f) == EOF;
  fclose(f);
  return equal;
}


int
tile_install(void) {
  size_t icon_len = web_icon_end - web_icon;
  struct stat st;

  if(!lstat(TILE_DIR, &st)) {
    if(!S_ISDIR(st.st_mode) || !equals_file(TILE_OWNER, marker, sizeof(marker) - 1)) {
      log_err("tile: %s exists and is not ours, leaving it alone", TILE_DIR);
      return -1;
    }
  } else if(errno != ENOENT || fs_mkdirs(TILE_DIR) ||
            fs_write_file(TILE_OWNER, marker, sizeof(marker) - 1)) {
    log_err("tile: cannot create %s", TILE_DIR);
    return -1;
  }

  if(equals_file(TILE_RECEIPT, marker, sizeof(marker) - 1) &&
     equals_file(TILE_PARAM, param, sizeof(param) - 1) &&
     equals_file(TILE_ICON, web_icon, icon_len)) {
    return 0;
  }

  fs_unlink(TILE_RECEIPT);
  if(fs_mkdirs(TILE_DIR "/sce_sys") ||
     fs_write_file(TILE_PARAM, param, sizeof(param) - 1) ||
     fs_write_file(TILE_ICON, web_icon, icon_len)) {
    log_err("tile: cannot write metadata");
    return -1;
  }
  /* Verify what was written before handing it to the system. */
  if(!equals_file(TILE_PARAM, param, sizeof(param) - 1) || !equals_file(TILE_ICON, web_icon, icon_len)) {
    log_err("tile: metadata did not verify after writing");
    return -1;
  }

  if(plat_register_tile(TITLE_ID)) return -1;
  fs_write_file(TILE_RECEIPT, marker, sizeof(marker) - 1);
  notify("ps5-theme: 'Themes' tile added to the home screen");
  return 0;
}
