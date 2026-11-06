"use strict";

/* ================================================================ live preview

   The home screen and the Control Center as the console draws them, with the theme: its background, pictures,
   font, colours and the Appearance artwork of the look you pick. It reads from the console with the same GETs the
   editor uses, builds nothing on the console, and uploads nothing. */

/* Each look's Appearance theme, as Settings names it. */
const LOOK_NAMES = { default: "Default look", ps: "PlayStation", ps2: "PlayStation 2", ps3: "PlayStation 3", ps4: "PlayStation 4" };

let pvLooksArt = new Set();   /* the looks the theme has artwork for */
let pvState = { look: "default", view: "home" };
function previewClose() {
  const ov = $("#pv-overlay");
  if (ov) ov.remove();
  document.removeEventListener("keydown", previewKey);
}
function previewKey(e) {
  if (e.key === "Escape") return previewClose();
  if (pvState.view !== "home" || !pvState.count || !/^Arrow(Left|Right)$/.test(e.key) || (e.target && /^(SELECT|INPUT|TEXTAREA)$/.test(e.target.tagName))) return;
  e.preventDefault();
  const to = Math.max(0, Math.min(pvState.count - 1, pvState.focus + (e.key === "ArrowLeft" ? -1 : 1)));
  if (to !== pvState.focus) { pvState.focus = to; previewRender(state.editId); }
}
/* A click on the drawn home screen: a tile selects it, Games or Media switches rows, as on the console. */
let pvHome = null;   /* where the last drawing put the tabs */
function previewHomeClick(e) {
  const c = e.target.closest && e.target.closest("canvas.hs");
  if (!c || !pvHome || pvState.view !== "home") return;
  const b = c.getBoundingClientRect(), x = (e.clientX - b.left) * 1920 / b.width, y = (e.clientY - b.top) * 1080 / b.height;
  const tab = y >= 28 && y <= 92 ? pvHome.tabs.findIndex(([from, to]) => x >= from - 16 && x <= to + 16) : -1;
  if (tab >= 0) {
    const v = tab ? "media" : "games";
    if (v !== pvState.tab) { pvState.tab = v; pvState.focus = v === "games" ? 1 : 0; previewRender(state.editId); }
    return;
  }
  const i = hsTileAt(x, y, pvState.focus, pvState.count);
  if (i >= 0 && i !== pvState.focus) { pvState.focus = i; previewRender(state.editId); }
}
document.addEventListener("keydown", e => { if (e.key === "Escape" && $("#bge")) { bgeClose(); e.stopImmediatePropagation(); } }, true);

function previewToolbar() {
  const opts = Object.keys(LOOK_NAMES).map(l =>
    `<option value="${l}" ${pvState.look === l ? "selected" : ""}>${esc(LOOK_NAMES[l])}${pvLooksArt.has(l) ? " ✦" : ""}</option>`).join("");
  return `<div class="pv-tabs">
      <button class="btn sm chip" type="button" data-act="pv-view" data-view="home" aria-pressed="${pvState.view === "home"}">Home</button>
      <button class="btn sm chip" type="button" data-act="pv-view" data-view="cc" aria-pressed="${pvState.view === "cc"}">Control Center</button></div>
    <label class="pv-looklbl">Appearance theme <select id="pv-look" style="width:auto">${opts}</select></label>`;
}

async function openPreview() {
  const t = themeById(state.editId);
  if (!t) return;
  previewClose();
  pvLooksArt = new Set([...state.files].map(p => (APPEARANCE_RE.exec(p) || [])[2]).filter(Boolean));
  pvState = { look: homeLookOf(state.files), view: "home", tab: "", focus: 0, count: 0 };
  homeDb = null;   /* (the console's rows as they are now) */
  const ov = document.createElement("div");
  ov.id = "pv-overlay";
  ov.className = "pv-overlay";
  ov.innerHTML = `<div class="pv-dialog" role="dialog" aria-modal="true" aria-label="Preview of ${esc(t.name)}">
      <div class="pv-bar"><h2>Preview · ${esc(t.name)}</h2><span class="spacer"></span>
        <button class="btn ghost sm" data-act="pv-refresh">Refresh</button>
        <button class="btn sm" data-act="pv-close">Close</button></div>
      <div class="pv-toolbar" id="pv-toolbar"></div>
      <div class="pv-stage" id="pv-stage"><div class="pv-busy">Building the preview…</div></div>
      <p class="pv-note" id="pv-note"></p>
    </div>`;
  document.body.append(ov);
  ov.addEventListener("click", e => { if (e.target === ov) previewClose(); else previewHomeClick(e); });
  document.addEventListener("keydown", previewKey);
  await previewRender(state.editId);
}

const PV_NOTES = {
  home: "Your console's games and apps, in its order. Select a tile with a click or ← →; Games and Media switch rows.",
  cc: "The Control Center over the home screen, with the look's pieces, the theme's colours and font.",
};
/* Draws the view the preview shows: the home screen, or the Control Center over it. */
let pvSeq = 0;
async function previewDraw(id) {
  const seq = ++pvSeq, files = state.files, view = pvState.view;
  if (!pvState.tab) {
    await loadAssets();
    Object.assign(pvState, homeCardView(await homeRows(), files, homeIndex()));   /* where the theme's card looks */
  }
  const where = { tab: pvState.tab, focus: pvState.focus, look: pvState.look };
  const scene = view === "cc" ? await ccScene(id, files, where) : await homeScene(id, files, where);
  const stage = $("#pv-stage");
  if (seq !== pvSeq || !stage || !$("#pv-overlay") || state.editId !== id || pvState.view !== view) return;
  const home = view === "cc" ? scene.home : scene;
  pvState.focus = home.focus;
  pvState.count = home.tiles.length;
  let c = stage.querySelector("canvas.hs");
  if (!c) {
    stage.innerHTML = `<canvas class="hs" width="1920" height="1080" role="img"></canvas>`;
    c = stage.querySelector("canvas.hs");
  }
  c.classList.toggle("cc", view === "cc");
  if (view === "cc") {
    c.setAttribute("aria-label", `The Control Center with the ${LOOK_NAMES[pvState.look]} look`);
    ccScreenDraw(c.getContext("2d"), scene);
    pvHome = null;
    return;
  }
  const sel = home.tiles[home.focus];
  c.setAttribute("aria-label", `The home screen: ${pvState.tab === "media" ? "Media" : "Games"}${sel ? `, ${sel.name} selected` : ""}`);
  pvHome = homeScreenDraw(c.getContext("2d"), scene);
}

function previewRender(id) {
  if (!$("#pv-stage")) return;
  const bar = $("#pv-toolbar");
  if (bar) bar.innerHTML = previewToolbar();
  const note = $("#pv-note");
  if (note) note.textContent = PV_NOTES[pvState.view] || "";
  return previewDraw(id);
}
