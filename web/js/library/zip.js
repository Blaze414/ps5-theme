"use strict";

/* ================================================================ zip import / export */

async function readZip(file) {
  const buf = new Uint8Array(await file.arrayBuffer());
  const dv = new DataView(buf.buffer);
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("Not a zip file");
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const dec = new TextDecoder();
  const entries = [];
  for (let n = 0; n < count; n++) {
    if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("Damaged zip file");
    const flags = dv.getUint16(p + 8, true), method = dv.getUint16(p + 10, true);
    const crc = dv.getUint32(p + 16, true), size = dv.getUint32(p + 20, true);
    const nameLen = dv.getUint16(p + 28, true), extraLen = dv.getUint16(p + 30, true), commentLen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = dec.decode(buf.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith("/")) continue;
    if (flags & 1) throw new Error("Encrypted zip files are not supported");
    const start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
    const data = buf.subarray(start, start + size);
    entries.push({
      path: name,
      blob: async () => {
        let blob;
        if (method === 0) blob = new Blob([data]);
        else if (method === 8) {
          if (typeof DecompressionStream === "undefined") throw new Error("This browser cannot unzip; use a current Chrome, Edge, Firefox or Safari");
          blob = await new Response(new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"))).blob();
        } else throw new Error(`${name}: unsupported compression`);
        if (crc32(new Uint8Array(await blob.arrayBuffer())) !== crc) throw new Error(`${name} is damaged (checksum mismatch)`);
        return blob;
      },
    });
  }
  return entries;
}

function makeZip(files) {
  const enc = new TextEncoder();
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.path), crc = crc32(f.data), size = f.data.length;
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true);
    lh.setUint16(12, 33, true); lh.setUint32(14, crc, true); lh.setUint32(18, size, true); lh.setUint32(22, size, true);
    lh.setUint16(26, name.length, true);
    parts.push(lh, name, f.data);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true);
    ch.setUint16(14, 33, true); ch.setUint32(16, crc, true); ch.setUint32(20, size, true); ch.setUint32(24, size, true);
    ch.setUint16(28, name.length, true); ch.setUint32(42, offset, true);
    central.push(ch, name);
    offset += 30 + name.length + size;
  }
  const cdSize = central.reduce((n, x) => n + x.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true);
  end.setUint32(12, cdSize, true); end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end], { type: "application/zip" });
}

async function exportTheme(id) {
  const t = themeById(id);
  const p = progressToast(`Exporting ${t.name}…`);
  try {
    const { files } = await api("GET", "/api/theme/files", { id });
    const out = [{ path: "theme.ini", data: new TextEncoder().encode(`name=${t.name}\nfw=${t.fw}\nmusic_gain=${t.gain || 0}\n` +
      `music_start=${t.musicStart || 0}\nmusic_end=${t.musicEnd || 0}\n` +
      `music_loop_start=${t.musicLoopStart == null ? -1 : t.musicLoopStart}\nmusic_loop_end=${t.musicLoopEnd == null ? -1 : t.musicLoopEnd}\n` +
      `music_order=${t.musicOrder || "music"}\nmusic_play_from=${t.musicPlayFrom == null ? -1 : t.musicPlayFrom}\n` +
      `music_loop=${t.musicLoop || "crossfade"}\nmusic_blend=${t.musicBlend == null ? 2000 : t.musicBlend}\n` + playlistIni(t)) }];
    /* a playlist's other songs, music2.wav … beside music.wav */
    const songs = t.music ? themeTracks(t).filter(x => x.slot !== 1) : [];
    const total = files.length + (t.music ? 1 : 0) + songs.length;
    for (let i = 0; i < files.length; i++) {
      const res = await fetch(`/api/theme/file?${q({ id, path: files[i].path })}`);
      if (!res.ok) throw new Error(`Could not read ${files[i].path}`);
      out.push({ path: "files" + files[i].path, data: new Uint8Array(await res.arrayBuffer()) });
      p.set(`Exporting ${t.name}…`, (i + 1) / total);
    }
    if (t.music) {
      const res = await fetch(`/api/theme/music?${q({ id })}`);
      if (!res.ok) throw new Error("Could not read the music");
      out.push({ path: "music.wav", data: new Uint8Array(await res.arrayBuffer()) });
    }
    for (let i = 0; i < songs.length; i++) {
      p.set(`Exporting ${t.name}: song ${i + 2} of ${songs.length + 1}…`, (files.length + 1 + i) / total, (files.length + 2 + i) / total);
      const res = await fetch(`/api/theme/music?${q({ id, slot: songs[i].slot })}`);
      if (!res.ok) throw new Error("Could not read a song of the playlist");
      out.push({ path: `music${songs[i].slot}.wav`, data: new Uint8Array(await res.arrayBuffer()) });
    }
    for (const [kind, name] of PREVIEW_FILES) {
      const res = await fetch(`/api/theme/preview?${q({ id, kind })}`);
      if (res.ok) out.push({ path: name, data: new Uint8Array(await res.arrayBuffer()) });
    }
    /* the editor's choices (placement, fit, song names …), when the theme has any */
    const choices = cleanChoices(await fetch(`/api/theme/choices?${q({ id })}`).then(r => r.ok ? r.json() : {}).catch(() => ({})));
    if (Object.keys(choices).length) out.push({ path: "choices.json", data: new TextEncoder().encode(JSON.stringify({ v: 1, ...choices })) });
    const zip = makeZip(out);
    const back = await readZip(new File([zip], "check.zip"));   /* verify before handing it over */
    if (back.length !== out.length) throw new Error("export did not verify");
    for (const e of back) await e.blob();
    const a = document.createElement("a");
    a.href = URL.createObjectURL(zip);
    a.download = `${id}.zip`;
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    const mb = (zip.size / 1048576).toFixed(zip.size < 10485760 ? 1 : 0);
    p.done(`Saved ${id}.zip: ${describeFiles(files.map(f => f.path)) || "no theme files"}` +
      `${songs.length ? `, ${plural(songs.length + 1, "song")}` : t.music ? ", the song" : ""}, ${mb} MB, verified.`, "ok");
  } catch (e) { p.done(e.message, "err"); }
}

/* theme.ini's playlist lines: the songs' order, each added song's part, how they join and how it ends. */
function playlistIni(t) {
  const tracks = themeTracks(t), pl = themePlaylist(t);
  return `music_tracks=${tracks.length}\nmusic_between=${pl.join}\nmusic_end_mode=${pl.endMode}\nmusic_sequence=${tracks.map(x => x.slot).join(",")}\n` +
    tracks.filter(x => x.slot !== 1).map(x => `music${x.slot}_start=${toMs(x.start)}\nmusic${x.slot}_end=${toMs(x.end)}\n`).join("");
}

/* "15 backgrounds, the home music and 2 other files", for export and import. */
function describeFiles(paths) {
  const bg = paths.filter(x => BG_RE.test(x)).length, music = paths.includes(HOME_MUSIC);
  const ui = paths.filter(x => /\.rco$/i.test(x)).length;
  const other = paths.length - bg - ui - (music ? 1 : 0);
  const parts = [bg ? plural(bg, "background") : "", music ? "the home music" : "", ui ? `interface pictures (${plural(ui, "file")})` : "",
                 other ? plural(other, "other file") : ""].filter(Boolean);
  return parts.length > 1 ? `${parts.slice(0, -1).join(", ")} and ${parts[parts.length - 1]}` : parts[0] || "";
}

function parseIni(text) {
  const out = {};
  text.split(/\r?\n/).forEach(l => { const i = l.indexOf("="); if (i > 0) out[l.slice(0, i).trim()] = l.slice(i + 1).trim(); });
  return out;
}

async function importEntries(entries, fallbackName) {
  const ini = entries.filter(e => /(^|\/)theme\.ini$/.test(e.path)).sort((a, b) => a.path.length - b.path.length)[0];
  const meta = ini ? parseIni(await (await ini.blob()).text()) : {};
  const prefix = ini ? ini.path.slice(0, -"theme.ini".length) : "";
  const music = entries.find(e => e.path === prefix + "music.wav");
  /* A playlist: its other songs (music2.wav …) and their order; only songs that are in the zip, and none without music.wav. */
  const songs = [];
  for (let s = 2; music && s <= MUSIC_MAX_TRACKS; s++) {
    const e = entries.find(x => x.path === prefix + `music${s}.wav`);
    if (e) songs.push({ slot: s, e });
  }
  const present = new Set(music ? [1, ...songs.map(x => x.slot)] : []);
  const seq = [...new Set(String(meta.music_sequence || "1").split(",").map(Number).filter(s => present.has(s)))];
  if (music && !seq.includes(1)) seq.unshift(1);
  const playlist = seq.length > 1;
  const pictures = PREVIEW_FILES.map(([kind, name]) => [kind, entries.find(e => e.path === prefix + name)]).filter(([, e]) => e);
  const choicesEntry = entries.find(e => e.path === prefix + "choices.json");
  /* Everything under the theme's files/ folder (the console decides what it
     accepts); without a theme.ini, anything under a themable folder. */
  const files = [];
  const root = prefix + "files/";
  for (const e of entries) {
    if (ini && e.path.startsWith(root)) { files.push({ asset: "/" + e.path.slice(root.length), blob: e.blob }); continue; }
    if (ini) continue;
    const idx = Math.min(...ASSET_ROOTS.map(r => { const i = e.path.indexOf(r); return i < 0 ? Infinity : i; }));
    if (idx !== Infinity && (idx === 0 || e.path[idx - 1] === "/")) files.push({ asset: "/" + e.path.slice(idx), blob: e.blob });
  }
  if (!files.length && !music) throw new Error("No theme files found. Expected theme.ini with files/system_ex/rnps/… or music.wav.");

  const name = uniqueName(meta.name || fallbackName.replace(/\.zip$/i, ""));
  const id = await createTheme(name, meta.fw || state.status.fw);
  const gain = Math.max(-12, Math.min(12, parseInt(meta.music_gain, 10) || 0));
  const settings = { id, gain };
  for (const [key, param] of [["music_start", "start"], ["music_end", "end"], ["music_loop_start", "loop_start"],
                              ["music_loop_end", "loop_end"], ["music_order", "order"], ["music_play_from", "play_from"],
                              ["music_loop", "loop"], ["music_blend", "blend"]]) {
    if (meta[key] !== undefined && meta[key] !== "") settings[param] = meta[key];
  }
  if (playlist) {
    Object.assign(settings, { sequence: seq.join(","), tracks: seq.length });
    if (meta.music_between) settings.between = meta.music_between;
    if (meta.music_end_mode) settings.end_mode = meta.music_end_mode;
    for (const s of seq) {
      if (s === 1) continue;
      for (const k of ["start", "end"]) if (meta[`music${s}_${k}`] !== undefined && meta[`music${s}_${k}`] !== "") settings[`${k}${s}`] = meta[`music${s}_${k}`];
    }
  }
  await api("POST", "/api/theme/settings", settings).catch(() => {});
  await refreshStatus();
  const p = progressToast(`Importing ${name}…`);
  let ok = 0, skipped = 0;
  const added = [], why = [];
  const convert = music && !files.some(f => f.asset === HOME_MUSIC);   /* exported before 1.10, or without its home music */
  const extra = songs.filter(x => playlist && seq.includes(x.slot));
  const total = files.length + (music ? 1 : 0) + extra.length + (convert ? 1 : 0);
  for (let i = 0; i < files.length; i++) {
    p.set(`Importing ${name}… ${i + 1}/${total}`, i / total, (i + 1) / total);
    try { await uploadVerified("/api/theme/file", { id, path: files[i].asset }, await files[i].blob()); ok++; added.push(files[i].asset); }
    catch (e) { skipped++; why.push(`${baseName(files[i].asset)}: ${e.message}`); }
    p.set(`Importing ${name}… ${i + 1}/${total}`, (i + 1) / total);
  }
  if (music) {
    try {
      const blob = await music.blob();
      p.set(`Importing ${name}: the music…`, files.length / total, (files.length + 1) / total);
      await uploadVerified("/api/theme/music", { id }, blob);
      ok++;
      if (convert && !playlist) {
        const { chans, rate } = wavChannels(await blob.arrayBuffer());
        const stereo = chans.length === 1 ? [chans[0], chans[0]] : chans;
        await convertHomeMusic(id, buildHomeMusic(stereo, rate, themePart(themeById(id) || {})), rate, gain, p);
        ok++;
        added.push(HOME_MUSIC);
      }
    } catch (e) { skipped++; why.push(`music: ${e.message}`); }
  }
  for (let i = 0; i < extra.length; i++) {
    const done = files.length + 1 + i;
    p.set(`Importing ${name}: song ${i + 2} of ${extra.length + 1}…`, done / total, (done + 1) / total);
    try { await uploadVerified("/api/theme/music", { id, slot: extra[i].slot }, await extra[i].e.blob()); ok++; }
    catch (e) { skipped++; why.push(`music${extra[i].slot}.wav: ${e.message}`); }
  }
  if (convert && playlist && music) {
    try {
      await refreshStatus();
      const a = await playlistAudio(id, null, 0);
      p.set(`Importing ${name}: making the home music…`, (total - 1) / total, 1);
      await convertHomeMusic(id, a.built, a.rate, gain, p);
      ok++;
      added.push(HOME_MUSIC);
    } catch (e) { skipped++; why.push(`home music: ${e.message}`); }
  }
  for (const [kind, e] of pictures) {
    try { await uploadVerified("/api/theme/preview", { id, kind }, await e.blob()); } catch (err) { /* optional */ }
  }
  if (choicesEntry) {
    /* only what the page understands goes in; a damaged file is left out */
    try {
      const c = cleanChoices(JSON.parse(await (await choicesEntry.blob()).text()));
      if (Object.keys(c).length) await putChoices(id, c);
    } catch (err) { /* optional */ }
  }
  await api("POST", "/api/theme/save", { id }).catch(() => {});   /* an imported theme starts saved */
  await refreshStatus();
  const got = describeFiles(added) || "no theme files";
  p.done(skipped
    ? `Imported ${name}: ${got}, verified. ${skipped} skipped (not on this firmware or not valid here): ${why.slice(0, 2).join("; ")}`
    : `Imported ${name}: ${got}, all verified.`, skipped ? "" : "ok");
  if (state.route === "library") render();
}

function readDirEntry(entry) {
  return new Promise(resolve => {
    const reader = entry.createReader(), all = [];
    const next = () => reader.readEntries(batch => { if (!batch.length) return resolve(all); all.push(...batch); next(); }, () => resolve(all));
    next();
  });
}
async function walk(entry, out) {
  if (entry.isFile) out.push({ path: entry.fullPath.replace(/^\//, ""), blob: () => new Promise((res, rej) => entry.file(res, rej)) });
  else if (entry.isDirectory) for (const child of await readDirEntry(entry)) await walk(child, out);
}
async function importDrop(dt) {
  const roots = Array.from(dt.items || []).filter(i => i.kind === "file")
    .map(i => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  try {
    if (roots.some(r => r.isDirectory)) {
      for (const root of roots.filter(r => r.isDirectory)) {
        const entries = [];
        await walk(root, entries);
        await importEntries(entries, root.name);
      }
      return;
    }
    for (const file of Array.from(dt.files)) await importFile(file);
  } catch (e) { toast(e.message, "err", 9000); }
}
async function importFile(file) {
  if (!/\.zip$/i.test(file.name)) throw new Error(`${file.name} is not a .zip theme`);
  await importEntries(await readZip(file), file.name);
}
