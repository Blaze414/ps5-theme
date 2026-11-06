"use strict";

/* ---- An icon the style made that the page lost track of ----

   Counted as changed by hand, styling again would put it, styled already, inside the shape a second time. One whose
   every pixel outside the shape (clear of its edge) matches the template as the console keeps it is the style's:
   it goes back on the style's list (and the console's), so styling again starts from the title's own icon. Looked
   at once per page per title, and again when the template changes. */
const notStyle = new Set();   /* "<theme>|<title>": looked at, not the style's */
async function adoptStyledIcons() {
  const st = iconStyle(), id = state.editId, kept = styleMadeByPlugin.id === id ? styleMadeByPlugin : null;
  if (!st.shape || !kept || !kept.template) return 0;
  const candidates = handChanged().filter(g => !notStyle.has(`${id}|${g.title}`));
  if (!candidates.length) return 0;
  const N = 96, rgba = img => {
    const c = document.createElement("canvas");
    c.width = c.height = N;
    const x = c.getContext("2d");
    x.imageSmoothingQuality = "high";
    x.drawImage(img, 0, 0, N, N);
    return x.getImageData(0, 0, N, N).data;
  };
  let base, mask;
  try { [base, mask] = (await Promise.all(["base", "mask"].map(part => loadImage(`/api/theme/icon-style?${q({ id, part })}&v=${Date.now()}`)))).map(rgba); }
  catch (e) { return 0; }
  const found = [];
  for (const g of candidates) {
    const set = g.sets.find(x => x.stem === "icon0"), png = set && set.files.find(a => ext(a.path) === "png" && state.files.has(a.path));
    let ours = false;
    if (png) { try { ours = styleMatch(rgba(await loadImage(`/api/theme/file?${q({ id, path: png.path })}&v=${stamp}`)), base, mask, N, N); } catch (e) { ours = false; } }
    if (state.editId !== id) return 0;
    if (ours) found.push(g.title); else notStyle.add(`${id}|${g.title}`);
  }
  if (!found.length) return 0;
  const t = themeById(id), had = !!(t && t.unsaved);
  await setIconStyle(x => { x.titles = [...new Set([...x.titles, ...found])]; x.hand = x.hand.filter(v => !found.includes(v)); });
  const list = [...new Set(iconStyle().titles.filter(v => /^[A-Z0-9]{9}$/.test(v)))];
  await uploadVerified("/api/theme/icon-style", { id, part: "styled" }, new Blob([list.map(v => `${v}\n`).join("") || "\n"])).catch(() => {});
  if (!had) { await api("POST", "/api/theme/save", { id }).catch(() => {}); await refreshStatus(); }
  titleNamesEnsure();
  if (titleNamesLoading) await titleNamesLoading.catch(() => {});
  toast(`${namesOf(found)}: ${found.length === 1 ? "its icon has" : "their icons have"} this style already, so ${found.length === 1 ? "it counts" : "they count"} as styled again.`, "", 8000);
  renderAspect(aspectById("titles"));
  return found.length;
}

/* The style taken off these titles' icons (installed ones, and tiles): your own picture again where you had changed
   one, the console's icon for the rest. `p`, the progress to show it in. How many got your picture back. */
async function unstyleGroups(groups, p) {
  const own = ownPictures();
  let yours = 0;
  for (let i = 0; i < groups.length; i++) {
    const g = groups[i];
    p.set(`Removing the style: ${titleName(g.title)} (${i + 1} of ${groups.length})…`, i / groups.length, (i + 1) / groups.length);
    if (own.has(g.title)) {   /* an icon you had changed: your picture again */
      try {
        const res = await fetch(ownPictureUrl(g.title));
        if (!res.ok) throw new Error("not kept");
        const file = new File([await res.blob()], `${g.title}-icon.png`, { type: "image/png" });
        if (await stylePicture(g, file)) { yours++; continue; }
      } catch (e) { /* not readable: the console's own, as for the others */ }
    }
    await resetAssets(g.sets.find(x => x.stem === "icon0").files);
  }
  return yours;
}

async function removeIconStyle() {
  titleNamesEnsure();
  tileNamesEnsure();
  const made = styledTitles(), own = ownPictures();
  /* the console's tiles the style made, whether or not the style is still for them */
  const groups = [...titleGroups(), ...tileStyleGroups(true)].filter(g => made.has(g.title));
  /* Titles not installed now: the style's icon from the console's own goes (the console's icon when installed
     again); one made from your picture stays, listed, with your picture kept: it can only go back as an icon file
     of the console's form once the title is installed. */
  const here = new Set(groups.map(g => g.title)), away = [...made].filter(t => !here.has(t));
  const waiting = away.filter(t => own.has(t));
  const iconFiles = t => [...state.files].filter(p => p.startsWith(`/user/appmeta/${t}/icon0.`) || p.startsWith(`/user/app/${t}/sce_sys/icon0.`)).map(path => ({ path }));
  for (const t of away.filter(t => !own.has(t))) await resetAssets(iconFiles(t));
  const p = progressToast(`Removing the style from ${plural(groups.length, "icon")}…`);
  const yours = await unstyleGroups(groups, p);
  await setIconStyle(x => { x.titles = waiting; x.hand = []; x.auto = false; });
  await api("POST", "/api/theme/icon-style", { id: state.editId, auto: 0, clear: 1 }).catch(() => {});
  if (waiting.length) {   /* still the style's: listed, so it is put right once installed */
    const list = new Blob([waiting.map(t => `${t}\n`).join("")]);
    await uploadVerified("/api/theme/icon-style", { id: state.editId, part: "styled" }, list).catch(() => {});
  }
  await loadPluginStyled();
  const rest = groups.length - yours;
  p.done(`The style is off ${plural(groups.length, "icon")}: ${yours ? `${yours} with your own picture again${rest ? `, ${rest} with the console's` : ""}` : "the console's own again in this theme"}.` +
    `${waiting.length ? ` ${plural(waiting.length, "game")} not installed now ${waiting.length === 1 ? "keeps" : "keep"} the style, with your picture kept, until installed again: press Remove the style again then.` : ""} Apply the theme to see it.`, "ok");
  stamp = Date.now();
  await refreshStatus();
  renderAspect(aspectById("titles"));
  renderAspect(aspectById("tiles"));
}

/* A system tile chosen, or not (`ids`: its tiles, "NPXS40145,NPXS40205"). Not: put back as it was (your picture,
   or the console's) where the style made it, and left out of the style. Chosen: styled with the rest the next time
   you style the icons. */
async function styleTile(ids, on) {
  const list = ids.split(",").filter(id => id in STYLE_TILES);
  if (on) {
    await setIconStyle(x => { x.tiles = [...new Set([...x.tiles, ...list])]; });
    return renderAspect(aspectById("titles"));
  }
  const made = styledTitles(), groups = tileStyleGroups(true).filter(g => list.includes(g.title) && made.has(g.title));
  await setIconStyle(x => { x.tiles = x.tiles.filter(t => !list.includes(t)); });
  if (!groups.length) return renderAspect(aspectById("titles"));
  const name = titleName(list[0]);
  const p = progressToast(`Putting back the ${name} icon…`);
  const yours = await unstyleGroups(groups, p);
  const off = new Set(groups.map(g => g.title));
  await setIconStyle(x => { x.titles = x.titles.filter(t => !off.has(t)); });
  for (const t of off) if (ownPictures().has(t)) await api("POST", "/api/theme/icon-style", { id: state.editId, forget: t }).catch(() => {});
  await loadPluginStyled();
  p.done(`${name}: ${yours ? "your own picture" : "the console's icon"} again. Apply the theme to see it.`, "ok");
  stamp = Date.now();
  await refreshStatus();
  renderAspect(aspectById("titles"));
  renderAspect(aspectById("tiles"));
}

/* What a title's picture is, in plain words: [button label, where it shows]. */
const PICTURE_NAMES = {
  icon0: ["icon", "The icon: the game's tile on the home screen and in the Game Library."],
  pic0: ["background", "The background: shown behind the home screen while the game is selected."],
  logo: ["logo", "The logo: the game's name as artwork."],
};
function pictureName(stem) {
  return PICTURE_NAMES[stem] || [`other artwork (${stem})`, `Other artwork the console keeps for the game (${stem}).`];
}

function titlesBody(asp) {
  const groups = titleGroups();
  if (!groups.length) return `<p class="note">No installed games or apps with pictures were found.</p>`;
  setTimeout(titleNamesEnsure);
  const filter = (state.filters[asp.id] || "").toLowerCase();
  const name = g => state.titleNames && state.titleNames.has(g.title) ? state.titleNames.get(g.title) || g.title : "Reading…";
  const shown = filter ? groups.filter(g => `${name(g)} ${g.title}`.toLowerCase().includes(filter)) : groups;
  const page = state.pages[asp.id] || 48;
  const rows = shown.slice(0, page).map(g => {
    const rep = g.pictures.some(a => state.files.has(a.path));
    /* Titles whose PlayStation Network pictures (a concept's) are this one's too. */
    const shared = [...new Set(g.pictures.flatMap(a => titlesOf(a.path)))].filter(t => t !== g.title)
      .map(t => (state.titleNames && state.titleNames.get(t)) || t);
    return `<div class="row-item${rep ? " replaced" : ""}">
      <div class="info"><div class="file">${esc(name(g))}${rep ? ` <span class="tag on">Changed</span>` : ""}</div>
        <div class="dir">${esc(g.title)} · ${plural(g.sets.length, "picture")}${shared.length ? ` · shares pictures with ${esc(shared.join(", "))}` : ""}</div></div>
      <div class="strip">${thumbFor(g.pictures.find(a => isImage(a) && state.files.has(a.path)) || g.pictures.find(isImage) || g.pictures[0])}</div>
      <div class="acts">${g.sets.map(x => `<button class="btn sm" data-act="title-pic" data-title="${esc(g.title)}" data-stem="${esc(x.stem)}" title="${esc(`${pictureName(x.stem)[1]} Files: ${x.files.map(a => a.path.replace(/^\/user\//, "")).join(", ")}`)}">Change ${esc(pictureName(x.stem)[0])}</button>`).join("")}
        ${rep ? `<button class="btn sm ghost danger" data-act="title-reset" data-title="${esc(g.title)}">Reset</button>` : ""}
      </div></div>`;
  }).join("");
  setTimeout(iconStyleShow);
  return `${iconStyleHtml()}
    <p class="note">A game whose pictures the theme changes can't be updated: choose <b>No theme</b> before an update, then apply the theme again.</p>
    ${groups.length > 12 ? `<div class="toolbar"><input type="search" data-filter-aspect="${asp.id}" placeholder="Search games and apps" value="${esc(state.filters[asp.id] || "")}" aria-label="Search games and apps"></div>` : ""}
    <div class="rows">${rows || `<p class="note">Nothing matches.</p>`}</div>
    ${shown.length > page ? `<div><button class="btn sm" data-act="more" data-aspect="${asp.id}">Show more (${shown.length - page})</button></div>` : ""}`;
}

function rcoBody(asp) {
  const all = aspectAssets(asp);
  if (!all.length) return `<p class="note">This firmware has no interface files that can be themed.</p>`;
  setTimeout(rcoEnsureLoaded);
  setTimeout(rcoShapesShow);
  const rows = all.slice().sort((a, b) => a.path.localeCompare(b.path)).map(a => {
    const rep = state.files.has(a.path), r = state.rco && state.rco.get(a.path), failed = rcoFailed.get(a.path);
    const pics = r ? rcoPictures(r) : null, svgs = pics ? pics.filter(x => x.kind === "svg").length : 0;
    const what = pics ? `${plural(pics.length, "picture")}${svgs ? `, ${svgs} of them vector icons` : ""}`
      : failed ? `Could not be read: ${esc(failed)}` : loadingInline(`stock:${a.path}`);
    return `<div class="row-item${rep ? " replaced" : ""}">
      <div class="info"><div class="file">${esc(baseName(a.path))}${rep ? ` <span class="tag on">Changed</span>` : ""}</div>
        <div class="dir">${what} · ${fmtSize(a.size)}</div></div>
      <div class="acts">${failed ? `<button class="btn sm" data-act="rco-retry">Try again</button>` : ""}${pics && pics.length ? `
        ${svgs ? `<button class="btn sm" data-act="rco-number" data-path="${esc(a.path)}" data-vector="1">Number vector icons</button>` : ""}
        <button class="btn sm" data-act="rco-number" data-path="${esc(a.path)}">Number all</button>` : ""}
        ${rep ? `<button class="btn sm ghost danger" data-act="rco-reset" data-path="${esc(a.path)}">Remove</button>` : ""}
      </div></div>`;
  }).join("");
  return `<p class="note">To find a picture, number a file's pictures, apply, use rest mode, then look up the number you see.${help("Each picture becomes a number on a pink square. Start with Sce.PlayStation.PUI_UI3.rco and its vector icons. The interface reads these files when it starts, so apply the theme, then put the console in rest mode and wake it. If it keeps restarting, the plugin takes the theme down by itself. Only pictures, sounds (under Sounds), colours and corner shapes change in these files; everything else stays the console's own.")}</p>
    <div class="rows">${rows}</div>
    <p class="note">Look up a picture by its number or its name (search, settings, home, power…), then choose an image.${help("Your image is written in exactly the format of the console's picture; for a vector icon it is drawn inside the console's own SVG. Numbering a file again replaces your pictures in it.")}</p>
    <div class="toolbar"><input type="search" id="rco-lookup" placeholder="A number from the screen, or a name" aria-label="Look up a picture"
      value="${esc(state.rcoLookup || "")}"></div>
    <div id="rco-lookup-result"></div>
    <div id="rco-shapes"></div>`;
}
