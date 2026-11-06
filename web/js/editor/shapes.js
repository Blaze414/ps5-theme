"use strict";

/* ---- Corner shapes: the interface's shape list (rcoShapes) ---- */
const SHAPE_NAMES = {
  "borderRadiusRatio": "Overall multiplier", "bar.fill": "Bars",
  "slider.thumb": "Slider handles", "slider.thumb.focused": "Slider handles, selected", "button.fill": "Buttons",
  "button.indicator.fill": "Button indicators", "checkbox.stroke": "Checkboxes", "dialog.fill": "Dialogs",
  "key-guide.fill": "Button guides", "menu.fill": "Menus", "menu.item.fill": "Menu items", "switch.track": "Switches",
  "switch.thumb": "Switch knobs", "tag.small": "Tags, small", "tag.medium": "Tags, medium", "tag.large": "Tags, large",
  "tooltip.fill": "Tooltips", "tooltip.error.stroke": "Error tooltips' outline", "tab.vertical": "Tabs, vertical",
  "tab.horizontal": "Tabs, horizontal", "list.item": "List items", "form": "Forms",
};
const shapeMax = name => /Ratio/.test(name) ? 10 : 100;
const shapeValues = text => new Map([...text.matchAll(SHAPE_PAIR)].map(m => [m[2], Number(m[3])]));

async function rcoShapesShow() {
  const box = $("#rco-shapes");
  if (!box) return;
  const wait = (key, label) => { if (box.isConnected) { box.innerHTML = loadingMarker(key, label); paintLoads(box); } };
  let html = "";
  try {
    for (const path of rcoPaths()) {
      if (rcoFailed.has(path)) throw new Error(rcoFailed.get(path));
      if (!(state.rco && state.rco.has(path))) wait(`stock:${path}`, "Reading the corner shapes…");
      const r = await rcoStock(path), sh = r && rcoShapes(r);
      if (!sh || !sh.values.length) continue;
      if (!rcoThemeReady(path)) wait(`theme:${path}`, "Reading this theme's corner shapes…");
      const mine = (await rcoChanges(path)).get(`${sh.f.el}\t${sh.f.attr}`);
      const now = mine ? shapeValues(new TextDecoder().decode(mine)) : new Map(sh.values);
      const rows = sh.values.map(([name, stock], i) => {
        const ratio = /Ratio/.test(name), cur = now.has(name) ? now.get(name) : stock;
        return `<label for="shape-${i}" style="${cur !== stock ? "font-weight:600" : ""}">${esc(SHAPE_NAMES[name] || name)}</label>
          <input type="number" id="shape-${i}" data-shape="${esc(name)}" min="0" max="${shapeMax(name)}" step="${ratio ? 0.1 : 0.5}"
            value="${cur}" style="width:100%">
          <span class="muted" style="font-size:.875rem">console: ${stock}</span>`;
      }).join("");
      html += `<div class="stack"><p class="note"><b>Corner shapes</b>: how round each part's corners are, in pixels (0 is square).${help(`In ${baseName(path)}, at 1080p. The overall multiplier at the end showed no change on the console. The home screen's tiles keep their shape: the home screen sets their corners itself. Apply the theme, then put the console in rest mode and wake it.`)}</p>
        <div style="display:grid;grid-template-columns:minmax(0,1fr) 96px auto;gap:8px 16px;align-items:center;max-width:440px">${rows}</div>
        <div class="toolbar"><button class="btn sm primary" data-act="shapes-save" data-path="${esc(path)}">Use these shapes</button>
          ${mine ? `<button class="btn sm ghost danger" data-act="shapes-reset" data-path="${esc(path)}">Console's shapes</button>` : ""}</div></div>`;
      break;
    }
  } catch (e) {
    html = `<div class="banner err" role="alert">The corner shapes could not be read: ${esc(e.message.replace(/\.$/, ""))}.
      <button class="btn sm" data-act="rco-retry">Try again</button></div>`;
  }
  if (box.isConnected) box.innerHTML = html;
}

/* The Appearance themes' colours: each theme's palette, kept in the interface's resource file, with the colour
   test per palette and its key. */
let colorsLast = null;   /* the colours as last shown, and for which theme: shown again at once when the section is redrawn */
function colorsWaitNote(path) {
  const a = aspectAssets(aspectById("interface")).find(x => x.path === path);
  return a ? `The console keeps them in a ${fmtSize(a.size)} file. It is read once while this page stays open.` : "";
}
function colorsBox() {
  if (colorsLast && colorsLast.id === state.editId) return colorsLast.html;
  const first = rcoPaths()[0];
  return first ? loadingMarker(`stock:${first}`, "Reading the console's colours…", colorsWaitNote(first)) : "";
}
async function appearanceColorsShow() {
  const box = $("#appearance-colors");
  if (!box) return;
  const id = state.editId, shown = colorsLast && colorsLast.id === id ? colorsLast.html : "";
  /* While a file is read: its marker, above the colours as last shown (dimmed, and not usable meanwhile). */
  const wait = (key, label, path) => {
    if (!box.isConnected) return;
    box.innerHTML = loadingMarker(key, label, shown ? "" : colorsWaitNote(path)) + (shown ? `<div class="stale" inert aria-hidden="true">${shown}</div>` : "");
    paintLoads(box);
  };
  let html = "", testOn = false, failed = false;
  try {
    for (const path of rcoPaths()) {
      if (rcoFailed.has(path)) throw new Error(rcoFailed.get(path));
      if (!(state.rco && state.rco.has(path))) wait(`stock:${path}`, "Reading the console's colours…", path);
      const r = await rcoStock(path), pals = r ? rcoColors(r).sort((x, y) => paletteRank(x.id) - paletteRank(y.id)) : [];
      if (!pals.length) continue;
      if (!rcoThemeReady(path)) wait(`theme:${path}`, "Reading this theme's colours…", path);
      const changes = await rcoChanges(path), group = colorTestValues(pals), panel = colorPanelValues(pals);
      /* Only the default look's palette is offered: the console uses it for the widgets, the Control Center and the button
         hints with every Appearance theme, and no change to a theme's own palette has shown on the console. A theme
         changed earlier in one of those can have them put back. */
      const hiddenChanged = pals.filter(p => p.id !== "color_default" && changes.has(`${p.f.el}\t${p.f.attr}`));
      const panelKey = COLOR_PANEL_TEST.map(([name, rgb, word]) => `<span style="display:inline-flex;align-items:center;gap:6px;margin:2px 14px 2px 0">
          <i style="width:14px;height:14px;border-radius:3px;background:#${rgb.toString(16).padStart(6, "0")}"></i>${esc(word)}: ${esc(name)}</span>`).join("");
      const key = COLOR_TEST.map(g => `<span style="display:inline-flex;align-items:center;gap:6px;margin:2px 14px 2px 0">
          <i style="width:14px;height:14px;border-radius:3px;background:#${g.rgb.toString(16).padStart(6, "0")}"></i>${esc(g.label)}</span>`).join("");
      html = `<h3 class="sub-head">Colours</h3>
        <div class="banner">For the widgets' colours, turn off <b>Use System Appearance for Widgets</b> in the Welcome hub's settings.${help("With it on, the Welcome hub picks the widgets' colours for each Appearance theme itself and ignores these.")}</div>
        <p class="note">The same colours with every Appearance theme.${help(`They are the default look's palette; each Appearance theme has one of its own too, but changes to those didn't show on the console, so they aren't offered. Each colour keeps the console's transparency, and Console's colours puts them back. They are kept in the interface's resource file (${baseName(path)}), so Interface icons shows that file as changed too.`)}</p>
        ${hiddenChanged.length ? `<div class="banner">This theme also changes the colours of ${plural(hiddenChanged.length, "Appearance theme")}
          (${hiddenChanged.map(p => esc(PALETTE_NAMES[p.id] || p.id)).join(", ")}), from an earlier test or edit. The console showed none of
          those changes, and they are no longer offered. <button class="btn sm ghost danger" data-act="colors-reset-others" data-path="${esc(path)}">Put the console's colours back there</button></div>` : ""}
        <div class="rows">${pals.map((p, i) => p.id !== "color_default" ? "" : (() => {
          const mine = changes.get(`${p.f.el}\t${p.f.attr}`), text = mine ? new TextDecoder().decode(mine) : null;
          const on = text === null ? "" : text === colorsText(p.text, panel[i]) ? "Panel test" : text === colorsText(p.text, group[i]) ? "Colour test" : "Your colours";
          testOn = testOn || /test$/.test(on);
          const editing = state.colorEdit === p.id;
          let editor = "";
          if (editing) {
            const cur = new Map(text ? [...text.matchAll(COLOR_PAIR)].map(m => [m[2], Number(m[3])]) : p.values), sets = colorEditSets(pals, i);
            const stockOf = new Map(p.values);
            const hex = c => "#" + c.toString(16).padStart(6, "0");
            editor = `<div class="row-item" style="display:block"><div style="display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:8px 14px;align-items:center;max-width:640px">
              ${COLOR_EDIT.filter(e => sets[e.key] && (p.id === "color_default" || !e.shared)).map(e => {
                const set = sets[e.key], name = set.names.find(n => rgbOf(stockOf.get(n)) === set.base) || set.names[0];
                const now = rgbOf(cur.get(name));
                return `<div><label for="ce-${e.key}"><b>${esc(e.label)}</b></label>${help(e.help)}${e.hint ? `<br><span class="muted" style="font-size:.85rem">${esc(e.hint)}</span>` : ""}</div>
                  <input type="color" id="ce-${e.key}" data-color-edit="${e.key}" value="${hex(now)}" style="width:56px;height:34px;padding:0;border:0;background:none">
                  <span class="muted" style="font-size:.85rem">console: ${hex(set.base)}</span>`;
              }).join("")}</div>
              <div class="toolbar"><button class="btn sm primary" data-act="colors-save" data-path="${esc(path)}" data-palette="${esc(p.id)}">Use these colours</button>
                <button class="btn sm ghost" data-act="colors-close">Close</button></div></div>`;
          }
          return `<div class="row-item${on ? " replaced" : ""}"><div class="info">
              <div class="file">${p.id === "color_default" ? "Every Appearance theme" : `${esc(PALETTE_NAMES[p.id] || p.id)}: Colours`}${help("The colours of the Welcome widgets and pills, the Control Center's backdrop and card, the button hints, the text in Settings and menus, and the accent, for every Appearance theme. For the widgets, turn off Use System Appearance for Widgets in the Welcome hub's settings.")}${on ? ` <span class="tag on">${on}</span>` : ""}</div>
              <div class="dir">${p.id === "color_default"
                ? "Welcome widgets and pills, Control Center, button hints, text and accent"
                : "This theme's own text and accent; what they change with this theme selected hasn't been seen on the console yet"}</div></div>
            <div class="acts">${editing ? "" : `<button class="btn sm" data-act="colors-edit" data-palette="${esc(p.id)}">Edit colours</button>`}
              ${on ? `<button class="btn sm ghost danger" data-act="colors-reset" data-path="${esc(path)}" data-palette="${esc(p.id)}">Console's colours</button>` : ""}</div></div>${editor}`;
        })()).join("")}</div>
        <details class="note" data-color-tests${state.colorTestsOpen ?? testOn ? " open" : ""}><summary><b>Tests</b>, for finding what other colours change</summary>
        <p>The <b>colour test</b> paints each kind of colour in a bright colour of its own, so a photo shows what it changes:</p>
        <div style="display:flex;flex-wrap:wrap">${key}</div>
        <p>With <b>Use System Appearance for Widgets</b> off, the colour test turns the Welcome widgets and the pills above them the
          panels' colour. The <b>panel test</b> gives each panel and base mat colour a colour of
          its own, everything else as the console's, to tell which one colours what:</p>
        <div style="display:flex;flex-wrap:wrap">${panelKey}</div>
        <div class="toolbar"><button class="btn sm" data-act="colors-test" data-path="${esc(path)}" data-palette="color_default">Colour test</button>
          <button class="btn sm" data-act="colors-panel" data-path="${esc(path)}" data-palette="color_default">Panel test</button></div></details>`;
      break;
    }
    if (!html) html = `<h3 class="sub-head">Colours</h3><p class="note">This firmware's interface files have no colours that can be changed.</p>`;
  } catch (e) {
    failed = true;
    html = `<h3 class="sub-head">Colours</h3><div class="banner err" role="alert">The colours could not be read: ${esc(e.message.replace(/\.$/, ""))}.
      <button class="btn sm" data-act="rco-retry">Try again</button></div>`;
  }
  if (!box.isConnected || state.editId !== id) return;
  box.innerHTML = html;
  if (!failed) colorsLast = { id, html };
}

function saveColorEdit(path, palette) { return rcoQueued("Saving the colours", () => saveColorEditNow(path, palette)); }
async function saveColorEditNow(path, palette) {
  try {
    const r = await rcoStock(path), pals = rcoColors(r), i = pals.findIndex(p => p.id === palette), changes = await rcoChanges(path);
    if (i < 0) throw new Error("no such palette");
    const picks = {};
    for (const input of document.querySelectorAll("[data-color-edit]")) {
      const m = /^#([0-9a-f]{6})$/i.exec(input.value.trim());
      if (!m) throw new Error("use a colour like #3366ff");
      picks[input.dataset.colorEdit] = parseInt(m[1], 16);
    }
    const p = pals[i], k = `${p.f.el}\t${p.f.attr}`, values = colorEditValues(pals, i, picks), text = colorsText(p.text, values);
    const widgets = (colorEditSets(pals, i).widgets || { names: [] }).names;
    const widgetsChanged = widgets.some(n => new Map(values).get(n) !== new Map(p.values).get(n));
    if (text === p.text) changes.delete(k); else changes.set(k, new TextEncoder().encode(text));
    await rcoSave(path, changes);
    state.colorEdit = null;
    toast(`${text === p.text ? "The console's own colours" : "Your colours"} saved and verified. Apply the theme, then put the console in rest mode and wake it.${widgetsChanged ? " For the widgets' colour, turn off Use System Appearance for Widgets in the Welcome hub's settings." : ""}`, "ok", 12000);
  } catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("appearance"));
  renderAspect(aspectById("interface"));
}

function resetOtherPalettes(path) { return rcoQueued("Putting the console's colours back", () => resetOtherPalettesNow(path)); }
async function resetOtherPalettesNow(path) {
  try {
    const r = await rcoStock(path), changes = await rcoChanges(path);
    for (const p of rcoColors(r)) if (p.id !== "color_default") changes.delete(`${p.f.el}\t${p.f.attr}`);
    await rcoSave(path, changes);
    toast("The Appearance themes' own colours are the console's again in this theme. Apply the theme to use it.", "ok", 7000);
  } catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("appearance"));
  renderAspect(aspectById("interface"));
}

function saveColors(path, reset, palette, panel = false) { return rcoQueued("Saving the colours", () => saveColorsNow(path, reset, palette, panel)); }
async function saveColorsNow(path, reset, palette, panel = false) {
  try {
    const r = await rcoStock(path), pals = rcoColors(r), changes = await rcoChanges(path);
    const test = panel ? colorPanelValues(pals) : colorTestValues(pals);
    pals.forEach((p, i) => {
      if (palette && p.id !== palette) return;
      const k = `${p.f.el}\t${p.f.attr}`;
      if (reset) changes.delete(k);
      else changes.set(k, new TextEncoder().encode(colorsText(p.text, test[i])));
    });
    await rcoSave(path, changes);
    toast(reset ? "The console's own colours are back in this theme. Apply the theme, then put the console in rest mode and wake it."
      : `${panel ? "Panel test" : "Colour test"} added and verified. Apply the theme, put the console in rest mode and wake it, and compare with the key.`, "ok", 9000);
  } catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("appearance"));
  renderAspect(aspectById("interface"));
}

function saveShapes(path, reset) { return rcoQueued("Saving the shapes", () => saveShapesNow(path, reset)); }
async function saveShapesNow(path, reset) {
  try {
    const r = await rcoStock(path), sh = rcoShapes(r), k = `${sh.f.el}\t${sh.f.attr}`, changes = await rcoChanges(path);
    if (reset) changes.delete(k);
    else {
      const values = sh.values.map(([name, stock]) => {
        const input = document.querySelector(`[data-shape="${CSS.escape(name)}"]`);
        const v = input && input.value.trim() !== "" ? Number(input.value) : stock;
        if (!Number.isFinite(v) || v < 0 || v > shapeMax(name)) {
          throw new Error(`${SHAPE_NAMES[name] || name}: use a number from 0 to ${shapeMax(name)}`);
        }
        return [name, v];
      });
      const text = shapesText(sh.text, values);
      if (text === sh.text) changes.delete(k); else changes.set(k, new TextEncoder().encode(text));
    }
    await rcoSave(path, changes);
    toast(reset ? "The console's own shapes are back." : "Shapes saved and verified. Apply the theme, then put the console in rest mode and wake it.", "ok");
  } catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("interface"));
}

let rcoLookupTimer = 0;
async function rcoLookup() {
  const text = (state.rcoLookup || "").trim().toLowerCase();
  const box = $("#rco-lookup-result");
  if (!box) return;
  /* The lookup needs every interface file: while they are read, how far that has got, when there will be something
     to show (a search, or the pictures this theme changes). */
  const paths = rcoPaths();
  if (!paths.every(p => state.rco && state.rco.has(p)) && (text || paths.some(p => state.files.has(p)))) {
    box.innerHTML = loadingMarker("rco:all", "Reading the interface files…", "The lookup needs all of them; they are read once while this page stays open.");
    paintLoads(box);
  }
  let html;
  try {
    const key = await rcoKey(), changed = new Map();
    for (const path of new Set(key.map(k => k.path))) changed.set(path, await rcoChanges(path));
    const isChanged = k => k.pic.files.some(f => changed.get(k.path).has(`${f.el}\t${f.attr}`));
    let hits, more = 0;
    if (!text) {
      const all = key.filter(isChanged);
      hits = all.slice(0, 24); more = all.length - hits.length;
      if (!hits.length) { $("#rco-lookup-result").innerHTML = ""; return; }
    } else {
      hits = /^\d+$/.test(text) ? key.filter(k => k.n === +text) : key.filter(k => k.pic.id.toLowerCase().includes(text)).slice(0, 24);
    }
    html = hits.length ? `${text ? "" : `<p class="note">Pictures this theme changes:</p>`}<div class="rows">${hits.map(k => {
      const f = k.pic.files[0], mine = changed.get(k.path).get(`${f.el}\t${f.attr}`), rep = isChanged(k);
      const url = URL.createObjectURL(new Blob([mine || rcoData(k.r, f)], { type: f.kind === "svg" ? "image/svg+xml" : "image/png" }));
      const what = k.pic.kind === "svg" ? "vector icon" : k.pic.kind === "png" ? (() => { const fm = pngFormat(rcoData(k.r, f)); return fm ? `${fm.w}×${fm.h} picture` : "picture"; })() : "picture";
      return `<div class="row-item${rep ? " replaced" : ""}"><div class="info"><div class="file">${k.n} · ${esc(k.pic.id)}${rep ? ` <span class="tag on">Changed</span>` : ""}</div>
          <div class="dir">${what} · ${esc(baseName(k.path))}</div></div>
        <div class="strip"><div class="thumb"><img alt="" src="${url}"></div></div>
        <div class="acts">${k.pic.kind ? `<button class="btn sm" data-act="rco-pick" data-path="${esc(k.path)}" data-el="${k.pic.el}">${rep ? "Replace" : "Use my picture"}</button>` : ""}
          ${rep ? `<button class="btn sm ghost danger" data-act="rco-unpick" data-path="${esc(k.path)}" data-el="${k.pic.el}">Reset</button>` : ""}</div></div>`;
    }).join("")}</div>${more ? `<p class="note">and ${more} more.</p>` : ""}` : `<p class="note">No picture ${/^\d+$/.test(text) ? "has that number" : "matches"}.</p>`;
  } catch (e) {
    html = `<div class="banner err" role="alert">The lookup could not read the interface files: ${esc(e.message.replace(/\.$/, ""))}.
      <button class="btn sm" data-act="rco-retry">Try again</button></div>`;
  }
  /* The section may have been redrawn, or the text changed, while reading. */
  const out = $("#rco-lookup-result");
  if (out && (state.rcoLookup || "").trim().toLowerCase() === text) out.innerHTML = html;
}
