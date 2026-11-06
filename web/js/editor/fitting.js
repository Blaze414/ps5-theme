"use strict";

/* ---- Fitting options, each drawn on your own picture in the exact shape of what it replaces ----

   The same arithmetic as the encoder (imageForAsset, textureForAsset and textureDraw), at a small size, so a choice
   can be judged before it is made. With no picture of yours kept yet, a sample stands in, and says so. */
const FITS_SHOWN = [["cover", "Fill", "Crops the edges"], ["contain", "Fit", "All of it, with bars"], ["stretch", "Stretch", "Squeezed to the shape"]];
const PLACES_SHOWN = [["fill", "Fill", "Cropped to the piece"], ["pad", "With padding", "Clear at the screen's edge"], ["fit", "Fit", "All of it"], ["stretch", "Stretch", "Squeezed to the piece"]];

function fitOptionsHtml() {
  return `<div class="field fit-field"><span>Pictures of another shape${help("How a picture that isn't the shape of what it replaces is fitted. Changing it remakes this theme's pictures from the ones you attached. Appearance pieces have their own placement.")}</span>
    <div class="opts" role="radiogroup" aria-label="Pictures of another shape">${FITS_SHOWN.map(([v, name, what]) =>
      `<button type="button" class="opt" role="radio" aria-checked="${state.fit === v}" data-act="fit-pick" data-v="${v}">
        <canvas data-opt="fit" data-v="${v}" width="176" height="99"></canvas><b>${esc(name)}</b><small>${esc(what)}</small></button>`).join("")}</div>
    <span class="note" id="fit-sample"></span></div>`;
}

function placeOptionsHtml(path) {
  const pl = placementFor(path);
  return `<div class="opts place" role="radiogroup" aria-label="Placement of ${esc(appearanceName(path))}">${PLACES_SHOWN
    .filter(([v]) => v !== "pad" || pl.hasEdge).map(([v, name, what]) =>
      `<button type="button" class="opt" role="radio" aria-checked="${pl.mode === v}" data-act="place-pick" data-path="${esc(path)}" data-v="${v}">
        <canvas data-opt="place" data-path="${esc(path)}" data-v="${v}" width="176" height="60"></canvas><b>${esc(name)}</b><small>${esc(what)}</small></button>`).join("")}</div>`;
}

const optPictures = new LimitedCache(6);   /* "<theme>|<path>|<stamp>" -> the kept picture, decoded */
async function keptPicture(path) {
  const key = `${state.editId}|${path}|${stamp}`;
  if (optPictures.has(key)) return optPictures.get(key);
  const res = await fetch(`/api/theme/source?${q({ id: state.editId, path })}&v=${stamp}`);
  if (!res.ok) throw new Error("no picture kept");
  const url = URL.createObjectURL(await res.blob());
  let img;
  try { img = await loadImage(url); } finally { URL.revokeObjectURL(url); }
  return optPictures.set(key, img);
}
const targetSizes = new Map();
/* The size of the file a picture replaces: a texture's from its header, an image's from the console's own. */
async function targetSize(path) {
  if (targetSizes.has(path)) return targetSizes.get(path);
  let size = null;
  const a = mixAsset(path);
  if (a && a.kind === "texture") {
    const info = await api("GET", "/api/asset/texture", { path });
    size = { w: info.width, h: info.height };
  } else {
    const img = await loadImage(`/api/asset?${q({ path })}`);
    size = { w: img.naturalWidth, h: img.naturalHeight };
  }
  targetSizes.set(path, size);
  return size;
}
/* A stand-in picture for a theme with none of yours kept: tall, so each option shows its difference. */
let samplePicture = null;
function sampleImage() {
  if (samplePicture) return samplePicture;
  const c = document.createElement("canvas");
  c.width = 600; c.height = 800;
  const x = c.getContext("2d"), g = x.createLinearGradient(0, 0, 0, 800);
  g.addColorStop(0, "#2a5bd7"); g.addColorStop(1, "#c3426f");
  x.fillStyle = g; x.fillRect(0, 0, 600, 800);
  x.fillStyle = "rgba(255,255,255,.9)";
  x.beginPath(); x.arc(300, 330, 150, 0, Math.PI * 2); x.fill();
  x.fillStyle = "rgba(0,0,0,.35)"; x.fillRect(0, 640, 600, 160);
  return (samplePicture = c);
}
function checker(ctx, w, h) {
  ctx.fillStyle = "#1a2131"; ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = "#232c40";
  for (let y = 0; y < h; y += 8) for (let x = (y / 8) % 2 ? 8 : 0; x < w; x += 16) ctx.fillRect(x, y, 8, 8);
}

/* Draw every option shown on the page. */
async function optionsShow() {
  const id = state.editId;
  const fits = [...document.querySelectorAll('canvas[data-opt="fit"]')];
  if (fits.length) {
    /* your first kept picture that the theme-wide fit applies to, in the shape it replaces */
    const path = [...state.sources].find(p => state.files.has(p) && !APPEARANCE_RE.test(p) && mixAsset(p));
    let img = null, size = { w: 1920, h: 1080 };
    try { if (path) [img, size] = await Promise.all([keptPicture(path), targetSize(path)]); } catch (e) { img = null; }
    if (state.editId !== id) return;
    const note = $("#fit-sample");
    if (note) note.textContent = img ? `Shown on your picture for ${mixElementOf(MIX_PARTS.find(p => p.id === mixPartOf(path)) || {}, path)[1] || baseName(path)}.`
      : "Shown on a sample picture until you attach one of yours.";
    const pic = img || sampleImage(), iw = img ? img.naturalWidth : pic.width, ih = img ? img.naturalHeight : pic.height;
    for (const c of fits) {
      const cw = c.width, ch = Math.max(24, Math.round(cw * size.h / size.w));
      c.height = Math.min(ch, 132);
      const x = c.getContext("2d"), w = c.width, h = c.height, fit = c.dataset.v;
      x.fillStyle = "#000"; x.fillRect(0, 0, w, h);
      let dw = w, dh = h;
      if (fit !== "stretch") { const sc = (fit === "cover" ? Math.max : Math.min)(w / iw, h / ih); dw = iw * sc; dh = ih * sc; }
      x.imageSmoothingQuality = "high";
      x.drawImage(pic, (w - dw) / 2, (h - dh) / 2, dw, dh);
    }
  }
  for (const c of document.querySelectorAll('canvas[data-opt="place"]')) {
    const path = c.dataset.path;
    let img, size;
    try { [img, size] = await Promise.all([keptPicture(path), targetSize(path)]); } catch (e) { continue; }
    if (state.editId !== id || !c.isConnected) return;
    const k = c.width / size.w;
    c.height = Math.max(24, Math.round(size.h * k));
    const x = c.getContext("2d"), pl = placementFor(path), place = placementParams(c.dataset.v, pl.pad), keep = keepClearFor(path);
    checker(x, c.width, c.height);   /* what the picture leaves bare is see-through on the console */
    const d = textureDraw(path, place.pad, place.fit, size.w, size.h, img.naturalWidth, img.naturalHeight, keep);
    x.save(); x.scale(k, k);
    x.beginPath(); x.rect(d.box.x, d.box.y, d.box.w, d.box.h); x.clip();
    x.imageSmoothingQuality = "high";
    x.drawImage(img, d.x, d.y, d.dw, d.dh);
    x.restore();
    const band = textureAnchor(path).band;
    if (band && keep) {   /* where the clock and icons are drawn: left clear */
      x.strokeStyle = "rgba(255,255,255,.55)"; x.setLineDash([3, 3]);
      x.beginPath(); x.moveTo(0, band * c.height + .5); x.lineTo(c.width, band * c.height + .5); x.stroke();
      x.setLineDash([]); x.fillStyle = "rgba(255,255,255,.85)"; x.font = `${Math.max(8, Math.round(band * c.height * .55))}px system-ui`;
      x.textAlign = "right"; x.textBaseline = "middle"; x.fillText("12:00", c.width - 6, band * c.height / 2);
    }
  }
}

/* Previous and Next: the parts either side of this one in the list. */
function stepsHtml(id) {
  const order = sectionOrder(), i = order.indexOf(id);
  const step = (to, dir) => to ? `<a class="step ${dir}" href="${sectionHref(to)}"><span class="note">${dir === "prev" ? "Previous" : "Next"}</span>${esc(sectionName(to))}</a>` : `<span></span>`;
  return step(order[i - 1], "prev") + step(order[i + 1], "next");
}

/* When a part's change shows on the console, where it isn't as soon as the theme is applied. */
const SHOWS_WHEN = {
  fonts: ["After rest mode", "The home screen reads its fonts as it starts: apply, then put the console in rest mode and wake it."],
  appearance: ["After rest mode", "Shows once that look is chosen in Settings > Appearance and Sound, after rest mode."],
  interface: ["After rest mode", "The interface reads these pictures as it starts: apply, then put the console in rest mode and wake it."],
  tiles: ["Home row after rest mode", "The Game Library shows a new icon at once; the home row after rest mode."],
  titles: ["Home row after rest mode", "The Game Library shows a new picture at once; the home row after rest mode."],
  music: ["After a game's music", "The home screen starts its music again only after another app's music has played: highlight a game with music, let it play, then go back."],
};

/* For a theme with nothing in it yet: the three steps from an empty theme to seeing it on the console. */
function startHereHtml(t) {
  if (t.files || t.music) return "";
  const to = (id, label) => `<a href="${sectionHref(id)}">${esc(label)}</a>`;
  return `<section class="panel start">
      <h2>Start here</h2>
      <ol>
        <li>Choose what to change from the list: ${to("background", "Background")}, ${to("tiles", "Tiles")}, ${to("music", "Home music")}, ${to("sounds", "Sounds")} and more.</li>
        <li>Press <b>Apply</b>. The home screen changes in a few seconds.</li>
        <li>Fonts, Appearance artwork, interface icons and the home row's icons show after rest mode: put the console in rest mode and wake it.</li>
      </ol>
    </section>`;
}

function themePaneHtml(t) {
  const fwAny = t.fw === "any";
  return `${startHereHtml(t)}
    <section class="panel builder-head">
      <h2 class="pane-title">Name and picture</h2>
      <label class="field"><span>Name</span><input type="text" id="theme-name" maxlength="60" value="${esc(t.name)}"></label>
      <label class="field"><span>Firmware${help("A theme is applied only on a console with its firmware. With Any firmware it is applied anywhere, and each file that no longer matches that console's is skipped.")}</span>
        <select id="theme-fw">
          <option value="${esc(state.status.fw)}" ${!fwAny ? "selected" : ""}>This console (${esc(state.status.fw)})</option>
          <option value="any" ${fwAny ? "selected" : ""}>Any firmware</option>
        </select></label>
      ${fitOptionsHtml()}
      <div class="field" id="originals-field">${originalsField()}</div>
    </section>
    <section class="panel cover-panel" id="cover-panel">${coverPanel(t)}</section>
    <p class="note">Code and libraries${state.protectedCount ? ` (${plural(state.protectedCount, "system file")})` : ""} are never changed. Every file is checked when you add it and when the theme is applied.</p>`;
}

function renderAspect(asp) {
  const el = $(`#aspect-${asp.id}`);
  if (!el) { syncNav(asp.id); return; }
  const replaced = aspectReplaced(asp);
  const status = sectionStatus(asp.id);
  const confirming = state.confirming === asp.id;
  const canReset = asp.mode !== "music" && replaced > 0;
  el.innerHTML = `
    <div class="aspect-head">
      <div class="text"><h2>${esc(asp.title)}</h2><div class="sub">${esc(asp.sub)}</div></div>
      ${SHOWS_WHEN[asp.id] ? `<span class="tag" title="${esc(SHOWS_WHEN[asp.id][1])}">${esc(SHOWS_WHEN[asp.id][0])}</span>` : ""}
      <span class="tag${UNCHANGED.has(status) ? "" : " on"}">${esc(status)}</span>
      ${confirming ? `<div class="confirm">Put back the console's own ${esc(asp.title.toLowerCase())}? ${plural(replaced, "file")} will be removed from this theme.
        <button class="btn sm danger armed" data-act="confirm-off" data-aspect="${asp.id}">Reset</button>
        <button class="btn sm ghost" data-act="cancel-off" data-aspect="${asp.id}">Keep</button></div>`
      : canReset ? `<button class="btn sm ghost danger" data-act="reset-all" data-aspect="${asp.id}">Reset all</button>` : ""}
    </div>
    <div class="aspect-body">${aspectBody(asp)}</div>`;
  paintLoads(el);
  syncNav(asp.id);
  if (asp.mode === "music" && state.trim) setupTrim();
}

/* A font of this theme, loaded so the page can draw with it (the list's "Aa", the Fonts part's samples): its record
   { family, ready, failed }, loading it the first time. With no path, the font the home screen uses most. */
const fontFaces = new Map();
function fontRec(path = null) {
  const a = path ? { path } : aspectAssets(aspectById("fonts")).filter(x => state.files.has(x.path))
    .sort((x, y) => fontRank(x) - fontRank(y))[0];
  if (!a || typeof FontFace !== "function") return null;
  const key = `${state.editId}|${a.path}|${stamp}`;
  if (fontFaces.has(key)) return fontFaces.get(key);
  const rec = { family: `theme-font-${fontFaces.size + 1}`, ready: false, failed: false };
  fontFaces.set(key, rec);
  const redraw = () => { syncNav("fonts"); if ($("#aspect-fonts")) renderAspect(aspectById("fonts")); };
  new FontFace(rec.family, `url("/api/theme/file?${q({ id: state.editId, path: a.path })}&v=${stamp}")`).load()
    .then(face => { document.fonts.add(face); rec.ready = true; redraw(); })
    .catch(() => { rec.failed = true; redraw(); });   /* this browser can't draw it; the console may still */
  return rec;
}
function themeFontFamily(path = null) {
  const rec = fontRec(path);
  return rec && rec.ready ? rec.family : "";
}

function dropZone(asp, title, detail) {
  return `<div class="drop-zone" data-drop="${asp.id}">
      <div class="text"><div class="title">${esc(title)}</div><div class="note">${esc(detail)}</div></div>
      <button class="btn" data-act="attach-aspect" data-aspect="${asp.id}">Choose file</button>
    </div>`;
}

/* A piece's picture: the theme's, the one you attached (kept with the theme) where the browser can't show the console's
   format, or the console's own. A piece with no picture to show has a drawing of its kind. */
const KIND_GLYPH = { texture: "images", audio: "sounds", font: "fonts", video: "video", json: "animations", rco: "interface" };
function thumbFor(a) {
  const rep = state.files.has(a.path);
  const cls = `thumb${rep ? " replaced" : ""}`;
  if (!isImage(a)) {
    if (rep && state.sources.has(a.path)) {
      return `<div class="${cls}"><img loading="lazy" alt="" src="${esc(`/api/theme/source?${q({ id: state.editId, path: a.path })}&v=${stamp}`)}"></div>`;
    }
    const what = a.kind === "texture" ? (rep ? "Your picture (not kept with the theme)" : "The console's picture") : ext(a.path).toUpperCase() || "File";
    return `<div class="${cls} glyph" title="${esc(what)}">${partIcon(KIND_GLYPH[a.kind] || "images")}</div>`;
  }
  const src = rep ? `/api/theme/file?${q({ id: state.editId, path: a.path })}&v=${stamp}` : `/api/asset?${q({ path: a.path })}`;
  return `<div class="${cls}"><img loading="lazy" alt="" src="${esc(src)}" onerror="this.remove()"></div>`;
}

/* A background area's name as people know it: a tile's area by the tile's name once it is read. */
function areaLabel(name) {
  const m = /^Home: (NPXS\d+) tile$/.exec(name);
  if (m) {
    const t = state.tileNames && state.tileNames.get(m[1]);
    return t ? `Home: ${t} tile` : `Home: a system tile (${m[1]})`;
  }
  if (name === "Image load error placeholder") return "Shown when a picture can't load";
  if (name === "System (VSH)") return "Other system backgrounds";
  return name;
}

/* A sound file by when it plays, where that's known (docs/home-music.md); otherwise its file name. */
const SOUND_FILES = {
  "sfx_warmboot.at9": "Waking from rest mode", "sfx_coldboot.at9": "Switching on (plays before the plugin starts)",
  "sfx_initialboot.at9": "First start, during set-up", "sfx_transition.at9": "A soft swell (when it plays isn't known yet)",
};
const soundName = path => SOUND_FILES[baseName(path)] || baseName(path);

/* A part's rows in groups people look for them by: fonts by where they are used, Appearance pieces by look. Each
   group folds; it opens when it is the first one or holds something replaced, and stays as it was left. A search
   shows its matches in one list. */
const FONT_LANGS = /^(Arabic|Thai|Vietnamese|Japanese|Korean|Chinese)/;
const ROW_GROUPS = {
  fonts: {
    of: g => { const b = baseName(g.assets[0].path), n = fontName(g.assets[0].path);
      return FONT_HOME.test(b) && FONT_LATIN.test(b) ? "home" : FONT_LANGS.test(n) ? "langs" : FONT_LATIN.test(b) || /^Interface text/.test(n) ? "latin" : "other"; },
    groups: [["home", "Home screen and Settings (Latin)"], ["latin", "Other Latin weights, for web pages"], ["langs", "Other languages"], ["other", "Other fonts"]],
  },
  appearance: {
    of: g => (APPEARANCE_RE.exec(g.assets[0].path) || [])[2] || "other",
    groups: [["ps", "PlayStation look"], ["ps2", "PlayStation 2 look"], ["ps3", "PlayStation 3 look"], ["ps4", "PlayStation 4 look"], ["other", "Other"]],
  },
};
function rowsGrouped(asp, list, filter, render) {
  const spec = ROW_GROUPS[asp.id];
  const flat = () => `<div class="rows">${list.map(render).join("")}</div>`;
  if (!spec || filter) return flat();
  const parts = spec.groups.map(([key, title]) => ({ key, title, items: list.filter(g => spec.of(g) === key) })).filter(x => x.items.length);
  if (parts.length < 2) return flat();
  const opened = state.foldOpen || (state.foldOpen = {});
  return `<div class="folds">${parts.map((x, i) => {
    const rep = x.items.filter(g => g.assets.some(a => state.files.has(a.path))).length;
    const key = `${asp.id}:${x.key}`;
    const open = key in opened ? opened[key] : i === 0 || rep > 0 || (asp.id === "appearance" && x.key === apView.look);
    return `<details class="fold" data-fold="${esc(key)}"${open ? " open" : ""}>
      <summary><span class="fold-title">${esc(x.title)}</span><span class="fold-count">${plural(x.items.length, asp.id === "fonts" ? "font" : "piece")}${rep ? ` · ${rep} replaced` : ""}</span></summary>
      <div class="rows">${x.items.map(render).join("")}</div></details>`;
  }).join("")}</div>`;
}

/* A line of the console's own words in a font the theme gives it. */
function fontSample(path) {
  const rec = fontRec(path);
  if (!rec || rec.failed) return `<div class="note">This browser can't draw this font, so there is no sample.</div>`;
  return rec.ready ? `<div class="font-sample" style="font-family:'${rec.family}'">Game Library  ·  Settings  ·  12:45</div>`
    : `<div class="note">Loading a sample…</div>`;
}

function aspectBody(asp) {
  if (asp.mode === "music") return musicBody();
  if (!state.assets) return `<p class="note">The system asset list is not available yet.</p>`;
  if (asp.mode === "rco") { setTimeout(rcoLookup); return rcoBody(asp); }
  if (asp.mode === "systiles") return tilesBody(asp);
  if (asp.mode === "titles") return titlesBody(asp);
  /* Sounds: the interface's own sounds follow the sound files */
  const uiBox = asp.id === "sounds" ? `<div id="ui-sounds">${uiSoundsBox()}</div>` : "";
  if (uiBox) setTimeout(uiSoundsShow);
  const all = aspectAssets(asp);
  if (!all.length) return `<p class="note">This firmware has no ${esc(asp.title.toLowerCase())} that can be themed. Anything else replaceable is under Other images.</p>${uiBox}`;

  if (asp.mode === "single") {
    tileNamesEnsure();
    const off = bgOff();
    const areas = new Map();
    for (const a of all) {
      const k = areaOf(a.path);
      if (!areas.has(k)) areas.set(k, []);
      areas.get(k).push(a);
    }
    const chosen = all.filter(a => !off.has(areaOf(a.path)));
    return `${dropZone(asp, "Drop an image or video here",
        chosen.length ? `Goes to ${plural(chosen.length, "file")} in the areas switched on below.` : "Switch on at least one area below.")}
      <p class="note">Where it goes${help("Each area is a place the home screen shows a background: the default background, the Game Library, a tile's background while that tile is selected, and others. Your picture goes to the areas switched on, made at each one's own size.")}</p>
      <div class="rows">${[...areas.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([name, list]) => {
        const on = !off.has(name), rep = list.filter(a => state.files.has(a.path)).length;
        return `<div class="row-item${rep ? " replaced" : ""}">
          <div class="info" style="display:flex;gap:14px;align-items:center;min-width:0">
            <button class="switch" role="switch" aria-checked="${on}" aria-label="${esc(areaLabel(name))}" data-act="toggle-area" data-area="${esc(name)}"></button>
            <div style="min-width:0"><div class="file">${esc(areaLabel(name))}${rep ? ` <span class="tag on">Replaced</span>` : ""}</div>
              ${list.length > 1 ? `<div class="dir">${plural(list.length, "file")}</div>` : ""}</div>
          </div>
          <div class="strip">${list.slice(0, 4).map(thumbFor).join("")}</div>
        </div>`;
      }).join("")}</div>
`;
  }

  const groupsAll = groupAssets(all);
  const filter = (state.filters[asp.id] || "").toLowerCase();
  const groups = filter ? groupsAll.filter(g => g.key.toLowerCase().includes(filter)) : groupsAll;
  const page = state.pages[asp.id] || 48;
  const shown = groups.slice(0, page);
  const search = groupsAll.length > 12
    ? `<div class="toolbar"><input type="search" data-filter-aspect="${asp.id}" placeholder="Search ${esc(asp.title.toLowerCase())}" value="${esc(state.filters[asp.id] || "")}" aria-label="Search ${esc(asp.title)}"></div>` : "";
  const more = groups.length > page ? `<div><button class="btn sm" data-act="more" data-aspect="${asp.id}">Show more (${groups.length - page})</button></div>` : "";
  if (!groups.length) return `${search}<p class="note">Nothing matches.</p>`;

  if (asp.mode === "tiles") {
    return `${search}<p class="note">Select one to replace it, or drop a picture on it.</p>
      <div class="tiles">${shown.map(g => {
        const rep = g.assets.filter(a => state.files.has(a.path)).length;
        return `<button class="tile${rep ? " replaced" : ""}" data-group="${esc(g.key)}" data-aspect="${asp.id}" title="${esc(g.key)}">
          ${rep ? `<span class="tag on">Replaced</span>` : ""}
          ${thumbFor(g.assets.find(a => state.files.has(a.path)) || g.assets[0])}
          <div class="file">${esc(g.name)}</div>
          <div class="meta">${g.assets.length > 1 ? plural(g.assets.length, "size") : fmtSize(g.assets[0].size)}</div>
        </button>`;
      }).join("")}</div>${more}
`;
  }

  const appearanceNote = asp.id === "appearance" ? `<p class="note">Each piece shows only while its look is chosen on the console. A PNG keeps its see-through parts.${help("Each picture is encoded to the texture's exact format and size (the panels are about 2560 pixels wide) and keeps its transparency. A change to Placement, padding or the clock band takes effect at once: the texture is made again from the original the theme keeps. If one doesn't, change the option and attach the picture again.")}</p>` : "";
  const fontNote = asp.id === "fonts" ? `<p class="note">Games keep their own fonts. The home screen draws in the weights marked <b>Home screen</b>.${help("The font needs the full Latin alphabet, digits and punctuation; fonts in other formats, damaged ones and ones without those characters are refused. On firmware 13.42 the home screen and Settings use the light and bold weights; the regular and medium Latin fonts are used by web pages only. If the home screen keeps restarting with a font, the plugin takes the theme down by itself.")}</p>
    <div class="toolbar"><button class="btn sm" data-act="fonts-latin" data-aspect="fonts">Use one font for all Latin interface text</button></div>` : "";
  const list = asp.id === "fonts"
    ? shown.slice().sort((x, y) => fontRank(x.assets[0]) - fontRank(y.assets[0]) || fontName(x.assets[0].path).localeCompare(fontName(y.assets[0].path)))
    : shown;
  if (asp.id === "appearance") setTimeout(appearanceColorsShow);
  /* Sounds: the interface's sounds (moving, selecting, going back) first, as people mean them; the sound files after */
  const soundsFirst = asp.id === "sounds" ? `${uiBox}<h3 class="sub-head">Other sounds</h3>` : "";
  if (asp.id === "appearance") { setTimeout(apPreviewShow); setTimeout(optionsShow); }
  return `${soundsFirst}${search}${fontNote}${appearanceNote}${asp.id === "appearance" ? apPreviewBox() : ""}${rowsGrouped(asp, list, filter, g => {
      const rep = g.assets.filter(a => state.files.has(a.path)).length;
      const a0 = g.assets[0];
      const isFont = asp.id === "fonts", isAppearance = asp.id === "appearance", isSound = asp.id === "sounds";
      const play = asp.id === "sounds" && ext(a0.path) === "wav"
        ? `<button class="btn sm ghost" data-act="wav-play" data-src="${esc(rep ? `/api/theme/file?${q({ id: state.editId, path: a0.path })}&v=${stamp}` : `/api/asset?${q({ path: a0.path })}`)}">Play</button>` : "";
      const title = isFont ? fontName(a0.path) : isAppearance ? appearanceName(a0.path) : isSound ? soundName(a0.path) : g.name;
      const file = baseName(a0.path) === title ? "" : `${baseName(a0.path)} · `;   // a file known only by its name isn't named twice
      return `<div class="row-item${rep ? " replaced" : ""}" data-drop-group="${esc(g.key)}" data-aspect="${asp.id}">
        <div class="info"><div class="file">${esc(title)}${isFont && FONT_HOME.test(baseName(a0.path)) ? ` <span class="tag">Home screen</span>` : ""}${rep ? ` <span class="tag on">Replaced</span>` : ""}</div>
          <div class="dir">${isFont || isAppearance || isSound ? `${esc(file)}${fmtSize(a0.size)}` : esc(g.dir)}${g.assets.length > 1 ? ` · ${plural(g.assets.length, "variant")}` : ""}</div>${isFont && rep ? fontSample(a0.path) : ""}${
          isAppearance && textureAnchor(a0.path).band ? `<label class="note check"><input type="checkbox" data-keep="${esc(a0.path)}"${keepClearFor(a0.path) ? " checked" : ""}> Keep the clock and icons clear${help("The clock and the search and settings icons are drawn over the top of this piece: a white or solid picture there hides them. On, the top 26% is left empty and the picture goes below it.")}</label>` : ""}</div>
        <div class="acts">${play}${isAppearance ? (() => {
          const pl = placementFor(a0.path);
          if (rep && state.sources.has(a0.path)) {
            return pl.mode === "pad" ? `<label class="note">Padding
            <input type="number" min="0" max="40" step="1" value="${pl.pad}" data-pad="${esc(a0.path)}" style="width:64px" aria-label="Padding in percent"> %</label>` : "";
          }
          return `<label class="note">Placement
            <select data-place="${esc(a0.path)}" aria-label="Placement">
              <option value="fill"${pl.mode === "fill" ? " selected" : ""}>Fill (crop to the piece)</option>
              ${pl.hasEdge ? `<option value="pad"${pl.mode === "pad" ? " selected" : ""}>Fill with padding</option>` : ""}
              <option value="fit"${pl.mode === "fit" ? " selected" : ""}>Fit (whole picture)</option>
              <option value="stretch"${pl.mode === "stretch" ? " selected" : ""}>Stretch</option>
            </select></label>${pl.mode === "pad" ? `<label class="note">Padding
            <input type="number" min="0" max="40" step="1" value="${pl.pad}" data-pad="${esc(a0.path)}" style="width:64px" aria-label="Padding in percent"> %</label>` : ""}${
            rep && !state.sources.has(a0.path) ? (state.keepOriginals
              ? `<span class="note" title="This picture was attached before the original was kept in the theme. Changing an option makes the texture again from the picture inside it; attach the original once more to get back anything the earlier placement cropped away.">options use the texture's own picture</span>`
              : `<span class="note" title="This theme does not keep original pictures: options apply when you attach a picture.">options apply when you attach</span>`) : ""}${
""}`;
        })() : ""}
          <button class="btn sm" data-act="attach-group" data-group="${esc(g.key)}" data-aspect="${asp.id}">${rep ? "Replace" : "Attach"}</button>
          ${rep ? `<button class="btn sm ghost danger" data-act="reset-group" data-group="${esc(g.key)}" data-aspect="${asp.id}">Reset</button>` : ""}
        </div>${isAppearance && rep && state.sources.has(a0.path) ? placeOptionsHtml(a0.path) : ""}</div>`;
    })}${more}${asp.id === "appearance" ? `<div id="appearance-colors">${colorsBox()}</div>` : ""}${soundsFirst ? "" : uiBox}`;
}

const gainLabel = g => (g ? `${g > 0 ? "+" : "−"}${Math.abs(g)} dB` : "same loudness as the console's music");

function musicBody() {
  const t = themeById(state.editId);
  if (state.assets && !state.assets.some(a => a.path === HOME_MUSIC)) {
    return `<p class="note">This firmware has no home music file (bgm_home.at9) to replace.</p>`;
  }
  if (state.trim && state.trim.id === t.id) return trimBody();
  if (isPlaylist(t)) return playlistBody(t);
  const converted = state.files.has(HOME_MUSIC);
  if (!t.music && !converted) {
    return dropZone(aspectById("music"), "Drop a music file here",
      "MP3, M4A, OGG, WAV, FLAC and more.");
  }
  const status = !converted ? "Added with an earlier version. Convert it to make it the home music."
    : "Checking the home music file…";
  if (converted) checkHomeMusic(t.id);
  return `<div class="row-item replaced">
      <div class="info"><div class="file">Home music${t.music ? ` · ${esc(partLabel(t))}` : ""}</div>
        <div class="dir" id="home-music-status">${esc(status)}</div>
        ${t.music ? `<audio controls preload="none" src="/api/theme/music?${q({ id: t.id })}&v=${stamp}"></audio>` : ""}</div>
      <div class="acts">
        ${t.music && !converted ? `<button class="btn sm primary" data-act="convert-music">Convert</button>` : ""}
        <button class="btn sm primary" data-act="convert-music" id="home-music-fix" hidden>Encode again</button>
        ${t.music ? `<button class="btn sm" data-act="choose-part">Choose part</button>
          <button class="btn sm" data-act="song-add">Add song</button>${help(`Make a playlist: the songs play one after another, each its own part, then start again (or the last one keeps repeating). Up to ${MUSIC_MAX_TRACKS} songs, within the console's limit of about twice its own music's length.`)}` : ""}
        <button class="btn sm" data-act="attach-aspect" data-aspect="music">Replace</button>
        <button class="btn sm ghost danger" data-act="remove-music">Remove</button>
      </div></div>
    ${t.music ? `<label class="field">Loudness <span id="gain-label">${esc(gainLabel(t.gain || 0))}</span>
      <input type="range" min="-12" max="12" step="1" value="${t.gain || 0}" id="music-gain"></label>` : ""}
    ${converted ? `<p class="note">${esc(MUSIC_HEAR_NOTE)} ${esc(MUSIC_PLACE_NOTE)}</p>` : ""}`;
}
