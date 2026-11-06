"use strict";

/* ---- textures: DDS for ShellUI (.dds / .dds.zst) ---- */

/* BC1 (DXT1) colours from two RGB565 endpoints: four opaque ones when the
   first is larger, else three and transparent black. */
function bc1Palette(c0, c1) {
  const rgb = c => [((c >> 11) << 3) | (c >> 13), (((c >> 5) & 63) << 2) | ((c >> 9) & 3), ((c & 31) << 3) | ((c >> 2) & 7)];
  const a = rgb(c0), b = rgb(c1), mix = (wa, wb, d) => a.map((v, i) => Math.round((wa * v + wb * b[i]) / d)).concat(255);
  return c0 > c1 ? [a.concat(255), b.concat(255), mix(2, 1, 3), mix(1, 2, 3)] : [a.concat(255), b.concat(255), mix(1, 1, 2), [0, 0, 0, 0]];
}

/* One opaque 4x4 block as BC1 (DXT1), as PS4 titles' icon0.dds: endpoints
   from the block's colour line, stored larger first so the block has four
   opaque colours, and each pixel the nearest of them. */
function bc1Block(px, out, o) {
  const [lo, hi] = blockLine(px);
  const q = (v, n) => Math.max(0, Math.min(n, Math.round(v * n / 255)));
  const to565 = c => (q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31);
  let c0 = to565(hi), c1 = to565(lo);
  if (c0 < c1) [c0, c1] = [c1, c0];
  let bits = 0;
  if (c0 !== c1) {  /* equal endpoints: index 0 everywhere, still opaque */
    const pal = bc1Palette(c0, c1);
    for (let p = 0; p < 16; p++) {
      let best = 0, bestErr = Infinity;
      for (let i = 0; i < 4; i++) {
        const dr = px[p * 4] - pal[i][0], dg = px[p * 4 + 1] - pal[i][1], db = px[p * 4 + 2] - pal[i][2];
        const e = dr * dr + dg * dg + db * db;
        if (e < bestErr) { bestErr = e; best = i; }
      }
      bits = (bits | (best << (2 * p))) >>> 0;
    }
  }
  out[o] = c0 & 255; out[o + 1] = c0 >> 8; out[o + 2] = c1 & 255; out[o + 3] = c1 >> 8;
  for (let i = 0; i < 4; i++) out[o + 4 + i] = (bits >>> (8 * i)) & 255;
}

/* BC1 decoder, used only to check the encoder's own output. */
function bc1Decode(src, o, px) {
  const pal = bc1Palette(src[o] | (src[o + 1] << 8), src[o + 2] | (src[o + 3] << 8));
  for (let p = 0; p < 16; p++) px.set(pal[(src[o + 4 + (p >> 2)] >> (2 * (p & 3))) & 3], p * 4);
  return true;
}

function b64bytes(s) { return Uint8Array.from(atob(s), c => c.charCodeAt(0)); }

const textureCache = new LimitedCache(4);   /* the last few only: a 4K texture is 16 MB */

/* Whether a top-right piece keeps the band its clock and icons are drawn over clear: on unless switched off. */
const keepClearFor = path => ((state.choices.pieces || {})[path] || {}).keepClear !== false;

/* A piece's saved placement: the choice and its padding (kept with the theme). */
function placementFor(path) {
  const a = textureAnchor(path), hasEdge = !!(a.top || a.right || a.bottom);
  const { place: mode, pad } = (state.choices.pieces || {})[path] || {};
  return { mode: PLACES.includes(mode) && (hasEdge || mode !== "pad") ? mode : "fill",
    pad: pad === undefined ? PAD_DEFAULT : Math.max(0, Math.min(40, +pad || 0)), hasEdge };
}

/* Textures with see-through parts keep their transparency: the Appearance themes' panels and bars. */
const ALPHA_TEXTURE = /\/system\/vsh_asset\/(control_center_|homeui_)[^/]*$/;

/* Encode an image into exactly the stock texture's format, size and mips, reusing its DDS header verbatim, then
   check the result. */
async function textureForAsset(file, assetPath, progress) {
  const info = await api("GET", "/api/asset/texture", { path: assetPath });
  const alpha = ALPHA_TEXTURE.test(assetPath) && (info.dxgi === 98 || info.dxgi === 99);
  const place = alpha ? placementParams(placementFor(assetPath).mode, placementFor(assetPath).pad) : { fit: state.fit, pad: 0 };
  const keep = alpha && keepClearFor(assetPath);
  const key = `${info.header}|${info.dataLength}|${place.fit}|${alpha}|${place.pad}|${keep}|${file.name}|${file.size}|${file.lastModified}`;
  if (textureCache.has(key)) return textureCache.get(key);
  const bc7 = info.dxgi === 98 || info.dxgi === 99, bc1 = info.dxgi === 71 || info.dxgi === 72 || info.format === "DXT1";
  const rgba = info.dxgi === 28 || info.dxgi === 29, bgra = info.dxgi === 87 || info.dxgi === 91;
  if (!bc7 && !bc1 && !rgba && !bgra) throw new Error(`${info.format} textures are not supported yet`);
  const blockBytes = bc1 ? 8 : 16, encode = bc1 ? bc1Block : alpha ? bc7BlockAlpha : bc7Block, decode = bc1 ? bc1Decode : bc7DecodeMode6;
  if (info.array !== 1) throw new Error("texture arrays are not supported");
  if (!file.type.startsWith("image/") && !/\.(png|jpe?g|webp|gif|bmp)$/i.test(file.name)) throw new Error(`${file.name} is not an image`);

  const url = URL.createObjectURL(file);
  let img;
  try { img = await loadImage(url); } finally { URL.revokeObjectURL(url); }
  const header = b64bytes(info.header);
  const data = new Uint8Array(info.dataLength);
  let o = 0, sampleErr = 0, sampled = 0, alphaErr = 0, alphaN = 0;
  const totalBlocks = Array.from({ length: info.mips }, (_, m) =>
    Math.ceil(Math.max(1, info.width >> m) / 4) * Math.ceil(Math.max(1, info.height >> m) / 4)).reduce((a, b) => a + b, 0);
  let done = 0, lastYield = performance.now();

  for (let m = 0; m < info.mips; m++) {
    const w = Math.max(1, info.width >> m), h = Math.max(1, info.height >> m);
    const canvas = document.createElement("canvas");
    canvas.width = w; canvas.height = h;
    const ctx = canvas.getContext("2d", { colorSpace: "srgb", willReadFrequently: true });
    ctx.imageSmoothingQuality = "high";
    if (!alpha) { ctx.fillStyle = "#000"; ctx.fillRect(0, 0, w, h); }   /* with transparency, what the picture leaves bare stays clear */
    if (alpha) {
      /* against the screen's edge, with the padding left clear */
      const d = textureDraw(assetPath, place.pad, place.fit, w, h, img.naturalWidth, img.naturalHeight, keep);
      ctx.save();
      ctx.beginPath(); ctx.rect(d.box.x, d.box.y, d.box.w, d.box.h); ctx.clip();
      ctx.drawImage(img, d.x, d.y, d.dw, d.dh);
      ctx.restore();
    } else {
      let dw = w, dh = h;
      if (state.fit !== "stretch") {
        const sc = (state.fit === "cover" ? Math.max : Math.min)(w / img.naturalWidth, h / img.naturalHeight);
        dw = img.naturalWidth * sc; dh = img.naturalHeight * sc;
      }
      ctx.drawImage(img, (w - dw) / 2, (h - dh) / 2, dw, dh);
    }
    const pix = ctx.getImageData(0, 0, w, h).data;
    if (alpha) bleedClear(pix, w, h);
    if (!bc7 && !bc1) {
      for (let i = 0; i < w * h; i++) {
        const s = i * 4;
        if (rgba) { data[o++] = pix[s]; data[o++] = pix[s + 1]; data[o++] = pix[s + 2]; data[o++] = 255; }
        else { data[o++] = pix[s + 2]; data[o++] = pix[s + 1]; data[o++] = pix[s]; data[o++] = 255; }
      }
      continue;
    }
    const block = new Uint8ClampedArray(64), check = new Uint8Array(64);
    const bw = Math.ceil(w / 4), bh = Math.ceil(h / 4);
    for (let by = 0; by < bh; by++) {
      for (let bx = 0; bx < bw; bx++) {
        for (let y = 0; y < 4; y++) {
          for (let x = 0; x < 4; x++) {
            const sx = Math.min(w - 1, bx * 4 + x), sy = Math.min(h - 1, by * 4 + y), s = (sy * w + sx) * 4, d = (y * 4 + x) * 4;
            block[d] = pix[s]; block[d + 1] = pix[s + 1]; block[d + 2] = pix[s + 2]; block[d + 3] = alpha ? pix[s + 3] : 255;
          }
        }
        encode(block, data, o);
        if ((bx * 7 + by * 13) % 97 === 0) {  /* re-decode a sample of blocks */
          if (!decode(data, o, check)) throw new Error("texture encoder self-check failed");
          for (let i = 0; i < 64; i++) {
            if ((i & 3) !== 3) {
              if (alpha && block[i | 3] < 16) continue;   /* the colour of a clear pixel doesn't show */
              const e = check[i] - block[i]; sampleErr += e * e; sampled++;
            } else if (alpha) { const e = check[i] - block[i]; alphaErr += e * e; alphaN++; }
            else if (check[i] !== 255) throw new Error("texture encoder self-check failed");
          }
        }
        o += blockBytes;
      }
      done += bw;
      if (performance.now() - lastYield > 50) {
        if (progress) progress(done / totalBlocks);
        await nextTask();
        lastYield = performance.now();
      }
    }
  }
  if (o !== data.length) throw new Error("texture size did not verify");
  if (sampled) {
    const psnr = 10 * Math.log10(255 * 255 / Math.max(1e-9, sampleErr / sampled));
    if (psnr < 24) throw new Error("the texture came out too unlike your picture to use (too much fine detail for its size); try a smoother picture");
  }
  if (alphaN && Math.sqrt(alphaErr / alphaN) > 24) throw new Error("the see-through parts did not encode well");
  const dds = new Uint8Array(header.length + data.length);
  dds.set(header); dds.set(data, header.length);
  const out = new Blob([info.zstd ? zstdStored(dds, info.frame ? b64bytes(info.frame) : null) : dds]);
  return textureCache.set(key, out);
}

async function fileForAsset(file, asset) {
  if (asset.kind === "texture") return textureForAsset(file, asset.path, attachProgress);
  if (isImage(asset)) return imageForAsset(file, asset.path);
  if (asset.kind === "audio") return soundForAsset(file, asset.path);
  return file;  /* fonts, JSON, video: the console validates against the stock file */
}

let attachProgress = null;

/* Put one file onto a set of system assets (for example all size variants).
   `copies`: the same picture already written to that many other files, for
   a message that counts files instead of sizes. */
/* `quiet`: part of a larger task (styling every icon) that has its own progress and says the result itself: no
   progress of this one's and no message, only the count of files written. */
async function attachTo(assets, file, label, copies = null, remake = false, shown = null, quiet = false) {
  const big = assets.length > 2 || assets.some(a => a.kind === "texture" || ext(a.path) === "at9");
  const p = quiet ? null : shown || (big ? progressToast(`Adding ${label}…`) : null);
  const base = shown ? shown.frac : 0;   /* a task begun before (reading a kept picture) keeps its part of the bar */
  let ok = 0, keptFirst = null;
  const errors = [];
  for (let i = 0; i < assets.length; i++) {
    const a = assets[i], at = x => base + (1 - base) * (i + x) / assets.length;
    const step = `${label}${assets.length > 1 ? ` (${i + 1} of ${assets.length})` : ""}`;
    attachProgress = p ? f => p.set(`Making ${step}… ${Math.round(f * 100)}%`, at(0.7 * f)) : null;
    /* each step says what it is as it starts */
    if (p) p.set(`${a.kind === "texture" || a.kind === "audio" || isImage(a) ? "Making" : "Preparing"} ${step}…`, at(0));
    try {
      const blob = await fileForAsset(file, a);
      const keep = !remake && state.keepOriginals && keepsOriginal(a) && blob !== file;   /* so an option changed later needs no new upload */
      if (p) p.set(`Sending ${baseName(a.path)} to the console…`, at(0.7), at(keep ? 0.9 : 1));
      await uploadVerified("/api/theme/file", { id: state.editId, path: a.path }, blob);
      state.files.add(a.path);
      if (keep) {
        if (p) p.set(keptFirst ? `Keeping your original for ${baseName(a.path)}…` : "Keeping your original picture…", at(0.9), at(1));
        await keepSource(a.path, file, keptFirst);
        if (!keptFirst && state.sources.has(a.path)) keptFirst = a.path;
      }
      ok++;
    } catch (e) { errors.push(`${baseName(a.path)}: ${e.message}`); }
    if (p) p.set(`${remake ? "Making" : "Adding"} ${label}${remake ? " again" : ""}… ${i + 1}/${assets.length}`, at(1));
  }
  stamp = Date.now();
  if (!remake && state.keepOriginals) await loadOriginals();
  /* The interface reads the Appearance artwork as it starts: it shows after rest mode. */
  const use = assets.some(a => APPEARANCE_RE.test(a.path))
    ? "Apply the theme, then put the console in rest mode and wake it to see the change." : "Apply the theme to use it.";
  const msg = errors.length
    ? `${label}: ${ok} of ${assets.length} added and verified. ${errors.slice(0, 2).join("; ")}`
    : copies !== null ? `${label} replaced in ${plural(ok + copies, "file")} and verified. ${use}`
    : `${label} ${remake ? "made again" : "added"} and verified${assets.length > 1 ? ` at ${plural(ok, "size")}` : ""}. ${use}`;
  const kind = errors.length ? (ok ? "" : "err") : "ok";
  if (quiet) return ok;
  if (p) p.done(msg, kind); else toast(msg, kind, errors.length ? 9000 : 4500);
  await refreshStatus();
  return ok;
}

/* After the Appearance artwork goes back to the console's own in the theme. */
function restReminder() {
  toast("Back to the console's own artwork in this theme. Apply the theme, then put the console in rest mode and wake it to see the change.", "ok", 7000);
}

/* Every song of the theme goes (a playlist's too), and the home music is the console's again. */
async function removeMusic() {
  const id = state.editId, extra = themeTracks(themeById(id)).map(x => x.slot).filter(s => s !== 1);
  for (const slot of extra) await api("DELETE", "/api/theme/music", { id, slot }).catch(() => {});
  await api("DELETE", "/api/theme/music", { id }).catch(() => {});
  if (extra.length) await api("POST", "/api/theme/settings", { id, sequence: "1", tracks: 1, loop_start: -1, loop_end: -1, order: "music" }).catch(() => {});
  setSongNames(names => { for (const k of Object.keys(names)) delete names[k]; });
  songCacheDrop(id);
  await resetAssets([{ path: HOME_MUSIC }]);
}

async function resetAssets(assets) {
  const hadBackground = backgroundFile([...state.files]);
  for (const a of assets.filter(x => state.files.has(x.path))) {
    try { await api("DELETE", "/api/theme/file", { id: state.editId, path: a.path }); state.files.delete(a.path); state.sources.delete(a.path); }
    catch (e) { toast(e.message, "err"); }
  }
  await loadOriginals();
  stamp = Date.now();
  /* No background left: neither is the picture made from it. */
  if (hadBackground && !backgroundFile([...state.files])) await removePreview(state.editId, "background");
  else await refreshStatus();
}
