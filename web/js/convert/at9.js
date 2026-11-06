"use strict";

/* ---- ATRAC9: the console's own sounds and home music (atrac9.js) ---- */

const at9Stock = new Map();   /* system .at9 path -> { bytes, file, rms } */

async function stockAt9(path) {
  if (!at9Stock.has(path)) {
    const res = await fetch(`/api/asset?${q({ path })}`);
    if (!res.ok) throw new Error("could not read the system sound");
    const bytes = new Uint8Array(await res.arrayBuffer());
    const file = ATRAC9.parseFile(bytes);
    at9Stock.set(path, { bytes, file, rms: ATRAC9.downmixRms(file) });
  }
  return at9Stock.get(path);
}

/* The longest audio a replacement may hold: the console accepts up to twice
   the system file's length, and looping music also repeats its opening
   seconds after the end (see encode in atrac9.js). */
function at9MaxSamples(stock) {
  const f = stock.file, cfg = f.config, delay = f.fact ? f.fact[2] : cfg.frameSamples;
  const tail = f.loop ? ATRAC9.LOOP_TAIL * cfg.rate : 0;
  return 2 * f.superframes * cfg.superframeSamples - Math.max(delay, cfg.frameSamples) - cfg.superframeSamples - tail;
}

function trimWithFade(chans, max, rate) {
  if (chans[0].length <= max) return chans;
  const fade = Math.min(max, 3 * rate);
  return chans.map(c => {
    const out = c.slice(0, max);
    for (let i = 0; i < fade; i++) out[max - fade + i] *= 1 - (i + 1) / fade;
    return out;
  });
}

/* Scale to the system sound's loudness (RMS of its stereo downmix), offset
   by gainDb, with a soft limit just below full scale. */
function matchLoudness(chans, target, gainDb) {
  let sum = 0, n = 0;
  for (const c of chans) for (let i = 0; i < c.length; i++) { sum += c[i] * c[i]; n++; }
  const rms = Math.sqrt(sum / Math.max(n, 1));
  if (!(rms > 1e-6)) return chans;
  const g = (target > 0 ? target : 0.1) / rms * 10 ** (gainDb / 20), knee = 0.89;
  return chans.map(c => c.map(v => {
    v *= g;
    const a = Math.abs(v);
    return a <= knee ? v : Math.sign(v) * (knee + (1 - knee) * Math.tanh((a - knee) / (1 - knee)));
  }));
}

/* Run the encoder in a Web Worker (atrac9.js doubles as one); on the page
   itself, with pauses to repaint, if a worker cannot start. */
function encodeAt9(stockBytes, left, right, onProgress, intro) {
  return new Promise((resolve, reject) => {
    const onPage = async () => {
      let last = performance.now();
      try {
        resolve(await ATRAC9.encode(stockBytes, left, right, async f => {
          if (onProgress) onProgress(f);
          if (performance.now() - last > 50) { await nextTask(); last = performance.now(); }
        }, intro));
      } catch (e) { reject(e); }
    };
    let worker;
    try { worker = new Worker("atrac9.js"); } catch (e) { onPage(); return; }
    let started = false;
    worker.onmessage = e => {
      const m = e.data;
      started = true;
      if (m.progress !== undefined) { if (onProgress) onProgress(m.progress); return; }
      worker.terminate();
      if (m.error) reject(new Error(m.error)); else resolve(m.result);
    };
    worker.onerror = e => {
      e.preventDefault();
      worker.terminate();
      if (started) reject(new Error("the encoder stopped unexpectedly")); else onPage();
    };
    worker.postMessage({ stock: stockBytes, left, right, intro });
  });
}

/* Encode audio as a replacement for the system ATRAC9 sound at `path`: same
   format, matched loudness, decoded again and compared before it is returned.
   `intro` (seconds) plays once before the part that loops. */
async function at9For(path, chans, rate, gainDb, onProgress, intro = 0) {
  const stock = await stockAt9(path);
  const cfg = stock.file.config;
  let st = await resample(chans.length === 1 ? [chans[0], chans[0]] : chans.slice(0, 2), rate, cfg.rate);
  const max = at9MaxSamples(stock), trimmed = st[0].length > max;
  st = matchLoudness(trimWithFade(st, max, cfg.rate), stock.rms, gainDb);
  const introSamples = Math.min(Math.round(intro * cfg.rate), Math.max(0, st[0].length - cfg.rate));
  const res = await encodeAt9(stock.bytes, st[0], st[1], onProgress, introSamples);
  return { blob: new Blob([res.bytes], { type: "audio/x-at9" }), seconds: res.samples / cfg.rate, trimmed };
}

async function soundForAsset(file, assetPath) {
  if (ext(assetPath) === "at9") {
    if (/\.at9$/i.test(file.name)) return file;  /* made elsewhere: the console checks the exact format */
    const stock = await stockAt9(assetPath);
    const chans = await decodeAudio(file, stock.file.config.rate, 2);
    return (await at9For(assetPath, chans, stock.file.config.rate, 0, attachProgress)).blob;
  }
  const stock = parseWav(await (await fetch(`/api/asset?${q({ path: assetPath })}`)).arrayBuffer());
  if (!stock) throw new Error("could not read the system sound");
  if (stock.format !== 1 || stock.bits !== 16) {
    if (!/\.wav$/i.test(file.name)) throw new Error(`attach a WAV in the same format (${stock.rate} Hz, ${stock.bits}-bit)`);
    return file;  /* the console checks the exact format */
  }
  return audioToWav(file, stock.rate, stock.channels);
}
