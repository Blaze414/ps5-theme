# How it works, and why

How themes reach the console without writing the system, and why it is built this way. Part of the [PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Never writing the system](#never-writing-the-system)
- [Reaching the home screen through its sandbox views](#reaching-the-home-screen-through-its-sandbox-views)
- [The 88-character mount point limit](#the-88-character-mount-point-limit)
- [Only media, checked against the system file](#only-media-checked-against-the-system-file)
- [What the checks cover, and what they don't](#what-the-checks-cover-and-what-they-dont)
- [Verified end to end](#verified-end-to-end)
- [Stock copies, linked staging and the swap](#stock-copies-linked-staging-and-the-swap)
- [Progress reporting](#progress-reporting)
- [Coming back after every start](#coming-back-after-every-start)
- [Updating in place](#updating-in-place)
- [Stopping without a restart](#stopping-without-a-restart)
- [Save and Discard](#save-and-discard)
- [Backgrounds: BC7 textures made in the browser](#backgrounds-bc7-textures-made-in-the-browser)
- [Home music: an ATRAC9 encoder in the browser](#home-music-an-atrac9-encoder-in-the-browser)
- [Interface pictures in resource files](#interface-pictures-in-resource-files)
- [Overlays still in use](#overlays-still-in-use)
- [When the interface keeps restarting](#when-the-interface-keeps-restarting)
- [Preview pictures](#preview-pictures)
- [Export and import](#export-and-import)
- [The web server](#the-web-server)
- [The tile](#the-tile)
- [Crashes, logs and threads](#crashes-logs-and-threads)
- [Firmware independence](#firmware-independence)

```
 phone / computer / PS5 browser
            │  HTTP on port 8095 (web manager + JSON API)
            ▼
 ┌──────────────────────── ps5-theme (etaHEN plugin) ──────────────────────────┐
 │  web server (listener + 4 workers)  ──►  /data/ps5-theme/themes/<id>/       │
 │        │ selection                         theme files, music, pictures     │
 │        ▼                                                                    │
 │  theme engine ──► stock copy (once per firmware) ──► linked stage folders   │
 │        │                                                                    │
 │        └──► read-only nullfs overlays on /system_ex/... and every sandbox   │
 │             view of it that ShellUI reads through, each checked afterwards  │
 └─────────────────────────────────────────────────────────────────────────────┘
```

The browser does the heavy media work: image fitting, BC7 texture encoding
and ATRAC9 audio encoding. The plugin stores files, checks every one against
the system file it replaces, and applies themes. Each part below explains
what it does and why it is built that way.

## Never writing the system

etaHEN remounts `/system_ex` read-write, so "don't write there" has to be
enforced, not assumed. Every write the plugin makes goes through
`src/fsguard.c`:

- A write is allowed only when the path is inside `/data/ps5-theme/` or the
  tile's `/user/app/THEM00001/`.
- The deepest existing parent, with symlinks resolved, must be inside the
  same place, so a planted symlink can't redirect a write.
- The final path component is never followed (`O_NOFOLLOW`, `rename` and
  `unlink` semantics).
- Theme files, settings and uploads are written under a temporary name and
  renamed into place, so a crash never leaves half a file.
- **One exception** (since 1.62): replacing an older installed copy of
  ps5-theme itself with the running version (see
  [Updating in place](#updating-in-place)). It is a separate call,
  `fs_replace_payload`, that only replaces an existing regular file named
  `.plugin` or `.elf`, directly in etaHEN's plugin folder or at most two
  folders deep in `/data/pldmgr/payloads/`, whose folder (symlinks resolved)
  is really inside those. The new copy is written beside it, read back and
  checked (its version, and the `.plugin` header), then renamed over the old
  one, so a cut-off write never leaves a broken copy. The plugin's own code
  makes sure the file it replaces is an older ps5-theme build. Nothing in the
  system partitions is ever written.

**What it doesn't cover:** the check and the write are two steps on a path.
Another program on the console that swapped a folder inside
`/data/ps5-theme/` for a symlink between them could redirect a write. Only
a program already running on the console with full rights could do that, and
such a program can write the system directly anyway. Nothing the manager
accepts over the network creates a symlink. The guard is
there to stop the plugin's own mistakes and anything it's sent from reaching
the system, not to contain other software on the console. (The SDK has the
directory-relative calls, `openat` and the like, that would close the gap if
that ever mattered.)

Themes are applied as **read-only nullfs overlays**: a staged folder
(the stock folder plus your files) is mounted over the system folder. Mounts
live in memory only.

**Why:** writing themes into the system partition is the usual way consoles
get bricked, and it would break "reboot restores stock". Overlays give the
same everyday result with nothing to undo. The plugin only unmounts mounts it
can prove it created.

## Reaching the home screen through its sandbox views

ShellUI doesn't read `/system_ex` directly. It reads through its own sandbox
views (nullfs mounts such as `/mnt/sandbox/NPXS40000_000/system_ex`), and a
mount on the real path isn't visible through them. The engine reads the full
mount table, finds every view that shows a themable folder (14 on 13.42 for
the home screen's assets), and places an overlay on each. The overlays go
on the real path as well, for anything that reads it directly.

The system can also make new views later. After rest mode, for example,
some of ShellUI's apps restart in new sandboxes whose views show the stock
files. While a theme is applied, the plugin notices when the mount table
changes (it compares the number of mounts every second, which costs almost
nothing) and overlays any new view from the same stage folders, checks it,
and logs it. It never takes anything down to do this, so there's no stock
flash, and nothing is forced off while an app is using it.

Some views are views of views. The console shows `/user` again as
`/original_user`, and ShellUI's sandboxes then show
`/original_user/appmeta`. The plugin follows such a chain back to the real
folder (`/user/appmeta`), so a game's icon is overlaid in every place the
home screen might read it. It does this only for `/user`, the one place it
happens.

## The 88-character mount point limit

The PS5 kernel keeps a mount point's name in a fixed 88-byte field, as
FreeBSD 11 does. Deep React Native asset folders inside sandbox views are
longer than that, and mounting them fails with `ENAMETOOLONG`. The engine
then mounts the nearest parent folder that fits, staging that whole parent
with only your files changed.

## Only media, checked against the system file

`src/policy.c` decides what a theme may replace: only media, and only inside
ShellUI's React Native apps (`/system_ex/rnps/apps`, `/mnt/rnps/apps`), its
VSH assets (`/system_ex/vsh_asset`, and `/system/vsh_asset` for the Appearance
themes' textures), the text fonts (`/preinst/common/font`), and installed
titles' metadata (`/user/appmeta`, pictures only). Each file is checked against the
system file it replaces when it is uploaded, and again when the theme is
applied:

| Kind | Rule |
|---|---|
| PNG / JPEG / WebP | Same format and exact pixel size, so every display resolution stays correct |
| SVG | Must contain `<svg`; scripts, external links, `foreignObject` and entities are refused |
| WAV | Same codec, sample rate, channels and bit depth |
| AT9 (ATRAC9) | Exactly the system file's format; every block of audio unpacks cleanly; only coding features the system file itself uses; no more than twice its length |
| Fonts | A whole TrueType or OpenType (CFF) font: tables in bounds and none twice, the tables every font needs, outlines, glyph counts and metrics that agree, and a character map covering printable ASCII. Collections and WOFF are refused |
| JSON | Valid (depth-limited parser); same top-level type; animations keep their `w`/`h` canvas |
| MP4 / WebM | Same container |
| DDS / `.dds.zst` textures | Fully decoded (built-in zstd decoder, checksum verified); the DDS header must be byte-identical to the system texture and the data exactly the right length. A compressed texture must be one zstd frame that is the whole file, starting with exactly the system file's frame header (flags, window and size field), and every BC7 block must use a valid mode. A texture right in every other way but its frame header (as the pages before 1.56 wrote them) is written again with the system file's header and checksum, its picture data untouched. The console's own textures pass all of this |
| Interface resource files (`.rco`) | The console's own file with only pictures changed: each PNG matching the console's picture in every chunk but its image data, and that data complete and sound (fully decompressed and checked); each SVG keeping the console's opening and passing the SVG check; everything else byte-identical |
| Games' and apps' pictures (`/user/appmeta`) | Pictures only, by the rules above; a title's `param.json`, `param.sfo` and anything else in its folder are protected |
| Everything else | Protected: JavaScript bundles, executables, libraries and unknown types can't be replaced |

**Why:** a file in the same format as the original, with the same
dimensions, is the kind of file the home screen already handles. Code is
never themable. The home screen's
colours and layout live inside ShellUI's encrypted JavaScript; changing them
would mean patching ShellUI at runtime, which is deliberately out of scope.

## What the checks cover, and what they don't

The checks are about format and structure, not proof that a file is
harmless to every decoder that reads it:

- **Fully read:** ATRAC9 (every block unpacked), textures (fully decoded,
  header compared byte for byte), JSON (parsed), WAV (chunks walked).
- **Fonts:** the file's structure (see the table above), not its glyph
  outlines: a font with sound tables but broken outlines would pass.
- **Header only:** PNG, JPEG and WebP (format and pixel size; the image data
  itself isn't decoded) and video (the container's first bytes).
- **SVG** is checked against a list of refused constructs. That isn't a full
  SVG sanitizer.

So a file can pass and still be damaged further in; the home screen's own
decoders then decide what happens. Theme files are your own, and the likely
result of a bad one is a picture or sound that doesn't show or play, or the
home screen restarting. **Restore stock** or a restart brings it back.

The plugin's own parsers read whatever is uploaded, so they get the most
care: every read is bounds-checked, each kind of file has a size limit, a
texture is never unpacked beyond the system texture's size, and the parsers
are fuzzed with libFuzzer under AddressSanitizer and UBSan
(`tests/fuzz/run.sh`). The results, and the bugs found and fixed, are under
[Security](security.md).

## Verified end to end

- In the browser, every converted image and sound is decoded again and
  compared with what you gave it before it is uploaded.
- Uploads carry a CRC-32. The plugin checks it, stores the file under a
  temporary name, re-reads it and only then renames it into place.
- When staging, each theme file in the stage must be the very file the
  theme holds (the same inode, size and modification time), or else match it
  by checksum. Each live overlay must show the very files of the stage folder
  it mounts, or else match them by checksum. An overlay that fails is
  removed.
- Each file's check against the system file it replaces is remembered for
  that exact pair of files, the plugin version and the revision of the rules
  the checks apply, so an unchanged theme isn't decompressed and checked again
  at every start. A new or edited file is a different file, so it is always
  checked, and a result made by other rules is never trusted: a build that
  changed what is accepted once handed back the refusal an earlier build of the
  same version had remembered, which is how the revision came to be.
- Exports are read back before they are offered for download, and imports
  check every entry of the `.zip`.

## Stock copies, linked staging and the swap

An overlay must contain the whole folder, not just the files you replaced.
The home screen's asset folder is 169 MB on 13.42.

- **Once per firmware,** the first apply that touches a folder keeps a copy
  of it under `/data/ps5-theme/stock/<firmware>/`, checked byte for byte
  against the system. An installed title's folder changes when the title is
  updated, so its copy is compared with the folder again at each apply
  (while none of our overlays covers it) and made again if they differ.
- **Every apply** builds its stage folder by hard-linking that copy's files,
  and your theme's files too, which takes no time and no space. Theme and
  stock files are only ever replaced, never edited in place, so neither can
  change under a live overlay. Where the filesystem can't hard-link (the
  console's `/data`, as it turned out), the stage folder holds copies
  instead, and the log says so.
- **An unchanged stage folder is reused.** Each stage folder is recorded with
  fingerprints of what it was built from (the stock copy and each theme file:
  inode, size, modification time) and of what it holds. When both still
  match, the next apply reuses it as it is, after a reboot too, so nothing is
  copied or read again.
- **The previous theme's folder is kept too.** When you switch themes or
  restore stock, the folder of the theme that was showing is unmounted but
  kept, and recorded in `/data/ps5-theme/spare`. Switching back reuses it
  just like a re-apply. Only one previous theme is kept, so this costs one
  more folder on the console's storage at most (about 300 MB for the home
  screen).
- **The new theme is prepared first,** while the previous one is still
  showing. Only then are the old overlays removed and the new ones mounted,
  one after another. The stock look shows for a moment instead of the whole
  time.
- The same stock copy is how the editor sees the system's own version of a
  file while a theme is applied (for example to match the console's music
  loudness), since the system path then shows the theme's file.

**Why:** early versions copied the whole folder on every apply. That took
about 30 seconds, and the home screen showed stock for all of it, which
looked as if the plugin had forgotten the theme after a reboot.

## Progress reporting

The engine reports its step and percentage in the status API, and the
manager shows both. Copying (the first-time copy of the system's files, and
a stage folder where hard links don't work) is measured in bytes copied,
since it takes most of the time; checking is measured by overlay. The
percentage never goes backwards.

## Coming back after every start

The selected theme is stored in `/data/ps5-theme/active`. After a reboot the
plugin applies it again at start, with a notification on the console.
**Restore stock** sets the selection to `none`.

When the plugin itself restarts without a reboot (an update, or etaHEN's
Stop, which ends a plugin at once), the overlays are still in place. After
every apply the plugin writes down the theme's target folders and stage
folders (`/data/ps5-theme/live`), and a new copy carries on with the
overlays when they are all still there for the selected theme. Nothing is
taken down, so nothing flashes and nothing an app is using is forced off. If
anything doesn't match, it applies the theme again as usual.

## Updating in place

Only one copy of the plugin runs at a time: each copy holds a lock file for
as long as it runs, and the kernel releases it when the process ends.

- Every 10 seconds the running copy looks in etaHEN's plugin folders,
  `/data/ps5-theme/update/` and PS5 Payload Manager's payload folders
  (`/data/pldmgr/payloads/` and the folders up to two levels down, such as
  `local/<name>/`, where newer versions of Payload Manager keep imported
  payloads; symlinked folders aren't followed) for a newer copy of itself: a
  `.plugin` file
  (checked by its header: `etaHEN_PLUGIN`, the title ID `THEM00001`, the
  version, then an ELF) or a plain `.elf` (checked by the marker
  `PS5THEME-VERSION:<version>:THEM00001` every build carries).
- It sends that program to the ELF loader on port 9021 (the standard PS5
  payload loader, which etaHEN also runs), which starts it as a new process.
- The new copy finds the lock taken, asks the running copy's version, and if
  it is newer asks it to step aside (`/api/handoff`, accepted only from the
  console itself). The old copy exits, leaving the overlays, and the new one
  takes the lock and carries on with them.
- A copy that has just started a newer one waits for it to ask before doing
  anything else. At boot, that means the newer copy puts the theme back, not
  the old one.
- An old copy in the middle of an apply finishes it first, and the new copy
  waits up to 3 minutes for that. The request carries the new copy's process
  ID, and the old copy steps aside only if that process is still there.
- A copy that isn't newer leaves the running one alone and exits. The new
  copy names itself `THEM00001`, as etaHEN's loader names plugins.
- If the running copy doesn't answer for 10 seconds (its web server can be
  down after rest mode, see [A manager that stopped answering](challenges.md#a-manager-that-stopped-answering)),
  the new copy finds its process and asks it to step aside with a signal
  (`SIGUSR1`). Each copy writes its process ID and start time to
  `/data/ps5-theme/pid` once it holds the lock, so a later process that
  happens to get the same ID is never mistaken for it; the thread name is a
  fallback. Copies before 1.50 end on that signal, which also leaves their
  overlays in place.
- Once it runs, the new copy brings the installed copies up to date (since
  1.62): every ps5-theme build in etaHEN's plugin folders or Payload
  Manager's payload folders that is older than itself is replaced with its
  own program (with the `.plugin` header in etaHEN's folder, the plain
  program elsewhere). Without this, an update through the update folder would
  last only until the next start, when etaHEN or Payload Manager starts the
  old copy again. It is the one write outside the plugin's own folders (see
  [Never writing the system](#never-writing-the-system)). Payload Manager's
  `meta.json` and anything that isn't a ps5-theme build are left alone.
- Only once every installed copy is up to date does it empty
  `/data/ps5-theme/update/` of copies that aren't newer than itself, and only
  those nothing has written to for a minute. That folder is only a drop box;
  if an installed copy can't be replaced, the copy there stays, so the
  update isn't lost.

**Why:** after rest mode etaHEN's Toolbox is often unavailable, and that was
the only way to restart a plugin without restarting the console.

## Stopping without a restart

`ps5-theme-stop.elf` is a small separate program for when the manager can't
be reached. Whether the plugin runs is decided by its lock alone, and the
tool changes overlays only while it holds that lock itself, never under a
running copy. If a copy runs, the tool finds it (by `/data/ps5-theme/pid`,
or its thread name) and asks it to stop (`SIGUSR2`). The plugin then takes
out only the overlays of games' and apps' pictures and exits. Those must not
stay in the home screen's view with nothing running to take them out at rest
mode (see [Apps that wouldn't start](challenges.md#apps-that-wouldnt-start)). The rest of
the theme stays, and the next copy carries on with it and puts the pictures
back. A copy that doesn't stop within two minutes is ended, and so is a copy
from before 1.50, which ends on the signal at once; once the lock is free
the tool takes out the pictures' overlays it left. A running copy the tool
can't find is left alone, and what the tool saw is written to
`/data/ps5-theme/dump/stop.txt`. It says what it did in a notification.

Overlays are taken out (here, and when ShellUI exits at rest mode) by what
the console's mount table shows, not only by the plugin's records, so an
overlay missing from the records still comes out.

After rest mode the system keeps some apps' older sandboxes, with the
theme's overlay still in their view of `/system_ex/vsh_asset`. A copy that
starts carries on with the theme all the same: such leftovers stay recorded
and come down with the next theme change. Only a game's or app's picture
left in one makes it apply the theme again, which takes it out.

## Save and Discard

The first change to a theme makes a snapshot of it in
`/data/ps5-theme/snapshots/<id>/`, made of hard links, so it is instant even
for a 180 MB theme. **Save** deletes the snapshot. **Discard** swaps it back
in with two renames and re-applies the theme if it's the applied one. The
snapshot is safe for the same reason as the stock copy: theme files are only
ever replaced, never changed in place. Pictures the library makes by itself
don't count as changes; a picture you choose does. **Duplicate** uses the
same hard links, so copying a large theme is instant too.

## Backgrounds: BC7 textures made in the browser

On 13.42 the home screen's backgrounds are 3840×2160 BC7 DDS textures,
compressed with zstd. The builder:

- fits your image to each texture's exact size and every mip level;
- encodes BC7 in the browser (mode 6, with endpoints taken from each 4×4
  block's principal colour axis);
- decodes a sample of the blocks again and refuses a result below 24 dB
  PSNR;
- reuses the system texture's DDS header byte for byte;
- wraps the data in a standard zstd frame of stored blocks, which any zstd
  decoder can read.

The plugin then decompresses it with its own zstd decoder (`src/zstd.c`,
following RFC 8878, with the XXH64 checksum verified) and checks the header
and data length against the system texture.

**Why in the browser:** a phone or computer encodes a 4K texture in a few
seconds. The plugin stays small and only needs to verify.

## Home music: an ATRAC9 encoder in the browser

The home screen plays `/system_ex/vsh_asset/bgm_home.at9`. On 13.42 it is
ATRAC9, 48 kHz 5.1, 300 kbps, with a loop. Browsers have no ATRAC9 codec, so
the builder carries its own (`web/atrac9.js`, run in a Web Worker):

- It encodes in exactly the system file's format: the same channel layout,
  sample rate, bitrate and codec settings. Your music goes to the front left
  and right channels; the others stay silent.
- It writes only the coding features the system file itself uses. On 13.42
  every system ATRAC9 file uses bit allocation mode 3, at most 13 bands
  (13.5 kHz, the console's own music bandwidth), precisions up to 6, 7 or 8
  and the same LFE coding. The encoder and the plugin both derive these
  limits from the console's own file. For a file that never goes above 6
  (`sfx_transition.at9`), the encoder also has higher gradients that lower
  every part's precision further; without them, a loud, pure tone needed 7
  somewhere and couldn't be encoded (fixed in 1.62).
- It fills the unused end of every superframe with `0x01`, as Sony's files
  do. See [the ticking music](challenges.md#the-music-that-ticked).
- It matches the loudness of the system music (the RMS of its stereo
  downmix), then applies your -12 to +12 dB offset.
- It lays out the loop as the console's own music does. The loop starts
  some seconds in and audio continues after the loop end, so the song
  repeats without a seam.
- It uses only parts of the format that two reference decoders, LibAtrac9
  and FFmpeg, read identically.
- It decodes every file again and compares it with your music, overall and
  passage by passage, before uploading.

The plugin (`src/atrac9.c`) checks the format, unpacks every block, and
compares the coding features with the system file's. A file that fails is
skipped when applying, and the editor offers **Encode again**.

The theme also keeps the whole song as 48 kHz 16-bit WAV (`music.wav`, up
to about 11 minutes), with the chosen parts and settings in `theme.ini`. The
home music is encoded again from it whenever you change the part, order,
joints or loudness. Parts are laid out in play order; the file's loop points
mark where the repeating starts, and each joint is crossfaded into the audio
just before where playback continues.

**Why the browser:** the encoder needs a full psychoacoustic pipeline (MDCT,
bit allocation, Huffman coding) and a lot of CPU. It runs in a Web Worker so
the page stays responsive, and it yields with `MessageChannel` instead of
timers, which browsers throttle.

## Interface pictures in resource files

ShellUI keeps its interface pictures in resource files (`RCOF` version
0x110, the layout the PS Vita uses): a 0x50-byte header with the offset and
size of nine tables, an element tree, string and ID tables, and at the end a
file table holding the embedded files. Each `texture` element points into the
file table (`src`, and `src_4k` for a 4K version) and is a PNG or an SVG,
whatever its declared type says: on 13.42 three "SVG" entries (the app tile
placeholders) hold PNG data. The
same files hold the interface sounds (VAG), every on-screen string (JSON per
language, zstd-compressed) and the colour palettes the 30th-anniversary
themes use.

- **Built in the browser from the console's own file.** The manager reads the
  stock file, swaps pictures and rebuilds it. Files keep their order on
  16-byte boundaries, and files the console shares between entries stay
  shared. An unchanged rebuild is byte-identical to the console's file; this
  was checked with 13.42's 37 MB `Sce.PlayStation.PUI_UI3.rco`.
- **Pictures that match Sony's bit for bit, apart from the pixels.** A
  replacement PNG copies every chunk of the console's picture other than the
  image data byte for byte and in the same order: the header (size, bit
  depth, colour type), palette, transparency, colour space and text chunks.
  Its image data keeps the console's zlib header and IDAT chunk size. A
  palette picture uses the console's own palette, each pixel mapped to the
  nearest entry. On 13.42 that covers 8- and 4-bit palette, 1-bit
  greyscale, greyscale with transparency, and full-colour pictures. A
  replacement SVG keeps the console's own opening (XML declaration and
  `<svg ...>` tag with its size and view box) byte for byte.
- **Checked strictly by the plugin** (`src/rco.c`, `src/png.c`). Everything
  before the file table must be byte-identical to the console's file apart
  from the pointers into it, and each pointer must stay inside the table on
  a 16-byte boundary. Each PNG must match the console's picture as above and
  is fully decompressed by the plugin's own inflate (`src/inflate.c`): the
  data must be exactly the size the header implies, its checksums (chunk CRCs
  and Adler-32) right, every row's filter valid, and every pixel of a palette
  picture inside the palette. Each SVG must keep the console's opening and
  pass the SVG check. A colour palette or the shape list must keep the
  console's names in order, each with a value in range; a sound is checked
  as described [below](#interface-sounds-in-sonys-layout). Every other
  embedded file (the strings, the controller's vibration patterns) must be
  unchanged.
- **Your own pictures, in the same format.** Your image is fitted (cover,
  contain or stretch, as chosen above the sections) to the picture's size and
  written with the same encoder: in a palette picture each pixel takes the
  nearest colour of the console's palette. For a vector icon, the SVG keeps
  the console's opening and draws your image as a PNG `<image>` filling its
  view box, at twice its size (at most 512 pixels).
- **Numbers drawn on whole pixels.** The test's numbers are seven-segment
  digits made of plain rectangles: in a PNG on whole pixels (two colours, so
  they fit any palette), in an SVG as `<rect>` elements, so they depend on no
  font and on the simplest SVG there is.

**Why:** an earlier theme pack replaces these files whole, with a separate
copy for each firmware, written into the system. A file from another firmware
can break the home screen, and writing it into the system makes that
permanent. Rebuilding the console's own file and checking it against the
original avoids both, and the overlay goes with a restart.

## Interface sounds, in Sony's layout

The interface's sounds (since 1.62) are the `sound/vag` entries of
`Sce.PlayStation.PUI_UI3.rco`: 107 on 13.42, among them `snd_focus_move`
(moving between items), `snd_enter`, `snd_cancel`, `snd_change_space`
(switching between Games and Media with L1 and R1), the PlayStation 1 to 4
Appearance themes' own (`snd_1_focus_move` to `snd_4_cancel`, 12 in all)
and 23 vibration patterns for the controller's haptic motors
(`..._vibration`), which the manager doesn't offer.

- **The format: Sony's HE-VAG.** A 48-byte big-endian header (`VAGp`,
  version `0x00020001`, the data size, the sample rate, the channel count at
  `0x1E`, a name at `0x20`), then 16-byte frames, one per channel in turn:
  a shift and a predictor, a flag, and 28 four-bit samples. Each sample is
  its code scaled by the shift plus a prediction from the four samples before
  it, by one of 128 predictors; Sony's sounds use them all (58% of their
  frames use more than the classic five). Stereo, 48 kHz (one at 44.1 kHz).
  Common tools read these files as mono PSX ADPCM, which they are not.
- **Sony's layout, byte for byte.** All 107 are laid out alike: the data size
  equals the data, the reserved header bytes are zero, each starts with one
  silent frame per channel, every frame is flagged 0 except each channel's
  last sound frame (flagged 1), and all end with the same closing frames
  (`00 07 77 …`, flag 7). A new sound takes the header (all but the data
  size), the opening frames and the closing frames from the console's own
  sound, and lays its own frames out the same way. Its frames use the
  classic VAG predictors 0 to 4, which HE-VAG decodes as the original VAG
  does, each frame with the predictor and shift that come closest.
- **Starting at the hit.** The console plays an interface sound the moment
  you press, so any wait is in the sound itself. Sony's get loud (within 15
  dB of their loudest 5 ms) 5 to 35 ms in for moving, selecting and going
  back. A new sound starts 10 ms before that point, faded in, and anything
  quieter before it is cut: on the console, a theme's sounds made from game
  clips had 150 to 300 ms of quiet sound before their hit and were heard
  late. Run on Sony's own 84 sounds, the same rule leaves the moving sounds
  untouched and cuts at most 20 to 55 ms of a slow swell from a few others.
- **Checked in the browser.** Decoded again, a new sound must give exactly
  the samples the encoder meant, sample for sample, and its layout must pass
  the same checks the plugin makes.
- **Checked by the plugin** (`vag_check` in `src/rco.c`): the header byte for
  byte apart from the data size, which must match the data; the opening and
  closing frames byte for byte; every frame in between flagged 0 (never a
  loop mark) except each channel's last sound frame; shifts of at most 12 and
  predictors among the 128; no longer than the console's sound. A vibration
  pattern can't be changed.
- **Tested on 13.42's own file** (locally; Sony's file stays out of the
  repository): the manager's decoder against vgmstream's on five sounds, a
  rebuild of the 39 MB file with one sound swapped that changes only that
  entry and its pointers, accepted by the plugin, and seven damaged versions
  refused (a loop mark, another name, a non-silent opening, another ending, a
  shift of 13, one frame group too long, a changed vibration pattern).

**Why:** the interface reads these sounds as it starts, like its pictures,
and Sony's player is the only one that ever reads them. Keeping every byte
that doesn't carry the sound exactly as Sony's, and refusing loop marks,
means a new sound is played the way the console's own is, once, and can't
keep playing.

## Overlays still in use

Switching themes or choosing **Restore stock** takes the current overlays
down. If an app is still reading from one (ShellUI keeps the home music and
its interface files open), the plugin waits a moment and then leaves it
where it is. It never forces it off. On 13.42, forcing it broke the
system's own views stacked on it ("Bad file descriptor"), and apps wouldn't
start until a restart. Instead:

- the new theme's overlay goes straight on top of the one in use (since
  1.63; before, a copy of the system's folder went in between, so each
  switch while ShellUI held its interface folder added two layers there);
- the same theme again, unchanged: its prepared folder is reused, and the
  overlay in use already shows it, so it carries on and nothing goes on top
  (the console's nullfs refuses to stack a folder on a view of itself,
  "Resource deadlock avoided"; 1.63's first build hit this on the console,
  and the plugin put the system's files on top before covering them again);
- if the new theme's overlay can't go on there, or fails its check, a
  verified copy of the system's own folder goes on top instead, so the
  console never shows the theme that was taken down;
- for **Restore stock**, a verified copy of the system's own folder goes on
  top, so the console looks stock;
- the app keeps reading what it already had open;
- the next switch tries again from the top down, and a restart clears
  everything.

A stage folder that an overlay still uses is never deleted.

## When the interface keeps restarting

A picture, sound or interface file the console can't handle can make one of
its apps crash as it starts. The console then starts it again, in a new
sandbox each time (`NPXS40087_002`, `_003` and so on), and after about ten
tries it gives up. The plugin's view watcher sees each of those sandboxes
anyway. If one app restarts 3 times within 60 seconds, or 5 times within 10
minutes, while a theme is applied, the plugin takes the theme down at once,
selects stock so the theme isn't put back at the next start, and says so on
the console and in the manager. One restart, as after rest mode, is normal
and changes nothing.
Sandboxes left by earlier restarts can stay on the console for good, so the
plugin counts only sandboxes newer than the ones it found when the theme was
applied.

## Preview pictures

A theme's card shows `picture.jpg` (chosen by you) if there is one, otherwise
`preview.jpg` (made from the background). Both are 1280×720 JPEGs checked by
the plugin. For themes made before preview pictures existed, the library
reads the theme's own background once (an image, a video frame, or a
texture decoded in the browser) and makes the picture.

## Export and import

Export builds a `.zip` in the browser: `theme.ini`, every file under
`files/`, the song and the preview pictures. It reads the result back before
offering it for download. Import accepts a `.zip` or a dropped folder, takes
everything under the theme's `files/` folder and uploads it file by file,
each checked by the console. Anything the console refuses (another firmware's
files, for example) is skipped and listed. The message says what came in,
for example "15 backgrounds and the home music, all verified".

## Mix and match

A new theme made of parts of others (since 1.65).

- **The page decides what makes each part.** It sorts each theme's files into
  the editor's sections by the same rules the editor lists them with, the
  more particular first (a tile's icon is a tile, not an icon), and into
  elements: background areas, Appearance pieces, tiles, games, fonts, or a
  file with its size variants. Files this console doesn't have are left out.
- **The plugin copies them** (`POST /api/themes/mix`): the page sends one line
  per file, kept original, song, music settings or preview picture, each
  naming the theme it comes from. Every path is checked as an upload's is (a
  replaceable system file of this console), must lie inside that theme's
  folder (no link out of it) and must be a file it has; anything else
  refuses the whole mix. The theme is built under a hidden name and renamed
  into place only when everything is in, so a mix that fails leaves nothing.
  The files are copied as they are, as Duplicate copies them, and checked
  against the console's own again when the theme is applied.
- **The interface file is made in the browser.** Interface icons, interface
  sounds, colours and corner shapes share one file, so each theme's changes to
  it are read against the console's own, and each changed entry is taken from
  the theme its part or element names: a picture with all its size variants,
  anything else entry by entry. The rebuilt file is uploaded like any other
  and checked by the plugin (see
  [Interface pictures in resource files](#interface-pictures-in-resource-files)).
- **The editor's choices** come along: the sections the parts use, the
  background's fit and areas, the Appearance pieces' placement, the songs'
  names.

## The web server

A small HTTP/1.1 server in C (`src/web.c`): one listener thread and four
workers fed by a bounded queue, which answers `503 busy` when full, so a slow
or broken client can't hold up others. It serves the manager (one HTML page
and the ATRAC9 module, both built into the plugin) and a JSON API. The page
reloads itself when the plugin is updated, so it never runs code older than
the plugin.

## The tile

A deep-link app: only `param.json` and `icon0.png` under
`/user/app/THEM00001`, with a `deeplinkUri` that opens the local manager in
the console's browser. It is registered through AppInstUtil. On firmware
without the single-title install function, the plugin uses the system's
registration scan instead, as etaHEN's Toolbox card does. An ownership marker
and a receipt keep it from being registered twice.

## Crashes, logs and threads

- Fatal signals write a report to `/data/ps5-theme/logs/crash-*.txt`: the
  registers, a backtrace as offsets into `ps5-theme.elf`, and what each
  thread was doing. Each thread has its own signal stack, so a stack
  overflow can still be reported. The process then exits quietly, without
  the system's crash dialog, and the next start announces the crash.
- The log rotates at 512 KB.
- There are three kinds of threads: the theme engine, the web server, and
  the tile installer. A theme lock keeps uploads from changing a theme while
  it is being staged and checked, and each large buffer belongs to exactly
  one thread.

## Firmware independence

Nothing is tied to one firmware. Kernel access comes from ps5-payload-sdk,
the firmware version is read at runtime, and the asset list is built on each
console. System functions are looked up at runtime from the libraries etaHEN's
own services link, never loaded again with `dlopen` and never imported
directly, so a function missing on some firmware turns one feature off
instead of stopping the plugin.
