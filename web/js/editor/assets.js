"use strict";

/* ================================================================ assets & grouping */

/* The console's files, read once; again (`force`) when its games and apps change (followConsole). The list read
   replaces the one shown only once it is whole. */
async function loadAssets(force = false) {
  if (state.assets && !force) return;
  const seen = state.status && state.status.titles;
  try {
    const r = await api("GET", "/api/assets", { limit: 5000 });
    state.assets = r.assets;
    state.concepts = r.concepts || {};
    state.protectedCount = r.protected || 0;
    state.assetsError = "";
    state.assetsSeen = seen;
  } catch (e) {
    if (force && state.assets) return;   /* the list it has stays: read again at the next look */
    state.assets = null;
    state.assetsError = e.message;
  }
}

/* Each of the open theme's loaders keeps what it read only if that theme is still the one open: another opened
   meanwhile (or the first one finishing last) never gets this one's files, choices or icons. */
async function loadThemeFiles(id) {
  let files;
  try {
    const r = await api("GET", "/api/theme/files", { id });
    files = new Set(r.files.map(f => f.path));
  } catch (e) { files = new Set(); }
  if (id !== state.editId) return;
  state.files = files;
  /* Which files have the picture they were made from kept in the theme. */
  await loadOriginals(id);
}

/* Which files have their original kept, whether the theme keeps them, and the space they take. */
async function loadOriginals(id = state.editId) {
  let r = null;
  try { r = await api("GET", "/api/theme/sources", { id }); } catch (e) { /* none known: the defaults below */ }
  if (id !== state.editId) return;
  state.sources = new Set();
  state.keepOriginals = true;
  state.originalsBytes = 0;
  try {
    if (!r) throw new Error("not read");
    state.sources = new Set(r.have);
    state.keepOriginals = r.keep !== false;
    state.originalsBytes = r.bytes || 0;
  } catch (e) { /* older plugin: none */ }
  const box = $("#originals-field");
  if (box && state.editId === id) box.innerHTML = originalsField();
}

/* The switch for keeping originals, with what is kept. */
function originalsField() {
  const n = state.sources.size;
  const kept = n ? `${plural(n, "original")} kept, ${fmtSize(state.originalsBytes)}. <button class="btn sm ghost" data-act="remove-originals" type="button">Remove them</button>` : "";
  return `<label class="note">
      <input type="checkbox" id="keep-originals"${state.keepOriginals ? " checked" : ""}> Keep original pictures${help("With the original picture kept, the fit or placement of what you attached can be changed later without attaching it again. It takes space on the console: about the size of the picture, for each file it went to (it is sent once). Never applied to the console.")}</label>
    <span class="note">${state.keepOriginals ? "" : "New pictures are not kept: options apply when you attach one. "}${kept}</span>`;
}

/* The picture a see-through texture was made from is kept in the theme, so changing its placement makes the
   texture again from it, without the picture being attached again. */
async function keepSource(path, file, sameAs = null) {
  if (!state.keepOriginals) return;
  if (sameAs) {
    /* the same picture as one just kept for another size: the console links it instead of storing it twice */
    try { await api("PUT", "/api/theme/source", { id: state.editId, path, same_as: sameAs }); state.sources.add(path); return; }
    catch (e) { /* an older plugin, or no link possible: it is sent */ }
  }
  try {
    await uploadVerified("/api/theme/source", { id: state.editId, path }, file);
    state.sources.add(path);
  } catch (e) { state.sources.delete(path); }
}
/* The pictures and textures made from the user's picture: its original is kept, not only the result. */
const keepsOriginal = a => a.kind === "texture" || isImage(a);

/* The kept original of a file, as a File; null when there is none. */
async function sourceFile(path, id = state.editId, onProgress = null) {
  const res = await fetch(`/api/theme/source?${q({ id, path })}`);
  if (!res.ok) { state.sources.delete(path); return null; }
  const bytes = await readBytes(res, onProgress);
  const type = sniffImage(bytes) || "image/png";
  return new File([bytes], `picture.${type.split("/")[1]}`, { type });
}
async function remakeFromSource(path, p = null) {
  const asset = (state.assets || []).find(a => a.path === path);
  if (!asset || !state.sources.has(path)) return false;
  const from = p ? p.frac : 0;
  const file = await sourceFile(path, state.editId, p ? f => p.set("Reading the picture kept in the theme…", from + 0.1 * Math.max(0, f)) : null);
  if (!file) return false;
  await attachTo([asset], file, appearanceName(path), null, true, p);
  return true;
}

/* "How images are fitted" changed: every picture and texture made from a kept original is made again with it. A
   change made while this runs is picked up by one more round. The Appearance pieces have their own placement. */
let refitting = false, refitAgain = false;
async function refitPictures() {
  if (refitting) { refitAgain = true; return; }
  refitting = true;
  const id = state.editId;
  try {
    do {
      refitAgain = false;
      const here = (state.assets || []).filter(a => keepsOriginal(a) && state.files.has(a.path) && !ALPHA_TEXTURE.test(a.path));
      const targets = here.filter(a => state.sources.has(a.path));
      const left = here.length - targets.length;
      const leftNote = left ? ` ${plural(left, "picture")} ${left === 1 ? "has" : "have"} no original kept and ${left === 1 ? "stays" : "stay"} as ${left === 1 ? "it is" : "they are"}.` : "";
      if (!targets.length) { if (left) toast(`The new fit applies to pictures attached from now on.${leftNote}`, "", 7000); break; }
      const p = progressToast(`Fitting ${plural(targets.length, "picture")} again…`);
      let ok = 0;
      const errors = [];
      for (let i = 0; i < targets.length && state.editId === id; i++) {
        const a = targets[i];
        const at = x => (i + x) / targets.length;
        attachProgress = f => p.set(`Fitting ${baseName(a.path)}… ${Math.round(f * 100)}%`, at(0.1 + 0.6 * f));
        try {
          p.set(`Reading the original of ${baseName(a.path)}…`, at(0));
          const file = await sourceFile(a.path, id, f => p.set(`Reading the original of ${baseName(a.path)}…`, at(0.1 * Math.max(0, f))));
          if (!file) throw new Error("its original is gone");
          p.set(`Fitting ${baseName(a.path)}…`, at(0.1));
          const made = await fileForAsset(file, a);
          p.set(`Sending ${baseName(a.path)} to the console…`, at(0.7), at(1));
          await uploadVerified("/api/theme/file", { id, path: a.path }, made);
          textureCache.clear();
          ok++;
        } catch (e) { errors.push(`${baseName(a.path)}: ${e.message}`); }
        p.set(`Fitting pictures again… ${i + 1}/${targets.length}`, (i + 1) / targets.length);
      }
      attachProgress = null;
      stamp = Date.now();
      /* the card's picture follows the background */
      const bg = backgroundFile(targets.map(a => a.path));
      if (bg && !errors.length && state.editId === id) {
        try { const f = await sourceFile(bg, id); if (f) await previewFromFile(id, f, false); } catch (e) { /* the old preview stays */ }
      }
      p.done(errors.length ? `${ok} of ${targets.length} made again. ${errors.slice(0, 2).join("; ")}${leftNote}`
        : `${plural(ok, "picture")} made again with the new fit, from their originals. Apply the theme to use ${ok === 1 ? "it" : "them"}.${leftNote}`,
        errors.length ? "err" : "ok");
      await refreshStatus();
      if (state.editId === id) ASPECTS.forEach(renderAspect);
    } while (refitAgain && state.editId === id);
  } finally { refitting = false; attachProgress = null; }
}

/* A piece attached before the original picture was kept: the picture inside its texture becomes the original, so
   its options work without attaching anything again. What the earlier placement cropped away is not in it. */
async function recoverSource(path, p = null) {
  const res = await fetch(`/api/theme/file?${q({ id: state.editId, path })}&v=${stamp}`);
  if (!res.ok) return false;
  let bytes = await readBytes(res, p ? f => p.set("Reading the picture inside the texture…", 0.1 * Math.max(0, f)) : null);
  if (bytes[0] === 0x28) bytes = unzstdStored(bytes);
  const canvas = await ddsCanvas(bytes, true);
  const blob = await new Promise(done => canvas.toBlob(done, "image/png"));
  if (!blob) return false;
  if (p) p.set("Keeping it as the original…", 0.1, 0.2);
  await keepSource(path, new File([blob], "picture.png", { type: "image/png" }));
  return state.sources.has(path);
}

/* Resolution variants of one asset share a key: size tokens such as @2x,
   _4k, _1080 or a /2160/ folder are ignored, so one file fills them all. */
const SIZE_TOKEN = /(@\d+(?:\.\d+)?x|[_-](?:\d{3,4}p?|[2-8]k|uhd|fhd|qhd|hd|sd|hidpi|retina|small|large|sm|lg|xl|xxl|x[1-4]|[1-4]x))(?=\.[a-z0-9]+$)/i;
const SIZE_DIR = /\/(?:\d{3,4}p?|[2-8]k|uhd|fhd|qhd|hd|sd|x[1-4]|[1-4]x|@\d+x)(?=\/|$)/gi;
function groupKey(path) { return dirName(path).replace(SIZE_DIR, "/*") + "/" + baseName(path).replace(SIZE_TOKEN, ""); }
function groupAssets(list) {
  const map = new Map();
  for (const a of list) {
    const k = groupKey(a.path);
    if (!map.has(k)) map.set(k, { key: k, name: baseName(k), dir: dirName(a.path), assets: [] });
    map.get(k).assets.push(a);
  }
  return [...map.values()].sort((a, b) => a.name.localeCompare(b.name) || a.key.localeCompare(b.key));
}

const isImage = a => a.kind === "image" || a.kind === "svg";

/* The console's text fonts by what they draw, not by file name. */
/* The 30th anniversary Appearance themes' artwork (Settings > Appearance and Sound): the Control Center's panels and
   the home screen's artwork, one set per theme. They show only while that theme is selected on the console. */
const APPEARANCE_RE = /^\/system\/vsh_asset\/(control_center|homeui)_(ps\d?)(?:_(top|bottom))?\.dds$/;
const APPEARANCE_THEMES = { ps: "PlayStation", ps2: "PlayStation 2", ps3: "PlayStation 3", ps4: "PlayStation 4" };
function appearanceName(path) {
  const m = APPEARANCE_RE.exec(path);
  if (!m) return baseName(path);
  const part = m[1] === "homeui" ? "Home screen artwork" : m[3] === "top" ? "Control Center, top-right corner" : "Control Center, bottom panel";
  return `${APPEARANCE_THEMES[m[2]] || m[2]}: ${part}`;
}

const FONT_FAMILIES = [
  [/^SSTArabic/i, "Arabic text"], [/^SSTThai/i, "Thai text"], [/^SSTVietnamese/i, "Vietnamese text"],
  [/^SSTTypewriter/i, "Typewriter-style text"], [/^SSTArib/i, "Japanese captions"],
  [/^(SSTJpPro|SCE-RDC|SIE-RDC)/i, "Japanese text"], [/^(YoonGothic|SCEPS4Yoon)/i, "Korean text"],
  [/^DFHEI/i, "Chinese text"], [/^SST/i, "Interface text (Latin)"],
];
/* The fonts the home screen (ShellUI) has in use on firmware 13.42, found by listing the font files the process
   has mapped after its text was drawn: the Light and Bold weights of the Latin, Arabic, Thai, Vietnamese and
   Korean fonts, and the Japanese medium and bold. The Regular and Medium Latin fonts are used by web pages. */
const FONT_HOME = /^(SST|SSTArabic|SSTThai|SSTVietnamese)-(Light|Bold)\.otf$|^SIE-RDC-Pr5N-(M|B)-JPN\.otf$|^SCEPS4Yoongd-(Light|Bold)\.otf$/;
const FONT_LATIN = /^SST-[A-Za-z]+\.otf$/;
const fontRank = a => FONT_HOME.test(baseName(a.path)) ? (FONT_LATIN.test(baseName(a.path)) ? 0 : 1) : 2;
function fontName(path) {
  const f = baseName(path).replace(/\.[^.]+$/, "");
  const fam = FONT_FAMILIES.find(([re]) => re.test(f));
  if (!fam) return baseName(path);
  const weight = /Bold|-Bd$|-B-/i.test(f) ? "bold" : /Light/i.test(f) ? "light" : /Medium|-M-/i.test(f) ? "medium" : /Roman|Regular|-R-/i.test(f) ? "regular" : "";
  const extra = [weight, /Italic/i.test(f) ? "italic" : ""].filter(Boolean).join(" ");
  return extra ? `${fam[1]}, ${extra}` : fam[1];
}

/* A readable name for where an asset is used, from its package folder:
   ".../assets/packages/mini-profile/..." -> "Mini profile". */
const AREA_SKIP = new Set(["src", "assets", "packages", "modules", "node_modules", "lib", "dist", "images", "img", "screens", "components", "res", "resources"]);
function areaOf(path) {
  if (path.startsWith("/system/vsh_asset/")) return /^welcome-hub/i.test(baseName(path)) ? "Welcome tab backgrounds" : "Appearance themes";
  if (path.includes("/vsh_asset/")) {
    const f = baseName(path);
    if (/^bg_hub_default/i.test(f)) return "Home: default background";
    if (/^library_bg/i.test(f)) return "Home: Game Library";
    const t = /^bg_(NPXS\d+)/i.exec(f);
    if (t) return `Home: ${t[1]} tile`;
    if (/^bg_image_load_error/i.test(f)) return "Image load error placeholder";
    return "System (VSH)";
  }
  const m = /\/apps\/([^/]+)\/(?:assets\/)?(.*)$/.exec(path);
  if (!m) return "Other";
  const segs = m[2].split("/").slice(0, -1).filter(x => !AREA_SKIP.has(x) && !x.startsWith("@") && !x.startsWith("."));
  const raw = segs[0] || m[1];
  return raw.replace(/[-_]+/g, " ").replace(/^./, c => c.toUpperCase());
}
