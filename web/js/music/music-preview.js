"use strict";

/* ---- preview (the processed music, looping as the console loops it) ---- */

let preview = null;

function stopPreview() {
  if (!preview) return;
  cancelAnimationFrame(preview.frame);
  clearTimeout(preview.timer);
  try { preview.src.stop(); } catch (e) { /* already stopped */ }
  preview.ctx.close();
  preview = null;
  const head = $("#trim-playhead"), play = $("#trim-play"), plPlay = $("#pl-play");
  if (head) head.hidden = true;
  if (play) play.textContent = "Play";
  if (plPlay) plPlay.textContent = "Play";
}

/* `around`: "loop" plays the seconds around the jump back, "joint" those
   around the change of part; otherwise from the beginning. */
function playPreview(around) {
  stopPreview();
  const t = state.trim;
  const built = buildHomeMusic(t.chans, t.rate, trimPart(t));
  const [L, R] = built.chans;
  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) return toast("This browser cannot play audio previews.", "err");
  const ctx = new Ctx(), buf = ctx.createBuffer(2, L.length, t.rate);
  buf.copyToChannel(L, 0);
  buf.copyToChannel(R, 1);
  const src = ctx.createBufferSource();
  src.buffer = buf;
  src.loop = true;
  src.loopStart = built.intro / t.rate;
  src.loopEnd = buf.duration;
  src.connect(ctx.destination);
  const offset = around === "loop" ? Math.max(0, buf.duration - 4)
    : around === "joint" && built.joint > 0 ? Math.max(0, built.joint / t.rate - 4) : 0;
  src.start(0, offset);
  preview = { ctx, src, offset, began: ctx.currentTime, frame: 0, timer: 0 };
  if (around) preview.timer = setTimeout(stopPreview, 8000);
  const play = $("#trim-play");
  if (play && !around) play.textContent = "Stop";
  const head = $("#trim-playhead"), intro = built.intro / t.rate, len = buf.duration;
  const tick = () => {
    if (!preview || !head) return;
    let pos = preview.offset + preview.ctx.currentTime - preview.began;
    if (pos >= len) pos = intro + (pos - intro) % (len - intro);
    const canvas = $("#wave-canvas");
    head.hidden = false;
    if (canvas) head.style.left = `${canvas.offsetLeft + built.source(Math.floor(pos * t.rate)) / t.rate / t.duration * canvas.clientWidth}px`;
    preview.frame = requestAnimationFrame(tick);
  };
  tick();
}

async function saveTrim() {
  if (state.trim.track) return saveSongPart();
  const t = state.trim;
  stopPreview();
  const part = trimPart(t);
  const toEnd = v => (v >= t.duration - 0.05 ? 0 : Math.round(v * 1000));
  const p = progressToast("Saving the music…");
  try {
    if (t.fresh) {
      p.set("Sending the music to the console…", 0, 0.2);
      await uploadVerified("/api/theme/music", { id: t.id }, pcmWav(t.chans, t.rate));
      setSongNames(names => { if (t.name) names[1] = t.name; else delete names[1]; });
      songCacheDrop(t.id, 1);
    }
    await api("POST", "/api/theme/settings", {
      id: t.id, start: Math.round(part.start * 1000), end: toEnd(part.end),
      play_from: part.playFrom === null ? -1 : Math.round(part.playFrom * 1000),
      loop_start: part.loopStart === null ? -1 : Math.round(part.loopStart * 1000),
      loop_end: part.loopEnd === null ? -1 : toEnd(part.loopEnd),
      order: part.order, loop: part.loop, blend: Math.round(part.blend * 1000),
    });
    await refreshStatus();
    const r = await convertHomeMusic(t.id, buildHomeMusic(t.chans, t.rate, part), t.rate, themeById(t.id).gain || 0, p);
    state.trim = null;
    stamp = Date.now();
    await refreshStatus();
    p.done(`Home music set and verified (${fmtClock(r.seconds)}). Apply the theme to hear it.`, "ok");
  } catch (e) {
    await refreshStatus();
    p.done(e.message, "err");
  }
  renderAspect(aspectById("music"));
}

function findGroup(asp, key) { return groupAssets(aspectAssets(asp)).find(g => g.key === key); }

async function attachToAspect(asp, file) {
  if (asp.mode === "music") return attachMusic(file);
  const isVideo = file.type.startsWith("video/") || /\.(mp4|webm|m4v)$/i.test(file.name);
  const off = bgOff();
  const targets = aspectAssets(asp).filter(a => (isVideo ? a.kind === "video" : a.kind !== "video") && !off.has(areaOf(a.path)));
  if (!targets.length) return toast(`No ${isVideo ? "video" : "image"} backgrounds in the areas switched on.`, "err");
  const ok = await attachTo(targets, file, asp.title);
  renderAspect(asp);
  if (asp.id === "background" && ok) await previewFromFile(state.editId, file, isVideo);
}

async function attachToGroup(asp, key, file) {
  const g = findGroup(asp, key);
  if (!g) return;
  await attachTo(g.assets, file, pieceLabel(asp, g));
  renderAspect(asp);
}
/* A piece by the name its part shows, for messages. */
function pieceLabel(asp, g) {
  const path = g.assets[0].path;
  return asp.id === "appearance" ? appearanceName(path) : asp.id === "fonts" ? fontName(path) : asp.id === "sounds" ? soundName(path) : g.name;
}

/* Ask the console whether the theme's home music file still passes its
   checks. Files from versions 1.10-1.12 do not: they fill unused space with
   zeros, which the console's decoder plays as ticks or silence. */
async function checkHomeMusic(id) {
  let r;
  try { r = await api("GET", "/api/theme/check", { id, path: HOME_MUSIC }); } catch (e) { return; }
  const el = $("#home-music-status"), fix = $("#home-music-fix"), t = themeById(id);
  if (!el || state.editId !== id) return;
  if (r.ok) {
    el.textContent = t && t.music ? "Encoded and verified" : "Home music file from an imported theme";
  } else {
    const m = /^the console cannot play this sound: it (.*?)\. Encode it again/.exec(r.error);
    el.textContent = `The console cannot play this file: ${m ? `it ${m[1]}` : r.error}. ${t && t.music ? "Encode it again." : "Replace it."}`;
    if (fix && t && t.music) fix.hidden = false;
  }
}

/* Encode music (built by buildHomeMusic) into the theme's home music and
   upload it. */
async function convertHomeMusic(id, built, rate, gain, p) {
  const r = await at9For(HOME_MUSIC, built.chans, rate, gain, f => p.set(`Encoding home music… ${Math.round(f * 100)}%`, 0.2 + f * 0.7),
                         built.intro / rate);
  p.set("Sending the home music to the console…", 0.92);
  await uploadVerified("/api/theme/file", { id, path: HOME_MUSIC }, r.blob);
  if (id === state.editId) state.files.add(HOME_MUSIC);
  return r;
}

/* A new song opens the part editor; nothing is saved until "Use this part".
   The theme keeps the whole song (48 kHz WAV, up to about 11 minutes) so the
   part and loudness can be changed later. */
async function attachMusic(file) {
  const id = state.editId;
  const p = progressToast("Reading music…");
  try {
    let chans = await decodeAudio(file, MUSIC_RATE, 2);
    const keep = Math.floor(MUSIC_SOURCE_MAX_BYTES / 4);
    const long = chans[0].length > keep;
    if (long) chans = trimWithFade(chans, keep, MUSIC_RATE);
    await openTrim(id, chans, MUSIC_RATE, { start: 0, end: 0, loop: "crossfade", blend: 2 }, true);
    state.trim.name = songTitle(file);   /* shown if the song becomes the first of a playlist */
    p.done(long ? `Only the first ${fmtTime(keep / MUSIC_RATE)} of the song can be kept. Choose the part to use.`
      : "Choose the part of the song to use.", "");
    const handle = $("#trim-start");
    if (handle) handle.focus();
  } catch (e) { p.done(e.message, "err"); }
}

async function openThemePart(id) {
  const p = progressToast("Reading the theme's music…");
  try {
    const res = await fetch(`/api/theme/music?${q({ id })}`);
    if (!res.ok) throw new Error("this theme has no music");
    const { chans, rate } = wavChannels(await res.arrayBuffer());
    await openTrim(id, chans.length === 1 ? [chans[0], chans[0]] : chans, rate, themePart(themeById(id)), false);
    p.done("Choose the part of the song to use.", "");
  } catch (e) { p.done(e.message, "err"); }
}

/* Encode the theme's stored music again (new loudness, or music from before
   version 1.10, which the plugin played itself). */
async function reencodeMusic(id, gain) {
  const p = progressToast("Reading the theme's music…");
  try {
    const res = await fetch(`/api/theme/music?${q({ id })}`);
    if (!res.ok) throw new Error("this theme has no music to convert");
    const { chans, rate } = wavChannels(await res.arrayBuffer());
    const stereo = chans.length === 1 ? [chans[0], chans[0]] : chans;
    const r = await convertHomeMusic(id, buildHomeMusic(stereo, rate, themePart(themeById(id))), rate, gain, p);
    stamp = Date.now();
    await refreshStatus();
    p.done(`Home music encoded and verified (${fmtTime(r.seconds)}). Apply the theme to hear it.`, "ok");
  } catch (e) { p.done(e.message, "err"); }
}
