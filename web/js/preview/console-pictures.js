"use strict";

/* ---- The console's own pictures, for the previews ----

   Where a theme leaves something as it is, the previews draw the console's own: its default home background and the
   Appearance themes' artwork (textures the browser can't read, decoded by the plugin to a quarter-size picture,
   /api/asset/picture), the system tiles' icons, its interface icons (/api/ui-icon) and its SST font. All read from
   the console as the page runs; nothing of Sony's is in this page. */
const consolePics = new Map();   /* asset path -> a promise of an object URL, or null if it can't be drawn */
function consoleTexture(path) {
  if (!path) return Promise.resolve(null);
  if (!consolePics.has(path)) consolePics.set(path, (async () => {
    try {
      const res = await fetch(`/api/asset/picture?${q({ path })}`);
      if (!res.ok) return null;
      const b = new Uint8Array(await res.arrayBuffer()), dv = new DataView(b.buffer);
      const w = dv.getUint32(0, true), h = dv.getUint32(4, true);
      if (!w || !h || b.length !== 8 + w * h * 4) return null;
      const c = document.createElement("canvas");
      c.width = w; c.height = h;
      c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(b.buffer, 8, w * h * 4), w, h), 0, 0);
      return await new Promise(r => c.toBlob(x => r(x ? URL.createObjectURL(x) : null), "image/png"));
    } catch (e) { return null; }
  })());
  return consolePics.get(path);
}
const uiIconUrl = id => `/api/ui-icon?${q({ id })}`;
/* The console's interface font, loaded once under its own name. */
let consoleFontLoading = null;
function consoleFont() {
  if (consoleFontLoading) return consoleFontLoading;
  const a = (state.assets || []).find(x => /^SST-(Roman|Regular|Light)\.otf$/i.test(baseName(x.path)));
  if (!a || typeof FontFace !== "function") return (consoleFontLoading = Promise.resolve(null));
  return (consoleFontLoading = new FontFace("console-sst", `url("/api/asset?${q({ path: a.path })}")`).load()
    .then(f => { document.fonts.add(f); return "console-sst"; }).catch(() => null));
}
