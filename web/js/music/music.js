"use strict";

/* ---- Home music: the music part, the loop part and their order ---- */

const LOOP_CHOICES = [
  ["crossfade", 1000, "Crossfade, 1 s"], ["crossfade", 2000, "Crossfade, 2 s"], ["crossfade", 4000, "Crossfade, 4 s"],
  ["fade", 1000, "Fade out and in"], ["cut", 0, "Straight cut"],
];
const ORDERS = [
  ["music", "Repeat the music part"],
  ["loop-first", "Loop part first, then the music part repeats"],
  ["alternate", "Loop part and music part take turns"],
  ["music-first", "Music part once, then the loop part repeats"],
];
const MIN_PART = 3;          /* seconds */
/* Seen on 13.42: the home screen's player keeps its place in time. It carries
   on after a game, and when the music changes under it (a theme is applied)
   it carries on in the new music from the same point. */
const MUSIC_HEAR_NOTE = "To hear the new home music after applying the theme, highlight a game or app that plays its own music, let it play, then go back to the home screen.";
const MUSIC_PLACE_NOTE = "The home screen keeps its place in the music: after a game it carries on where it stopped. When you apply a theme, the new music carries on from about where the old music had got to, so the first time it can start part-way through instead of at the beginning.";
const SNAP = 0.25;           /* seconds: a loop edge this close to the music's edge joins it */

/* A theme's music settings, in seconds: the music part (end 0 = to the end
   of the song), the loop part (null = none), their order and the joints. */
function themePart(t) {
  const loop = t.musicLoopStart >= 0 && t.musicLoopEnd >= 0;
  return {
    start: (t.musicStart || 0) / 1000, end: (t.musicEnd || 0) / 1000,
    playFrom: t.musicPlayFrom >= 0 ? t.musicPlayFrom / 1000 : null,
    loopStart: loop ? t.musicLoopStart / 1000 : null, loopEnd: loop ? t.musicLoopEnd / 1000 : null,
    order: loop ? t.musicOrder || "music" : "music",
    loop: t.musicLoop || "crossfade", blend: (t.musicBlend == null ? 2000 : t.musicBlend) / 1000,
  };
}

function fmtClock(sec) {
  const s = Math.max(0, sec), m = Math.floor(s / 60);
  return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`;
}

function parseClock(text) {
  const m = /^\s*(?:(\d+):)?(\d+(?:\.\d*)?)\s*$/.exec(text);
  return m ? (+(m[1] || 0)) * 60 + +m[2] : NaN;
}

/* Open the part editor. `chans` is the music at `rate`; `fresh` means it has
   not been saved to the theme yet. `track` makes it a playlist song's part:
   { slot, pos, adding, name }; its length is what the other songs leave. */
async function openTrim(id, chans, rate, part, fresh, track = null) {
  const stock = await stockAt9(HOME_MUSIC);
  const duration = chans[0].length / rate;
  let max = at9MaxSamples(stock) / stock.file.config.rate;        /* the whole result, which can be longer than the song */
  if (track) {
    const t = themeById(id), tracks = themeTracks(t), pl = themePlaylist(t), n = track.adding ? tracks.length + 1 : tracks.length;
    track.others = tracks.reduce((s, tr, k) => k === track.pos ? s : s + trackSeconds(tr, songDuration(t, tr.slot), pl, k === n - 1), 0);
    track.total = max;
    track.last = track.pos === n - 1;
    track.loopLast = track.last && pl.endMode === "loop-last";
    max -= track.others;
    if (max < MIN_PART) throw new Error(`The other songs already fill the console's ${fmtClock(track.total)}. Shorten one of them first.`);
  }
  const peaks = new Float32Array(2048);
  const per = chans[0].length / (peaks.length / 2);
  for (let b = 0; b < peaks.length / 2; b++) {
    let lo = 0, hi = 0;
    for (let i = Math.floor(b * per), stop = Math.min(chans[0].length, Math.floor((b + 1) * per)); i < stop; i++) {
      const v = (chans[0][i] + chans[1][i]) / 2;
      if (v < lo) lo = v; else if (v > hi) hi = v;
    }
    peaks[2 * b] = lo;
    peaks[2 * b + 1] = hi;
  }
  const end = part.end > part.start ? Math.min(part.end, duration) : duration;
  const hasLoop = part.loopStart !== null && part.loopStart !== undefined;
  state.trim = {
    id, chans, rate, duration, max, peaks, fresh, start: part.start, end, order: hasLoop ? part.order : "music",
    playFrom: part.playFrom !== null && part.playFrom !== undefined ? part.playFrom : part.start,
    loopStart: hasLoop ? part.loopStart : 0, loopEnd: hasLoop ? (part.loopEnd > 0 ? Math.min(part.loopEnd, duration) : duration) : 0,
    loop: part.loop, blend: part.blend, track,
  };
  if (track) {
    /* the last song, when the playlist ends by repeating it: its part once, then what repeats (at first, all of it) */
    state.trim.order = track.loopLast ? "music-first" : "music";
    state.trim.playFrom = state.trim.start;
    if (track.loopLast && !hasLoop) { state.trim.loopStart = state.trim.start; state.trim.loopEnd = end; }
  } else if (!hasLoop) defaultLoop(state.trim);
  clampTrim("end");
  if (!aspectReplaced(aspectById("music"))) setAspectFlag(aspectById("music"), true);
  renderAspect(aspectById("music"));
}

// A starting loop part for an order: the second half of the music part for
// "music part once", otherwise up to 30 seconds from its middle.
function defaultLoop(t) {
  const len = t.end - t.start;
  if (t.order === "music-first") {
    t.loopStart = t.start + len / 2;
    t.loopEnd = t.end;
  } else {
    t.loopStart = t.start + len * 0.4;
    t.loopEnd = Math.min(t.end, t.loopStart + Math.max(MIN_PART, Math.min(30, len * 0.3)));
  }
}

function trimBody() {
  const t = state.trim, loopOn = t.order !== "music", tr = t.track;
  const choice = LOOP_CHOICES.findIndex(([l, ms]) => l === t.loop && (l === "cut" || ms === Math.round(t.blend * 1000)));
  const handle = (id, label, v, loop) => `<button class="handle${loop ? " loop" : ""}" id="${id}" role="slider" aria-label="${label}"
      aria-valuemin="0" aria-valuemax="${t.duration.toFixed(1)}" aria-valuenow="${v.toFixed(1)}" aria-valuetext="${fmtClock(v)}" ${loop && !loopOn ? "hidden" : ""}></button>`;
  const field = (id, label, v) => `<label class="field">${label}<input type="text" id="${id}" value="${fmtClock(v)}" inputmode="decimal" autocomplete="off"></label>`;
  /* A playlist song has a part (and, when the playlist ends by repeating it, what repeats); the order and the joints are the playlist's. */
  const words = tr ? { part: "Part", starts: "Part starts", ends: "Part ends", loop: "Repeats", lstarts: "Repeats from", lends: "Repeats to" }
                   : { part: "Music part", starts: "Music part starts", ends: "Music part ends", loop: "Loop part", lstarts: "Loop part starts", lends: "Loop part ends" };
  const note = !tr ? "Choose the music part, and if you like a loop part and the order they play in."
    : tr.loopLast ? "Choose the part of this song that plays, and the part that then repeats until you leave the home screen."
    : "Choose the part of this song that plays in the playlist.";
  return `<div class="trim">
      ${tr ? `<h3 class="sub-head" style="margin-top:0">${esc(tr.name || trackLabel(t.id, tr.slot, tr.pos))}${tr.adding ? ` <span class="tag">New song</span>` : ""}</h3>` : ""}
      <p class="note">${note} Drag the handles, move a selected handle with the arrow keys, or type the times.</p>
      <div class="wave" id="wave"><canvas id="wave-canvas"></canvas>
        ${handle("trim-start", `${words.part} start`, t.start)}${handle("trim-end", `${words.part} end`, t.end)}
        ${handle("trim-lstart", words.lstarts, t.loopStart, true)}${handle("trim-lend", words.lends, t.loopEnd, true)}
        <button class="handle from" id="trim-from" role="slider" aria-label="Begins at" aria-valuemin="0"
          aria-valuemax="${t.duration.toFixed(1)}" aria-valuenow="${t.playFrom.toFixed(1)}" aria-valuetext="${fmtClock(t.playFrom)}" ${loopOn || tr ? "hidden" : ""}></button>
        <div class="start-flag" id="trim-flag" ${loopOn ? "" : "hidden"}></div>
        <div class="playhead" id="trim-playhead" hidden></div></div>
      <div class="legend"><span class="key start"></span>Begins here<span class="key music"></span>${words.part}${loopOn ? `<span class="key loop"></span>${words.loop}` : ""}</div>
      <div class="trim-fields">${field("trim-start-time", words.starts, t.start)}${field("trim-end-time", words.ends, t.end)}
        ${loopOn || tr ? "" : field("trim-from-time", "Begins at", t.playFrom)}</div>
      ${tr ? "" : `<div class="trim-fields">
        <label class="field">Play order
          <select id="trim-order">${ORDERS.map(([v, label]) => `<option value="${v}" ${v === t.order ? "selected" : ""}>${label}</option>`).join("")}</select></label>
      </div>`}
      ${loopOn ? `<div class="trim-fields">${field("trim-lstart-time", words.lstarts, t.loopStart)}${field("trim-lend-time", words.lends, t.loopEnd)}</div>` : ""}
      ${tr ? "" : `<div class="trim-fields">
        <label class="field">How the joints sound
          <select id="trim-loop">${LOOP_CHOICES.map(([, , label], i) => `<option value="${i}" ${i === (choice < 0 ? 1 : choice) ? "selected" : ""}>${label}</option>`).join("")}</select></label>
      </div>`}
      <p class="note" id="trim-summary"></p>
      ${tr ? "" : `<p class="note">${esc(MUSIC_PLACE_NOTE)}</p>`}
      <div class="trim-actions">
        <button class="btn sm" data-act="trim-play" id="trim-play">Play</button>
        ${!tr || tr.loopLast ? `<button class="btn sm" data-act="trim-seam">Hear where it loops</button>` : ""}
        <button class="btn sm" data-act="trim-joint" id="trim-joint" ${playPlan(trimPart(t)).segs.length > 1 ? "" : "hidden"}>Hear the change of part</button>
        <span class="spacer"></span>
        <button class="btn sm ghost" data-act="trim-cancel">Cancel</button>
        <button class="btn sm primary" data-act="trim-save">${tr && tr.adding ? "Add to the playlist" : "Use this"}</button>
      </div>
    </div>`;
}

const trimPart = t => ({
  start: t.start, end: t.end, loop: t.loop, blend: t.blend, order: t.order,
  playFrom: t.order === "music" && t.playFrom > t.start + 0.05 ? t.playFrom : null,
  loopStart: t.order === "music" ? null : t.loopStart, loopEnd: t.order === "music" ? null : t.loopEnd,
});

// Length of what the home music file holds, in seconds.
function trimLength(t) {
  return playPlan(trimPart(t)).segs.reduce((n, [a, b]) => n + (b - a), 0);
}

// Keep both parts valid: each at least MIN_PART long, within the song, and
// together within the console's length limit. `moved` names the edge that
// changed (start, end, lstart, lend).
function clampTrim(moved) {
  const t = state.trim, d = t.duration, min = Math.min(MIN_PART, d);
  const range = (a, b, movedA) => {
    t[a] = Math.max(0, Math.min(t[a], d - min));
    t[b] = Math.max(min, Math.min(t[b], d));
    if (t[b] - t[a] < min) { if (movedA) t[a] = t[b] - min; else t[b] = t[a] + min; }
  };
  const musicFirst = t.order === "music-first";
  range("start", "end", moved === "start");
  const keepFrom = () => {
    if (moved === "from" && t.playFrom - t.start < SNAP) t.playFrom = t.start;
    t.playFrom = Math.max(t.start, Math.min(t.playFrom, t.end - 1));
  };
  keepFrom();
  if (t.order !== "music") {
    if (musicFirst && (moved === "lend" || moved === "end") && Math.abs(t.loopEnd - t.end) < SNAP + (moved === "end" ? 0.5 : 0)) t.loopEnd = t.end;
    if (musicFirst && moved === "lstart" && Math.abs(t.loopStart - t.start) < SNAP) t.loopStart = t.start;
    range("loopStart", "loopEnd", moved === "lstart");
  }
  for (let pass = 0; pass < 4; pass++) {
    const over = trimLength(t) - t.max;
    if (over <= 1e-6) break;
    const shrinkLoop = (moved === "lstart" || moved === "lend") && t.order !== "music" && playPlan(trimPart(t)).segs.length > 1;
    if (shrinkLoop && moved === "lstart") t.loopStart += over;
    else if (shrinkLoop) t.loopEnd -= over;
    else if (moved === "start") t.start += over;
    else if (moved !== "from") t.end -= over;
    if (moved === "from") t.playFrom += over;
    range("start", "end", moved === "start");
    keepFrom();
    if (t.order !== "music") range("loopStart", "loopEnd", moved === "lstart");
    moved = pass ? (moved === "end" ? "lend" : "end") : moved;
  }
  if (t.track && t.order !== "music") {   /* a playlist's last song repeats within its own part */
    t.loopStart = Math.max(t.start, Math.min(t.loopStart, t.end - min));
    t.loopEnd = Math.max(t.loopStart + min, Math.min(t.loopEnd, t.end));
  }
}

function drawTrim() {
  const t = state.trim, canvas = $("#wave-canvas");
  if (!t || !canvas) return;
  const w = canvas.clientWidth, h = canvas.clientHeight, dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext("2d");
  ctx.scale(dpr, dpr);
  const loopOn = t.order !== "music";
  const X = v => v / t.duration * w, cols = t.peaks.length / 2;
  const x0 = X(t.start), x1 = X(t.end), l0 = loopOn ? X(t.loopStart) : -1, l1 = loopOn ? X(t.loopEnd) : -1;
  let peak = 1e-6;
  for (const v of t.peaks) peak = Math.max(peak, Math.abs(v));
  ctx.fillStyle = "rgba(255, 255, 255, .07)";
  ctx.fillRect(x0, 0, x1 - x0, h);
  if (loopOn) {
    ctx.fillStyle = "rgba(31, 124, 240, .22)";
    ctx.fillRect(l0, 0, l1 - l0, h);
  }
  const wave = h - 14;
  for (let x = 0; x < w; x++) {
    let lo = 0, hi = 0;
    for (let b = Math.floor(x / w * cols); b < Math.max(Math.floor((x + 1) / w * cols), Math.floor(x / w * cols) + 1); b++) {
      lo = Math.min(lo, t.peaks[2 * b]);
      hi = Math.max(hi, t.peaks[2 * b + 1]);
    }
    const y0 = wave / 2 - hi / peak * wave * 0.45, y1 = wave / 2 - lo / peak * wave * 0.45;
    ctx.fillStyle = (x >= x0 && x <= x1) || (x >= l0 && x <= l1) ? "rgba(255, 255, 255, .92)" : "rgba(255, 255, 255, .2)";
    ctx.fillRect(x, y0, 1, Math.max(1, y1 - y0));
  }
  ctx.fillStyle = "rgba(255, 255, 255, .55)";
  ctx.fillRect(x0, h - 12, x1 - x0, 3);
  if (loopOn) {
    ctx.fillStyle = "#1f7cf0";
    ctx.fillRect(l0, h - 6, l1 - l0, 4);
  }
  const first = playPlan(trimPart(t)).segs[0][0];
  ctx.fillStyle = "#3fcf8e";
  ctx.fillRect(X(first) - 1, 0, 2, h);
  $("#trim-flag").style.left = `${canvas.offsetLeft + X(first)}px`;
  for (const [id, v] of [["#trim-start", t.start], ["#trim-end", t.end], ["#trim-lstart", t.loopStart], ["#trim-lend", t.loopEnd], ["#trim-from", t.playFrom]]) {
    const el = $(id);
    el.style.left = `${canvas.offsetLeft + X(v)}px`;
    el.setAttribute("aria-valuenow", v.toFixed(1));
    el.setAttribute("aria-valuetext", fmtClock(v));
  }
  for (const [id, v] of [["#trim-start-time", t.start], ["#trim-end-time", t.end], ["#trim-lstart-time", t.loopStart], ["#trim-lend-time", t.loopEnd], ["#trim-from-time", t.playFrom]]) {
    const field = $(id);
    if (field && document.activeElement !== field) field.value = fmtClock(v);
  }
  const total = trimLength(t);
  if (t.track) {
    const tr = t.track, then = tr.loopLast ? "" : tr.last ? " Then the playlist starts again." : " Then the next song plays.";
    const whole = tr.loopLast && Math.abs(t.loopStart - t.start) < 0.05 && Math.abs(t.loopEnd - t.end) < 0.05;
    const what = !tr.loopLast ? `Plays ${span([t.start, t.end], t.duration)}.`
      : whole ? `Plays ${span([t.start, t.end], t.duration)}, then keeps repeating it.` : describePlan(trimPart(t), t.duration);
    $("#trim-summary").textContent = `${what}${then}` +
      ` The playlist in all: ${fmtClock(tr.others + total)} of up to ${fmtClock(tr.total)}.`;
  } else {
    const limit = total > t.max * 0.8 || t.duration > t.max ? ` In all ${fmtClock(total)}; the console allows up to ${fmtClock(t.max)}.` : "";
    $("#trim-summary").textContent = describePlan(trimPart(t), t.duration) + limit;
  }
  const joint = $("#trim-joint");
  if (joint) joint.hidden = playPlan(trimPart(t)).segs.length < 2;
}

const HANDLES = { start: "#trim-start", end: "#trim-end", lstart: "#trim-lstart", lend: "#trim-lend", from: "#trim-from" };
const EDGE_KEY = { start: "start", end: "end", lstart: "loopStart", lend: "loopEnd", from: "playFrom" };

function trimFromPointer(e, which) {
  const t = state.trim, r = $("#wave-canvas").getBoundingClientRect();
  t[EDGE_KEY[which]] = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * t.duration;
  clampTrim(which);
  drawTrim();
}

// Wire up the editor after it is rendered.
function setupTrim() {
  const wave = $("#wave");
  if (!wave || wave.dataset.ready) return;
  wave.dataset.ready = "1";
  let dragging = null;
  wave.addEventListener("pointerdown", e => {
    const t = state.trim, r = $("#wave-canvas").getBoundingClientRect(), v = (e.clientX - r.left) / r.width * t.duration;
    dragging = Object.keys(HANDLES).find(k => e.target.id === HANDLES[k].slice(1));
    if (!dragging) {
      // Nearest edge. The lower band of the waveform belongs to the loop part,
      // and with "repeat the music part" the upper band to the start point.
      const y = (e.clientY - r.top) / r.height;
      const lane = t.order !== "music" && y > 0.6 ? ["lstart", "lend"] : t.order === "music" && !t.track && y < 0.3 ? ["from"] : ["start", "end"];
      dragging = lane.reduce((a, b) => (Math.abs(v - t[EDGE_KEY[a]]) <= Math.abs(v - t[EDGE_KEY[b]]) ? a : b));
    }
    $(HANDLES[dragging]).classList.add("active");
    wave.setPointerCapture(e.pointerId);
    trimFromPointer(e, dragging);
    e.preventDefault();
  });
  wave.addEventListener("pointermove", e => { if (dragging) trimFromPointer(e, dragging); });
  const stop = () => {
    if (!dragging) return;
    $(HANDLES[dragging]).classList.remove("active");
    $(HANDLES[dragging]).focus();
    dragging = null;
  };
  wave.addEventListener("pointerup", stop);
  wave.addEventListener("pointercancel", stop);
  requestAnimationFrame(drawTrim);
}
