"use strict";

/* The console's textures with see-through parts carry colour in their clear pixels too (so the edges blend
   without dark fringes): the clear pixels take the colour of the nearest visible ones, up to 8 pixels away,
   and white beyond, as the console's own do. Alpha stays as it is. */
function bleedClear(pix, w, h) {
  const SEEN = 8;
  let todo = [];
  const known = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) { if (pix[i * 4 + 3] >= SEEN) known[i] = 1; else todo.push(i); }
  for (let pass = 0; pass < 8 && todo.length; pass++) {
    const fill = [], next = [];
    for (const i of todo) {
      const x = i % w, y = (i - x) / w;
      let r = 0, g = 0, b = 0, n = 0;
      for (const j of [x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1]) {
        if (j >= 0 && known[j]) { r += pix[j * 4]; g += pix[j * 4 + 1]; b += pix[j * 4 + 2]; n++; }
      }
      if (n) fill.push([i, Math.round(r / n), Math.round(g / n), Math.round(b / n)]); else next.push(i);
    }
    for (const [i, r, g, b] of fill) { pix[i * 4] = r; pix[i * 4 + 1] = g; pix[i * 4 + 2] = b; known[i] = 1; }
    todo = next;
  }
  for (const i of todo) { pix[i * 4] = 255; pix[i * 4 + 1] = 255; pix[i * 4 + 2] = 255; }
}

/* Where a see-through texture meets the screen's edge: the Control Center's bottom panel at the bottom, its
   top-right piece at the top and right, the home screen's artwork at the top. A picture is placed
   against that edge (so a crop takes the part away from it), and can be pulled in from it by a padding, in
   percent of the texture's size, left clear. */
function textureAnchor(path) {
  if (/_bottom\.dds$/.test(path)) return { v: "bottom", h: "center", top: 0, right: 0, bottom: 1 };
  /* (the clock and the search and settings icons are drawn over the top 26% of this piece; on the PlayStation 2
     theme the glass bar behind the clock spans 7% to 22% of it, and its glow reaches 35%) */
  if (/_top\.dds$/.test(path)) return { v: "top", h: "right", top: 1, right: 1, bottom: 0, band: 0.26 };
  /* the home screen's artwork hangs from the top of the screen, across its width */
  if (/\/homeui_[^/]*\.dds$/.test(path)) return { v: "top", h: "center", top: 1, right: 0, bottom: 0 };
  return { v: "center", h: "center", top: 0, right: 0, bottom: 0 };
}
const PAD_DEFAULT = 6;
/* The choice made for a piece: "fill" (crop to the texture, against its edge), "pad" (the same, with the padding
   left clear at the screen's edge), "fit" (the whole picture) or "stretch". How each is drawn, and its padding. */
function placementParams(mode, pad) {
  if (mode === "pad") return { fit: "cover", pad: Math.max(0, Math.min(40, +pad || 0)) };
  if (mode === "fit") return { fit: "contain", pad: 0 };
  if (mode === "stretch") return { fit: "stretch", pad: 0 };
  return { fit: "cover", pad: 0 };
}
/* Where the picture goes in a w x h texture: its rectangle x, y, dw, dh, and the clear-margin rectangle to clip to. */
function textureDraw(path, pct, fit, w, h, iw, ih, keepClear) {
  const a = textureAnchor(path), p = Math.max(0, Math.min(40, +pct || 0)) / 100;
  let t = a.top ? Math.round(h * p) : 0;
  const b = a.bottom ? Math.round(h * p) : 0, r = a.right ? Math.round(w * p) : 0;
  if (keepClear && a.band) t = Math.max(t, Math.round(h * a.band));   /* the band the clock and icons are drawn over stays clear */
  const box = { x: 0, y: t, w: Math.max(1, w - r), h: Math.max(1, h - t - b) };
  let dw = box.w, dh = box.h;
  if (fit !== "stretch") {
    const sc = (fit === "cover" ? Math.max : Math.min)(box.w / iw, box.h / ih);
    dw = iw * sc; dh = ih * sc;
  }
  const x = box.x + (a.h === "right" ? box.w - dw : (box.w - dw) / 2);
  const y = box.y + (a.v === "bottom" ? box.h - dh : a.v === "top" ? 0 : (box.h - dh) / 2);
  return { x, y, dw, dh, box };
}
