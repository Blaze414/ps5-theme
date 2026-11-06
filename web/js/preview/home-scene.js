"use strict";

/* ---- What the home screen shows: the console's own rows (its app database), each title's name and icon, and the
   selected one's background, with the theme's pictures and font where it has them. All read as the page runs. */
let homeDb = null;   /* { at, rows }: read again after a minute, as games are played */
async function homeRows() {
  if (homeDb && Date.now() - homeDb.at < 60000) return homeDb.rows;
  let rows = null;
  try {
    const res = await fetch(`/api/app-db?v=${Date.now()}`);
    if (res.ok) rows = homeRowsOf(sqliteTable(new Uint8Array(await res.arrayBuffer()), "tbl_iconinfo_"));
  } catch (e) { /* a database this page can't read: the games the editor lists, in its order */ }
  if (!rows) rows = { games: ["NPXS40047", ...titleGroups().slice(0, 12).map(g => g.title), "NPXS40071"], media: ["NPXS40056", "NPXS40139"], apps: new Set(), names: new Map() };
  homeDb = { at: Date.now(), rows };
  return rows;
}
/* The console's pictures by title, read once per drawing: tile icons, games' pictures, tiles' own backgrounds. */
function homeIndex() {
  const bgs = new Map();
  let def = "";
  for (const a of state.assets || []) {
    const m = /\/vsh_asset\/bg_(NPXS\d+)\.dds(\.zst)?$/.exec(a.path);
    if (m) bgs.set(m[1], a.path);
    else if (!def && areaOf(a.path) === "Home: default background") def = a.path;
  }
  return { tiles: new Map(tileGroups().map(g => [g.title, g])), titles: new Map(titleGroups().map(g => [g.title, g])), bgs, def };
}
/* One of a title's pictures (icon0, pic0): the theme's copy if it has one, else the console's, PNG first. */
function homeTitlePicture(ix, T, stem, files) {
  const set = ((ix.titles.get(T) || {}).sets || []).find(x => x.stem === stem);
  if (!set) return stem === "icon0" ? `/user/appmeta/${T}/icon0.png` : "";
  const png = a => /\.png$/i.test(a.path), own = set.files.filter(a => files.has(a.path));
  return (own.find(png) || own[0] || set.files.find(a => a.path === `/user/appmeta/${T}/${stem}.png`) || set.files.find(png) || set.files[0]).path;
}
function homeIconPath(ix, T, files) {
  const g = ix.tiles.get(T);
  return g ? (g.icons.find(a => files.has(a.path)) || g.icons[0]).path : homeTitlePicture(ix, T, "icon0", files);
}
/* The background a title shows when selected: a system tile's own, a game's or app's picture, else the default one. */
function homeBgPath(ix, T, files) {
  return (/^NPXS/.test(T) ? ix.bgs.get(T) : homeTitlePicture(ix, T, "pic0", files)) || ix.def;
}
const homePics = new LimitedCache(80);   /* url -> a promise of something to draw (a picture or a canvas), or null */
function homePic(url, dds = false) {
  if (!url) return Promise.resolve(null);
  return homePics.once(url, async () => {
    try {
      if (!dds) return await loadImage(url);
      const res = await fetch(url);
      if (!res.ok) return null;
      let b = new Uint8Array(await res.arrayBuffer());
      if (b[0] === 0x28 && b[1] === 0xb5 && b[2] === 0x2f && b[3] === 0xfd) b = unzstdStored(b);
      return await ddsCanvas(b, true);
    } catch (e) { return null; }
  });
}
/* A file as the theme has it, or the console's own. */
async function homeFile(id, files, path) {
  if (!path) return null;
  const dds = /\.dds(\.zst)?$/i.test(path);
  if (files.has(path)) return homePic(`/api/theme/file?${q({ id, path })}&v=${stamp}`, dds);
  if (dds) return homePic(await consoleTexture(path));
  return homePic(`/api/asset?${q({ path })}`);
}
/* A background: the theme's own one through the picture made from it (quick to read), else the file itself. */
async function homeBg(id, files, path) {
  if (path && files.has(path) && backgroundFile([...files]) === path) {
    const made = await homePic(`/api/theme/preview?${q({ id, kind: "background" })}&v=${stamp}`);
    if (made) return made;
  }
  return homeFile(id, files, path);
}
/* The theme's font as the home screen uses it, loaded once per change of it; else the console's. */
const homeFonts = new Map();   /* theme -> { key, face, family } */
async function homeFont(id, files) {
  const fonts = [...files].filter(p => /\.otf$/i.test(p) && (FONT_LATIN.test(baseName(p)) || FONT_HOME.test(baseName(p))))
    .sort((a, b) => (FONT_LATIN.test(baseName(a)) ? 0 : 1) - (FONT_LATIN.test(baseName(b)) ? 0 : 1));
  if (!fonts.length || typeof FontFace !== "function") return (await consoleFont()) || "";
  const key = `${fonts[0]}|${stamp}`, had = homeFonts.get(id);
  if (had && had.key === key) return had.family;
  try {
    const family = `hs-${id}-${Date.now()}`, face = new FontFace(family, `url("/api/theme/file?${q({ id, path: fonts[0] })}&v=${stamp}")`);
    await face.load();
    document.fonts.add(face);
    if (had) try { document.fonts.delete(had.face); } catch (e) { /* gone */ }
    homeFonts.set(id, { key, face, family });
    return family;
  } catch (e) { return (await consoleFont()) || ""; }
}
/* The Appearance theme a theme's pictures are for: the first whose home screen artwork it changes. */
const homeLookOf = files => ["ps", "ps2", "ps3", "ps4"].find(l => files.has(apPath(l, "homeui"))) || "default";
const hsClock = (d = new Date()) => `${d.getHours()}:${String(d.getMinutes()).padStart(2, "0")}`;
/* Where the card's picture looks: the tile whose background the theme changes, or a title showing the default
   background when that is the one it changes, else the game played last. */
function homeCardView(rows, files, ix) {
  const bg = backgroundFile([...files]), tile = bg && /bg_(NPXS\d+)\./.exec(bg);
  for (const tab of ["games", "media"]) {
    const i = tile ? rows[tab].indexOf(tile[1]) : -1;
    if (i >= 0) return { tab, focus: i };
  }
  if (bg && bg === ix.def) {
    for (const tab of ["games", "media"]) {
      const i = rows[tab].findIndex(T => homeBgPath(ix, T, files) === ix.def);
      if (i >= 0) return { tab, focus: i };
    }
  }
  return { tab: "games", focus: rows.games.length > 2 ? 1 : 0 };
}
/* Everything homeScreenDraw needs, for theme `id` (its files: `files`) with tile `focus` of row `tab` selected. */
async function homeScene(id, files, { tab, focus, look }) {
  await loadAssets();
  const rows = await homeRows(), ix = homeIndex(), list = rows[tab] || rows.games;
  focus = Math.max(0, Math.min(focus, list.length - 1));
  const T = list[focus];
  const name = x => rows.names.get(x) || (state.tileNames && state.tileNames.get(x)) || (state.titleNames && state.titleNames.get(x)) || x;
  const [tiles, bg, art, search, settings, font] = await Promise.all([
    Promise.all(list.map(async x => ({ id: x, name: name(x), img: await homeFile(id, files, homeIconPath(ix, x, files)) }))),
    homeBg(id, files, homeBgPath(ix, T, files)),
    look && look !== "default" ? homeFile(id, files, apPath(look, "homeui")) : null,
    homePic(uiIconUrl("iconid_search")), homePic(uiIconUrl("iconid_settings")),
    homeFont(id, files),
  ]);
  return { bg, art, tab, tabs: ["Games", "Media"], tiles, focus, clock: hsClock(), search, settings, font,
    hub: T && !/^NPXS/.test(T) ? { name: name(T), button: rows.apps.has(T) ? "Start" : "Play" } : null };
}
