# Challenges and how they were solved

The problems met while building this, and how each was solved. Part of the [PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Writing to a read-write system partition](#writing-to-a-read-write-system-partition)
- [Overlays the home screen couldn't see](#overlays-the-home-screen-couldnt-see)
- [Stock again after rest mode](#stock-again-after-rest-mode)
- [Updating when the Toolbox is gone](#updating-when-the-toolbox-is-gone)
- [Nothing running after a restart](#nothing-running-after-a-restart)
- [The interface that wouldn't start](#the-interface-that-wouldnt-start)
- [A guard that took down every theme](#a-guard-that-took-down-every-theme)
- [Changing game and app icons safely](#changing-game-and-app-icons-safely)
- [PS5 game icons that stayed the same](#ps5-game-icons-that-stayed-the-same)
- [Apps that wouldn't start](#apps-that-wouldnt-start)
- [A manager that stopped answering](#a-manager-that-stopped-answering)
- [A WAV file that never finished checking](#a-wav-file-that-never-finished-checking)
- ["File name too long"](#file-name-too-long)
- [The tile on firmware without a function](#the-tile-on-firmware-without-a-function)
- [Fitting every size and format](#fitting-every-size-and-format)
- [4K textures without a texture tool](#4k-textures-without-a-texture-tool)
- [The Appearance themes' artwork](#the-appearance-themes-artwork)
- [Music no one could hear](#music-no-one-could-hear)
- [No ATRAC9 encoder](#no-atrac9-encoder)
- [The music that ticked](#the-music-that-ticked)
- [Slow encoding in some browsers](#slow-encoding-in-some-browsers)
- [Reading the system's own file while a theme is applied](#reading-the-systems-own-file-while-a-theme-is-applied)
- [Music that starts part-way through](#music-that-starts-part-way-through)
- [A slow start after every reboot](#a-slow-start-after-every-reboot)
- ["It forgot my theme after a reboot"](#it-forgot-my-theme-after-a-reboot)
- [An import that lost the backgrounds](#an-import-that-lost-the-backgrounds)
- [Small things that confused people](#small-things-that-confused-people)
- [Testing without the console](#testing-without-the-console)

## Writing to a read-write system partition

etaHEN makes `/system_ex` writable, and a theme tool that writes there can
brick a console. **Solution:** a write guard that every write must pass
(paths, symlinks and the final component all checked), and themes applied
only as read-only overlays held in memory.

## Overlays the home screen couldn't see

An overlay on the real path is correct when the path is read directly, but
ShellUI reads the system through its own sandbox views, and a mount on the
real path isn't visible through a view.
**Solution:** read the full mount table, find every view that shows a
themable folder, and overlay each one, checking every overlay afterwards.

## Stock again after rest mode

After rest mode the home screen showed stock, and etaHEN's Toolbox stopped
working too. The log showed why: two of ShellUI's apps had restarted in new
sandboxes, and their new views of the system folders had no overlays.
Re-applying fixed it, because a re-apply covers every view that exists at
that moment, but it also forced the old overlay off while those apps were
using it. **Solution:** a watcher that notices new views as soon as they
appear and covers them, with nothing taken down. `/api/mounts` returns the
live mount table, to see what the system made, and
`/api/title/where?id=<title ID>` lists where the system keeps one title's
pictures: the files named after it in the folders ShellUI can see, and the
strings in the system's databases that mention it. `/api/diag/watch?id=<title
ID>&seconds=<n>` reports which of that title's picture files, the console's
and the theme's copies, anything opens or reads in that time.

## Updating when the Toolbox is gone

After rest mode, etaHEN's Toolbox often stopped working, and it was the
only way to restart a plugin, so a new version copied to the console waited
for a full restart. Restarting the plugin also took the theme down and put it
back. **Solution:** the plugin updates itself: it notices a newer plugin
file, starts it through etaHEN's ELF loader and hands over to it, and the new
copy keeps the overlays already in place.

## Nothing running after a restart

After a restart, etaHEN started an older plugin while a newer one waited in
the update folder. The old copy started the newer one, but then carried on
putting the theme back, which took longer than the newer copy waited. The
newer copy gave up, and the old one then stepped aside anyway, leaving
nothing running. **Solution:** a copy that has just started a newer one
waits for it before doing anything else, the newer copy waits much longer,
and a copy steps aside only if the newer copy is still there.

## The interface that wouldn't start

The first numbered test (1.31) changed every picture in all five of the
console's interface files at once. After rest mode, ShellUI crashed each
time it started: a new sandbox every 9 seconds, ten times in 90 seconds, and
then the console gave up and stayed in rest mode until it was turned off at
the power button. Nothing was written to the system, so a restart brought
everything back. Two things in those files differed from Sony's: every PNG
had been written in full colour, while many of the console's are palette,
1-bit or greyscale pictures, and three entries declared as SVG, the app tile
placeholders, hold PNG data, which the test had replaced with SVG text.
Which of these ShellUI failed on isn't known yet. **Solution:** pictures now match the console's bit for bit apart from their
pixels, and the plugin checks that strictly; a picture's kind comes from its
bytes; the test goes one file at a time; and the plugin takes a theme down by
itself when the interface keeps restarting.

## A guard that took down every theme

The first console test of game icons (1.34) seemed to do nothing. The log
showed why: 19 seconds after the theme was applied, the plugin took it down
for an interface crash loop, and then took the next theme down the moment it
was applied. ShellUI had restarted only once. But the console keeps the
sandboxes of earlier restarts (`NPXS40087_000`, `_001` and `_002` were all
there at once), and the guard counted each of those as a fresh restart.
**Solution:** the guard takes the newest sandbox it finds when a theme is
applied as its starting point, and only newer ones count. A host test now
starts with three old sandboxes, applies a theme, and checks that a rest
mode restart is covered, that a crash loop is still caught, and that
applying again with all six sandboxes still there keeps the theme.

## Changing game and app icons safely

Changing the icons of installed games and apps on the home screen took 20
versions (1.33 to 1.53), each tested on a PS5 with firmware 13.42. Most of
them changed something on screen, and several broke the console's ability
to start games until it was restarted. The entries below this one tell each
part in detail; this is the whole story in short.

**Where the pictures were put, and what happened.** The combination that
works is the last three rows together.

| Where the game pictures were put | Home row icons | Game Library and background | Games and apps after rest mode | Tried in |
|---|---|---|---|---|
| `/user/appmeta/<title>` in every view, with its `.dds` textures | PS4 games only | not checked | start | 1.34 to 1.36 |
| The game's installation, `/user/app/<title>/sce_sys`, on the real folder | change | change | **the themed game won't start** | 1.38 |
| The real folders only | unchanged | unchanged | other apps start; a game whose own `sce_sys` folder is overlaid doesn't (1.38) | 1.42 to 1.44 |
| `/original_user`, the system's second view of `/user` | unchanged | unchanged | start | 1.42 to 1.44 |
| The icon service's views (NPXS40102) | unchanged | unchanged | start | 1.42 to 1.44 |
| ShellUI's own view of `/user`, left in place | only after ShellUI restarts | change at once | **no game or app starts (CE-105733-9) until a restart** | 1.38 to 1.44 |
| ShellUI's own view, taken out the moment ShellUI exits | **change after rest mode** | change at once | **start** | 1.46 |
| ...plus PlayStation Network's copies, each in the format it really holds | games with PSN artwork too | change at once | start | 1.48 |
| ...taken out by a thread of the plugin's own, also in the middle of an apply | change after rest mode | change at once | start, also with rest mode during an apply | 1.53 |

**The setbacks, and what resolved each one.**

- **1.34: the theme taken down by mistake.** The crash guard counted old
  sandboxes the system keeps as app restarts. *Resolved* by counting only
  sandboxes that appear after the plugin's first look.
- **1.35 to 1.36: PS5 games' icons stayed the same**, though every view of
  their `/user/appmeta` folder was covered and their textures were written
  too. *Resolved* by a read-only lookup of every file and database string
  naming a title: the console's app database points most PS5 games at their
  installation's `sce_sys` folder.
- **1.38: the themed game wouldn't start, then no app at all.** The game
  starts from its `sce_sys` folder, and an overlay there stopped it. With the
  pictures moved out of the real folders, apps still failed (CE-105733-9),
  and stock didn't help until a restart. 1.39 and 1.40 chased other
  suspects (forced unmounts, the system apps' records), and in 1.41 game
  pictures were turned off. *Resolved* by taking the theme apart section by
  section on the console: the background alone was fine, and game pictures
  broke launching even when they were only in ShellUI's view.
- **1.42 to 1.44: four places tested one at a time.** Three were safe but
  changed nothing. ShellUI's view changed the background and the Game
  Library at once, and the home row only after ShellUI restarted, but it
  broke launching after rest mode. *Resolved* by reading the kernel log
  (`ps5-klog.elf`): at rest mode the system ends ShellUI, and its launcher
  then fails to take ShellUI's sandbox down (EBUSY) because of the overlay
  inside it.
- **1.45: the overlays were never taken out**, because the plugin couldn't
  find ShellUI's process. *Resolved* in 1.46 by reading the console's
  process records by their own size, which differs from the SDK's. That
  version changed the home row's icons after rest mode with every app still
  starting.
- **1.47: Death Stranding's pictures were refused.** Its PlayStation Network
  copies are WebP and JPEG files named `.png`. *Resolved* in 1.48 by writing
  each file in the format it really holds.
- **1.49: the manager stopped answering after rest mode**, and starting the
  plugin again couldn't replace it. *Resolved* in 1.50 and 1.51: the web
  server opens its socket again, a new copy finds the old one by the process
  ID it records and signals it to hand over, and `ps5-theme-stop.elf` stops
  it with the theme left on.
- **1.50: the stop tool did damage.** It looked for the plugin by a thread
  name the console doesn't list, took "not found" for "not running", and
  removed a running copy's overlays, leaving three without a record. The
  console had to be restarted once more. *Resolved* in 1.51: the plugin's
  lock alone says whether a copy runs, and overlays are taken out by what
  the mount table shows, not only by the records.
- **Found while tidying up for release: a gap.** The plugin watched ShellUI
  from the same loop that applies themes, so rest mode during an apply could
  leave the pictures in place. *Resolved* in 1.53 by a thread that only
  watches ShellUI, checked on the console with rest mode in the middle of an
  apply.

**Other fixes along the way (1.31 to 1.53).**

- An overlay still in use is never forced off any more: the new theme, or
  the system's own files, go on top of it, and it comes down once it's free
  (1.39). The crash guard also catches a slow restart loop: 5 restarts
  within 10 minutes.
- A sandbox the system kept from an earlier ShellUI is never covered, even
  in the moment between ShellUI exiting and its next sandbox appearing,
  when the old one briefly looks like the newest (1.49).
- A copy that starts after `ps5-theme-stop` carries on with the theme
  instead of applying it again from scratch, though old sandboxes still hold
  some of its overlays (1.52).
- Reading the mount table is safe from two threads at once: the call the
  plugin used before keeps one buffer for the whole program (1.53).
- PlayStation Network's numbered copies (`logo_00` to `logo_30`,
  `icon0_11`) count as one picture, and only installed games are listed,
  though the console keeps artwork for others too (1.48).
- WebP pictures are written in the console's own layout (a plain `VP8 `
  file without the colour profile browsers add) (1.48).
- The manager names each game picture by what it is (icon, background,
  logo), and an apply says what to do to see changes the home screen picks
  up later: rest mode for game and app icons, another app's music for the
  home music (1.53).
- `ps5-klog.elf` opens its socket again after rest mode, as the manager does.
- The parsers were fuzzed again for this release, every target clean.

**Other key findings about the console (13.42).**

- The home screen (ShellUI) reads a title's pictures by the paths in
  `/system_data/priv/mms/app.db`, through its own sandbox's view of `/user`:
  PS4 games' in `/user/appmeta/<title>`, most PS5 games' and homebrew's in
  `/user/app/<title>/sce_sys`, and some games' from PlayStation Network in
  `/user/catalog_downloader/conceptmeta/<concept>`.
- It reads the home row's icons once, as it starts; the Game Library's when
  that opens; and a game's background (`pic0`) every time its tile is
  selected. The plugin starts after ShellUI (the jailbreak runs after boot),
  so the home row needs one rest mode after a restart.
- At rest mode the system ends ShellUI, and its launcher (SceLncService)
  takes the sandbox down. An overlay inside its view of `/user` makes that
  fail and leaves the launcher stuck. Overlays in `/system_ex/vsh_asset` are
  tolerated: the system's own log shows workarounds for them.
- Forcing an overlay off while it's in use breaks the system's views on top
  of it ("Bad file descriptor"), and ShellUI then restarts about once a
  minute.
- The system keeps old sandboxes' folders, and some of their views, after
  rest mode. Their numbers (`NPXS40087_000`, `_001`, ...) only grow until a
  restart, so the newest one is the running app's.
- A game's `sce_sys` folder is part of what the console starts the game
  from: it must never be overlaid on the real folder.
- PlayStation Network's pictures are WebP and JPEG files under `.png` names,
  in numbered copies, kept per title and per concept (a game and its
  editions). The app database names the concept folder in each title's
  record; `appinfo.db` keeps the same concept in hexadecimal.
- The console's process records are larger than the SDK's, `kern.proc.all`
  doesn't exist, and every payload's process is named after the loader
  (`payload.elf`).
- Rest mode can break a payload's listening socket for good: `accept()`
  fails with error `0xA3`, as etaHEN's own servers see. The plugin's
  manager, the kernel log payload and the ELF loader all went quiet that way.

**What helped.**

- **Read-only tools in the plugin:** a lookup of every file and database
  string that names a title, a watch of which picture files any process
  opens and reads, the process list and the mount table. They found where
  the home screen reads icons from without changing anything.
- **`ps5-klog.elf`**, a small payload that serves the console's kernel log
  on TCP port 3232. It showed the launcher's failed teardown and ShellUI
  loading each background.
- **Testing on the console one change at a time**, with a restart after each
  failure, and taking a theme apart section by section to find the part
  that broke launching.
- **The plugin's own log, read over the console's FTP server**, when the
  manager couldn't be reached.
- **etaHEN's source**, for how its servers come back after rest mode.
- **A fixed set of console tests** (see [Tests on the console](testing.md#tests-on-the-console)),
  run the same way after every build.
- **The host test rig:** every failure on the console became a test, with a
  stand-in ShellUI process and sandboxes, applies held part-way, a silenced
  web server and thread names switched off as on the console. Each new test
  was also run with the fix taken out, to check that it fails then.

## PS5 game icons that stayed the same

With the guard fixed, the next test changed a PS4 game's icon but none of
the PS5 ones, although the log showed every view of their folders covered.
Listing the folders turned up an `icon0.dds` next to every `icon0.png`, so
the page learnt to write those textures too (BC7 for PS5 games, DXT1 for
PS4 ones). But a PS5 homebrew app with no texture at all hadn't changed
either, so that couldn't be the whole answer. A read-only lookup,
`/api/title/where`, then listed every file named after a title in the
folders ShellUI can see, and the lines of the system's databases that
mention it. The app database, `appinfo.db`, gives each title a
`_metadata_path`: `/user/appmeta/CUSA05070` for the PS4 game that changed,
but `/user/app/<title ID>/sce_sys` for Black Myth: Wukong, Ghost of Yōtei and
the homebrew app, a folder the plugin had never touched. **Solution:** the
pictures directly in a title's `sce_sys` folder can be themed too, and
**Change icon** writes every copy of the icon in both folders.

That build changed the icons, and the changed games then wouldn't start.
It had covered each game's real `sce_sys` folder, which the system starts
the game from. **Solution:** a title's pictures are now themed only in the
system apps' sandboxes (`/mnt/sandbox/NPXS…`), where the home screen reads
them, never on the real folders, `/original_user` or a game's own sandbox.
A copy that finds an overlay an older version left on a real folder takes
it down and applies again. The host test
gives its PS5 game an installation like Black Myth's: pictures beside
`param.json`, a license, a key file, a library in a subfolder, trophies,
and the game's package beside `sce_sys`. It checks that only the pictures
are listed and accepted, that ShellUI's sandbox shows the theme's pictures
while the real folders, `/original_user` and the game's own sandbox show the
console's, that every other file reads back unchanged and can't be written,
that an overlay left on the real folder by 1.38 is taken down, and that the
installation is byte-identical afterwards.

## Apps that wouldn't start

After 1.38, no game or app would start while Number Test was applied
(error CE-105733-9), and choosing stock didn't help until the console was
restarted. Number Test changes the interface icons, a tile background, the
system tiles' icons and game icons. With the interface icons switched off
it still happened, and the log showed where the rest had gone: the system
apps' real `appdb` folders, which hold each app's record (`param.json`)
beside its icon, the system's writable copy of them in `/mnt/rnps`, and the
sandboxes of system services other than the home screen (NPXS40000,
NPXS40094, NPXS40102). 1.40 then themed tile icons and game pictures only
inside ShellUI's sandbox: the icons stopped showing, and apps still
wouldn't start. Taking Number Test apart one section at a time on the
console found the cause: with only its background, apps started; with the
game pictures added, even only in ShellUI's sandbox, none did. The log
showed why it could matter: when the system shut down an old sandbox, it
couldn't remove a view of `/user` with one of our overlays inside it, and
new apps' sandboxes stopped being built just before `/user/home` was
mounted. **Solution:** in 1.41 games' and apps' pictures are turned off.
They're refused, a theme that has some is applied without them, nothing is
mounted in or under `/user`, and an overlay an older copy left on a game's
folder is taken down. Tile icons work as they did in 1.39. A host test
checks all of this, and fails if game folders are applied again. (1.46
brought them back safely, by taking them out of ShellUI's view the moment it
exits: see [Changing game and app icons safely](#changing-game-and-app-icons-safely).)

Along the way, 1.39 also stopped forcing off an overlay an app is still
using: every theme switch had done that to ShellUI's interface files, and
the log showed the system's own views on top of them breaking afterwards
("Bad file descriptor"), with ShellUI restarting about once a minute.
**Solution:** an overlay in use stays, with the new theme or the system's
own files on top (see [Overlays still in use](how-it-works.md#overlays-still-in-use)), and
the crash guard also catches a slower loop: 5 restarts within 10 minutes. A
host test holds a theme's picture open while switching to another theme and
then to stock: the reader keeps its file, the new theme and then the stock
picture show, the stage folder in use stays, and once the file is closed the
next switch takes every layer off. With the old forced removal put back,
that test fails. Another test restarts an app every few seconds, never 3
within the short window, and checks that the fifth restart takes the theme
down.

## A manager that stopped answering

After a few rest-mode cycles with 1.49, the manager page wouldn't open, and
starting the plugin again only said it "could not take over from the running
copy". The plugin's own log, read over the console's FTP server, showed the
running copy still at work (it took the game pictures out at the next rest
mode) but no longer answering on port 8095, even on the console itself. The
kernel log payload and the ELF loader had gone quiet at the same moment. In
etaHEN's source, its servers treat a failing `accept()` after rest mode
(error `0xA3`) as a sign to close the socket and listen again, and the
plugin's server had simply retried the broken socket every second.
**Solution:** in 1.50 the web server closes a listening socket that stops
working and opens a new one, and also tries it every 5 seconds when nothing
arrives, in case it fails silently. A new copy that gets no answer for 10
seconds signals the running one by process to step aside, so starting the
plugin again works, and `ps5-theme-stop.elf` stops it with the theme left on.

On the console, 1.50 then found nothing: both the new copy and the stop tool
looked for the plugin by its thread name, and the console's process list
didn't show it. Worse, the stop tool took "not found" for "not running" and
took the running copy's game-picture overlays out from under it, while it put
them back, and three of them ended up missing from its records. Those would
have stayed in the home screen's sandbox at the next rest mode, so the
console had to be restarted. **Solution (1.51):** each copy records its
process ID and start time when it takes the lock, and that is how it is
found; the lock alone says whether a copy runs, and the stop tool changes
nothing unless it holds the lock; and overlays are taken out by what the
mount table shows, so a missing record can't leave one behind. Host tests
break the listening socket and check that the manager comes back, silence the
web server for good and check that a new copy takes over with the theme kept
(with thread names unavailable, as on the console), run the stop tool against
a running copy, one it can't find (nothing may change) and one that was
ended, and take an overlay's record away before ShellUI exits.

## A WAV file that never finished checking

Fuzzing the parsers turned up a WAV file whose check never ended: a chunk
size near 4 GB made the step to the next chunk wrap around to zero in 32-bit
arithmetic, so the same chunk was read forever. Anyone on the network could
upload one and tie up the manager. **Solution:** the step is computed in 64
bits, a host test uploads that file, and the fuzz targets now flag any input
that takes more than 10 seconds. The measured before and after are under
[Security](security.md).

## "File name too long"

Some overlays failed with `ENAMETOOLONG`. The PS5 kernel stores a mount
point's name in an 88-byte field, and deep asset folders inside sandbox
views are longer. **Solution:** mount the nearest parent that fits, staged
with everything else in it unchanged.

## The tile on firmware without a function

The tile's registration function wasn't exported on every firmware.
**Solution:** fall back to the system's registration scan, as etaHEN's
Toolbox card does, and look every system function up at runtime so a missing
one can't stop the plugin. Loading system modules again from the plugin could
hang the dynamic linker, so the plugin only uses the libraries etaHEN's own
services already link.

## Fitting every size and format

The same icon exists in several sizes, and every system file has an exact
format. **Solution:** the browser fits one image to each size and format, and
the plugin's policy refuses anything that doesn't match the original
exactly.

## 4K textures without a texture tool

The home screen's backgrounds are 4K BC7 textures compressed with zstd, and
no browser can write either. **Solution:** a BC7 encoder in the page that
checks its own output, a zstd frame writer, and a full zstd decoder in the
plugin to verify every texture before it's used.

## The Appearance themes' artwork

With the PlayStation 2 Appearance theme selected, the Control Center has a
cloud backdrop and the clock sits in a glass capsule. Neither changed with
any theme, and the question was where they come from.

**What was observed along the way:**

1. **They looked like icons, and weren't.** The first reading of a photo of
   the two areas took them for interface icons: the search and settings
   icons, and the Control Center's row. Those are icons, but what was meant
   was the artwork behind them. How something looks on screen says little
   about where it is stored.
2. **What didn't change said the most.** The numbered test turns every
   picture in the interface's resource files into a number. On the photo of
   it, every icon, pill and badge had a number, and the capsule and the
   backdrop had none. So they are not PNG or SVG pictures in those files.
3. **Every texture already known was accounted for.** An inventory of every
   DDS texture in the folders the plugin listed found the 16 home
   backgrounds in `/system_ex/vsh_asset` (all full-screen 3840 by 2160), 14
   `.gnf` files in Sony's GPU format (particles, indicators, VR marks), the
   apps' controller and VR artwork, and the avatars, emoji and mats inside
   the interface files. None was the capsule or the panel, so they had to be
   somewhere the plugin wasn't looking.
4. **Watching what loads was blind to them.** `ps5-assetwatch` (in
   `tools/`) samples the files the home screen and the web renderer have
   mapped, twice a second for two minutes. While the Control Center, menus
   and tabs were opened it took 227 samples and saw no file appear or go
   away: the home screen had only 14 files mapped the whole time, all fonts.
   It reads its pictures and textures by loading them and closing the file,
   so a file-level tracker cannot see textures at all.

**The thought that solved it.** Every step so far had asked what the console
does: what it draws, what it loads. The turn came from a small remark: the
look was the **PlayStation 2 theme from Appearance and Sound**, one of the
console's 30th anniversary themes. If that theme can be chosen in Settings,
its artwork must already be on the console, as files, and probably named
after it. So the question became what must exist, and the search went by
name instead of by behaviour:

- The console's file lists had `30thAnniv_Settings_Preview_PS2_CC.png` and
  its siblings for each theme: the previews Settings shows of each theme's
  Control Center. Previews of the Control Center per theme meant the real
  artwork per theme existed somewhere.
- A read-only look at the console's top-level folders showed that `/system`
  has a `vsh_asset` folder of its own, beside the `/system_ex/vsh_asset` that
  holds the backgrounds and the `/preinst/vsh_asset` with the boot movie.
  Every earlier search had been aimed at the folders the plugin knew.
- In it, among the welcome pictures, were twelve textures:
  `control_center_<theme>_bottom.dds`, `control_center_<theme>_top.dds` and
  `homeui_<theme>.dds` for `ps`, `ps2`, `ps3` and `ps4`. The home screen's
  code names these paths.

Decoding them confirmed it. They start with zstd's magic although named
`.dds`; inside is BC7, 2560 by 820 for the PlayStation 2 bottom panel, 980
by 620 for its corner piece and 2560 by 800 for the artwork, with the other
themes' sizes a little different; most of each panel is clear, and the clear
pixels still carry colour. And they are plain files, so a theme can replace
them as it replaces backgrounds. **Solution:** `/system/vsh_asset` became a
themable folder, overlaid like the others, and its pieces are listed live
under **Appearance themes** with plain names.

The lesson: when watching the system can't find something, ask what has to
exist for what you see to be possible, and look for that by name. Here, the
anniversary themes being selectable at all meant their artwork had to be on
the console; that one thought replaced a search by behaviour that could not
work.

Then getting them right:

- **Byte for byte.** The files are BC7 and zstd-compressed although their
  names end in `.dds`, in three different zstd frame forms on 13.42.
  **Solution:** the page copies the system file's DDS header and frame
  header, and its XXH64 checksum when it has one, and the plugin refuses a
  texture that differs from the system file in anything but its picture.
- **Applied partly, twice.** The first strict build refused every texture
  made before it, because their frame header was of the old form. The
  next build fixed that, but still said "applied partly": the plugin
  remembers each file's check result by version, and every build said 1.56,
  so the first build's refusal was handed back. **Solution:** a texture
  right in all but its frame header is written again with the system file's
  header; remembered results carry a revision of the rules too.
- **See-through edges.** About half of a panel's pixels are clear, and
  Sony's files keep colour in them (straight alpha), so edges blend without
  dark fringes. The first encoder gave fully opaque pixels an alpha of 254.
  **Solution:** BC7 mode 6 with an alpha channel, alpha weighted far above
  colour when each block's parity bits and indices are chosen, and clear
  pixels filled with the nearest visible colour before encoding, as Sony's
  are.
- **Clouds that lost their middle.** A 16:9 cloud strip, centred on the 2560
  by 820 panel, lost its lower middle clouds and met the screen's edge flat.
  **Solution:** each piece is placed against the screen's edge it meets
  (bottom, top right, top), with **Fill**, **Fill with padding**, **Fit**
  and **Stretch**.
- **The clock that disappeared.** The same cloud on the corner piece hid the
  clock: it is white, and drawn over that piece. **Solution:** **Keep the
  clock and icons clear** leaves the top 26% of the piece empty (Sony's glass
  bar spans 7% to 22% of it, its glow 35%).
- **Options that needed the picture again.** A changed placement took
  effect only when the picture was attached again. **Solution:** the theme
  keeps the original picture (once for all sizes, switchable to save
  space) and the page makes the texture again from it at once; a piece
  attached before that is read back from its own texture.
- **Nothing changed after applying.** The interface reads these files as
  it starts. **Solution:** the manager and a console notification say to
  use rest mode after every change to a piece.

[Appearance themes](appearance-themes.md) is the guide, and [How the
Appearance textures are made](appearance-themes.md#how-the-appearance-textures-are-made) the
details.

## Music no one could hear

The first music feature played audio from the plugin itself, and the console
kept that silent. **Solution:** replace the home screen's own music file,
`bgm_home.at9`, so the console plays the theme's music with its own player.

## No ATRAC9 encoder

Browsers have no ATRAC9 support. **Solution:** an encoder written for this
project, using the codebooks and bitstream layout from LibAtrac9 and checked
in tests against two independent decoders (LibAtrac9 and FFmpeg).

## The music that ticked

The first encoded music played as ticks on the console, although every
decoder on a computer played it perfectly.

- First, the encoder was limited to the coding features Sony's own files
  use. Still ticks.
- Then the loop was laid out like Sony's. Lower ticks, still wrong.
- Four diagnostic themes isolated the cause: Sony's own music copied
  unchanged (played), Sony's onboarding music in its place (played), the
  encoder's music with Sony's layout (silent, and the console's audio
  stuttered), and the same music with one change, the unused bytes at the end
  of each superframe set to `0x01` as in Sony's files instead of zeros
  (played).

**Solution:** the encoder pads with `0x01`, and the plugin refuses any file
padded differently from the system file. The console's decoder evidently
reads bytes that other decoders ignore.

## Slow encoding in some browsers

Encoding was about 30 times slower in some browsers, because they throttle
timers in pages that aren't in front. **Solution:** run the encoder in a Web
Worker and yield with `MessageChannel` instead of timers.

## Reading the system's own file while a theme is applied

With a theme applied, the system path shows the theme's file, so the editor
compared new music and textures with the theme instead of the system.
**Solution:** keep a checked copy of the system's files and read the stock
version from there.

## Music that starts part-way through

After applying, the home music sometimes began near the end of the song. The
console's player keeps its place: after a game it carries on, and when the
music file changes it continues from the same point in the new file. Changing
that would mean changing how the console's own software runs. **Solution:**
the editor and README say so in plain words, and the editor describes what
plays from the start of the music.

## A slow start after every reboot

Even with the stock copy, putting a theme back after a reboot took about 37
seconds: every apply decompressed and checked each 4K background again,
staged about 300 MB, and re-read it through each of 15 overlays. The
console's log showed why staging was slow: hard links don't work on its
`/data`, so the stage folders held copies. **Solution:** checks against the
system files are remembered for the exact pair of files; a stage folder
built from the same files is reused as it is, after a reboot too; and each
overlay is confirmed by file identity against the folder it mounts.
Applying the same theme again now takes about a second, and a theme change
reports how far the copy has got.

## "It forgot my theme after a reboot"

It hadn't. Putting the theme back took about 30 seconds (each apply copied
the 169 MB asset folder), and the manager showed "Stock" until it finished.
**Solution:** the stock copy and hard-linked staging, preparing the new
theme before swapping, an "Applying…" state with real progress, and a console
notification when the plugin puts the theme back.

## An import that lost the backgrounds

An import brought back only the music. The importer only looked in the app
folders, not the home screen's asset folder, and a manager page left open
across an update kept running the old code. **Solution:** import takes
everything under the theme's `files/` folder, the page reloads itself when
the plugin is updated, and import and export say exactly what they contain.

## Small things that confused people

Testing on the console turned up:

- an Apply button that only updated after leaving the page;
- a text cursor blinking in plain text;
- a separate Editor tab that duplicated the Edit buttons;
- a button that should have been hidden;
- no way to back out of changes.

Each was fixed: the edit page's buttons follow the theme, the cursor appears
only in text fields, editing lives under Library, and Save and Discard were
added.

## Testing without the console

Only the console can run the plugin, and only its owner can test on it.
**Solution:** everything except `src/platform_ps5.c` also builds for Linux.
The real code runs against a fake console tree, with read-only bind mounts
standing in for nullfs, under AddressSanitizer and UBSan. The same fake
console serves the manager for trying the page in a desktop browser.
