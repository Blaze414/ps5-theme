# Design decisions

The hard choices in the plugin, each with what forced it, what else was considered, and what it costs. For
contributors weighing a change: if it undoes one of these, the reasons are here. The parts and threads are in
[Architecture](architecture.md); the problems met along the way, in [Challenges](challenges.md). Part of the
[PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Overlays, never writes](#overlays-never-writes)
- [A write is made in the folder that was checked](#a-write-is-made-in-the-folder-that-was-checked)
- [Only media, each checked against the file it replaces](#only-media-each-checked-against-the-file-it-replaces)
- [SVG: the console's own subset, parsed](#svg-the-consoles-own-subset-parsed)
- [Whole folders, copied once and reused](#whole-folders-copied-once-and-reused)
- [No code in the home screen](#no-code-in-the-home-screen)
- [Nothing in a sandbox while the system builds it](#nothing-in-a-sandbox-while-the-system-builds-it)
- [An overlay in use is never forced off](#an-overlay-in-use-is-never-forced-off)
- [The media work in the browser](#the-media-work-in-the-browser)
- [One process, plain C, its own web server](#one-process-plain-c-its-own-web-server)
- [Devices allowed with a code the console shows](#devices-allowed-with-a-code-the-console-shows)
- [Notifications as the system's own, said once](#notifications-as-the-systems-own-said-once)
- [Updating by hand-over](#updating-by-hand-over)
- [Telling files apart by identity, not content](#telling-files-apart-by-identity-not-content)
- [A list of overlays that is appended, not synced](#a-list-of-overlays-that-is-appended-not-synced)
- [Discard by swapping folders, under a lock of its own](#discard-by-swapping-folders-under-a-lock-of-its-own)
- [Two clocks](#two-clocks)
- [Screenshots for the docs, read-only and without Sony's pictures](#screenshots-for-the-docs-read-only-and-without-sonys-pictures)

## Overlays, never writes

**Context.** The console's interface reads its pictures, sounds and fonts from `/system` and `/system_ex`. A jailbreak
can make those writable, and early theme tools wrote into them.

**Decision.** The system is never written. A theme is shown by mounting a read-only folder (nullfs) over the folder
the console reads, and over every view of it the system makes in apps' sandboxes. Removing the theme is unmounting;
a restart removes everything.

**Considered.** Writing the files (what a failed write or a bad file does to a console is permanent, and a tool that
writes there can brick one: see [Challenges](challenges.md#writing-to-a-read-write-system-partition)); one mount on the
real folder only (the home screen reads through its own sandbox views, where that isn't seen: see
[Challenges](challenges.md#overlays-the-home-screen-couldnt-see)).

**Costs.** An overlay replaces a whole folder, so every overlay holds a full copy of it (see the next decisions); the
console's 88-character mount point limit; every new view the system makes has to be found and covered.

## A write is made in the folder that was checked

**Context.** The write guard is what stands between the plugin and a system partition etaHEN made writable. Until
1.80 it resolved a path, checked where it led, and wrote by the path again; a folder swapped for a link in between
was followed ([Challenges](challenges.md#a-check-and-a-write-that-were-two-lookups)).

**Decision.** The folder a write lands in is opened from its root one part at a time, each as a folder, never through
a link, and the write is made relative to the open folder (`openat`, `renameat`,
`unlinkat`, `linkat`, `mkdirat`). Deleting walks folders the same way, deleting links rather than following them, and never starts where the
mount table shows something mounted inside.
What is read and then used is checked on what was opened: a theme's file is staged only if it is the file its check
passed, an overlay's top folder must be the stage folder built.

**Considered.** Resolving the path once more just before the write (narrows the gap, doesn't close it); FreeBSD's
`O_BENEATH` or `O_RESOLVE_BENEATH` (not in the console's kernel, which is FreeBSD 11's); holding the plugin's folder
open for its whole run (a folder replaced from outside would leave writes going to one that is gone).

**Costs.** A few more system calls per write (one open per folder on the way, about as many as resolving the path
took); the plugin's folder can't itself be a link to another place. On the PS5 these calls go through the plugin's
own, as the SDK's report a failure as if it were the result (see [Challenges](challenges.md#failures-that-looked-like-results));
the build fails if anything calls one of the SDK's.

## Only media, each checked against the file it replaces

**Context.** An overlay can show any file. A broken interface file or a program in the wrong place could stop the
console from starting its interface.

**Decision.** Only pictures, textures, sounds, fonts and the interface's resource files can be themed, never code or
libraries (816 such system files are refused outright), and each file is checked against the one it replaces: the
same format, size and structure, down to the texture's compression and the sound's frame layout. Checked when it is
uploaded, again when it is applied, and read back through the live overlay.

**Considered.** Trusting the browser (a page from anywhere could send anything); checking only the format (a picture
of the wrong size breaks the interface's layout).

**Costs.** Every file kind needs a parser, which is where most of the fuzzing goes; some things a theme could change
are refused because their checks don't exist yet.

## SVG: the console's own subset, parsed

**Context.** The console's interface draws SVG, and a theme can replace its SVG pictures. Until 1.80 a file was
refused if it contained one of a list of words (`<script`, `javascript:` and others): XML's namespaces, entities,
character references and spellings are more than a list foresees.

**Decision.** A parser (`src/svg.c`) that accepts only what the console's own 1,092 SVG pictures use, and refuses
anything else by name: no DOCTYPE or entities, one SVG root in the SVG namespace, drawing elements only, presentation
and geometry attributes only, links inside the picture only, embedded PNG or JPEG only, plain style rules only.
Character references are decoded before anything is checked.

**Considered.** A complete XML parser with a sanitiser that removes what's unsafe (far more code with the console's
privileges, and a file changed on its way in is no longer the one its author checked); converting every SVG to a
PNG (the console's own icons scale with the screen; a PNG wouldn't).

**Costs.** An SVG from a drawing tool may use something outside the subset (text, another filter) and be refused;
the message says what. A firmware whose own pictures use more would need the subset widened, which its own SVGs
would show at once in the tests.

## Whole folders, copied once and reused

**Context.** An overlay hides the whole folder under it, so it must hold every file of the folder, not only the
changed ones. The home screen's asset folder is 169 MB.

**Decision.** A verified copy of each system folder is kept once per firmware; each apply builds a stage folder from
it plus the theme's files; a stage folder built from the very same files is reused as it is, after a restart too.
The previous theme's are kept to switch back. The console's `/data` can't hard-link, so stage folders are copies.

**Considered.** Per-file overlays (thousands of mounts, and the 88-character limit); copying at every apply (30
seconds, with the stock look showing all that time).

**Costs.** Storage: one copy of each touched folder per firmware, and two themes' stage folders. 1 GB of the console's
storage is always left free.

## No code in the home screen

**Context.** Hooking or injecting into the home screen's process (ShellUI) would allow more: its layout, its colours
at run time, changes shown without rest mode.

**Decision.** Nothing runs inside another process. Everything the plugin does is files, mounts and the system's own
notifications and app registration.

**Considered.** Hooking ShellUI for live reloads and layout changes.

**Costs.** Some changes show only after rest mode (the home row's icons, fonts, the Appearance artwork), and the
layout can't be themed. In return a theme can't crash the home screen by anything but its files, which the checks
cover, and a firmware update can't turn a hook into a crash.

## Nothing in a sandbox while the system builds it

**Context.** At rest mode and on waking, the system starts the home screen and the apps that go with it again, each in
a new sandbox it builds view by view. Twice, the plugin covering views as they were made kept the console from
waking: the home screen's font in 1.67, two apps' views in 1.79
([Challenges](challenges.md#a-console-that-didnt-wake-again)).

**Decision.** Nothing goes into the home screen's sandbox until its process runs and is watched, and everything comes
out the moment it exits (1.68): in the order the system takes the views down, newest first, and with the two apps
that close with it (1.89). From its exit until the next one is watched, nothing goes into any sandbox; and a view in
any other app's sandbox is covered only once it has been there 2 seconds (1.80).

**Considered.** Covering as fast as possible (what the home screen reads as it starts would then always be themed:
that is why its own views still go in at once); waiting for each app's process (the system doesn't say which process
a sandbox belongs to, apart from the home screen's).

**Costs.** An app that reads a picture in its first 2 seconds shows the console's own until it reads it again; rest
mode, not speed, is what decides when a change shows.

## An overlay in use is never forced off

**Context.** The home screen keeps its font and interface folders open while it runs. Forcing an overlay off under an
open file broke the system's views stacked on it ("Bad file descriptor"), and apps wouldn't start until a restart.

**Decision.** An overlay that is in use stays. The next theme goes straight on top; where nothing replaces it (No
theme), a copy of the system's own folder goes on top; it comes off once free. Where the next theme goes on, the
plugin tries once rather than waiting (1.77).

**Considered.** Forced unmounts; waiting until free (the home screen never lets go while it runs).

**Costs.** Layers can stack until the next restart; the overlay list allows a mount point more than once.

## The media work in the browser

**Context.** Themes need 4K BC7 textures, zstd frames, ATRAC9 audio and rebuilt resource files. No ready tool does
all of it on the console, and the plugin must stay small and safe.

**Decision.** The manager's page does the encoding (in a Web Worker for audio), with its own encoders written for
this project; the plugin only checks what arrives. Each encoder is cross-checked against a reference decoder in the
tests (LibAtrac9 and FFmpeg for ATRAC9, the real `zstd`).

**Considered.** Encoders in the plugin (more code running with the console's privileges, and slower than a
computer's browser); requiring desktop tools (a theme couldn't be made from a phone).

**Costs.** A large page (about 520 KB, sent compressed as 151 KB); encoding speed depends on the browser.

## One process, plain C, its own web server

**Context.** A payload runs with the console's privileges, in a sandboxed toolchain with a small C library.

**Decision.** One process with a few threads, written in C with no outside libraries; its own small HTTP server with a
fixed number of workers and a bounded queue; every answer closes its connection.

**Considered.** A general web server or framework (more code, more to go wrong, not built for the console);
keep-alive connections (a few slow clients could hold every worker).

**Costs.** Parsers, decoders and the server are this project's own, so they are fuzzed and tested; four workers
answer one request each at a time.

## Devices allowed with a code the console shows

**Context.** Until 1.80 anyone on the network could open the manager, change themes and read the log, and a web page
could reach it through DNS rebinding. The manager must still open from a phone with no setup, and on the console with
a controller.

**Decision.** Only addresses on the console's own network, and no internet names for the console. By default only
devices allowed once with a six-digit code the console shows in a notification; the device gets a random key (a
cookie the manager's own requests only carry), kept by the console as its hash. The console's own browser is always
allowed. Any device on the network, or only the console, can be chosen instead.

**Considered.** A password (one more thing to set up and forget, typed with a controller on the console, and the
same for every device); a code printed in the log or a file (anyone who can read the log over the network could
use it); HTTPS (the console has no certificate a browser trusts, and a self-signed one teaches people to click
through warnings).

**Costs.** The first visit from each device takes a code. The connection is plain HTTP: someone who can watch the
network's traffic could see a key, and forgetting the device is the remedy.

## Notifications as the system's own, said once

**Context.** The plugin's notifications were plain text, and many: "starting" then "ready" two seconds apart, and
after an apply "applied" and then one per thing to do (rest mode for icons, for the font, for the artwork; the
music), up to five on the TV for one press. etaHEN's show its icon beside two lines.

**Decision.** The system's notification service (`sceNotificationSend`), as etaHEN uses it: the plugin's icon, a
first line, a second line. One notification where there were several: "PS5 Themes is ready", "Theme applied" with
what to do on its second line, "PS5 Themes stopped" said once by the stop tool. Which ones appear is a setting;
failures always do. `libSceNotification` is linked at start like the other system libraries, and its function looked
up, so a firmware without it gets the plain notification instead.

**Considered.** Loading the library while running (loading a system module again from a spawned process can hang the
dynamic linker for every thread); keeping the plain notifications (the console's own and etaHEN's don't look like
that); an action button opening the manager on the notification (its deep link to a web page isn't one the console
is known to follow; not worth a broken button).

**Costs.** The plugin now needs `libSceNotification` to load at start, as the SDK's own notification sample does; the
icon is a file the service reads from the plugin's folder.

## Updating by hand-over

**Context.** etaHEN's Toolbox, the usual way to restart a plugin, is often gone after rest mode, and restarting the
console takes the theme off for a while.

**Decision.** A newer copy, started any way (the update folder, Payload Manager, etaHEN), asks the running one to
step aside; the running one finishes what it is doing and exits leaving every overlay in place; the new one carries on
with them. It then replaces the older copies in etaHEN's and Payload Manager's folders.

**Considered.** Asking for a restart; killing the old copy (mid-apply, its overlays and records would be half done).

**Costs.** A copy that never answers needs `ps5-theme-stop`; a file copied in slowly has to be told apart from a
whole one ([Challenges](challenges.md#an-update-copied-in-half)).

## Telling files apart by identity, not content

**Context.** Reading files to see whether they changed is slow on the console: a game's folder compared both ways at
every apply took 30 seconds.

**Decision.** Theme, stock and stage files are only ever replaced, never changed in place, so a file's identity
(inode, size, time) stands for its content. Stage folders are reused, checks remembered, and a game's folder checked
for an update, by identity; the content is read only when that changed.

**Considered.** Content hashes every time (correct, but the 30 seconds); trusting dates alone (installers date files
ahead of the clock).

**Costs.** A file changed in place, keeping its size and time, wouldn't be noticed; nothing the console or the plugin
does writes that way.

## A list of overlays that is appended, not synced

**Context.** The plugin must know which overlays are its own, after a crash or a hand-over too. Rewriting and syncing
the whole list after each of 217 overlays took seconds of every switch.

**Decision.** One line is added per overlay, without waiting for the disk. The list matters only while the console
runs: a crash leaves what was written to the kernel, and a power loss takes the overlays with it. A last line cut
short is ignored, and the list is written whole again before the next line (1.79).

**Considered.** Syncing each write (the seconds); keeping the list in memory only (a crash would leave overlays nobody
knows about, which in the home screen's sandbox keeps the console from waking).

**Costs.** None found on the console; the cut-short line has a test.

## Discard by swapping folders, under a lock of its own

**Context.** Discard puts back the theme as it was at its last save. The quickest whole replacement is renaming
folders, and for that moment the theme has no folder.

**Decision.** Discard swaps the folders holding a lock of its own to write; whatever finds a theme missing waits for
it and looks again; the status and short reads hold it to read. Uploads are received outside the theme and placed
under the themes lock, which is also the only place a theme's folders are made (1.77, 1.78).

**Considered.** Copying the saved files over the edited ones (slow, and a crash midway leaves a mix); one lock for
everything (a long upload would hold every read up).

**Costs.** Two locks with a fixed order, and a rule that no read takes the themes lock; both checked by tests and by
ThreadSanitizer.

## Two clocks

**Context.** The console sets its clock from the network after it starts, so the clock can jump.

**Decision.** Waits and intervals are timed on a clock that only goes forward. App restarts, for the crash-loop
check, are timed on the console's clock, so rest mode counts between them, and a gap below zero never counts. How long
a home screen was up, which tells a rest from a crash for that check (since 1.88), takes the longer of the two.

**Considered.** One clock for both (forward-only would count a rest as no time between restarts; the console's would
stall waits).

**Costs.** Two kinds of time in the code, each named for which it is.

## Screenshots for the docs, read-only and without Sony's pictures

**Context.** The docs need screenshots of the manager as it is used, which means a console's own themes and
pictures. The console's interface icons and artwork, and games' icons, are other people's and must never be in this
repository.

**Decision.** `tools/docshots.mjs` takes them from a console's manager in a headless browser, refusing every request
that would change something, and answers every picture of the console's, every game's, and every system tile's icon
styled from the console's own, with a striped stand-in.

**Considered.** Screenshots by hand (one forgotten picture is enough); mock data (the docs would show what the
manager doesn't do).

**Costs.** The stand-ins show where a picture would be, not the picture.
