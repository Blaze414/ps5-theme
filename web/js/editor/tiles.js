"use strict";

/* ---------------------------------------------------------------- home screen tiles */

/* The console's own tiles: title ID -> their icon files, with names from
   each app's param.json (read once). */
function tileGroups() {
  const groups = new Map();
  for (const a of aspectAssets(aspectById("tiles"))) {
    const t = tileTitle(a.path);
    if (!groups.has(t)) groups.set(t, { title: t, icons: [] });
    groups.get(t).icons.push(a);
  }
  const bgs = new Map((state.assets || []).filter(a => /\/vsh_asset\/bg_NPXS\d+\.dds(\.zst)?$/.test(a.path))
    .map(a => [/bg_(NPXS\d+)/.exec(a.path)[1], a]));
  return [...groups.values()].sort((a, b) => a.title.localeCompare(b.title))
    .map((g, i) => ({ ...g, n: i + 1, bg: bgs.get(g.title) || null }));
}

let tileNamesLoading = null;
function tileNamesEnsure() {
  state.tileNames = state.tileNames || new Map();
  const missing = tileGroups().filter(g => !state.tileNames.has(g.title));
  if (!missing.length || tileNamesLoading) return;
  tileNamesLoading = (async () => {
    for (const g of missing) {
      let name = "";
      for (const a of g.icons) {
        try {
          const res = await fetch(`/api/asset?${q({ path: a.path.replace(/icon0\.png$/, "param.json") })}`);
          if (!res.ok) continue;
          const pj = await res.json(), loc = pj.localizedParameters || {};
          const own = loc[loc.defaultLanguage] || loc["en-US"] || Object.values(loc).find(v => v && v.titleName) || {};
          name = own.titleName || "";
          if (name) break;
        } catch (e) { /* no param.json beside this copy */ }
      }
      state.tileNames.set(g.title, name);
    }
  })().finally(() => { tileNamesLoading = null; renderAspect(aspectById("tiles")); renderAspect(aspectById("background")); renderAspect(aspectById("titles")); });
}

/* A tile's icon from your image (or its number, for the test), each copy in
   exactly the format of the console's. */
async function tileIconFiles(g, make) {
  for (const a of g.icons) {
    const res = await fetch(`/api/asset?${q({ path: a.path })}`);
    if (!res.ok) throw new Error(`cannot read ${a.path} from the console`);
    await uploadVerified("/api/theme/file", { id: state.editId, path: a.path }, new Blob([await make(new Uint8Array(await res.arrayBuffer()))]));
    state.files.add(a.path);
  }
}

async function tileIcon(title, file) {
  const g = tileGroups().find(x => x.title === title), name = (state.tileNames && state.tileNames.get(title)) || title;
  const p = progressToast(`Adding the ${name} icon…`);
  try {
    await tileIconFiles(g, async stock => {
      const f = pngFormat(stock);
      if (!f || !f.writable) throw new Error("this icon's format can't be written");
      return encodePng(await imageRgba(file, f.w, f.h), f, zlibDeflate);
    });
    p.done(`${name} icon replaced and verified${g.icons.length > 1 ? ` (${plural(g.icons.length, "copy", "copies")})` : ""}. Apply the theme to use it.`, "ok");
    if (title in STYLE_TILES) await iconChangedByHand(title);   /* yours now, not the icon style's */
  } catch (e) { p.done(`${name}: ${e.message}`, "err"); }
  stamp = Date.now();
  await refreshStatus();
  renderAspect(aspectById("tiles"));
}

async function numberTileIcons() {
  const groups = tileGroups(), p = progressToast("Numbering the tile icons…");
  try {
    for (const [i, g] of groups.entries()) {
      p.set(`Numbering tile ${g.n} of ${groups.length}…`, i / groups.length, (i + 1) / groups.length);
      await tileIconFiles(g, async stock => {
        const out = await numberPicture(g.n, stock, zlibDeflate);
        if (!out) throw new Error(`tile ${g.n}'s icon format can't be written`);
        return out;
      });
    }
    p.done(`${plural(groups.length, "tile icon")} numbered and verified. Apply the theme, then look at the home screen.`, "ok");
    for (const g of groups) if (g.title in STYLE_TILES) await iconChangedByHand(g.title);
  } catch (e) { p.done(`Numbering failed: ${e.message}`, "err"); }
  stamp = Date.now();
  await refreshStatus();
  renderAspect(aspectById("tiles"));
}

function tilesBody(asp) {
  const groups = tileGroups();
  if (!groups.length) return `<p class="note">This firmware has no tile icons that can be themed.</p>`;
  setTimeout(tileNamesEnsure);
  const styledNow = styledTitles();
  const rows = groups.map(g => {
    const rep = g.icons.some(a => state.files.has(a.path)), bgRep = g.bg && state.files.has(g.bg.path);
    const inStyle = rep && styledNow.has(g.title);   /* (its icon the style's, set in Games and apps) */
    const name = state.tileNames && state.tileNames.has(g.title) ? state.tileNames.get(g.title) || "(no name)" : "Reading…";
    return `<div class="row-item${rep || bgRep ? " replaced" : ""}">
      <div class="info"><div class="file">${g.n} · ${esc(name)}${inStyle ? ` <span class="tag on">Icon style</span>` : rep ? ` <span class="tag on">Icon changed</span>` : ""}${bgRep ? ` <span class="tag on">Background changed</span>` : ""}</div>
        <div class="dir">${esc(g.title)}${g.icons.length > 1 ? ` · ${plural(g.icons.length, "copy", "copies")} of its icon` : ""}${g.bg ? " · has its own background" : ""}</div></div>
      <div class="strip">${thumbFor(g.icons.find(a => state.files.has(a.path)) || g.icons[0])}</div>
      <div class="acts">
        <button class="btn sm" data-act="tile-icon" data-title="${esc(g.title)}">Change icon</button>
        ${g.bg ? `<button class="btn sm" data-act="tile-bg" data-title="${esc(g.title)}">Change background</button>` : ""}
        ${rep || bgRep ? `<button class="btn sm ghost danger" data-act="tile-reset" data-title="${esc(g.title)}">Reset</button>` : ""}
      </div></div>`;
  }).join("");
  return `<div class="toolbar"><button class="btn sm" data-act="tiles-number">Number tile icons</button>
      <span class="note">A test: each icon becomes the number before its name, to tell the tiles apart on the console.</span></div>
    <div class="rows">${rows}</div>`;
}
