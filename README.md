<p align="center">
  <img src="docs/icon-rounded.png" alt="The PS5 Themes icon" width="128">
</p>

# PS5 Themes (ps5-theme)

Custom home-screen themes for jailbroken PS5 consoles on firmware 13.60 and
below, as an [etaHEN](https://github.com/GronedWaffel/etahen-11.00-13.60)
plugin or a standalone payload. Change the home screen's backgrounds, icons, sounds, fonts and home
music, keep as many themes as you like, and switch between them from the
console, a phone or a computer. The system is never written: every theme is a
read-only overlay held in memory, so the console is stock again the moment
the plugin stops, and the plugin puts your theme back each time etaHEN starts.

![The theme library](docs/screenshots/library.png)

Version 1.24. Developed and tested on a PS5 with firmware 13.42, both with
etaHEN 2.5B (unified build) and on its own from PS5 Payload Manager.

> **About the screenshots.** They were taken from the author's own console.
> The test themes in them use Peanuts artwork, which belongs to its rights
> holders and is not part of this project. The console's own system artwork
> is blurred on purpose: system files stay on the console and are never
> redistributed.

## Contents

- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Install](#install)
- [Running without etaHEN](#running-without-etahen)
- [Your first theme](#your-first-theme)
- [A tour of the manager](#a-tour-of-the-manager)
- [How it works, and why](#how-it-works-and-why)
- [Theme format](#theme-format)
- [Challenges and how they were solved](#challenges-and-how-they-were-solved)
- [Testing](#testing)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [Firmware support and known limits](#firmware-support-and-known-limits)
- [Network access](#network-access)
- [Removing](#removing)
- [Development Approach](#development-approach)
- [Acknowledgements](#acknowledgements)
- [Licences and trademarks](#licences-and-trademarks)

## What it does

- **Themes tile and web manager.** A **Themes** tile on the home screen opens
  the manager in the console's browser. The same manager works from any
  phone or computer on your network at `http://<console-ip>:8095`.
- **Build a theme from parts.** Switch on what you want to change and add a
  file: background, icons, home music, system sounds, fonts, animations,
  video, textures and other images.
- **Any file, fitted for you.** One image fills every size of an asset. Any
  audio file is converted to the exact format and loudness of the sound it
  replaces, including the console's ATRAC9 home music. The browser does this,
  and the console checks every file again.
- **Home music editor.** Pick the part of a song to use, an optional loop
  part, the order they play in, how the joints sound and the loudness, then
  hear it before you apply it.
- **Preview pictures.** Each theme's card shows its background, or any
  picture you choose, so you can tell themes apart at a glance.
- **Save and Discard.** Changes are kept as unsaved until you save them, and
  Discard puts the theme back as it was.
- **Clear progress.** Applying shows what the console is doing and how far it
  has got.
- **Library tools.** Duplicate, export to a `.zip`, import a `.zip` or a folder
  (drag it in), and delete.
- **Comes back by itself.** The applied theme is put back every time etaHEN
  starts the plugin, after a reboot or an update. Only **Restore stock**
  stops it.
- **Safe by construction.** Nothing under `/system` or `/system_ex` is ever
  opened for writing, only media files can be themed, and every file is
  checked against the system file it replaces.

## Requirements

- A PS5 on firmware 13.60 or below, jailbroken, running etaHEN or any
  jailbreak with the standard ELF loader (see
  [Running without etaHEN](#running-without-etahen)). Tested on 13.42 with
  [etaHEN 2.5B unified](https://github.com/GronedWaffel/etahen-11.00-13.60/releases/tag/v2.5B-unified.1),
  which covers 11.00 to 13.60 except 11.40.
- A phone or computer on the same network is the easiest way to add files.
  The console's own browser works for applying and switching themes.
- To build from source: Docker. Nothing else on the host.

## Install

1. Copy `ps5-theme.plugin` to `/data/etaHEN/plugins/` on the console.
2. Turn on auto-start, so it runs whenever etaHEN runs. Either:
   - create an empty file `/data/etaHEN/plugins/ps5-theme.plugin.auto_start`, or
   - turn on auto-start for it in the etaHEN Toolbox plugin list.
3. Start it from the Toolbox, or jailbreak again.

On its first start the plugin lists the home screen's own assets (before any
theme is mounted), starts the manager on port **8095**, adds the **Themes**
tile, and shows a notification with the address to open from a phone or
computer.

**Updating:** copy the new `.plugin` file over the old one in
`/data/etaHEN/plugins/`. Within about 10 seconds the running plugin finds it,
starts it through etaHEN's ELF loader and hands over to it. Your theme stays
on the screen the whole time, and no Toolbox or restart is needed, which
matters after rest mode, when etaHEN's Toolbox is often gone. An open manager
page reloads itself with the new version. (This works from 1.23 on: to get
1.23 itself from an older version, restart the console once.) A newer
`ps5-theme.elf` or `.plugin` in `/data/ps5-theme/update/` works the same way,
with or without etaHEN.

## Running without etaHEN

The plugin doesn't need etaHEN, only a jailbreak: a kernel exploit and the
standard PS5 ELF loader (port 9021) that ps5-payload-sdk programs run under.
It gets its own privileges, and everything else is its own code. Every
release has both files, which contain the same program:

- `ps5-theme.elf`: for any ELF loader, such as
  [PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager);
- `ps5-theme.plugin`: for etaHEN, the same program with etaHEN's header.

**With PS5 Payload Manager:**

- **Start it:** put `ps5-theme.elf` in a folder under
  `/data/pldmgr/payloads/` (or import it from USB) and launch it from the
  manager. It then does everything it does under etaHEN: the web manager,
  the Themes tile, and putting the selected theme back.
- **Start it after every jailbreak:** add it to the manager's autoload list.
- **Update it:** replace that `ps5-theme.elf` with the newer one (or put the
  newer one in `/data/ps5-theme/update/`). Within about 10 seconds the
  running copy finds it, starts it through the ELF loader and hands over,
  keeping your theme on the screen. Every build carries its version inside
  the program, which is how a plain `.elf` is recognised.

**Keeping both as a backup:** you can set up `ps5-theme.elf` in Payload
Manager and `ps5-theme.plugin` with auto-start in etaHEN at the same time.
Only one copy ever runs: when the second one starts, it finds the first, and
it exits if it isn't newer or takes over if it is. So whichever way starts
first, you get one copy, and the newest one.

**What's different without etaHEN:** nothing starts it at boot unless your
jailbreak or Payload Manager does. Both ways have been run on a PS5 with
13.42: `ps5-theme.elf` from PS5 Payload Manager, and `ps5-theme.plugin` from
etaHEN. If something works differently on your setup, the Activity page says
what happened.

## Your first theme

1. Open the **Themes** tile, or `http://<console-ip>:8095` on a phone or
   computer.
2. Press **New theme**, give it a name and press **Create**.
3. Press **Edit** on its card, switch on **Background** and drop in a picture.
   Add home music, icons or sounds the same way.
4. Press **Save** in the bar at the bottom, then **Apply to console**.
5. Give it a few seconds. The home screen may show the stock look for a
   moment while it switches.

To change only the music, make a theme with just **Home music**. Everything
else stays stock.

## A tour of the manager

### Library

![The theme library](docs/screenshots/library.png)

The library is the home page.

- **Cards:** each theme shows its preview picture, firmware, number of files,
  music length, and "unsaved changes" when it has some. The applied theme is
  outlined and tagged **Applied**.
- **Card buttons:** **Apply** (or **Re-apply**), **Edit**, **Duplicate**,
  **Export** and **Delete**. Delete asks for a second press. The applied
  theme can't be deleted.
- **Stock card:** **Restore stock** removes every overlay and stops the theme
  from coming back at the next start.
- **Top bar:** shows the console's firmware and what is applied, or what is
  being applied right now.
- **Banner:** under the title, the verified result of the last apply, such
  as "16 files verified on the console".
- **Import .zip:** imports a theme. You can also drop a `.zip` or a folder
  anywhere on the page.

The page refreshes by itself every few seconds, and every second while a
theme is being applied, so it stays correct even when you use it from two
devices at once.

### Creating a theme

![The new theme form](docs/screenshots/new-theme.png)

A theme needs only a name. **Firmware** is set to this console's firmware;
choose **Any firmware** on the edit page for themes you share. A theme made
for another firmware can be kept and edited but not applied, since system
files differ between firmware versions.

### The edit page

![The top of the edit page](docs/screenshots/edit-top.png)

Open a theme with **Edit** on its card. The menu next to the title switches
between themes.

- **Name** and **Firmware.**
- **How images are fitted:** fill (crop the edges), fit (keep the whole
  picture) or stretch. This applies to every image you add from then on.
- **Export** and **Apply to console** act on the theme as it is now. They are
  available as soon as the theme has anything in it.

Below that, each part of the home screen has its own section with a switch.
Switching a section off asks before removing its files.

#### Preview picture

The picture on the theme's library card. When you add a background, a
1280×720 picture is made from it, fitted the same way. **Choose picture**
uses any image instead, such as a photo of your TV with the theme applied.
**Remove picture** goes back to the background one. Preview pictures are
kept with the theme and exported with it, but never applied to the console.

#### Background

![The background section](docs/screenshots/background.png)

One image or video for the home screen's backgrounds. The rows are the
places a background is used: the default home background, the Game Library,
each system tile's background, and other screens. Switch on the areas you
want, then drop in one file. It's encoded separately for each area, at that
area's own format and size. On 13.42 the home screen's backgrounds are
3840×2160 textures, and encoding one takes a few seconds in the browser.

#### Icons and other images

![The icons section](docs/screenshots/icons.png)

Icons are shown as tiles with search. Click a tile, or drop a file onto it.
Many icons exist in several sizes; your one file fills every size variant.
**Other images** works the same way for every other replaceable image.
(System artwork is blurred in these screenshots.)

#### Sounds

![The sounds section](docs/screenshots/sounds.png)

System sounds and music clips, such as the wake-from-rest chime. Any audio
file is converted to the sound's own format (WAV or ATRAC9) and loudness. The
cold-boot sounds play before the jailbreak, so a theme can't reach them.

#### Home music

![The home music section](docs/screenshots/home-music.png)

The music the home screen plays. Add any audio file the browser can play
(MP3, AAC, FLAC, WAV and so on). The section then shows:

- the music's length and whether it is "Encoded and verified";
- a player for the song the theme keeps;
- **Choose part**, **Replace** and **Remove**;
- **Loudness**, relative to the console's own music, from -12 to +12 dB.

The note at the bottom explains something the console does by itself: the
home screen keeps its place in the music. After a game it carries on where it
stopped, and when you apply a theme the new music carries on from about where
the old music had got to.

![The music editor](docs/screenshots/music-editor.png)

**Choose part** opens the music editor:

- **Music part:** drag the white handles on the waveform, move a selected
  handle with the arrow keys (the controller's D-pad works too), or type the
  times.
- **Play order:**
  - *Repeat the music part.* You can also choose where it begins (the green
    marker).
  - *Loop part first, then the music part repeats.* For example, open on the
    chorus.
  - *Loop part and music part take turns.*
  - *Music part once, then the loop part repeats.* An intro, like the
    console's own music.
- **How the joints sound:** a crossfade of 1, 2 or 4 seconds, a fade out and
  in, or a straight cut.
- **The summary line** says in plain words what will play, for example
  "Plays 0:30.0 to 1:20.0 once, then repeats 0:05.0 to 1:20.0".
- **Play** previews it exactly as the console will loop it. **Hear where it
  loops** and **Hear the change of part** play the seconds around each joint.
- **Use this** encodes and verifies the home music. **Cancel** leaves
  everything as it was.

#### Fonts, animations, video and textures

- **Fonts:** one font file per system font, in the same container (TTF, OTF,
  TTC, WOFF, WOFF2).
- **Animations:** JSON animation and data files. They must be valid and keep
  their canvas size.
- **Video:** clips in the same container (MP4 or WebM).
- **Textures:** other system DDS textures. Any image is encoded to the
  texture's exact format and size.

#### Save and Discard

![The unsaved changes bar](docs/screenshots/save-bar.png)

Your first change to a theme brings up this bar at the bottom of the page.

- **Save** keeps the changes.
- **Discard** asks once ("Discard all changes?"), then puts the theme back as
  it was at the last save. If the theme is applied, the console is updated to
  match.

Unsaved changes are kept if you leave the page or close the browser. The
library card says "unsaved changes", and a reminder appears when you leave.
Imported themes start out saved.

### Applying

![A theme being applied](docs/screenshots/applying.png)

While a theme is applied, the library banner and the message in the corner
show what the console is doing and how far it has got:

1. **Keeping a copy of the system's files (first time only): 88 of 169 MB.**
   This happens only on the first apply on a firmware. It's the slow part,
   so it's counted in megabytes.
2. **Preparing the theme.**
3. **Switching to the new theme.**
4. **Checking it on the console (11 of 15).** Every screen that shows the
   theme is read back and checked.

The top bar says **Applying…** until the console confirms the result. The
same happens after a reboot, when the plugin puts your theme back. Applying
can take up to a minute the first time on a firmware, and a few seconds after
that.

The console shows its own notifications too, so you can follow along on the
TV without the manager open: "Applying 'Name'…" (or "Putting 'Name' back…"
after a reboot), then for a long apply the current step and percentage, at
most every 8 seconds so they don't pile up, and finally "'Name' applied (16
files verified)".

### Activity

![The activity log](docs/screenshots/activity.png)

The plugin's log: what was applied, every overlay and the files checked
through it, uploads with their checksums, and any errors in plain words. A
crash report from a previous run appears here too.

### On a phone, and on the console

<p>
  <img src="docs/screenshots/phone-library.png" width="300" alt="The library on a phone">
  &nbsp;
  <img src="docs/screenshots/phone-edit.png" width="300" alt="The edit page on a phone">
</p>

The manager works at phone width and in the console's own browser. A
selected control turns white and lifts, as in the console's own menus, and
the music editor's handles move with the D-pad. Adding files is easiest from
a phone or computer; the console's browser is best for applying and switching
themes.

### The Themes tile

The tile is a small home-screen app whose only content is a link that opens
`http://127.0.0.1:8095` in the console's browser. It is registered once and
kept across restarts.

## How it works, and why

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

### Never writing the system

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

Themes are applied as **read-only nullfs overlays**: a staged folder
(the stock folder plus your files) is mounted over the system folder. Mounts
live in memory only.

**Why:** writing themes into the system partition is the usual way consoles
get bricked, and it would break "reboot restores stock". Overlays give the
same everyday result with nothing to undo. The plugin only unmounts mounts it
can prove it created.

### Reaching the home screen through its sandbox views

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

### The 88-character mount point limit

The PS5 kernel keeps a mount point's name in a fixed 88-byte field, as
FreeBSD 11 does. Deep React Native asset folders inside sandbox views are
longer than that, and mounting them fails with `ENAMETOOLONG`. The engine
then mounts the nearest parent folder that fits, staging that whole parent
with only your files changed.

### Only media, checked against the system file

`src/policy.c` decides what a theme may replace: only media, and only inside
ShellUI's React Native apps (`/system_ex/rnps/apps`, `/mnt/rnps/apps`) and
its VSH assets (`/system_ex/vsh_asset`). Each file is checked against the
system file it replaces when it is uploaded, and again when the theme is
applied:

| Kind | Rule |
|---|---|
| PNG / JPEG / WebP | Same format and exact pixel size, so every display resolution stays correct |
| SVG | Must contain `<svg`; scripts, external links, `foreignObject` and entities are refused |
| WAV | Same codec, sample rate, channels and bit depth |
| AT9 (ATRAC9) | Exactly the system file's format; every block of audio unpacks cleanly; only coding features the system file itself uses; no more than twice its length |
| Fonts | Same container (TrueType, OTF, collection, WOFF, WOFF2) |
| JSON | Valid (depth-limited parser); same top-level type; animations keep their `w`/`h` canvas |
| MP4 / WebM | Same container |
| DDS / `.dds.zst` textures | Fully decoded (built-in zstd decoder, checksum verified); the DDS header must be byte-identical to the system texture and the data exactly the right length |
| Everything else | Protected: JavaScript bundles, executables, libraries and unknown types can't be replaced |

**Why:** a file that matches the original's format exactly is one the home
screen already knows how to use. Code is never themable. The home screen's
colours and layout live inside ShellUI's encrypted JavaScript; changing them
would mean patching ShellUI at runtime, which is deliberately out of scope.

### Verified end to end

- In the browser, every converted image and sound is decoded again and
  compared with what you gave it before it is uploaded.
- Uploads carry a CRC-32. The plugin checks it, stores the file under a
  temporary name, re-reads it and only then renames it into place.
- When applying, each file is checksummed after staging, and again through
  each live overlay. An overlay that fails is removed.
- Exports are read back before they are offered for download, and imports
  check every entry of the `.zip`.

### Stock copies, linked staging and the swap

An overlay must contain the whole folder, not just the files you replaced.
The home screen's asset folder is 169 MB on 13.42.

- **Once per firmware,** the first apply that touches a folder keeps a copy
  of it under `/data/ps5-theme/stock/<firmware>/`, checked byte for byte
  against the system.
- **Every apply** builds its stage folder by hard-linking that copy's files,
  which takes no time and no space, then writes your files in as new files.
  Theme and stock files are only ever replaced, never edited in place, so
  the stock copy can't change.
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

### Progress reporting

The engine reports its step and percentage in the status API, and the
manager shows both. The first-time copy is measured in bytes copied, staging
by folder, and checking by overlay. The percentage never goes backwards.

### Coming back after every start

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

### Updating in place

Only one copy of the plugin runs at a time: each copy holds a lock file for
as long as it runs, and the kernel releases it when the process ends.

- Every 10 seconds the running copy looks in etaHEN's plugin folders,
  `/data/ps5-theme/update/` and PS5 Payload Manager's payload folders for a
  newer copy of itself: a `.plugin` file
  (checked by its header: `etaHEN_PLUGIN`, the title ID `THEM00001`, the
  version, then an ELF) or a plain `.elf` (checked by the marker
  `PS5THEME-VERSION:<version>:THEM00001` every build carries).
- It sends that program to the ELF loader on port 9021 (the standard PS5
  payload loader, which etaHEN also runs), which starts it as a new process.
- The new copy finds the lock taken, asks the running copy's version, and if
  it is newer asks it to step aside (`/api/handoff`, accepted only from the
  console itself). The old copy exits, leaving the overlays, and the new one
  takes the lock and carries on with them.
- A copy that isn't newer leaves the running one alone and exits. The new
  copy names itself `THEM00001`, as etaHEN's loader names plugins.

**Why:** after rest mode etaHEN's Toolbox is often unavailable, and that was
the only way to restart a plugin without restarting the console.

### Save and Discard

The first change to a theme makes a snapshot of it in
`/data/ps5-theme/snapshots/<id>/`, made of hard links, so it is instant even
for a 180 MB theme. **Save** deletes the snapshot. **Discard** swaps it back
in with two renames and re-applies the theme if it's the applied one. The
snapshot is safe for the same reason as the stock copy: theme files are only
ever replaced, never changed in place. Pictures the library makes by itself
don't count as changes; a picture you choose does. **Duplicate** uses the
same hard links, so copying a large theme is instant too.

### Backgrounds: BC7 textures made in the browser

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

### Home music: an ATRAC9 encoder in the browser

The home screen plays `/system_ex/vsh_asset/bgm_home.at9`. On 13.42 it is
ATRAC9, 48 kHz 5.1, 300 kbps, with a loop. Browsers have no ATRAC9 codec, so
the builder carries its own (`web/atrac9.js`, run in a Web Worker):

- It encodes in exactly the system file's format: the same channel layout,
  sample rate, bitrate and codec settings. Your music goes to the front left
  and right channels; the others stay silent.
- It writes only the coding features the system file itself uses. On 13.42
  every system ATRAC9 file uses bit allocation mode 3, at most 13 bands
  (13.5 kHz, the console's own music bandwidth), precisions up to 7 or 8 and
  the same LFE coding. The encoder and the plugin both derive these limits
  from the console's own file.
- It fills the unused end of every superframe with `0x01`, as Sony's files
  do. See [the ticking music](#the-music-that-ticked).
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

### Preview pictures

A theme's card shows `picture.jpg` (chosen by you) if there is one, otherwise
`preview.jpg` (made from the background). Both are 1280×720 JPEGs checked by
the plugin. For themes made before preview pictures existed, the library
reads the theme's own background once (an image, a video frame, or a
texture decoded in the browser) and makes the picture.

### Export and import

Export builds a `.zip` in the browser: `theme.ini`, every file under
`files/`, the song and the preview pictures. It reads the result back before
offering it for download. Import accepts a `.zip` or a dropped folder, takes
everything under the theme's `files/` folder and uploads it file by file,
each checked by the console. Anything the console refuses (another firmware's
files, for example) is skipped and listed. The message says what came in,
for example "15 backgrounds and the home music, all verified".

### The web server

A small HTTP/1.1 server in C (`src/web.c`): one listener thread and four
workers fed by a bounded queue, which answers `503 busy` when full, so a slow
or broken client can't hold up others. It serves the manager (one HTML page
and the ATRAC9 module, both built into the plugin) and a JSON API. The page
reloads itself when the plugin is updated, so it never runs code older than
the plugin.

### The tile

A deep-link app: only `param.json` and `icon0.png` under
`/user/app/THEM00001`, with a `deeplinkUri` that opens the local manager in
the console's browser. It is registered through AppInstUtil. On firmware
without the single-title install function, the plugin uses the system's
registration scan instead, as etaHEN's Toolbox card does. An ownership marker
and a receipt keep it from being registered twice.

### Crashes, logs and threads

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

### Firmware independence

Nothing is tied to one firmware. Kernel access comes from ps5-payload-sdk,
the firmware version is read at runtime, and the asset list is built on each
console. System functions are looked up at runtime from the libraries etaHEN's
own services link, never loaded again with `dlopen` and never imported
directly, so a function missing on some firmware turns one feature off
instead of stopping the plugin.

## Theme format

A theme is a folder, or a `.zip` of one:

```
my-theme/
  theme.ini
  music.wav          optional: the song, 48 kHz 16-bit stereo PCM
  preview.jpg        optional: library picture made from the background
  picture.jpg        optional: library picture you chose (shown first)
  files/
    system_ex/vsh_asset/bg_hub_default.dds.zst     the default home background
    system_ex/vsh_asset/bgm_home.at9               home music, encoded from music.wav
    system_ex/rnps/apps/NPXS40008/assets/.../some_icon.png
```

Every file under `files/` replaces the system file at the same absolute path,
if it exists on the console and passes the checks above.

`theme.ini`:

```ini
name=My Theme
fw=13.42
music_gain=0
music_start=12500
music_end=80100
music_play_from=-1
music_loop_start=40000
music_loop_end=55000
music_order=loop-first
music_loop=crossfade
music_blend=1000
```

- `fw`: `any`, or a comma-separated list of exact versions such as
  `13.40,13.42`.
- `music_gain`: loudness in dB relative to the console's own music (-12 to 12).
- `music_start`, `music_end`: the music part of `music.wav`, in ms (an end of
  0 means to the end).
- `music_play_from`: with the `music` order, where playback begins, in ms
  (-1 for the start of the part).
- `music_loop_start`, `music_loop_end`: the loop part, in ms (-1 for none).
- `music_order`: `music` (repeat the music part), `loop-first`, `alternate`
  or `music-first`.
- `music_loop`: how the joints sound: `crossfade`, `fade` or `cut`.
- `music_blend`: the length of the crossfade or fades, in ms.

On the console:

| Path | What |
|---|---|
| `/data/ps5-theme/themes/<id>/` | Themes |
| `/data/ps5-theme/active` | The selected theme (`none` means stock) |
| `/data/ps5-theme/stock/<firmware>/` | The checked copies of system folders |
| `/data/ps5-theme/snapshots/<id>/` | Unsaved changes' snapshots |
| `/data/ps5-theme/stage/` | Stage folders behind the live overlays |
| `/data/ps5-theme/live` | The applied theme's folders, for a new copy to carry on with |
| `/data/ps5-theme/update/` | Drop a newer `ps5-theme.elf` or `.plugin` here to update |
| `/data/ps5-theme/logs/` | Log and crash reports |
| `/data/ps5-theme/dump/` | The asset listing and mount table, for reference |

## Challenges and how they were solved

#### Writing to a read-write system partition

etaHEN makes `/system_ex` writable, and a theme tool that writes there can
brick a console. **Solution:** a write guard that every write must pass
(paths, symlinks and the final component all checked), and themes applied
only as read-only overlays held in memory.

#### Overlays the home screen couldn't see

An overlay on the real path is correct when the path is read directly, but
ShellUI reads the system through its own sandbox views, and a mount on the
real path isn't visible through a view.
**Solution:** read the full mount table, find every view that shows a
themable folder, and overlay each one, checking every overlay afterwards.

#### Stock again after rest mode

After rest mode the home screen showed stock, and etaHEN's Toolbox stopped
working too. The log showed why: two of ShellUI's apps had restarted in new
sandboxes, and their new views of the system folders had no overlays.
Re-applying fixed it, because a re-apply covers every view that exists at
that moment, but it also forced the old overlay off while those apps were
using it. **Solution:** a watcher that notices new views as soon as they
appear and covers them, with nothing taken down. `/api/mounts` returns the
live mount table, to see what the system made.

#### Updating when the Toolbox is gone

After rest mode, etaHEN's Toolbox often stopped working, and it was the
only way to restart a plugin, so a new version copied to the console waited
for a full restart. Restarting the plugin also took the theme down and put it
back. **Solution:** the plugin updates itself: it notices a newer plugin
file, starts it through etaHEN's ELF loader and hands over to it, and the new
copy keeps the overlays already in place.

#### "File name too long"

Some overlays failed with `ENAMETOOLONG`. The PS5 kernel stores a mount
point's name in an 88-byte field, and deep asset folders inside sandbox
views are longer. **Solution:** mount the nearest parent that fits, staged
with everything else in it unchanged.

#### The tile on firmware without a function

The tile's registration function wasn't exported on every firmware.
**Solution:** fall back to the system's registration scan, as etaHEN's
Toolbox card does, and look every system function up at runtime so a missing
one can't stop the plugin. Loading system modules again from the plugin could
hang the dynamic linker, so the plugin only uses the libraries etaHEN's own
services already link.

#### Fitting every size and format

The same icon exists in several sizes, and every system file has an exact
format. **Solution:** the browser fits one image to each size and format, and
the plugin's policy refuses anything that doesn't match the original
exactly.

#### 4K textures without a texture tool

The home screen's backgrounds are 4K BC7 textures compressed with zstd, and
no browser can write either. **Solution:** a BC7 encoder in the page that
checks its own output, a zstd frame writer, and a full zstd decoder in the
plugin to verify every texture before it's used.

#### Music no one could hear

The first music feature played audio from the plugin itself, and the console
kept that silent. **Solution:** replace the home screen's own music file,
`bgm_home.at9`, so the console plays the theme's music with its own player.

#### No ATRAC9 encoder

Browsers have no ATRAC9 support. **Solution:** an encoder written for this
project, using the codebooks and bitstream layout from LibAtrac9 and checked
in tests against two independent decoders (LibAtrac9 and FFmpeg).

#### The music that ticked

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

#### Slow encoding in some browsers

Encoding was about 30 times slower in some browsers, because they throttle
timers in pages that aren't in front. **Solution:** run the encoder in a Web
Worker and yield with `MessageChannel` instead of timers.

#### Reading the system's own file while a theme is applied

With a theme applied, the system path shows the theme's file, so the editor
compared new music and textures with the theme instead of the system.
**Solution:** keep a checked copy of the system's files and read the stock
version from there.

#### Music that starts part-way through

After applying, the home music sometimes began near the end of the song. The
console's player keeps its place: after a game it carries on, and when the
music file changes it continues from the same point in the new file. Changing
that would mean changing how the console's own software runs. **Solution:**
the editor and README say so in plain words, and the editor describes what
plays from the start of the music.

#### "It forgot my theme after a reboot"

It hadn't. Putting the theme back took about 30 seconds (each apply copied
the 169 MB asset folder), and the manager showed "Stock" until it finished.
**Solution:** the stock copy and hard-linked staging, preparing the new
theme before swapping, an "Applying…" state with real progress, and a console
notification when the plugin puts the theme back.

#### An import that lost the backgrounds

An import brought back only the music. The importer only looked in the app
folders, not the home screen's asset folder, and a manager page left open
across an update kept running the old code. **Solution:** import takes
everything under the theme's `files/` folder, the page reloads itself when
the plugin is updated, and import and export say exactly what they contain.

#### Small things that confused people

Testing on the console turned up:

- an Apply button that only updated after leaving the page;
- a text cursor blinking in plain text;
- a separate Editor tab that duplicated the Edit buttons;
- a button that should have been hidden;
- no way to back out of changes.

Each was fixed: the edit page's buttons follow the theme, the cursor appears
only in text fields, editing lives under Library, and Save and Discard were
added.

#### Testing without the console

Only the console can run the plugin, and only its owner can test on it.
**Solution:** everything except `src/platform_ps5.c` also builds for Linux.
The real code runs against a fake console tree, with read-only bind mounts
standing in for nullfs, under AddressSanitizer and UBSan. The same fake
console serves the manager for trying the page in a desktop browser.

## Testing

```bash
tests/host/run.sh
```

Builds the real plugin sources for Linux with AddressSanitizer and UBSan and
runs 32 end-to-end tests against a fake console tree in a privileged
container. They cover:

- upload checks and the media policy;
- apply, re-apply, restore and stock copies with linked staging;
- read-only overlays, sandbox views and symlink containment;
- concurrent requests during an apply, slow and malformed clients;
- recovery after a hard kill, log rotation and the crash handler;
- new sandbox views made after a theme is applied (as after rest mode);
- updating in place, from a `.plugin` in etaHEN's folder and from a plain
  `.elf` in PS5 Payload Manager's folders or the plugin's own update folder:
  the program sent to a stand-in for the ELF loader, the hand-over, keeping
  the overlays, and a copy that isn't newer stepping aside;
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

## Building from source

```bash
./build.sh
```

Produces `build/ps5-theme.plugin` and `build/ps5-theme.elf` in Docker, with
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.43 and LLVM 18
on Ubuntu 24.04. The `.plugin` file is the ELF with etaHEN's plugin header:
`etaHEN_PLUGIN`, the title ID `THEM00001` and the version
(`tools/make_plugin.py`).

## Project layout

| Path | What |
|---|---|
| `src/main.c` | The theme engine: stock copies, staging, overlays, checks, progress, re-apply at start |
| `src/web.c` | Web server and JSON API: uploads, settings, Save/Discard, previews, status |
| `src/update.c` | One running copy at a time, finding a newer plugin file and handing over to it |
| `src/fsguard.c` | The write guard and file helpers (atomic writes, linked copies) |
| `src/policy.c` | What can be themed, and the checks for each kind of file |
| `src/verify.c` | Image header parsing |
| `src/texture.c` | DDS texture checks |
| `src/zstd.c` | Zstandard decoder with XXH64 |
| `src/atrac9.c`, `src/atrac9_tables.h` | ATRAC9 checks and codebooks |
| `src/tile.c` | The Themes tile |
| `src/crash.c`, `src/log.c` | Crash reports, logging and notifications |
| `src/platform_ps5.c`, `src/platform.h` | Everything specific to the console |
| `web/index.html` | The manager: library, editor, music editor, BC7 encoder, zip reader and writer |
| `web/atrac9.js` | ATRAC9 encoder and decoder (also the Web Worker) |
| `tools/` | Table generator, icon generator, plugin packer |
| `tests/host/` | End-to-end tests and the fake console |
| `tests/atrac9/`, `tests/zstd/` | Codec cross-checks |
| `examples/sample-theme/` | A minimal theme |

## Firmware support and known limits

- Built for firmware up to 13.60; tested on 13.42 with etaHEN 2.5B unified.
- On other firmware, the asset list, sandbox views and tile registration are
  all discovered on the console, but they haven't been tried there. If a
  theme doesn't show, leave and re-enter the home screen, or jailbreak again;
  the theme is put back at start either way.
- The home music's position is kept by the console (see
  [Music that starts part-way through](#music-that-starts-part-way-through)).
- After rest mode, the plugin covers the new views ShellUI's restarted apps
  make (see [Stock again after rest mode](#stock-again-after-rest-mode)). An
  app that read its background before the overlay arrived may show stock
  until it reloads; **Re-apply** brings it back at once. etaHEN's own Toolbox
  stopping after rest mode is outside this plugin.
- Background and icon sorting is by file name. Anything it misses is under
  **Other images**.
- The home screen's colours and layout can't be themed (they are code).
- etaHEN's plugin **Stop** ends the plugin at once: overlays stay until you
  choose **Restore stock**, start the plugin again, or reboot.

## Network access

The manager listens on port 8095 on your local network and has no password.
Requests that change anything must carry an `X-PS5-Theme` header, which
browsers won't send from another site, so web pages opened on the console
can't drive the API. The hand-over request used by updates is accepted only
from the console itself. Every write stays inside `/data/ps5-theme/` and the
tile's folder. The plugin sends nothing over the internet.

## Removing

1. Choose **Restore stock**.
2. Delete `/data/etaHEN/plugins/ps5-theme.plugin*` and `/data/ps5-theme/`.
3. Delete the **Themes** tile from the home screen.

## Development Approach

This project explores AI-assisted development for PS5 homebrew, with an
emphasis on practical validation rather than accepting generated code at face
value.

Claude Code was used to assist with implementation, debugging and iterative
improvements. Development was guided by testing on physical PS5 hardware,
examining application logs, investigating observed failures and verifying
fixes on the console.

The project combines hardware testing with host-side automated tests to
exercise application behaviour, error handling and recovery scenarios. The aim
is to evaluate how AI-assisted development can be used to build a useful
console utility while maintaining a focus on reliability, safety and
reproducibility.

AI assistance does not replace testing or eliminate the need for continued
review. Firmware compatibility and known limitations are documented
separately.

## Acknowledgements

This project stands on the work of people who share what they learn with
everyone. My deepest gratitude to each of them. Without their work, their
openness and the time they gave freely, none of this would exist.

- **etaHEN**, by LightningMods and its contributors, and
  **[GronedWaffel](https://github.com/GronedWaffel)** for the
  [11.00–13.60 unified build](https://github.com/GronedWaffel/etahen-11.00-13.60)
  this was built and tested on. etaHEN is the foundation everything here runs
  on: the plugin format, auto-start and the Toolbox card installer, whose
  approach the Themes tile follows.
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**, by John
  Törnblom and contributors: the toolchain, the kernel access and the
  examples the whole plugin is built with.
- **[kstuff-lite](https://github.com/EchoStretch/kstuff-lite)**, by
  EchoStretch, which ships with the etaHEN build this was developed on.
- **[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)**, by Alex Barney
  (MIT License): the ATRAC9 codebooks and bitstream layout behind the
  encoder and the plugin's checks, and one of the two reference decoders in
  the tests. Without it, home music would not have been possible.
- **[FFmpeg](https://ffmpeg.org/)** and its ATRAC9 decoder, the second
  independent reference in the codec tests.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** by
  drakmor, **[PS5-File-Explorer](https://github.com/juma-sayeh/PS5-File-Explorer)**
  by juma-sayeh and **[onionHEN](https://github.com/aydencharles/onionHEN)**
  by aydencharles, whose home-screen app registration and navigation work
  etaHEN's Toolbox card builds on, and through it the Themes tile.
- **[ps5-webkit-autoloader](https://github.com/itsPLK/ps5-webkit-autoloader)**
  by itsPLK, which showed that a home-screen app can open a page served by a
  payload on the console itself.
- **[PSDevWiki](https://www.psdevwiki.com/ps5/)** and everyone who documents
  the PS5 there, in particular `param.json` and its `deeplinkUri`.
- **Zstandard and xxHash**, by Yann Collet and the zstd contributors: the
  format specification (RFC 8878) and the educational decoder that the
  plugin's decoder follows.
- **Microsoft's Direct3D documentation** of the BC7 format, which the texture
  encoder follows.
- **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** by Swordpdf and
  **[ATRACTool-Reloaded](https://github.com/XyLe-GBP/ATRACTool-Reloaded)** by
  XyLe-GBP, studied while working out how to get music onto the home screen.
- Everyone in the PS5 homebrew community who tests, reports and explains:
  your write-ups and answers made the hard parts possible.

## Licences and trademarks

Copyright (C) 2026 Blaze414

- **ps5-theme is free software under the GNU General Public License,
  version 3 or (at your option) any later version** (GPL-3.0-or-later). The
  full text is in [`LICENSE`](LICENSE). You may use, study, change and share
  it; if you share a changed version, it must stay under the same licence
  with its source available. It comes with no warranty.
- Why the GPL: the plugin is built with
  [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), whose kernel
  interface is GPL-3.0-or-later (© John Törnblom), so the plugin as built is
  under the same terms.
- The ATRAC9 tables are generated from LibAtrac9, © 2018 Alex Barney, MIT
  License (`licenses/LibAtrac9.txt`), which allows their use in a GPL
  project.
- No system files are included. The plugin works with the files on your own
  console, and the stock copies it keeps never leave it.
- "PlayStation", "PS5" and "ATRAC" are trademarks of Sony Interactive
  Entertainment Inc. or Sony Group Corporation. This project is not
  affiliated with or endorsed by Sony.
- The screenshots in `docs/screenshots/` show the author's own test themes.
  The Peanuts characters in them belong to their rights holders, are not part
  of this project and are not covered by its licence.
