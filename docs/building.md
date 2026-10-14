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
