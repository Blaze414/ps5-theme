"use strict";

/* ---- The edit page: a list of the theme's parts beside the part you're changing, as the console's Settings are laid
   out. Each part has its own address (#/edit/<theme>/<part>), so Back and Forward step between the parts you opened.
   On a phone the list comes first and a part opens on its own. */

const ASPECT_GROUPS = [
  { id: "home", title: "Home screen", ids: ["background", "tiles", "titles"] },
  { id: "sound", title: "Music and sounds", ids: ["music", "sounds"] },
  { id: "look", title: "Fonts, colours and icons", ids: ["fonts", "appearance", "interface", "icons"] },
  { id: "advanced", title: "Advanced", ids: ["textures", "video", "animations", "images"], advanced: true },
];
const SECTION_NAMES = { theme: "Name and picture", tiles: "Tiles" };

/* Small line drawings for the parts that have no picture of their own (24×24, drawn in the text colour). */
const GLYPHS = {
  theme: `<rect x="3.5" y="5" width="17" height="14" rx="2.5"/><path d="M3.5 15l4.5-4 3.5 3 3-2.5 6 5"/><circle cx="15.5" cy="9" r="1.4"/>`,
  background: `<rect x="3.5" y="5" width="17" height="14" rx="2.5"/><path d="M3.5 15l4.5-4 3.5 3 3-2.5 6 5"/>`,
  tiles: `<rect x="3.5" y="3.5" width="7" height="7" rx="1.6"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.6"/><rect x="3.5" y="13.5" width="7" height="7" rx="1.6"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.6"/>`,
  titles: `<path d="M7 8.5h10a4 4 0 014 4v1.5a3 3 0 01-5.3 1.9L14.5 14.5h-5l-1.2 1.4A3 3 0 013 14v-1.5a4 4 0 014-4z"/><path d="M7.5 11v3M6 12.5h3"/>`,
  music: `<path d="M9 18V6l10-2v12"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="16.5" cy="16" r="2.5"/>`,
  sounds: `<path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z"/><path d="M15.5 9a4 4 0 010 6M18 6.5a7.5 7.5 0 010 11"/>`,
  fonts: `<path d="M4 19l5-14h1l5 14M6 14h7"/><path d="M20 19v-4.5a2.5 2.5 0 00-5 0M15 16.5c0 1.4 1.1 2.5 2.5 2.5S20 17.9 20 16.5"/>`,
  appearance: `<circle cx="12" cy="12" r="8.5"/><path d="M12 3.5a8.5 8.5 0 000 17z" fill="currentColor" stroke="none" opacity=".35"/>`,
  interface: `<circle cx="7.5" cy="7.5" r="3.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><path d="M4 20l3.5-6 3.5 6zM13 16.5h7M16.5 13v7"/>`,
  icons: `<path d="M12 3.8l2.4 4.9 5.4.8-3.9 3.8.9 5.4L12 16.2l-4.8 2.5.9-5.4-3.9-3.8 5.4-.8z"/>`,
  textures: `<path d="M12 4l8.5 4.5L12 13 3.5 8.5z"/><path d="M3.5 12.5L12 17l8.5-4.5M3.5 16.5L12 21l8.5-4.5"/>`,
  video: `<rect x="3.5" y="5" width="17" height="14" rx="2.5"/><path d="M10.5 9.2v5.6l4.5-2.8z"/>`,
  animations: `<path d="M3.5 17c3-9 6-9 8.5-4s5.5 5 8.5-4"/><circle cx="20.5" cy="9" r="1.2"/>`,
  images: `<rect x="6.5" y="3.5" width="14" height="11" rx="2"/><path d="M3.5 7.5v10a2 2 0 002 2h11"/>`,
};
const glyph = id => `<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${GLYPHS[id] || GLYPHS.images}</svg>`;

/* The console's own icon for each part, read from the console as the page runs (/api/ui-icon): nothing of Sony's
   is in this page. A firmware without one shows the drawing above instead. */
const PART_ICONS = {
  theme: "iconid_edit", background: "iconid_photo", tiles: "iconid_app_all_apps", titles: "iconid_games_and_apps",
  music: "iconid_music", sounds: "iconid_sound", fonts: "iconid_text_format_clear", appearance: "iconid_ps_shapes",
  interface: "iconid_customize_menu", icons: "iconid_grid", textures: "iconid_image_and_video",
  video: "iconid_screen_and_video", animations: "iconid_av_transition_effect", images: "iconid_app_media_gallery",
};
const partIcon = id => PART_ICONS[id]
  ? `<img class="ci" alt="" src="/api/ui-icon?${q({ id: PART_ICONS[id] })}" onerror="this.nextElementSibling.hidden=false;this.remove()"><span class="ci-fb" hidden>${glyph(id)}</span>`
  : glyph(id);

function advancedShown() {
  return !!state.showAdvanced || ASPECT_GROUPS.find(g => g.advanced).ids.some(id => id === state.section || aspectOn(aspectById(id)));
}
/* The parts in the order the list shows them: Previous and Next follow it. */
function sectionOrder() {
  const adv = advancedShown();
  return ["theme", ...ASPECT_GROUPS.filter(g => !g.advanced || adv).flatMap(g => g.ids)];
}
const sectionName = id => SECTION_NAMES[id] || (aspectById(id) || {}).title || id;
const UNCHANGED = new Set(["Console's own", "None on this firmware"]);
const sectionHref = id => `#/edit/${encodeURIComponent(state.editId)}${id ? `/${id}` : ""}`;

/* What a part holds in this theme, in a few words. */
function sectionStatus(id) {
  const t = themeById(state.editId);
  if (id === "theme") return t.fw === "any" ? "Any firmware" : `Firmware ${t.fw}`;
  const asp = aspectById(id);
  if (asp.mode === "music") {
    if (!t.music && !state.files.has(HOME_MUSIC)) return "Console's own";
    return isPlaylist(t) ? `${plural(themeTracks(t).length, "song")}, ${fmtTime(playlistSeconds(t))}` : t.music ? fmtTime(t.music) : "Changed";
  }
  if (!aspectAssets(asp).length) return "None on this firmware";
  const n = aspectReplaced(asp);
  const ui = id === "sounds" && uiSoundsLast && uiSoundsLast.id === state.editId ? uiSoundsLast.replaced : 0;
  const total = n + ui;
  return total ? `${total} changed` : aspectOn(asp) ? "Changed" : "Console's own";
}

/* The part's picture in the list: the theme's own where it has one the browser can show, otherwise its drawing. */
function sectionThumb(id) {
  const t = themeById(state.editId);
  const img = (src, cls = "") => `<img alt="" loading="lazy" class="${cls}" src="${esc(src)}" onerror="this.remove()">`;
  if (id === "theme") return t.cover ? img(coverUrl(t), "cover") : "";
  if (id === "background") {
    return aspectReplaced(aspectById(id)) ? img(`/api/theme/preview?${q({ id: t.id, kind: "background" })}&v=${t.coverTime}-${stamp}`, "cover") : "";
  }
  if (id === "fonts") {
    const f = themeFontFamily();
    return f ? `<span class="aa" style="font-family:'${f}'">Aa</span>` : "";
  }
  const asp = aspectById(id);
  if (!asp || !asp.filter || asp.mode === "rco") return "";
  for (const a of aspectAssets(asp)) {
    if (!state.files.has(a.path)) continue;
    if (isImage(a)) return img(`/api/theme/file?${q({ id: t.id, path: a.path })}&v=${stamp}`);
    if (state.sources.has(a.path)) return img(`/api/theme/source?${q({ id: t.id, path: a.path })}&v=${stamp}`);
  }
  return "";
}

function secNavItem(id) {
  const sel = (state.section || (wideEditor() ? "theme" : "")) === id;
  const status = sectionStatus(id), changed = id !== "theme" && !UNCHANGED.has(status);
  return `<a class="nav-item${sel ? " sel" : ""}${changed ? " on" : ""}" href="${sectionHref(id)}" data-section="${id}"${sel ? ` aria-current="page"` : ""}>
      <span class="nav-thumb">${sectionThumb(id) || partIcon(id)}</span>
      <span class="nav-text"><span class="nav-name">${esc(sectionName(id))}</span><span class="nav-status">${esc(status)}</span></span>
    </a>`;
}
function secNavHtml() {
  const adv = advancedShown();
  return `${secNavItem("theme")}${ASPECT_GROUPS.map(g => {
    const used = g.ids.some(id => aspectOn(aspectById(id)) || id === state.section);
    const head = `<div class="nav-group"><span>${esc(g.title)}</span>${g.advanced && !used
      ? `<button type="button" class="nav-more" data-act="advanced" aria-expanded="${adv}">${adv ? "Hide" : "Show"}</button>` : ""}</div>`;
    return g.advanced && !adv ? `${head}<p class="nav-hint">Textures, video, animations and other images.</p>` : head + g.ids.map(secNavItem).join("");
  }).join("")}`;
}
/* One part's line in the list follows what it holds. */
function syncNav(id) {
  const el = document.querySelector(`#secnav [data-section="${id}"]`);
  if (!el) return;
  const tmp = document.createElement("div");
  tmp.innerHTML = secNavItem(id);
  const now = tmp.firstElementChild, had = document.activeElement === el;
  el.replaceWith(now);
  if (had) now.focus({ preventScroll: true });   /* a controller's place in the list stays where it was */
}
const wideEditor = () => matchMedia("(min-width: 900px)").matches;

/* The part shown beside the list: the theme's name and picture, or one section, with the way to the parts before and
   after it. `focus`: move to its title, after choosing it. */
function showPane(focus = true) {
  const pane = $("#pane"), ed = $("#editor");
  if (!pane || !ed) return;
  const id = state.section || "theme";
  ed.classList.toggle("picked", !!state.section);
  for (const a of document.querySelectorAll("#secnav .nav-item")) {
    const sel = a.dataset.section === (state.section || (wideEditor() ? "theme" : ""));
    a.classList.toggle("sel", sel);
    if (sel) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current");
  }
  const t = themeById(state.editId);
  pane.innerHTML = `
    <a class="back list-back" href="${sectionHref("")}">All parts</a>
    ${id === "theme" ? themePaneHtml(t) : `<section class="aspect" id="aspect-${id}"></section>`}
    <nav class="steps" id="steps" aria-label="Previous and next part">${stepsHtml(id)}</nav>`;
  if (id !== "theme") renderAspect(aspectById(id)); else setTimeout(optionsShow);
  if (focus) {
    const h = pane.querySelector("h2");
    if (h) { h.setAttribute("tabindex", "-1"); h.focus({ preventScroll: true }); }
    const top = ed.getBoundingClientRect().top;
    if (top < 0 || !wideEditor()) ed.scrollIntoView({ block: "start" });
  }
}
