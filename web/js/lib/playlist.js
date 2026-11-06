"use strict";

/* The order things play in: segments [from, to] of the song (seconds), and
   where the repeating starts (in segment `seg`, at song time `at`). The home
   screen starts at the beginning of the first segment. */
function playPlan(p) {
  const M = [p.start, p.end], L = [p.loopStart, p.loopEnd];
  const later = p.playFrom !== null && p.playFrom !== undefined && p.playFrom > p.start + 0.05 && p.playFrom < (p.end || Infinity);
  switch (p.order) {
    case "loop-first": return { segs: [L, M], seg: 1, at: M[0] };
    case "alternate": return { segs: [L, M], seg: 0, at: L[0] };
    case "music-first":
      if (Math.abs(L[1] - M[1]) < 1e-6 && L[0] >= M[0] && L[0] < M[1]) return { segs: [M], seg: 0, at: L[0] };
      return { segs: [M, L], seg: 1, at: L[0] };
    default: return later ? { segs: [[p.playFrom, p.end], M], seg: 1, at: M[0] } : { segs: [M], seg: 0, at: M[0] };
  }
}

// "0:10.0 to 0:40.0" for a segment; `songEnd` resolves an end of 0.
const span = ([a, b], songEnd) => `${fmtClock(a)} to ${fmtClock(b || songEnd)}`;

// From the beginning of the home music, in plain words.
function describePlan(p, songEnd) {
  const plan = playPlan(p), M = [p.start, p.end], L = [p.loopStart, p.loopEnd];
  switch (p.order) {
    case "loop-first": return `Plays ${span(L, songEnd)} once, then repeats ${span(M, songEnd)}.`;
    case "alternate": return `Plays ${span(L, songEnd)}, then ${span(M, songEnd)}, over and over.`;
    case "music-first": return `Plays ${span(M, songEnd)} once, then repeats ${span(L, songEnd)}.`;
    default: return plan.segs.length > 1
      ? `Plays ${span(plan.segs[0], songEnd)} once, then repeats ${span(M, songEnd)}.`
      : `Repeats ${span(M, songEnd)}.`;
  }
}

function partLabel(t) {
  const part = themePart(t);
  if (part.order === "music" && !part.start && !part.end && part.playFrom === null) return fmtTime(t.music);
  return describePlan({ ...part, end: part.end || t.music, loopEnd: part.loopEnd || t.music }, t.music).replace(/\.$/, "");
}

/* What the home screen plays, cut from the theme's music in the chosen
   order. `intro` is where the repeating starts in the result. Every joint (a
   part into the next, and the jump back) is blended so it makes no click: a
   crossfade leads the sound before the joint into the sound just before
   where it continues (at the very start of the song, a fade instead); a fade
   dips the sound on both sides; a cut leaves it as it is. */
function buildHomeMusic(chans, rate, part) {
  const n = chans[0].length, at = v => Math.max(0, Math.min(n, Math.round(v * rate)));
  const p = { ...part };
  if (p.order !== "music" && (p.loopStart === null || p.loopStart === undefined)) p.order = "music";
  const plan = playPlan(p);
  const segs = plan.segs.map(([a, b]) => {
    const from = Math.min(at(a), n - 1);
    return [from, b > 0 ? Math.max(from + 1, at(b)) : n];
  });
  const shortest = Math.min(...segs.map(([a, b]) => b - a));
  const c = p.loop === "cut" ? 0 : Math.min(Math.round(p.blend * rate), Math.floor(shortest / 3));
  let target = Math.max(segs[plan.seg][0], Math.min(at(plan.at), segs[plan.seg][1] - 1));
  // A crossfade needs the sound before the loop start: at the very start of
  // the song, the repeating starts that much later instead.
  if (p.loop === "crossfade" && c > 0 && target < c && target + c < segs[plan.seg][1]) target += c;
  const offsets = [];
  let total = 0;
  for (const [a, b] of segs) { offsets.push(total); total += b - a; }
  const intro = offsets[plan.seg] + (target - segs[plan.seg][0]);
  // Joints: [position in the result, song position the sound continues from,
  // position in the result where that continuation sits].
  const joints = segs.slice(1).map(([a], i) => [offsets[i + 1], a, offsets[i + 1]]);
  joints.push([total, target, intro]);
  const out = chans.slice(0, 2).map(x => {
    const y = new Float32Array(total);
    segs.forEach(([a, b], i) => y.set(x.subarray(a, b), offsets[i]));
    for (const [end, from, landing] of joints) {
      if (p.loop === "crossfade" && c > 0 && from >= c) {
        for (let i = 0; i < c; i++) {
          const ang = (i + 0.5) / c * Math.PI / 2;
          y[end - c + i] = y[end - c + i] * Math.cos(ang) + x[from - c + i] * Math.sin(ang);
        }
      } else if (p.loop !== "cut" && c > 0) {
        for (let i = 0; i < c; i++) {
          const g = (i + 0.5) / c;
          y[end - 1 - i] *= g;
          y[landing + i] *= g;
        }
      }
    }
    return y;
  });
  // Where a sample of the result came from in the song (for the playhead).
  const source = i => {
    let k = segs.length - 1;
    while (k > 0 && i < offsets[k]) k--;
    return segs[k][0] + (i - offsets[k]);
  };
  return { chans: out, intro, source, joint: segs.length > 1 ? offsets[1] : -1 };
}

/* What the home screen plays for a playlist: each song's part in turn, every
   song first brought to the same loudness (the encoder then matches the whole
   to the console's music), joined as `pl.join` says: an overlapping crossfade,
   a fade out and in, or a straight cut. At the end it starts again from the
   first song ("loop-all", joined the same way), or the last song repeats its
   loop part ("loop-last", like a single song's "music part once, then the
   loop part repeats"; with no loop part its whole part repeats). `tracks`:
   [{ chans, start, end, loopStart, loopEnd }] in seconds of each song's audio,
   in play order (end 0 = to the end); `pl`: { join, blend (seconds), endMode }.
   `intro` is where the repeating starts in the result, in samples. */
const PLAYLIST_LEVEL = 0.1;   /* RMS each song is brought to before they are joined */
function buildPlaylist(tracks, rate, pl) {
  const keepLast = pl.endMode === "loop-last";
  const parts = tracks.map((t, k) => {
    const n = t.chans[0].length, at = v => Math.max(0, Math.min(n, Math.round(v * rate)));
    let y, intro = -1;
    if (keepLast && k === tracks.length - 1) {
      const end = t.end > 0 ? t.end : n / rate;
      const loop = t.loopStart !== null && t.loopStart !== undefined && t.loopEnd > t.loopStart;
      const built = buildHomeMusic(t.chans, rate, {
        start: t.start, end, order: "music-first", loop: pl.join, blend: pl.blend,
        loopStart: loop ? t.loopStart : t.start, loopEnd: loop ? t.loopEnd : end,
      });
      y = built.chans;
      intro = built.intro;
    } else {
      const a = Math.min(at(t.start), n - 1), b = t.end > 0 ? Math.max(a + 1, at(t.end)) : n;
      y = t.chans.slice(0, 2).map(c => c.slice(a, b));
    }
    let sum = 0, count = 0;
    for (const c of y) for (let i = 0; i < c.length; i++) { sum += c[i] * c[i]; count++; }
    const rms = Math.sqrt(sum / Math.max(count, 1)), g = rms > 1e-6 ? PLAYLIST_LEVEL / rms : 1;
    if (g !== 1) for (const c of y) for (let i = 0; i < c.length; i++) c[i] *= g;
    return { y, intro, len: y[0].length };
  });
  const c = pl.join === "cut" ? 0
    : Math.max(0, Math.min(Math.round(pl.blend * rate), Math.floor(Math.min(...parts.map(p => p.len)) / 3)));
  // A crossfade overlaps the songs; never as far as where a repeating last song lands.
  const overlap = k => pl.join !== "crossfade" || k === 0 ? 0 : parts[k].intro >= 0 ? Math.min(c, parts[k].intro) : c;
  const starts = [];
  let total = 0;
  parts.forEach((p, k) => { total -= overlap(k); starts.push(total); total += p.len; });
  const out = [new Float32Array(total), new Float32Array(total)];
  parts.forEach((p, k) => {
    const o = starts[k], ov = overlap(k);
    for (let ch = 0; ch < 2; ch++) {
      const y = out[ch], s = p.y[ch];
      for (let i = 0; i < ov; i++) {
        const ang = (i + 0.5) / ov * Math.PI / 2;
        y[o + i] = y[o + i] * Math.cos(ang) + s[i] * Math.sin(ang);
      }
      y.set(ov ? s.subarray(ov) : s, o + ov);
    }
    if (k > 0 && pl.join === "fade" && c > 0) {
      const f = p.intro >= 0 ? Math.min(c, p.intro) : c;
      for (let ch = 0; ch < 2; ch++) {
        for (let i = 0; i < c; i++) out[ch][o - 1 - i] *= (i + 0.5) / c;
        for (let i = 0; i < f; i++) out[ch][o + i] *= (i + 0.5) / f;
      }
    }
  });
  let intro = 0;
  if (keepLast) intro = starts[parts.length - 1] + parts[parts.length - 1].intro;
  else if (pl.join === "crossfade" && c > 0) {
    // The end crossfades into the first song's opening; it plays on from just after it.
    for (let ch = 0; ch < 2; ch++) {
      const y = out[ch], head = y.slice(0, c);
      for (let i = 0; i < c; i++) {
        const ang = (i + 0.5) / c * Math.PI / 2;
        y[total - c + i] = y[total - c + i] * Math.cos(ang) + head[i] * Math.sin(ang);
      }
    }
    intro = c;
  } else if (pl.join === "fade" && c > 0) {
    for (let ch = 0; ch < 2; ch++) for (let i = 0; i < c; i++) {
      out[ch][total - 1 - i] *= (i + 0.5) / c;
      out[ch][i] *= (i + 0.5) / c;
    }
  }
  return { chans: out, intro, starts, total };
}
