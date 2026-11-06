"use strict";

/* ---- Icon style: every game's and app's icon in a shape, over a colour or a picture ----

   Made in this page from each title's own icon (the console's, never one already styled, so styling again never
   stacks), written as the icons are when you change one by hand. An icon you changed yourself is styled too (unless
   you say not to): your picture goes in the shape, and is kept with the style first (icon-style/own/<title>.png),
   so styling again starts from it and removing the style puts it back. The theme lists the titles its style made.
   Games installed later are styled when the theme is opened here (and by the plugin, autostyle.c); nothing is done
   while the console installs one, since a theme over a game's pictures keeps the console from writing them. */
const ICON_SHAPES = [["", "None"], ["circle", "Circle"], ["rounded", "Rounded square"], ["square", "Square"], ["diamond", "Diamond"], ["hexagon", "Hexagon"], ["triangle", "Triangle"]];
const ICON_STYLE_DEFAULT = { shape: "", scale: 72, behind: "colour", colour: "#0d1424", auto: true, mine: true,
  tiles: null, titles: [], hand: [] };   /* (tiles: the system tiles' IDs chosen; null: all of STYLE_TILE_SETS) */
const allStyleTiles = () => STYLE_TILE_SETS.flatMap(t => t.ids);
const iconStyle = () => {
  const st = { ...ICON_STYLE_DEFAULT, ...(state.choices.iconStyle || {}) };
  if (!Array.isArray(st.tiles)) st.tiles = st.tiles === false ? [] : allStyleTiles();
  return st;
};
function setIconStyle(change) {
  return setChoice(c => {
    const st = { ...ICON_STYLE_DEFAULT, ...(c.iconStyle || {}) };
    if (!Array.isArray(st.tiles)) st.tiles = st.tiles === false ? [] : allStyleTiles();
    change(st);
    c.iconStyle = st;
  });
}
function shapePath(x, shape, cx, cy, r) {
  x.beginPath();
  if (shape === "circle") x.arc(cx, cy, r, 0, Math.PI * 2);
  else if (shape === "rounded" || shape === "square") {
    const k = shape === "rounded" ? r * 0.44 : 0;
    x.roundRect ? x.roundRect(cx - r, cy - r, 2 * r, 2 * r, k) : x.rect(cx - r, cy - r, 2 * r, 2 * r);
  } else {
    const n = shape === "diamond" ? 4 : shape === "hexagon" ? 6 : 3, rot = shape === "hexagon" ? Math.PI / 6 : -Math.PI / 2;
    const rr = shape === "triangle" ? r * 1.12 : r, dy = shape === "triangle" ? r * 0.16 : 0;
    for (let i = 0; i < n; i++) {
      const a = rot + i * 2 * Math.PI / n;
      x[i ? "lineTo" : "moveTo"](cx + rr * Math.cos(a), cy + dy + rr * Math.sin(a));
    }
    x.closePath();
  }
}
/* One styled icon, w x h, from a title's own icon: what is behind, then the icon cut to the shape, with a soft shadow. */
function drawStyledIcon(x, w, h, icon, st, behind) {
  x.clearRect(0, 0, w, h);
  if (behind) {
    const sc = Math.max(w / behind.width, h / behind.height), bw = behind.width * sc, bh = behind.height * sc;
    x.drawImage(behind, (w - bw) / 2, (h - bh) / 2, bw, bh);
  } else { x.fillStyle = st.colour; x.fillRect(0, 0, w, h); }
  drawIconInShape(x, w, h, icon, st);
}
/* The title's icon cut to the style's shape, with its shadow, over whatever is drawn already. */
function drawIconInShape(x, w, h, icon, st) {
  if (!st.shape) { x.drawImage(icon, 0, 0, w, h); return; }
  const r = Math.min(w, h) / 2 * st.scale / 100, cx = w / 2, cy = h / 2;
  x.save();
  x.shadowColor = "rgba(0, 0, 0, .45)"; x.shadowBlur = r * 0.18; x.shadowOffsetY = r * 0.05;
  shapePath(x, st.shape, cx, cy, r); x.fillStyle = "#000"; x.fill();
  x.restore();
  x.save();
  shapePath(x, st.shape, cx, cy, r); x.clip();
  x.imageSmoothingQuality = "high";
  x.drawImage(icon, cx - r, cy - r, 2 * r, 2 * r);
  x.restore();
}
/* Pictures drawn again and again as the style's options change (a title's icon, what is behind): read once each.
   Their addresses change when they do (the stamp), so a new one is read. */
const imageCache = new LimitedCache(32);
function cachedImage(url) {
  return imageCache.once(url, () => loadImage(url));
}
/* What goes behind the icons: the theme's background (its 16:9 preview), your picture, or nothing (the colour). */
async function iconBehind(st) {
  if (st.behind === "colour") return null;
  const t = themeById(state.editId);
  try { return await cachedImage(`/api/theme/preview?${q({ id: state.editId, kind: st.behind === "picture" ? "iconbg" : "background" })}&v=${(t && t.coverTime) || 0}-${stamp}`); }
  catch (e) { return null; }
}
/* A title's own icon: the first PNG of its icon0 set, as the console has it. */
const titleIcon = g => { const set = g.sets.find(x => x.stem === "icon0"); return set && set.files.find(a => ext(a.path) === "png"); };
async function titleOwnIcon(g) {
  const a = titleIcon(g);
  return a ? cachedImage(`/api/asset?${q({ path: a.path })}`) : null;
}
/* The titles the plugin styled by itself, read from the theme (merged with the page's own list). */
let styleMadeByPlugin = { id: null, titles: [] };
async function loadPluginStyled(id = state.editId) {
  let made;
  try {
    const r = await api("GET", "/api/theme/icon-style", { id });
    made = { id, titles: r.made || [], auto: r.auto, template: r.template, styledList: r.styledList, own: r.own || [],
      settle: r.settle || 120, waiting: r.waiting || {} };
  } catch (e) { made = { id, titles: [], own: [], waiting: {} }; }
  if (id !== state.editId) return;
  styleMadeByPlugin = made;
  /* a title still installing: looked at again once it counts as installed */
  clearTimeout(followTimer);
  const secs = Object.values(styleMadeByPlugin.waiting);
  if (secs.length) followTimer = setTimeout(() => { state.followDue = "settled"; }, (Math.min(...secs) + 2) * 1000);
}
/* Titles still installing (something in their folders changed lately), with the seconds until they count as
   installed: none is styled until then, here or by the console, since a theme over a game's pictures keeps the
   console from writing them. */
const installingNow = () => styleMadeByPlugin.id === state.editId ? styleMadeByPlugin.waiting || {} : {};
/* The console's own tiles the home screen shows, styled with the games when the style says so, by
   their system IDs (the same in every language), each its own choice, in the home screen's groups. Their icons
   are a symbol on the console's own dark backing, which keeps it readable in the shape. Left out, as a home screen
   never shows them: the developer kits' apps (marked with a star: Debug Settings, Store Preview and the rest). A
   choice can cover more than one tile (the two Compilation tiles). The names are the console's own once read. */
const STYLE_TILE_SETS = [
  { group: "Games home", name: "Welcome", ids: ["NPXS40063"] },
  { group: "Games home", name: "PlayStation Store", ids: ["NPXS40047"] },
  { group: "Games home", name: "PlayStation Plus", ids: ["NPXS40037"] },
  { group: "Games home", name: "Game Library", ids: ["NPXS40071"] },
  { group: "Games home", name: "Media Gallery", ids: ["NPXS40075"] },
  { group: "Media home", name: "Discover", ids: ["NPXS40053"] },
  { group: "Media home", name: "Music Home", ids: ["NPXS40054"] },
  { group: "Media home", name: "All Apps", ids: ["NPXS40056"] },
  { group: "Media home", name: "Live TV", ids: ["NPXS40305"] },
  { group: "Media home", name: "App Library", ids: ["NPXS40139"] },
  { group: "Discs", name: "PS5 Disc", ids: ["NPXS40149"] },
  { group: "Discs", name: "PS4 Disc", ids: ["NPXS40150"] },
  { group: "Discs", name: "Compilation", ids: ["NPXS40145", "NPXS40205"] },
  { group: "Discs", name: "Unsupported Disc", ids: ["NPXS40144"] },
  { group: "Discs", name: "Unreadable Disc", ids: ["NPXS40148"] },
];
const STYLE_TILES = Object.fromEntries(STYLE_TILE_SETS.flatMap(t => t.ids.map(id => [id, t.name])));   /* ID -> name */
/* The choices this console has (a firmware without a tile leaves it out), each with only the IDs it has. */
function styleTileSets() {
  const present = new Set(tileGroups().map(g => g.title));
  return STYLE_TILE_SETS.map(t => ({ ...t, ids: t.ids.filter(id => present.has(id)) })).filter(t => t.ids.length);
}
const tileSetName = t => (state.tileNames && state.tileNames.get(t.ids[0])) || t.name;
/* Each as a title whose icon is in every file the tile's icon is (tileGroups): those you chose, or (`all`) the five
   this firmware has. */
function tileStyleGroups(all = false) {
  const chosen = new Set(iconStyle().tiles);
  return tileGroups().filter(g => g.title in STYLE_TILES && (all || chosen.has(g.title)))
    .map(g => ({ title: g.title, tile: true, sets: [{ stem: "icon0", files: g.icons }] }));
}
/* The folders the theme changes files in (each a folder the plugin overlays), and the most it applies at once
   (the status says). With `more`, files it would have too. */
function themeFolders(more = []) {
  const dirs = new Set([...state.files].map(p => p.slice(0, p.lastIndexOf("/"))));
  for (const p of more) dirs.add(p.slice(0, p.lastIndexOf("/")));
  return dirs.size;
}
const maxFolders = () => (state.status && state.status.maxFolders) || 64;
/* What the style is for: the installed games and apps, and the tiles you chose. */
const styleGroups = () => [...titleGroups(), ...tileStyleGroups()];
/* A styled icon written into all the files of a title's icon, or a tile's, each in the console's exact format. */
async function stylePicture(g, file) {
  if (!g.tile) return titlePicture(g.title, "icon0", file, true);
  const t = tileGroups().find(x => x.title === g.title);
  if (!t) return false;
  try {
    await tileIconFiles(t, async stock => {
      const f = pngFormat(stock);
      if (!f || !f.writable) throw new Error("this icon's format can't be written");
      return encodePng(await imageRgba(file, f.w, f.h), f, zlibDeflate);
    });
    return true;
  } catch (e) { return false; }
}
/* The titles whose icon is the style's: styled here, or by the plugin (unless you changed one by hand since). */
function styledTitles() {
  const st = iconStyle(), hand = new Set(st.hand);
  return new Set([...st.titles, ...(styleMadeByPlugin.id === state.editId ? styleMadeByPlugin.titles.filter(t => !hand.has(t)) : [])]);
}
/* The titles with your picture kept with the style (an icon you had changed, then styled). */
const ownPictures = () => new Set(styleMadeByPlugin.id === state.editId ? styleMadeByPlugin.own || [] : []);
const ownPictureUrl = title => `/api/theme/icon-style?${q({ id: state.editId, own: title })}&v=${stamp}`;
/* Icons you changed by hand, not the style's: with "yours too", the style puts your picture in the shape. */
function handChanged() {
  const made = styledTitles();
  return styleGroups().filter(g => {
    const set = g.sets.find(x => x.stem === "icon0");
    return set && titleIcon(g) && !made.has(g.title) && set.files.some(a => state.files.has(a.path));
  });
}

/* Your picture for a title's icon as the theme holds it, as a PNG of at most 1024 pixels a side: the original you
   attached if the theme kept it, else the theme's PNG, else its texture. */
async function themeIconPng(set) {
  const id = state.editId;
  let img = null;
  const kept = set.files.find(a => state.sources.has(a.path));
  if (kept) {
    const f = await sourceFile(kept.path).catch(() => null);
    if (f) { const u = URL.createObjectURL(f); try { img = await loadImage(u); } catch (e) { img = null; } finally { URL.revokeObjectURL(u); } }
  }
  const png = !img && set.files.find(a => ext(a.path) === "png" && state.files.has(a.path));
  if (png) { try { img = await loadImage(`/api/theme/file?${q({ id, path: png.path })}&v=${stamp}`); } catch (e) { img = null; } }
  const tex = !img && set.files.find(a => ext(a.path) === "dds" && state.files.has(a.path));
  if (tex) {
    const res = await fetch(`/api/theme/file?${q({ id, path: tex.path })}&v=${stamp}`);
    if (res.ok) { let bytes = new Uint8Array(await res.arrayBuffer()); if (bytes[0] === 0x28) bytes = unzstdStored(bytes); img = await ddsCanvas(bytes, true); }
  }
  if (!img) throw new Error("your picture for this icon can't be read from the theme");
  const w = img.naturalWidth || img.width, h = img.naturalHeight || img.height, k = Math.min(1, 1024 / Math.max(w, h));
  const c = document.createElement("canvas");
  c.width = Math.max(16, Math.round(w * k));
  c.height = Math.max(16, Math.round(h * k));
  c.getContext("2d").drawImage(img, 0, 0, c.width, c.height);
  return new Promise((done, fail) => c.toBlob(b => b ? done(b) : fail(new Error("could not make the picture")), "image/png"));
}

/* The picture a title's styled icon is made from. One the style made: your picture kept with it if it had one,
   else the console's icon. One you changed by hand, styled now: your picture as the theme holds it, kept with the
   style first, so styling again never puts a styled icon in the shape and removing the style puts it back. */
async function styleSource(g) {
  if (styledTitles().has(g.title)) return ownPictures().has(g.title) ? loadImage(ownPictureUrl(g.title)) : titleOwnIcon(g);
  const set = g.sets.find(x => x.stem === "icon0");
  if (!set.files.some(a => state.files.has(a.path))) return titleOwnIcon(g);
  const blob = await themeIconPng(set);
  await uploadVerified("/api/theme/icon-style", { id: state.editId, part: "own", title: g.title }, blob);
  if (styleMadeByPlugin.id === state.editId) styleMadeByPlugin.own = [...new Set([...(styleMadeByPlugin.own || []), g.title])];
  const u = URL.createObjectURL(blob);
  try { return await loadImage(u); } finally { URL.revokeObjectURL(u); }
}

/* You changed a title's icon by hand: it is yours now, not the style's, and a picture kept for it is let go. */
async function iconChangedByHand(title, back = false) {
  const st = iconStyle(), plugin = styleMadeByPlugin.id === state.editId ? styleMadeByPlugin.titles : [];
  /* (put back to the console's: still counted as the style's, so it isn't styled again behind your back) */
  if (!back && (st.titles.includes(title) || plugin.includes(title))) {
    await setIconStyle(x => { x.titles = x.titles.filter(t => t !== title); if (plugin.includes(title)) x.hand = [...new Set([...x.hand, title])]; });
  }
  if (ownPictures().has(title)) {
    await api("POST", "/api/theme/icon-style", { id: state.editId, forget: title }).catch(() => {});
    await loadPluginStyled();
  }
}

/* The style's template for the plugin, which styles titles installed later by itself (autostyle.c): the backdrop
   with the shape's shadow, the shape alone, and the titles installed now, which it leaves to you. */
async function keepStyleTemplate(st) {
  const id = state.editId, side = 512;
  notStyle.clear();
  const behind = await iconBehind(st);
  const base = document.createElement("canvas"), mask = document.createElement("canvas"), none = document.createElement("canvas");
  base.width = base.height = mask.width = mask.height = side;
  none.width = none.height = 1;
  drawStyledIcon(base.getContext("2d"), side, side, none, st, behind);
  const m = mask.getContext("2d"), r = side / 2 * st.scale / 100;
  shapePath(m, st.shape, side / 2, side / 2, r);
  m.fillStyle = "#fff"; m.fill();
  const png = c => new Promise(res => c.toBlob(res, "image/png"));
  await uploadVerified("/api/theme/icon-style", { id, part: "base" }, await png(base));
  await uploadVerified("/api/theme/icon-style", { id, part: "mask" }, await png(mask));
  /* always sent, even with no titles: without it the plugin can't tell a new title from one you had */
  const known = [...new Set(titleGroups().map(g => g.title).filter(t => /^[A-Z0-9]{9}$/.test(t)))];
  await uploadVerified("/api/theme/icon-style", { id, part: "known" }, new Blob([known.map(t => `${t}\n`).join("") || "\n"]));
  /* the titles this style made, installed or not: one installed again later gets the style as it is then */
  const styled = [...new Set(iconStyle().titles.filter(t => /^[A-Z0-9]{9}$/.test(t)))];
  await uploadVerified("/api/theme/icon-style", { id, part: "styled" }, new Blob([styled.map(t => `${t}\n`).join("") || "\n"]));
  await api("POST", "/api/theme/icon-style", { id, auto: st.auto ? 1 : 0, scale: st.scale });
}

/* Titles the style is for: with an icon, not still installing, and not changed by hand (one the style made counts
   as the style's). `onlyNew`: games and apps installed since, never the console's tiles (always there: styled when
   you style the icons, never by themselves). */
function styleTargets(onlyNew) {
  const st = iconStyle(), made = styledTitles(), busy = installingNow();
  return (onlyNew ? titleGroups() : styleGroups()).filter(g => {
    const set = g.sets.find(x => x.stem === "icon0");
    if (!set || !titleIcon(g) || g.title in busy) return false;
    const changed = set.files.some(a => state.files.has(a.path));
    return onlyNew ? !changed && !made.has(g.title) : !changed || made.has(g.title) || st.mine;
  });
}

function iconStyleHtml() {
  const st = iconStyle(), all = styleTargets(false).length, made = styledTitles().size, fresh = st.shape ? styleTargets(true).length : 0;
  const hand = handChanged().length, own = ownPictures().size;
  const byPlugin = styleMadeByPlugin.id === state.editId ? styleMadeByPlugin.titles.length : 0;
  /* the tiles chosen, once the games are styled: those not styled yet (and not changed by hand) */
  const madeNow = styledTitles(), changedNow = g => g.sets[0].files.some(a => state.files.has(a.path));
  const tilesChosen = tileStyleGroups(), tileSets = styleTileSets();
  const tilesPending = st.shape && madeNow.size ? tilesChosen.filter(g => !madeNow.has(g.title) && !changedNow(g)).length : 0;
  const tileOn = t => t.ids.every(id => st.tiles.includes(id));
  tileNamesEnsure();
  const gamesTargets = all - styleTargets(false).filter(g => g.tile).length, tilesTargets = all - gamesTargets;
  /* still installing: new ones get the style once installed; the style's own, or yours, being updated are left
     as they are until then */
  const busy = installingNow(), madeSet = styledTitles();
  const held = st.shape ? titleGroups().filter(g => g.title in busy && titleIcon(g)) : [];
  const isNew = g => !madeSet.has(g.title) && !g.sets.find(x => x.stem === "icon0").files.some(a => state.files.has(a.path));
  const coming = held.filter(isNew), updating = held.filter(g => !isNew(g));
  const mins = n => `${Math.max(1, Math.ceil(n / 60))} minute${Math.ceil(n / 60) > 1 ? "s" : ""}`;
  const installingNote = [
    !coming.length ? "" : coming.length === 1
      ? `${titleName(coming[0].title)} is still installing: ${st.auto ? `its icon gets the style in about ${mins(busy[coming[0].title])}` : "style it once the install has finished"}.`
      : `${coming.length} games and apps are still installing: ${st.auto ? `each gets the style ${mins(styleMadeByPlugin.settle || 120)} after its install finishes` : "style them once their installs have finished"}.`,
    !updating.length ? "" : `${namesOf(updating.map(g => g.title))} ${updating.length === 1 ? "is" : "are"} installing or updating: left as ${updating.length === 1 ? "it is" : "they are"} until then.`,
  ].filter(Boolean).join(" ");
  const t = themeById(state.editId);
  const chip = (v, label, on, off = false) => `<button type="button" class="btn sm chip" data-act="istyle-behind" data-v="${v}" aria-pressed="${on}"${off ? " disabled" : ""}>${esc(label)}</button>`;
  const hasBg = !!(t && aspectReplaced(aspectById("background")));   /* (offered always: without one, said how to get it) */
  return `<section class="panel icon-style">
    <div class="form-title"><h3 class="sub-head" style="margin:0">Icon style</h3><span class="note">Games', apps' and system tiles' icons in one shape, over a colour or a picture.</span></div>
    <div class="opts" role="radiogroup" aria-label="Shape">${ICON_SHAPES.map(([v, name]) =>
      `<button type="button" class="opt" role="radio" aria-checked="${st.shape === v}" data-act="istyle-shape" data-v="${v}">
        <canvas data-istyle="${v}" width="112" height="112"></canvas><b>${esc(name)}</b></button>`).join("")}</div>
    <p class="note istyle-busy" id="istyle-busy" role="status" aria-live="polite"></p>
    ${st.shape ? `<div class="istyle-row">
      <label class="field">Size of the icon in the shape: <span id="istyle-scale-label">${st.scale}%</span>
        <input type="range" min="40" max="100" step="2" value="${st.scale}" id="istyle-scale"></label>
      <div class="field"><span>Behind it</span><div class="pv-tabs" style="flex-wrap:wrap">${chip("colour", "A colour", st.behind === "colour")}${
        chip("background", "The theme's background", st.behind === "background" && hasBg, !hasBg)}${chip("picture", "A picture of yours", st.behind === "picture")}</div>
        ${hasBg ? "" : `<span class="note">The theme's background: give this theme one in <b>Background</b> first.</span>`}
        ${st.behind === "colour" ? `<input type="color" id="istyle-colour" value="${esc(st.colour)}" aria-label="Colour behind the icons">` : ""}
        ${st.behind === "picture" ? `<div class="pv-tabs" style="margin-top:8px"><button class="btn sm" type="button" data-act="istyle-adjust">Adjust picture</button>
          <button class="btn sm ghost" type="button" data-act="istyle-picture">Choose another</button></div>` : ""}</div>
    </div>
    <div class="istyle-groups">
      <fieldset class="istyle-group"><legend>Games and apps <span class="count">${titleGroups().filter(g => titleIcon(g)).length}</span></legend>
        <label class="note check"><input type="checkbox" id="istyle-auto"${st.auto ? " checked" : ""}> Style ones installed later${help("While this theme is applied, the console styles a new game or app on its own about 2 minutes after its install finishes, applies the theme again and says so, even with a game open. A game styled before and installed again gets the style as it is then. Ones you changed by hand stay as they are.")}</label>
        ${byPlugin ? `<p class="note">${plural(byPlugin, "icon")} styled by the console on its own after ${byPlugin === 1 ? "its game was" : "their games were"} installed.</p>` : ""}
        ${installingNote ? `<p class="note">${esc(installingNote)}</p>` : ""}
      </fieldset>
      ${tileSets.length ? `<fieldset class="istyle-group"><legend>System tiles <span class="count">${tileSets.filter(tileOn).length} of ${tileSets.length}</span></legend>
        ${[...new Set(tileSets.map(t => t.group))].map(grp => `<div class="istyle-cat"><span>${esc(grp === "Discs" ? "Discs, shown while one is in" : grp)}</span>
          <div class="opts istyle-tiles" role="group" aria-label="${esc(grp)}">${tileSets.filter(t => t.group === grp).map(t =>
            `<button type="button" class="opt" aria-pressed="${tileOn(t)}" data-act="istyle-tile" data-v="${esc(t.ids.join(","))}">
              <canvas data-istyle-tile="${esc(t.ids[0])}" data-on="${tileOn(t)}" width="144" height="144"></canvas><b>${esc(tileSetName(t))}</b></button>`).join("")}</div></div>`).join("")}
        <p class="note">${tilesPending ? `${plural(tilesPending, "tile")} get${tilesPending === 1 ? "s" : ""} the style the next time you style the icons. ` : ""}Each is drawn as it will look: the style on the ones chosen, as now on the others.${help("Each chosen tile's icon goes in the shape, made from the console's own icon (one you changed by hand: as for games). Choosing one off puts it back as it was. Left out: the developer kits' apps, which a home screen doesn't show. The home row shows the tiles after rest mode.")}</p>
      </fieldset>` : ""}
    </div>
    ${hand ? `<label class="note check"><input type="checkbox" id="istyle-mine"${st.mine ? " checked" : ""}> The ${plural(hand, "icon")} you changed by hand too: your picture goes in the shape${help("Your picture is kept with the style first, so styling again starts from it, never from an icon already styled, and Remove the style puts it back. Unticked, the icons you changed stay as they are.")}</label>` : ""}
    <div class="toolbar">
      <button class="btn primary" type="button" data-act="istyle-run"${all ? "" : " disabled"}${tilesTargets ? ` aria-describedby="istyle-what"` : ""}>${made && !fresh ? `Style ${plural(all, "icon")} again` : `Style ${plural(all, "icon")}`}</button>${styling ? `<span class="note">Styling…</span>` : ""}
      ${tilesTargets ? `<span class="note" id="istyle-what">${plural(gamesTargets, "game or app", "games and apps")}, ${plural(tilesTargets, "system tile")}</span>` : ""}
      ${made ? `<button class="btn ghost danger" type="button" data-act="istyle-remove">Remove the style (${made})</button>` : ""}
      <span class="note">${made && st.ranAs && st.ranAs !== styleSig(st) ? "Your icons have the earlier style until you style them again. " : ""}${fresh && made ? `${plural(fresh, "game or app", "games and apps")} installed since.` : ""}${hand && !st.mine ? " Icons you changed by hand are kept." : ""}${own ? ` Remove the style puts your own ${own === 1 ? "picture" : "pictures"} back.` : ""}</span>
    </div>` : made ? `<div class="toolbar"><button class="btn ghost danger" type="button" data-act="istyle-remove">Remove the style (${made})</button>
      <span class="note">${plural(made, "icon")} still ${made === 1 ? "has" : "have"} the earlier style.</span></div>` : ""}
  </section>`;
}
/* The shape choices, drawn on one of your games' own icons with what is behind. */
async function iconStyleShow() {
  const cs = [...document.querySelectorAll("canvas[data-istyle]")];
  if (!cs.length) return;
  const g = titleGroups().find(x => titleIcon(x));
  let icon = null, behind = null;
  const st = iconStyle();
  /* what is being read, said when it takes a moment (the first time; then each is kept in the page) */
  const what = st.behind === "background" ? "Reading the theme's background to draw the shapes on…"
    : st.behind === "picture" ? "Reading your picture to draw the shapes on…" : "Reading one of your games' icons to draw the shapes with…";
  const slow = setTimeout(() => {
    const say = $("#istyle-busy"), box = $(".icon-style .opts");
    if (say) say.textContent = what;
    if (box) box.setAttribute("aria-busy", "true");
  }, 200);
  try { [icon, behind] = await Promise.all([g ? titleOwnIcon(g) : null, iconBehind(st)]); } catch (e) { /* drawn without */ }
  clearTimeout(slow);
  const say = $("#istyle-busy"), box = $(".icon-style .opts");
  if (say) say.textContent = st.behind !== "colour" && !behind ? (st.behind === "background" ? "The theme's background can't be read here: the shapes show without it." : "Your picture can't be read here: the shapes show without it.") : "";
  if (box) box.removeAttribute("aria-busy");
  for (const c of cs) {
    const x = c.getContext("2d");
    if (!icon) { x.fillStyle = "#1a2131"; x.fillRect(0, 0, c.width, c.height); continue; }
    drawStyledIcon(x, c.width, c.height, icon, { ...st, shape: c.dataset.istyle }, behind);
  }
  await tileStylesShow(st, behind);
}
/* The system tiles as they will look: a chosen one in the style, from the picture styling will start from (your
   picture kept with the style, or one you changed by hand if the style takes yours, else the console's icon); one
   not chosen as it is now. */
async function tileStylesShow(st, behind) {
  const made = styledTitles(), own = ownPictures(), id = state.editId;
  for (const c of document.querySelectorAll("canvas[data-istyle-tile]")) {
    const t = c.dataset.istyleTile, g = tileGroups().find(x => x.title === t);
    const stock = g && g.icons.find(a => ext(a.path) === "png"), mine = g && g.icons.find(a => ext(a.path) === "png" && state.files.has(a.path));
    /* (one you changed by hand stays as it is unless the style takes yours) */
    const on = c.dataset.on === "true" && !!st.shape && !(mine && !made.has(t) && !st.mine);
    let url = stock ? `/api/asset?${q({ path: stock.path })}` : null;
    if (on && made.has(t) && own.has(t)) url = ownPictureUrl(t);
    else if (mine && !made.has(t) && (!on || st.mine)) url = `/api/theme/file?${q({ id, path: mine.path })}&v=${stamp}`;
    let img = null;
    try { img = url ? await cachedImage(url) : null; } catch (e) { img = null; }
    if (!c.isConnected) continue;
    const x = c.getContext("2d");
    x.clearRect(0, 0, c.width, c.height);
    x.fillStyle = "#24262d";   /* the home row's tile: under a see-through icon as the console draws it */
    x.fillRect(0, 0, c.width, c.height);
    if (!img) continue;
    if (on) drawStyledIcon(x, c.width, c.height, img, st, behind);
    else x.drawImage(img, 0, 0, c.width, c.height);
  }
}
/* What the icons were last styled as: when the shape, size or what is behind differs, the part says so. */
const styleSig = st => `${st.shape}|${st.scale}|${st.behind}|${st.colour}|${st.behind === "picture" ? st.picAt || 0 : 0}`;
let styling = false, styleStop = false;
/* The console says what a styling run here is doing (the plugin words it: style_note), so whoever is at the TV
   knows: as it starts, at most every 8 seconds as it goes, and as it ends. Never waited for. */
let styleNotedAt = 0;
function styleNote(note, params) {
  if (note === "step" && Date.now() - styleNotedAt < 8000) return;
  styleNotedAt = Date.now();
  api("POST", "/api/theme/icon-style", { id: state.editId, note, ...params }).catch(() => {});
}

/* Style the icons; `note`, said at the end in place of "apply the theme". The titles styled. */
async function styleIcons(onlyNew, note = null) {
  if (styling) return [];
  const st = iconStyle();
  if (!st.shape) return [];
  const targets = styleTargets(onlyNew);
  if (!targets.length) return [];
  /* never a theme the plugin can't apply: each icon's files are in a folder of their own the theme then changes */
  const folders = themeFolders(targets.flatMap(g => g.sets.find(x => x.stem === "icon0").files.map(a => a.path)));
  if (folders > maxFolders()) {
    toast(`Styling these would make the theme change files in ${folders} folders, more than the ${maxFolders()} the plugin applies at once. ` +
      `Style fewer: choose some system tiles off, or take some pictures out of the theme.`, "err");
    return [];
  }
  styling = true;
  styleStop = false;
  const p = progressToast(`Styling ${plural(targets.length, "icon")}…`);
  /* a long run can be stopped between two titles: those done stay styled */
  const stop = document.createElement("button");
  stop.type = "button"; stop.className = "btn sm ghost stop-run"; stop.textContent = "Stop styling"; stop.style.marginTop = "8px";
  stop.addEventListener("click", () => { styleStop = true; stop.disabled = true; stop.textContent = "Stopping after this one…"; });
  p.el.append(stop);
  const made = new Set(st.titles), styledNow = [];
  let done = 0, failed = 0;
  const auto = onlyNew ? 1 : 0, total = targets.length;
  styleNote("start", { total, auto, ...(total === 1 && !targets[0].tile ? { title: targets[0].title } : {}) });
  try {
    const behind = await iconBehind(st);
    for (const g of targets) {
      if (styleStop) break;
      p.set(`Styling icons: ${titleName(g.title)} (${done + 1} of ${targets.length})…`, done / targets.length, (done + 1) / targets.length);
      if (done) styleNote("step", { n: done + 1, total });
      try {
        const icon = await styleSource(g);
        const c = document.createElement("canvas");
        c.width = icon.naturalWidth; c.height = icon.naturalHeight;
        drawStyledIcon(c.getContext("2d"), c.width, c.height, icon, st, behind);
        const blob = await new Promise(r => c.toBlob(r, "image/png"));
        if (await stylePicture(g, new File([blob], `${g.title}-icon.png`, { type: "image/png" }))) { made.add(g.title); styledNow.push(g.title); }
        else failed++;
      } catch (e) { failed++; }
      done++;
    }
  } finally {
    styling = false;
    styleNote(styleStop ? "stopped" : "done", { n: done, total, failed, auto });
    await setIconStyle(x => { x.titles = [...made]; x.hand = x.hand.filter(t => !made.has(t)); if (!styleStop) x.ranAs = styleSig(st); });
    /* the plugin carries on from here for games installed later */
    try { await keepStyleTemplate(iconStyle()); await loadPluginStyled(); }
    catch (e) { toast(`The style for games installed later could not be kept: ${e.message}`, "err"); }
    p.done(`${plural(done - failed, "icon")} styled${failed ? `, ${failed} could not be` : ""}${styleStop ? ` (stopped; ${plural(targets.length - done, "icon")} left as ${targets.length - done === 1 ? "it was" : "they were"})` : ""}. ${note || "Apply the theme to see them: the Game Library at once, the home row after rest mode."}`, failed ? "" : "ok");
    stamp = Date.now();
    await refreshStatus();
    renderAspect(aspectById("titles"));
    renderAspect(aspectById("tiles"));
  }
  return styledNow;
}
