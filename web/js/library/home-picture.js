"use strict";

/* ---- The picture on a theme's card in the library: its home screen, drawn here and kept with the theme (kind
   "home"). Drawn again a moment after each change to the theme from this page, and for a theme that has none. */
const homeDirty = new Set(), homeTried = new Set(), homeDrawnAt = new Map();
let homeTimer = 0, homeDrawing = false;
function homePictureSoon(id) {
  if (!id) return;
  homeDirty.add(id);
  clearTimeout(homeTimer);
  homeTimer = setTimeout(homeDrawQueued, 1200);
}
/* A change to a theme this page made (api() and uploadVerified report each): its picture is drawn again. */
function homeTouched(method, path, params) {
  if (method === "GET" || !params) return;
  if (path === "/api/theme/preview") { if (params.kind === "background") homePictureSoon(params.id); }
  else if (/^\/api\/theme\/(file|save|discard)$/.test(path) || path === "/api/themes/mix") homePictureSoon(params.id);
  else if (path === "/api/themes/duplicate") homePictureSoon(params.new_id);
}
async function homePicture(id) {
  let files;
  try { files = new Set((await api("GET", "/api/theme/files", { id })).files.map(f => f.path)); } catch (e) { return false; }
  if (!files.size) {   /* nothing in it: its card says so, rather than show the console as it is */
    const t = themeById(id);
    if (!t || !t.homeTime) return false;
    await api("DELETE", "/api/theme/preview", { id, kind: "home" }).catch(() => {});
    return true;
  }
  await loadAssets();
  const rows = await homeRows(), view = homeCardView(rows, files, homeIndex());
  const scene = await homeScene(id, files, { ...view, look: homeLookOf(files) });
  const c = document.createElement("canvas");
  c.width = PREVIEW_W; c.height = PREVIEW_H;
  const g = c.getContext("2d");
  g.imageSmoothingQuality = "high";
  g.scale(PREVIEW_W / 1920, PREVIEW_H / 1080);
  homeScreenDraw(g, scene);
  const blob = await new Promise(r => c.toBlob(r, "image/jpeg", 0.85));
  if (!blob || !themeById(id)) return false;
  const bytes = new Uint8Array(await blob.arrayBuffer()), crc = hex8(crc32(bytes));
  const r = await api("PUT", "/api/theme/preview", { id, kind: "home", crc }, bytes);
  return r.crc32 === crc && r.size === bytes.length;
}
async function homeDrawQueued() {
  if (homeDrawing) return;
  if (writing > 0) { homeTimer = setTimeout(homeDrawQueued, 1500); return; }   /* (after the uploads under way) */
  homeDrawing = true;
  try {
    while (homeDirty.size) {
      const id = homeDirty.values().next().value;
      homeDirty.delete(id);
      let ok = false;
      try { ok = await homePicture(id); } catch (e) { console.warn(`home picture for ${id}:`, e); }
      if (!ok) continue;
      homeDrawnAt.set(id, Date.now());
      await refreshStatus();
      const t = themeById(id), el = t && document.querySelector(`[data-cover="${CSS.escape(id)}"]`);
      if (el) el.outerHTML = cardPreview(t);
      if (t && state.route === "edit" && state.editId === id) {
        if ($("#cover-panel")) $("#cover-panel").innerHTML = coverPanel(t);
        syncNav("theme");
      }
    }
  } finally { homeDrawing = false; }
}
/* In the library: themes with no home picture yet (made before 1.89, or imported), or one from another day (the
   console's games have moved on), get one, once per visit. */
function fillHomePictures() {
  const day = Date.now() / 1000 - 86400;
  for (const t of state.status.themes) {
    if (!t.files || homeTried.has(t.id) || (t.homeTime && t.homeTime > day)) continue;
    homeTried.add(t.id);
    homePictureSoon(t.id);
  }
}
