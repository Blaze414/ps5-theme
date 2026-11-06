"use strict";

/* ================================================================ the editor's choices

   How pictures are fitted, each Appearance piece's placement, which
   background areas and sections are on, and the playlist's song names belong
   to the theme: the plugin keeps them in its choices.json (never applied to
   the console), so they are the same on every device and go with an export.
   What this browser kept for a theme before 1.62 moves there the first time
   the theme is opened here. */

const FITS = ["cover", "contain", "stretch"], PLACES = ["fill", "pad", "fit", "stretch"];

/* Only what the page understands, as the right types: choices.json can come from an imported zip. */
function cleanChoices(c) {
  const out = {};
  if (!c || typeof c !== "object" || Array.isArray(c)) return out;
  if (FITS.includes(c.fit)) out.fit = c.fit;
  if (c.sections && typeof c.sections === "object") {
    for (const a of ASPECTS) if (c.sections[a.id]) (out.sections = out.sections || {})[a.id] = 1;
  }
  if (Array.isArray(c.bgOff)) out.bgOff = c.bgOff.filter(x => typeof x === "string" && x.length <= 120).slice(0, 200);
  if (c.pieces && typeof c.pieces === "object" && !Array.isArray(c.pieces)) {
    for (const [path, p] of Object.entries(c.pieces).slice(0, 500)) {
      if (!path.startsWith("/") || path.length > 512 || !p || typeof p !== "object") continue;
      const piece = {};
      if (PLACES.includes(p.place)) piece.place = p.place;
      if (typeof p.pad === "number" && Number.isFinite(p.pad)) piece.pad = Math.max(0, Math.min(40, Math.round(p.pad)));
      if (typeof p.keepClear === "boolean") piece.keepClear = p.keepClear;
      if (Object.keys(piece).length) (out.pieces = out.pieces || {})[path] = piece;
    }
  }
  if (c.iconStyle && typeof c.iconStyle === "object" && !Array.isArray(c.iconStyle)) {
    const st = c.iconStyle, o = {};
    if (ICON_SHAPES.some(([v]) => v === st.shape)) o.shape = st.shape;
    if (typeof st.scale === "number" && Number.isFinite(st.scale)) o.scale = Math.max(40, Math.min(100, Math.round(st.scale)));
    if (["colour", "background", "picture"].includes(st.behind)) o.behind = st.behind;
    if (typeof st.colour === "string" && /^#[0-9a-f]{6}$/i.test(st.colour)) o.colour = st.colour.toLowerCase();
    if (typeof st.auto === "boolean") o.auto = st.auto;
    if (typeof st.ranAs === "string" && st.ranAs.length <= 80) o.ranAs = st.ranAs;
    if (typeof st.picAt === "number" && Number.isFinite(st.picAt)) o.picAt = st.picAt;
    if (typeof st.mine === "boolean") o.mine = st.mine;
    if (Array.isArray(st.titles)) o.titles = st.titles.filter(x => typeof x === "string" && /^[A-Za-z0-9_-]{1,40}$/.test(x)).slice(0, 4000);
    if (Array.isArray(st.hand)) o.hand = st.hand.filter(x => typeof x === "string" && /^[A-Z0-9]{9}$/.test(x)).slice(0, 4000);
    if (Array.isArray(st.tiles)) o.tiles = st.tiles.filter(x => typeof x === "string" && x in STYLE_TILES);   /* (the system tiles chosen) */
    if (Object.keys(o).length) out.iconStyle = o;
  }
  if (c.songs && typeof c.songs === "object" && !Array.isArray(c.songs)) {
    for (let s = 1; s <= MUSIC_MAX_TRACKS; s++) {
      const n = c.songs[s];
      if (typeof n === "string" && n.trim()) (out.songs = out.songs || {})[s] = n.trim().slice(0, 80);
    }
  }
  return out;
}

/* What this browser kept for a theme before 1.62. */
function legacyChoices(id) {
  const c = { pieces: {}, songs: {} };
  const fit = localGet("fit");
  if (fit) c.fit = fit;
  try { c.sections = JSON.parse(localGet(`aspects:${id}`) || "{}"); } catch (e) { /* none */ }
  try { const off = localGet(`bg-off:${id}`); if (off) c.bgOff = JSON.parse(off); } catch (e) { /* none */ }
  try {
    for (let i = 0; i < localStorage.length; i++) {
      const key = localStorage.key(i) || "", m = /^(keep|place|pad):([^:]+):(\/.+)$/.exec(key);
      if (!m || m[2] !== id) continue;
      const v = localStorage.getItem(key), p = c.pieces[m[3]] = c.pieces[m[3]] || {};
      if (m[1] === "keep") p.keepClear = v !== "0";
      else if (m[1] === "place") p.place = v;
      else p.pad = +v;
    }
  } catch (e) { /* no storage */ }
  for (let s = 1; s <= MUSIC_MAX_TRACKS; s++) { const n = localGet(`track-name:${id}:${s}`); if (n) c.songs[s] = n; }
  return cleanChoices(c);
}

let choicesWork = Promise.resolve();
/* Send a theme's choices, one write at a time. `quiet`: moved from this browser, not a change for Discard to undo. */
function putChoices(id, c, quiet = false) {
  const bytes = new TextEncoder().encode(JSON.stringify({ v: 1, ...c })), crc = hex8(crc32(bytes));
  const run = choicesWork.then(async () => {
    const r = await api("PUT", "/api/theme/choices", quiet ? { id, quiet: 1, crc } : { id, crc }, bytes);
    if (r.crc32 !== crc || r.size !== bytes.length) throw new Error("the console stored something different from what was sent");
  });
  choicesWork = run.catch(() => {});
  return run;
}

async function loadChoices(id) {
  let c = null, fresh = false;
  try {
    const res = await fetch(`/api/theme/choices?${q({ id })}&v=${Date.now()}`);
    if (res.ok) { const raw = await res.json(); fresh = !raw || !("v" in raw); c = cleanChoices(raw); }
  } catch (e) { /* not reachable or not readable: the defaults */ }
  if (c && fresh) {
    const old = legacyChoices(id);
    if (Object.keys(old).length) { c = old; putChoices(id, c, true).catch(() => {}); }
  }
  if (id !== state.editId) return;
  state.choices = c || {};
  state.choicesId = id;
  state.fit = state.choices.fit || "cover";
}

/* Change the open theme's choices: sent at once, and saved or discarded with the theme like any change.
   The Save bar shows at once (the plugin marks the theme changed as it keeps them); only a failure asks
   the console how things stand. */
function setChoice(change) {
  const id = state.editId, t = themeById(id);
  if (state.choicesId !== id) return Promise.resolve();
  change(state.choices);
  if (t && !t.unsaved) { t.unsaved = true; syncEditorHead(); }
  return putChoices(id, state.choices).catch(e => { toast(`This choice could not be kept: ${e.message}`, "err"); return refreshStatus(); });
}
const pieceChoice = (c, path) => ((c.pieces = c.pieces || {})[path] = c.pieces[path] || {});
const songName = slot => (state.choices.songs || {})[slot] || "";
function setSongNames(change) {
  return setChoice(c => { c.songs = c.songs || {}; change(c.songs); if (!Object.keys(c.songs).length) delete c.songs; });
}

function bgOff() {
  const saved = state.choices.bgOff;
  if (saved) return new Set(saved);
  /* Default: only the home screen's own backgrounds, when this firmware has them. */
  const areas = new Set(aspectAssets(aspectById("background")).map(a => areaOf(a.path)));
  const home = [...areas].some(a => a.startsWith("Home:"));
  return new Set(home ? [...areas].filter(a => !a.startsWith("Home:")) : []);
}
function setBgOff(set) { setChoice(c => { c.bgOff = [...set]; }); }
