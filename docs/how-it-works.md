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
- [The console's own pictures in the manager](#the-consoles-own-pictures-in-the-manager)
- [Icons styled in the background](#icons-styled-in-the-background)
- [Preview pictures](#preview-pictures)
- [Export and import](#export-and-import)
- [The web server](#the-web-server)
- [The tile](#the-tile)
- [Threads, and what they share](#threads-and-what-they-share)
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
- **Room for the console** (since 1.66): the storage under `/data` is the
  console's own, shared with saves, app data and the system's records. Before
  receiving an upload (whose size the request states), and before each copy
  it makes (staging a theme's folders where hard links aren't available, Mix
  and match), the guard's `fs_room_for` checks that 1 GB stays free
  (`FS_RESERVE`), from `statfs` on `/data/ps5-theme`. When it wouldn't, the
  write is refused before anything is written: an upload gets
  `507 Insufficient Storage` ("the console's storage is nearly full"), a copy
  fails and is logged as "storage nearly full". `/api/status` reports `room`,
  what a theme can still take, so the page refuses a file that won't fit
  before sending it: a refusal while a large upload is still arriving can
  reach the browser as a dropped connection instead of the message.
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

The home screen's own sandbox (`/mnt/sandbox/NPXS40087_<n>`) gets nothing
until its process (`SceShellUI`) is running and the plugin is watching it
(since 1.68). The system builds the sandbox first, then starts the process,
and if the process ends, the plugin takes its overlays out of the sandbox
at once, before the system takes the sandbox down. Before 1.68 the font
went into a new home screen's sandbox while the system was still building
it. On 2026-10-29, waking from rest mode, two home screens in a row never
started; the plugin's font overlay was still on the system's font folder in
their sandboxes, so the system couldn't take them down, and no home screen
started again until the console was restarted. Now, for 30 seconds after
the home screen exits, the plugin looks for the next one every 10 ms and
covers its sandbox as soon as it is found, early enough for what it reads
as it starts (confirmed on 13.42: the font and the game icons show after
one wake).

Two more rules since 1.80, after 1.79 left the console unable to wake: two
apps that start with the home screen (`NPXS40101` and `NPXS40102`) had their
views covered as the system made them, during rest mode; they started again
and the next home screen never did.

- From the moment the home screen exits until the next one is running and
  watched, nothing goes into any app's sandbox. What the system makes
  meanwhile is covered once the new home screen runs, a second or two later.
- A view in any other app's sandbox, or in one of the home screen's own
  inner sandboxes (`sce_sbx`), is covered only once it has been there 2
  seconds, when the system has finished making it. The home screen's own
  views go in at once, as before. A view where an overlay of the plugin's is already present, even
under another mount, never gets a second one.

The same care when overlays come out (since 1.69):

- **Only what is exactly ours.** An unmount by path takes off the newest
  mount at that point. Where the system has made a mount over one of ours,
  that would be the system's, so such an overlay of ours stays (the log
  says how many) and the system's mount is left as it is. Our own layers on
  one point come off newest first.
- **When the exit can't be watched.** Should the console refuse to report
  the process's exit as an event, the watcher looks every 50 ms instead.
  Before, it asked again at once, for good, which would have kept a
  processor core busy for as long as the home screen ran.
- **When the plugin crashes.** With no copy running, nothing would take its
  overlays out of the home screen's sandbox when the home screen exits at
  rest mode. The crash handler takes them out first, with system calls only,
  from the plugin's records, and only those the path shows as ours; the
  crash report says so.

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
home screen restarting. **No theme** or a restart brings it back.

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

A theme may change files in up to 256 folders, each overlaid on its own (64
before 1.75: a theme of 18 games with its icon style and the system tiles
came to 66, and was refused). The status gives the limit (`maxFolders`), the
manager checks a style against it before making it and says when a theme is
over it, and an apply over it says so plainly ("changes files in more than
256 folders") rather than that the theme couldn't be read. Each folder costs
about 2 KB of the apply's tables for every view of it.

An overlay must contain the whole folder, not just the files you replaced.
The home screen's asset folder is 169 MB on 13.42.

- **Once per firmware,** the first apply that touches a folder keeps a copy
  of it under `/data/ps5-theme/stock/<firmware>/`, checked byte for byte
  against the system. An installed title's folder changes when the title is
  updated, so at each apply its copy is checked against the folder (while
  none of our overlays covers it) and made again if they differ. The check
  compares each file's identity, size and time with what they were when the
  copy was last found the same, and reads the files only when one of them
  changed (since 1.78: 1.77 read every file of every game's folder both
  ways at every apply, 30 of an apply's 36 seconds on the console).
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
**No theme** sets the selection to `none`.

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
- Only a whole copy is started (since 1.73). One written in the last 10
  seconds must look the same (size and date) at two looks in a row; the
  program sent must carry the version found and end with its section table
  (a copy still coming in has zeros there). Some tools make the file its
  full size first and fill it in within the same second: 1.72 could read it
  then, remember it by size and date as no ps5-theme at all, and never look
  again (2026-10-29). What it reads of a file changed in the last 5 seconds
  is no longer remembered.
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
running copy. If a copy runs, the tool asks it to stop over its web port
first (`/api/stop`, accepted only from the console itself, since 1.73), which
needs no looking for its process; else it finds it (by
`/data/ps5-theme/pid`, or its thread name) and asks by signal (`SIGUSR2`).
The pid file's start time is matched give or take two minutes since 1.73:
the console's boot time moves when it sets its clock, and matched to the
microsecond, a copy that had run for 43 minutes wasn't recognised
(2026-10-29), with no thread names to fall back on on 13.42. The plugin then takes
out only the overlays of games' and apps' pictures and exits. Those must not
stay in the home screen's view with nothing running to take them out at rest
mode (see [Apps that wouldn't start](challenges.md#apps-that-wouldnt-start)). The rest of
the theme stays, and the next copy carries on with it and puts the pictures
back. A copy that doesn't stop within two minutes is ended, and so is a copy
from before 1.50, which ends on the signal at once; once the lock is free
the tool takes out the pictures' overlays it left. A running copy the tool
can't find is left alone, and what the tool saw is written to
`/data/ps5-theme/dump/stop.txt` (the pid file, that process's start time as
the console gives it now, whether the web port answers, and the threads it
could list). It says what it did in a notification.

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

Switching themes or choosing **No theme** takes the current overlays
down. If an app is still reading from one (ShellUI keeps the home music and
its interface files open), the plugin leaves it where it is. It never forces
it off. Where the next theme goes on at the same place it tries once (since
1.77; it used to wait 3 seconds for each, and the home screen keeps four
such folders open for as long as it runs, which added 12 seconds to every
apply); where nothing goes on after it, as for **No theme**, it waits those
3 seconds first. On 13.42, forcing it broke the
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
- for **No theme**, a verified copy of the system's own folder goes on
  top, so the console looks stock;
- the app keeps reading what it already had open;
- the next switch tries again from the top down, and a restart clears
  everything.

A stage folder that an overlay still uses is never deleted.

The apply's summary line in Activity says how long each step took (since
1.77): "… 2 file(s) compared by content; 18.4 s (preparing 4.1, switching
12.9, checking 1.4)". Each overlay is added to the plugin's record of them
as one more line as it goes on, not by writing the whole record again and
waiting for the disk each time (since 1.79: 217 of them took seconds of the
switch on the console, while the home screen showed its own look). The
record matters only while the console runs, as the overlays go with a
restart; a last line cut short as the power went is never read as a record,
and the record is written whole again before the next line goes on.

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
applied. The restarts are timed by the console's clock, so rest mode counts
between them; when that clock is set back meanwhile (the console sets it
from the network after it starts), a restart before the change never counts
as within the time (since 1.77).

A view the watcher leaves as it is (one of the plugin's overlays is already
there, under a mount the system made on top of it later, as after a game is
installed again) is looked at again every minute but written in Activity
once per apply (since 1.77; 1.76 wrote the same lines every minute, 5,800 a
day, and pushed what mattered out of the log). The theme shows there again
after rest mode, when the home screen's new sandbox gets its views.

## The console's own pictures in the manager

The manager draws the console's own pictures where a theme leaves things as
they are, and its list of a theme's parts with the console's own icons
(since 1.66). None of them are part of the plugin or the page: two read-only
requests read them from the console as the page runs, from the console's own
copy of each file (`stock_path`), never a theme's.

- **`/api/ui-icon?id=iconid_…`**: one icon of the main interface file
  (`Sce.PlayStation.PUI_UI3.rco`). Only the part of the file before its
  pictures is read to find them (about 200 KB of 39 MB on 13.42), once while
  the file stays the same (its device, inode, size and time), then each icon
  on its own. Only `iconid_` ids of letters, digits and underscores are
  accepted, and the reply carries `Content-Security-Policy: default-src
  'none'`, so an SVG opened on its own can't run anything.
- **`/api/asset/picture?path=…`**: a system texture as a quarter-size
  picture, one RGBA pixel per 4x4 block (the block's average, its colour
  weighted by how opaque each pixel is): a 4K background becomes 960x540, an
  Appearance piece 640x205. The console's textures are zstd-compressed BC7
  using all eight of its modes, which no browser reads; the plugin's decoder
  follows Microsoft's specification, with its partition and anchor tables,
  and matched Pillow's decoder exactly on the console's own textures (the
  four looks' pieces and the 4K background) and on the tests' fixture, whose
  result the tests pin. BC1 and 8-bit RGBA/BGRA textures are read too.

## Icons styled in the background

A theme's icon style (Games and apps) reaches games installed after it was
used without the manager open (since 1.66). `src/autostyle.c` looks every
15 seconds (every minute before 1.73), and only between applies, at the theme that is applied, if its style
says to (`icon-style/style.ini`, `auto=1`) and its template is whole
(`base.png`, `mask.png` and `known.txt`; without the list of titles
installed then, every title would look new, so nothing is styled and the log
says to open the theme in the manager, which keeps it again):

- **Which titles:** a folder in `/user/appmeta` named like a title ID, not in
  the titles installed when the page last used the style (`known.txt`), not
  styled here before (`made.txt`), and with no file of the theme's for it (a
  title you changed by hand; the page styles those, with your picture kept
  in `icon-style/own/<ID>.png`). A title whose icon you put back is never
  styled again behind your back.
- **When:** once nothing in its folders (`/user/appmeta/<ID>`,
  `/user/app/<ID>`, its `sce_sys`), its package (`app.pkg`, or a patch's
  under `/user/patch/<ID>`) and its download (`/user/download/<ID>`, since
  1.73) has changed for 2 minutes (10 before 1.73). The console writes those while it
  installs, downloads or updates a game (a package grows without its folder
  changing; a download is written in place, its size fixed from the start),
  and a theme over them would keep it from writing, so the styler waits. A
  theme deleted while a title is styled gets nothing written into it.
- **Timed on its own clock** (since 1.73): each look takes a print of every
  one of those files' dates and sizes, and the 2 minutes run from the last
  time the print changed, by the plugin's own clock. Before, they ran from
  the newest date on the files, and an installer can date them wrong: one
  dated an app's files an hour ahead of the console's clock (2026-10-29), so
  the wait couldn't start for an hour, and the newest date hid its download
  still being written. Only at the first look (the plugin just started) do
  the dates say how long a title has been as it is, never more than until
  now. The page asks the same (`autostyle_wait`).
- **How:** the title's own icon is read as pixels (`png_decode_rgba`: 8-bit
  RGB or RGBA PNGs, at most 2048 pixels a side, through the same inflate the
  checks use, since a homebrew package's icon is untrusted), set into the
  shape over the page's template, and written as each of its icon files is:
  `png_encode_like` keeps every chunk of the console's PNG and its zlib
  header, with stored blocks; `texture_encode_like` writes the console's DDS
  header and every mip, with the page's own BC7 mode-6 and BC1 encoders.
  Each file is checked exactly as an upload is (`asset_validate`) before it
  goes into the theme; a title whose files can't all be made (a compressed
  texture, a picture of another kind) is left for the page. Each title is
  tried once per run of the plugin.
- **With a game open too:** 1.67 to 1.69 waited while a game was being
  played, judged by its use of the processor, on the idea that the console
  suspends a game left on the home screen. Measured on 13.42 (2026-10-29)
  it doesn't: Yakuza 0 left on the home screen used about 41% of one core,
  and 21 to 44% while played, and the home screen's own process was no
  surer guide. Waiting for no game to be open could mean waiting for good,
  so since 1.70 the styler doesn't wait. An apply never touches a game's own
  sandbox (its 31 mounts held nothing of the theme's), only the home
  screen's folders and the system's. `/api/diag/procs` shows each process's
  processor time.
- **Then:** the theme is applied again (only the new title's folders are
  new), quietly (`reapply_quietly`, since 1.72): no "applying…", no steps,
  no tips, only the note, by the games' names (`title_name`: its
  `param.json` in its default language, else English, else any; a PS4
  title's `param.sfo`; else the ID), once the apply is done. An apply you
  ask for meanwhile is said as usual, the note after it. A title it can't
  style is said too, for the page.
- **Told once:** a new title (or one coming back) seen still installing is
  said once per install, with when the style follows.
- **The page follows:** the status carries `titles`, a short text that
  changes when a title folder appears or goes in `/user/appmeta`, and when
  the styler styles, puts back or leaves a title (`titles_stamp`). An open
  page reads the titles and the theme again when it changes. The style's
  answer lists the titles still installing, with the seconds left
  (`autostyle_wait`, the same rule), so the page styles none of them early
  either, and the kept template (`part=base`, `part=mask`), against which
  the page tells an icon the style made from one changed by hand.
- **Installed again:** an apply lists the titles the theme has pictures for
  whose metadata folder isn't there (`titles_awaiting_install`). Once one is
  installed again and its install is over, the theme is applied again for
  it (since 1.70), once per install. If its pictures were the style's (the
  page's list, `icon-style/styled.txt`, or `made.txt`), they are made again
  first with the template as it is then, from `icon-style/own/<ID>.png`
  where there is one, else from the title's own icon.

A title that isn't installed (uninstalled, or not yet) keeps its pictures in
the theme: an apply leaves its folders out and logs "isn't installed; its
pictures stay in the theme", without calling the apply partial.

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
or broken client can't hold up others. A request's line and headers get 10
seconds in all (since 1.77; before, only each read was timed, and a client
sending a byte every few seconds could hold a worker for as long as it
liked).

The page (518 KB) and the ATRAC9 module (84 KB) are built into the plugin
compressed too, and a browser that takes gzip gets them at a third of the
size (151 KB and 28 KB). Both are asked for again each time the page opens
(`no-cache`), and while the plugin is the same they are answered "not
modified" (an `ETag` of the plugin's version and a checksum of the bytes),
so an updated plugin's page is never an old one (the plugin's logo too,
sent as it is: a PNG is compressed already). A theme's files and
preview pictures have an `ETag` of their own (the file's identity, size and
time: they are only ever replaced, never changed in place): the library
redrawing its cards costs a few bytes for each picture, not the picture.
Before 1.77 everything was sent whole every time (`no-store`). It serves the manager (one HTML page
and the ATRAC9 module, both built into the plugin) and a JSON API. The page
reloads itself when the plugin is updated, so it never runs code older than
the plugin, and reads the console's games again when the status' `titles`
changes (since 1.72), never while it is sending something.

The status the manager asks for every few seconds (every second during an
apply), from every device that has it open, used to walk every theme's
files each time. Since 1.66 that walk (each theme's file count and the
pictures on its card) is kept: until the plugin writes anything (the write
guard counts its writes, after each one, so a change made through the
plugin shows at once) or for 10 seconds, so a change made from outside, over
FTP, shows within that. Everything else in the status (names, music, the
unsaved mark, the preview picture) is read every time.

## The tile

A deep-link app: only `param.json` and `icon0.png` under
`/user/app/THEM00001`, with a `deeplinkUri` that opens the local manager in
the console's browser. It is registered through AppInstUtil. On firmware
without the single-title install function, the plugin uses the system's
registration scan instead, as etaHEN's Toolbox card does. An ownership marker
and a receipt keep it from being registered twice.

## Threads, and what they share

The plugin's threads: the engine (applies), the web listener and its four
workers, the tile installer, the interface guard and the icon styler. What
they share, and how:

- **A theme's files** change under one lock (`themes_lock`): an apply holds
  it throughout; uploads, settings, the icon style's settings and list, and
  the styler's placing of its files take it for their change.
- **Each whole-file write** goes through a temporary name of its own
  (`fs_write_file`, since 1.66), so two threads writing one file (the chosen
  theme, from two requests at once) never write into each other's, and the
  last one wins whole. The temporary names end in `.part`, which applies and
  listings skip, so one cut off by a crash or a power loss is never taken for
  a theme's file.
- **The chosen theme** is written only by a request to apply one; the
  styler only asks for the chosen theme to be applied again, and only if it
  is still the theme it styled for (`reapply_active`), so a theme chosen
  while it works is never undone.
- **Caches** (the status's walk of each theme, the interface icons' index,
  the remembered checks) each have their own lock, held briefly, with no
  other lock taken inside it, so no two threads can wait on each other.

- **Discard** puts a theme back by moving its folder aside and the saved
  copy in its place. For that moment the theme has no folder, so it does it
  holding a lock of its own to write (`swap_lock`): a request that finds the
  folder missing waits for it and looks again, and the status and the short
  reads of a theme (its files, choices, icon style, originals) hold it to
  read, so none ever sees a theme gone or half put back. An upload is
  received in a folder of its own (`/data/ps5-theme/incoming`, emptied at
  each start), not in the theme's, and moved in under the themes lock once
  it has passed its checks, so an upload under way neither goes with a
  folder moved aside, copied or deleted meanwhile nor fails for it (since
  1.77; the seeded test of every combination found all three: "no such
  theme", "could not store the file" and a good picture refused as
  unreadable). No request that only reads takes the themes lock, so the two
  are always taken in the same order. A theme's folders are made only under
  the themes lock, as a file is put in place, by the manager and the icon
  styler alike (since 1.78: an upload made the folders for its file before
  it was received, and doing that while Discard had the theme moved aside
  left Discard unable to put either copy back, with the theme's files left
  aside and the theme empty; the same test found it). A Discard cut off
  between its two steps by a crash or a power loss is put right at the next
  start: the theme's files go back.
- **Waits and intervals** (the update check, the watcher's look every 30
  seconds, a view refused for a minute, the guard's quick looks after the
  home screen exits, a request's 10 seconds, the notes during an apply) are
  timed on a clock that only goes forward (since 1.77): the console sets
  its own clock from the network after it starts, and an interval measured
  across that stalled or ended early.
- **Folder walks** never go deeper than any of the console's folders (32
  levels for a theme's files, 64 for deleting and copying), and the walks a
  web worker makes keep their paths on the heap, not its 512 KB stack: a
  theme with folders nested 100 deep (put there by hand) crashed the status
  until 1.77. Deleting such a theme deletes nothing and says to use FTP,
  rather than leaving part of it.

ThreadSanitizer runs the whole suite (`tests/host/run.sh tsan`, see
[Testing](testing.md)) without a race reported.

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
