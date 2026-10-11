#!/usr/bin/env node
/* Encode test signals with web/atrac9.js and decode them again with it, for
   check.sh to compare against LibAtrac9 and FFmpeg.

     codec_test.js <outdir> [console.at9]

   With a console file, that file's format is the template and the file
   itself is decoded too. Writes per signal: <name>.at9, <name>.f32 (the
   input, stereo float) and <name>.js.raw (this decoder, 16-bit PCM), plus
   manifest.json. */
'use strict';

const fs = require('fs');
const path = require('path');
const ATRAC9 = require(path.join(__dirname, '../../web/atrac9.js'));
const { template } = require(path.join(__dirname, '../host/at9_fixture.js'));

const [out, consoleFile] = process.argv.slice(2);
const RATE = 48000;
let seed = 1;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff - 0.5;

function signal(seconds, fn) {
  const n = Math.max(1, Math.round(RATE * seconds)), L = new Float32Array(n), R = new Float32Array(n);
  for (let t = 0; t < n; t++) [L[t], R[t]] = fn(t, t / RATE);
  return [L, R];
}

/* [stereo input, lowest acceptable SNR of the 16-bit reference decode].
   The encoder keeps Sony's 13.5 kHz bandwidth: full-band noise and clicks
   keep about 44% of their energy above it, which alone caps them near 3.5 dB;
   -80 dBFS tones are only ~3 LSB, so 16-bit rounding caps that measurement
   near 19 dB. */
const SIGNALS = {
  music: [signal(20, (t, s) => {
    const env = 0.5 + 0.5 * Math.sin(2 * Math.PI * 0.25 * s), click = t % 24000 < 200 ? 0.4 * rnd() : 0;
    return [0.2 * Math.sin(2 * Math.PI * 220 * s) + 0.1 * Math.sin(2 * Math.PI * 1760 * s) * env + 0.03 * rnd() + click,
            0.2 * Math.sin(2 * Math.PI * 330 * s + 1) + 0.08 * Math.sin(2 * Math.PI * 5000 * s) * (1 - env) + 0.03 * rnd() - click];
  }), 20],
  noise: [signal(5, () => [0.1 * rnd(), 0.1 * rnd()]), 2],
  sweep: [signal(10, (t, s) => { const v = 0.3 * Math.sin(2 * Math.PI * (20 * s + 1000 * s * s)); return [v, -v]; }), 3],   // a third of it above 13.5 kHz
  clipped: [signal(4, (t, s) => { const v = Math.sin(2 * Math.PI * 100 * s) > 0 ? 1.5 : -1.5; return [v, v * 0.9]; }), 15],
  impulses: [signal(3, t => (t % 9600 === 0 ? [0.9, -0.9] : [0, 0])), 2],
  quiet: [signal(5, (t, s) => [1e-4 * Math.sin(2 * Math.PI * 440 * s), 1e-4 * Math.sin(2 * Math.PI * 550 * s)]), 15],
  silence: [signal(5, () => [0, 0]), null],
  short: [signal(100 / RATE, (t, s) => [0.5 * Math.sin(2 * Math.PI * 1000 * s), 0]), null],
};

function pcm16(channels) {
  const n = channels[0].length, ch = channels.length, b = Buffer.alloc(n * ch * 2);
  for (let t = 0; t < n; t++) for (let c = 0; c < ch; c++) {
    b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.floor(channels[c][t] * 32768 + 0.5))), (t * ch + c) * 2);
  }
  return b;
}

(async () => {
  fs.mkdirSync(out, { recursive: true });
  // Without a console file: a minute of soft chord in the console's format,
  // so the plugin's length rule (up to twice the system sound) has room.
  const stock = consoleFile ? fs.readFileSync(consoleFile)
    : (await ATRAC9.encode(template(3), ...signal(60, (t, s) => [0.02 * Math.sin(2 * Math.PI * 220 * s), 0.02 * Math.sin(2 * Math.PI * 330 * s)]))).bytes;
  fs.writeFileSync(path.join(out, 'template.at9'), stock);
  const manifest = { template: path.join(out, 'template.at9'), signals: [], console: null };

  if (consoleFile) {
    const file = ATRAC9.parseFile(stock);
    const t0 = Date.now();
    const { channels } = ATRAC9.decode(file);
    fs.writeFileSync(path.join(out, 'console.js.raw'), pcm16(channels));
    manifest.console = { path: consoleFile, channels: file.config.channels, ms: Date.now() - t0,
                         loudness: 20 * Math.log10(ATRAC9.downmixRms(file)) };
  }
  for (const [name, [[L, R], minSnr]] of Object.entries(SIGNALS)) {
    const t0 = Date.now();
    // "music" also plays an intro once before the part that loops.
    const intro = name === 'music' ? Math.round(2.5 * RATE) : 0;
    const res = await ATRAC9.encode(stock, L, R, null, intro);
    if (intro && (res.loop.start !== intro || res.loop.end !== L.length - 1)) throw new Error(`loop points ${JSON.stringify(res.loop)} for a ${intro}-sample intro`);
    const ms = Date.now() - t0;
    const file = ATRAC9.parseFile(res.bytes);
    fs.writeFileSync(path.join(out, `${name}.at9`), res.bytes);
    const input = Buffer.alloc(L.length * 8);
    for (let t = 0; t < L.length; t++) { input.writeFloatLE(L[t], t * 8); input.writeFloatLE(R[t], t * 8 + 4); }
    fs.writeFileSync(path.join(out, `${name}.f32`), input);
    fs.writeFileSync(path.join(out, `${name}.js.raw`), pcm16(ATRAC9.decode(file).channels));
    manifest.signals.push({ name, samples: L.length, delay: file.fact[2], channels: file.config.channels,
                            minSnr, encoderSnr: res.snr, worst: res.worst, ms });
  }
  fs.writeFileSync(path.join(out, 'manifest.json'), JSON.stringify(manifest, null, 1));
})().catch(e => { console.error(e.stack || e.message); process.exit(1); });
