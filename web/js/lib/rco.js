"use strict";

/* ---- Interface resource files

   The console keeps its interface pictures (icons, buttons, symbols) in resource files in /system_ex/vsh_asset
   (RCOF 0x110): a header with nine tables, an element tree whose type-8 attributes point into the file table at the
   end, and the files themselves. A theme may swap pictures and sounds and change colours and shapes; the console
   refuses anything else that differs from its own file. Like every file in js/lib, this runs in the tests too
   (no DOM). */
const RCO_HEADER = 0x50, RCO_FILE_TABLE = 8;

function rcoParse(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength), u32 = o => dv.getUint32(o, true);
  if (b.length < RCO_HEADER || b[0] !== 0x52 || b[1] !== 0x43 || b[2] !== 0x4f || b[3] !== 0x46 || u32(4) !== 0x110) {
    throw new Error("not an interface resource file");
  }
  const t = [];
  for (let i = 0; i < 9; i++) {
    const off = u32(8 + 8 * i), size = u32(12 + 8 * i);
    if (off < RCO_HEADER || off + size > b.length) throw new Error("damaged resource file");
    t.push([off, size]);
  }
  const [fileOff, fileSize] = t[RCO_FILE_TABLE], [treeOff, treeSize] = t[0];
  if (fileOff + fileSize !== b.length || treeOff !== RCO_HEADER) throw new Error("damaged resource file");
  const dec = new TextDecoder();
  const str = (tab, o) => {
    const [base, size] = t[tab];
    if (o >= size) return "";
    let e = base + o;
    while (e < base + size && b[e]) e++;
    return e < base + size ? dec.decode(b.subarray(base + o, e)) : "";
  };
  const files = [];
  for (let at = 0, el = 0; at < treeSize; el++) {
    if (treeSize - at < 28) throw new Error("damaged resource file");
    const nattr = u32(treeOff + at + 4);
    if (nattr > (treeSize - at - 28) / 16) throw new Error("damaged resource file");
    const elem = str(3, u32(treeOff + at));
    let id = "", type = "";
    const own = [];
    for (let i = 0; i < nattr; i++) {
      const a = treeOff + at + 28 + 16 * i, name = str(3, u32(a)), kind = u32(a + 4);
      if ((kind === 9 || kind === 10) && name === "id") id = str(1, u32(a + 8) + 4);
      else if (kind === 3 && name === "type") type = str(3, u32(a + 8));
      else if (kind === 8) {
        const off = u32(a + 8), size = u32(a + 12);
        if (off + size > fileSize) throw new Error("damaged resource file");
        own.push({ attr: name, slot: a + 8, off, size });
      }
    }
    for (const f of own) files.push({ el, elem, id, type, ...f });
    at += 28 + 16 * nattr;
  }
  return { b, fileOff, fileSize, files };
}

const rcoData = (r, f) => r.b.subarray(r.fileOff + f.off, r.fileOff + f.off + f.size);

/* What a picture really is, from its bytes: "png", "svg" or "" (some
   entries declared as SVG hold PNG data). */
function pictureKind(d) {
  if (d.length >= 8 && d[0] === 0x89 && d[1] === 0x50 && d[2] === 0x4e && d[3] === 0x47) return "png";
  return svgRootEnd(new TextDecoder().decode(d.subarray(0, 4096))) ? "svg" : "";
}

/* The pictures of a resource file, one entry per picture (with its size
   variants, such as src and src_4k), each with what it really is. */
function rcoPictures(r) {
  const out = new Map();
  for (const f of r.files) {
    if (f.elem !== "texture" || (f.type !== "texture/png" && f.type !== "texture/svg")) continue;
    if (!out.has(f.el)) out.set(f.el, { el: f.el, id: f.id, type: f.type, files: [] });
    out.get(f.el).files.push({ ...f, kind: pictureKind(rcoData(r, f)) });
  }
  return [...out.values()].map(p => ({ ...p, kind: p.files.every(f => f.kind === "svg") ? "svg" : p.files.every(f => f.kind === "png") ? "png" : "" }));
}

/* The interface's shape list (an entry of type "shape", shape_default on
   13.42): corner radii by component, {"button.fill": 1.0, ...,
   "borderRadiusRatio": 1.0}. Its entry, text and [name, number] pairs in
   order, or null. */
const SHAPE_PAIR = /("([^"\\]+)"\s*:\s*)(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)/g;
function rcoShapes(r) {
  const f = r.files.find(x => x.elem === "file" && x.type === "shape");
  if (!f) return null;
  const text = new TextDecoder().decode(rcoData(r, f));
  return { f, text, values: [...text.matchAll(SHAPE_PAIR)].map(m => [m[2], Number(m[3])]) };
}

/* A shape number as the console writes them: 3 -> "3.0", 2.5 -> "2.5". */
const shapeNumber = v => Number.isInteger(v) ? v.toFixed(1) : String(Math.round(v * 100) / 100);

/* The console's shape list with new numbers, each written where the old one
   stands, so everything else stays byte for byte. */
function shapesText(text, values) {
  const map = new Map(values);
  return text.replace(SHAPE_PAIR, (all, head, name, old) => map.has(name) && map.get(name) !== Number(old) ? head + shapeNumber(map.get(name)) : all);
}

/* The interface's colour palettes (entries of type "color": color_default,
   and color_ps1 to color_ps4 for the PlayStation 1 to 4 Appearance themes),
   {"text.primary.enabled": 4294967295, ...}: each colour a whole number,
   0xAARRGGBB. Their entries, texts and [name, colour] pairs in order. */
const COLOR_PAIR = /("([^"\\]+)"\s*:\s*)(\d+)/g;
function rcoColors(r) {
  return r.files.filter(f => f.elem === "file" && f.type === "color").map(f => {
    const text = new TextDecoder().decode(rcoData(r, f));
    return { f, id: f.id, text, values: [...text.matchAll(COLOR_PAIR)].map(m => [m[2], Number(m[3])]) };
  });
}

/* Each palette's Appearance theme, as Settings names it. */
const PALETTE_NAMES = { color_default: "Default look", color_ps1: "PlayStation", color_ps2: "PlayStation 2", color_ps3: "PlayStation 3",
                        color_ps4: "PlayStation 4" };
const paletteRank = id => Object.keys(PALETTE_NAMES).indexOf(id) >>> 0;

/* A palette with new colours, each written where the old one stands. */
function colorsText(text, values) {
  const map = new Map(values);
  return text.replace(COLOR_PAIR, (all, head, name, old) =>
    map.has(name) && map.get(name) !== Number(old) ? head + String(map.get(name) >>> 0) : all);
}

/* The colour test: each kind of colour in one bright colour of its own, its
   transparency kept, in every palette, so a photo of the console shows what
   each kind colours. The first group a name fits is its group; "varies" is
   true for a name whose colour differs between the palettes (the Appearance
   theme's own colour). Names in no group keep the console's colour. */
const COLOR_TEST = [
  { label: "Focus", rgb: 0x00e5ff, fits: n => /^focus\./.test(n) },
  { label: "Text and labels (Settings and the system's menus)", rgb: 0xff1a1a, fits: n => /^text\.|label|^dialog\.(header|body)/.test(n) },
  { label: "Icons", rgb: 0xff8c00, fits: n => /icon/.test(n) },
  { label: "Base mat (the backdrop behind the Control Center)", rgb: 0xff00ff, fits: n => /^base-mat\./.test(n) },
  { label: "Background", rgb: 0x1f4bff, fits: n => /^background\./.test(n) },
  { label: "Panels (the Control Center's cards, button hints, menus, dialogs)", rgb: 0x00c853,
    fits: n => /^(dialog|menu|form)\.fill|\.dark\.fill|overlay\.fill|key-guide\.tooltip\.fill/.test(n) },
  { label: "Accent (the colours that change with the Appearance theme)", rgb: 0xffd600, fits: (n, varies) => varies },
];
/* The panel test: each panel and base mat colour in a distinct colour of its own, everything else as the console's,
   to tell which one colours the Welcome widgets, the pills, the Control Center's cards and the button hints. */
const COLOR_PANEL_TEST = [
  ["button.overlay.fill.enabled", 0xff0000, "red"], ["checkbox.overlay.fill.enabled", 0xff8000, "orange"],
  ["dialog.fill.enabled", 0xffee00, "yellow"], ["form.fill.selected.enabled", 0x80ff00, "lime"],
  ["key-guide.overlay.fill.enabled", 0x00a000, "green"], ["key-guide.tooltip.fill.enabled", 0x00ffff, "cyan"],
  ["menu.fill.disabled", 0x0040ff, "blue"], ["menu.fill.enabled", 0x8000ff, "purple"],
  ["radio-button.overlay.fill.enabled", 0xff00ff, "magenta"], ["tag.dark.fill.enabled", 0xff80c0, "pink"],
  ["tooltip.dark.fill.enabled", 0xffffff, "white"], ["base-mat.area.enabled", 0x8b4513, "brown"],
  ["base-mat.overlay.enabled", 0x008080, "teal"],
];
function colorPanelValues(palettes) {
  const map = new Map(COLOR_PANEL_TEST.map(([name, rgb]) => [name, rgb]));
  return palettes.map(p => p.values.map(([name, v]) => [name, map.has(name) ? ((v & 0xff000000) | map.get(name)) >>> 0 : v]));
}
/* The colour editor: the colours whose effect the tests showed on the console, each named by what it changes. A
   colour keeps the console's transparency. "text" is every light text and label colour (dark labels on light
   buttons stay readable); "accent" is the colour that changes with the Appearance theme, its darker and lighter
   shades following it in proportion. */
/* "shared": the console uses the default look's palette (color_default) for these with every Appearance theme
   selected (seen on the console with Use System Appearance for Widgets off), so they are edited there only. */
const COLOR_EDIT = [
  { key: "widgets", shared: true, help: "The Welcome hub's widget cards (Trophies, Console Storage, News and the others) and the pills above them. Shows with Use System Appearance for Widgets off.", label: "Welcome widgets and pills", hint: "turn off Use System Appearance for Widgets in the Welcome hub's settings to see it", names: ["menu.fill.enabled"] },
  { key: "backdrop", shared: true, help: "The backdrop that covers the screen behind the Control Center when you press the PS button. It also tints the Media Gallery card.", label: "Control Center backdrop", hint: "also tints the Media Gallery card", names: ["base-mat.overlay.enabled"] },
  { key: "card", shared: true, help: "The card at the left of the Control Center (Recently created).", label: "Control Center card", names: ["radio-button.overlay.fill.enabled"] },
  { key: "hints", shared: true, help: "The background of the button hints at the bottom right of the Welcome hub (Utilities, Edit Widgets).", label: "Button hints", hint: "behind Utilities and Edit Widgets", names: ["key-guide.overlay.fill.enabled"] },
  { key: "text", help: "The light text and labels in Settings and the system's menus: titles, menu items, values. Dark labels on light buttons stay as they are, so they stay readable.", label: "Text in Settings and menus", group: "text" },
  { key: "accent", help: "The colour of buttons, switches, sliders and progress bars. Its darker and lighter shades follow it in proportion.", label: "Accent", hint: "buttons, switches, sliders and progress bars", group: "accent" },
];
const rgbOf = v => (v >>> 0) & 0xffffff;
const lum = c => 0.299 * (c >> 16) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255);
/* For palette `i`: each editor colour's names, and the colour that stands for it (its "base"). */
function colorEditSets(palettes, i) {
  const values = new Map(palettes[i].values), sets = {};
  const varies = name => new Set(palettes.map(p => (p.values.find(v => v[0] === name) || [])[1])).size > 1;
  const groupOf = name => (COLOR_TEST.find(t => t.fits(name, varies(name))) || {}).label;
  for (const e of COLOR_EDIT) {
    let names;
    if (e.names) names = e.names.filter(n => values.has(n));
    else if (e.group === "text") names = [...values.keys()].filter(n => groupOf(n) === COLOR_TEST[1].label && rgbOf(values.get(n)) === 0xffffff);
    else names = [...values.keys()].filter(n => groupOf(n) === COLOR_TEST[COLOR_TEST.length - 1].label);
    if (!names.length) continue;
    const count = new Map();
    for (const n of names) count.set(rgbOf(values.get(n)), (count.get(rgbOf(values.get(n))) || 0) + 1);
    const base = [...count].sort((a, b) => b[1] - a[1])[0][0];
    sets[e.key] = { names, base };
  }
  return sets;
}
/* Palette `i` with the colours picked (`picks`: key -> 0xRRGGBB); a pick equal to the console's leaves its names
   as they are. */
function colorEditValues(palettes, i, picks) {
  const sets = colorEditSets(palettes, i), out = new Map(palettes[i].values);
  for (const [key, set] of Object.entries(sets)) {
    const pick = picks[key];
    if (pick === undefined || pick === set.base) continue;
    for (const n of set.names) {
      const v = out.get(n), rgb = rgbOf(v);
      let c = pick;
      if (rgb !== set.base) {       /* a shade of the accent: the same step from the new colour */
        const r = lum(set.base) ? Math.min(2, lum(rgb) / lum(set.base)) : 1;
        c = [16, 8, 0].reduce((acc, sh) => acc | (Math.max(0, Math.min(255, Math.round(((pick >> sh) & 255) * r))) << sh), 0);
      }
      out.set(n, ((v & 0xff000000) | c) >>> 0);
    }
  }
  return palettes[i].values.map(([n]) => [n, out.get(n)]);
}
function colorTestValues(palettes) {
  const varies = name => new Set(palettes.map(p => (p.values.find(v => v[0] === name) || [])[1])).size > 1;
  return palettes.map(p => p.values.map(([name, v]) => {
    const g = COLOR_TEST.find(t => t.fits(name, varies(name)));
    return [name, g ? ((v & 0xff000000) | g.rgb) >>> 0 : v];
  }));
}

/* A copy with some files replaced: `replace` maps "element\tattribute" to
   new bytes. Files keep their order, 16-byte aligned, and files the console
   shares between entries stay shared. */
function rcoRebuild(r, replace) {
  const own = f => `${f.el}\t${f.attr}`;
  const keyOf = f => replace.has(own(f)) ? own(f) : `${f.off}:${f.size}`;
  const where = new Map(), parts = [];
  let len = 0;
  const pad = () => { const n = (16 - len % 16) % 16; if (n) { parts.push(new Uint8Array(n)); len += n; } };
  for (const f of [...r.files].sort((x, y) => x.off - y.off || x.size - y.size)) {
    const k = keyOf(f);
    if (where.has(k)) continue;
    const data = replace.get(own(f)) || rcoData(r, f);
    pad();
    where.set(k, [len, data.length]);
    parts.push(data);
    len += data.length;
  }
  pad();
  const out = new Uint8Array(r.fileOff + len);
  out.set(r.b.subarray(0, r.fileOff));
  const dv = new DataView(out.buffer);
  for (const f of r.files) {
    const [o, size] = where.get(keyOf(f));
    dv.setUint32(f.slot, o, true);
    dv.setUint32(f.slot + 4, size, true);
  }
  dv.setUint32(12 + 8 * RCO_FILE_TABLE, len, true);
  let at = r.fileOff;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

/* A number drawn as seven-segment digits in a box, as rectangles (no fonts
   needed): rows of two digits, padded to an even count ("0123"). */
const SEGMENTS = ["abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg"];
function numberRects(n, w, h) {
  let digits = String(n);
  if (digits.length > 1 && digits.length % 2) digits = "0" + digits;
  const rows = [];
  for (let i = 0; i < digits.length; i += 2) rows.push(digits.slice(i, i + 2));
  const m = Math.min(w, h) * 0.08, cols = Math.max(...rows.map(x => x.length));
  const cw = (w - 2 * m) / cols, ch = (h - 2 * m) / rows.length, out = [];
  rows.forEach((row, ri) => [...row].forEach((d, ci) => {
    const dw = cw * 0.72, dh = ch * 0.84, t = Math.max(Math.min(dw, dh) * 0.2, 1);
    const x = m + ci * cw + (cw - dw) / 2, y = m + ri * ch + (ch - dh) / 2;
    const seg = { a: [x, y, dw, t], d: [x, y + dh - t, dw, t], g: [x, y + (dh - t) / 2, dw, t],
                  f: [x, y, t, dh / 2], b: [x + dw - t, y, t, dh / 2], e: [x, y + dh / 2, t, dh / 2], c: [x + dw - t, y + dh / 2, t, dh / 2] };
    for (const s of SEGMENTS[+d]) out.push(seg[s]);
  }));
  return out;
}

/* A numbered SVG made from the console's own: its beginning up to and
   including the <svg ...> start tag byte for byte (XML declaration, size,
   view box), then a pink square with the digits as rectangles, then its
   closing tag. null if it has no ordinary start tag. */
function numberSvg(n, stockText) {
  const end = svgRootEnd(stockText);
  if (!end) return null;
  const [x0, y0, w, h] = svgBox(stockText.slice(stockText.lastIndexOf("<svg", end), end)), fx = v => +v.toFixed(2);
  const rects = numberRects(n, w, h).map(([x, y, rw, rh]) =>
    `<rect x="${fx(x0 + x)}" y="${fx(y0 + y)}" width="${fx(rw)}" height="${fx(rh)}" fill="#ffffff"/>`).join("");
  const close = stockText.lastIndexOf("</svg>");
  return stockText.slice(0, end) + `<rect x="${fx(x0)}" y="${fx(y0)}" width="${fx(w)}" height="${fx(h)}" fill="#ff00ff"/>` +
    rects + (close >= end ? stockText.slice(close) : "</svg>");
}

/* An <svg ...> start tag's view box [x, y, width, height] (from viewBox,
   else width and height). */
function svgBox(root) {
  const attr = name => (new RegExp(`\\s${name}\\s*=\\s*["']([^"']*)["']`, "i").exec(root) || [])[1];
  const num = v => parseFloat(v) || 0;
  const vb = (attr("viewBox") || "").trim().split(/[\s,]+/).map(Number);
  return vb.length === 4 && !vb.some(isNaN) && vb[2] > 0 && vb[3] > 0 ? vb : [0, 0, num(attr("width")) || 64, num(attr("height")) || 64];
}

/* The console's SVG with a picture (a data: URL) in place of its drawing:
   its opening byte for byte, then an <image> filling its view box. */
function svgWithImage(stockText, dataUrl) {
  const end = svgRootEnd(stockText);
  if (!end) return null;
  const root = stockText.slice(stockText.lastIndexOf("<svg", end), end);
  const [x0, y0, w, h] = svgBox(root), fx = v => +v.toFixed(2);
  const href = /\sxmlns:xlink\s*=/.test(root) ? "xlink:href" : "href";
  const close = stockText.lastIndexOf("</svg>");
  return stockText.slice(0, end) +
    `<image x="${fx(x0)}" y="${fx(y0)}" width="${fx(w)}" height="${fx(h)}" preserveAspectRatio="xMidYMid meet" ${href}="${dataUrl}"/>` +
    (close >= end ? stockText.slice(close) : "</svg>");
}

/* A home screen tile's icon file: the title ID it belongs to, or "". The
   icon of an app is appdb/<title or "default">/icon0.png in its folder
   (some apps keep a second copy under assets). */
function tileTitle(path) {
  const m = /^\/(?:system_ex|mnt)\/rnps\/apps\/(NPXS\d+)\/(?:(?:assets\/)?(?:applications\/[^/]+\/)?appdb\/([^/]+)\/|assets\/src\/assets\/texture\/)icon0\.png$/.exec(path);
  return m ? (m[2] && m[2] !== "default" ? m[2] : m[1]) : "";
}

/* A game's or app's picture: in its folder in /user/appmeta, in the
   sce_sys folder of its installation, which most PS5 games' records point
   to instead, or among the pictures the console downloaded from PlayStation
   Network for it (by title ID) or for its game (by concept number, shared
   by a game's editions), which some games' records point to. */
const TITLE_PICTURE_RE = /^\/user\/(?:appmeta\/([^/]+)|app\/([^/]+)\/sce_sys|catalog_downloader\/appmeta\/([^/]+)|catalog_downloader\/conceptmeta\/([^/]+))\/[^/]+$/;
function isTitlePicture(path) { return TITLE_PICTURE_RE.test(path); }

/* The titles a picture belongs to: one, or for a concept's pictures every
   installed title the console's records show them for (from the plugin). */
function titlesOf(path) {
  const m = TITLE_PICTURE_RE.exec(path);
  if (!m) return [];
  return m[4] ? (state.concepts && state.concepts[m[4]]) || [] : [m[1] || m[2] || m[3]];
}

/* A PS4 param.sfo's TITLE, or "". */
function sfoTitle(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength), dec = new TextDecoder();
  if (b.length < 20 || dv.getUint32(0, true) !== 0x46535000) return "";
  const keys = dv.getUint32(8, true), data = dv.getUint32(12, true), n = dv.getUint32(16, true);
  for (let i = 0; i < n && 36 + 16 * i <= b.length; i++) {
    const e = 20 + 16 * i, ko = keys + dv.getUint16(e, true), len = dv.getUint32(e + 4, true), off = data + dv.getUint32(e + 12, true);
    let k = ko;
    while (k < b.length && b[k]) k++;
    if (dec.decode(b.subarray(ko, k)) === "TITLE" && off + len <= b.length) return dec.decode(b.subarray(off, off + len)).replace(/\0+$/, "");
  }
  return "";
}

/* Where an SVG's <svg ...> start tag ends (after its ">"), or 0. */
function svgRootEnd(text) {
  const m = /<svg[\s>]/.exec(text);
  if (!m) return 0;
  const end = text.indexOf(">", m.index + 4);
  return end < 0 || text[end - 1] === "/" ? 0 : end + 1;
}

/* A PNG's chunks: [{ type, start, end, data }] (end exclusive, CRC included). */
function pngChunks(d) {
  const out = [], dv = new DataView(d.buffer, d.byteOffset, d.length);
  if (d.length < 8 || d[0] !== 0x89 || d[1] !== 0x50 || d[2] !== 0x4e || d[3] !== 0x47) return null;
  for (let pos = 8; pos + 12 <= d.length;) {
    const len = dv.getUint32(pos), end = pos + 12 + len;
    if (end > d.length) return null;
    const type = String.fromCharCode(d[pos + 4], d[pos + 5], d[pos + 6], d[pos + 7]);
    out.push({ type, start: pos, end, data: d.subarray(pos + 8, pos + 8 + len) });
    if (type === "IEND") return out;
    pos = end;
  }
  return null;
}

/* What a console picture is: size, bit depth, colour type, palette (RGBA
   entries), its chunks, and whether this page can write it again exactly. */
function pngFormat(d) {
  const chunks = pngChunks(d);
  if (!chunks || chunks[0].type !== "IHDR" || chunks[0].data.length !== 13) return null;
  const h = chunks[0].data, dv = new DataView(h.buffer, h.byteOffset, 13);
  const f = { w: dv.getUint32(0), h: dv.getUint32(4), depth: h[8], ctype: h[9], chunks, stock: d };
  const ok = { 0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16] }[f.ctype];
  const idat = chunks.filter(c => c.type === "IDAT");
  f.zhead = idat.length && idat[0].data.length >= 2 ? [idat[0].data[0], idat[0].data[1]] : null;
  f.writable = !!ok && ok.includes(f.depth) && !h[10] && !h[11] && !h[12] && f.w > 0 && f.h > 0 && !!f.zhead;
  if (f.ctype === 3) {
    const plte = chunks.find(c => c.type === "PLTE"), trns = chunks.find(c => c.type === "tRNS");
    if (!plte) f.writable = false;
    else f.palette = Array.from({ length: plte.data.length / 3 }, (_, i) =>
      [plte.data[3 * i], plte.data[3 * i + 1], plte.data[3 * i + 2], trns && i < trns.data.length ? trns.data[i] : 255]);
  }
  /* IDAT chunks are split as the console's are. */
  f.idatSize = idat.length > 1 ? idat[0].data.length : Infinity;
  return f;
}

const PNG_CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function pngChunk(type, data) {
  const out = new Uint8Array(12 + data.length), dv = new DataView(out.buffer);
  dv.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  let c = 0xffffffff;
  for (let i = 4; i < 8 + data.length; i++) c = PNG_CRC[(c ^ out[i]) & 0xff] ^ (c >>> 8);
  dv.setUint32(8 + data.length, (c ^ 0xffffffff) >>> 0);
  return out;
}

/* zlib with stored (uncompressed) blocks: valid for any window size, and
   used when the browser has no CompressionStream. */
function zlibStored(data) {
  const blocks = Math.max(1, Math.ceil(data.length / 65535));
  const out = new Uint8Array(2 + data.length + 5 * blocks + 4);
  out[0] = 0x78; out[1] = 0x01;
  let o = 2;
  for (let b = 0; b < blocks; b++) {
    const part = data.subarray(b * 65535, Math.min(data.length, (b + 1) * 65535));
    out[o++] = b === blocks - 1 ? 1 : 0;
    out[o++] = part.length & 0xff; out[o++] = part.length >> 8;
    out[o++] = ~part.length & 0xff; out[o++] = (~part.length >> 8) & 0xff;
    out.set(part, o); o += part.length;
  }
  let a = 1, b2 = 0;
  for (let i = 0; i < data.length; i++) { a = (a + data[i]) % 65521; b2 = (b2 + a) % 65521; }
  new DataView(out.buffer).setUint32(o, ((b2 << 16) | a) >>> 0);
  return out;
}

/* RGBA pixels as a PNG that matches the console's picture `f` (pngFormat)
   in everything but the pixels: every other chunk is copied byte for byte
   and in place (header, palette, transparency, colour space, text), the
   image data keeps the console's zlib header and IDAT chunk size, and a
   palette picture uses the console's own palette (each pixel the nearest
   entry). `deflate` returns a zlib stream; it is used only where the
   console's header allows its 32 KB window, stored blocks otherwise. */
async function encodePng(rgba, f, deflate) {
  const { w, h, depth, ctype } = f, n = w * h;
  const grey = i => Math.round(0.299 * rgba[4 * i] + 0.587 * rgba[4 * i + 1] + 0.114 * rgba[4 * i + 2]);
  let samples, channels;
  if (ctype === 3) {
    const cache = new Map();
    const nearest = i => {
      const key = ((rgba[4 * i] << 24) | (rgba[4 * i + 1] << 16) | (rgba[4 * i + 2] << 8) | rgba[4 * i + 3]) >>> 0;
      if (cache.has(key)) return cache.get(key);
      let best = 0, bestD = Infinity;
      f.palette.forEach((p, j) => {
        let dd = 0;
        for (let c = 0; c < 4; c++) { const e = p[c] - rgba[4 * i + c]; dd += e * e; }
        if (dd < bestD) { bestD = dd; best = j; }
      });
      cache.set(key, best);
      return best;
    };
    samples = new Uint16Array(n);
    for (let i = 0; i < n; i++) samples[i] = nearest(i);
    channels = 1;
  } else {
    channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[ctype];
    samples = new Uint16Array(n * channels);
    const scale = v => depth === 16 ? v * 257 : depth === 8 ? v : Math.round(v * ((1 << depth) - 1) / 255);
    for (let i = 0; i < n; i++) {
      const px = ctype === 0 ? [grey(i)] : ctype === 4 ? [grey(i), rgba[4 * i + 3]]
        : ctype === 2 ? [rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2]] : [rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2], rgba[4 * i + 3]];
      for (let c = 0; c < channels; c++) samples[i * channels + c] = scale(px[c]);
    }
  }
  /* Scanlines, filter 0 (none), samples packed most significant bit first. */
  const rowBytes = Math.ceil(w * channels * depth / 8), raw = new Uint8Array(h * (1 + rowBytes));
  for (let y = 0; y < h; y++) {
    const row = raw.subarray(y * (1 + rowBytes) + 1, (y + 1) * (1 + rowBytes));
    for (let x = 0; x < w * channels; x++) {
      const v = samples[y * w * channels + x];
      if (depth === 16) { row[2 * x] = v >> 8; row[2 * x + 1] = v & 255; }
      else if (depth === 8) row[x] = v;
      else { const bit = x * depth; row[bit >> 3] |= v << (8 - depth - (bit & 7)); }
    }
  }
  const body = (f.zhead[0] >> 4) === 7 && deflate ? (await deflate(raw)).subarray(2) : zlibStored(raw).subarray(2);
  const z = new Uint8Array(2 + body.length);
  z[0] = f.zhead[0]; z[1] = f.zhead[1];
  z.set(body, 2);
  const parts = [f.stock.subarray(0, 8)];
  let idatDone = false;
  for (const c of f.chunks) {
    if (c.type !== "IDAT") { parts.push(f.stock.subarray(c.start, c.end)); continue; }
    if (idatDone) continue;
    idatDone = true;
    const step = Math.min(f.idatSize, z.length);
    for (let o = 0; o < z.length; o += step) parts.push(pngChunk("IDAT", z.subarray(o, Math.min(z.length, o + step))));
  }
  const out = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

/* A number's pixels on whole pixels, RGBA: `bg` square, `fg` digits. */
function numberPixels(n, w, h, bg = [255, 0, 255, 255], fg = [255, 255, 255, 255]) {
  const px = new Uint8Array(4 * w * h);
  for (let i = 0; i < w * h; i++) px.set(bg, 4 * i);
  for (const [x, y, rw, rh] of numberRects(n, w, h)) {
    const x0 = Math.round(x), y0 = Math.round(y), x1 = Math.max(x0 + 1, Math.round(x + rw)), y1 = Math.max(y0 + 1, Math.round(y + rh));
    for (let yy = Math.max(0, y0); yy < Math.min(h, y1); yy++) {
      for (let xx = Math.max(0, x0); xx < Math.min(w, x1); xx++) px.set(fg, 4 * (yy * w + xx));
    }
  }
  return px;
}

/* The numbered version of a console picture, in its exact format; null if
   that format can't be written. A palette picture is drawn with two of its
   own colours: those nearest pink and white, or else its darkest and
   lightest visible ones. */
async function numberPicture(n, stock, deflate) {
  const f = pngFormat(stock);
  if (!f || !f.writable) return null;
  let bg = [255, 0, 255, 255], fg = [255, 255, 255, 255];
  if (f.ctype === 3) {
    const dist = (p, q) => p.reduce((s, v, i) => s + (v - q[i]) ** 2, 0);
    const near = q => f.palette.reduce((b, p, i) => dist(p, q) < dist(f.palette[b], q) ? i : b, 0);
    let bi = near(bg), fi = near(fg);
    if (bi === fi) {
      const light = p => p[3] * (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]);
      const order = f.palette.map((p, i) => i).sort((a, b) => light(f.palette[a]) - light(f.palette[b]));
      bi = order[0]; fi = order[order.length - 1];
    }
    bg = f.palette[bi]; fg = f.palette[fi];
  }
  return encodePng(numberPixels(n, f.w, f.h, bg, fg), f, deflate);
}
