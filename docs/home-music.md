# Home music

How to give the home screen your own music, and the console your own wake-up sound: what they need, and how they are made. Part of the [PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Step by step](#step-by-step)
- [Changing it later](#changing-it-later)
- [What it needs](#what-it-needs)
- [When you hear it](#when-you-hear-it)
- [If it doesn't play](#if-it-doesnt-play)
- [The wake-up sound](#the-wake-up-sound)
- [How it is made and stored](#how-it-is-made-and-stored)

The home screen plays the console's own music. A theme can replace it with
any song: the manager converts the song to the console's own format in your
browser, and the console plays it with its own player, looping the way its
own music does. Only the home screen's music changes; games and apps keep
theirs. The console's other sounds (its start-up sounds and the set-up
music) are under **Sounds** in the same editor.

![The home music section](screenshots/home-music.png)

## Step by step

1. **Open the theme.** On the library, press **Edit** on its card, or
   **New theme** first. A computer or phone is the easiest place: the song is
   converted in that browser, which is quicker there than in the console's
   own browser.
2. **Add the song.** Switch on **Home music**, then drop the file on it or
   press **Choose file**. Any audio file the browser can play works: MP3,
   M4A (AAC), OGG, FLAC, WAV and more. The music editor opens.
3. **Choose what plays.**

   ![The music editor](screenshots/music-editor.png)

   - **Music part:** drag the white handles on the waveform, move a selected
     handle with the arrow keys (the controller's D-pad works too), or type
     the times.
   - **Play order:** repeat the music part (you can choose where it begins:
     the green marker); play a loop part first and then repeat the music
     part (to open on the chorus, for example); let the two take turns; or
     play the music part once and then repeat the loop part, like an intro.
   - **How the joints sound:** a crossfade of 1, 2 or 4 seconds, a fade out
     and in, or a straight cut.
   - **The summary line** says what will play, for example "Plays 0:30.0 to
     1:20.0 once, then repeats 0:05.0 to 1:20.0".
   - **Play** plays it as the console will loop it; **Hear where it loops**
     and **Hear the change of part** play the seconds around each joint.
4. **Press Use this.** The manager converts the music and checks the result,
   showing its progress, and then says "Home music set and verified" with its
   length. **Cancel** leaves everything as it was.
5. **Set the loudness** if you like: from -12 to +12 dB against the console's
   own music (the starting point matches it). Changing it converts the music
   again.
6. **Save** (the bar at the bottom), then **Apply to console**.
7. **Hear it:** highlight a game or app on the home screen that plays its own
   music, let it play, then go back to the home screen. See [When you hear
   it](#when-you-hear-it).

To change only the music, make a theme with nothing but **Home music**:
everything else stays as the console's.

## Changing it later

- **Choose part** opens the editor again with the whole song. The theme keeps
  the song, not only the part you chose, so the part, the order, the joints
  and the loudness can all be changed without adding the song again.
- **Replace** puts a different song in. **Remove** (or switching the section
  off) puts the console's own music back in this theme.
- After a change, **Apply** the theme again, then hear it as in step 7.

## What it needs

- **Console:** a jailbroken PS5 on firmware 13.60 or below with this plugin.
  Tested on 13.42.
- **A browser that can play the file:** current Chrome, Edge, Firefox or
  Safari. The converting runs in the browser, not on the console.
- **Length:** the theme keeps up to about 11 minutes of a song; of a longer
  one, the first 11 minutes are kept, and the manager says so. What plays,
  with its parts and loop, can be up to about twice as long as the console's
  own music: on 13.42 a little under 8 minutes. As you get close, the summary
  line says "In all …; the console allows up to …", and the editor keeps the
  parts within it.
- **Disk space on the console:** the song is kept as a WAV file, about
  11.5 MB a minute, and the home music the console plays takes about 2.3 MB
  a minute.
- **The theme's firmware** must match the console (or be set to **Any
  firmware**), as for every theme.

## When you hear it

- **Not the moment it's applied.** The console keeps playing the music it
  already has loaded. Highlight a game or app that plays its own music, let
  it play, then go back to the home screen: the home screen then starts its
  music again, and it's the theme's. The result of the apply, the section's
  note and a notification on the console say so too.
- **After a restart too.** The home screen loads its music before the
  jailbreak and the plugin start, so do the same once after a restart.
- **It can start part-way through.** The console keeps its place in the
  music: after a game it carries on where it stopped, and with new music it
  carries on from about where the old music had got to. That is how the
  console's own player works, so the first time, the theme's music may not
  start at its beginning.

## If it doesn't play

- **Still the console's music:** do the step above (another app's music,
  then back to the home screen). Check that the apply said **applied**, not
  **applied partly**; **Activity** says what was skipped and why.
- **Encode again** in the section: home music made with versions 1.10 to
  1.12 doesn't pass the console's checks (the console played it as ticks or
  silence). **Encode again** makes it again from the song the theme keeps.
- **Convert** in the section: music added with a version before 1.10, which
  the plugin played itself. **Convert** makes it the home music.
- **Too quiet or too loud:** change **Loudness**.
- **On firmware other than 13.42:** not tested. The plugin checks every music
  file against the console's own and skips one that doesn't match, so the
  console's music plays instead.

## The wake-up sound

The console plays a short sound as it wakes from rest mode
(`sfx_warmboot.at9`, 5.4 seconds on 13.42). A theme can replace it, and it
has been heard on a PS5 with 13.42: the plugin keeps running through rest
mode, and the console reads the sound as it wakes.

1. In the theme's editor, switch on **Sounds** and press **Attach** on
   `sfx_warmboot.at9`.
2. Choose a short sound. It can be up to about twice as long as the
   console's (on 13.42, about 10 seconds); a longer one is cut, fading out.
   Its loudness is matched to the console's own sound.
3. **Save**, then **Apply to console**.
4. Put the console in rest mode and wake it.

The other sounds in that list:

| File | When it plays | Can a theme change it? |
|---|---|---|
| `sfx_warmboot.at9` | Waking from rest mode | **Yes**, heard on the console |
| `sfx_coldboot.at9` | Switching the console on | No: it plays before the jailbreak and the plugin start |
| `sfx_initialboot.at9` | Most likely the first start, during set-up | No: set-up comes before a jailbreak |
| `sfx_transition.at9` | Not known yet | Not tried |
| `bgm_onboarding.at9`, `bgm_onboarding_vr.at9` | Most likely the music during set-up (and VR set-up) | Not tried |

## How it is made and stored

- **The file:** the home screen plays `/system_ex/vsh_asset/bgm_home.at9`. On
  13.42 it is ATRAC9, 48 kHz 5.1, 300 kbps, with a loop. Browsers can't write
  ATRAC9, so the manager carries an encoder of its own. It writes exactly the
  console's format, puts your music on the front left and right channels,
  matches the loudness of the console's music, lays out the loop as the
  console's own music does, and decodes the result again to compare it with
  your music before sending it.
- **The checks:** the plugin compares the file with the console's own (the
  format, the coding features, the padding Sony's decoder reads) before it is
  used, and a theme's file that fails is skipped when applying.
- **In the theme:** the song as `music.wav` (48 kHz, 16-bit) with the part
  and settings in `theme.ini`, and the home music file under `files/`. See
  [Theme format](theme-format.md).
- **More detail:** [Home music: an ATRAC9 encoder in the
  browser](how-it-works.md#home-music-an-atrac9-encoder-in-the-browser), and
  the problems solved on the way: [music no one could
  hear](challenges.md#music-no-one-could-hear), [no ATRAC9
  encoder](challenges.md#no-atrac9-encoder), [the music that
  ticked](challenges.md#the-music-that-ticked) and [music that starts
  part-way through](challenges.md#music-that-starts-part-way-through).
