"use strict";

async function zlibDeflate(data) {
  if (typeof CompressionStream === "undefined") return zlibStored(data);
  const stream = new Blob([data]).stream().pipeThrough(new CompressionStream("deflate"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/* The console's own resource files, read once per page (they are large, up to 40 MB). Parts of the page waiting
   for the same file share one download, and its loading markers follow it. */
const rcoReading = new Map();
function rcoStock(path) {
  state.rco = state.rco || new Map();
  if (state.rco.has(path)) return Promise.resolve(state.rco.get(path));
  if (!rcoReading.has(path)) {
    const key = `stock:${path}`;
    rcoReading.set(path, (async () => {
      loadProgress(key, 0);
      let res;
      try { res = await fetch(`/api/asset?${q({ path })}`); } catch (e) { throw new Error("the console could not be reached"); }
      if (!res.ok) throw new Error(`cannot read ${baseName(path)} from the console`);
      const bytes = await readBytes(res, f => loadProgress(key, f));
      let parsed = null;
      try { parsed = rcoParse(bytes); } catch (e) { /* another kind of .rco: left out */ }
      state.rco.set(path, parsed);
      return parsed;
    })().catch(e => { loads.delete(key); throw e; }).finally(() => rcoReading.delete(path)));
  }
  return rcoReading.get(path);
}

/* The interface file that holds the colour palettes and the shape list (on 13.42), read before the others: the
   colours and the shapes show as soon as it is in, and the other files are read for them only if it has none. */
const RCO_MAIN = "Sce.PlayStation.PUI_UI3.rco";
function rcoPaths() {
  return aspectAssets(aspectById("interface")).map(a => a.path)
    .sort((a, b) => (baseName(b) === RCO_MAIN) - (baseName(a) === RCO_MAIN) || a.localeCompare(b));
}

/* Read every resource file once, then show the section with what each holds (files of another kind are left
   out). A file that could not be read says so on its row, with Try again; it is not taken for another kind. */
let rcoLoading = null;
const rcoFailed = new Map();   /* path -> why it could not be read */
function rcoEnsureLoaded() {
  const paths = rcoPaths();
  if (rcoLoading || paths.every(p => (state.rco && state.rco.has(p)) || rcoFailed.has(p))) return;
  rcoLoading = (async () => {
    for (const p of paths) {
      if (rcoFailed.has(p)) continue;
      await rcoStock(p).catch(e => rcoFailed.set(p, e.message));
    }
  })().finally(() => { rcoLoading = null; renderAspect(aspectById("interface")); });
}
function rcoRetry() {
  rcoFailed.clear();
  renderAspect(aspectById("interface"));
  if (aspectOn(aspectById("appearance"))) renderAspect(aspectById("appearance"));
}

/* Every picture in every resource file, numbered from 1 in a fixed order
   (files by name, pictures in file order): the key to the numbered test. */
async function rcoKey(onFile) {
  const paths = aspectAssets(aspectById("interface")).map(a => a.path).sort();
  const key = [];
  for (let i = 0; i < paths.length; i++) {
    if (onFile) onFile(i, paths.length, paths[i]);
    const r = await rcoStock(paths[i]);
    if (r) for (const pic of rcoPictures(r)) key.push({ n: key.length + 1, path: paths[i], r, pic });
  }
  return key;
}

/* Number the pictures of one resource file (`vectorOnly`: only its SVG
   icons). The numbers come from the key, so they are the same whichever
   file is numbered first. */
function numberPictures(path, vectorOnly) { return rcoQueued("Numbering the pictures", () => numberPicturesNow(path, vectorOnly), true); }
async function numberPicturesNow(path, vectorOnly) {
  const p = progressToast("Reading the console's interface files…");
  try {
    const key = (await rcoKey((i, n, x) => p.set(`Reading ${baseName(x)}…`, i / n * 0.3))).filter(k => k.path === path);
    if (!key.length) throw new Error("this file has no pictures");
    /* Numbering replaces the theme's pictures in the file, but keeps its shapes. */
    const sh = rcoShapes(key[0].r), shapes = sh && (await rcoChanges(path)).get(`${sh.f.el}\t${sh.f.attr}`);
    const replace = new Map(shapes ? [[`${sh.f.el}\t${sh.f.attr}`, shapes]] : []);
    let done = 0, skipped = 0;
    for (const k of key) {
      if (vectorOnly && k.pic.kind !== "svg") continue;
      let ok = false;
      for (const f of k.pic.files) {
        let bytes = null;
        if (f.kind === "png") bytes = await numberPicture(k.n, rcoData(k.r, f), zlibDeflate);
        else if (f.kind === "svg") {
          const text = numberSvg(k.n, new TextDecoder().decode(rcoData(k.r, f)));
          bytes = text === null ? null : new TextEncoder().encode(text);
        }
        if (bytes) { replace.set(`${f.el}\t${f.attr}`, bytes); ok = true; }
      }
      if (ok) done++; else skipped++;
      if ((done + skipped) % 50 === 0) p.set(`Numbering pictures… ${done + skipped} of ${key.length}`, 0.3 + 0.4 * (done + skipped) / key.length);
    }
    p.set(`Sending ${baseName(path)} to the console…`, 0.75);
    await uploadVerified("/api/theme/file", { id: state.editId, path }, new Blob([rcoRebuild(key[0].r, replace)]));
    state.files.add(path);
    stamp = Date.now();
    p.done(`${plural(done, "picture")} numbered in ${baseName(path)} and verified${skipped ? ` (${skipped} left as they are, in a format this page can't write)` : ""}. ` +
      "Apply the theme, then put the console in rest mode and wake it.", "ok");
  } catch (e) {
    p.done(`Numbering failed: ${e.message}`, "err");
  }
  await refreshStatus();
  renderAspect(aspectById("interface"));
}
