# Backgrounds

How to change the home screen's backgrounds, what it needs, and how they are made. Part of the [PS5 Themes](../README.md#documentation) documentation.

**On this page:**

- [Step by step](#step-by-step)
- [Where a background is used](#where-a-background-is-used)
- [Changing it later](#changing-it-later)
- [What it needs](#what-it-needs)
- [If it doesn't show](#if-it-doesnt-show)
- [How it is made](#how-it-is-made)

One picture becomes the home screen's background: the default background,
the Game Library's, and the one shown while each of the console's own tiles
is selected. The manager makes it in the exact format and size of each of
the console's backgrounds; the console's own files are never written (the
plugin lays the theme over them, see [How it works](how-it-works.md)).

![The Background part: the box to drop a picture on, then where your background is used, each area with its switch](screenshots/background.png)

## Step by step

1. **Open the theme.** On the library, press **Edit** on its card, or **New
   theme** first. A computer or phone is the easiest place: the picture is
   made into the console's format in that browser, which is quicker there
   than in the console's own browser.
2. **Choose how the picture is fitted**, in **Name and picture** (**Pictures
   of another shape**, each choice drawn on your own picture once you have
   one): **Fill** crops its edges to fill the screen, **Fit** keeps all of it
   with bars at the sides or top, **Stretch** squeezes it. A 16:9 picture
   fits exactly either way.
3. **Open Background** in the list of parts, and under **Choose where your
   background is used**, switch on the areas you want it (see
   [below](#where-a-background-is-used)). The home screen's own backgrounds
   are on to begin with.
4. **Add the picture:** drop it on the box at the top of the part, or
   press **Choose file**. It's made for every background in the areas
   switched on (on 13.42, 15 of them), each at its own size, with the
   progress showing, and then "Background added and verified".
5. **The theme's card** in the library takes a picture made from the
   background, unless you chose your own under **Preview picture**.
6. **Save** (the bar at the bottom), then **Apply**. The home
   screen shows the new background within a few seconds; it can show the
   console's own look for a moment while it switches. A tile's background
   shows when that tile is selected.

## Where a background is used

Each row under **Choose where your background is used** is an area where a
background is shown, with a switch. On 13.42:

| Area | What it is | To begin with |
|---|---|---|
| **Home: default background** | Behind the home screen | On |
| **Home: Game Library** | Behind the Game Library | On |
| **Home: NPXS… tile** | Behind the home screen while that tile of the console's own is selected (13 of them: PlayStation Plus, PlayStation Store, Discover, Music Home, All Apps, Welcome and others) | On |
| **Image load error placeholder** | Shown when a picture can't be loaded | Off |
| **Welcome tab backgrounds** | The Welcome hub's own backgrounds, the ones chosen in its settings: short videos, each with a still picture shown while it loads and a small one to choose it by. A picture goes to the still ones, a video to the videos | Off |
| **Other areas** | Backgrounds inside other screens: payment promotions, the accessibility widget, gradients, the VR tutorial | Off |

- **Switching an area off** only changes where the next picture goes. A
  picture already attached there stays until you reset it.
- **The switches are kept with the theme** (since 1.62): the same on every
  device you edit it from, and in an export.
- **One tile only:** **Home screen tiles** has **Change background** for each
  of the console's tiles, with their names. **One game only:** see
  [Changing game and app icons](games-and-apps.md).

## Changing it later

- **A different picture:** drop it in again. It replaces the backgrounds in
  the areas switched on.
- **A different fit:** change **Pictures of another shape**. With **Keep
  original pictures** on (both in **Name and picture**; on to begin with), every background
  is made again from the picture you attached, with a progress bar, without
  attaching anything again.
- **Back to the console's backgrounds:** **Reset background** at the bottom
  of the section, or switch the section off.
- After a change, **Apply** the theme again.

## What it needs

- **Console:** a jailbroken PS5 on firmware 13.60 or below with this plugin.
  Tested on 13.42.
- **A picture** the browser can open: JPEG, PNG, WebP and so on. Any size: it
  is fitted to each background. For the sharpest result, use 3840 by 2160 (4K,
  16:9).
- **A picture for the home screen.** Its backgrounds are pictures on 13.42,
  so a video can't go there. A video goes only to backgrounds that are
  videos: the Welcome hub's own (switch on **Welcome tab backgrounds**). It
  goes in as it is, without converting, so use an MP4 like the console's;
  this hasn't been tried on a console yet. With no video background in the
  areas switched on, dropping a video says so.
- **A browser with canvas and `BigInt`** (current Chrome, Edge, Firefox or
  Safari), since the console doesn't make the textures itself.
- **Disk space on the console:** each home background takes about 8 MB in the
  theme (about 125 MB for all 15). With **Keep original pictures** on, your
  picture is kept too, once for each background it went to (the console's
  storage can't share one copy between files). Switch it off in a theme to
  save that space; the fit then applies only when you attach a picture.
- **The theme's firmware** must match the console (or be set to **Any
  firmware**), as for every theme.

## If it doesn't show

- **"No image backgrounds in the areas switched on":** switch on at least
  one area.
- **"The texture came out too unlike your picture":** the picture has more
  fine detail than the format can hold at that size (very fine noise, for
  example). Use a smoother picture.
- **The console's background after rest mode:** a screen that read its
  background before the theme was back in place can show the console's own
  until it reloads. **Apply again** brings it back at once.
- **Applied partly:** **Activity** says which file was skipped and why.

## How it is made

- **The files:** on 13.42 the home screen's backgrounds are 3840 by 2160
  textures in `/system_ex/vsh_asset` (`bg_hub_default`, `Library_BG_…` and
  `bg_NPXS…` for the tiles), in the BC7 format and compressed with zstd. No
  browser can write either, so the manager carries a BC7 encoder and a zstd
  writer of its own.
- **Made to match:** it fits your picture to each texture's exact size,
  encodes it, checks a sample of the result against your picture (and
  refuses a poor one), reuses the console's own texture header byte for
  byte, and wraps it the way the console's files are wrapped.
- **Checked on the console:** the plugin unpacks every texture with its own
  zstd decoder and compares its header and size with the console's before
  it's used.
- **More detail:** [Backgrounds: BC7 textures made in the
  browser](how-it-works.md#backgrounds-bc7-textures-made-in-the-browser) and
  [4K textures without a texture tool](challenges.md#4k-textures-without-a-texture-tool).
