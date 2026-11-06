"use strict";

/* ---------------------------------------------------------------- games and apps */

/* A title's picture by name: icon0.png and icon0.dds are the same icon, and
   so are the numbered copies PlayStation Network's pictures come in
   (icon0_11.png, logo_00.png ... logo_30.png, pic0_08.jpg). */
function pictureStem(path) { return baseName(path).replace(/\.(png|dds|jpe?g|webp)$/i, "").replace(/_\d+$/, "").toLowerCase(); }

/* Installed titles and their pictures, the icon first. A picture can be in
   several files: a PNG and a texture, in the title's /user/appmeta folder
   and in the sce_sys folder of its installation, and the PlayStation Network
   copies (titlesOf). Which one the home screen shows depends on the title
   (most PS5 games' records point at sce_sys, some at the network's), so
   each picture (`sets`) is changed in all its files at once. */
function titleGroups() {
  const groups = new Map();
  for (const a of aspectAssets(aspectById("titles"))) {
    for (const t of titlesOf(a.path)) {
      if (!groups.has(t)) groups.set(t, { title: t, pictures: [] });
      groups.get(t).pictures.push(a);
    }
  }
  /* Installed ones only: PlayStation Network's pictures are also kept for
     games that aren't (or are no longer) installed. */
  for (const [t, g] of groups) {
    if (!g.pictures.some(a => /^\/user\/app(?:meta)?\//.test(a.path))) groups.delete(t);
  }
  const rank = { icon0: 0, pic0: 1, logo: 2 };
  const iconFirst = stem => stem in rank ? rank[stem] : 3, pngFirst = a => ext(a.path) === "png" ? 0 : 1;
  for (const g of groups.values()) {
    g.pictures.sort((a, b) => iconFirst(pictureStem(a.path)) - iconFirst(pictureStem(b.path)) ||
      pictureStem(a.path).localeCompare(pictureStem(b.path)) || pngFirst(a) - pngFirst(b) || a.path.localeCompare(b.path));
    const sets = new Map();
    for (const a of g.pictures) {
      if (!sets.has(pictureStem(a.path))) sets.set(pictureStem(a.path), []);
      sets.get(pictureStem(a.path)).push(a);
    }
    g.sets = [...sets].map(([stem, files]) => ({ stem, files }));
  }
  const name = g => (state.titleNames && state.titleNames.get(g.title)) || g.title;
  return [...groups.values()].sort((a, b) => name(a).localeCompare(name(b)));
}

let titleNamesLoading = null;
function titleNamesEnsure() {
  state.titleNames = state.titleNames || new Map();
  const missing = titleGroups().filter(g => !state.titleNames.has(g.title));
  if (!missing.length || titleNamesLoading) return;
  titleNamesLoading = (async () => {
    for (const g of missing) {
      let name = "";
      const dir = `/user/appmeta/${g.title}`;
      try {
        const res = await fetch(`/api/asset?${q({ path: `${dir}/param.json` })}`);
        if (res.ok) {
          const pj = await res.json(), loc = pj.localizedParameters || {};
          name = ((loc[loc.defaultLanguage] || loc["en-US"] || Object.values(loc).find(v => v && v.titleName) || {}).titleName) || "";
        }
      } catch (e) { /* not JSON */ }
      if (!name) {
        try {
          const res = await fetch(`/api/asset?${q({ path: `${dir}/param.sfo` })}`);
          if (res.ok) name = sfoTitle(new Uint8Array(await res.arrayBuffer()));
        } catch (e) { /* no param.sfo either */ }
      }
      state.titleNames.set(g.title, name);
    }
  })().finally(() => { titleNamesLoading = null; renderAspect(aspectById("titles")); });
}

/* One of a title's pictures from your image, in every file that holds it:
   a PNG in exactly the format of the console's, a texture (and anything
   else) through the usual conversion into the console's exact format. */
async function titlePicture(title, stem, file, quiet = false) {
  const g = titleGroups().find(x => x.title === title), set = g && g.sets.find(x => x.stem === stem);
  if (!set) return;
  const label = `${(state.titleNames && state.titleNames.get(title)) || title} ${pictureName(stem)[0]}`;
  const rest = [];
  let failed = false, written = 0;
  for (const a of set.files) {
    if (ext(a.path) !== "png") { rest.push(a); continue; }
    try {
      const res = await fetch(`/api/asset?${q({ path: a.path })}`);
      if (!res.ok) throw new Error("cannot read the console's picture");
      const f = pngFormat(new Uint8Array(await res.arrayBuffer()));
      if (!f || !f.writable) { rest.push(a); continue; }
      await uploadVerified("/api/theme/file", { id: state.editId, path: a.path }, new Blob([await encodePng(await imageRgba(file, f.w, f.h), f, zlibDeflate)]));
      state.files.add(a.path);
      written++;
    } catch (e) {
      failed = true;
      if (!quiet) toast(`${label} (${baseName(a.path)}): ${e.message}`, "err", 9000);   /* a batch says how many failed */
    }
  }
  if (rest.length && (await attachTo(rest, file, label, written, false, null, quiet)) < rest.length) failed = true;
  else if (!rest.length && !failed && !quiet) toast(`${label} replaced${written > 1 ? ` in ${plural(written, "file")}` : ""} and verified. Apply the theme to use it.`, "ok");
  stamp = Date.now();
  if (quiet) return !failed;
  await refreshStatus();
  renderAspect(aspectById("titles"));
  return !failed;
}
