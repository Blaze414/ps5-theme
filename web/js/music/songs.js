"use strict";

/* ---- Home music: a playlist of songs ----

   A theme can keep up to MUSIC_MAX_TRACKS songs (slot 1 is music.wav, the
   others music2.wav …, as the plugin stores them). They play one after
   another, then start again, or the last one keeps repeating. The plugin
   keeps the songs, their order and parts; this page puts them together into
   the one home music file, as it does a single song. Slot 1 holds a song
   whenever the theme has music, so what looks for music.wav still finds it. */

const MUSIC_MAX_TRACKS = 8;
const SONG_PRE = 5;                  /* seconds kept before a song's part: crossfades reach back into it */
const toMs = v => Math.round(v * 1000);

/* The theme's songs in play order: [{ slot, start, end }] in seconds (end 0 = to the end of the song). */
function themeTracks(t) {
  if (!t) return [{ slot: 1, start: 0, end: 0 }];
  const seq = t.musicSequence && t.musicSequence.length ? t.musicSequence : [1];
  return seq.map((slot, i) => ({
    slot,
    start: ((t.musicStarts || [])[i] ?? (slot === 1 ? t.musicStart || 0 : 0)) / 1000,
    end: ((t.musicEnds || [])[i] ?? (slot === 1 ? t.musicEnd || 0 : 0)) / 1000,
  }));
}
const isPlaylist = t => !!t && themeTracks(t).length > 1;

/* How the songs join and how the playlist ends, with the last song's repeating part (seconds), if it has one. */
function themePlaylist(t) {
  const loop = t.musicLoopStart >= 0 && t.musicLoopEnd > t.musicLoopStart;
  return {
    join: t.musicBetween || t.musicLoop || "crossfade",
    blend: (t.musicBlend == null ? 2000 : t.musicBlend) / 1000,
    endMode: t.musicEndMode === "loop-last" ? "loop-last" : "loop-all",
    loopStart: loop ? t.musicLoopStart / 1000 : null, loopEnd: loop ? t.musicLoopEnd / 1000 : null,
  };
}

/* Songs read from the console, kept while the page stays open: "id|slot" ->
   { chans, rate, from, to, duration } (seconds), only the part the playlist
   plays and a little before it. */
const songCache = new Map();
function songCacheDrop(id, slot) {
  for (const k of [...songCache.keys()]) if (slot === undefined ? k.startsWith(`${id}|`) : k === `${id}|${slot}`) songCache.delete(k);
}
function songDuration(t, slot) {
  const c = songCache.get(`${t.id}|${slot}`);
  return c ? c.duration : slot === 1 ? t.music || 0 : 0;
}
function keepSong(id, slot, chans, rate, from, to) {
  const a = Math.max(0, Math.floor(from * rate)), b = Math.max(a + 1, Math.min(chans[0].length, Math.ceil(to * rate)));
  songCache.set(`${id}|${slot}`, { chans: chans.slice(0, 2).map(c => c.slice(a, b)), rate, from: a / rate, to: b / rate, duration: chans[0].length / rate });
}

/* How long a song plays in the playlist, in seconds (`dur`: the song's length, for an end of 0). */
function trackSeconds(tr, dur, pl, last) {
  const end = tr.end > 0 ? tr.end : dur;
  let len = Math.max(0, end - tr.start);
  if (last && pl.endMode === "loop-last" && pl.loopStart !== null && Math.abs(pl.loopEnd - end) > 1e-3) len += pl.loopEnd - pl.loopStart;
  return len;
}
function playlistSeconds(t) {
  const tracks = themeTracks(t), pl = themePlaylist(t);
  return tracks.reduce((n, tr, i) => n + trackSeconds(tr, songDuration(t, tr.slot), pl, i === tracks.length - 1), 0);
}

const songTitle = file => file.name.replace(/\.[^.]+$/, "").slice(0, 80);
/* A song's name (from the file it came from), or its place in the playlist. */
function trackLabel(id, slot, pos) { return (id === state.choicesId && songName(slot)) || `Song ${pos + 1}`; }

function freeSlot(t) {
  const used = new Set(themeTracks(t).map(x => x.slot));
  for (let s = 2; s <= MUSIC_MAX_TRACKS; s++) if (!used.has(s)) return s;
  return 0;
}

/* A song from the console, decoded whole. */
async function fetchSong(id, slot, onProgress) {
  const res = await fetch(`/api/theme/music?${q({ id, slot })}`);
  if (!res.ok) throw new Error("a song could not be read from the console");
  const bytes = await readBytes(res, onProgress);
  const { chans, rate } = wavChannels(bytes.byteOffset === 0 && bytes.buffer.byteLength === bytes.length ? bytes.buffer : bytes.slice().buffer);
  return { chans: chans.length === 1 ? [chans[0], chans[0]] : chans, rate };
}

/* The part of a song the playlist plays: kept from before, or read from the console. */
async function songPart(id, tr, onProgress) {
  const key = `${id}|${tr.slot}`, from = Math.max(0, tr.start - SONG_PRE);
  let c = songCache.get(key);
  if (!c || c.from > from + 1e-3 || c.to < (tr.end > 0 ? Math.min(tr.end, c.duration) : c.duration) - 1e-3) {
    const s = await fetchSong(id, tr.slot, onProgress);
    const dur = s.chans[0].length / s.rate;
    keepSong(id, tr.slot, s.chans, s.rate, from, tr.end > 0 ? Math.min(tr.end, dur) : dur);
    c = songCache.get(key);
  }
  return c;
}

/* The playlist put together (buildPlaylist), its songs read first; the progress fills up to `upTo`. */
async function playlistAudio(id, p, upTo) {
  const t = themeById(id), tracks = themeTracks(t), pl = themePlaylist(t), input = [];
  let rate = MUSIC_RATE;
  for (let i = 0; i < tracks.length; i++) {
    const tr = tracks[i], last = i === tracks.length - 1, label = `Reading song ${i + 1} of ${tracks.length}…`;
    if (p) p.set(label, i / tracks.length * upTo, (i + 1) / tracks.length * upTo);
    const c = await songPart(id, tr, f => { if (p && f >= 0) p.set(label, (i + f) / tracks.length * upTo, (i + 1) / tracks.length * upTo); });
    const end = tr.end > 0 ? Math.min(tr.end, c.duration) : c.duration;
    input.push({
      chans: c.chans, start: tr.start - c.from, end: end - c.from,
      loopStart: last && pl.loopStart !== null ? pl.loopStart - c.from : null,
      loopEnd: last && pl.loopEnd !== null ? pl.loopEnd - c.from : null,
    });
    rate = c.rate;
  }
  return { built: buildPlaylist(input, rate, pl), rate, count: tracks.length };
}

/* Draw a section again, keeping the focus on the same control (a list that moved under it keeps its place). */
function renderKeepingFocus(asp) {
  const f = document.activeElement, box = $(`#aspect-${asp.id}`);
  const inside = f && box && box.contains(f), act = inside && f.dataset.act, pos = inside && f.dataset.pos, fid = inside && f.id;
  renderAspect(asp);
  if (!inside) return;
  const again = fid ? document.getElementById(fid) : act ? $(`[data-act="${act}"]${pos !== undefined ? `[data-pos="${pos}"]` : ""}`, box) : null;
  if (again && !again.disabled) again.focus();
}

/* Put the playlist together into the theme's home music and send it, one at a
   time: a change made while it runs makes it again afterwards, as things are then. */
let rebuilding = null, rebuildAgain = false;
function rebuildPlaylist(id) {
  if (rebuilding) { rebuildAgain = true; return rebuilding; }
  rebuilding = (async () => {
    try {
      do { rebuildAgain = false; await rebuildPlaylistNow(id); } while (rebuildAgain && state.editId === id);
    } finally { rebuilding = null; }
  })();
  return rebuilding;
}
async function rebuildPlaylistNow(id) {
  const p = progressToast("Making the home music…");
  try {
    const a = await playlistAudio(id, p, 0.2);
    const r = await convertHomeMusic(id, a.built, a.rate, (themeById(id) || {}).gain || 0, p);
    stamp = Date.now();
    await refreshStatus();
    p.done(`Home music set and verified: ${plural(a.count, "song")}, ${fmtClock(r.seconds)}${r.trimmed ? ", cut to the console's limit" : ""}. Apply the theme to hear it.`, "ok");
  } catch (e) {
    await refreshStatus();
    p.done(e.message, "err");
  }
  if (state.route === "edit" && state.editId === id && !state.trim) renderKeepingFocus(aspectById("music"));
}

/* The theme's home music made again from its songs: the playlist, or the one song. */
function remakeHomeMusic(id) {
  const t = themeById(id);
  return isPlaylist(t) ? rebuildPlaylist(id) : reencodeMusic(id, (t && t.gain) || 0);
}

function playlistBody(t) {
  const id = t.id, tracks = themeTracks(t), pl = themePlaylist(t), total = playlistSeconds(t);
  if (!state.musicMax) {
    stockAt9(HOME_MUSIC).then(s => {
      state.musicMax = at9MaxSamples(s) / s.file.config.rate;
      if (state.route === "edit" && state.editId === id && !state.trim) renderKeepingFocus(aspectById("music"));
    }).catch(() => {});
  }
  const max = state.musicMax, converted = state.files.has(HOME_MUSIC);
  if (converted) checkHomeMusic(id);
  const choice = LOOP_CHOICES.findIndex(([l, ms]) => l === pl.join && (l === "cut" || ms === Math.round(pl.blend * 1000)));
  const rows = tracks.map((tr, i) => {
    const last = i === tracks.length - 1, dur = songDuration(t, tr.slot), end = tr.end > 0 ? tr.end : dur;
    const name = trackLabel(id, tr.slot, i), len = trackSeconds(tr, dur, pl, last);
    const part = end > tr.start ? `${fmtClock(tr.start)} to ${fmtClock(end)}` : `From ${fmtClock(tr.start)} to the end`;
    const repeat = !last || pl.endMode !== "loop-last" ? ""
      : pl.loopStart !== null && Math.abs(pl.loopEnd - end) > 1e-3 ? `, then repeats ${fmtClock(pl.loopStart)} to ${fmtClock(pl.loopEnd)}`
      : pl.loopStart !== null && pl.loopStart > tr.start + 0.05 ? `, then repeats from ${fmtClock(pl.loopStart)}` : ", then repeats";
    return `<div class="row-item replaced">
        <div class="info"><div class="file">${i + 1}. ${esc(name)}</div>
          <div class="dir">${esc(part + repeat)}${len ? ` · ${fmtTime(len)}` : ""}</div></div>
        <div class="acts">
          <button class="btn sm ghost" data-act="song-up" data-pos="${i}" aria-label="Play ${esc(name)} earlier" ${i === 0 ? "disabled" : ""}>↑</button>
          <button class="btn sm ghost" data-act="song-down" data-pos="${i}" aria-label="Play ${esc(name)} later" ${last ? "disabled" : ""}>↓</button>
          <button class="btn sm" data-act="song-part" data-pos="${i}">Choose part</button>
          <button class="btn sm" data-act="song-replace" data-pos="${i}">Replace</button>
          <button class="btn sm ghost danger" data-act="song-remove" data-pos="${i}">Remove</button>
        </div></div>`;
  }).join("");
  return `<div class="row-item replaced">
      <div class="info"><div class="file">Playlist · ${plural(tracks.length, "song")}${total ? ` · ${fmtTime(total)}` : ""}</div>
        <div class="dir" id="home-music-status">${converted ? "Checking the home music file…" : "Not made into the home music yet."}</div></div>
      <div class="acts">
        <button class="btn sm primary" data-act="convert-music" id="home-music-fix" ${converted ? "hidden" : ""}>${converted ? "Encode again" : "Make the home music"}</button>
        <button class="btn sm" data-act="pl-play" id="pl-play">Play</button>
        <button class="btn sm" data-act="pl-seam">Hear where it repeats</button>
        <button class="btn sm ghost danger" data-act="remove-music">Remove all</button>
      </div></div>
    <div class="rows pl-songs">${rows}</div>
    <div class="pl-foot">
      <button class="btn sm" data-act="song-add" ${tracks.length >= MUSIC_MAX_TRACKS ? "disabled" : ""}>Add song</button>
      <div class="pl-meter${max && total > max * 0.95 ? " full" : ""}">
        <div class="progress" role="progressbar" aria-label="Length of the playlist" aria-valuemin="0" aria-valuemax="${Math.round(max || 0)}" aria-valuenow="${Math.round(total)}"><i style="width:${max ? Math.min(100, total / max * 100).toFixed(1) : 0}%"></i></div>
        <span>${max ? `${fmtTime(total)} of up to ${fmtTime(max)}` : "Reading the console's limit…"}${help("The console plays home music up to about twice as long as its own. Each song's part counts, and what a last song repeats; a crossfade between songs makes the whole a little shorter.")}</span>
      </div></div>
    <div class="trim-fields">
      <label class="field">How the songs join
        <select id="pl-join">${LOOP_CHOICES.map(([, , label], i) => `<option value="${i}" ${i === (choice < 0 ? 1 : choice) ? "selected" : ""}>${label}</option>`).join("")}</select></label>
      <label class="field">At the end
        <select id="pl-end">
          <option value="loop-all" ${pl.endMode === "loop-all" ? "selected" : ""}>Start again from the first song</option>
          <option value="loop-last" ${pl.endMode === "loop-last" ? "selected" : ""}>Keep repeating the last song</option>
        </select></label>
    </div>
    <label class="field">Loudness <span id="gain-label">${esc(gainLabel(t.gain || 0))}</span>
      <input type="range" min="-12" max="12" step="1" value="${t.gain || 0}" id="music-gain"></label>
    <p class="note">Each song is brought to the same loudness first, so none is much louder than the others.${converted ? ` ${esc(MUSIC_HEAR_NOTE)} ${esc(MUSIC_PLACE_NOTE)}` : ""}</p>`;
}

/* Adding or replacing a song opens the part editor; nothing is saved until "Use this". */
async function addSong() {
  const id = state.editId, t = themeById(id), slot = freeSlot(t);
  if (!slot) return toast(`A playlist holds up to ${MUSIC_MAX_TRACKS} songs.`, "err");
  const file = await pickFile("audio/*");
  if (file) await openSongEditor(id, file, { slot, pos: themeTracks(t).length, adding: true });
}
async function replaceSong(pos) {
  const id = state.editId, tr = themeTracks(themeById(id))[pos];
  if (!tr) return;
  const file = await pickFile("audio/*");
  if (file) await openSongEditor(id, file, { slot: tr.slot, pos });
}
async function openSongEditor(id, file, track) {
  const p = progressToast("Reading music…");
  try {
    let chans = await decodeAudio(file, MUSIC_RATE, 2);
    const keep = Math.floor(MUSIC_SOURCE_MAX_BYTES / 4), long = chans[0].length > keep;
    if (long) chans = trimWithFade(chans, keep, MUSIC_RATE);
    const pl = themePlaylist(themeById(id));
    track.name = songTitle(file);
    await openTrim(id, chans, MUSIC_RATE, { start: 0, end: 0, loop: pl.join, blend: pl.blend }, true, track);
    p.done(long ? `Only the first ${fmtTime(keep / MUSIC_RATE)} of the song can be kept. Choose the part to use.`
      : "Choose the part of the song to use.", "");
    const handle = $("#trim-start");
    if (handle) handle.focus();
  } catch (e) { p.done(e.message, "err"); }
}
async function openSongPart(pos) {
  const id = state.editId, t = themeById(id), tracks = themeTracks(t), tr = tracks[pos];
  if (!tr) return;
  const pl = themePlaylist(t), repeats = pos === tracks.length - 1 && pl.endMode === "loop-last" && pl.loopStart !== null;
  const p = progressToast("Reading the song…");
  try {
    const s = await fetchSong(id, tr.slot, f => { if (f >= 0) p.set("Reading the song…", f); });
    await openTrim(id, s.chans, s.rate, {
      start: tr.start, end: tr.end, loop: pl.join, blend: pl.blend, order: "music",
      loopStart: repeats ? pl.loopStart : null, loopEnd: repeats ? pl.loopEnd : null,
    }, false, { slot: tr.slot, pos, name: songName(tr.slot) });
    p.done("Choose the part of the song to use.", "");
    const handle = $("#trim-start");
    if (handle) handle.focus();
  } catch (e) { p.done(e.message, "err"); }
}

async function saveSongPart() {
  const t = state.trim, tr = t.track, id = t.id;
  stopPreview();
  const p = progressToast(tr.adding ? "Adding the song…" : "Saving the song's part…");
  try {
    const seq = themeTracks(themeById(id)).map(x => x.slot);
    if (t.fresh) {
      p.set("Sending the song to the console…", 0, 0.9);
      await uploadVerified("/api/theme/music", { id, slot: tr.slot }, pcmWav(t.chans, t.rate));
      setSongNames(names => { if (tr.name) names[tr.slot] = tr.name; else delete names[tr.slot]; });
    }
    const params = tr.slot === 1 ? { start: toMs(t.start), end: toMs(t.end), play_from: -1 }
                                 : { [`start${tr.slot}`]: toMs(t.start), [`end${tr.slot}`]: toMs(t.end) };
    if (tr.adding) {
      /* a new last song: nothing of the old last one's repeat carries over; one song becomes the first of a playlist */
      const next = seq.concat(tr.slot);
      Object.assign(params, { sequence: next.join(","), tracks: next.length, loop_start: -1, loop_end: -1 });
      if (seq.length === 1) params.order = "music";
    }
    if (tr.loopLast) Object.assign(params, { loop_start: toMs(t.loopStart), loop_end: toMs(t.loopEnd) });
    await api("POST", "/api/theme/settings", { id, ...params });
    keepSong(id, tr.slot, t.chans, t.rate, Math.max(0, t.start - SONG_PRE), t.end);
    state.trim = null;
    await refreshStatus();
    p.done("", "");
  } catch (e) {
    await refreshStatus();
    p.done(e.message, "err");
    renderAspect(aspectById("music"));
    return;
  }
  renderAspect(aspectById("music"));
  await rebuildPlaylist(id);
}

async function removeSong(pos) {
  const id = state.editId, tracks = themeTracks(themeById(id)), gone = tracks[pos];
  if (!gone || tracks.length < 2) return;
  stopPreview();
  const p = progressToast("Removing the song…");
  try {
    const rest = tracks.filter((_, k) => k !== pos), params = {};
    if (gone.slot === 1) {
      /* music.wav stays the theme's song: the next song moves into it */
      const mover = rest[0];
      p.set("Moving a song…", 0.05, 0.9);
      const res = await fetch(`/api/theme/music?${q({ id, slot: mover.slot })}`);
      if (!res.ok) throw new Error("a song could not be read from the console");
      await uploadVerified("/api/theme/music", { id, slot: 1 }, new Blob([await res.arrayBuffer()]));
      await api("DELETE", "/api/theme/music", { id, slot: mover.slot });
      const moved = mover.slot;
      setSongNames(names => { if (names[moved]) names[1] = names[moved]; else delete names[1]; delete names[moved]; });
      const kept = songCache.get(`${id}|${mover.slot}`);
      songCacheDrop(id, 1);
      songCacheDrop(id, mover.slot);
      if (kept) songCache.set(`${id}|1`, kept);
      Object.assign(params, { start: toMs(mover.start), end: toMs(mover.end), play_from: -1 });
      mover.slot = 1;
    } else {
      await api("DELETE", "/api/theme/music", { id, slot: gone.slot });
      setSongNames(names => { delete names[gone.slot]; });
      songCacheDrop(id, gone.slot);
    }
    const seq = rest.map(x => x.slot);
    Object.assign(params, { sequence: seq.join(","), tracks: seq.length });
    if (pos === tracks.length - 1 || seq.length === 1) Object.assign(params, { loop_start: -1, loop_end: -1, order: "music" });
    await api("POST", "/api/theme/settings", { id, ...params });
    await refreshStatus();
    p.done("", "");
  } catch (e) {
    await refreshStatus();
    p.done(e.message, "err");
    renderAspect(aspectById("music"));
    return;
  }
  renderAspect(aspectById("music"));
  await remakeHomeMusic(id);
}

async function moveSong(pos, dir) {
  const id = state.editId, tracks = themeTracks(themeById(id)), to = pos + dir;
  if (to < 0 || to >= tracks.length) return;
  stopPreview();
  const seq = tracks.map(x => x.slot);
  [seq[pos], seq[to]] = [seq[to], seq[pos]];
  const params = { sequence: seq.join(","), tracks: seq.length };
  if (seq[seq.length - 1] !== tracks[tracks.length - 1].slot) Object.assign(params, { loop_start: -1, loop_end: -1 });
  try {
    await api("POST", "/api/theme/settings", { id, ...params });
    await refreshStatus();
  } catch (e) { return toast(e.message, "err"); }
  const box = $("#aspect-music");
  renderAspect(aspectById("music"));
  /* the focus follows the song */
  const same = $(`[data-act="${dir < 0 ? "song-up" : "song-down"}"][data-pos="${to}"]`, box);
  const other = $(`[data-act="${dir < 0 ? "song-down" : "song-up"}"][data-pos="${to}"]`, box);
  if (same && !same.disabled) same.focus(); else if (other) other.focus();
  await rebuildPlaylist(id);
}

async function setPlaylistJoin(i) {
  const id = state.editId, [join, blend] = LOOP_CHOICES[i] || LOOP_CHOICES[1];
  try {
    await api("POST", "/api/theme/settings", { id, between: join, loop: join, blend });
    await refreshStatus();
  } catch (e) { return toast(e.message, "err"); }
  await rebuildPlaylist(id);
}
async function setPlaylistEnd(mode) {
  const id = state.editId;
  try {
    await api("POST", "/api/theme/settings", { id, end_mode: mode });
    await refreshStatus();
  } catch (e) { return toast(e.message, "err"); }
  renderKeepingFocus(aspectById("music"));
  await rebuildPlaylist(id);
}

/* Play the playlist as the console will loop it (`around`: only the seconds around where it repeats). */
async function playPlaylist(around) {
  stopPreview();
  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) return toast("This browser cannot play audio previews.", "err");
  const ctx = new Ctx();     /* made at the press, so the browser lets it play */
  const id = state.editId, p = progressToast("Putting the playlist together…");
  let a;
  try { a = await playlistAudio(id, p, 1); p.done("", ""); }
  catch (e) { ctx.close(); return p.done(e.message, "err"); }
  if (state.editId !== id || state.trim || !$("#pl-play") || preview) { ctx.close(); return; }
  if (ctx.state === "suspended") await ctx.resume().catch(() => {});
  const [L, R] = a.built.chans, buf = ctx.createBuffer(2, L.length, a.rate);
  buf.copyToChannel(L, 0);
  buf.copyToChannel(R, 1);
  const src = ctx.createBufferSource();
  src.buffer = buf;
  src.loop = true;
  src.loopStart = a.built.intro / a.rate;
  src.loopEnd = buf.duration;
  src.connect(ctx.destination);
  const offset = around ? Math.max(0, buf.duration - 5) : 0;
  src.start(0, offset);
  preview = { ctx, src, offset, began: ctx.currentTime, frame: 0, timer: around ? setTimeout(stopPreview, 10000) : 0 };
  const play = $("#pl-play");
  if (play && !around) play.textContent = "Stop";
}

function pickFile(accept) {
  return new Promise(resolve => {
    const picker = $("#picker");
    picker.value = "";
    picker.accept = accept || "";
    picker.onchange = () => resolve(picker.files[0] || null);
    picker.click();
  });
}
