# Theme format

What a theme folder and its export contain. Part of the [PS5 Themes](../README.md#documentation) documentation.

A theme is a folder, or a `.zip` of one:

```
my-theme/
  theme.ini
  music.wav          optional: the song, 48 kHz 16-bit stereo PCM
  music2.wav         optional: a playlist's other songs, music2.wav to music8.wav, the same format
  preview.jpg        optional: library picture made from the background
  picture.jpg        optional: library picture you chose (shown first)
  choices.json       optional: the editor's choices for this theme (see below)
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
music_tracks=3
music_between=crossfade
music_end_mode=loop-all
music_sequence=1,3,2
music3_start=12000
music3_end=80000
music2_start=0
music2_end=95000
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
- `music_tracks`, `music_sequence`: a playlist's number of songs and the order
  they play in, as the songs' file numbers (1 is `music.wav`, 2 is
  `music2.wav`, and so on, up to 8). Without them the theme has one song,
  `music.wav`.
- `music<N>_start`, `music<N>_end`: the part of `music<N>.wav` that plays,
  in ms; song 1's is `music_start` and `music_end`.
- `music_between`: how a playlist's songs join: `crossfade` (they overlap by
  `music_blend`), `fade` or `cut`; also where the playlist starts again.
- `music_end_mode`: `loop-all` (the playlist starts again from its first
  song) or `loop-last` (the last song keeps repeating `music_loop_start` to
  `music_loop_end`, or its whole part).
- In a playlist, `music_order` and `music_play_from` are not used.

`choices.json` keeps what you chose in the editor, so it is the same on every
device and goes with an export: how pictures are fitted (`fit`), each
Appearance piece's placement, padding and clock band (`pieces`, by the path of
the file), the background areas switched off (`bgOff`), sections switched on
with nothing in them yet (`sections`) and the playlist's song names (`songs`,
by song number). It is one JSON object of at most 64 KB, never applied to the
console; the editor reads only the values it knows, and a theme without it
uses the defaults.

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
| `/data/ps5-theme/update/` | Drop a newer `ps5-theme.elf` or `.plugin` here to update; it is removed once that version runs |
| `/data/ps5-theme/logs/` | Log and crash reports |
| `/data/ps5-theme/dump/` | The asset listing and mount table, for reference |
