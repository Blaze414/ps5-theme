"use strict";

/* Interface sounds: the "sound/vag" entries of the interface's resource file
   (snd_focus_move, the sound of moving between items, and 106 others on
   13.42). They are Sony's HEVAG (High Efficiency VAG, VAGp version
   0x00020001): a 48-byte big-endian header ("VAGp", the version, the data
   size at 12, the sample rate at 16, the channel count at 0x1E, a name at
   0x20), then 16-byte frames, one per channel in turn: a shift and a
   predictor, a flag, and 28 four-bit samples. A sample is its code scaled
   by the shift, plus a prediction from the four samples before it by one of
   128 predictors. The predictors are vgmstream's (src/coding/psv_decoder.c,
   ISC licence: see the README's acknowledgements), as whole numbers of
   1/8192.

   All 107 of 13.42's sounds are laid out alike, byte for byte wherever the
   bytes don't depend on the sound: the header (apart from the data size),
   one silent frame per channel to open, each channel's last sound frame
   flagged 1, then the same closing frames (flag 7). A new sound takes those
   bytes from the console's own, and its frames use the classic VAG
   predictors 0-4, which HEVAG decodes as the original VAG does. Decoded
   again it must give exactly the samples the encoder meant, and the console
   checks the same layout (rco.c). */
const HEVAG_COEFS = Int16Array.of(
  0,0,0,0,7680,0,0,0,14720,-6656,0,0,12544,-7040,0,0,
  15616,-7680,0,0,14731,-7059,0,0,14507,-7366,0,0,13920,-7522,0,0,
  13133,-7680,0,0,12028,-7680,0,0,10764,-7680,0,0,9359,-7680,0,0,
  7832,-7680,0,0,6201,-7680,0,0,4488,-7680,0,0,2717,-7680,0,0,
  910,-7680,0,0,-910,-7680,0,0,-2717,-7680,0,0,-4488,-7680,0,0,
  -6201,-7680,0,0,-7832,-7680,0,0,-9359,-7680,0,0,-10764,-7680,0,0,
  -12028,-7680,0,0,-13133,-7680,0,0,-13920,-7522,0,0,-14507,-7366,0,0,
  -14731,-7059,0,0,5376,-9216,3328,-3072,-6400,-7168,-3328,-2304,-10496,-7424,-3584,-1024,
  -167,-2722,-494,-541,-7430,-2221,-2298,424,-8001,-3166,-2814,289,6018,-4750,2649,-1298,
  3798,-6946,3875,-1216,-8237,-2596,-2071,227,9199,1982,-1382,-2316,13021,-3044,-3792,1267,
  13112,-4487,-2250,1665,-1668,-3744,-6456,840,7819,-4328,2111,-506,9571,-1336,-757,487,
  10032,-2562,300,199,-4745,-4122,-5486,-1493,-5896,2378,-4787,-6947,-1193,-9117,-1237,-3114,
  2783,-7108,-1575,-1447,-7334,-2062,-2212,446,6127,-2577,-315,-18,9457,-1858,102,258,
  7876,-4483,2126,-538,-7172,-1795,-2069,482,-7358,-2102,-2233,440,-9170,-3509,-2674,-391,
  -2638,-2647,-1929,-1637,1873,9183,1860,-5746,9214,1859,-1124,-2427,13204,-3012,-4139,1370,
  12437,-4792,-256,622,-2653,-1144,-3182,-6878,9331,-1048,-828,507,1642,-620,-946,-4229,
  4246,-7585,-533,-2259,-8988,-3891,-2807,44,-2562,-2735,-1730,-1899,3182,-483,-714,-1421,
  7937,-3844,2821,-1019,10069,-2609,314,195,8400,-3297,1551,-155,-8529,-2775,-2432,-336,
  9477,-1882,108,256,75,-2241,-298,-6937,-9143,-4160,-2963,5,-7270,-1958,-2156,460,
  -2740,3745,5936,-1089,8993,1948,-683,-2704,13101,-2835,-3854,1055,9543,-1961,130,250,
  5272,-4270,3124,-3157,-7696,-3383,-2907,-456,7309,2523,434,-2461,10275,-2867,391,172,
  10940,-3721,665,97,24,-310,-1262,320,-8122,-2411,-2311,-271,-8511,-3067,-2337,163,
  326,-3846,419,-933,8895,2194,-541,-2880,12073,-1876,-2017,-601,8729,-3423,1674,-169,
  12950,-3847,-3007,1946,10038,-2570,302,198,9385,-2757,1008,41,-4720,-5006,-2852,-1161,
  7869,-4326,2135,-501,2450,-8597,1299,-2780,10192,-2763,360,181,11313,-4213,833,53,
  10154,-2716,345,185,9638,-1417,-737,482,3854,-4554,2843,-3397,6699,-5659,2249,-1074,
  11082,-3908,728,80,-1026,-9810,-805,-3462,10396,-3746,1367,-96,10287,988,-1915,-1437,
  7953,3878,-764,-3263,12689,-3375,-3354,2079,6641,3166,231,-2089,-2348,-7354,-1944,-4122,
  9290,-4039,1885,-246,4633,-6403,1748,-1619,11247,-4125,802,61,9807,-2284,219,222,
  9736,-1536,-706,473,8440,-3436,1562,-176,9307,-1021,-835,509,1698,-9025,688,-3037,
  10214,-2791,368,179,8390,3248,-758,-2989,7201,3316,46,-2614,-88,-7809,-538,-4571,
  6193,-5189,2760,-1245,12325,-1290,-3284,253,13064,-4075,-2824,1877,5333,2999,775,-1132);
const VAG_HEADER = 0x30, VAG_SAMPLES = 28;

const vagBe32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

/* A sound's layout; an error if it isn't laid out as the console's are. */
function vagInfo(b) {
  if (b.length < VAG_HEADER + 16 || b[0] !== 0x56 || b[1] !== 0x41 || b[2] !== 0x47 || b[3] !== 0x70 || vagBe32(b, 4) !== 0x00020001) {
    throw new Error("not one of the interface's sounds");
  }
  const channels = b[0x1e] || 1, size = vagBe32(b, 12), rate = vagBe32(b, 16), group = 16 * channels;
  if (channels > 2 || !rate || size !== b.length - VAG_HEADER || size % group || size / group < 3) throw new Error("a damaged sound");
  return { channels, rate, size, group, groups: size / group };
}

/* Decode the frame at `f`: 28 samples into out[at..], continuing from the four before it (h, newest first). The
   history is kept unrounded, as Sony's decoder keeps it; what plays is rounded. */
function vagFrame(b, f, h, out, at) {
  let idx = (b[f + 1] & 0xf0) | (b[f] >> 4);
  if (idx > 127) idx = 0;
  const shift = b[f] & 15, flag = b[f + 1] & 15, k = 4 * idx;
  const c0 = HEVAG_COEFS[k] / 8192, c1 = HEVAG_COEFS[k + 1] / 8192, c2 = HEVAG_COEFS[k + 2] / 8192, c3 = HEVAG_COEFS[k + 3] / 8192;
  for (let i = 0; i < VAG_SAMPLES; i++) {
    let s = 0;
    if (flag < 7) {   /* a closing frame is silence */
      const nib = ((b[f + 2 + (i >> 1)] >> (i & 1 ? 4 : 0)) << 28) >> 28;
      s = ((nib << 12) >> shift) + h[0] * c0 + h[1] * c1 + h[2] * c2 + h[3] * c3;
    }
    out[at + i] = Math.max(-32768, Math.min(32767, Math.round(s)));
    h[3] = h[2]; h[2] = h[1]; h[1] = h[0]; h[0] = s;
  }
}

/* A sound's samples per channel, as the console plays them (-32768..32767). */
function vagSamples(b) {
  const v = vagInfo(b);
  const chans = [...Array(v.channels)].map(() => new Int16Array(v.groups * VAG_SAMPLES));
  for (let c = 0; c < v.channels; c++) {
    const h = [0, 0, 0, 0];
    for (let g = 0; g < v.groups; g++) vagFrame(b, VAG_HEADER + g * v.group + 16 * c, h, chans[c], g * VAG_SAMPLES);
  }
  return { rate: v.rate, chans };
}

/* The same, as -1..1 channel data (to play, or to match loudness against). */
function vagDecode(b) {
  const s = vagSamples(b);
  return { rate: s.rate, chans: s.chans.map(c => Float32Array.from(c, x => x / 32768)) };
}

/* The most samples a new sound may hold: as many sound frames as the console's own. */
function vagMaxSamples(stock) {
  const v = vagInfo(stock);
  return (v.groups - 2) * VAG_SAMPLES;
}

/* What keeps `b` from being laid out as the console's sound `stock` is, byte for byte where the console's bytes
   don't depend on the sound ("" if nothing): the same checks as the console's (rco.c). */
function vagLayoutProblem(stock, b) {
  const s = vagInfo(stock);
  let v;
  try { v = vagInfo(b); } catch (e) { return e.message; }
  const same = (x0, y0, n) => { for (let i = 0; i < n; i++) if (stock[x0 + i] !== b[y0 + i]) return false; return true; };
  if (!same(0, 0, 12) || !same(16, 16, VAG_HEADER - 16)) return "its header differs from the console's";
  if (b.length > stock.length) return "it is longer than the console's";
  if (!same(VAG_HEADER, VAG_HEADER, s.group)) return "it doesn't start as the console's does";
  if (!same(stock.length - s.group, b.length - s.group, s.group)) return "it doesn't end as the console's does";
  for (let g = 1; g + 1 < v.groups; g++) {
    for (let c = 0; c < v.channels; c++) {
      const f = VAG_HEADER + g * v.group + 16 * c, flag = b[f + 1] & 15;
      if (flag !== (g + 2 === v.groups ? 1 : 0)) return "its frames are flagged differently from the console's";
      if ((b[f] & 15) > 12 || ((b[f + 1] & 0xf0) | (b[f] >> 4)) > 127) return "a frame is damaged";
    }
  }
  return "";
}

/* `chans` (-1..1, at the console's sound's sample rate) as a new sound in the console's layout: its header with
   the new length, its opening frames, the new frames, its closing frames. Each frame takes the predictor (0-4)
   and shift that come closest. Decoded again, the sound must give exactly the samples the encoder meant, and
   be laid out as the console's (vagLayoutProblem). */
function vagEncode(stock, chans) {
  const v = vagInfo(stock), n = Math.min(...chans.map(c => c.length));
  if (!n) throw new Error("the sound is empty");
  if (n > vagMaxSamples(stock)) throw new Error("the sound is longer than the console's");
  const src = chans.length === v.channels ? chans : v.channels === 2 ? [chans[0], chans[0]]
    : [Float32Array.from({ length: n }, (_, i) => chans.reduce((sum, c) => sum + c[i], 0) / chans.length)];
  const frames = Math.ceil(n / VAG_SAMPLES), groups = frames + 2, size = groups * v.group;
  const out = new Uint8Array(VAG_HEADER + size);
  out.set(stock.subarray(0, VAG_HEADER + v.group));                           /* header and opening frames */
  out.set(stock.subarray(stock.length - v.group), VAG_HEADER + size - v.group); /* closing frames */
  out[12] = size >>> 24; out[13] = (size >>> 16) & 255; out[14] = (size >>> 8) & 255; out[15] = size & 255;
  const x = new Float64Array(VAG_SAMPLES), q = new Int8Array(VAG_SAMPLES), bestQ = new Int8Array(VAG_SAMPLES);
  const meant = [];
  let signal = 0, noise = 0;
  for (let c = 0; c < v.channels; c++) {
    const want = new Int16Array(groups * VAG_SAMPLES);   /* the opening and closing frames play silence */
    let h1 = 0, h2 = 0;
    for (let fr = 0; fr < frames; fr++) {
      for (let i = 0; i < VAG_SAMPLES; i++) {
        const t = fr * VAG_SAMPLES + i;
        x[i] = t < n ? Math.max(-1, Math.min(1, src[c][t])) * 32767 : 0;
      }
      let best = Infinity, bf = 0, bs = 0;
      for (let f = 0; f <= 4; f++) {
        const c0 = HEVAG_COEFS[4 * f] / 8192, c1 = HEVAG_COEFS[4 * f + 1] / 8192;
        for (let shift = 0; shift <= 12; shift++) {
          const step = 1 << (12 - shift);
          let a = h1, b = h2, err = 0;
          for (let i = 0; i < VAG_SAMPLES && err < best; i++) {
            let k = Math.round((x[i] - (a * c0 + b * c1)) / step);
            k = k < -8 ? -8 : k > 7 ? 7 : k;
            const s = k * step + a * c0 + b * c1;   /* as the decoder adds it up */
            const y = Math.max(-32768, Math.min(32767, Math.round(s)));
            err += (x[i] - y) ** 2 + (s > 32767.49 || s < -32768.49 ? 1e15 : 0);   /* never rely on clipping */
            q[i] = k; b = a; a = s;
          }
          if (err < best) { best = err; bf = f; bs = shift; bestQ.set(q); }
        }
      }
      const o = VAG_HEADER + (fr + 1) * v.group + 16 * c;
      const c0 = HEVAG_COEFS[4 * bf] / 8192, c1 = HEVAG_COEFS[4 * bf + 1] / 8192, step = 1 << (12 - bs);
      out[o] = (bf << 4) | bs;                  /* predictors 0-4: the high bits in the next byte stay 0 */
      out[o + 1] = fr === frames - 1 ? 1 : 0;   /* each channel's last sound frame is flagged 1 */
      for (let i = 0; i < VAG_SAMPLES; i++) {
        const k = bestQ[i];
        out[o + 2 + (i >> 1)] |= (k & 15) << (i & 1 ? 4 : 0);
        const s = k * step + h1 * c0 + h2 * c1;
        const y = Math.max(-32768, Math.min(32767, Math.round(s)));
        want[(fr + 1) * VAG_SAMPLES + i] = y;
        if (fr * VAG_SAMPLES + i < n) { signal += x[i] * x[i]; noise += (x[i] - y) ** 2; }
        h2 = h1; h1 = s;
      }
    }
    meant.push(want);
  }
  const problem = vagLayoutProblem(stock, out);
  if (problem) throw new Error(`the converted sound did not verify: ${problem}`);
  const got = vagSamples(out).chans;
  for (let c = 0; c < v.channels; c++) {
    for (let i = 0; i < got[c].length; i++) if (got[c][i] !== meant[c][i]) throw new Error("the converted sound did not verify");
  }
  const snr = signal > 0 ? 10 * Math.log10(signal / Math.max(noise, 1e-9)) : Infinity;
  if (signal / (n * v.channels) > 10 && snr < 6) throw new Error("the converted sound did not verify");
  return { bytes: out, samples: n, snr };
}

/* The loudness of the loud part of a sound: the RMS of its 10 ms windows within 30 dB of the loudest. */
function loudRms(chans, rate) {
  const n = Math.min(...chans.map(c => c.length)), w = Math.max(1, Math.round(rate * 0.01)), powers = [];
  for (let s = 0; s < n; s += w) {
    let sum = 0, m = 0;
    for (const c of chans) for (let i = s; i < Math.min(n, s + w); i++) { sum += c[i] * c[i]; m++; }
    powers.push(sum / Math.max(m, 1));
  }
  const top = Math.max(0, ...powers), loud = powers.filter(p => p >= top / 1000);
  return top > 0 ? Math.sqrt(loud.reduce((a, b) => a + b, 0) / loud.length) : 0;
}

/* Where a sound gets loud: the start of its first 5 ms within 15 dB of its loudest 5 ms. Sony's own get there
   within 5-35 ms (moving, selecting, going back) and nearly all within 85 ms. */
function uiSoundHit(chans, rate) {
  const n = Math.min(...chans.map(c => c.length)), w = Math.max(1, Math.round(rate * 0.005)), p = [];
  for (let s = 0; s < n; s += w) {
    let e = 0, m = 0;
    for (const c of chans) for (let i = s; i < Math.min(n, s + w); i++) { e += c[i] * c[i]; m++; }
    p.push(e / Math.max(m, 1));
  }
  const top = Math.max(0, ...p);
  return top > 0 ? p.findIndex(v => v >= top / 10 ** 1.5) * w : 0;
}
const UI_SOUND_LEAD = 0.01;   /* seconds kept before a sound's hit, faded in */

/* Your audio for an interface sound: the silence before and after it trimmed, and anything quieter before its hit
   down to the last 10 ms (faded in), so it plays the moment you press as the console's do; no longer than `max`
   samples (cut with a short fade); as loud as the console's sound (`target`, by their loud parts), with a soft
   limit just below full scale. `lead`: the seconds of quieter sound cut before the hit. */
function uiSoundPrepare(chans, rate, target, max) {
  const n = Math.min(...chans.map(c => c.length)), quiet = 0.001;
  const heard = i => chans.some(c => Math.abs(c[i]) > quiet);
  let a = 0, z = n;
  while (a < n && !heard(a)) a++;
  while (z > a && !heard(z - 1)) z--;
  if (a >= z) throw new Error("the sound is silent");
  a = Math.max(0, a - Math.round(rate * 0.001));
  z = Math.min(n, z + Math.round(rate * 0.005));
  const hit = a + uiSoundHit(chans.map(c => c.subarray(a, z)), rate);
  const from = Math.max(a, hit - Math.round(rate * UI_SOUND_LEAD)), lead = from - a;
  a = from;
  const rise = lead > 0 ? hit - from : 0;   /* the part kept before the hit fades in, when quieter sound was cut */
  const cut = z - a > max, len = Math.min(z - a, max);
  const fade = cut ? Math.min(Math.round(rate * 0.03), Math.floor(len / 4)) : Math.min(Math.round(rate * 0.002), len);
  const out = chans.map(c => {
    const o = c.slice(a, a + len);
    for (let i = 0; i < Math.min(rise, len); i++) o[i] *= i / rise;
    for (let i = 0; i < fade; i++) o[len - fade + i] *= 1 - (i + 1) / fade;
    return o;
  });
  const want = loudRms(target, rate), have = loudRms(out, rate);
  const g = want > 0 && have > 0 ? want / have : 1, knee = 0.89;
  for (const o of out) {
    for (let i = 0; i < o.length; i++) {
      const v = o[i] * g, m = Math.abs(v);
      o[i] = m <= knee ? v : Math.sign(v) * (knee + (1 - knee) * Math.tanh((m - knee) / (1 - knee)));
    }
  }
  return { chans: out, cut, seconds: len / rate, lead: lead / rate };
}
