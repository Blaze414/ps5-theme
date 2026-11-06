"use strict";

/* ================================================================ conversions (verified) */

function loadImage(src) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error("could not read the image"));
    img.src = src;
  });
}

/* What a picture file really holds, from its first bytes: the console keeps
   the pictures it downloads from PlayStation Network as WebP or JPEG under
   .png names. "" if none of these. */
function sniffImage(b) {
  if (b.length >= 8 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return "image/png";
  if (b.length >= 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return "image/jpeg";
  const tag = (o, t) => [...t].every((c, i) => b[o + i] === c.charCodeAt(0));
  if (b.length >= 16 && tag(0, "RIFF") && tag(8, "WEBP")) return "image/webp";
  return "";
}
const MIME_NAME = { "image/png": "PNG", "image/jpeg": "JPEG", "image/webp": "WebP" };

/* A WebP file's chunks after its RIFF header: [{ type, data }], or null. */
function webpChunks(b) {
  if (b.length < 20 || sniffImage(b) !== "image/webp") return null;
  const dv = new DataView(b.buffer, b.byteOffset, b.length), out = [];
  for (let o = 12; o + 8 <= b.length;) {
    const n = dv.getUint32(o + 4, true);
    if (o + 8 + n > b.length) return null;
    out.push({ type: String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]), data: b.subarray(o + 8, o + 8 + n) });
    o += 8 + n + (n & 1);
  }
  return out.length ? out : null;
}

function webpFile(chunks) {
  const size = chunks.reduce((a, c) => a + 8 + c.data.length + (c.data.length & 1), 4);
  const out = new Uint8Array(8 + size), dv = new DataView(out.buffer);
  out.set([0x52, 0x49, 0x46, 0x46]); dv.setUint32(4, size, true); out.set([0x57, 0x45, 0x42, 0x50], 8);
  let o = 12;
  for (const c of chunks) {
    for (let i = 0; i < 4; i++) out[o + i] = c.type.charCodeAt(i);
    dv.setUint32(o + 4, c.data.length, true);
    out.set(c.data, o + 8);
    o += 8 + c.data.length + (c.data.length & 1);
  }
  return out;
}

/* The browser's WebP laid out as the console's: a plain one (a single "VP8 "
   chunk) when the console's is plain, and otherwise without the colour
   profile browsers add, unless the console's has one too. */
function webpLike(out, stock) {
  const oc = webpChunks(out), sc = webpChunks(stock);
  if (!oc || !sc) return out;
  const vp8 = oc.find(c => c.type === "VP8 ");
  if (sc.length === 1 && sc[0].type === "VP8 " && vp8 && !oc.some(c => c.type === "ALPH")) return webpFile([vp8]);
  if (sc[0].type === "VP8X" && oc[0].type === "VP8X" && !sc.some(c => c.type === "ICCP")) {
    const x = oc[0].data.slice();
    x[0] &= ~0x20;   /* "has a colour profile" */
    return webpFile([{ type: "VP8X", data: x }, ...oc.slice(1).filter(c => c.type !== "ICCP")]);
  }
  return out;
}

/* Re-encode an image to the asset's exact format (the one its file really
   holds) and pixel size, then decode the result again to prove it. */
async function imageForAsset(file, assetPath) {
  if (!IMAGE_MIME[ext(assetPath)]) {
    if (!/svg/.test(file.type) && !/\.svg$/i.test(file.name)) throw new Error("use an SVG file for this one");
    return file;
  }
  if (!file.type.startsWith("image/") && !/\.(png|jpe?g|webp|gif|bmp|svg)$/i.test(file.name)) throw new Error(`${file.name} is not an image`);
  const res = await fetch(`/api/asset?${q({ path: assetPath })}`);
  if (!res.ok) throw new Error("could not read the system image");
  const stockBytes = new Uint8Array(await res.arrayBuffer());
  const mime = sniffImage(stockBytes) || IMAGE_MIME[ext(assetPath)];
  /* A WebP without an alpha channel (a plain "VP8 " one) stays opaque. */
  const opaque = mime === "image/jpeg" || (mime === "image/webp" && stockBytes[12] === 0x56 && stockBytes[15] === 0x20);
  const url = URL.createObjectURL(file), stockUrl = URL.createObjectURL(new Blob([stockBytes], { type: mime }));
  let stock, img;
  try {
    [stock, img] = await Promise.all([loadImage(stockUrl), loadImage(url)]);
  } finally { URL.revokeObjectURL(url); URL.revokeObjectURL(stockUrl); }
  const w = stock.naturalWidth, h = stock.naturalHeight;
  if (!w || !h) throw new Error("could not read the system image size");
  if (file.type === mime && img.naturalWidth === w && img.naturalHeight === h) return file;

  const canvas = document.createElement("canvas");
  canvas.width = w; canvas.height = h;
  const ctx = canvas.getContext("2d", { colorSpace: "srgb" });  /* sRGB: same look in SDR and HDR */
  ctx.imageSmoothingQuality = "high";
  if (opaque) { ctx.fillStyle = "#000"; ctx.fillRect(0, 0, w, h); }
  let dw = w, dh = h;
  if (state.fit !== "stretch") {
    const s = (state.fit === "cover" ? Math.max : Math.min)(w / img.naturalWidth, h / img.naturalHeight);
    dw = img.naturalWidth * s; dh = img.naturalHeight * s;
  }
  ctx.drawImage(img, (w - dw) / 2, (h - dh) / 2, dw, dh);
  let blob = await new Promise(r => canvas.toBlob(r, mime, 0.92));
  if (!blob || blob.type !== mime) throw new Error(`this browser cannot make ${MIME_NAME[mime] || mime} pictures; use Chrome, Edge or Firefox for this one`);
  if (mime === "image/webp") blob = new Blob([webpLike(new Uint8Array(await blob.arrayBuffer()), stockBytes)], { type: mime });
  const check = URL.createObjectURL(blob);
  try {
    const out = await loadImage(check);
    if (out.naturalWidth !== w || out.naturalHeight !== h) throw new Error("converted image did not verify");
  } finally { URL.revokeObjectURL(check); }
  return blob;
}

function parseWav(buf) {
  const dv = new DataView(buf);
  const tag = o => String.fromCharCode(...new Uint8Array(buf, o, 4));
  if (buf.byteLength < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") return null;
  const info = {};
  for (let o = 12; o + 8 <= buf.byteLength; ) {
    const id = tag(o), size = dv.getUint32(o + 4, true);
    if (id === "fmt ") {
      info.format = dv.getUint16(o + 8, true); info.channels = dv.getUint16(o + 10, true);
      info.rate = dv.getUint32(o + 12, true); info.bits = dv.getUint16(o + 22, true);
    } else if (id === "data") {
      info.dataOffset = o + 8; info.dataSize = size;
      break;
    }
    o += 8 + size + (size & 1);
  }
  return info.format && info.dataOffset ? info : null;
}

/* Decode any audio the browser supports to `channels` channels at `rate`. */
async function decodeAudio(file, rate, channels) {
  const data = await file.arrayBuffer();
  let decoded;
  try {
    decoded = await new OfflineAudioContext(1, 1, rate).decodeAudioData(data.slice(0));
  } catch (e) { throw new Error(`${file.name} is not audio this browser can read`); }
  return renderAudio(decoded, rate, channels);
}

/* Render an AudioBuffer (any rate and layout) with `channels` channels at `rate`. */
async function renderAudio(buffer, rate, channels) {
  const frames = Math.max(1, Math.ceil(buffer.duration * rate));
  const ctx = new OfflineAudioContext(channels, frames, rate);
  const src = ctx.createBufferSource();
  src.buffer = buffer;
  src.connect(ctx.destination);
  src.start();
  const out = await ctx.startRendering();
  return [...Array(channels)].map((_, c) => out.getChannelData(c));
}

async function resample(chans, from, to) {
  if (from === to) return chans;
  const ctx = new OfflineAudioContext(chans.length, 1, from);
  const buf = ctx.createBuffer(chans.length, chans[0].length, from);
  chans.forEach((c, i) => buf.copyToChannel(c, i));
  return renderAudio(buf, to, chans.length);
}

/* 16-bit PCM WAV from channel data, parsed again to prove its shape. */
function pcmWav(chans, rate) {
  const channels = chans.length, frames = chans[0].length;
  const bytes = 44 + frames * channels * 2;
  const buf = new ArrayBuffer(bytes);
  const dv = new DataView(buf);
  const str = (o, s) => [...s].forEach((c, i) => dv.setUint8(o + i, c.charCodeAt(0)));
  str(0, "RIFF"); dv.setUint32(4, bytes - 8, true); str(8, "WAVE");
  str(12, "fmt "); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, channels, true);
  dv.setUint32(24, rate, true); dv.setUint32(28, rate * channels * 2, true); dv.setUint16(32, channels * 2, true);
  dv.setUint16(34, 16, true); str(36, "data"); dv.setUint32(40, frames * channels * 2, true);
  let o = 44;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < channels; c++) {
      const v = Math.max(-1, Math.min(1, chans[c][i]));
      dv.setInt16(o, v < 0 ? v * 32768 : v * 32767, true);
      o += 2;
    }
  }
  const check = parseWav(buf);
  if (!check || check.rate !== rate || check.channels !== channels || check.bits !== 16 ||
      check.dataSize !== frames * channels * 2) throw new Error("converted audio did not verify");
  return new Blob([buf], { type: "audio/wav" });
}

/* Channel data of a 16-bit PCM WAV. */
function wavChannels(buf) {
  const info = parseWav(buf);
  if (!info || info.format !== 1 || info.bits !== 16) throw new Error("the theme's music is not a 16-bit PCM WAV");
  const n = Math.floor(info.dataSize / (2 * info.channels));
  const dv = new DataView(buf, info.dataOffset, n * 2 * info.channels);
  const chans = [...Array(info.channels)].map(() => new Float32Array(n));
  for (let i = 0, o = 0; i < n; i++) for (let c = 0; c < info.channels; c++, o += 2) chans[c][i] = dv.getInt16(o, true) / 32768;
  return { chans, rate: info.rate };
}

/* Any audio as a 16-bit PCM WAV of the given shape. */
async function audioToWav(file, rate, channels) {
  return pcmWav(await decodeAudio(file, rate, channels), rate);
}
