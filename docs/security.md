# Security

Who can reach the plugin, what protects the system, and the fuzzing results. Part of the [PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Who can reach the plugin](#who-can-reach-the-plugin)
- [What protects the system](#what-protects-the-system)
- [Fuzzing results](#fuzzing-results)
- [Bugs found, and proof they're fixed](#bugs-found-and-proof-theyre-fixed)
- [What isn't covered](#what-isnt-covered)

## Who can reach the plugin

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

## What protects the system

- Every write goes through the write guard and stays inside
  `/data/ps5-theme/` and the tile's folder (see
  [Never writing the system](how-it-works.md#never-writing-the-system), including what it
  doesn't cover).
- Themes are read-only overlays held in memory, so a restart always brings
  back the stock console.
- Only media can be replaced, never code, and each file is checked against
  the system file it replaces (see
  [What the checks cover, and what they don't](how-it-works.md#what-the-checks-cover-and-what-they-dont)).

## Fuzzing results

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

## Bugs found, and proof they're fixed

Each fix has a host test, and each test was also run against the old code to
make sure it catches the bug.

| Bug | Found by | Old code | Fixed |
|---|---|---|---|
| **A WAV file whose check never ended.** A chunk size of `0xFFFFFFF7` made the step to the next chunk wrap to 0 in 32-bit arithmetic. | Fuzzing: the WAV target stopped making progress, spinning at full CPU on a 181-byte file. | The test's 48-byte upload got no answer (timed out after 30 s) and tied up one of the manager's 4 worker threads for good. Four such uploads would leave it unable to serve anyone. | Refused at once: "not a valid WAV file". |
| **A texture unpacked with no limit.** A 25-byte file claiming to unpack to 200 MB. | Review | Room for 200 MB was set aside before the file was found to be invalid ("not a valid zstd-compressed texture"). | Refused before unpacking: "larger than the system texture". The texture fuzz target ran about 17 times faster afterwards (93 to 1,577 inputs a second). |
| **A file left open when saving failed.** `fsync(fd) \|\| close(fd)` skipped the close when `fsync` failed. | Review | 25 failed saves raised the plugin's open files from 8 to 33, one leaked per failure. | No file left open (the test allows a margin of 3 for open connections), `theme.ini` intact, no temporary file left behind. |
| **The interface crashing on start with numbered pictures** (1.31's test). | Console log | ShellUI restarted 10 times in 90 s (new sandboxes `NPXS40087_001` to `_010`) and the console stayed in rest mode. | Not yet confirmed on the console. That copy's pictures are refused by the new check (on 13.42's real file: "its size or pixel format differs", and Sony's `<svg>` opening not kept), while the new test copies are accepted. A host test shows a theme taken down after 3 restarts; with the guard switched off it fails ("the theme was not taken down"). |
| **No plugin running after a restart.** An older copy started by etaHEN started a newer one, then kept the newer one waiting while it put the theme back. | Console log | 13:49:43 newer copy starts; 13:50:03 it gives up; 13:50:31 the old copy steps aside anyway. Nothing ran until it was started again at 14:14. The host test fails: "the old copy applied before handing over". | On the console with 1.30: the old copy started 1.30 at 15:41:58, finished putting the theme back at 15:42:45 and handed over at 15:42:46, and 1.30 kept running. |
| **Two uploads of the same file at once wrote into one temporary file.** Every upload was received into `<file>.part`, so a second upload of the same file truncated and overwrote the first one's half-received bytes. | Console log: four uploads of the interface file failed together ("500 could not store the file", 14 October 18:42:48) when several palette buttons were pressed while a save was still running; the manager showed "empty or unreadable file" and "not a resource file (.rco) of this kind", the checks having read the other upload's half-written file. | Two simultaneous uploads in the host test both failed: `[500, 500]`. | Each upload is received into a temporary file of its own. Six rounds of two simultaneous uploads all succeed, the file ends up as one of them whole, and no temporary file is left (1.59). |

Also fixed in 1.59 after a review of the whole plugin and manager page:

- **A file that didn't read back as sent stayed in the theme.** After moving an
  upload into place, the plugin reads it back; when that failed it removed the
  temporary name, which no longer existed, and left the bad file. It is now
  removed. (No test can force a read-back failure.)
- **Too-long requests were cut, not refused.** An address longer than the
  server's fields was cut silently, and a value longer than its field (a path
  over 511 characters) was cut to a shorter one, which is a different path.
  Both are refused now ("414: the request's address is too long", or an empty
  value that every check refuses). Host test: a 2,100-character address gets
  414, and 660-character paths that start with a real file's path aren't read
  as that file.
- **"The same picture as" failed on the console.** Keeping one original for a
  picture attached at several sizes used a hard link, which the console's
  `/data` doesn't have (its log: "hard links aren't available here"), and
  bypassed the write guard. It now links through the guard where it can and
  otherwise copies the file on the console, so the picture still crosses the
  network once. Host test: with hard links switched off, the second original
  is a copy, byte for byte.
- **The manager page's memory only grew.** It kept a parsed copy of the
  interface file (up to 40 MB) for every change made to a theme, and every
  texture it had made (up to 16 MB each). It keeps one copy per file and the
  last four textures now. (Checked in the page's code; no automated test.)
- **Failures nothing caught were silent.** An action failing where no code
  caught it left the button doing nothing. The page now says what went wrong.
- **Interface file changes could undo each other** (two palette buttons
  pressed while a save was running): they are queued in the page, each
  starting from the file as the one before left it, and the plugin no longer
  mixes the uploads either (above).
- **The refit could read another theme's picture** if you switched themes
  while it ran. It now reads from the theme it started with.
- **Refusals weren't in the log.** Checks the plugin refused (and other
  conflicts) now show in **Activity** as "web: refused (code): reason".

## What isn't covered

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
