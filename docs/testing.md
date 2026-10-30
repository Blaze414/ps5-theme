# Testing

The automated tests and the tests on the console. Part of the [PS5 Themes](../README.md#documentation) documentation.

```bash
tests/host/run.sh
```

Builds the real plugin sources for Linux with AddressSanitizer and UBSan and
runs 72 end-to-end tests against a fake console tree in a privileged
container (GitHub Actions runs them on every push: see
[Building](building.md#continuous-integration)). They cover:

- upload checks and the media policy, including files crafted to hang or
  exhaust the checks;
- apply, re-apply, restore and stock copies with linked staging, reusing
  stage folders where hard links don't work, across a simulated reboot, and
  switching back to the previous theme;
- a write that fails at the disk still closing its file;
- interface resource files: pictures swapped and accepted, anything else
  refused (pixel formats, extra chunks, other compression, damaged or short
  image data, invalid row filters, colours outside the palette, an SVG where
  the console has PNG data, strings, colours, structure, pointers),
  and the manager page's own code, run in Node, numbering every picture in
  the console's exact formats, compressed and in stored blocks;
- interface sounds: a sound laid out as the console's accepted, and refused
  with another header, name, sample rate or channel count, a data size that
  doesn't match, longer than the console's, another opening or ending, a loop
  mark, a sound frame flagged as the last too early or not at all, a shift
  of 13, a predictor beyond the 128, or a changed vibration pattern; the
  page's encoder, run in Node, trimming the silence before a sound and a
  quiet stretch before its hit (getting loud within 10 ms), cutting
  one that is too long to the console's length, refusing to write a longer
  one, keeping the console's header, opening and closing frames byte for
  byte, decoding again to exactly the samples it meant, and changing only
  the sounds it was given;
- the interface's shape list: new numbers accepted, and refused with a name
  changed, added or missing, a number out of range, or anything that isn't
  a plain number; the page's editor writes the numbers where the console's
  stand, so unchanged numbers give the console's own text;
- installed games' and apps' pictures: listed and accepted only in the
  console's exact formats (a game's `param.json`, license, keys, the rest of
  its installation and the plugin's own tile refused, PlayStation Network's
  WebP and JPEG copies only in their own format), applied only in the home
  screen's own view of `/user` once its process is found, never the real
  folders, `/original_user` or other apps' views; an overlay an older copy
  left on a real folder is taken down; the result and a notification say to
  use rest mode;
- the Appearance themes' textures: listed from `/system/vsh_asset`, accepted
  only as the console's exact format (zstd under a `.dds` name; frame header,
  checksum, whole-file frame, BC7 modes), applied on the folder and in the
  system's views of it, and the rest of the folder untouched; the page's BC7
  encoder with transparency, its zstd frames equal to a reference writer's
  and readable by the real `zstd`, and its colour bleed into clear pixels;
- fonts: listed from the fonts folder, 22 kinds of unusable font refused with
  a plain reason (web fonts, collections, cut-short files, a missing or
  doubled table, a broken header, too few glyphs, no Latin letter, no Unicode
  map) and TrueType, OpenType and format 12 maps accepted; applied only in
  ShellUI's view of the folder (the real folder and other apps' views keep
  the console's fonts) and out of its sandbox when it exits;
- the home screen's process exiting: the overlays out of its sandbox at
  once, also with an overlay missing from the records, and also while the
  plugin is held in the middle of an apply, before or after mounting
  (nothing goes into that sandbox afterwards); its older sandboxes and ones
  the system left behind never covered; a sandbox the system builds for a
  home screen that never starts gets nothing and is taken down cleanly; its
  next sandbox covered only once its process runs, then within half a
  second; with its exit not reported as an event, the plugin doesn't spin
  and still takes everything out within a second; with a mount of the
  system's over one of ours, only what is exactly ours comes off;
- the plugin crashing with a theme applied: its overlays out of the home
  screen's sandbox before it exits, and the report saying so;
- your own picture inside a vector icon, a home screen tile icon numbered
  by the page in its exact format, and tile icon files mapped to their
  apps, all accepted;
- a system app that keeps restarting with a theme applied, quickly or about
  once a minute: the theme taken down and not put back at the next start,
  while one restart, or sandboxes left by earlier restarts, change nothing;
- an overlay still in use when switching themes or restoring stock: never
  forced off, the same theme again carrying on with it (no layer added,
  recorded under its folder's new name), the new theme straight on top of
  it (one layer per switch),
  the system's own files on top for Restore stock and where the new theme's
  overlay fails its check, its stage folder kept, and every layer off once it
  is free;
- read-only overlays, sandbox views and symlink containment;
- concurrent requests during an apply, slow and malformed clients (one
  sending a byte of its headers every few seconds cut off after 10 seconds);
- the page and its ATRAC9 module sent compressed and inflating to exactly
  the built-in bytes, sent whole to a browser that turns gzip off, and "not
  modified" while the plugin is the same; a theme's picture "not modified"
  while unchanged and the new one as soon as it is replaced;
- a theme with folders nested 100 deep (put there by hand): its status,
  apply, delete (nothing deleted, "delete it over FTP") and, once within
  reach, duplicate and delete, with the plugin running throughout (until
  1.77 the status crashed on it, over and over);
- recovery after a hard kill, log rotation and the crash handler;
- new sandbox views made after a theme is applied (as after rest mode);
- updating in place, from a `.plugin` in etaHEN's folder and from a plain
  `.elf` in PS5 Payload Manager's folders or the plugin's own update folder:
  the program sent to a stand-in for the ELF loader, the hand-over, keeping
  the overlays, a copy that isn't newer stepping aside, the hand-over at boot
  (the old copy steps aside before putting the theme back), and no hand-over
  to a copy that has gone; older copies installed in etaHEN's folder and in
  both of Payload Manager's layouts (`payloads/<name>/` and
  `payloads/local/<name>/`) replaced by the running version, with
  Payload Manager's `meta.json` unchanged; a copy the write guard refuses
  left as it was, and the update folder's copy kept for it; then the update
  folder emptied of the copy, and nothing else in it touched;
- Save and Discard, preview pictures and duplicates; Discard applying the
  theme again only when the console shows other files than the ones it puts
  back; a Discard cut off between its two steps put right at the next start;
- Writes from many threads: four threads renaming a theme, changing its
  icon style and its choices and reading the status, and two choosing
  themes, all at once: every request answered, every file whole with one of
  the values written, and no temporary file left;
- Every combination: six clients at once, each drawing 40 steps from
  everything the manager does (applying a theme or none, uploading a
  picture, a broken one, removing it, renaming, the icon style, the
  choices, Save, Discard, duplicating and deleting, creating and deleting,
  reading), in an order fixed by a seed so a failure is the same failure
  again, five seeds in a row. After each: never a server error or a dropped
  connection, the plugin running, the console showing exactly the applied
  theme's file (or its own), every overlay recorded and every record
  mounted, and no stage folder or temporary file left behind;
- The status's walk of each theme kept: a change made through the plugin
  shows at once, one made from outside within 10 seconds (and not at once);
- Icons styled in the background: each template part answered with the
  size and checksum stored, and the template whole only with all three
  parts; with its style on and a stand-in game busy all along, a title
  installed 15 minutes ago is styled by the plugin (one whose package is
  still downloading is not)
  (its PNGs the icon in the shape over the template, its textures BC7), the
  theme applied again and the console told once, by the title's name (from
  its `param.json` in its default language with escapes, a PS4 title's
  `param.sfo`, or its ID when the name can't be read), with no steps or tips
  from the apply; the titles still installing said once, and one it can't
  read left for the page and said; a title whose files are dated an hour
  ahead, and one whose download is still being written, kept waiting; one
  seen before that comes back with other files, dated long ago, waiting the
  whole time from then and said as installing again, and one that comes back
  as it was put back at once; the status' `titles` changing as titles
  come and go, the titles still installing listed with the seconds left, the
  kept template read back; the page's own styling applied again the same way,
  and an apply you ask for said as usual; a title still being written and
  the titles known when the style was used are left alone; two titles the
  theme has pictures for, uninstalled and installed again: the theme applied
  again by itself, the style's made again from the picture kept for it and
  yours put back as it was, and the style's list cleared with the style; bad template
  parts and settings refused; once uninstalled, its pictures stay in the
  theme and the apply isn't partial;
- A styling run said on the console as it starts, goes (no more often than
  every 8 seconds) and ends, by name for a game installed since, and bad
  requests refused;
- A theme of 71 folders applied; one of more than 256 refused, saying so and
  what to do; the status giving the limit;
- The page telling an icon the style made from one changed by hand: the
  same backdrop outside the shape (but for rounding) is the style's; a
  picture of yours, another backdrop, or a shape that leaves too little of
  it is not;
- The console's own pictures: an interface icon by id (the SVG and the PNG
  one, byte for byte from the file, the SVG with a policy that runs
  nothing), a missing one and bad ids refused; a texture's quarter-size
  picture, pinned (its decoder matched Pillow's), and non-textures refused;
  the icon style's backdrop picture kept, read, refused under an unknown kind
  and removed;
- The storage reserve: with the free space set just above 1 GB, an upload is
  refused (507, "storage is nearly full") with no temporary file left, the
  status reports the room left, and a mix that needs a copy is refused as
  such with no theme left; with room again, the same upload and mix go
  through;
- Mix and match: a new theme made of three themes' parts, each file,
  original, song and the music settings copied byte for byte, the themes
  taken from unchanged, then applied; a list naming another theme's file, a
  file a theme doesn't have, a path out of the system's folders or through a
  link out of the theme, a theme that isn't there, a song numbered 9, an
  unknown part, an existing id, an empty or too long list or a request
  without the manager's header refused, leaving nothing behind; the page's
  merge of the interface file taking each kind, and a single element, from
  its own theme, and the plugin accepting the result;
- home music made by the browser's encoder, and damaged or wrong-format
  ATRAC9 files; a pure tone encoded for a sound never coded above precision
  6, as 13.42's `sfx_transition.at9` is;
- home music playlists: songs kept in numbered slots (`music.wav`,
  `music2.wav` …) with their order and parts, wrong values refused, carried
  by a duplicate, and an older `theme.ini` read as one song; the page's code,
  run in Node, joining the songs by crossfade, fade or cut and looping back to
  the start or repeating the last song;
- the editor's choices kept with the theme (`choices.json`): read back as
  sent, anything but one JSON object in UTF-8 text refused, undone by
  Discard, copied by a duplicate;
- apply progress;
- a byte-for-byte check that `/system_ex` is unchanged afterwards.

```bash
tests/host/run.sh emulated
```

The same suite in an unprivileged build step, with mounts emulated and the
read-only checks skipped.

```bash
tests/host/run.sh tsan
```

The same suite against a ThreadSanitizer build of the plugin, which stops at
the first data race between its threads (the engine, the web listener and
its four workers, the tile installer and the icon styler). It builds for the
machine's own architecture (ThreadSanitizer can't run under the x86-64
emulation Docker uses on an Apple Silicon Mac; the plugin's code is
portable). On 29 October 2026 every test passed under it, natively on ARM64,
with no race reported, the test of writes from many threads included.

```bash
tests/host/run.sh serve
```

Runs the plugin with a fake console at `http://localhost:18095`, for trying
the manager in a browser.

```bash
tests/atrac9/run.sh
```

Checks the ATRAC9 codec. It encodes test signals (music-like, noise, sweep,
clipping, clicks, very quiet, silence), decodes them with `web/atrac9.js`,
LibAtrac9 and FFmpeg, and requires all three to agree within one 16-bit step.
The plugin's validator must accept every file. Pass a console file to check
that too; it is mounted read-only and never copied into the image:

```bash
tests/atrac9/run.sh path/to/bgm_home.at9
```

`tests/zstd/run.sh` checks the plugin's zstd decoder against the reference
`zstd` tool.

```bash
tests/fuzz/run.sh
```

Fuzzes the plugin's parsers (zstd, DDS textures, ATRAC9, WAV, image headers,
JSON, SVG, PNG pictures with the plugin's inflate, interface resource files)
with libFuzzer, AddressSanitizer and UBSan, 60 seconds each by default
(`FUZZ_SECONDS` changes it). Inputs that crash or hang are saved in
`tests/fuzz/out/`.

## How the bugs were found

1.77 to 1.80 were a pass over the whole plugin for stability, speed and
safety, each finding checked before it was fixed and each fix tested before
it went to the console. What was used, and what each turned up:

- **The console's own log, read only.** The log, the mount table, the list
  of processes and ShadowMountPlus's log (which records the console's power
  states), read over FTP and the plugin's own diagnostics, never changing
  anything on the console. They showed a line repeated every minute, the
  apply's slow steps once 1.77 timed each step, and why 1.79 didn't wake.
- **A static analyser** (Clang's) over every source file: four warnings,
  three of them false; the fourth made safer anyway.
- **The full suite under AddressSanitizer and UBSan**, on real mounts and with
  mounts emulated, and **under ThreadSanitizer**, which stops at the first
  data race between threads: none reported, in any version.
- **New tests for what was found**: a client sending its headers a byte at a
  time; a theme nested 100 folders deep (it crashed the status until 1.77);
  the page and pictures answered "not modified"; a game update noticed
  without reading every game's folder; a list of overlays with a last line
  cut short; a Discard cut off between its two steps; nothing going into an
  app's sandbox while the home screen restarts, and an app's new view waiting
  2 seconds.
- **The test of every combination**: six clients at once, each taking 40
  steps drawn from everything the manager does, five seeds, then the
  console checked against what must always hold. It found the Discard races
  of 1.77 and 1.78 (see
  [Challenges](challenges.md#discard-that-could-empty-a-theme)); after the
  fix it passed 10 times in a row, on real and emulated mounts, 12,000
  requests in all.
- **Fuzzing**: all 15 parsers (pictures, textures, sounds, fonts, interface
  files, JSON, zstd), 46 seconds each, about 35 million inputs: no crash, hang
  or memory error.
- **The page in a browser** against the test rig: through an apply, the cards,
  pictures and progress bar stay the same elements, the bar slides, and no
  picture is fetched again; a reload costs a few hundred bytes; opening one
  theme then another quickly, leaving during a styling run, closing Mix and
  match while it loads, and opening and closing the Preview quickly all
  behave.
- **The console**, after each: 1.77 and 1.78 applied and rested; 1.79 applied
  in 2 seconds but didn't wake from rest; 1.80 woke three times in a row.

## Tests on the console

The host tests can't show what the console's own software does, so every
version was also tried on a PS5 before release. The tests below are from
1.33 to 1.53, while game and app icons, interface icons and the tools for
stopping and updating were being made.

**The setup.** A PS5 with firmware 13.42, jailbroken, running etaHEN 2.5B
(unified build) and PS5 Payload Manager. The games used were chosen because
each keeps its icon in a different place:

| Game | Kind | Where the home screen reads its icon |
|---|---|---|
| Rocket League (CUSA05070) | PS4 game | `/user/appmeta/CUSA05070` |
| Black Myth: Wukong (PPSA23226) | PS5 game | its installation's `sce_sys` folder |
| Ghost of Yōtei (PPSA26344) | PS5 game | its installation's `sce_sys` folder |
| PS5SX2 (PPSA99203) | PS5 homebrew, `icon0.png` only | its installation's `sce_sys` folder |
| Death Stranding Director's Cut (PPSA01968) | PS5 game with PlayStation Network artwork | `/user/catalog_downloader/conceptmeta/234585` (WebP files named `.png`) |

The test theme, **Number Test**, numbers everything it can (system tiles,
interface icons, game icons) and changes one background, so each picture on
screen shows which file it came from. Other test themes added music.

**How each test ran.** The new `.plugin` was copied into etaHEN's plugin
folder, and the running copy handed over to it. Each test then followed the
same steps:

1. Apply the theme.
2. Check the home row, the Game Library and a game's background.
3. Put the console in rest mode, wake it, and check the home row again.
4. Start a themed game, an unthemed game and Settings.

Evidence came from four places:

- the plugin's log, through `/api/log`, or over etaHEN's FTP server when
  the manager couldn't be reached;
- the console's mount table, compared with the plugin's records;
- the kernel log, from `ps5-klog.elf` on port 3232;
- read-only lookups in the plugin: where a title's files are, which files
  are read, and the process list.

After any failure the console was restarted before the next test.

| Version | What was tried on the console | What happened |
|---|---|---|
| 1.33 | System tile icons and one tile background | Changed after rest mode, without a crash |
| 1.34 | Game icons in `/user/appmeta`, in every view of it | The crash guard took the theme down: it counted old sandboxes as restarts |
| 1.35 | The same, with that fixed | Rocket League's icon changed; the PS5 games' didn't |
| 1.36 | Their `icon0.dds` textures written too (BC7, DXT1) | PS5 icons still unchanged; PS5SX2, which has no texture, unchanged too |
| 1.37 | Read-only lookup of each game's files and database records | The app database points most PS5 games at their installation's `sce_sys` folder |
| 1.38 | The pictures in `sce_sys` too, real folder and views | Icons changed, but the themed game wouldn't start, and with Number Test applied no app started (CE-105733-9) until a restart |
| 1.39 | Views only, never the real folders; overlays in use never forced off | Stacking over an overlay in use works; apps still failed |
| 1.40 | Game pictures and app records only in the home screen's view | Icons didn't show; apps still failed. Number Test taken apart: the background alone was fine, and with game pictures added nothing started |
| 1.41 | Game pictures off; tiles, interface icons and background on | Apps start |
| 1.42 to 1.44 | Game pictures in one place at a time: the icon service's views, the real folders, `/original_user`, the home screen's own view | The first three changed nothing. The home screen's view changed the background and the Game Library at once, but not the home row, which ShellUI reads as it starts. The kernel log, recorded through rest mode, showed why such overlays had stopped apps from starting: the launcher failed to take ShellUI's sandbox down (EBUSY) |
| 1.45 | Overlays taken out of the home screen's view when ShellUI exits | Nothing changed: the plugin never found ShellUI's process |
| 1.46 | The same, with the process found | **The home row's icon changed after rest mode, and every app started.** Overlays out in 75 ms; the kernel log showed a clean teardown |
| 1.47 | PlayStation Network's picture folders added | Rocket League's and PS5SX2's icons changed; Death Stranding's copies were refused (WebP and JPEG files) |
| 1.48 | Each file written in the format it really holds | Death Stranding's icon and background changed |
| 1.49 | More rest-mode cycles | Fine at first; after the second cycle the manager, the kernel log payload and the ELF loader stopped answering |
| 1.50 | Socket reopened; take-over and stop by process | Neither found the plugin (the console doesn't list thread names), and the stop tool removed a running copy's overlays; restarted |
| 1.51 | Process found by its recorded ID | Two rest-mode cycles fine; `ps5-theme-stop` stopped it with the theme kept; starting it again brought the manager back |
| 1.52 | Update over the running copy; theme switch both ways; Restore stock; restart with a theme | Carried on with the theme; every step fine; after the restart the Game Library showed the icons at once and the home row after the first rest mode |
| 1.53 | Rest mode in the middle of an apply; the new reminders | The overlays came out of ShellUI's sandbox in 5 ms while the apply was running, nothing went into it afterwards, apps start; the rest mode and home music reminders appeared |
| 1.54 | Corner shapes: multiplier at 3, switches at 0, then rest mode | Switches came out square-cornered; the multiplier changed nothing visible; the home screen's tiles didn't change |
| 1.55 | Fonts: `SST-Roman` replaced, then rest mode; a scan of the fonts each process has mapped | No change on the home screen. The overlay was in ShellUI's view and ShellUI mapped its fonts from it, but only the Light and Bold weights; Roman and Medium were mapped by the web renderer alone |
| 1.55 | Fonts: one font for all the Latin interface fonts, then rest mode | **The home screen's text changed** |
| 1.56 | Appearance themes: a 16:9 cloud strip on the PlayStation 2 Control Center's bottom panel | **The Control Center showed the picture.** Centred, it lost the lower middle clouds and met the screen's edge flat: now placed against the bottom edge, with padding: **the placement options work** |
| 1.56 | The same cloud on the top-right piece | The clock disappeared: white text on a white picture, drawn over it. Now the band its clock and icons sit in can be kept clear: **works** |
| 1.56 | An older theme with a page-made background, applied twice with 1.56 builds | "Applied partly" both times: first from the frame check refusing the old form, then from a remembered refusal handed back by the next build. Remembered results are now tagged with a revision of the checking rules. With that build, reported working |
| 1.56 | Home screen artwork (`homeui`): placement and padding | **Works** |
| 1.56 | Fit and placement changed later, for pictures attached before and after originals were kept; the fit setting for every picture; the switch to not keep originals | Reported working, on the last two builds |
| 1.57 | The rest-mode reminders after attaching, changing or resetting an Appearance piece and after applying; the updated Placement tooltip and note | **Works** |
| 1.59 | Colour test on every palette, then photos of the Control Center, the Welcome hub and Settings | Base mat = the backdrop behind the Control Center; panels = the Control Center's cards and the button hints; text = Settings' text (some labels stayed white). The Welcome widgets and the home screen's text didn't change: they follow the Appearance theme by another route |
| 1.59 | The same, with the Welcome hub's Use System Appearance for Widgets off | **The widget cards and the pills turned the panels' green**, the Media Gallery tint the base mat's magenta; card text stayed white. With the setting on, the Welcome hub picks the widgets' colours itself |
| 1.59 | Panel test (each panel and base mat colour its own), setting off | Widget cards and pills = `menu.fill.enabled`; the Media Gallery tint = `base-mat.overlay.enabled`; the button hints = `key-guide.overlay.fill.enabled`; with the Control Center open, its backdrop = `base-mat.overlay.enabled` and its card = `radio-button.overlay.fill.enabled` |
| 1.59 | Colour editor: red widgets picked under Default look, PlayStation 2 selected, setting off | **Red widgets and pills.** An edit of the PlayStation 2 palette alone didn't show: the widgets, the Control Center and the button hints use the default look's palette with every theme |
| 1.59 | The same theme, PlayStation 2 selected, Use System Appearance for Widgets turned **on** | Blue widgets and pills: the Welcome hub's own PlayStation 2 colour. The PlayStation 2 palette then held red widgets and a yellow accent, and neither showed: with the setting on, no palette is used for the widgets |
| 1.59 | The released build: the colour editor with Default look only, the review's fixes (uploads made at the same time), the help marks, progress with percentages and the loading markers | **Works** |
| 1.59 | A theme that said "applied partly … 1 skipped, 107 of 27 folders" (cut off mid-sentence) after a Discard: an upload's temporary file had come back with it | With the fix: "applied: 67 files verified on the console", every hint in full; the temporary file is left out |
| 1.60 | An apply with the library open; the update from 1.59 | **Works**: one progress display (the banner); 1.59 handed over by itself, and the next apply removed the cut-off upload's file |
| 1.60 | A sound of the user's on `sfx_warmboot.at9` (about 10 seconds), applied, then rest mode and waking | **Works**: the theme's sound played as the console woke |
| 1.61 | The plugin keeping a playlist's songs (`music2.wav` …) and their settings, with a one-song theme applied | **Works**: the one song as home music, as before |
| 1.61 | A home music playlist made in the editor (songs added, each its own part), applied, then a game's music and back to the home screen | **Works** |
| 1.61 | The editor's Preview of a theme (home screen and Control Center drawn from its files) | **Works** |
| 1.62 | An update through the update folder, with the plugin installed in PS5 Payload Manager (`payloads/local/ps5-theme/`) | **Works**: the new version took the place of Payload Manager's copy (byte for byte the build), and the update folder emptied itself |
| 1.62 | Interface sounds: moving between items, selecting and going back replaced with sounds from a game, applied, then rest mode | **The new sounds played**, but late. Read back from the console, they got loud 180 to 325 ms after a press, after a quiet stretch in the clips (Sony's: 5 to 35 ms) |
| 1.62 | The same sounds started at their hit (Start at the hit, and attached again) | **Works**: no delay |
| 1.62 | Which sound plays on L1 and R1 (the candidates decoded and played on a computer) | `snd_change_space`, now named Switching between Games and Media; it and the Control Center's sounds replaced: **work** |
| 1.63 | Theme switches while the home screen held the interface folder, the same theme applied again among them | One layer per switch, but the same theme again was refused on three folders ("Resource deadlock avoided"): the console's files went on top, then the theme again, three layers and "applied partly" |
| 1.63 | The fix, after a restart: Yakuza 0, Yakuza 0 again, Snoopy, Yakuza 0 | **Works**: each applied completely; exactly one theme layer on every folder afterwards (13 after a day of tests with 1.62) |
| 1.64 | The update from 1.63 through the update folder; the options menu's opening and closing sounds, the screenshot's and a dialog's replaced, applied, then rest mode | **Works** |
| 1.66 | Icon style (a shape over a picture) with games installed later on; choosing options with the controller; a homebrew app installed afterwards | **Three faults**: "the console stored something different" (the plugin answered the style's template upload without its checksum, so only the first part was kept and the setting never saved); a chosen option showed only after Save (the white focus hid its ring); the new app wasn't styled (no template, and a game left on the home screen counted as one being played). The running game's process is named `eboot.bin`, as assumed |
| 1.67 | The fixes above; Yakuza 0 (with its own font) applied, then rest mode | **The console didn't wake**: the plugin covered the new home screen's font while the system was still building it; two home screens in a row never started and none started again until the console was restarted (see [A home screen that never came back](challenges.md#a-home-screen-that-never-came-back)) |
| 1.68 | Nothing in a home screen's sandbox until its process runs; Yakuza 0, rest mode | **Works**: wakes, and the font and the game icons show after one wake |
| 1.69 | The update from 1.68, then two rest-mode cycles with Yakuza 0 | **Works**: each time the overlays came out of the old home screen in about 65 ms, and the new one was covered only once it was running |
| 1.69 | Icon style run with your own icons included, then Itemzflow (installed in the 1.66 test, uninstalled) installed again with Yakuza 0 left on the home screen | **Partly**: your four icons were kept and styled, and the template kept whole. Itemzflow got nothing: the theme already had its icon from the 1.66 test, so it wasn't new, and nothing applied the theme again for it; and the open game counted as played, as Yakuza 0 used about 41% of one core on the home screen, as much as while played. Fixed in 1.70 |
| 1.70 | Itemzflow uninstalled and installed again, with Yakuza 0 left open on the home screen | **Works**: ten minutes after the install the plugin applied the theme again by itself and said so, and Itemzflow's icon from the theme was overlaid and checked. Its icon was in the style, but the page had lost its record as the style's (Remove the style dropped games not installed at the time), fixed in 1.71 |
| 1.71 | The update from 1.70; the manager opened on Yakuza 0 with a new app (PPSA99000) installed; Itemzflow after rest mode | **Works**: the page styled the new app on opening the theme, saved and applied it; Itemzflow showed its styled icon, which matched the style's backdrop pixel for pixel. Two gaps: a page left open didn't show games installed meanwhile, and Itemzflow's lost record made it count as changed by hand, so styling again with your icons included would have put it in the shape twice. Both fixed in 1.72 |
| 1.72 | The update from 1.71; two homebrew apps installed with Yakuza 0 applied and the manager closed: Reload PS2 Disc, and PS5SX2 | **Reload PS2 Disc works**: "Reload PS2 Disc is installing" at 20:35, styled by itself at 20:45, one notification ("icon styled for Reload PS2 Disc"), nothing else; shown after rest mode. **PS5SX2 waited**: its installer dated its files an hour ahead of the console's clock, so the wait couldn't start until then, and its download was still being written, which the plugin didn't look at. Fixed in 1.73 (timed on the plugin's own clock; the download watched) |
| 1.72 | Updating to 1.73 through the update folder (copied in with ps5upload) | **Didn't update**: the running copy read the file while it was being copied in, at its full size but not yet filled, remembered it as no ps5-theme by its size and date, and never looked again (the copy was byte for byte right). The stop tool couldn't find the running copy: its recorded start time differed by exactly an hour from the console's, and 13.42 has no thread list. The update check fixed in 1.73 (whole copies only, nothing remembered from a file still changing); the stop tool asks over the web port first, and matches the start time give or take two minutes |
| 1.73 | The update from 1.72 (the new copy uploaded as `.part`, then renamed); PS5SX2 after it; the stop tool | **Works**: 1.72 took it at once; PS5SX2 styled 15 seconds after 1.73 started, one notification ("icon styled for PS5SX2"), its icon exactly the style's; two rest-mode cycles straight after, one during the apply, handled; the stop tool stopped the plugin over its web port and the next copy carried on |
| 1.74 | Icon style with the system tiles: 37 icons (22 games and apps, 15 tiles), then Apply | **The icons were made right** (every tile in the shape, over Yakuza 0's background), **but the apply was refused**: the theme now changed files in 66 folders, over the plugin's limit of 64, and the message ("could not read Yakuza 0, or it touches more than 64 folders") didn't say which. Fixed in 1.75: up to 256 folders, the page checks before styling, the message says what to do; and **Stop styling** turns red when pointed at |
| 1.75 | The update from 1.74; Yakuza 0 applied again (66 folders), then rest mode | **Works**: applied (217 overlays, 142 files checked; the staged copies rebuilt once, after the refused apply); at rest mode 103 overlays came out of the old home screen in 1.3 s and the new one got all of them, the 28 tile folders included |
| 1.76 to 1.78 | Updates, applies, Apply again; rest mode on 1.77 | **Works**: rest mode on 1.77 (108 overlays out in 0.47 s, the next home screen covered in 3 s); Apply again 36.3 s on 1.77 (31.9 of it reading every game's folder), 5.7 s on 1.78 |
| 1.79 | Apply again; then rest mode | Apply again **2.0 s**, but **the console didn't wake**: going into rest, two apps that start with the home screen had their views covered as the system made them, started again, and the next home screen never started; the console had to be switched off at its power button. Fixed in 1.80: nothing goes into any sandbox while the home screen restarts, and an app's new view waits 2 seconds |
| 1.80 | The update from 1.79 (after the console was switched off at its power button); three rests in a row | **Works**: each woke; the home screen's overlays out in under a second and the next one covered at once; the two apps that start with it covered 2 to 3 seconds after they appeared |
