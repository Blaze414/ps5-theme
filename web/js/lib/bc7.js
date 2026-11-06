"use strict";

const BC7_WEIGHTS = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

/* A 4x4 block's colour line: the two ends of its pixels' range along their
   principal colour axis, as RGB. Both block encoders start from it. */
function blockLine(px) {
  let mr = 0, mg = 0, mb = 0;
  for (let i = 0; i < 64; i += 4) { mr += px[i]; mg += px[i + 1]; mb += px[i + 2]; }
  mr /= 16; mg /= 16; mb /= 16;
  let rr = 0, rg = 0, rb = 0, gg = 0, gb = 0, bb = 0;
  for (let i = 0; i < 64; i += 4) {
    const r = px[i] - mr, g = px[i + 1] - mg, b = px[i + 2] - mb;
    rr += r * r; rg += r * g; rb += r * b; gg += g * g; gb += g * b; bb += b * b;
  }
  let ax = 0.58, ay = 0.58, az = 0.58;
  for (let k = 0; k < 8; k++) {
    const nx = rr * ax + rg * ay + rb * az, ny = rg * ax + gg * ay + gb * az, nz = rb * ax + gb * ay + bb * az;
    const m = Math.max(Math.abs(nx), Math.abs(ny), Math.abs(nz));
    if (m < 1e-9) break;
    ax = nx / m; ay = ny / m; az = nz / m;
  }
  const len2 = ax * ax + ay * ay + az * az;
  let tmin = 0, tmax = 0;
  if (len2 > 1e-12) {
    tmin = Infinity; tmax = -Infinity;
    for (let i = 0; i < 64; i += 4) {
      const t = ((px[i] - mr) * ax + (px[i + 1] - mg) * ay + (px[i + 2] - mb) * az) / len2;
      if (t < tmin) tmin = t;
      if (t > tmax) tmax = t;
    }
  }
  return [[mr + tmin * ax, mg + tmin * ay, mb + tmin * az], [mr + tmax * ax, mg + tmax * ay, mb + tmax * az]];
}

/* One opaque 4x4 block as BC7 mode 6 (7-bit endpoints + p-bit, 4-bit indices).
   Endpoints come from the block's principal colour axis; every pixel then
   takes the exact nearest of the 16 palette colours. */
function bc7Block(px, out, o) {
  const [lo, hi] = blockLine(px);
  /* p-bit 1 on both endpoints keeps alpha at exactly 255 (A0 = A1 = 127). */
  const q = v => Math.max(0, Math.min(127, Math.round((v - 1) / 2)));
  let e0 = lo.map(q), e1 = hi.map(q);
  const pal = new Int32Array(48);
  const fill = () => {
    for (let i = 0; i < 16; i++) {
      const w = BC7_WEIGHTS[i];
      for (let c = 0; c < 3; c++) pal[i * 3 + c] = ((64 - w) * ((e0[c] << 1) | 1) + w * ((e1[c] << 1) | 1) + 32) >> 6;
    }
  };
  fill();
  const idx = new Uint8Array(16);
  for (let p = 0; p < 16; p++) {
    const r = px[p * 4], g = px[p * 4 + 1], b = px[p * 4 + 2];
    let best = 0, bestErr = Infinity;
    for (let i = 0; i < 16; i++) {
      const dr = r - pal[i * 3], dg = g - pal[i * 3 + 1], db = b - pal[i * 3 + 2];
      const e = dr * dr + dg * dg + db * db;
      if (e < bestErr) { bestErr = e; best = i; }
    }
    idx[p] = best;
  }
  if (idx[0] >= 8) {  /* the anchor index must fit in 3 bits */
    [e0, e1] = [e1, e0];
    for (let p = 0; p < 16; p++) idx[p] = 15 - idx[p];
  }
  let bit = 0;
  out.fill(0, o, o + 16);
  const put = (v, n) => { for (let i = 0; i < n; i++, bit++) if ((v >> i) & 1) out[o + (bit >> 3)] |= 1 << (bit & 7); };
  put(64, 7);
  for (let c = 0; c < 3; c++) { put(e0[c], 7); put(e1[c], 7); }
  put(127, 7); put(127, 7);
  put(1, 1); put(1, 1);
  put(idx[0], 3);
  for (let p = 1; p < 16; p++) put(idx[p], 4);
}

/* Mode-6 decoder, used only to check the encoder's own output. */
/* One 4x4 block with transparency as BC7 mode 6: endpoints in RGBA with a
   p-bit each, 4-bit indices. The endpoints come from the block's principal
   RGBA axis (the colour of clear pixels is already filled in, see
   bleedClear), and alpha counts double when a pixel picks its palette entry,
   so edges keep their softness. */
function bc7BlockAlpha(px, out, o) {
  const v = Float64Array.from(px);
  const m = [0, 0, 0, 0];
  for (let i = 0; i < 64; i += 4) for (let c = 0; c < 4; c++) m[c] += v[i + c] / 16;
  const cov = new Float64Array(16);
  for (let i = 0; i < 64; i += 4) {
    const d = [v[i] - m[0], v[i + 1] - m[1], v[i + 2] - m[2], v[i + 3] - m[3]];
    for (let a = 0; a < 4; a++) for (let b = 0; b < 4; b++) cov[a * 4 + b] += d[a] * d[b];
  }
  let ax = [0.5, 0.5, 0.5, 0.5];
  for (let k = 0; k < 8; k++) {
    const n = [0, 0, 0, 0];
    for (let a = 0; a < 4; a++) for (let b = 0; b < 4; b++) n[a] += cov[a * 4 + b] * ax[b];
    const mx = Math.max(Math.abs(n[0]), Math.abs(n[1]), Math.abs(n[2]), Math.abs(n[3]));
    if (mx < 1e-9) break;
    ax = n.map(x => x / mx);
  }
  const len2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2] + ax[3] * ax[3];
  let tmin = 0, tmax = 0;
  if (len2 > 1e-12) {
    tmin = Infinity; tmax = -Infinity;
    for (let i = 0; i < 64; i += 4) {
      let t = 0;
      for (let c = 0; c < 4; c++) t += (v[i + c] - m[c]) * ax[c];
      t /= len2;
      if (t < tmin) tmin = t;
      if (t > tmax) tmax = t;
    }
  }
  const end = t => [0, 1, 2, 3].map(c => Math.max(0, Math.min(255, m[c] + t * ax[c])));
  /* An endpoint is 7 bits plus the block's p-bit for it as the lowest bit: try both. */
  const quant = e => {
    let best = null;
    for (let p = 0; p < 2; p++) {
      const q = e.map(x => Math.max(0, Math.min(127, Math.round((x - p) / 2))));
      /* A clear or solid alpha must stay exact: its error weighs far more than a colour's. */
      const err = e.reduce((acc, x, c) => acc + (c === 3 ? 100 : 1) * ((q[c] << 1 | p) - x) ** 2, 0);
      if (!best || err < best.err) best = { q, p, err };
    }
    return best;
  };
  let a = quant(end(tmin)), b = quant(end(tmax));
  const pal = new Int32Array(64);
  const fill = () => {
    for (let i = 0; i < 16; i++) {
      const w = BC7_WEIGHTS[i];
      for (let c = 0; c < 4; c++) pal[i * 4 + c] = ((64 - w) * (a.q[c] << 1 | a.p) + w * (b.q[c] << 1 | b.p) + 32) >> 6;
    }
  };
  fill();
  const idx = new Uint8Array(16);
  for (let p = 0; p < 16; p++) {
    let best = 0, bestErr = Infinity;
    for (let i = 0; i < 16; i++) {
      let e = 0;
      for (let c = 0; c < 3; c++) { const d = v[p * 4 + c] - pal[i * 4 + c]; e += d * d; }
      const da = v[p * 4 + 3] - pal[i * 4 + 3];
      e += 2 * da * da;
      if (e < bestErr) { bestErr = e; best = i; }
    }
    idx[p] = best;
  }
  if (idx[0] >= 8) {  /* the anchor index must fit in 3 bits */
    [a, b] = [b, a];
    for (let p = 0; p < 16; p++) idx[p] = 15 - idx[p];
  }
  let bit = 0;
  out.fill(0, o, o + 16);
  const put = (x, n) => { for (let i = 0; i < n; i++, bit++) if ((x >> i) & 1) out[o + (bit >> 3)] |= 1 << (bit & 7); };
  put(64, 7);
  for (let c = 0; c < 4; c++) { put(a.q[c], 7); put(b.q[c], 7); }
  put(a.p, 1); put(b.p, 1);
  put(idx[0], 3);
  for (let p = 1; p < 16; p++) put(idx[p], 4);
}

function bc7DecodeMode6(src, o, px) {
  let bit = 0;
  const get = n => { let v = 0; for (let i = 0; i < n; i++, bit++) v |= ((src[o + (bit >> 3)] >> (bit & 7)) & 1) << i; return v; };
  if (get(7) !== 64) return false;
  const e = [[0, 0, 0, 0], [0, 0, 0, 0]];
  for (let c = 0; c < 4; c++) { e[0][c] = get(7); e[1][c] = get(7); }
  const p0 = get(1), p1 = get(1);
  for (let c = 0; c < 4; c++) { e[0][c] = (e[0][c] << 1) | p0; e[1][c] = (e[1][c] << 1) | p1; }
  for (let p = 0; p < 16; p++) {
    const w = BC7_WEIGHTS[get(p === 0 ? 3 : 4)];
    for (let c = 0; c < 4; c++) px[p * 4 + c] = ((64 - w) * e[0][c] + w * e[1][c] + 32) >> 6;
  }
  return true;
}
