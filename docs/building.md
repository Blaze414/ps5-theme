# Building from source

Building the plugin and the tools, and where everything is in the source. Part of the [PS5 Themes](../README.md#documentation) documentation.

```bash
./build.sh
```

Produces `build/ps5-theme.plugin`, `build/ps5-theme.elf` and
`build/ps5-theme-stop.elf` in Docker, with
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.43 and LLVM 18
on Ubuntu 24.04. The `.plugin` file is the ELF with etaHEN's plugin header:
`etaHEN_PLUGIN`, the title ID `THEM00001` and the version
(`tools/make_plugin.py`).

The manager's page (`web/index.html`) and its ATRAC9 module are built into
the plugin twice: as they are, and compressed with `gzip -9 -n` into
`build-meta/` as the build runs (since 1.77), for browsers that take gzip.
`build-meta/` is made by the build and never committed.

## What the ELF says about itself

Since 1.73 the ELF (and so the `.plugin`, which wraps it) carries its own
description, for any loader or tool to read without running it: two
sections the console never loads, so they take no memory.

- `.ps5meta.json`: UTF-8 JSON, schema 1: `id` (`ps5-theme`), `name` (PS5
  Themes), `version`, `title_id`, `author` (Blaze414), `description`,
  `homepage`, `license` (GPL-3.0-or-later) and `icon` (the section below).
- `.ps5meta.icon`: the logo, the Themes tile's 512 × 512 PNG.

`tools/payload_meta.py` writes the JSON from the Makefile's version as the
plugin builds (`src/meta.c` places both), and the build checks the finished
ELF has both, unchanged and not loaded into memory, or fails. To read them:

```bash
llvm-objcopy -O binary --only-section=.ps5meta.json build/ps5-theme.elf meta.json
```

## PS5 Payload Manager's repository

PS5 Payload Manager shows a payload's logo, name, version, author and
description only for one installed from a repository; for one installed by
hand it shows the file's name. `tools/payload_meta.py repo` writes the two
files of a repository for a release: `repository.json` (the payload, its
version, SHA-256 and the release's download address) and `icons.json` (the
logo at 128 × 128, `assets/icon-128.png`):

```bash
python3 tools/payload_meta.py repo build/ps5-theme.elf THEM00001 1.80 pldmgr
```

They are made again for each release, from the very file attached to it
(Payload Manager checks every download against the SHA-256). Added as a
source in Payload Manager (**Settings → Manage Sources**), the repository
offers each new release as an update.


## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) runs on every push to main and
every pull request:

- the [end-to-end tests](testing.md), in a privileged container with real
  mounts and again with mounts emulated;
- the PS5 build, its three files and their SHA-256 sums attached to the run;
- `tools/check_repo.py`: every local link and heading link in the README and
  the docs resolves, and the version is the same in the Makefile, the
  plugin, the README and the tests.

The same checks run by hand: `tests/host/run.sh`, `tests/host/run.sh
emulated`, `./build.sh` and `python3 tools/check_repo.py`.


## Project layout

| Path | What |
|---|---|
| `src/main.c` | The theme engine: stock copies, staging, overlays, checks, progress, re-apply at start |
| `src/web.c` | Web server and JSON API: uploads, settings, Save/Discard, previews, status |
| `src/update.c` | One running copy at a time, finding a newer plugin file and handing over to it |
| `src/running.c` | Finding the running copy, and asking it over its own web port (shared with the stop tool) |
| `src/autostyle.c` | The icon styler: games installed later styled in the theme's icon style |
| `src/meta.c` | The ELF's own description and logo, in sections the console never loads |
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
| `tools/docshots.mjs` | The manager's screenshots in `docs/screenshots/`, taken from a console's manager in a headless Chromium, read-only (every request that would change something is refused), with the console's own pictures, games' icons and the system tiles' styled icons (made from the console's own) shown as stand-ins |
| `tools/payload_meta.py` | The ELF's description, its check, and the PS5 Payload Manager repository files |
| `tools/` | Table generator, icon generator, plugin packer, repository checks (`check_repo.py`) |
| `.github/workflows/ci.yml` | The tests, the build and the repository checks on every push |
| `tests/host/` | End-to-end tests and the fake console (`rco_fixture.py` builds resource files) |
| `tests/atrac9/`, `tests/zstd/` | Codec cross-checks |
| `tests/fuzz/` | Fuzzing the parsers |
| `examples/sample-theme/` | A minimal theme |
