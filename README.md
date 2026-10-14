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

Version 1.56. Developed and tested on a PS5 with firmware 13.42, both with
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
- [Changing game and app icons](#changing-game-and-app-icons)
- [A tour of the manager](#a-tour-of-the-manager)
- [How it works, and why](#how-it-works-and-why)
- [Theme format](#theme-format)
- [Challenges and how they were solved](#challenges-and-how-they-were-solved)
- [Testing](#testing)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [Firmware support and known limits](#firmware-support-and-known-limits)
- [Security](#security)
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
  video, textures and other images, the home screen's own tiles and the
  interface's icons.
- **Game and app icons.** Change the icon, background and logo of any
  installed game or app, homebrew and PlayStation Network artwork included.
  The Game Library shows them at once, and the home screen after rest mode;
  games and apps keep starting as before (see
  [Changing game and app icons safely](#changing-game-and-app-icons-safely)).
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
- **Never writes the system.** Nothing under `/system` or `/system_ex` is
  opened for writing, only media files can be themed, and every file is
  checked against the system file it replaces (see
  [what the checks cover](#what-the-checks-cover-and-what-they-dont)).

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

**If the manager can't be reached:** start ps5-theme again (from the Toolbox
or PS5 Payload Manager). The new copy takes over from the running one even
when that one doesn't answer, and keeps your theme. If that doesn't help,
start `ps5-theme-stop.elf` from a payload loader or PS5 Payload Manager, then
start ps5-theme again. It stops the running copy and leaves your theme on the
screen, apart from the games' and apps' pictures until ps5-theme runs again
(see [Stopping without a restart](#stopping-without-a-restart)). No restart
is needed in either case.

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
6. **To hear new home music,** move to a game or app on the home screen that
   has its own music, so the console switches to that music, then come back.
   The console keeps playing the music it already had loaded and only picks
   up the theme's music when it starts the home music again.

To change only the music, make a theme with just **Home music**. Everything
else stays stock.

## Changing game and app icons

Any installed game or app, homebrew included, can get its own icon,
background and logo in a theme.

1. Open the manager and press **Edit** on a theme, or create one first.
2. Switch on **Games and apps**. Your installed games and apps are listed by
   name; when there are many, type in the search box.
3. Press the button for the picture you want to change:
   - **Change icon**: the game's tile on the home screen and in the Game
     Library.
   - **Change background**: the picture shown behind the home screen while
     the game is selected.
   - **Change logo**: the game's name as artwork, where the game has one.
   - **Change other artwork**: other pictures some games keep.

   Hold the pointer over a button to see where its picture appears.
4. Choose an image. Any size works: it's fitted as set in **How images are
   fitted** at the top of the page (fill, fit or stretch), then written in
   exactly the console's format into every file that holds that picture. The
   message says how many files were changed.
5. Do the same for other games, then press **Save** and **Apply to console**.
6. The Game Library and the backgrounds show the new pictures straight away.
   For the icons on the home screen, **put the console in rest mode and wake
   it**; the notification after applying says so too. After a console
   restart it's the same: the home row picks the icons up after the first
   rest mode.

**To undo** one game's pictures, press **Reset** on its row, then **Save**
and **Apply to console**. **Restore stock** takes the whole theme off.

**Good to know:**

- Before installing an update for a game whose pictures the theme changes,
  choose **Restore stock**, then apply the theme again afterwards: while the
  theme is applied, the console can't update that game's pictures.
- Some games' pictures come from PlayStation Network as WebP files. Safari
  can't make those, so use Chrome, Edge or Firefox for them.
- When two editions of a game share their PlayStation Network artwork, the
  row says "shares pictures with" the other, and changing one changes both.
- Games and apps keep starting while their pictures are changed. [Changing
  game and app icons safely](#changing-game-and-app-icons-safely) explains
  how.

### The console's own tiles and interface icons

- **Home screen tiles** (PlayStation Store, Game Library, the Media tab's
  tiles and others): switch on **Home screen tiles**, then press **Change
  icon** or **Change background** on a tile. Not sure which tile is which?
  **Number tile icons** turns each icon into the number shown in front of its
  name.
- **Interface icons** (search, settings, the Control Center and more):
  switch on **Interface icons**, type a picture's name (search, settings,
  power and so on) in the lookup box, then choose an image. To find a
  picture by sight, use **Number vector icons**: apply the theme, use rest
  mode, then type the number you see into the lookup box.
- **Fonts**: in **Fonts**, replace the text fonts the home screen and Settings
  use. **Use one font for all Latin interface text** changes the Latin text
  in one step; fonts marked **Home screen** are the ones it draws with. See
  [Fonts](#fonts).
- **Corner shapes**: in **Interface icons**, change how round the
  interface's buttons, switches, menus and other parts are, part by part
  (0 makes a part square-cornered), then press **Use these shapes**. The
  home screen's tiles keep their shape.
- Like game icons, all of these show on the home screen after rest mode.

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
  picture) or stretch. When you attach a picture to a texture or an image,
  the theme keeps your original too (never applied to the console; one
  picture attached at several sizes is stored once). Changing this setting
  then makes every picture and texture that has a kept original again with
  the new fit, with a progress bar, and without attaching anything again.
  Pictures attached before originals were kept stay as they are, and a
  message says how many. The Appearance pieces have their own placement.
  If a changed fit does not show on the console after you apply the theme
  again, change the setting and then attach the pictures again: attaching
  always uses the setting as it is.
- **Keep original pictures:** on by default. Switch it off in a theme to save
  space: new pictures are then not kept, and fit and placement options apply
  when you attach a picture (as before). The line beside the switch shows how
  many originals are kept and what they take (a picture attached at several
  sizes counts once), with **Remove them** (click twice) to free the space;
  the theme's own files are not touched. The choice is stored with the theme
  (a file `originals-off` in its folder) and goes with a copy of it.
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

**Hearing the new music:** the console keeps playing the music it already
had loaded, so the theme's music doesn't start the moment it's applied. Move
to a game or app on the home screen that has its own music, so the console
switches to that music, then come back: the home screen then starts its music
again and plays the theme's. The same applies after a restart, since the home
screen loads its music before the jailbreak and the plugin start. The result
of the apply, the section's note and a notification on the console say so.

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

- **Fonts:** replace one of the console's text fonts (the interface's own
  SST, and the Arabic, Thai, Vietnamese, Japanese, Korean and Chinese ones)
  with a TTF or OTF font of your own. See [Fonts](#fonts) below.
- **Animations:** JSON animation and data files. They must be valid and keep
  their canvas size.
- **Video:** clips in the same container (MP4 or WebM).
- **Appearance themes:** the artwork of the PlayStation, PlayStation 2, 3
  and 4 themes in **Settings > Appearance and Sound**: the Control Center's
  bottom panel and top-right corner piece, and the home screen's artwork.
  See [Appearance themes](#appearance-themes).
- **Textures:** other system DDS textures. Any image is encoded to the
  texture's exact format and size.

#### Interface icons

The console draws many of its interface icons, buttons and symbols (search,
settings, controller buttons and more) from resource files in
`/system_ex/vsh_asset`. This section lists them and how many pictures each
holds: on 13.42, `Sce.PlayStation.PUI_UI3.rco` holds 1,432.

**Use your own pictures:** type a picture's name (search, settings, home,
power, trophies and so on) or a number from the test into the lookup box,
then choose an image for it. It is written in exactly the format of the
console's picture. For a vector icon, your picture is drawn inside the
console's own SVG. **Reset** puts one picture back; with nothing typed, the
box lists the pictures the theme changes.

It also offers a test that shows which picture is where, one file at a
time. **Number vector icons** (start with these, in
`Sce.PlayStation.PUI_UI3.rco`) or **Number all** turns each picture of that
file into its own number on a pink square, drawn in exactly the format of
the console's own picture. Apply the theme, then put the console in rest mode
and wake it: the interface reads these files when it starts. Type any number
you see into the lookup box to find out which picture it is. **Remove** puts
the console's own pictures back. If the interface keeps restarting with the
theme applied, the plugin takes the theme down by itself (see
[When the interface keeps restarting](#when-the-interface-keeps-restarting)).
On 13.42 the test showed, among others: search 9406 (`iconid_search`),
settings 9414 (`iconid_settings`), and the Control Center's Home, Switcher,
Notifications, Game Base, Music, Downloads, Sound, Mic, Accessories and Power
(`iconid_home`, `iconid_switcher`, `iconid_notification`,
`iconid_game_base`, `iconid_music`, `iconid_download`,
`iconid_sound_speaking`, `iconid_mic`, `iconid_game_controller_1`,
`iconid_power`). Nothing is drawn beside the **Games** and **Media** tabs.

**Corner shapes:** below the files, **Corner shapes** lists how
round the interface's corners are, as the shape list in
`Sce.PlayStation.PUI_UI3.rco` gives them: buttons, switches, slider handles,
tooltips, menus, tags and so on, in pixels at 1080p, and at the end an
**Overall multiplier** (`borderRadiusRatio`). Change any number and
press **Use these shapes**; **Console's shapes** puts them back. Numbering
the file's pictures keeps your shapes. The plugin accepts the list only with
the console's own names, in the same order, each a plain number from 0 to
100 (0 to 10 for the multiplier), and nothing else in the file may change.
On the console, switches set to 0 came out square-cornered. The multiplier
at 3 changed nothing that could be seen, and the home screen's tiles keep
their shape whatever the numbers. Their corners are set in the home screen's program,
not in a file it reads (its own resource files hold only pictures), and the
plugin changes only pictures, sounds and data.

#### Home screen tiles

The console's own tiles (PlayStation Store, PlayStation Plus, Game Library,
Media Gallery, Welcome and others) take their icons from each app's own
`icon0.png`, not from the interface files. This section lists them by name,
with **Change icon** (written in exactly the format of the console's icon,
every copy of it) and, where the tile has one, **Change background** (the
picture shown while the tile is selected). **Number tile icons** is the same
kind of test as above: each icon becomes the number in front of its name.
On 13.42 this was seen working on the console for the Games tab's tiles and
the Media tab's Discover, Music Home, All Apps, Live TV and App Library, and
Discover's own background.


#### Games and apps

Every installed game and app, homebrew included, keeps its pictures in its
metadata folder, `/user/appmeta/<title ID>/`, and most PS5 games also in
the `sce_sys` folder of their installation, `/user/app/<title ID>/sce_sys/`.
This section lists them by name with a **Change** button for each picture,
which writes your picture into every copy of it, each in exactly the
console's format (PNG with the console's own chunks, BC7 or DXT1 textures
behind the console's own header). A title's `param.json`, license, keys,
trophies and everything else stay protected, and so does the rest of
`/user/app` and the plugin's own tile.

Some games' tiles show pictures the console downloaded from PlayStation
Network instead, kept per title in `/user/catalog_downloader/appmeta/<title ID>/`
and per game in `/user/catalog_downloader/conceptmeta/<concept>/`, a folder
that a game's editions share (on 13.42, Death Stranding Director's Cut's tile
points at `conceptmeta/234585`). The plugin reads from the console's app
database, read-only, which titles use which concept folder, and lists those
pictures with the title, so **Change** writes them too. These files are
WebP or JPEG pictures under `.png` names, often in numbered copies
(`icon0_11.png`, `logo_00.png` to `logo_30.png`): each is written in the
format it really holds, and the numbered copies count as the same picture.
Browsers that can't make WebP pictures (Safari) can't change those copies;
Chrome, Edge and Firefox can. When two installed titles share a concept,
changing that picture changes it for both.

The buttons name each picture by what it is: **Icon** (the game's tile on
the home screen and in the Game Library), **Background** (shown behind the
home screen while the game is selected), **Logo** (the game's name as
artwork) and, for some games, **other artwork**. Hovering a button shows
where the picture appears and which files hold it.

After you apply the theme, the Game Library and the backgrounds show the new
pictures straight away. The home screen's icons change once you put the
console in rest mode and wake it: the home screen reads them only as it
starts. The result of the apply, and a notification on the console, say so.

On firmware 13.42 the home screen (ShellUI) reads a title's pictures only
through its own sandbox's view of `/user`, by the paths in the console's app
database (for example `/user/app/<title ID>/sce_sys/icon0.png`), so that is
the only place the plugin puts them: never the real folders (the game
itself starts from them) or other apps' views. When ShellUI closes (rest
mode), the system's launcher (SceLncService) takes its sandbox down, and an
overlay left inside makes that fail, after which no game or app would start
(error CE-105733-9) until a restart. So a thread of the plugin's own watches
ShellUI's process and takes the overlays out of its sandbox the moment it
exits, also while the plugin is busy applying a theme, and nothing is put
into that sandbox afterwards. ShellUI's next sandbox is covered as soon as
its views appear, before the home row is read. Until the plugin has found
ShellUI's process, games' and apps' pictures wait, so on a firmware where it
can't be found they are never put in place. [PS5 game icons that stayed the
same](#ps5-game-icons-that-stayed-the-same) and [Apps that wouldn't
start](#apps-that-wouldnt-start) tell how this was worked out.

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
2. **Preparing the theme: 120 of 300 MB.** The console's storage can't share
   files between folders, so a theme's folder is a full copy. It's made once
   and reused until the theme or the system's files change.
3. **Switching to the new theme.**
4. **Checking it on the console (11 of 15).** Every screen that shows the
   theme is read back and checked.

The top bar says **Applying…** until the console confirms the result. The
same happens after a reboot, when the plugin puts your theme back. Applying
can take up to a minute the first time on a firmware, and about 20 seconds
when you change a theme or switch to one you haven't just used. Applying the
same theme again, switching back to the previous one, or putting it back
after a reboot, takes a second or two.

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

Some views are views of views. The console shows `/user` again as
`/original_user`, and ShellUI's sandboxes then show
`/original_user/appmeta`. The plugin follows such a chain back to the real
folder (`/user/appmeta`), so a game's icon is overlaid in every place the
home screen might read it. It does this only for `/user`, the one place it
happens.

### The 88-character mount point limit

The PS5 kernel keeps a mount point's name in a fixed 88-byte field, as
FreeBSD 11 does. Deep React Native asset folders inside sandbox views are
longer than that, and mounting them fails with `ENAMETOOLONG`. The engine
then mounts the nearest parent folder that fits, staging that whole parent
with only your files changed.

### Only media, checked against the system file

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

### What the checks cover, and what they don't

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
[Security](#security).

### Verified end to end

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

### Stock copies, linked staging and the swap

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

### Progress reporting

The engine reports its step and percentage in the status API, and the
manager shows both. Copying (the first-time copy of the system's files, and
a stage folder where hard links don't work) is measured in bytes copied,
since it takes most of the time; checking is measured by overlay. The
percentage never goes backwards.

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
- A copy that has just started a newer one waits for it to ask before doing
  anything else. At boot, that means the newer copy puts the theme back, not
  the old one.
- An old copy in the middle of an apply finishes it first, and the new copy
  waits up to 3 minutes for that. The request carries the new copy's process
  ID, and the old copy steps aside only if that process is still there.
- A copy that isn't newer leaves the running one alone and exits. The new
  copy names itself `THEM00001`, as etaHEN's loader names plugins.
- If the running copy doesn't answer for 10 seconds (its web server can be
  down after rest mode, see [A manager that stopped answering](#a-manager-that-stopped-answering)),
  the new copy finds its process and asks it to step aside with a signal
  (`SIGUSR1`). Each copy writes its process ID and start time to
  `/data/ps5-theme/pid` once it holds the lock, so a later process that
  happens to get the same ID is never mistaken for it; the thread name is a
  fallback. Copies before 1.50 end on that signal, which also leaves their
  overlays in place.

**Why:** after rest mode etaHEN's Toolbox is often unavailable, and that was
the only way to restart a plugin without restarting the console.

### Stopping without a restart

`ps5-theme-stop.elf` is a small separate program for when the manager can't
be reached. Whether the plugin runs is decided by its lock alone, and the
tool changes overlays only while it holds that lock itself, never under a
running copy. If a copy runs, the tool finds it (by `/data/ps5-theme/pid`,
or its thread name) and asks it to stop (`SIGUSR2`). The plugin then takes
out only the overlays of games' and apps' pictures and exits. Those must not
stay in the home screen's view with nothing running to take them out at rest
mode (see [Apps that wouldn't start](#apps-that-wouldnt-start)). The rest of
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

### Appearance themes

The 30th anniversary Appearance themes (**Settings > Appearance and Sound**:
PlayStation, PlayStation 2, 3 and 4) draw their artwork from DDS textures in
`/system/vsh_asset`, not the `vsh_asset` folder under `/system_ex`:
`control_center_<theme>_bottom.dds` is the panel behind the Control Center's
row of icons, `control_center_<theme>_top.dds` the piece in the top-right
corner by the clock, and `homeui_<theme>.dds` the home screen's artwork
(`ps` is the original PlayStation). They are 2560 pixels wide (the top piece
980 by 620) and BC7. The home screen's code names them. They are plain files,
so a theme can replace them.

How they were found: the clock's capsule and the Control Center's backdrop
didn't turn into numbers in the numbered test, so they are not pictures in
the interface files. The file-level tracker (`ps5-assetwatch`) saw no file
load at all, because the home screen reads such files and closes them.
Naming the theme in use (PlayStation 2) led to the folder.

Two things had to be right for a replacement to be accepted, and both follow
the console's own files byte for byte (a texture made before 1.56, with the
old frame form, is rewritten with the system file's header when uploaded or
applied: the first 1.56 build skipped such files and reported "applied partly",
which is how this was found):

- **The format.** The DDS header is the system file's, copied unchanged. The
  files are zstd-compressed although their names end in `.dds`, so the plugin
  recognises compression by content. Their frame headers come in three forms
  on 13.42 (single segment `0xa0`, the same with a content checksum `0xa4`,
  and a windowed `0x80`); the page copies the form of the file it replaces,
  and writes the XXH64 content checksum when that form has one.
- **Transparency.** These textures are mostly clear: about half of a panel's
  pixels have no colour at all. Sony stores straight alpha and fills the
  clear pixels with colour (bright, mostly), so edges blend without dark
  fringes. The page encodes such textures as BC7 mode 6 with an alpha channel
  and does the same: clear pixels take the nearest visible colour, then white.
  A background texture stays opaque.

The textures show only while the matching Appearance theme is selected, and,
like every interface change, after rest mode. The manager lists them under
**Appearance themes** with plain names.

**Placing a picture.** These pieces sit against the screen's edge: the panel
at the bottom, the corner piece at the top right, the home screen's artwork
(solid at the top, fading to clear below) across the top. Each piece has a
**Placement** choice, made before the picture is attached: **Fill** (crop to
the piece, keeping the part against the screen's edge, so a wide picture
keeps its bottom clouds), **Fill with padding** (the same, leaving a clear
margin at that edge, in percent), **Fit** (the whole picture, clear sides) and
**Stretch**. First test on the console: a 16:9 cloud strip on the bottom
panel. Cropped around its centre it lost the lower clouds in the middle and
only the tall ones at the corners showed, and the clouds met the screen's
edge flat; placing it against the bottom edge, with padding, fixed both.

**Changing the options later.** A picture's options (placement, padding, the
clock band) apply the moment they are changed: the plugin keeps the original
picture in the theme (`sources/`, not applied) and the page makes the texture
again from it, so nothing is attached twice. A picture attached before that
was added has no original kept, so the first change takes the picture back out
of the texture itself (the page reads the texture it made, with its see-through
parts), keeps it as the original and makes the texture again from it. Nothing
is attached again; what the earlier placement had cropped away, or left clear,
is not in that picture, so attach the original once more to get it back. Only
textures the page itself made can be read back this way.

**If an option doesn't take effect.** When a changed option does not make the
texture again by itself, or the look does not change after you apply the
theme again, change the option and then **attach the picture again** (Replace
on that piece). Attaching always makes the texture fresh with the options as
they are at that moment, so it works whenever the automatic step did not.
Then **Apply** the theme again and put the console in rest mode. The options
(placement, padding, the clock band) are remembered by the browser you used,
so in another browser they start at their defaults: set them first, then
attach.

**The clock.** The corner piece is drawn over the clock and the search and
settings icons, which sit in its top band (the PlayStation 2 glass bar behind
the clock spans 7% to 22% of the piece, its glow 35%). A picture there that is
the clock's own white hides it: that was the second finding, a white cloud
there made the clock disappear. **Keep the clock and icons clear** (on by
default for the corner piece) leaves the top 26% empty and puts the picture
below it.

#### The three pieces, and how each is made

| Piece | File (`/system/vsh_asset/`) | Size | What it is | Placed against | Options |
|---|---|---|---|---|---|
| **Bottom** | `control_center_<theme>_bottom.dds` | 2560 by 820 | The panel behind the Control Center's row of icons (the PS button menu, on the home screen and in games) | the bottom edge | Placement, padding |
| **Top right** | `control_center_<theme>_top.dds` | 980 by 620 | The corner piece by the clock, with the search and settings icons | the top and right edges | Placement, padding, **Keep the clock and icons clear** |
| **Home screen artwork** | `homeui_<theme>.dds` | 2560 wide | The artwork that hangs from the top of the home screen (solid at the top, fading to clear below) | the top edge | Placement, padding |

`<theme>` is `ps`, `ps2`, `ps3` or `ps4`. Each Appearance theme has its own
three files, so a picture made for the PlayStation 2 pieces shows only while
the PlayStation 2 theme is selected.

How a piece gets onto the console, start to finish:

1. **The plugin lists the folder live.** `/system/vsh_asset` is an allowed
   target (the plugin only ever themes media), and is covered like the other
   asset folders: a read-only overlay of the staged folder over the real path
   and in the system's views of it. The console's own files are never
   written. The pieces appear in the manager under **Appearance themes**.
2. **The page makes the texture.** When you attach a picture, the page asks
   the plugin for the console's texture it replaces (its DDS header, size, and
   the zstd frame header of the file). It draws your picture on a canvas of
   the texture's size, as the piece's **Placement** says: against the piece's
   edge, with the padding left clear, and for the corner piece with the clock
   band left empty. It then fills the clear pixels with the colour of the
   nearest visible ones (white beyond 8 pixels), as Sony's files do, encodes
   BC7 mode 6 with an alpha channel, re-decodes a sample of the blocks to
   check the picture and the see-through parts, and wraps the result in zstd
   with the frame header of the console's file, and its checksum if that file
   has one.
3. **The plugin checks it again** on upload and at apply time: the DDS header
   identical to the system file's, the exact length, the same frame header
   bytes, the whole file one frame, and every BC7 block in a valid mode. A
   file that is right except for its frame header is rewritten with the system
   file's. Anything else is refused with a plain reason, so a bad texture
   never reaches the console.
4. **The original picture is kept** in the theme (`sources/`, never applied),
   so Placement, padding and the clock band can be changed later and the page
   makes the texture again at once. The **Keep original pictures** switch in
   the editor turns this off to save space.
5. **Apply** puts the overlays in place. The console reads the files when it
   builds the interface, which is why the change shows after rest mode.

What each piece needed, as found on the console:

- **Bottom:** a 16:9 picture centred on the panel lost its lower middle
  clouds and met the screen's edge flat. Placing it against the bottom edge,
  with padding, fixed both.
- **Top right:** the piece is drawn over the clock, which is white. A white
  cloud there made the clock disappear, so the top 26% can be kept clear.
- **Home screen artwork:** it hangs from the top, so placement and padding
  are measured from the top edge.

##### How this depends on the anniversary assets

Nothing here is added to the console: every piece replaces a texture that
Sony's own 30th anniversary Appearance themes already ship in
`/system/vsh_asset`, and the feature works only as far as those files do.

- **The files must be on the console.** The plugin lists the folder live from
  the console, so **Appearance themes** shows exactly the pieces that exist
  there. A console without the anniversary themes' textures shows none, and
  the plugin has nothing to replace. It carries no copy of Sony's textures.
- **Each piece is modelled on the console's own file.** The DDS header, the
  size, the number of mip levels and the zstd frame header of the file being
  replaced are read from it and copied, so a texture is accepted only if it
  matches that file byte for byte in all but its picture data. If a firmware
  changes a piece's size or format, the page follows the new file; a format it
  cannot write (anything but BC7) is refused with a reason.
- **The checked copy is the console's.** The first apply that touches the
  folder keeps a copy of it under `/data/ps5-theme/stock/<firmware>/`, checked
  against the system, and every apply builds the overlay from that copy with
  only your files replaced. The copy is per firmware, so an update to the
  console makes a fresh one.
- **A piece shows only with its own Appearance theme.** The pieces exist per
  theme (`ps`, `ps2`, `ps3`, `ps4`) and the home screen draws the ones of the
  theme selected in **Settings > Appearance and Sound**. Replacing the PS2
  pieces changes nothing while another theme is selected.
- **Sony can change them.** The names, sizes and formats above are those of
  13.42, where they were tested. The plugin checks each file against the
  console's own at every apply instead of assuming them, so a change on
  another firmware shows up as a refused file, not a broken screen.

##### Requirements

- **Console:** a jailbroken PS5 on firmware 13.60 or below, with this plugin
  at **version 1.56 or later** (earlier versions don't know the folder).
  Tested on 13.42 with etaHEN 2.5B and with PS5 Payload Manager (the plugin
  or the standalone `ps5-theme.elf`); the textures' formats on other firmware
  have not been checked.
- **The anniversary Appearance themes must be on the console** (their
  textures in `/system/vsh_asset`; see above), and the one whose pieces you
  replaced must be selected in **Settings > Appearance and Sound**. Pieces of
  another theme are not drawn.
- **Apply the theme**, then put the console in **rest mode** and wake it. The
  interface reads these files as it starts.
- **A picture:** PNG or WebP for see-through parts (a JPEG has none, so it
  covers the whole piece). Any size: it is fitted to the piece. A browser with
  canvas and `BigInt` (current Chrome, Edge, Firefox or Safari) makes the
  texture, since the console does not.
- **The theme's firmware** must match the console (or be set to **Any
  firmware**), as for every theme.
- **For the corner piece:** a picture that is light at the top hides the
  clock; leave **Keep the clock and icons clear** on, or use a dark one.
- **Disk space:** the textures are stored without compression in the theme
  (BC7 is one byte per pixel: about 3 MB for the bottom panel, under 1 MB for
  the corner piece). With **Keep original pictures** on, the originals take
  their size too, once however many files they were attached to.

#### Guide: changing the bottom panel, the top-right piece and the home screen artwork

1. **Make the picture.** A PNG or WebP, with transparent parts wherever the
   console's own artwork is clear (a JPEG covers the whole piece). Any size
   works, since it is fitted to the piece, but these match the pieces exactly:

   | Piece | Size | Notes |
   |---|---|---|
   | Bottom panel | 2560 by 820 | Sits behind the row of Control Center icons: keep it calm and not too light, so the icons stay readable. |
   | Top-right piece | 980 by 620 | The clock and the search and settings icons are drawn over its top band: a light picture there hides them. |
   | Home screen artwork | 2560 wide | Hangs from the top of the home screen. Sony's own is solid at the top and fades to clear by about two thirds of the way down: do the same, so the home screen's own background shows below it. |

   The home screen artwork is the Appearance theme's own layer, not the
   **Background** section's picture; the two are changed separately.
2. **Open the manager** at `http://<console-ip>:8095` on a computer or
   phone, make a theme (or press **Edit** on an existing one), and switch on
   **Appearance themes**. Each piece is a row named after its theme, for
   example **PlayStation 2: Control Center, bottom panel**, **PlayStation 2:
   Control Center, top-right corner** and **PlayStation 2: Home screen
   artwork**. Use the rows of the theme you have selected on the console.
3. **Choose how the picture is placed**, on the row, before or after
   attaching:
   - **Fill** crops the picture to the piece and keeps the part against the
     screen's edge it meets (the bottom for the panel, the top for the
     artwork and the corner piece).
   - **Fill with padding** does the same and leaves a clear margin at that
     edge: set it in percent (0 to 40).
   - **Fit** shows the whole picture with clear sides, **Stretch** squeezes
     it to the piece.
   - **Keep the clock and icons clear** (corner piece, on by default) leaves
     the top band empty and puts the picture below it. Turn it off only for a
     picture that is dark there.
4. **Attach the picture:** press **Attach** on the row, or drop the file on
   it. The page makes the texture, the plugin checks it, and the row shows
   **Replaced** with a message that it was added and verified.
5. **Adjust it.** Change the placement, the padding or the clock band and the
   texture is made again at once from the original the theme keeps (see
   [Changing the options later](#appearance-themes) above for when it
   isn't, and what to do). Judge the result on the console.
6. **Save** the theme (the bar at the bottom) when you are happy, or
   **Discard** to go back to the last save.
7. **Apply to console**, then select the same Appearance theme in
   **Settings > Appearance and Sound** if it isn't, and put the console in
   **rest mode** and wake it. Press the PS button for the Control Center, and
   look at the home screen for the artwork.
8. **Go back.** **Reset** on a row removes your picture (and its kept
   original) from the theme; apply again. **Restore stock** takes the whole
   theme off the console.

If it doesn't look right: a piece that stays as it was usually means a
different Appearance theme is selected on the console or the console wasn't
put through rest mode; a message that the theme was **applied partly** names
the file that was refused and why, in the **Activity** list.

### Fonts

The console keeps its text fonts as TrueType and OpenType files in
`/preinst/common/font`: `SST-*` for the interface's Latin text, with Arabic,
Thai, Vietnamese, Japanese, Korean and Chinese files beside them. Games read
the same folder, so a font put on the real folder would reach them. The
plugin puts a theme's font only where the home screen reads it: in ShellUI's
own view of the folder, as it does for game icons, once ShellUI's process
has been found, and takes it out of that view the moment ShellUI exits.
Other apps' views and the real folder keep the console's fonts.

Before a font is accepted, `src/font.c` reads its structure in memory. It
must be a whole TrueType or OpenType (CFF) font: its table list inside the
file, every table in bounds and none listed twice, the tables every font
needs (`cmap`, `head`, `hhea`, `hmtx`, `maxp`, `name`, `OS/2`, `post`) and its
outlines (`glyf` and `loca`, or `CFF `), with the glyph count, metrics and
glyph index tables agreeing, and a Unicode character map covering the
printable ASCII characters, so text doesn't come out with gaps. Collections
(`.ttc`) and WOFF files are refused. The console's own SST, Arabic and Thai
fonts pass the check, and so do common Latin fonts; symbol fonts and
single-script fonts without Latin punctuation don't.

**Which fonts the home screen uses.** The first console test replaced
`SST-Roman.otf` and nothing changed on the home screen, although the
plugin's log showed the overlay in ShellUI's view, before and after rest
mode. `ps5-fontscan.elf` (`tools/fonts`) lists the font files each process
has open or mapped, read-only. On a 13.42 console it showed that ShellUI
maps its fonts from the overlay's folder, so the overlay does reach it, and
that it uses only the **Light** and **Bold** weights: `SST-Light` and
`SST-Bold` for Latin text, with the Arabic, Thai, Vietnamese and Korean
Light and Bold files and the Japanese medium and bold. The regular and
medium Latin fonts (`SST-Roman`, `SST-Medium`) were mapped only by
`SceNKWebProcess`, the web renderer, so replacing them shows nothing on the
home screen. The manager marks the fonts the home screen uses and offers one
font for all the Latin weights at once. The two fonts in the system's
`font2` folders that ShellUI also maps (`DFHEI5-SONY2022.ttf`, `PS4Icon.ttf`)
can't be themed.

Fonts are read as the home screen starts, so apply the theme, then use rest
mode. If the home screen keeps restarting with a font, the crash guard takes
the theme down.

### Interface pictures in resource files

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
  pass the SVG check. Every other embedded file (sounds, strings, colours)
  must be unchanged.
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

### Overlays still in use

Switching themes or choosing **Restore stock** takes the current overlays
down. If an app is still reading from one (ShellUI keeps the home music and
its interface files open), the plugin waits a moment and then leaves it
where it is. It never forces it off. On 13.42, forcing it broke the
system's own views stacked on it ("Bad file descriptor"), and apps wouldn't
start until a restart. Instead:

- the new theme's overlay goes on top of the one in use;
- for **Restore stock**, a verified copy of the system's own folder goes on
  top, so the console looks stock;
- the app keeps reading what it already had open;
- the next switch tries again from the top down, and a restart clears
  everything.

A stage folder that an overlay still uses is never deleted.

### When the interface keeps restarting

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
| `/data/ps5-theme/stage/` | Stage folders behind the live overlays, and the previous theme's |
| `/data/ps5-theme/live` | The applied theme's folders, for a new copy to carry on with |
| `/data/ps5-theme/spare` | The previous theme's folders, kept for switching back |
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
live mount table, to see what the system made, and
`/api/title/where?id=<title ID>` lists where the system keeps one title's
pictures: the files named after it in the folders ShellUI can see, and the
strings in the system's databases that mention it. `/api/diag/watch?id=<title
ID>&seconds=<n>` reports which of that title's picture files, the console's
and the theme's copies, anything opens or reads in that time.

#### Updating when the Toolbox is gone

After rest mode, etaHEN's Toolbox often stopped working, and it was the
only way to restart a plugin, so a new version copied to the console waited
for a full restart. Restarting the plugin also took the theme down and put it
back. **Solution:** the plugin updates itself: it notices a newer plugin
file, starts it through etaHEN's ELF loader and hands over to it, and the new
copy keeps the overlays already in place.

#### Nothing running after a restart

After a restart, etaHEN started an older plugin while a newer one waited in
the update folder. The old copy started the newer one, but then carried on
putting the theme back, which took longer than the newer copy waited. The
newer copy gave up, and the old one then stepped aside anyway, leaving
nothing running. **Solution:** a copy that has just started a newer one
waits for it before doing anything else, the newer copy waits much longer,
and a copy steps aside only if the newer copy is still there.

#### The interface that wouldn't start

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

#### A guard that took down every theme

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

#### Changing game and app icons safely

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
- **A fixed set of console tests** (see [Tests on the console](#tests-on-the-console)),
  run the same way after every build.
- **The host test rig:** every failure on the console became a test, with a
  stand-in ShellUI process and sandboxes, applies held part-way, a silenced
  web server and thread names switched off as on the console. Each new test
  was also run with the fix taken out, to check that it fails then.

#### PS5 game icons that stayed the same

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

#### Apps that wouldn't start

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
own files on top (see [Overlays still in use](#overlays-still-in-use)), and
the crash guard also catches a slower loop: 5 restarts within 10 minutes. A
host test holds a theme's picture open while switching to another theme and
then to stock: the reader keeps its file, the new theme and then the stock
picture show, the stage folder in use stays, and once the file is closed the
next switch takes every layer off. With the old forced removal put back,
that test fails. Another test restarts an app every few seconds, never 3
within the short window, and checks that the fifth restart takes the theme
down.

#### A manager that stopped answering

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

#### A WAV file that never finished checking

Fuzzing the parsers turned up a WAV file whose check never ended: a chunk
size near 4 GB made the step to the next chunk wrap around to zero in 32-bit
arithmetic, so the same chunk was read forever. Anyone on the network could
upload one and tie up the manager. **Solution:** the step is computed in 64
bits, a host test uploads that file, and the fuzz targets now flag any input
that takes more than 10 seconds. The measured before and after are under
[Security](#security).

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

#### A slow start after every reboot

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

### Tests on the console

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

## Building from source

```bash
./build.sh
```

Produces `build/ps5-theme.plugin`, `build/ps5-theme.elf` and
`build/ps5-theme-stop.elf` in Docker, with
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
| `src/rco.c` | Interface resource file checks |
| `src/font.c` | The check a replacement font must pass |
| `src/png.c`, `src/inflate.c` | PNG pictures checked against the console's own, and the inflate they need |
| `src/texture.c` | DDS texture checks |
| `src/zstd.c` | Zstandard decoder with XXH64 |
| `src/atrac9.c`, `src/atrac9_tables.h` | ATRAC9 checks and codebooks |
| `src/tile.c` | The Themes tile |
| `src/crash.c`, `src/log.c` | Crash reports, logging and notifications |
| `src/platform_ps5.c`, `src/platform.h` | Everything specific to the console |
| `web/index.html` | The manager: library, editor, music editor, BC7 encoder, zip reader and writer |
| `web/atrac9.js` | ATRAC9 encoder and decoder (also the Web Worker) |
| `tools/stop/` | `ps5-theme-stop.elf`: stops the plugin and leaves the theme on |
| `tools/klog/` | `ps5-klog.elf`: the kernel log on TCP port 3232, for finding out what the system does |
| `tools/fonts/` | `ps5-fontscan.elf`: lists the font files each process has open or mapped, read-only, for the font test |
| `tools/assetwatch/` | `ps5-assetwatch.elf`: for two minutes, logs the files the home screen and the web renderer map and unmap, read-only, to find where something on screen is loaded from |
| `tools/` | Table generator, icon generator, plugin packer |
| `tests/host/` | End-to-end tests and the fake console (`rco_fixture.py` builds resource files) |
| `tests/atrac9/`, `tests/zstd/` | Codec cross-checks |
| `tests/fuzz/` | Fuzzing the parsers |
| `examples/sample-theme/` | A minimal theme |

## Firmware support and known limits

- Built for firmware up to 13.60; tested on 13.42 with etaHEN 2.5B unified.
- On other firmware, the asset list, sandbox views and tile registration are
  all discovered on the console, but they haven't been tried there. If a
  theme doesn't show, leave and re-enter the home screen, or jailbreak again;
  the theme is put back at start either way.
- New home music is heard only once the console starts its home music again:
  move to a game or app that has its own music, then come back (see
  [Home music](#home-music)). Its position is kept by the console (see
  [Music that starts part-way through](#music-that-starts-part-way-through)).
- After rest mode, the plugin covers the new views ShellUI's restarted apps
  make (see [Stock again after rest mode](#stock-again-after-rest-mode)). An
  app that read its background before the overlay arrived may show stock
  until it reloads; **Re-apply** brings it back at once. etaHEN's own Toolbox
  stopping after rest mode is outside this plugin.
- Background and icon sorting is by file name. Anything it misses is under
  **Other images**.
- The home screen's layout can't be themed (it is code). Some of its colours
  live in the interface resource files' palettes, which the plugin doesn't
  change yet.
- Interface pictures: the console reads its resource files when the
  interface starts, so changed ones show after rest mode or a restart. The
  numbered test drew nothing beside the **Games** and **Media** tabs on 13.42.
  Whether the console draws your own picture inside a vector icon, and
  whether the home screen uses changed tile icons, hasn't been seen on the
  console yet.
- etaHEN's plugin **Stop** ends the plugin at once: overlays stay until you
  choose **Restore stock**, start the plugin again, or reboot.

## Security

### Who can reach the plugin

- **Anyone on your local network.** The manager listens on port 8095 and
  has no password, so anyone on the network can upload files and apply
  themes. Use it on a network you trust.
- **Not web pages.** Requests that change anything must carry an
  `X-PS5-Theme` header, which browsers won't send from another site, so a
  web page opened on the console can't drive the API.
- **Not other machines, for updates.** The hand-over request used by updates
  is accepted only from the console itself.
- **What it can read back.** Beyond themes and the files they replace,
  anyone on the network can read the plugin's log, the mount table, and the
  names (not contents) of the files the system keeps for one game, with the
  database lines that mention that game.
- **Nothing outside your network.** The plugin sends nothing over the
  internet.

**Why this matters:** to mount overlays, the plugin runs as root, with
system-level rights and outside the console's sandbox. A memory bug in the
code that reads uploads could let someone on your network run code on the
console with those rights. So that code gets the most testing.

### What protects the system

- Every write goes through the write guard and stays inside
  `/data/ps5-theme/` and the tile's folder (see
  [Never writing the system](#never-writing-the-system), including what it
  doesn't cover).
- Themes are read-only overlays held in memory, so a restart always brings
  back the stock console.
- Only media can be replaced, never code, and each file is checked against
  the system file it replaces (see
  [What the checks cover, and what they don't](#what-the-checks-cover-and-what-they-dont)).

### Fuzzing results

The plugin's parsers for uploaded files were fuzzed with libFuzzer,
AddressSanitizer and UBSan (`tests/fuzz/run.sh`) on 12 October 2026 (the font
check on 13 October). Video isn't listed: only its first bytes are read. These
are the numbers from those runs, made after the fixes below:

| Parser | Inputs tried | Result |
|---|---|---|
| Font check | 7,279,891 in 2 min | no findings |
| DDS textures, with the frame checks (14 October) | 182,011 in 2 min | no findings |
| zstd decoder | 495,977 in 60 s | no findings |
| DDS textures | 473,116 in 5 min | no findings |
| ATRAC9 | 202,175 in 5 min | no findings |
| WAV | 3,781,560 in 60 s | no findings |
| PNG / JPEG / WebP headers | 7,098,883 in 3 min | no findings |
| JSON | 3,274,997 in 60 s | no findings |
| SVG check | 24,814,338 in 3 min | no findings |
| PNG pictures against the console's own (with the plugin's inflate) | 26,897,248 in 3 min | no findings |
| Interface resource files | 8,437,490 in 3 min | no findings |
| Interface resource files, structure kept (only pointers and embedded files vary) | 6,744,214 in 3 min | no findings |

"No findings" means no crash, no sanitizer report and no input taking more
than 10 seconds. The runs used a Linux x86-64 build of the same source, not
the console's build.

### Bugs found, and proof they're fixed

Each fix has a host test, and each test was also run against the old code to
make sure it catches the bug.

| Bug | Found by | Old code | Fixed |
|---|---|---|---|
| **A WAV file whose check never ended.** A chunk size of `0xFFFFFFF7` made the step to the next chunk wrap to 0 in 32-bit arithmetic. | Fuzzing: the WAV target stopped making progress, spinning at full CPU on a 181-byte file. | The test's 48-byte upload got no answer (timed out after 30 s) and tied up one of the manager's 4 worker threads for good. Four such uploads would leave it unable to serve anyone. | Refused at once: "not a valid WAV file". |
| **A texture unpacked with no limit.** A 25-byte file claiming to unpack to 200 MB. | Review | Room for 200 MB was set aside before the file was found to be invalid ("not a valid zstd-compressed texture"). | Refused before unpacking: "larger than the system texture". The texture fuzz target ran about 17 times faster afterwards (93 to 1,577 inputs a second). |
| **A file left open when saving failed.** `fsync(fd) \|\| close(fd)` skipped the close when `fsync` failed. | Review | 25 failed saves raised the plugin's open files from 8 to 33, one leaked per failure. | No file left open (the test allows a margin of 3 for open connections), `theme.ini` intact, no temporary file left behind. |
| **The interface crashing on start with numbered pictures** (1.31's test). | Console log | ShellUI restarted 10 times in 90 s (new sandboxes `NPXS40087_001` to `_010`) and the console stayed in rest mode. | Not yet confirmed on the console. That copy's pictures are refused by the new check (on 13.42's real file: "its size or pixel format differs", and Sony's `<svg>` opening not kept), while the new test copies are accepted. A host test shows a theme taken down after 3 restarts; with the guard switched off it fails ("the theme was not taken down"). |
| **No plugin running after a restart.** An older copy started by etaHEN started a newer one, then kept the newer one waiting while it put the theme back. | Console log | 13:49:43 newer copy starts; 13:50:03 it gives up; 13:50:31 the old copy steps aside anyway. Nothing ran until it was started again at 14:14. The host test fails: "the old copy applied before handing over". | On the console with 1.30: the old copy started 1.30 at 15:41:58, finished putting the theme back at 15:42:45 and handed over at 15:42:46, and 1.30 kept running. |

### What isn't covered

- **Fuzzing makes bugs less likely, not impossible.** It ran for minutes,
  not days.
- **The HTTP server is not fuzzed.** Request handling (`src/web.c`) is tested
  with malformed, oversized and slow requests in the host suite.
- **A file that passes can still upset the console's own decoders.** The
  checks are about format and structure, not proof that every decoder
  handles a file safely.
- **Another program on the console with full rights** could get around the
  write guard, but such a program can write the system directly anyway.
- **There is no password.** Anyone on your network can use the manager.

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
- **[PS5Themes](https://github.com/MasterPS0/PS5Themes)** by MasterPS0, whose
  theme packs showed which files hold the home screen's tab icons, tile icons
  and per-tile backgrounds.
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
