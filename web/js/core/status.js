"use strict";

/* ================================================================ status & routing */

async function refreshStatus() {
  try {
    state.status = await api("GET", "/api/status", {}, undefined, { timeout: 10000 });
  } catch (e) {
    $("#pills").textContent = "The console isn't answering";
    return false;
  }
  const s = state.status;
  /* The plugin was updated while this page stayed open: load the new page,
     so it never runs code older than the plugin. */
  if (!state.pageVersion) state.pageVersion = s.version;
  else if (s.version !== state.pageVersion && !state.reloading) {
    state.reloading = true;
    toast(`The plugin is now ${s.version}. ${pageBusy() ? "Reload this page when the current task is done." : "Loading the new page…"}`, "", 0);
    if (!pageBusy()) setTimeout(() => location.reload(), 1500);
  }
  const nameOf = id => (themeById(id) || { name: id }).name;
  const pills = s.applying ? `Applying <b>${esc(nameOf(s.applying))}…</b>`
    : s.applied ? `<b>${esc(nameOf(s.applied))}</b> applied` : "No theme applied";
  setHtml($("#pills"), pills);
  if (state.route === "edit") syncEditorHead();
  return true;
}

/* The edit page's Apply and Export follow the theme as files are added or
   removed (the rest of the page redraws one section at a time). */
function syncEditorHead() {
  const t = themeById(state.editId), apply = $("#edit-apply"), exp = $("#edit-export");
  if (!t || !apply || apply.dataset.id !== t.id) return;
  const has = !!(t.files || t.music);
  apply.disabled = !(t.compatible && has);
  apply.textContent = applyLabel(t, state.status.applied === t.id);
  apply.title = applyBlocked(t);
  if (exp) { exp.disabled = !has; exp.title = has ? "" : "Add something to the theme first."; }
  const bar = $("#savebar");
  if (bar) bar.hidden = !t.unsaved;
}

/* ---- Following the console ----

   A game or app installed or removed, and what the console's icon styler did by itself, show here without reloading
   the page: the status says when either changed (`titles`), and the page reads the console's titles and the theme
   again, then says what changed. Never while something else is under way here (a change being sent, an upload, a
   picture being placed, a form open): it waits for that to finish. */
const titleName = t => (state.titleNames && state.titleNames.get(t)) || (state.tileNames && state.tileNames.get(t)) || STYLE_TILES[t] || t;
/* "Itemzflow", "Itemzflow and Astro Bot", "Itemzflow, Astro Bot and Bloodborne", "Itemzflow, Astro Bot and 3 more" */
function namesOf(titles) {
  const n = titles.length, shown = n > 3 ? 2 : n, names = titles.slice(0, shown).map(titleName);
  if (n > shown) return `${names.join(", ")} and ${n - shown} more`;
  return n > 1 ? `${names.slice(0, -1).join(", ")} and ${names[n - 1]}` : names[0] || "";
}
function pageBusy() {
  const a = document.activeElement;
  return writing > 0 || styling || !!following || !!autoRun || progressStack.length > 0 || !!state.trim || !!state.creating || !!state.mix ||
    !!document.querySelector(".pv-overlay") ||
    !!(a && (/^(SELECT|TEXTAREA)$/.test(a.tagName) || (a.tagName === "INPUT" && /^(text|search|number|email|url|tel|password)$/.test(a.type))));   /* typing */
}
let following = null, followTimer = 0;   /* (the read under way) */
/* Read the console's titles and the theme again if they changed (`force`: whether or not), then say what changed
   and, on the theme's page, style what is new and installed (`act`). `now`: for an action of yours, which waits
   for a read under way and goes ahead whatever else is happening. */
async function followConsole({ force = false, act = true, now = false } = {}) {
  const s = state.status;
  if (following) { if (now) await following.catch(() => {}); return; }
  if (!s || !state.assets) return;      /* nothing read yet: the first read is as the console is */
  const changed = s.titles && s.titles !== state.assetsSeen, due = force || state.followDue || changed;
  if (!due || (!now && pageBusy())) return;           /* (looked at again at the next poll) */
  const settledOnly = state.followDue === "settled" && !changed && !force;   /* only a wait ending */
  let finish;
  following = new Promise(r => { finish = r; });
  state.followDue = false;
  try {
    const id = state.route === "edit" ? state.editId : null;
    const was = new Set(titleGroups().map(g => g.title));
    const madeWas = new Set(styleMadeByPlugin.id === id ? styleMadeByPlugin.titles : []);
    await loadAssets(true);
    if (id) await Promise.all([loadThemeFiles(id), loadPluginStyled(id)]);
    const here = state.route === "edit" && state.editId === id;
    const after = titleGroups().map(g => g.title), added = after.filter(t => !was.has(t)), gone = [...was].filter(t => !after.includes(t));
    const byConsole = here ? styleMadeByPlugin.titles.filter(t => !madeWas.has(t)) : [];
    if (added.length) { titleNamesEnsure(); if (titleNamesLoading) await titleNamesLoading.catch(() => {}); }
    const said = [], busy = installingNow(), installing = added.filter(t => t in busy), done = added.filter(t => !(t in busy));
    if (done.length) said.push(`Installed on the console: ${namesOf(done)}.`);
    if (installing.length) said.push(`Installing on the console: ${namesOf(installing)}.`);
    if (gone.length) said.push(`No longer on the console: ${namesOf(gone)}.`);
    if (byConsole.length) said.push(`Styled by the console: ${namesOf(byConsole)}.`);
    if (said.length) toast(said.join(" "), "", 8000);
    if (here) { syncEditorHead(); renderAspect(aspectById("titles")); }
    /* A title's install just counted as finished, on the theme the console has applied, whose styler does the same
       within a minute: left to it (both at once would apply the theme twice); this page follows when it has. */
    if (settledOnly && state.status.applied === id && styleMadeByPlugin.auto && styleMadeByPlugin.template) act = false;
  } finally { following = null; finish(); }
  if (act && state.route === "edit") await autoStyleIcons();
}
/* Before the icons are styled or the style removed: the console's titles and the style's record as they are now. */
async function catchUp() {
  if (await refreshStatus()) await followConsole({ act: false, now: true });
}
async function pollConsole(tick) {
  if (document.hidden) return;
  const lib = state.route === "library" && !state.creating && !state.mix;   /* a form open keeps its focus */
  if (!(lib && state.status && state.status.applying) && tick % 4) return;   /* every second while a theme is being applied */
  if (!lib && pageBusy()) return;   /* the page's own work says what is happening meanwhile */
  const before = JSON.stringify(state.status);
  if (!await refreshStatus()) return;
  /* Looked at again after the wait: a theme opened, or a form, meanwhile is never drawn over by the library. */
  const stillLib = state.route === "library" && !state.creating && !state.mix;
  if (lib && stillLib && JSON.stringify(state.status) !== before) {
    /* Only what changed is redrawn: the cards (and their pictures) when the themes or what is applied change; the
       apply's progress in place, so the bar moves smoothly and nothing under the pointer or the focus is rebuilt. */
    if (libKey(state.status) !== state.libKey || !patchLibraryBanner()) renderLibrary();
  }
  await followConsole();
}

/* What Save does: a choice still on its way to the console goes first (saved before it, it would show as unsaved
   again), then the copy kept for Discard is let go. The plugin applies the chosen theme again if the console shows
   other files: `reapplied`. */
async function saveWork(id, name, p) {
  p.wait("Keeping your last choice on the console…");
  await choicesWork;
  p.wait(`Saving ${name}: letting go of the copy kept for Discard…`);
  return api("POST", "/api/theme/save", { id });
}

/* Apply, with unsaved changes: they are saved first, so what the console shows is always a saved theme (never
   changes Discard would take back). */
async function applyPressed(id) {
  const t = themeById(id);
  if (!t || !t.unsaved) return applyTheme(id);
  const p = laterProgress(`Saving ${t.name}…`);
  let r;
  try { r = await saveWork(id, t.name, p); }
  catch (e) { return p.done(`Not applied: the changes could not be saved. ${e.message}`, "err"); }
  p.done();
  await refreshStatus();
  if (r && r.reapplied) return followApply(id, "Saved.");
  return applyTheme(id, null, "Saved.");
}

/* The Apply button's name: Save and apply while there are unsaved changes, Apply again for the theme applied. */
const applyLabel = (t, on) => t.unsaved ? "Save and apply" : on ? "Apply again" : "Apply";

async function saveTheme(id, btn = null) {
  const t = themeById(id), name = t ? t.name : id;
  if (btn) { btn.disabled = true; btn.textContent = "Saving…"; }
  const p = laterProgress(`Saving ${name}…`);
  let r;
  try {
    r = await saveWork(id, name, p);
  } catch (e) {
    if (btn && btn.isConnected) { btn.disabled = false; btn.textContent = "Save"; }
    return p.done(e.message, "err");
  }
  p.wait("Checking the console…");
  await refreshStatus();
  /* The theme on the console was applied again to show what was saved: one message for both */
  if (r && r.empty) return p.done("Saved. It's empty now, so the console keeps showing it as it was until you choose No theme.", "warn");
  if (!r || !r.reapplied) return p.done("Saved.", "ok");
  p.done();
  await followApply(id, "Saved.");
}

let discardArmed = 0;
async function discardTheme(id, btn) {
  if (Date.now() > discardArmed) {   /* first press asks */
    discardArmed = Date.now() + 6000;
    btn.textContent = "Press again to discard";
    btn.classList.add("danger", "armed");
    setTimeout(() => { if (btn.isConnected && Date.now() > discardArmed) { btn.textContent = "Discard"; btn.classList.remove("danger", "armed"); } }, 6100);
    return;
  }
  discardArmed = 0;
  btn.disabled = true;
  btn.textContent = "Discarding…";
  const p = laterProgress("Discarding your changes…");
  let r;
  try {
    p.wait("Keeping your last choice on the console…");
    await choicesWork;
    p.wait("Putting the theme back as it was at the last save…");
    r = await api("POST", "/api/theme/discard", { id });
  } catch (e) {
    if (btn.isConnected) { btn.disabled = false; btn.textContent = "Discard"; btn.classList.remove("danger", "armed"); }
    return p.done(e.message, "err");
  }
  songCacheDrop(id);        /* the songs may be the saved ones again */
  stamp = Date.now();
  p.wait("Checking the console…");
  await refreshStatus();
  if (!r.reapplied) p.done("Changes discarded.", "ok");
  else p.done();
  if (state.route === "edit" && state.editId === id) renderEditor();
  if (r.reapplied) await followApply(id, "Changes discarded.");
}

function resultBanner() {
  const s = state.status;
  if (s.applying) {
    const pr = s.progress;
    return `<div class="banner" role="status"><div class="row"><span class="label">Applying ${esc((themeById(s.applying) || { name: s.applying }).name)}` +
      `${pr ? `: ${esc(pr.text)}…` : "…"}</span>${pr ? `<span class="pct">${pr.percent}%</span>` : ""}</div>` +
      `<div class="progress" style="margin-top:10px"><i style="width:${pr ? pr.percent : 0}%"></i></div>` +
      `<span class="muted" style="font-size:.875rem">The home screen can show the console's own look for a moment while it switches.</span></div>`;
  }
  const r = s.result;
  if (!r || (!s.applied && /^No theme:/.test(r))) return "";   /* the No theme card says it */
  const kind = /could not|not applied|is for firmware|Invalid|Could not/.test(r) ? "err"
    : /partly|skipped/.test(r) ? "warn" : "ok";
  /* Applied: its message said so; what is left to do to see all of it stays here */
  const parts = kind === "ok" && applyResultParts(r);
  if (parts) return parts.rest ? `<div class="banner" role="status">${esc(parts.rest)}</div>` : "";
  return `<div class="banner ${kind}" role="status">${esc(r)}</div>`;
}
/* The console's result of an apply that went through, in two: what happened ("Night applied."), and what to do to see
   all of it (rest mode for a new font...). How many files were checked is for the log. */
function applyResultParts(r) {
  const m = /^(.*) applied: \d+ files? checked on the console\.\s*([\s\S]*)$/.exec(r || "");
  return m ? { said: `${m[1]} applied.`, rest: m[2].trim() } : null;
}

function parseRoute() {
  const parts = location.hash.replace(/^#\/?/, "").split("/");
  /* (Activity and Access were pages of their own until 1.81: their old addresses still lead there) */
  const old = { activity: "log", access: "devices" }[parts[0]];
  state.route = old ? "settings" : ["library", "edit", "settings"].includes(parts[0]) ? parts[0] : "library";
  state.settingsPart = old || (state.route === "settings" && SETTINGS.some(x => x[0] === parts[1]) ? parts[1] : "");
  if (state.route === "edit") state.editId = decodeURIComponent(parts[1] || "") || state.editId;
  /* New theme is a screen of its own (#/library/new): opened at that address, closed when the page goes anywhere else. */
  const creatingHere = state.route === "library" && parts[1] === "new";
  if (creatingHere && !state.creating) state.creating = { mode: "empty", name: "", copyOf: "", anyFw: false };
  if (!creatingHere && (state.creating || state.mix)) { state.creating = false; state.mix = null; state.newPushed = false; }
  const part = state.route === "edit" ? parts[2] || "" : "";
  state.section = part === "theme" || aspectById(part) ? part : "";
  const gear = $(".gear");
  if (gear) state.route === "settings" ? gear.setAttribute("aria-current", "page") : gear.removeAttribute("aria-current");
}

async function render() {
  parseRoute();
  if (gated) return;
  if (state.route !== "edit") document.title = state.route === "settings" ? "Settings · PS5 Themes" : "PS5 Themes";
  if (state.route !== "library") document.body.classList.remove("apply-shown");
  if (state.route === "library") renderLibrary();
  else if (state.route === "edit") await renderEditor();
  else await renderSettings();
}
