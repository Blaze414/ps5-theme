# Testing

The automated tests and the tests on the console. Part of the [PS5 Themes](../README.md#documentation) documentation.

```bash
tests/host/run.sh
```

Builds the real plugin sources for Linux with AddressSanitizer and UBSan and
runs 51 end-to-end tests against a fake console tree in a privileged
container. They cover:

- upload checks and the media policy, including files crafted to hang or
  exhaust the checks;
- apply, re-apply, restore and stock copies with linked staging, reusing
  stage folders where hard links don't work, across a simulated reboot, and
  switching back to the previous theme;
- a write that fails at the disk still closing its file;
- interface resource files: pictures swapped and accepted, anything else
  refused (pixel formats, extra chunks, other compression, damaged or short
  image data, invalid row filters, colours outside the palette, an SVG where
  the console has PNG data, sounds, strings, colours, structure, pointers),
  and the manager page's own code, run in Node, numbering every picture in
  the console's exact formats, compressed and in stored blocks;
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
  the system left behind never covered; its next sandbox covered;
- your own picture inside a vector icon, a home screen tile icon numbered
  by the page in its exact format, and tile icon files mapped to their
  apps, all accepted;
- a system app that keeps restarting with a theme applied, quickly or about
  once a minute: the theme taken down and not put back at the next start,
  while one restart, or sandboxes left by earlier restarts, change nothing;
- an overlay still in use when switching themes or restoring stock: never
  forced off, the new theme or the system's own files shown on top, its
  stage folder kept, and every layer off once it is free;
- read-only overlays, sandbox views and symlink containment;
- concurrent requests during an apply, slow and malformed clients;
- recovery after a hard kill, log rotation and the crash handler;
- new sandbox views made after a theme is applied (as after rest mode);
- updating in place, from a `.plugin` in etaHEN's folder and from a plain
  `.elf` in PS5 Payload Manager's folders or the plugin's own update folder:
  the program sent to a stand-in for the ELF loader, the hand-over, keeping
  the overlays, a copy that isn't newer stepping aside, the hand-over at boot
  (the old copy steps aside before putting the theme back), and no hand-over
  to a copy that has gone;
- Save and Discard, preview pictures and duplicates;
- home music made by the browser's encoder, and damaged or wrong-format
  ATRAC9 files;
- apply progress;
- a byte-for-byte check that `/system_ex` is unchanged afterwards.

```bash
tests/host/run.sh emulated
```

The same suite in an unprivileged build step, with mounts emulated and the
read-only checks skipped.

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
