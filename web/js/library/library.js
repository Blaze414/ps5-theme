"use strict";

/* ================================================================ library */

function cardPreview(t) {
  if (t.cover) return `<div class="preview shot" data-cover="${esc(t.id)}"><img loading="lazy" alt="${esc(t.name)}" src="${coverUrl(t)}"></div>`;
  return `<div class="preview" data-cover="${esc(t.id)}">${(t.preview || []).length
    ? t.preview.map(p => `<img loading="lazy" alt="" src="/api/theme/file?${q({ id: t.id, path: p })}&v=${stamp}">`).join("")
    : `<span class="none">${t.music ? "Music only" : t.files ? "No preview picture" : "Nothing added yet"}</span>`}</div>`;
}

function renderLibrary() {
  const s = state.status;
  /* An apply under way shows in the banner here; its message (shown on the other pages) would cover the banner.
     New theme shows the form alone, with the way back, and the apply's message. */
  document.body.classList.toggle("apply-shown", !!s.applying && !state.creating);
  if (state.creating) {
    document.title = "New theme · PS5 Themes";
    view.innerHTML = `
    <div class="head"><div class="edit-title"><a class="back" href="#/library" data-act="cancel-new">Library</a><h1 id="create-title">New theme</h1></div></div>
    ${createPanel()}`;
    state.libKey = "";
    if (!state.creating.focused) { state.creating.focused = true; $("#new-name").focus(); }
    paintLoads(view);
    return;
  }
  const cards = s.themes.slice().sort((a, b) => a.name.localeCompare(b.name)).map(t => {
    const on = s.applied === t.id, busy = s.applying === t.id;
    /* The firmware only when it says something: a theme for this console's needs no label */
    const meta = [];
    if (t.fw === "any") meta.push("Any firmware");
    if (t.files) meta.push(plural(t.files, "file"));
    else if (!t.music) meta.push("Empty");
    if (t.music) meta.push(`music ${fmtTime(t.music)}`);
    if (t.unsaved) meta.push("unsaved changes");
    const blocked = applyBlocked(t), has = !!(t.files || t.music);
    const keep = on || s.active === t.id;
    return `
    <article class="card${on ? " current" : ""}">
      <a class="card-open" href="#/edit/${encodeURIComponent(t.id)}" tabindex="-1" aria-hidden="true">${cardPreview(t)}</a>
      <div class="body">
        <div class="title"><span class="name">${esc(t.name)}</span>
          ${busy ? `<span class="tag">Applying…</span>` : on ? `<span class="tag on">Applied</span>` : t.compatible ? "" : `<span class="tag warn">Other firmware</span>`}</div>
        <div class="meta">${esc(meta.join(" · "))}</div>
        ${blocked && !on ? `<div class="why">${esc(blocked)}</div>` : ""}
        <div class="actions">
          <button class="btn primary sm" data-act="apply" data-id="${esc(t.id)}" ${blocked ? `disabled title="${esc(blocked)}"` : ""}>${applyLabel(t, on)}</button>
          <a class="btn sm" href="#/edit/${encodeURIComponent(t.id)}">Edit</a>
        </div>
        <div class="actions secondary">
          <button class="btn ghost sm" data-act="duplicate" data-id="${esc(t.id)}">Duplicate</button>
          <button class="btn ghost sm" data-act="export" data-id="${esc(t.id)}" ${has ? "" : `disabled title="Add something to the theme first."`}>Export</button>
          <button class="btn ghost danger sm" data-act="delete" data-id="${esc(t.id)}" ${keep ? `disabled title="It is applied: apply another theme, or No theme, first."` : ""}>Delete</button>
        </div>
      </div>
    </article>`;
  }).join("");

  const stockOn = !s.applied && !s.applying;
  view.innerHTML = `
    <div class="head">
      <h1>Library</h1><span class="spacer"></span>
      <button class="btn" data-act="import" title="A theme .zip, made with Export or shared by someone">Import</button>
      <button class="btn primary" data-act="new">New theme</button>
    </div>
    <div id="lib-banner">${resultBanner()}</div>
    ${s.themes.length ? `<div class="grid">
      <article class="card${stockOn ? " current" : ""}">
        <div class="preview"><img class="logo" src="/icon.png" alt=""></div>
        <div class="body">
          <div class="title"><span class="name">No theme</span>${stockOn ? `<span class="tag on">Applied</span>` : ""}</div>
          <div class="meta">The console's own look, also after a restart.</div>
          ${stockOn ? "" : `<div class="actions"><button class="btn primary sm" data-act="apply" data-id="none">Apply</button></div>`}
        </div>
      </article>${cards}</div>`
    : `<div class="empty">
        <h2>No themes yet</h2>
        <p class="muted" style="margin:0">A theme changes the background, icons, sounds, fonts and music. Make one with New theme, or import one someone shared.</p>
      </div>`}
    ${isLocal() && s.ip ? `<p class="hint">Pictures and music are easier to add from a phone or computer: open <code>http://${esc(s.ip)}:8095</code> there.</p>` : ""}`;
  state.libKey = libKey(s);
  paintLoads(view);
  fillMissingPreviews();
  fillHomePictures();
}

/* What the library's cards show: they are drawn again only when this changes. */
function libKey(s) {
  return JSON.stringify([s.themes, s.applied, s.applying, s.active]);
}
/* The library's banner brought up to date where it is: the apply's text and percentage changed, and the bar moves
   to the new width (its transition runs). Redrawn whole only when it becomes another kind of banner. False when the
   library isn't the page shown. */
function patchLibraryBanner() {
  const box = $("#lib-banner");
  if (!box) return false;
  const s = state.status, pr = s.applying && s.progress, bar = box.querySelector(".progress i");
  if (pr && bar) {
    const label = box.querySelector(".label"), pct = box.querySelector(".pct");
    const text = `Applying ${(themeById(s.applying) || { name: s.applying }).name}: ${pr.text}…`;
    if (label && label.textContent !== text) label.textContent = text;
    if (pct) pct.textContent = `${pr.percent}%`;
    else { setHtml(box, resultBanner()); return true; }
    bar.style.width = `${pr.percent}%`;
    return true;
  }
  setHtml(box, resultBanner());
  return true;
}
/* Sets an element's HTML only when it says something else: an unchanged one keeps its nodes (focus, hover, images).
   What it last set is compared with what the element holds now, so a change made some other way is never kept. */
const htmlSet = new WeakMap();
function setHtml(el, html) {
  if (!el) return;
  const was = htmlSet.get(el);
  if (was && was.html === html && el.innerHTML === was.out) return;
  el.innerHTML = html;
  htmlSet.set(el, { html, out: el.innerHTML });
}

/* New theme: a name, then how it starts (nothing yet, a copy of one of yours, or parts of several: Mix and match),
   each way shown as a picture rather than named in a list. */
const GLYPH = {
  sound: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"><path d="M4 9.5h3.5L12 6v12l-4.5-3.5H4z"/><path d="M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11"/></svg>',
  music: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"><path d="M9 17.5V5.5l10-2v12"/><circle cx="6.5" cy="17.5" r="2.5"/><circle cx="16.5" cy="15.5" r="2.5"/></svg>',
  colour: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="8" cy="9" r="4"/><circle cx="16" cy="9" r="4"/><circle cx="12" cy="15.5" r="4"/></svg>',
  icons: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><rect x="3.5" y="3.5" width="7" height="7" rx="2"/><rect x="13.5" y="3.5" width="7" height="7" rx="2"/><rect x="3.5" y="13.5" width="7" height="7" rx="2"/><rect x="13.5" y="13.5" width="7" height="7" rx="2"/></svg>',
  shape: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="3.5" y="5.5" width="17" height="13" rx="5"/></svg>',
  file: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M6 3.5h8l4 4v13H6z"/><path d="M14 3.5v4h4"/></svg>',
};
const byName = (a, b) => a.name.localeCompare(b.name);
const themePics = t => t.cover ? [coverUrl(t)] : (t.preview || []).map(p => `/api/theme/file?${q({ id: t.id, path: p })}&v=${stamp}`);

function newTheme(mode = "empty") {
  state.creating = { mode, name: "", copyOf: "", anyFw: false };
  state.mix = null;
  if (mode === "mix") openMix();
  if (location.hash === "#/library/new") return renderLibrary();
  state.newPushed = true;   /* Cancel goes back to where it was opened from */
  location.hash = "#/library/new";
}
/* New theme closed without making one: back to the page it was opened from, or the library. */
function closeCreate() {
  state.creating = false;
  state.mix = null;
  if (state.newPushed) { state.newPushed = false; return history.back(); }
  history.replaceState(null, "", "#/library");
  render();
}
/* The theme just made opens in place of New theme: Back from it leads where New theme was opened from, not to the
   form again. */
function openCreated(id) {
  state.creating = false;
  state.mix = null;
  state.newPushed = false;
  location.replace(`#/edit/${encodeURIComponent(id)}`);
}

function createPanel() {
  const c = state.creating, fw = state.status.fw;
  const mine = state.status.themes.filter(t => t.compatible).sort(byName);
  const pics = mine.flatMap(t => themePics(t).slice(0, 1));
  const starts = [
    ["empty", "Empty", "Choose each part in the editor", `<div class="start-art plus">+</div>`, true],
    ["copy", "A copy of a theme", "Change what you like; yours stays as it is",
     `<div class="start-art">${pics[0] ? `<img alt="" src="${pics[0]}">` : GLYPH.file}</div>`, mine.length > 0],
    ["mix", "Parts of several themes", "A background from one, icons from another",
     `<div class="start-art slices">${pics.slice(0, 3).map(u => `<img alt="" src="${u}">`).join("") || GLYPH.icons}</div>`, mine.length > 1],
  ];
  const ready = c.mode === "empty" || (c.mode === "copy" && c.copyOf) || (c.mode === "mix" && state.mix && !state.mix.loading && mixChosen());
  const label = c.mode === "copy" ? "Make the copy" : c.mode === "mix" ? "Make the theme" : "Create";
  return `
  <form class="panel new-theme" id="create" aria-labelledby="create-title">
    <label class="field big"><span>Name</span><input type="text" id="new-name" maxlength="60" autocomplete="off" spellcheck="false"
      placeholder="${esc(c.mode === "copy" && c.copyOf ? `${(themeById(c.copyOf) || {}).name} copy` : c.mode === "mix" ? "My mix" : "My theme")}" value="${esc(c.name)}"></label>
    <div class="field"><span>Start with</span>
      <div class="opts starts" role="radiogroup" aria-label="Start with">${starts.map(([v, name, what, art, ok]) => `
        <button type="button" class="opt" role="radio" aria-checked="${c.mode === v}" data-act="new-start" data-v="${v}"${ok ? "" : ` aria-disabled="true"`}>
          ${art}<b>${esc(name)}</b><small>${esc(ok ? what : v === "mix" ? "Make or import two themes first." : "Make or import a theme first.")}</small></button>`).join("")}</div></div>
    ${c.mode === "copy" ? `<div class="field"><span>Which theme</span>
      <div class="picks" role="radiogroup" aria-label="Which theme">${mine.map(t => `
        <button type="button" class="opt" role="radio" aria-checked="${c.copyOf === t.id}" data-act="new-copy" data-v="${esc(t.id)}">
          <div class="pick-art">${themePics(t)[0] ? `<img loading="lazy" alt="" src="${themePics(t)[0]}">` : GLYPH.file}</div><b>${esc(t.name)}</b>
          <small>${esc(t.files ? plural(t.files, "file") : t.music ? "Music only" : "Empty")}</small></button>`).join("")}</div></div>` : ""}
    ${c.mode === "mix" ? mixSection() : ""}
    <div class="create-foot">
      ${c.mode === "empty" ? `<button type="button" class="choice" role="switch" aria-checked="${c.anyFw}" data-act="new-anyfw">
        <span><b>Can be applied on other firmware</b><small>Only if the files it changes are the same there. Made for ${esc(fw)}.</small></span><i class="switch"></i></button>` : ""}
      ${ready ? "" : `<span class="note">${c.mode === "copy" ? "Choose the theme to copy." : state.mix && state.mix.loading ? "" : "Choose a part from one of your themes."}</span>`}
      <span class="spacer"></span>
      <button class="btn ghost" type="button" data-act="cancel-new">Cancel</button>
      <button class="btn primary" type="submit"${ready ? "" : " disabled"}>${label}</button>
    </div>
  </form>`;
}

/* The panel drawn again where it is, keeping the focus on the control that was pressed. */
function redrawCreate() {
  const f = $("#create");
  if (!f) return renderLibrary();
  const a = document.activeElement, key = a && a.dataset && a.dataset.act ? [a.dataset.act, a.dataset.v, a.dataset.part, a.dataset.key] : null;
  f.outerHTML = createPanel();
  paintLoads(view);
  if (key) {
    const sel = `[data-act="${key[0]}"]` + ["v", "part", "key"].map((k, i) => key[i + 1] !== undefined ? `[data-${k}="${CSS.escape(key[i + 1])}"]` : "").join("");
    const again = document.querySelector(sel);
    if (again) again.focus({ preventScroll: true });
  }
}

async function submitCreate() {
  const c = state.creating, typed = ($("#new-name").value || "").trim();
  try {
    if (c.mode === "mix") return createMix(typed);
    if (c.mode === "copy") {
      const t = themeById(c.copyOf);
      if (!t) return;
      const name = uniqueName(typed || `${t.name} copy`), id = uniqueId(name);
      await api("POST", "/api/themes/duplicate", { id: t.id, new_id: id, name });
      await refreshStatus();
      toast(`${name} is a copy of ${t.name}.`, "ok");
      return openCreated(id);
    }
    openCreated(await createTheme(uniqueName(typed || "My theme"), c.anyFw ? "any" : state.status.fw));
  } catch (err) { toast(err.message, "err"); }
}

function slug(name) { return name.toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "theme"; }
function uniqueId(name) { const base = slug(name); let id = base, n = 2; while (themeById(id)) id = `${base}-${n++}`; return id; }
function uniqueName(name) {
  const taken = new Set(state.status.themes.map(t => t.name));
  let out = name, n = 2;
  while (taken.has(out)) out = `${name} (${n++})`;
  return out;
}

async function createTheme(name, fw) {
  const id = uniqueId(name);
  await api("POST", "/api/themes", { id, name, fw });
  await refreshStatus();
  return id;
}

/* Seen on 13.42: the home screen reloads its files after an apply, and can
   show the stock look for a moment before the theme. */
const APPLY_WAIT_NOTE = "The home screen can show the console's own look for a few seconds first.";

/* Why a theme can't be applied, in words, or "" when it can. */
function applyBlocked(t, inEditor = state.route === "edit") {
  if (!t.compatible) return inEditor ? `Made for firmware ${t.fw}. To try it on ${state.status.fw}, set Firmware to Any firmware in Name and picture.`
    : `Made for firmware ${t.fw}. To try it on ${state.status.fw}, open Edit and set Firmware to Any firmware.`;
  if (!(t.files || t.music)) return inEditor ? "Add something to the theme first." : "Nothing in it yet: open Edit to add something.";
  return "";
}

/* `styled`: the titles this page styled by itself, which the theme is applied again for: the console says so once,
   when it is done, by their names. */
async function applyTheme(id, styled = null, first = "") {
  const params = styled && styled.length ? { id, styled: styled.slice(0, 32).join(",") } : { id };
  try { await api("POST", "/api/apply", params); } catch (e) { return toast(first ? `${first} ${e.message}` : e.message, "err"); }
  return followApply(id, first);
}
/* An apply under way, until the console says how it went. `first`: what started it, said before the rest ("Saved."
   or "Changes discarded."): the plugin applies the chosen theme again so the console shows the theme as saved. */
async function followApply(id, first = "") {
  const name = (themeById(id) || { name: id }).name, pre = first ? `${first} ` : "";
  const label = id === "none" ? "Removing the theme" : first ? `${pre}Applying ${name} again` : `Applying ${name}`;
  const p = progressToast(`${label}…`);
  p.el.dataset.apply = "1";   /* on the library, its banner shows the same progress: the message steps aside there */
  const want = id === "none" ? "" : id;
  const before = state.status.result;
  let seen = false, lastKey = "", lastChange = Date.now();
  /* As long as the console reports progress: a large theme can take minutes. It gives up only after 3 minutes
     without any change (the console out of reach, or the plugin stopped). */
  for (let i = 0; Date.now() - lastChange < 180000; i++) {
    await sleep(750);
    if (!await refreshStatus()) continue;
    const s = state.status;
    const key = JSON.stringify([s.applying, s.progress, s.applied, s.result]);
    if (key !== lastKey) { lastKey = key; lastChange = Date.now(); }
    if (s.applying) {
      seen = seen || s.applying === want;
      const pr = s.applying === want ? s.progress : null;
      p.set(pr ? `${label}\n${pr.text}… ${pr.percent}%` : `${label}…`, pr ? pr.percent / 100 : 0);
      continue;
    }
    const settled = s.applied === want && (seen || i > 1 || want === "");
    const failed = want && s.applied !== want && (seen || (s.result !== before && i > 1));
    if (settled || failed) {
      const clean = settled && !/partly/.test(s.result), parts = clean && want && applyResultParts(s.result);
      p.done(pre + (parts ? `${parts.said}\n${APPLY_WAIT_NOTE}` : s.result || (settled ? "Done." : "Not applied.")), clean ? "ok" : settled ? "" : "err");
      return render();
    }
  }
  p.done(`${pre}The console stopped answering while applying. Check the log in Settings, or open this page again in a minute.`, "err");
  render();
}

/* A sound file the browser can play (WAV): Play, and Stop while it plays; one at a time. */
let wavPlaying = null;
function playWav(btn) {
  const was = wavPlaying;
  if (was) {
    was.audio.pause();
    was.btn.textContent = "Play";
    wavPlaying = null;
    if (was.btn === btn) return;
  }
  const audio = new Audio(btn.dataset.src);
  wavPlaying = { audio, btn };
  btn.textContent = "Stop";
  const end = () => { if (wavPlaying && wavPlaying.audio === audio) { btn.textContent = "Play"; wavPlaying = null; } };
  audio.addEventListener("ended", end);
  audio.play().catch(e => { end(); toast(`Could not play it: ${e.message}`, "err"); });
}

const armed = new Map();
function armDelete(btn, id) {
  if (armed.get(id)) {
    armed.delete(id);
    return api("DELETE", "/api/themes", { id })
      .then(() => { toast("Theme deleted.", "ok"); localSet(`aspects:${id}`, null); return refreshStatus(); })
      .then(render)
      .catch(e => toast(e.message, "err"));
  }
  armed.set(id, true);
  btn.classList.add("armed");
  btn.textContent = "Press again to delete";
  setTimeout(() => { armed.delete(id); if (btn.isConnected) { btn.classList.remove("armed"); btn.textContent = "Delete"; } }, 6000);
}

async function duplicateTheme(id) {
  const t = themeById(id);
  const name = uniqueName(`${t.name} copy`);
  try {
    await api("POST", "/api/themes/duplicate", { id, new_id: uniqueId(name), name });
    await refreshStatus();
    toast(`Created ${name}.`, "ok");
    libraryIfShown();
  } catch (e) { toast(e.message, "err"); }
}
/* After a wait: the library drawn again only if it is still the page shown (a theme opened meanwhile stays open). */
function libraryIfShown() {
  if (state.route === "library") renderLibrary();
}
