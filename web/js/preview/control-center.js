"use strict";

/* ---- What the Control Center shows: the home screen behind it, the look's two pieces (the theme's, else the
   console's own), the console's icons, and the colours of the default palette, which it uses with every look. */

/* The row of icons, left to right: [icon, a notification not yet seen], null where the profile picture goes. */
const CC_ROW = [["iconid_home"], ["iconid_switcher"], ["iconid_notification", true], ["iconid_game_base"], ["iconid_music"],
  ["iconid_download"], ["iconid_sound_speaking"], ["iconid_mic_mute"], ["iconid_game_controller_1"], null, ["iconid_power"]];
const CC_COLORS = { backdrop: 0xff020408, card: 0xa3020408 };   /* 13.42's own, 0xAARRGGBB */

/* The backdrop and card colours with the theme's edits. The interface file is read only when the theme changes it
   or the editor has it already: the console's own colours are known. */
async function ccColors() {
  const path = rcoPaths()[0];
  if (!path || (!state.files.has(path) && !(state.rco && state.rco.get(path)))) return CC_COLORS;
  try {
    const r = await rcoStock(path);
    const pal = r && rcoColors(r).find(p => p.id === "color_default");
    if (!pal) return CC_COLORS;
    const edit = (await rcoChanges(path)).get(`${pal.f.el}\t${pal.f.attr}`);
    const values = new Map(edit ? [...new TextDecoder().decode(edit).matchAll(COLOR_PAIR)].map(m => [m[2], Number(m[3])]) : pal.values);
    const named = (name, fb) => values.has(name) ? values.get(name) >>> 0 : fb;
    return { backdrop: named("base-mat.overlay.enabled", CC_COLORS.backdrop), card: named("radio-button.overlay.fill.enabled", CC_COLORS.card) };
  } catch (e) { return CC_COLORS; }
}

/* One of the look's pieces with its size on the console, or null when there is none. The console's own comes
   decoded at a quarter of its size. */
async function ccPiece(id, files, look, which) {
  const path = apPath(look, which), mine = files.has(path);
  if (!mine && !(state.assets || []).some(a => a.path === path)) return null;
  const img = await homeFile(id, files, path);
  if (!img) return null;
  const k = mine ? 1 : 4;
  return { img, w: (img.naturalWidth || img.width) * k, h: (img.naturalHeight || img.height) * k };
}

/* Everything ccScreenDraw needs, for theme `id` (its files: `files`), the home screen behind with tile `focus` of
   row `tab` selected, and the pieces of look `look`. */
async function ccScene(id, files, { tab, focus, look }) {
  const [home, top, bottom, icons, capture, colors] = await Promise.all([
    homeScene(id, files, { tab, focus, look }),
    ccPiece(id, files, look, "top"), ccPiece(id, files, look, "bottom"),
    Promise.all(CC_ROW.map(async x => x && { img: await homePic(uiIconUrl(x[0])), dot: !!x[1] })),
    homePic(uiIconUrl("iconid_capture_still")),
    ccColors(),
  ]);
  const shot = new Date(Date.now() - 14 * 60000);   /* the card's screenshot, taken a little earlier */
  return { home, top, bottom, icons, capture, colors, clock: hsClock(), shot: { day: `${shot.getDate()}/${shot.getMonth() + 1}`, time: hsClock(shot) } };
}
