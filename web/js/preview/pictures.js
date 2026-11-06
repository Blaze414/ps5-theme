"use strict";

/* ================================================================ preview pictures

   Each theme can have a picture for its library card: one made from its
   background when the background is added (kind "background"), and one the
   user chooses (kind "picture"), which is shown instead. */

const PREVIEW_W = 1280, PREVIEW_H = 720;

const PREVIEW_FILES = [["background", "preview.jpg"], ["picture", "picture.jpg"], ["iconbg", "icon-backdrop.jpg"]];   /* in a theme .zip */
const coverUrl = t => `/api/theme/preview?${q({ id: t.id })}&v=${t.coverTime}-${stamp}-${homeDrawnAt.get(t.id) || 0}`;

/* A 16:9 JPEG of an image, video frame or canvas, fitted as `fit`. */
async function previewJpeg(src, w, h, fit) {
  const c = document.createElement("canvas");
  c.width = PREVIEW_W; c.height = PREVIEW_H;
  const ctx = c.getContext("2d");
  ctx.imageSmoothingQuality = "high";
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, c.width, c.height);
  let dw = c.width, dh = c.height;
  if (fit !== "stretch") {
    const sc = (fit === "contain" ? Math.min : Math.max)(c.width / w, c.height / h);
    dw = w * sc; dh = h * sc;
  }
  ctx.drawImage(src, (c.width - dw) / 2, (c.height - dh) / 2, dw, dh);
  const blob = await new Promise(resolve => c.toBlob(resolve, "image/jpeg", 0.85));
  if (!blob) throw new Error("this browser could not make the picture");
  await loadImage(URL.createObjectURL(blob)).then(img => URL.revokeObjectURL(img.src));  /* check it reads back */
  return blob;
}

/* A frame from a little way into a video (backgrounds often fade in). */
function videoFrame(url) {
  return new Promise((resolve, reject) => {
    const v = document.createElement("video");
    const timer = setTimeout(() => reject(new Error("could not read the video")), 10000);
    v.muted = true; v.playsInline = true; v.preload = "auto";
    v.onloadedmetadata = () => { v.currentTime = Math.min(1, (v.duration || 0) / 3); };
    v.onseeked = () => { clearTimeout(timer); resolve(v); };
    v.onerror = () => { clearTimeout(timer); reject(new Error("could not read the video")); };
    v.src = url;
  });
}

async function previewSource(url, isVideo) {
  if (isVideo) { const v = await videoFrame(url); return [v, v.videoWidth, v.videoHeight]; }
  const img = await loadImage(url);
  return [img, img.naturalWidth, img.naturalHeight];
}

async function savePreview(id, kind, blob) {
  await uploadVerified("/api/theme/preview", { id, kind }, blob);
  stamp = Date.now();
  await refreshStatus();
  if (state.route === "edit" && state.editId === id && $("#cover-panel")) $("#cover-panel").innerHTML = coverPanel(themeById(id));
  if (state.route === "edit" && state.editId === id) { syncNav("theme"); syncNav("background"); }
}

async function removePreview(id, kind) {
  await api("DELETE", "/api/theme/preview", { id, kind }).catch(() => {});
  stamp = Date.now();
  await refreshStatus();
  if (state.route === "edit" && state.editId === id && $("#cover-panel")) $("#cover-panel").innerHTML = coverPanel(themeById(id));
  if (state.route === "edit" && state.editId === id) { syncNav("theme"); syncNav("background"); }
}

/* After a background is added: the same picture, fitted as the background was. */
async function previewFromFile(id, file, isVideo) {
  const url = URL.createObjectURL(file);
  try {
    const [src, w, h] = await previewSource(url, isVideo);
    await savePreview(id, "background", await previewJpeg(src, w, h, state.fit));
  } catch (e) {
    console.warn("preview picture:", e);
  } finally { URL.revokeObjectURL(url); }
}

/* ---- reading a theme's own background back (themes made before previews) */

/* The content of a zstd frame made of raw and RLE blocks, which is how this
   builder stores textures. Compressed blocks are not read. */
function unzstdStored(b) {
  if (b[0] !== 0x28 || b[1] !== 0xb5 || b[2] !== 0x2f || b[3] !== 0xfd) throw new Error("not zstd");
  const fhd = b[4], single = (fhd >> 5) & 1, fcs = [single ? 1 : 0, 2, 4, 8][fhd >> 6];
  let o = 5 + (single ? 0 : 1) + [0, 1, 2, 4][fhd & 3] + fcs;
  const parts = [];
  for (;;) {
    if (o + 3 > b.length) throw new Error("truncated zstd");
    const h = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16), type = (h >> 1) & 3, size = h >>> 3;
    o += 3;
    if (type === 0) { parts.push(b.subarray(o, o + size)); o += size; }
    else if (type === 1) { parts.push(new Uint8Array(size).fill(b[o])); o += 1; }
    else throw new Error("compressed zstd");
    if (h & 1) break;
  }
  const out = new Uint8Array(parts.reduce((n, x) => n + x.length, 0));
  let w = 0;
  for (const x of parts) { out.set(x, w); w += x.length; }
  return out;
}

/* The top level of a DDS texture as a canvas: BC7 (as this builder encodes
   it), RGBA8 or BGRA8. `keepAlpha`: BC7 with its see-through parts, for a picture taken back out of a texture. */
async function ddsCanvas(bytes, keepAlpha = false) {
  const d = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (d.getUint32(0, true) !== 0x20534444) throw new Error("not a DDS texture");
  const h = d.getUint32(12, true), w = d.getUint32(16, true);
  const fourcc = d.getUint32(84, true), dx10 = fourcc === 0x30315844;
  let dxgi = dx10 ? d.getUint32(128, true) : 0;
  if (!dx10 && d.getUint32(88, true) === 32) dxgi = d.getUint32(92, true) === 0xff ? 28 : d.getUint32(92, true) === 0xff0000 ? 87 : 0;
  const data = bytes.subarray(dx10 ? 148 : 128);
  if (!w || !h || w > 8192 || h > 8192) throw new Error("unexpected texture size");
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const ctx = c.getContext("2d");
  const img = ctx.createImageData(w, h), px = img.data;
  if (dxgi === 98 || dxgi === 99) {
    const bw = Math.ceil(w / 4), bh = Math.ceil(h / 4), block = new Uint8Array(64);
    if (data.length < bw * bh * 16) throw new Error("texture is truncated");
    let last = performance.now();
    for (let by = 0; by < bh; by++) {
      for (let bx = 0; bx < bw; bx++) {
        if (!bc7DecodeMode6(data, (by * bw + bx) * 16, block)) throw new Error("texture uses BC7 modes this preview can't read");
        for (let y = 0; y < 4 && by * 4 + y < h; y++) {
          for (let x = 0; x < 4 && bx * 4 + x < w; x++) {
            const s = (y * 4 + x) * 4, t = ((by * 4 + y) * w + bx * 4 + x) * 4;
            px[t] = block[s]; px[t + 1] = block[s + 1]; px[t + 2] = block[s + 2]; px[t + 3] = keepAlpha ? block[s + 3] : 255;
          }
        }
      }
      if (performance.now() - last > 50) { await nextTask(); last = performance.now(); }
    }
  } else if (dxgi === 28 || dxgi === 29 || dxgi === 87 || dxgi === 91) {
    if (data.length < w * h * 4) throw new Error("texture is truncated");
    const bgra = dxgi === 87 || dxgi === 91;
    for (let i = 0; i < w * h * 4; i += 4) {
      px[i] = data[i + (bgra ? 2 : 0)]; px[i + 1] = data[i + 1]; px[i + 2] = data[i + (bgra ? 0 : 2)]; px[i + 3] = 255;
    }
  } else throw new Error("texture format not readable here");
  ctx.putImageData(img, 0, 0);
  return c;
}

/* The theme's home background, preferring the default home screen one. */
function backgroundFile(paths) {
  const bgs = paths.filter(p => BG_RE.test(p));
  const rank = p => areaOf(p) === "Home: default background" ? 0 : areaOf(p).startsWith("Home:") ? 1 : 2;
  return bgs.sort((a, b) => rank(a) - rank(b))[0] || null;
}

async function previewFromTheme(id) {
  const { files } = await api("GET", "/api/theme/files", { id });
  const path = backgroundFile(files.map(f => f.path));
  if (!path) return false;
  const url = `/api/theme/file?${q({ id, path })}`;
  let src, w, h;
  if (/\.dds(\.zst)?$/i.test(path)) {
    const res = await fetch(url);
    if (!res.ok) throw new Error("could not read the background");
    let bytes = new Uint8Array(await res.arrayBuffer());
    if (/\.zst$/i.test(path)) bytes = unzstdStored(bytes);
    src = await ddsCanvas(bytes); w = src.width; h = src.height;
  } else {
    [src, w, h] = await previewSource(url, /\.(mp4|webm|m4v)$/i.test(path));
  }
  await savePreview(id, "background", await previewJpeg(src, w, h, "stretch"));
  return true;
}

/* Themes made before preview pictures: make one from the background, once
   per visit. Quietly does nothing when it can't. */
const previewTried = new Set();
let previewFilling = false;
async function fillMissingPreviews() {
  if (previewFilling) return;
  previewFilling = true;
  try {
    for (const t of state.status.themes) {
      if (t.cover || !t.files || previewTried.has(t.id)) continue;
      previewTried.add(t.id);
      try {
        const el = await previewFromTheme(t.id) && document.querySelector(`[data-cover="${CSS.escape(t.id)}"]`);
        if (el && themeById(t.id)) el.outerHTML = cardPreview(themeById(t.id));
      } catch (e) { console.warn(`preview picture for ${t.id}:`, e); }
    }
  } finally { previewFilling = false; }
}

function coverPanel(t) {
  const text = t.cover === "picture" ? "Your picture, shown instead of the home screen. Remove it to show the home screen again."
    : t.cover === "home" ? "Its home screen as your console shows it, drawn again as the theme changes. Or choose a picture of your own."
    : t.cover === "background" ? "Made from the background, until the home screen is drawn."
    : "The theme's home screen, drawn once the theme has something in it. Or choose a picture of your own.";
  return `
    <div class="cover">${t.cover ? `<img alt="Preview of ${esc(t.name)}" src="${coverUrl(t)}">` : "No picture yet"}</div>
    <div class="text"><h2>Preview picture</h2><div class="sub">${esc(text)}</div>
      <div class="acts"><button class="btn sm" data-act="choose-cover">Choose picture</button>
        ${t.cover === "picture" ? `<button class="btn sm ghost danger" data-act="remove-cover">Remove picture</button>` : ""}</div></div>`;
}

async function chooseCover() {
  const id = state.editId;
  const file = await pickFile("image/*");
  if (!file) return;
  const url = URL.createObjectURL(file);
  try {
    const [img, w, h] = await previewSource(url, false);
    await savePreview(id, "picture", await previewJpeg(img, w, h, "cover"));
    toast("Preview picture set.", "ok");
  } catch (e) {
    toast(e.message, "err");
  } finally { URL.revokeObjectURL(url); }
}
