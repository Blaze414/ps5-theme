"use strict";

/* ---- The picture behind the icons, placed as a profile picture is ----

   Your picture with the square each icon shows outlined and the rest dimmed, the style's shape and one of your
   games' icons on it, and the icons at the sizes the console shows them. Drag it (or the arrow keys, as a
   controller's D-pad sends) to move it; zoom with the slider, + and − or the wheel. The zoom never goes below
   what fills the square, so the icons never show an edge of it. "Use this picture" keeps the square as the theme's
   picture behind the icons (1024 pixels, never applied to the console on its own). */
const BGE_OUT = 1024;
let bge = null;

async function openBgEditor(source) {
  bgeClose();
  let img = null;
  try {
    if (source instanceof Blob) {
      const url = URL.createObjectURL(source);
      try { img = await loadImage(url); } finally { URL.revokeObjectURL(url); }
    } else {
      img = await loadImage(source);
    }
  } catch (e) { return toast("That picture can't be read here: use a PNG, JPEG or WebP.", "err"); }
  const st = iconStyle(), g = titleGroups().find(x => titleIcon(x));
  let icon = null;
  try { icon = g ? await titleOwnIcon(g) : null; } catch (e) { icon = null; }
  const F = Math.max(200, Math.min(400, innerWidth - 96)), M = Math.round(F * 0.14);
  const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
  const fill = Math.max(F / iw, F / ih);   /* the smallest zoom that still fills the square */
  bge = { img, iw, ih, F, M, fill, zoom: 1, s: fill, ox: (F - iw * fill) / 2, oy: (F - ih * fill) / 2, icon, showIcon: !!icon, st };
  const hasBg = aspectReplaced(aspectById("background")) > 0;
  const ov = document.createElement("div");
  ov.id = "bge";
  ov.className = "pv-overlay";
  ov.innerHTML = `<div class="bge-dialog" role="dialog" aria-modal="true" aria-labelledby="bge-title">
      <div class="pv-bar"><h2 id="bge-title">Picture behind the icons</h2><span class="spacer"></span>
        <button class="btn ghost sm" type="button" data-act="bge-cancel">Cancel</button>
        <button class="btn primary sm" type="button" data-act="bge-save">Use this picture</button></div>
      <div class="bge-body">
        <canvas id="bge-canvas" width="${F + 2 * M}" height="${F + 2 * M}" tabindex="0"
          aria-label="The picture behind the icons. Drag it, or use the arrow keys, to move it; + and − zoom."></canvas>
        <div class="panel bge-side">
          <div class="field"><span>Zoom <span id="bge-zoom-label">100%</span></span>
            <div class="bge-zoom">
              <button class="btn sm" type="button" data-act="bge-zoom" data-v="-1" aria-label="Zoom out">−</button>
              <input type="range" id="bge-zoom" min="100" max="400" step="1" value="100" aria-label="Zoom">
              <button class="btn sm" type="button" data-act="bge-zoom" data-v="1" aria-label="Zoom in">+</button>
            </div></div>
          <div><button class="btn sm ghost" type="button" data-act="bge-reset">Centre and fit</button></div>
          ${icon ? `<label class="note check"><input type="checkbox" id="bge-icon" checked> Show a game's icon on it</label>` : ""}
          <div class="bge-previews">
            <figure><canvas data-bge-pv="160" width="160" height="160"></canvas><figcaption>Home screen</figcaption></figure>
            <figure><canvas data-bge-pv="72" width="72" height="72"></canvas><figcaption>Game Library</figcaption></figure></div>
          <div class="bge-sources"><button class="btn sm" type="button" data-act="bge-file">Choose another picture</button>
            ${hasBg ? `<button class="btn sm" type="button" data-act="bge-fromtheme">Use the theme's background</button>` : ""}</div>
          <p class="note">Only the bright square shows behind the icons. Drag the picture or use the arrow keys.</p>
        </div>
      </div></div>`;
  document.body.append(ov);
  const c = $("#bge-canvas");
  let drag = null;
  c.addEventListener("pointerdown", e => { drag = { x: e.clientX, y: e.clientY, ox: bge.ox, oy: bge.oy }; c.setPointerCapture(e.pointerId); });
  c.addEventListener("pointermove", e => {
    if (!drag || !bge) return;
    const k = c.width / c.getBoundingClientRect().width;   /* the canvas is shown smaller on a phone */
    bge.ox = drag.ox + (e.clientX - drag.x) * k; bge.oy = drag.oy + (e.clientY - drag.y) * k;
    bgeClamp(); bgeDraw();
  });
  const end = () => { drag = null; };
  c.addEventListener("pointerup", end);
  c.addEventListener("pointercancel", end);
  c.addEventListener("wheel", e => {
    e.preventDefault();
    const r = c.getBoundingClientRect(), k = c.width / r.width;
    bgeZoom(bge.zoom * (e.deltaY < 0 ? 1.08 : 1 / 1.08), (e.clientX - r.left) * k - bge.M, (e.clientY - r.top) * k - bge.M);
  }, { passive: false });
  c.addEventListener("keydown", e => {
    const step = e.shiftKey ? 40 : 10, moves = { ArrowLeft: [step, 0], ArrowRight: [-step, 0], ArrowUp: [0, step], ArrowDown: [0, -step] };
    if (moves[e.key]) { bge.ox += moves[e.key][0]; bge.oy += moves[e.key][1]; bgeClamp(); bgeDraw(); e.preventDefault(); }
    else if (e.key === "+" || e.key === "=") { bgeZoom(bge.zoom * 1.1); e.preventDefault(); }
    else if (e.key === "-" || e.key === "_") { bgeZoom(bge.zoom / 1.1); e.preventDefault(); }
  });
  $("#bge-zoom").addEventListener("input", e => bgeZoom(+e.target.value / 100));
  const show = $("#bge-icon");
  if (show) show.addEventListener("change", e => { bge.showIcon = e.target.checked; bgeDraw(); });
  ov.addEventListener("click", e => { if (e.target === ov) bgeClose(); });
  bgeDraw();
  c.focus();
}
function bgeClose() {
  const ov = $("#bge");
  if (ov) ov.remove();
  bge = null;
}
/* The picture always covers the square. */
function bgeClamp() {
  const w = bge.iw * bge.s, h = bge.ih * bge.s;
  bge.ox = Math.min(0, Math.max(bge.F - w, bge.ox));
  bge.oy = Math.min(0, Math.max(bge.F - h, bge.oy));
}
/* Zoom to `z` (1 = just fills the square), keeping the point (px, py) of the square where it is. */
function bgeZoom(z, px = bge.F / 2, py = bge.F / 2) {
  z = Math.max(1, Math.min(4, z));
  const s = bge.fill * z, k = s / bge.s;
  bge.ox = px - (px - bge.ox) * k; bge.oy = py - (py - bge.oy) * k;
  bge.zoom = z; bge.s = s;
  bgeClamp();
  const r = $("#bge-zoom"), l = $("#bge-zoom-label");
  if (r) r.value = String(Math.round(z * 100));
  if (l) l.textContent = `${Math.round(z * 100)}%`;
  bgeDraw();
}
/* The square as it is now, at `side` pixels. */
function bgeSquare(side) {
  const c = document.createElement("canvas");
  c.width = c.height = side;
  const x = c.getContext("2d"), k = side / bge.F;
  x.imageSmoothingQuality = "high";
  x.drawImage(bge.img, bge.ox * k, bge.oy * k, bge.iw * bge.s * k, bge.ih * bge.s * k);
  return c;
}
function bgeDraw() {
  const c = $("#bge-canvas");
  if (!c || !bge) return;
  const x = c.getContext("2d"), { M, F } = bge;
  x.fillStyle = "#0b0f18"; x.fillRect(0, 0, c.width, c.height);
  x.imageSmoothingQuality = "high";
  x.drawImage(bge.img, M + bge.ox, M + bge.oy, bge.iw * bge.s, bge.ih * bge.s);
  x.save();
  x.fillStyle = "rgba(6, 9, 17, .7)";   /* what the icons leave out: the page's own colour, dimmed over it */
  x.beginPath(); x.rect(0, 0, c.width, c.height); x.rect(M, M, F, F); x.fill("evenodd");
  x.restore();
  if (bge.showIcon && bge.icon && bge.st.shape) {
    x.save(); x.translate(M, M); x.beginPath(); x.rect(0, 0, F, F); x.clip();
    drawIconInShape(x, F, F, bge.icon, bge.st);
    x.restore();
  }
  x.strokeStyle = "#fff"; x.lineWidth = 2; x.strokeRect(M - 1, M - 1, F + 2, F + 2);
  const sq = bgeSquare(256);
  for (const pv of document.querySelectorAll("canvas[data-bge-pv]")) {
    const n = +pv.dataset.bgePv, px = pv.getContext("2d");
    if (bge.icon) drawStyledIcon(px, n, n, bge.icon, bge.st, sq);
    else px.drawImage(sq, 0, 0, n, n);
  }
}
async function bgeSave() {
  const out = bgeSquare(BGE_OUT);
  const blob = await new Promise(r => out.toBlob(r, "image/jpeg", 0.92));
  await uploadVerified("/api/theme/preview", { id: state.editId, kind: "iconbg" }, blob);
  stamp = Date.now();
  await setIconStyle(x => { x.behind = "picture"; x.picAt = Date.now(); });
  bgeClose();
  await refreshStatus();
  renderAspect(aspectById("titles"));
  toast(styledTitles().size ? "Picture kept. Style the icons again to put it behind them." : "Picture kept. Style the icons to put it behind them.", "ok");
}

async function iconBackdropKept() {
  try { return (await fetch(`/api/theme/preview?${q({ id: state.editId, kind: "iconbg" })}`, { method: "GET" })).ok; } catch (e) { return false; }
}
/* Games and apps installed since the style was last used: styled as the theme opens, if it is to be, and as the
   console says one was installed. One run at a time. */
let autoRun = null;
function autoStyleIcons() {
  if (!autoRun) autoRun = autoStyleIconsNow().catch(e => toast(`New icons could not be styled: ${e.message}`, "err")).finally(() => { autoRun = null; });
  return autoRun;
}
async function autoStyleIconsNow() {
  const st = iconStyle();
  if (!st.shape || !st.titles.length || state.route !== "edit") return;
  /* The console's copy of the template, kept again when it isn't whole (an upload that stopped), lacks the list
     of the titles the style made, or its on/off differs: only for the style the icons were made with, so later
     installs match them. */
  const kept = styleMadeByPlugin.id === state.editId ? styleMadeByPlugin : null;
  if (kept && "template" in kept && st.ranAs === styleSig(st) &&
      (!kept.template || kept.auto !== !!st.auto || kept.styledList === false)) {
    try { await keepStyleTemplate(st); await loadPluginStyled(); }
    catch (e) { toast(`The style for games installed later could not be kept: ${e.message}`, "err"); }
  }
  await adoptStyledIcons();
  if (!st.auto) return;
  const fresh = styleTargets(true);   /* (none still installing) */
  if (!fresh.length) return;
  /* Styled by the page on its own, as the plugin does: kept and applied again when the theme is the one applied,
     unless you have changes of your own waiting (applying would put them on the console too, unasked). */
  const id = state.editId, t = themeById(id);
  const yours = !!(t && t.unsaved), applied = state.status && state.status.applied === id;
  titleNamesEnsure();
  if (titleNamesLoading) await titleNamesLoading.catch(() => {});
  toast(`Installed since this theme's icon style was used: ${namesOf(fresh.map(g => g.title))}. Styling ${fresh.length === 1 ? "its icon" : "their icons"}.`, "", 6000);
  const done = await styleIcons(true, applied && !yours ? "Applying the theme again to show them…" : null);
  if (!done.length || state.editId !== id || yours) return;
  try { await api("POST", "/api/theme/save", { id }); } catch (e) { return toast(`The styled icons are in the theme but not saved: ${e.message}`, "err"); }
  await refreshStatus();
  if (applied && state.status.applied === id && !state.status.applying) await applyTheme(id, done);   /* said once on the console */
}
