/* ps5-theme: custom home-screen themes on a jailbroken PS5 (etaHEN plugin).

   main() starts the plugin's threads (the home screen's guard, the web server, the icon styler, the tile) and then
   runs the engine: whenever the chosen theme changes it applies it (apply.c), and between applies it covers the
   views the system makes after one (watch.c). */

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apply.h"
#include "autostyle.h"
#include "config.h"
#include "crash.h"
#include "diag.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "platform.h"
#include "prefs.h"
#include "shellui.h"
#include "status.h"
#include "store.h"
#include "theme.h"
#include "tile.h"
#include "update.h"
#include "watch.h"

#define POLL_SECONDS     1
#define UPDATE_CHECK_SECONDS 10
#define HANDOFF_ASK_SECONDS  30    /* for a newer copy this one started to ask to take over */

#define TAKEOVER_ASK_SECONDS 10    /* for the running copy's web server to answer */
#define WEB_STACK_SIZE   (1024 * 1024)

static atomic_int tile_done;

static void *
tile_thread(void *arg) {
  (void)arg;
  crash_thread_init(CRUMB_TILE);
  crumb(CRUMB_TILE, "registering the home-screen tile");
  tile_install();
  atomic_store(&tile_done, 1);
  crumb(CRUMB_TILE, "done");
  crash_thread_exit(CRUMB_TILE);
  return NULL;
}

int
main(void) {
  char fw[16], active[128], current[128] = {0}, taken_down[256] = "";
  pthread_t web;
  pthread_attr_t attr;

  /* Visible before anything can fail, so a silent start is impossible. */
  crash_install();
  crash_on_fatal(crash_release);
  /* (one notification when it is ready, not one now and another two seconds later; failures before then say so) */
  plat_klog("[ps5-theme] " PLUGIN_VERSION " starting\n");

  /* Privileges first: until then the process may only see a sandbox. */
  crumb(CRUMB_MAIN, "raising privileges");
  if(plat_init()) {
    plat_notify("ps5-theme: could not get privileges. Is the console jailbroken?");
    plat_klog("[ps5-theme] could not raise privileges\n");
    return 1;
  }
  if(fs_guard_init()) {
    plat_notify("ps5-theme: cannot create " BASE_DIR);
    plat_klog("[ps5-theme] cannot create " BASE_DIR "\n");
    return 1;
  }
  log_init();
  crash_ready();
  {   /* the icon of its notifications, beside the text as etaHEN's own (TOAST_ICON) */
    extern const char web_icon[], web_icon_end[];
    size_t len = (size_t)(web_icon_end - web_icon);
    uint32_t have = 0;
    uint64_t size = 0;
    if(fs_crc32_file(TOAST_ICON, &have, &size) || size != len || have != crc32_update(0, web_icon, len)) {
      fs_write_file(TOAST_ICON, web_icon, len);
    }
  }
  fw_string(fw, sizeof(fw));
  log_msg("ps5-theme %s starting, pid %d, firmware %s", PLUGIN_VERSION, getpid(), fw);
#ifdef TEST_HOOKS
  /* Shorter crash-loop windows ("fast,slow" seconds, and how long a home
     screen is up for its exit to count as rest mode), so the tests needn't
     wait minutes. */
  const char *windows = getenv("PS5THEME_TEST_CRASH_WINDOWS");
  int fast_s = -1, slow_s = -1, up_s = -1, left_s = -1;
  if(windows) sscanf(windows, "%d,%d,%d,%d", &fast_s, &slow_s, &up_s, &left_s);
  if(fast_s > 0 && slow_s > 0) watch_set_windows(fast_s, slow_s);
  if(up_s >= 0) shellui_up_ms = up_s * 1000;
  if(left_s > 0) shellui_left_ms = left_s * 1000;
#endif

  /* One copy at a time. A newer copy takes over from an older one (an
     update); an older or equal one leaves the running copy alone. */
  if(instance_lock(0)) {
    /* Its web server may be starting, or coming back after rest mode. */
    int running = running_version(), mine = my_version();
    for(int i = 0; running < 0 && i < TAKEOVER_ASK_SECONDS; i++) {
      sleep(1);
      running = running_version();
    }
    if(running >= mine) {
      log_msg("ps5-theme %d.%02d is already running; this copy exits", running / 100, running % 100);
      return 0;
    }
    if(take_over(running)) {
      log_err("the running copy did not hand over; this copy exits");
      notify("ps5-theme: the running copy did not hand over. Start ps5-theme-stop, then ps5-theme again.");
      return 1;
    }
  } else if(running_version() >= 0) {
    /* A copy from before 1.23 holds no lock and can't hand over. */
    log_err("an older copy without hand-over is running; this copy exits");
    notify("ps5-theme: an older copy is still running. Restart the console to finish the update.");
    return 1;
  }
  record_running();
  plat_set_name(TITLE_ID);   /* the name etaHEN knows a running plugin by */
  /* Uploads cut off by the last copy's end (a crash, a power loss): none is being received now, as only the copy
     holding the lock receives any. */
  if(fs_is_dir(INCOMING_DIR) && fs_rm_rf_under(BASE_DIR, INCOMING_DIR)) log_err("could not clear %s", INCOMING_DIR);
  snapshots_recover();

  /* ShellUI is watched from the start: overlays a previous copy left in
     its view must come out if it exits while this one starts up. */
  pthread_t guard;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&guard, &attr, shellui_guard, NULL)) {
    log_err("could not start watching ShellUI; games' and apps' pictures stay off");
  } else {
    pthread_detach(guard);
  }
  pthread_attr_destroy(&attr);
  crash_report_previous();

  fs_mkdirs(THEMES_DIR);
  fs_mkdirs(STAGE_DIR);
  /* What the storage check works from, so a figure that looks wrong can be seen in Activity. */
  uint64_t avail = plat_free_bytes(BASE_DIR);
  if(avail == UINT64_MAX) log_msg("storage: the free space can't be told; nothing is refused for room");
  else log_msg("storage: %llu MB free, %llu MB of it kept for the console", (unsigned long long)(avail >> 20),
               (unsigned long long)(FS_RESERVE >> 20));
  fs_mkdirs(DUMP_DIR);
  fs_mkdirs(UPDATE_DIR);

  /* 1.09's audio output choice; the plugin no longer plays audio itself. */
  if(!access(BASE_DIR "/audio-route", F_OK)) fs_unlink(BASE_DIR "/audio-route");
  /* 1.42 to 1.52's choice of where games' pictures go: there is one way now. */
  if(!access(BASE_DIR "/title-placement", F_OK)) fs_unlink(BASE_DIR "/title-placement");

  /* Overlays from a previous copy (an update, or plugin Stop, which is a hard
     kill) are kept when they are complete and for the selected theme;
     otherwise they are taken down and the theme is applied again. */
  char listing[PATH_MAX];
  asset_listing_path(listing, sizeof(listing));
  read_active(active, sizeof(active));
  int adopted = strcmp(active, "none") && !access(listing, F_OK) && !adopt_live(active);
  if(adopted) {
    char title[128];
    theme_title(active, title, sizeof(title));
    status_adopted(active);
    snprintf(current, sizeof(current), "%s", active);
    set_result("%s is applied (kept through the plugin's restart).", title);
  } else {
    apply_after_reboot();
  }
  dump_mount_table();

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WEB_STACK_SIZE);
  if(pthread_create(&web, &attr, web_server_thread, NULL)) {
    log_err("could not start the web server");
  } else {
    pthread_detach(web);
  }
  pthread_attr_destroy(&attr);

  /* Stock listing for the editor; runs before any theme is mounted. */
  dump_assets_once();
  dump_shell_files();

  /* The icon style for titles installed later: it looks every minute, and works only between applies. */
  pthread_t styler;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&styler, &attr, autostyle_thread, NULL)) log_err("could not start the icon styler");
  else pthread_detach(styler);
  pthread_attr_destroy(&attr);

  /* System app registration can block; keep it off the engine thread. */
  pthread_t tile;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&tile, &attr, tile_thread, NULL)) {
    log_err("could not start the tile installer");
  } else {
    pthread_detach(tile);
  }
  pthread_attr_destroy(&attr);

  long long tile_started = mono_seconds();
  int tile_warned = 0;
  char ip[32];
  lan_address(ip, sizeof(ip));
  if(!pref_on("starts")) log_msg("ready (its notification is turned off in Settings)");
  else if(ip[0]) toast("PS5 Themes is ready", "Open the Themes tile, or %s:%d on a phone or computer.", ip, WEB_PORT);
  else toast("PS5 Themes is ready", "Open the Themes tile.");
  crumb(CRUMB_MAIN, "idle");

  /* The guard's first look decides whether games' pictures may go into
     ShellUI's view in the first apply. */
  for(int i = 0; i < 20 && !shellui_looked(); i++) usleep(100 * 1000);

  long long last_update_check = -1000000;   /* (looked at once at start) */
  for(;;) {
    if(handoff_due()) {
      log_msg("handing over to the new copy; the theme stays as it is");
      _exit(0);   /* the overlays stay; the lock is released with the process */
    }
    if(stop_requested()) {
      int stuck, out = release_titles(&stuck);
      log_msg("stopping on request: the theme stays; %d overlay(s) of games' and apps' pictures taken out until the next copy starts", out);
      if(stuck) log_err("%d overlay(s) of games' and apps' pictures are in use and stay", stuck);
      _exit(0);   /* (the stop tool, which asked, says so on the console) */
    }
    if(mono_seconds() - last_update_check >= UPDATE_CHECK_SECONDS) {
      last_update_check = mono_seconds();
      if(update_check()) {
        /* The newer copy asks to take over as soon as it is up. Wait for
           that instead of starting an apply (at boot, putting the theme
           back) that it would then have to wait for. */
        for(int i = 0; i < HANDOFF_ASK_SECONDS * 10 && !handoff_requested(); i++) usleep(100 * 1000);
        continue;
      }
    }
    /* Say so if the system is slow to register the tile; nothing else waits on it. */
    if(!tile_warned && !atomic_load(&tile_done) && mono_seconds() - tile_started > 45) {
      tile_warned = 1;
      log_err("tile: registration has not finished after 45 s");
      if(ip[0]) notify("ps5-theme: the Themes tile is slow to register. The manager works now at http://%s:%d", ip, WEB_PORT);
      else notify("ps5-theme: the Themes tile is slow to register; the theme manager still works.");
    }
    read_active(active, sizeof(active));
    int forced = status_take_request();

    if(forced || strcmp(active, current)) {
      int theme = strcmp(active, "none") != 0;
      status_apply_begins(theme ? active : "", current[0] != 0);
      if(theme) {
        set_progress(0, "Starting");
        /* Say on the console what is happening: at start it shows stock until the theme is back. */
        char title[128];
        theme_title(active, title, sizeof(title));
        if(!pref_on("progress")) { /* (said only when it is done) */ }
        else if(!current[0]) notify("ps5-theme: putting '%s' back...", title);
        else if(!status_apply_quiet()) notify("ps5-theme: applying '%s'...", title);
        themes_lock();
        int rc = apply_theme(active);
        if(!rc) applied_files_record(active);
        else applied_files_forget();
        themes_unlock();
        status_apply_ends(active, !rc);
      } else {
        apply_none();
        themes_lock();
        applied_files_forget();
        themes_unlock();
        if(taken_down[0]) {
          set_result("%s", taken_down);
          taken_down[0] = 0;
        } else {
          set_result("No theme: the console's own look.");
          if(current[0]) notify("ps5-theme: stock theme restored");
        }
      }
      snprintf(current, sizeof(current), "%s", active);
      crumb(CRUMB_MAIN, "idle");
      watch_views(1);
    }
    if(shellui_records_stale()) tidy_records();
    if(shellui_replan_due()) watch_look_now();
    if(watch_views(0)) {
      /* The console's interface keeps crashing with this theme: back to stock at once, and not again at the next
         start. */
      char title[128];
      theme_title(current, title, sizeof(title));
      snprintf(taken_down, sizeof(taken_down),
               "%s was taken down: the console's interface kept restarting with it applied (%s).", title,
               watch_crash_app());
      notify("ps5-theme: '%s' was taken down because the console's interface kept restarting with it", title);
      if(write_active("none")) log_err("could not select the stock theme");
      continue;
    }
    /* Wait for the next second or a mount change: a new view to cover at
       once (ShellUI reads the home row's icons as it starts). Right after
       ShellUI exited, a few times a second: its successor's views are
       covered as soon as the guard has found its process. */
    plat_wait_events(-1, shellui_replan_pending() ? 0 : shellui_hunting() ? 20 : POLL_SECONDS * 1000);
  }
  return 0;
}
