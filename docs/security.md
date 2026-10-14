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
