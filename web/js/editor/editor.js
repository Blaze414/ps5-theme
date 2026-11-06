"use strict";

/* ================================================================ editor (theme builder) */

async function renderEditor() {
  const themes = state.status.themes;
  if (!themes.length) {
    document.title = "PS5 Themes";
    view.innerHTML = `<div class="empty"><h2>Nothing to edit yet</h2>
      <p class="muted" style="margin:0">Create a theme first, then choose what it changes.</p>
      <button class="btn primary" data-act="new-from-editor">New theme</button></div>`;
    return;
  }
  if (!themeById(state.editId)) state.editId = themes[0].id;
  const t = themeById(state.editId);
  loads.set("open", -1);
  view.innerHTML = loadingMarker("open", `Opening ${t.name}…`);
  paintLoads(view);
  const stale = !!(state.assets && state.status.titles && state.status.titles !== state.assetsSeen);   /* games came or went */
  await Promise.all([loadAssets(stale), loadThemeFiles(t.id), loadChoices(t.id), loadPluginStyled(t.id)]);
  /* Left meanwhile (the library, or another theme opened): that page is the one drawn, never this one over it. */
  if (state.route !== "edit" || state.editId !== t.id) return;
  const on = state.status.applied === t.id;
  const blocked = applyBlocked(t);
  document.title = `${t.name} · PS5 Themes`;
  view.innerHTML = `
    <div class="head">
      <div class="edit-title"><a class="back" href="#/library">Library</a><h1 id="edit-name">${esc(t.name)}</h1></div>
      <span class="spacer"></span>
      <button class="btn ghost" data-act="preview" data-id="${esc(t.id)}" title="A drawing of the home screen with this theme">Preview</button>
      <button class="btn ghost" data-act="export" data-id="${esc(t.id)}" id="edit-export" ${t.files || t.music ? "" : `disabled title="Add something to the theme first."`}>Export</button>
      <button class="btn primary" data-act="apply" data-id="${esc(t.id)}" id="edit-apply" ${blocked ? `disabled title="${esc(blocked)}"` : ""}>${applyLabel(t, on)}</button>
    </div>
    ${on ? resultBanner() : ""}
    ${isLocal() && /PlayStation/i.test(navigator.userAgent)
      ? `<div class="banner">Adding pictures and music is easier from a computer or phone: open <b>http://${esc(state.status.ip)}:8095</b>.</div>` : ""}
    ${state.assetsError ? `<div class="banner err">${esc(state.assetsError)}. Choose No theme in the library and restart the plugin once, so it can list the console's files.</div>` : ""}
    ${!t.compatible ? `<div class="banner warn">${esc(blocked)}</div>` : ""}
    ${themeFolders() > maxFolders() ? `<div class="banner warn">This theme changes files in ${themeFolders()} folders, more than the ${maxFolders()} the plugin applies at once, so it can't be applied. Take some of its pictures out, or choose fewer system tiles in Games and apps.</div>` : ""}
    <div class="editor${state.section ? " picked" : ""}" id="editor">
      <nav class="secnav" id="secnav" aria-label="Parts of ${esc(t.name)}">${secNavHtml()}</nav>
      <div class="pane" id="pane"></div>
    </div>
    <div class="savebar" id="savebar" role="region" aria-label="Unsaved changes" ${t.unsaved ? "" : "hidden"}>
      <div class="text"><b>Unsaved changes</b></div>
      <button class="btn ghost" data-act="discard" data-id="${esc(t.id)}" id="discard-btn">Discard</button>
      <button class="btn primary" data-act="save" data-id="${esc(t.id)}">Save</button>
    </div>`;
  showPane(false);
  setTimeout(autoStyleIcons, 400);
}
