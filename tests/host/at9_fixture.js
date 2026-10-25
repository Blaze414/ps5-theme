#!/usr/bin/env node
/* ATRAC9 fixtures for the end-to-end tests, made with the theme builder's own
   encoder (web/atrac9.js):

     at9_fixture.js stock <out> <seconds> [cci] [level] [precision]
         a stand-in for the console's bgm_home.at9: the same ATRAC9 format as
         13.42's (48 kHz 5.1, 800-byte superframes, loop chunk) by default,
         or channel layout `cci` (2 = stereo), holding a soft chord (peak
         `level`, default 0.05), coded at most at `precision` (default 7;
         13.42's sfx_transition.at9 never goes above 6)
     at9_fixture.js music <stock> <out> <seconds> <hz>
         a replacement encoded against <stock>, as the browser makes it

   Each run prints the encoder's own decode check (SNR) to stderr. */
'use strict';

const fs = require('fs');
const path = require('path');
const ATRAC9 = require(path.join(__dirname, '../../web/atrac9.js'));

function template(cci) {
  const channels = [1, 2, 2, 6, 8, 4][cci];
  const cfg = [0xfe, 0x70 | (cci << 1), 0x18, 0xf0];      // 48 kHz, 200-byte frames, 4 per superframe
  const fmt = Buffer.alloc(52);
  fmt.writeUInt16LE(0xfffe, 0); fmt.writeUInt16LE(channels, 2); fmt.writeUInt32LE(48000, 4);
  fmt.writeUInt32LE(37500, 8); fmt.writeUInt16LE(800, 12); fmt.writeUInt16LE(0, 14);
  fmt.writeUInt16LE(34, 16); fmt.writeUInt16LE(1024, 18);
  fmt.writeUInt32LE({ 1: 0x4, 2: 0x3, 4: 0x33, 6: 0x60f, 8: 0x63f }[channels], 20);
  Buffer.from(ATRAC9.GUID, 'hex').copy(fmt, 24);
  fmt.writeUInt32LE(1, 40);
  Buffer.from(cfg).copy(fmt, 44);
  const fact = Buffer.alloc(12);
  fact.writeUInt32LE(1, 0); fact.writeUInt32LE(256, 4); fact.writeUInt32LE(256, 8);
  const smpl = Buffer.alloc(60);
  smpl.writeUInt32LE(60, 12); smpl.writeUInt32LE(1, 28); smpl.writeUInt32LE(24, 32);
  const chunk = (id, body) => Buffer.concat([Buffer.from(id), Buffer.from(Uint32Array.of(body.length).buffer), body]);
  const body = Buffer.concat([Buffer.from('WAVE'), chunk('fmt ', fmt), chunk('fact', fact), chunk('smpl', smpl), chunk('data', Buffer.alloc(0))]);
  return Buffer.concat([Buffer.from('RIFF'), Buffer.from(Uint32Array.of(body.length).buffer), body]);
}

function tone(seconds, freqs, level) {
  const n = Math.round(48000 * seconds), L = new Float32Array(n), R = new Float32Array(n);
  for (let t = 0; t < n; t++) {
    let v = 0;
    freqs.forEach((f, i) => { v += Math.sin(2 * Math.PI * f * t / 48000 + i); });
    L[t] = level * v / freqs.length;
    R[t] = level * Math.sin(2 * Math.PI * freqs[0] * 1.5 * t / 48000);
  }
  return [L, R];
}

module.exports = { template };
if (require.main === module) (async () => {
  const [mode, a, b, c, d, e] = process.argv.slice(2);
  let res;
  if (mode === 'stock') {
    const [L, R] = tone(+b, [220, 277, 330], d === undefined ? 0.05 : +d);
    if (e !== undefined) ATRAC9.DEFAULT_PROFILE.maxPrecision = +e;
    res = await ATRAC9.encode(template(c === undefined ? 3 : +c), L, R);
    fs.writeFileSync(a, res.bytes);
  } else if (mode === 'music') {
    const [L, R] = tone(+c, [+d, +d * 2], 0.4);
    res = await ATRAC9.encode(fs.readFileSync(a), L, R);
    fs.writeFileSync(b, res.bytes);
  } else {
    throw new Error('usage: at9_fixture.js stock|music ...');
  }
  process.stderr.write(`at9_fixture: ${mode} ${res.samples} samples, ${res.bytes.length} bytes, SNR ${res.snr.toFixed(1)} dB\n`);
})().catch(e => { console.error(e.message); process.exit(1); });
