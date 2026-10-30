# Architecture

How the plugin is put together: its parts, its threads and what they share, what it keeps on the console, and what
happens when something fails. For contributors; the reasons behind each choice are in
[Design decisions](decisions.md), and the user-facing explanation in [How it works](how-it-works.md). Part of the
[PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [The parts](#the-parts)
- [Threads](#threads)
- [What it keeps on the console](#what-it-keeps-on-the-console)
- [Starting, running, handing over, stopping](#starting-running-handing-over-stopping)
- [Applying a theme](#applying-a-theme)
- [The home screen's sandbox](#the-home-screens-sandbox)
- [Locks, and the order they are taken in](#locks-and-the-order-they-are-taken-in)
- [A request through the web server](#a-request-through-the-web-server)
- [When something fails](#when-something-fails)
- [What must always hold](#what-must-always-hold)

## The parts

```
 browser (phone, computer, the console's own)            the console
 ┌───────────────────────────────┐                        ┌───────────────────────────────────────────────┐
 │ web/index.html                │   HTTP, port 8095      │ ps5-theme (one process, plain C, no libraries)│
 │  the manager: library, editor │ ─────────────────────► │  web server ──► /data/ps5-theme (themes, ...) │
 │  encoders: BC7, ATRAC9, zstd, │   JSON API, uploads    │  theme engine ─► stock copies ─► stage folders │
 │  PNG; resource-file editing   │ ◄───────────────────── │       │                                       │
 └───────────────────────────────┘                        │       └─► read-only nullfs overlays on the     │
                                                          │           system's folders and its sandbox views│
 ps5-theme-stop.elf ── /api/stop (from the console only) ─►│  ShellUI guard, icon styler, tile installer   │
                                                          └───────────────────────────────────────────────┘
```

- **The manager** (`web/index.html`, `web/atrac9.js`) runs in the browser and does all the media work: fitting
  pictures, encoding BC7 textures, zstd frames, PNG, ATRAC9 audio, and editing interface resource files. It sends
  finished files, each with its checksum.
- **The plugin** (`src/`) stores those files, checks each against the system file it replaces, and shows them with
  read-only overlays. It never writes the system, and never runs code in another process.
- **The stop tool** (`tools/stop/`) asks a running copy to stop and leave its theme, over the web port, or by a
  signal when that doesn't answer.

Which file does what is in [Building](building.md#project-layout).

## Threads

| Thread | Started in | Does |
|---|---|---|
| main (theme engine) | `main()` | applies themes, covers new sandbox views (the watcher), takes a theme down if an app keeps crashing with it, looks for updates, hands over, stops |
| web listener | `web_server_thread` | accepts connections, queues them (32), answers `503 busy` when full; listens again if rest mode breaks the socket |
| web workers (4) | `start_workers` | read a request, route it, answer; uploads, settings, Save/Discard, the status |
| ShellUI guard | `shellui_guard` | finds the home screen's process, watches it exit, takes the theme's overlays out of its sandbox at once |
| icon styler | `autostyle_thread` | notices games installed later, styles their icons once the install settles, asks for a quiet re-apply |
| tile installer | `tile_thread` | registers the Themes tile once, then ends |

The crash handler (`src/crash.c`) runs on whichever thread faults, on its own stack, using only system calls and
static memory.

## What it keeps on the console

Everything under `/data/ps5-theme/` (`BASE_DIR`), the only place the write guard (`src/fsguard.c`) allows writes,
apart from the tile's folder and the plugin's own copies in etaHEN's and Payload Manager's folders when it updates
them.

| Path | What | Written by |
|---|---|---|
| `themes/<id>/` | a theme: `theme.ini`, `files/<system path>`, music, `choices.json`, `icon-style/`, kept originals | web workers, styler (under the themes lock) |
| `snapshots/<id>/` | the theme as it was at its last save, for Discard | web workers |
| `incoming/` | uploads being received; emptied at each start | web workers, styler |
| `stock/<firmware>/<path>` | a verified copy of each system folder a theme touches, once per firmware; `.complete/<path>.ok` marks a whole one, `.ok.fp` holds a game folder's fingerprint | main |
| `stage/g<generation>-<n>/` | the folders actually mounted: the stock copy plus the theme's files | main |
| `mounts` | one line per overlay mounted: mount point, source, system folder; appended as each goes on | main |
| `live`, `spare` | the applied theme's stage folders and their fingerprints; the previous theme's, kept to switch back | main |
| `active` | the theme chosen ("none" for the console's own look) | web workers |
| `checked` | remembered results of the file checks, by each file's identity | any thread (its own lock) |
| `update/` | a newer `ps5-theme.elf` or `.plugin` to take over | you |
| `logs/` | `log.txt` (rotated at 512 KB), crash reports | any thread (its own lock) |
| `dump/` | the mount table and asset listings, for diagnosing | main, web workers |
| `instance.lock`, `pid` | one running copy at a time, and how another program finds it | main |

Files are written whole: a temporary name of its own, synced, renamed over the old one (`fs_write_file`). Theme,
stock and stage files are only ever replaced, never changed in place, which is what lets a file's identity (inode,
size, time) stand for its content.

## Starting, running, handing over, stopping

**Start** (`main()`): privileges; the write guard; the log; take the instance lock, or, with a copy already running,
ask it to hand over if this one is newer, and exit if not; record the pid; empty `incoming/`; put back a theme a Discard left aside; start the
ShellUI guard and let it look once; start the web server, the styler and the tile; then the main loop. The first pass
puts the chosen theme back, carrying on with a previous copy's overlays when they are all there and the same.

**The main loop**, once a second or when the mount table changes: hand over if a newer copy asked; stop if asked; look
in the update folders (every few seconds); apply if the chosen theme changed or a re-apply was asked for; tidy the
overlay list after the guard took some out; cover new sandbox views; wait.

**Hand-over:** a newer copy asks over `/api/handoff` (from the console only), with its pid. The running copy finishes
what it is doing, checks that copy still exists, and exits leaving every overlay in place; the new copy takes the lock
and carries on with them. **Stop** is the same, but the home screen's games' and apps' pictures come out first.

## Applying a theme

`apply_theme()` in `src/main.c`, on the main thread:

1. **Targets:** every folder the theme changes files in (`collect_targets`), at most 256.
2. **Jobs:** each target, and every view of it the system has made, as a mount point (`plan_jobs`). A game's folder is
   only ever overlaid in the home screen's own view.
3. **Stock copies:** made once per firmware; a game's is checked against its folder each time, by fingerprint first,
   by content only when that changed.
4. **Stage folders,** while the previous theme still shows: the stock copy plus the theme's files, each checked. One
   built from the very same files as before is reused as it is.
5. **Swap:** the previous overlays come down (newest first; one still in use stays and the new one goes on top) and
   the new ones go up, each added to `mounts` as it does.
6. **Check:** every file read back through every live mount, as the home screen will see it.
7. Save `live`, say the result, and log how long each step took.

## The home screen's sandbox

The home screen (`SceShellUI`, sandbox `/mnt/sandbox/NPXS40087_<n>`) is restarted by the system at rest mode and when
it crashes. The guard and the engine follow it:

```
 looking ──found──► watching <n> ──exit──► between: overlays out of <n>, nothing into any sandbox
    ▲                                         │
    └──────────── found the next one ◄────────┘   (looked for every 10 ms for 30 s)
```

- Into the home screen's sandbox: only while its process is watched, and only the watched instance's.
- Into any app's sandbox: never between instances of the home screen; otherwise once a view has been there 2 seconds.
- Out: the guard takes everything of ours out of the home screen's sandbox the moment its process exits; the crash
  handler does the same if the plugin dies.

See [How it works](how-it-works.md#reaching-the-home-screen-through-its-sandbox-views) and
[Design decisions](decisions.md#nothing-in-a-sandbox-while-the-system-builds-it).

## Locks, and the order they are taken in

| Lock | Kind | Guards |
|---|---|---|
| `themes_mutex` (`themes_lock()`) | mutex | a theme's files and folders: every write, an apply, Save, Discard, Duplicate, the styler's placing |
| `swap_lock` | rwlock | a theme's folder being moved aside and back (Discard writes it); the status and short reads of a theme read it |
| `state_lock` | mutex | the applying/applied/progress fields of the status, the quiet-apply note |
| `walks_lock`, `checked_lock`, `queue_lock`, `log_lock`, `seen_lock`, `awaiting_lock`, `dead_lock` | mutex | one cache or queue each, held briefly, nothing taken inside |

The order: `themes_lock` before `swap_lock`. No request that only reads takes `themes_lock` (checked by walking the
call graph of every read-only route), so a reader holding `swap_lock` never waits on it. A thread that holds
`swap_lock` to read marks itself (`swap_reading`, thread-local) and never takes it again. Everything the engine keeps
(the overlay list, stage folders, the watcher's tables) is main-thread only; flags shared with other threads are
atomics. ThreadSanitizer runs the whole suite on every push.

## A request through the web server

1. The listener accepts, sets per-read timeouts (5 s receiving, 15 s sending), queues it.
2. A worker reads the request line and headers: 16 KB at most, 10 seconds in all.
3. Routing (`route()`): `GET`s read; anything that changes something needs the `X-PS5-Theme` header (a page from
   another site can't send it), and `/api/handoff` and `/api/stop` must come from the console itself.
4. **Uploads:** received into `incoming/` with a running CRC32, checked against the browser's checksum, then against
   the system file (format, size, structure), then moved into the theme under the themes lock, its folders made
   there.
5. Every answer closes the connection. The page and pictures carry an `ETag` and are answered "not modified" when
   unchanged; the page is sent compressed to a browser that takes it.

## When something fails

| What | What happens |
|---|---|
| The plugin crashes | the crash handler takes the theme's overlays out of the home screen's sandbox, writes a report; the next start carries on |
| It is killed outright | overlays stay; the next start carries on with them or takes them down and applies again |
| Power loss | overlays go with it; the next start applies the chosen theme; a half-written record line is ignored; a Discard cut off is put right |
| The home screen keeps crashing | 3 restarts in 60 s, or 5 in 10 minutes: the theme is taken down and not put back at the next start |
| An overlay is in use | never forced; the next one goes on top; it comes off once free |
| Storage nearly full | 1 GB is always left for the console; uploads, copies and stage folders that would eat into it are refused |
| An update copied in half | not started until whole and unchanged at two looks |
| A request that never finishes | cut off after 10 s (headers) or a read that waits 5 s |
| The console's clock jumps | waits use a clock that only goes forward |
| A theme folder too deep to walk | never walked; not deleted in part |

## What must always hold

The tests check these after every combination of actions ([Testing](testing.md)):

- Every overlay is read-only; nothing under `/system` or `/system_ex` is ever opened for writing.
- Every overlay is recorded, and every record is mounted.
- The console shows exactly the applied theme's file, or its own.
- Nothing of a theme is in a home screen's sandbox that isn't being watched, or in any sandbox while the home screen
  restarts.
- No temporary file, stage folder or upload is left behind.
- A theme's file is only ever replaced whole, never changed in place.
