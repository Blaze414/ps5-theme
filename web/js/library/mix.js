"use strict";

/* ================================================================ mix and match */

/* A new theme made of parts of others: each part, or each element of a part, taken from one of your themes or
   left as the console's own. The parts are the editor's sections, with what the interface file holds (its
   pictures, sounds, colours and corner shapes) split into parts of their own. The themes taken from stay as they
   are. The plugin copies the files (/api/themes/mix); the interface file is made here, from each theme's changes
   to it, entry by entry. */
/* What a theme has of a part, counted in that part's own words: "3 tiles", "1 area". */
const MIX_NOUN = {
  background: ["area"], icons: ["icon"], sounds: ["sound"], uisounds: ["sound"], fonts: ["font"], appearance: ["piece"],
  colours: ["palette"], interface: ["icon"], shapes: ["shape"], tiles: ["tile"], titles: ["game or app", "games and apps"], other: ["file"],
};
const MIX_PARTS = [
  { id: "background", label: "Background", aspects: ["background"], by: "area" },
  { id: "icons", label: "Other icons", aspects: ["icons"], by: "group" },
  { id: "music", label: "Home music", music: true },
  { id: "sounds", label: "Sounds", aspects: ["sounds"], by: "group" },
  { id: "uisounds", label: "Interface sounds", rco: "sound", sections: ["sounds"] },
  { id: "fonts", label: "Fonts", aspects: ["fonts"], by: "font" },
  { id: "appearance", label: "Appearance themes' artwork", aspects: ["appearance"], by: "piece" },
  { id: "colours", label: "Colours", rco: "color", sections: ["appearance"] },
  { id: "interface", label: "Interface icons", rco: "picture", sections: ["interface"] },
  { id: "shapes", label: "Corner shapes", rco: "shape", sections: ["interface"] },
  { id: "tiles", label: "Home screen tiles", aspects: ["tiles"], by: "tile" },
  { id: "titles", label: "Games and apps", aspects: ["titles"], by: "title" },
  { id: "other", label: "Animations, video, textures and other images", aspects: ["animations", "video", "textures", "images"], by: "group" },
];
/* Where a file goes when more than one section would list it: the more particular first. */
const MIX_ORDER = ["background", "appearance", "tiles", "titles", "sounds", "fonts", "icons", "animations", "video", "textures", "images"];
const MIX_ASPECT_PART = Object.fromEntries(MIX_PARTS.flatMap(p => (p.aspects || []).map(a => [a, p.id])));
const mixPartById = id => MIX_PARTS.find(p => p.id === id);

let mixAssetIndex = null;
function mixAsset(path) {
  if (!mixAssetIndex || mixAssetIndex.list !== state.assets) {
    mixAssetIndex = { list: state.assets, map: new Map((state.assets || []).map(a => [a.path, a])) };
  }
  return mixAssetIndex.map.get(path);
}

/* The part a theme's file belongs to: "music" for the home music, "rco" for an interface file, or null for a file
   this console doesn't have (it is left out). */
function mixPartOf(path) {
  if (path === HOME_MUSIC) return "music";
  const a = mixAsset(path);
  if (!a) return null;
  if (a.kind === "rco") return "rco";
  for (const id of MIX_ORDER) {
    const asp = aspectById(id);
    if (asp && asp.filter && asp.filter(a)) return MIX_ASPECT_PART[id];
  }
  return null;
}

/* The element of its part a file belongs to, [key, name]: a background area, an Appearance piece, a font, a tile,
   a game, or the file with its size variants. */
function mixElementOf(part, path) {
  switch (part.by) {
    case "area": return [areaOf(path), areaOf(path)];
    case "piece": return [path, appearanceName(path)];
    case "font": return [path, fontName(path)];
    case "tile": { const t = tileTitle(path); return [t, (state.tileNames && state.tileNames.get(t)) || t]; }
    case "title": { const t = titlesOf(path)[0] || path; return [t, (state.titleNames && state.titleNames.get(t)) || t]; }
    default: { const k = groupKey(path); return [k, baseName(k)]; }
  }
}

/* What one of your themes has, by part. */
async function mixThemeInfo(t) {
  const r = await api("GET", "/api/theme/files", { id: t.id });
  const info = { id: t.id, name: t.name, parts: {}, rco: [], home: false, slots: [], left: 0 };
  for (const f of r.files) {
    const part = mixPartOf(f.path);
    if (part === "rco") info.rco.push(f.path);
    else if (part === "music") info.home = true;
    else if (part) (info.parts[part] = info.parts[part] || []).push(f.path);
    else info.left++;
  }
  if (t.music) info.slots = [...new Set(t.musicSequence && t.musicSequence.length ? t.musicSequence : [1])];
  return info;
}

/* Which themes a part can be taken from: those that have something of it (any interface file, for its parts: what
   is in it shows once the file is read). */
function mixHas(info, part) {
  if (part.music) return info.home || info.slots.length > 0;
  if (part.rco) return info.rco.length > 0;
  return (info.parts[part.id] || []).length > 0;
}

/* A part's elements across your themes: key, name, and the files (or the interface file's entries) each theme has. */
function mixElements(part) {
  const out = new Map();
  const add = (key, label, tid, item) => {
    if (!out.has(key)) out.set(key, { key, label, themes: new Map() });
    const e = out.get(key);
    if (!e.themes.has(tid)) e.themes.set(tid, []);
    e.themes.get(tid).push(item);
  };
  if (part.rco) {
    for (const [tid, byPath] of state.mix.rco) {
      for (const [path, changes] of byPath) {
        const r = state.rco && state.rco.get(path);
        if (!r) continue;
        const files = new Map(r.files.map(f => [`${f.el}\t${f.attr}`, f]));
        const sounds = part.rco === "sound" ? new Map(uiSounds(r).map(s => [s.id, s.label])) : null;
        for (const k of changes.keys()) {
          const f = files.get(k);
          if (!f || rcoEntryKind(f) !== part.rco) continue;
          const label = part.rco === "sound" ? sounds.get(f.id) || f.id : part.rco === "color" ? PALETTE_NAMES[f.id] || f.id
            : part.rco === "shape" ? "Corner shapes" : f.id;
          add(`${path}|${part.rco === "picture" ? f.el : k}`, label, tid, k);
        }
      }
    }
  } else if (!part.music) {
    for (const info of state.mix.themes.values()) {
      for (const path of info.parts[part.id] || []) {
        const [key, label] = mixElementOf(part, path);
        add(key, label, info.id, path);
      }
    }
  }
  return [...out.values()].sort((a, b) => a.label.localeCompare(b.label) || a.key.localeCompare(b.key));
}

/* Where an element comes from: chosen for it, or its part's choice ("" for the console's own). */
function mixSource(part, key) {
  const own = state.mix.elems[part.id] || {};
  return key in own ? own[key] : state.mix.picks[part.id] || "";
}

/* A theme's changes to an interface file, against the console's own (read once while the form is open). */
async function mixRcoChanges(tid, path) {
  const byPath = state.mix.rco.get(tid) || new Map();
  state.mix.rco.set(tid, byPath);
  if (byPath.has(path)) return byPath.get(path);
  const stock = await rcoStock(path), out = new Map();
  const res = await fetch(`/api/theme/file?${q({ id: tid, path })}`);
  if (stock && res.ok) {
    const t = rcoParse(await readBytes(res, f => loadProgress(`mix:${tid}:${path}`, f)));
    const theirs = new Map(t.files.map(f => [`${f.el}\t${f.attr}`, f]));
    for (const f of stock.files) {
      const k = `${f.el}\t${f.attr}`, o = theirs.get(k);
      if (!o) continue;
      const a = rcoData(stock, f), b = rcoData(t, o);
      if (a.length !== b.length || a.some((v, i) => v !== b[i])) out.set(k, b);
    }
  }
  byPath.set(path, out);
  return out;
}

/* Read the interface files of the themes that have one, for choosing their contents element by element. */
async function mixLoadRco() {
  const m = state.mix;   /* this form: closed (or opened again) meanwhile, the reading stops and draws nothing */
  if (!m || m.rcoLoading) return;
  m.rcoLoading = true;
  libraryIfShown();
  try {
    const all = [...m.themes.values()].flatMap(info => info.rco.map(path => [info.id, path]));
    for (let i = 0; i < all.length && state.mix === m; i++) {
      loadProgress("mix:rco", i / all.length);
      await mixRcoChanges(all[i][0], all[i][1]);
    }
    loadProgress("mix:rco", 1);
  } catch (e) { if (state.mix === m) toast(`The interface files could not be read: ${e.message}`, "err"); }
  m.rcoLoading = false;
  m.rcoReady = true;
  if (state.mix === m) libraryIfShown();
}

async function openMix() {
  const m = state.mix = { loading: true, themes: new Map(), name: "", picks: {}, elems: {}, open: {}, rco: new Map() };
  try {
    await loadAssets();
    if (!state.assets) throw new Error(state.assetsError || "the console's files could not be listed");
    titleNamesEnsure();   /* games and apps by name, as the editor shows them */
    const list = state.status.themes.filter(x => x.compatible);
    for (let i = 0; i < list.length && state.mix === m; i++) {
      loadProgress("mix", i / list.length);
      const info = await mixThemeInfo(list[i]);
      info.sources = new Set((await api("GET", "/api/theme/sources", { id: list[i].id }).catch(() => ({ have: [] }))).have || []);
      m.themes.set(list[i].id, info);
    }
    loadProgress("mix", 1);
    if (titleNamesLoading) await titleNamesLoading.catch(() => {});
  } catch (e) {
    if (state.mix !== m) return;   /* closed meanwhile: nothing to say */
    state.mix = null;
    if (state.creating) state.creating.mode = "empty";
    toast(`Mix and match: ${e.message}`, "err");
    return libraryIfShown();
  }
  if (state.mix !== m) return;     /* closed, or opened again, meanwhile */
  m.loading = false;
  libraryIfShown();
}

/* Something to choose in a part is chosen: the theme can be made. */
function mixChosen() {
  const m = state.mix;
  return !!m && MIX_PARTS.some(p => m.picks[p.id] || Object.values(m.elems[p.id] || {}).some(Boolean));
}

/* A part's picture in one of your themes (tid), or the console's own (""): its first picture, the original kept for
   a texture, or a sign of what it is. */
function mixThumb(part, tid, paths) {
  const t = tid && themeById(tid), info = tid && state.mix.themes.get(tid);
  if (part.music) return `<div class="pick-art">${GLYPH.music}${t && t.music ? `<span class="time">${esc(fmtTime(t.music))}</span>` : ""}</div>`;
  if (part.rco) return `<div class="pick-art">${GLYPH[{ sound: "sound", color: "colour", picture: "icons", shape: "shape" }[part.rco]]}</div>`;
  if (part.id === "sounds") return `<div class="pick-art">${GLYPH.sound}</div>`;
  if (part.id === "fonts") return `<div class="pick-art"><span class="aa">Aa</span></div>`;
  const pic = (paths || []).find(x => /\.(png|jpe?g|webp)$/i.test(x));
  const square = part.id === "icons" || part.id === "tiles" || part.id === "titles";
  if (pic) return `<div class="pick-art${square ? " contain" : ""}"><img loading="lazy" alt="" src="${tid
    ? `/api/theme/file?${q({ id: tid, path: pic })}&v=${stamp}` : `/api/asset?${q({ path: pic })}`}"></div>`;
  const kept = tid && info && info.sources && (paths || []).find(x => info.sources.has(x));
  if (kept) return `<div class="pick-art"><img loading="lazy" alt="" src="/api/theme/source?${q({ id: tid, path: kept })}"></div>`;
  return `<div class="pick-art">${GLYPH.file}</div>`;
}

function mixTile(act, part, v, chosen, paths, name, sub, extra = "") {
  return `<button type="button" class="opt" role="radio" aria-checked="${chosen}" data-act="${act}" data-part="${esc(part.id)}" data-v="${esc(v)}"${extra}>
    ${v === "*" ? `<div class="pick-art"><span class="aa">=</span></div>` : mixThumb(part, v, paths)}<b>${esc(name)}</b>${sub ? `<small>${esc(sub)}</small>` : ""}</button>`;
}

function mixSection() {
  const m = state.mix;
  if (!m || m.loading) return loadingMarker("mix", "Reading your themes…", "Each theme's parts, to show them here.");
  const themes = [...m.themes.values()].sort(byName);
  /* An interface file's parts, once the files are read: only the themes that change that kind, with how much. */
  const rcoCounts = new Map(MIX_PARTS.filter(p => p.rco && m.rcoReady).map(p => {
    const n = new Map();
    for (const e of mixElements(p)) for (const tid of e.themes.keys()) n.set(tid, (n.get(tid) || 0) + 1);
    return [p.id, n];
  }));
  const count = (part, n) => plural(n, ...(MIX_NOUN[part.id] || ["piece"]));
  const countOf = (info, part) => part.music ? "" : part.rco ? (rcoCounts.has(part.id) ? count(part, rcoCounts.get(part.id).get(info.id) || 0) : "")
    : count(part, new Set((info.parts[part.id] || []).map(x => mixElementOf(part, x)[0])).size);
  const rows = MIX_PARTS.map(part => {
    const have = themes.filter(i => mixHas(i, part) && (!rcoCounts.has(part.id) || rcoCounts.get(part.id).has(i.id)));
    if (!have.length) return "";
    if (rcoCounts.has(part.id) && m.picks[part.id] && !rcoCounts.get(part.id).has(m.picks[part.id])) m.picks[part.id] = "";
    const pick = m.picks[part.id] || "", open = !!m.open[part.id];
    const all = [...new Set(have.flatMap(i => i.parts[part.id] || []))];
    let detail = "";
    if (open && part.rco && !m.rcoReady) {
      detail = m.rcoLoading ? loadingMarker("mix:rco", "Reading the themes' interface files…", "Each is read once while this stays open.")
        : `<p class="note">The interface files are read to show what each theme changes in them.</p>`;
    } else if (open) {
      const elems = mixElements(part), own = m.elems[part.id] || {};
      detail = elems.length ? elems.map(e => {
        const cur = e.key in own ? own[e.key] : "*", stock = [...e.themes.values()][0];
        return `<div class="mix-elem"><span>${esc(e.label)}</span><div class="picks small" role="radiogroup" aria-label="${esc(e.label)}">
          ${mixTile("mix-elem-pick", part, "*", cur === "*", null, "As above", "", ` data-key="${esc(e.key)}"`)}
          ${mixTile("mix-elem-pick", part, "", cur === "", stock, "Console's own", "", ` data-key="${esc(e.key)}"`)}
          ${themes.filter(i => e.themes.has(i.id)).map(i => mixTile("mix-elem-pick", part, i.id, cur === i.id, e.themes.get(i.id), i.name, "", ` data-key="${esc(e.key)}"`)).join("")}</div></div>`;
      }).join("") : `<p class="note">None of your themes changes any of it.</p>`;
    }
    return `<section class="mix-row" aria-label="${esc(part.label)}">
      <div class="mix-head"><b>${esc(part.label)}</b>${pick ? "" : `<small>Not changed</small>`}
        ${part.music ? "" : `<button class="btn ghost sm" type="button" data-act="mix-elements" data-part="${esc(part.id)}">${open ? "All from one" : "Piece by piece"}</button>`}</div>
      <div class="picks" role="radiogroup" aria-label="${esc(part.label)}">
        ${mixTile("mix-pick", part, "", !pick, all, "Console's own", "")}
        ${have.map(i => mixTile("mix-pick", part, i.id, pick === i.id, i.parts[part.id], i.name, countOf(i, part))).join("")}</div>
      ${detail}</section>`;
  }).join("");
  return `
    <div class="mix-fill"><span>Everything from</span>${themes.map(i => `<button class="btn sm" type="button" data-act="mix-fill" data-v="${esc(i.id)}">${esc(i.name)}</button>`).join("")}
      ${help("A new theme is made from copies of the parts you choose; the themes they come from stay as they are. Piece by piece takes a part's pieces from different themes: each background area, each Appearance piece, each tile, game, sound or colour palette on its own. Only themes for this console's firmware are offered.")}</div>
    <div class="mix-rows">${rows}</div>`;
}

/* Make the new theme: the files copied by the plugin, the interface files made here, the editor's choices, saved. */
async function createMix(typed) {
  const m = state.mix, name = uniqueName(typed || "My mix"), id = uniqueId(name);
  const p = progressToast(`Making ${name}…`);
  try {
    const lines = [], used = new Set(), sourcesOf = new Map();
    const originals = async tid => {
      if (!sourcesOf.has(tid)) {
        const r = await api("GET", "/api/theme/sources", { id: tid }).catch(() => ({ have: [] }));
        sourcesOf.set(tid, new Set(r.have || []));
      }
      return sourcesOf.get(tid);
    };
    const sections = new Set();
    for (const part of MIX_PARTS) {
      if (part.music) {
        const tid = m.picks.music, info = tid && m.themes.get(tid);
        if (!info) continue;
        lines.push(`settings\t${tid}`);
        if (info.home) lines.push(`file\t${tid}\t${HOME_MUSIC}`);
        for (const slot of info.slots) lines.push(`music\t${tid}\t${slot}`);
        used.add(tid);
        sections.add("music");
        continue;
      }
      if (part.rco) continue;
      for (const e of mixElements(part)) {
        const tid = mixSource(part, e.key), paths = tid && e.themes.get(tid);
        if (!paths) continue;
        const have = await originals(tid);
        for (const path of paths) {
          lines.push(`file\t${tid}\t${path}`);
          if (have.has(path)) lines.push(`source\t${tid}\t${path}`);
          const a = mixAsset(path);
          for (const asp of part.aspects) if (aspectById(asp).filter(a)) sections.add(asp);
        }
        used.add(tid);
      }
    }
    if (m.picks.background) lines.push(`preview\t${m.picks.background}`);
    const choicesOf = async tid => cleanChoices(await api("GET", "/api/theme/choices", { id: tid }).catch(() => ({})));
    const style = await mixIconStyle(choicesOf);
    if (style) {
      lines.push(`style\t${style.from}`);
      for (const t of style.own) lines.push(`own\t${style.from}\t${t}`);
    }

    /* The interface files: each entry from the theme its part or element is taken from. */
    const rcoParts = MIX_PARTS.filter(part => part.rco && (m.picks[part.id] || Object.values(m.elems[part.id] || {}).some(Boolean)));
    const rcoFiles = new Map();
    if (rcoParts.length) {
      const involved = new Set();
      for (const part of rcoParts) {
        if (m.picks[part.id]) involved.add(m.picks[part.id]);
        for (const v of Object.values(m.elems[part.id] || {})) if (v) involved.add(v);
      }
      const paths = new Set([...involved].flatMap(tid => m.themes.get(tid).rco));
      let i = 0;
      for (const path of paths) {
        p.set(`Reading ${baseName(path)}…`, 0.1 + 0.4 * i++ / paths.size);
        const changes = new Map();
        for (const tid of involved) if (m.themes.get(tid).rco.includes(path)) changes.set(tid, await mixRcoChanges(tid, path));
        const kinds = new Map(rcoParts.map(part => [part.rco, part]));
        const merged = rcoMixEntries(await rcoStock(path), changes, (kind, key) => kinds.has(kind) ? mixSource(kinds.get(kind), key) : "", path);
        if (merged.size) {
          rcoFiles.set(path, merged);
          for (const part of rcoParts) {
            const own = [...merged.keys()].some(k => rcoEntryKind((state.rco.get(path).files).find(f => `${f.el}\t${f.attr}` === k)) === part.rco);
            if (own) for (const s of part.sections) sections.add(s);
          }
        }
      }
    }

    p.set(`Copying the parts…`, 0.55);
    await api("POST", "/api/themes/mix", { id, name }, lines.join("\n") + "\n");
    let i = 0;
    for (const [path, merged] of rcoFiles) {
      p.set(`Sending ${baseName(path)} to the console…`, 0.6 + 0.3 * i++ / rcoFiles.size);
      await uploadVerified("/api/theme/file", { id, path }, new Blob([rcoRebuild(await rcoStock(path), merged)]));
    }
    /* The editor's choices: the sections in use, and the background's, the Appearance pieces', the songs' and the
       icon style's own. */
    const c = { sections: Object.fromEntries([...sections].map(s => [s, 1])) };
    if (style) {
      c.iconStyle = style.choice;
      const styled = style.choice.titles.filter(t => /^[A-Z0-9]{9}$/.test(t));
      await uploadVerified("/api/theme/icon-style", { id, part: "styled" }, new Blob([styled.map(t => `${t}\n`).join("") || "\n"]));
    }
    if (m.picks.background) {
      const b = await choicesOf(m.picks.background);
      if (b.fit) c.fit = b.fit;
      if (b.bgOff) c.bgOff = b.bgOff;
    }
    if (m.picks.music) { const s = await choicesOf(m.picks.music); if (s.songs) c.songs = s.songs; }
    const appearance = mixPartById("appearance");
    for (const e of mixElements(appearance)) {
      const tid = mixSource(appearance, e.key), paths = tid && e.themes.get(tid);
      if (!paths) continue;
      const pieces = (await choicesOf(tid)).pieces || {};
      for (const path of paths) if (pieces[path]) (c.pieces = c.pieces || {})[path] = pieces[path];
    }
    await putChoices(id, c, true);
    if (rcoFiles.size) await api("POST", "/api/theme/save", { id });
    await refreshStatus();
    const from = [...used, ...rcoParts.flatMap(part => [m.picks[part.id], ...Object.values(m.elems[part.id] || {})])].filter(Boolean);
    p.done(`${name} made from ${plural(new Set(from).size, "theme")}. Open it to check, then apply it.`, "ok");
    openCreated(id);
  } catch (e) {
    p.done(`Mix and match: ${e.message}`, "err");
  }
}

/* The icon style that goes with the mix's icons: that of the theme most of its games', apps' and tiles' icons come
   from, if that theme has one (its shape, its template for games installed later, the picture behind). Only the
   icons that come from it count as made by it (the titles listed, and your pictures kept for them): one from another
   theme stays as that theme made it, and styling the mix again puts it in the shape. null: no style goes with them. */
async function mixIconStyle(choicesOf) {
  const from = new Map();   /* title or tile ID -> the theme its pictures come from */
  for (const part of [mixPartById("titles"), mixPartById("tiles")]) {
    for (const e of mixElements(part)) {
      const tid = mixSource(part, e.key);
      if (tid && e.themes.has(tid)) from.set(e.key, tid);
    }
  }
  const count = new Map();
  for (const tid of from.values()) count.set(tid, (count.get(tid) || 0) + 1);
  const order = [...count.keys()].sort((a, b) => count.get(b) - count.get(a)
    || (b === state.mix.picks.titles) - (a === state.mix.picks.titles) || (b === state.mix.picks.tiles) - (a === state.mix.picks.tiles));
  for (const tid of order) {
    const st = (await choicesOf(tid)).iconStyle;
    if (!st || !st.shape) continue;
    const r = await api("GET", "/api/theme/icon-style", { id: tid }).catch(() => ({}));
    const hand = new Set(st.hand || []), mine = t => from.get(t) === tid;
    const titles = [...new Set([...(st.titles || []), ...(r.made || []).filter(t => !hand.has(t))])].filter(mine);
    return { from: tid, own: (r.own || []).filter(mine), choice: { ...st, titles, hand: (st.hand || []).filter(mine) } };
  }
  return null;
}
