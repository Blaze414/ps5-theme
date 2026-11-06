"use strict";

/* ---------------------------------------------------------------- your own pictures */

/* Your image as RGBA pixels at w x h (fitted as chosen above the sections),
   transparent where it doesn't reach. */
async function imageRgba(file, w, h) {
  const url = URL.createObjectURL(file);
  let img;
  try { img = await loadImage(url); } finally { URL.revokeObjectURL(url); }
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const g = c.getContext("2d", { colorSpace: "srgb", willReadFrequently: true });
  g.imageSmoothingQuality = "high";
  let dw = w, dh = h;
  if (state.fit !== "stretch") {
    const sc = (state.fit === "cover" ? Math.max : Math.min)(w / img.naturalWidth, h / img.naturalHeight);
    dw = img.naturalWidth * sc; dh = img.naturalHeight * sc;
  }
  g.drawImage(img, (w - dw) / 2, (h - dh) / 2, dw, dh);
  return new Uint8Array(g.getImageData(0, 0, w, h).data.buffer);
}

/* Your image for one console picture, in exactly its format: a PNG written
   like the console's (pngFormat), or the console's SVG with your image in
   it as a PNG at twice the SVG's size (at most 512 pixels). */
async function pictureFromImage(file, stock, kind) {
  if (kind === "png") {
    const f = pngFormat(stock);
    if (!f || !f.writable) throw new Error("this picture's format can't be written");
    return encodePng(await imageRgba(file, f.w, f.h), f, zlibDeflate);
  }
  const text = new TextDecoder().decode(stock), end = svgRootEnd(text);
  if (!end) throw new Error("this picture can't be replaced");
  const [, , vw, vh] = svgBox(text.slice(text.lastIndexOf("<svg", end), end));
  const scale = Math.min(512 / Math.max(vw, vh), 2 * Math.max(1, 128 / Math.max(vw, vh)));
  const w = Math.max(1, Math.round(vw * scale)), h = Math.max(1, Math.round(vh * scale));
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(await imageRgba(file, w, h)), w, h), 0, 0);
  return new TextEncoder().encode(svgWithImage(text, c.toDataURL("image/png")));
}

/* The theme's changes to a resource file: entry ("element\tattribute") ->
   bytes, for every embedded picture that differs from the console's. */
/* The theme's copy of each resource file, parsed: one per file (a resource file is up to 40 MB; keeping one per
   change would fill the browser's memory, the console's own browser first). */
const rcoThemeCache = new Map();
function rcoThemeCacheSet(key, value) {
  const file = key.slice(0, key.lastIndexOf("|") + 1);
  for (const k of [...rcoThemeCache.keys()]) if (k.slice(0, k.lastIndexOf("|") + 1) === file) rcoThemeCache.delete(k);
  rcoThemeCache.set(key, value);
}
/* Whether the theme's copy of a file is at hand (rcoChanges then needs no download). */
const rcoThemeReady = path => !state.files.has(path) || rcoThemeCache.has(`${state.editId}|${path}|${stamp}`);
const rcoThemeReading = new Map();
async function rcoChanges(path) {
  const r = await rcoStock(path), out = new Map();
  if (!r || !state.files.has(path)) return out;
  const cacheKey = `${state.editId}|${path}|${stamp}`;
  let t;
  if (rcoThemeCache.has(cacheKey)) t = rcoThemeCache.get(cacheKey);
  else {
    if (!rcoThemeReading.has(cacheKey)) {
      const key = `theme:${path}`, url = `/api/theme/file?${q({ id: state.editId, path })}&v=${stamp}`;
      rcoThemeReading.set(cacheKey, (async () => {
        loadProgress(key, 0);
        let res;
        try { res = await fetch(url); } catch (e) { throw new Error("the console could not be reached"); }
        const parsed = res.ok ? rcoParse(await readBytes(res, f => loadProgress(key, f))) : null;
        rcoThemeCacheSet(cacheKey, parsed);
        return parsed;
      })().finally(() => rcoThemeReading.delete(cacheKey)));
    }
    /* the file as read, even if a newer one has replaced it in the cache since */
    t = await rcoThemeReading.get(cacheKey);
  }
  if (!t) return out;
  const theirs = new Map(t.files.map(f => [`${f.el}\t${f.attr}`, f]));
  for (const f of r.files) {
    const k = `${f.el}\t${f.attr}`, o = theirs.get(k);
    if (!o) continue;
    const a = rcoData(r, f), b = rcoData(t, o);
    if (a.length !== b.length || a.some((v, i) => v !== b[i])) out.set(k, b);
  }
  return out;
}

async function rcoSave(path, changes) {
  let made = null;
  if (!changes.size) await resetAssets([{ path }]);
  else {
    made = rcoRebuild(await rcoStock(path), changes);
    await uploadVerified("/api/theme/file", { id: state.editId, path }, new Blob([made]));
    state.files.add(path);
  }
  stamp = Date.now();
  /* the file just sent is the theme's now: the next change starts from it without fetching it again */
  if (made) rcoThemeCacheSet(`${state.editId}|${path}|${stamp}`, rcoParse(made));
  await refreshStatus();
}

/* Changes to a resource file, one at a time: each reads the theme's file as the change before it left it (a
   change started meanwhile would otherwise start from the old file and undo the other when it is sent). */
let rcoWork = Promise.resolve(), rcoWaiting = 0;
function rcoQueued(label, fn, ownProgress = false) {
  const waiting = rcoWaiting++ > 0;
  const p = waiting || !ownProgress ? progressToast(waiting ? `${label}: waiting for the change before it…` : `${label}…`) : null;
  const run = rcoWork.then(async () => {
    if (p && ownProgress) { p.done("", ""); }
    else if (p) p.set(`${label}…`, 0.3);
    try { return await fn(); } finally { rcoWaiting--; if (p && !ownProgress) p.done("", ""); }
  });
  rcoWork = run.catch(() => {});
  return run;
}

function replacePicture(path, el, file) { return rcoQueued("Adding your picture", () => replacePictureNow(path, el, file), true); }
async function replacePictureNow(path, el, file) {
  const r = await rcoStock(path), pic = rcoPictures(r).find(p => p.el === el);
  if (!pic) return;
  const p = progressToast(`Adding your picture as ${pic.id}…`);
  try {
    const changes = await rcoChanges(path);
    for (const f of pic.files) {
      if (!f.kind) throw new Error("this picture can't be replaced");
      changes.set(`${f.el}\t${f.attr}`, await pictureFromImage(file, rcoData(r, f), f.kind));
    }
    p.set(`Sending ${baseName(path)} to the console…`, 0.6);
    await rcoSave(path, changes);
    p.done(`${pic.id} replaced and verified${pic.kind === "svg" ? " (a vector icon: your picture is drawn inside the console's own SVG)" : ""}. Apply the theme, then put the console in rest mode and wake it.`, "ok");
  } catch (e) {
    p.done(`${pic.id}: ${e.message}`, "err");
  }
  renderAspect(aspectById("interface"));
}

function resetPicture(path, el) { return rcoQueued("Putting the console's picture back", () => resetPictureNow(path, el)); }
async function resetPictureNow(path, el) {
  const r = await rcoStock(path), pic = rcoPictures(r).find(p => p.el === el);
  const changes = await rcoChanges(path);
  for (const f of pic.files) changes.delete(`${f.el}\t${f.attr}`);
  try { await rcoSave(path, changes); toast(`${pic.id} is back to the console's own.`, "ok"); }
  catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("interface"));
}
