"use strict";

/* XXH64 (the zstd content checksum is its low 32 bits). */
function xxh64(data) {
  const M = (1n << 64n) - 1n, P1 = 11400714785074694791n, P2 = 14029467366897019727n, P3 = 1609587929392839161n,
    P4 = 9650029242287828579n, P5 = 2870177450012600261n;
  const rotl = (x, r) => ((x << BigInt(r)) | (x >> BigInt(64 - r))) & M;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const rd64 = o => dv.getBigUint64(o, true);
  const round = (acc, v) => (rotl((acc + v * P2) & M, 31) * P1) & M;
  const merge = (h, v) => (((h ^ round(0n, v)) * P1) & M) + P4 & M;
  let o = 0, h;
  const n = data.length;
  if (n >= 32) {
    let v1 = (P1 + P2) & M, v2 = P2, v3 = 0n, v4 = (0n - P1) & M;
    for (; o + 32 <= n; o += 32) { v1 = round(v1, rd64(o)); v2 = round(v2, rd64(o + 8)); v3 = round(v3, rd64(o + 16)); v4 = round(v4, rd64(o + 24)); }
    h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M;
    h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
  } else h = P5;
  h = (h + BigInt(n)) & M;
  for (; o + 8 <= n; o += 8) h = ((rotl(h ^ round(0n, rd64(o)), 27) * P1) & M) + P4 & M;
  if (o + 4 <= n) { h = ((rotl(h ^ ((BigInt(dv.getUint32(o, true)) * P1) & M), 23) * P2) & M) + P3 & M; o += 4; }
  for (; o < n; o++) h = (rotl(h ^ ((BigInt(data[o]) * P5) & M), 11) * P1) & M;
  h ^= h >> 33n; h = (h * P2) & M; h ^= h >> 29n; h = (h * P3) & M; h ^= h >> 32n;
  return h;
}

/* A zstd frame of stored blocks around `data`, with the frame header the system file has
   (`like`: that file's header bytes: descriptor, window, content size), and its content checksum
   when the descriptor asks for one. Without a model: single segment with a 4-byte size. */
function zstdStored(data, like) {
  const desc = like && like.length >= 6 ? like[4] : 0xa0;
  const single = (desc >> 5) & 1, fcs = desc >> 6, dict = desc & 3, sum = (desc >> 2) & 1;
  const windowByte = single ? 0 : (like ? like[5] : 0);
  const dictLen = dict === 3 ? 4 : dict, fcsLen = fcs === 0 ? single : fcs === 1 ? 2 : fcs === 2 ? 4 : 8;
  const head = 5 + (single ? 0 : 1) + dictLen + fcsLen;
  /* A block may hold at most the window, and no more than 128 KB. */
  const window = single ? data.length : Math.ceil((1 << (10 + (windowByte >> 3))) * (1 + (windowByte & 7) / 8));
  const BLOCK = Math.max(1, Math.min(128 * 1024, window)), blocks = Math.max(1, Math.ceil(data.length / BLOCK));
  const out = new Uint8Array(head + blocks * 3 + data.length + (sum ? 4 : 0));
  const dv = new DataView(out.buffer);
  out.set([0x28, 0xb5, 0x2f, 0xfd, desc]);
  let o = 5;
  if (!single) out[o++] = windowByte;
  if (dict && like) out.set(like.subarray(o, o + dictLen), o);
  o += dictLen;
  if (fcsLen === 1) out[o] = data.length;
  else if (fcsLen === 2) dv.setUint16(o, data.length - 256, true);
  else if (fcsLen === 4) dv.setUint32(o, data.length, true);
  else if (fcsLen === 8) dv.setBigUint64(o, BigInt(data.length), true);
  o += fcsLen;
  for (let b = 0; b < blocks; b++) {
    const start = b * BLOCK, size = Math.min(BLOCK, data.length - start), last = b === blocks - 1 ? 1 : 0;
    const h = (size << 3) | last;
    out[o] = h & 255; out[o + 1] = (h >> 8) & 255; out[o + 2] = (h >> 16) & 255;
    out.set(data.subarray(start, start + size), o + 3);
    o += 3 + size;
  }
  if (sum) dv.setUint32(o, Number(xxh64(data) & 0xffffffffn), true);
  return out;
}
