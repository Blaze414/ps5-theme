"use strict";

/* ================================================================ activity */

/* ================================================================ settings */

/* Settings: what is set once, or looked at when something goes wrong, out of the way of making themes, behind the
   gear where the console keeps its own. */
const SET_ICON = {
  notifications: '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round" stroke-linecap="round"><path d="M6 10.2a6 6 0 0 1 12 0v3.6l1.7 2.9H4.3L6 13.8z"/><path d="M9.8 19.6a2.3 2.3 0 0 0 4.4 0"/></svg>',
  storage: '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"><rect x="3" y="4.5" width="18" height="6.5" rx="1.6"/><rect x="3" y="13" width="18" height="6.5" rx="1.6"/><circle cx="7" cy="7.75" r="1" fill="currentColor" stroke="none"/><circle cx="7" cy="16.25" r="1" fill="currentColor" stroke="none"/></svg>',
  devices: '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round" stroke-linecap="round"><rect x="2" y="4.5" width="14" height="10" rx="1.6"/><path d="M5 18.5h8"/><rect x="17.5" y="8" width="5" height="11.5" rx="1.4"/></svg>',
  log: '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round" stroke-linecap="round"><rect x="4" y="2.5" width="16" height="19" rx="2"/><path d="M8 8h8M8 12h8M8 16h5"/></svg>',
  about: '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><circle cx="12" cy="12" r="9.2"/><path d="M12 11v6"/><circle cx="12" cy="7.6" r="1.1" fill="currentColor" stroke="none"/></svg>',
};
const SETTINGS = [
  ["notifications", "Notifications", "What the console says, and when"],
  ["devices", "Devices", "Who can open this manager"],
  ["storage", "Storage", "What the plugin keeps on the console"],
  ["log", "Log", "What the plugin did"],
  ["about", "About", "Version, firmware and address"],
];
const NOTICES = [
  ["starts", "When PS5 Themes starts, stops or updates", "\u201cPS5 Themes is ready\u201d, with the address to open on a phone or computer."],
  ["tips", "Tips after applying", "On the \u201cTheme applied\u201d notification: rest mode for new icons and fonts, when the music starts."],
  ["progress", "Progress while applying", "\u201cApplying\u2026\u201d, and how far a long apply has got, every few seconds."],
];

async function renderSettings() {
  const part = state.settingsPart, shown = part || SETTINGS[0][0];
  const [, name] = SETTINGS.find(x => x[0] === shown);
  view.innerHTML = `
    <div class="settings${part ? " has-part" : ""}">
      <div class="settings-side"><h1>Settings</h1>
        <nav class="settings-list" aria-label="Settings">${SETTINGS.map(([id, label, what]) => `
          <a href="#/settings/${id}"${id === shown ? ' aria-current="page"' : ""}>${SET_ICON[id]}<span><b>${label}</b><small>${what}</small></span></a>`).join("")}
        </nav></div>
      <section class="settings-pane" aria-labelledby="pane-title">
        <a class="back" href="#/settings">Settings</a>
        <div class="pane-head"><h2 id="pane-title">${name}</h2><span class="spacer"></span>${shown === "log" ? '<button class="btn sm ghost" data-act="refresh-log">Refresh</button><button class="btn sm" data-act="copy-log">Copy</button>' : ""}</div>
        <div id="pane-body"></div></section>
    </div>`;
  const body = $("#pane-body");
  if (shown === "notifications") {
    body.innerHTML = `<p class="note">Loading…</p>`;
    await loadNotices(body);
  } else if (shown === "storage") {
    body.innerHTML = loadingMarker("storage", "Counting what the plugin keeps…", "It reads its folders on the console: a few seconds.");
    await loadStorage(body);
  } else if (shown === "devices") {
    body.innerHTML = `<div id="access"><p class="note">Loading…</p></div>`;
    await loadAccess();
  } else if (shown === "log") {
    body.innerHTML = `<p class="note" style="margin-bottom:16px">Newest at the bottom. Copy it into your report when you ask for help with a theme.</p>
      <div class="activity"><div id="crashes"></div><pre class="log" id="log">Loading…</pre></div>`;
    await loadLog();
  } else {
    if (!state.access) state.access = await api("GET", "/api/access").catch(() => null);
    if (body.isConnected) renderAbout(body);
  }
}

/* What the console says: each kind of notification on or off. A failure is always said. */
async function loadNotices(body) {
  let on;
  try { on = await api("GET", "/api/settings"); }
  catch (e) { if (body.isConnected) body.innerHTML = `<p class="note">${esc(said(e.message))}</p>`; return; }
  if (!body.isConnected) return;
  body.innerHTML = `<div class="choices">${NOTICES.map(([id, name, what]) => `
    <button type="button" class="choice" role="switch" aria-checked="${!!on[id]}" data-act="pref" data-v="${id}">
      <span><b>${esc(name)}</b><small>${esc(what)}</small></span><i class="switch"></i></button>`).join("")}</div>
    <p class="note" style="margin-top:12px">You're always told when a theme can't be applied or is taken down.</p>`;
}

async function setPref(btn) {
  const on = btn.getAttribute("aria-checked") !== "true";
  btn.setAttribute("aria-checked", String(on));   /* shown at once; put back if the console says no */
  try { await api("POST", "/api/settings", { name: btn.dataset.v, on: on ? 1 : 0 }); }
  catch (e) { btn.setAttribute("aria-checked", String(!on)); toast(e.message, "err"); }
}

const sizeOf = b => b >= 1e9 ? `${(b / 1e9).toFixed(b >= 1e10 ? 0 : 1)} GB` : b >= 1e6 ? `${Math.round(b / 1e6)} MB` : b > 0 ? "Under 1 MB" : "None";

/* What the plugin keeps, part by part, and the copies left from another firmware, which can go. */
async function loadStorage(body) {
  let st;
  try { st = await api("GET", "/api/storage"); }
  catch (e) { if (body.isConnected) body.innerHTML = `<p class="note">${esc(said(e.message))}</p>`; return; }
  if (!body.isConnected) return;
  const parts = [
    ["themes", "Your themes", "Their pictures, sounds and music, and what Discard goes back to", st.themes],
    ["copies", `Copies of the console's folders (${st.firmware})`, "What a theme is put together from, never the console's own files", st.copies],
    ["ready", "Ready to show", "The applied theme, and the one before it, so switching back is quick", st.ready],
    ["logs", "Log and diagnostics", "", st.logs],
  ];
  if (st.other) parts.push(["other", `Copies for other firmware (${st.otherFirmware.join(", ")})`, "Left from before a firmware update; not used again", st.other]);
  const used = parts.reduce((n, p) => n + p[3], 0), total = used + Math.max(st.room, 0);
  body.innerHTML = `
    <div class="usage" role="img" aria-label="${esc(`${sizeOf(used)} used by PS5 Themes, ${sizeOf(Math.max(st.room, 0))} left`)}">${parts
      .filter(p => p[3] > 0).map(p => `<i class="usage-${p[0]}" style="width:${(100 * p[3] / total).toFixed(2)}%"></i>`).join("")}</div>
    <dl class="facts">${parts.map(([id, name, what, b]) => `
      <div><dt><span class="key usage-${id}"></span>${esc(name)}${what ? `<small>${esc(what)}</small>` : ""}</dt>
        <dd>${esc(sizeOf(b))}${id === "other" ? `<button class="btn sm danger" data-act="storage-clean">Remove</button>` : ""}</dd></div>`).join("")}
      <div><dt>Room left for themes<small>The plugin always keeps 1 GB of the console's storage free</small></dt><dd>${esc(st.room < 0 ? "Unknown" : sizeOf(st.room))}</dd></div>
    </dl>`;
}

async function storageClean(btn) {
  if (!armed.get("storage")) {
    armed.set("storage", true);
    btn.classList.add("armed");
    btn.textContent = "Press again to remove";
    setTimeout(() => { armed.delete("storage"); if (btn.isConnected) { btn.classList.remove("armed"); btn.textContent = "Remove"; } }, 6000);
    return;
  }
  armed.delete("storage");
  btn.disabled = true;
  btn.textContent = "Removing…";
  try {
    const r = await api("POST", "/api/storage/clean", { what: "firmware" });
    toast(r.kept ? `Removed ${sizeOf(r.freed)}. Some copies are in use and stay until the console restarts.` : `Removed ${sizeOf(r.freed)}.`, "ok");
  } catch (e) { toast(e.message, "err"); }
  const body = $("#pane-body");
  if (body) await loadStorage(body);
}

/* What a report about the console needs: the plugin's version, the firmware, where the manager is, the room left. */
function renderAbout(body) {
  const s = state.status, a = state.access;
  const rows = [
    ["PS5 Themes", s.version],
    ["Console firmware", s.fw],
    ["Manager", s.ip ? `http://${s.ip}:8095` : "On this console only"],
    ["Room left for themes", typeof s.room === "number" ? sizeOf(s.room) : "Unknown", "The plugin always keeps 1 GB of the console's storage free"],
    ["Applied theme", s.applied ? (themeById(s.applied) || { name: s.applied }).name : "No theme"],
    ["Who can open the manager", a ? (ACCESS_MODES.find(m => m[0] === a.mode) || [0, a.mode])[1] : "…"],
  ];
  body.innerHTML = `<dl class="facts">${rows.map(([k, v, small]) => `<div><dt>${esc(k)}${small ? `<small>${esc(small)}</small>` : ""}</dt><dd>${esc(v)}</dd></div>`).join("")}</dl>`;
}

/* The whole log to the clipboard. The manager is plain http on the console, where browsers keep the clipboard API
   from pages, so it copies the selected text the older way; failing that, the text stays selected to copy by hand. */
async function copyLog() {
  const pre = $("#log");
  if (!pre) return;
  try { if (navigator.clipboard && isSecureContext) { await navigator.clipboard.writeText(pre.textContent); return toast("Log copied."); } }
  catch { }
  const range = document.createRange(), sel = getSelection();
  range.selectNodeContents(pre); sel.removeAllRanges(); sel.addRange(range);
  let ok = false;
  try { ok = document.execCommand("copy"); } catch { }
  if (ok) { sel.removeAllRanges(); toast("Log copied."); }
  else toast("The log is selected: copy it with your device's copy command.");
}
async function loadLog() {
  const el = $("#log");
  try {
    const text = await api("GET", "/api/log");
    el.textContent = text || "Nothing logged yet.";
    el.scrollTop = el.scrollHeight;
  } catch (e) { el.textContent = e.message; }
  const box = $("#crashes");   /* (taken now: the log left meanwhile, the answer goes nowhere) */
  try {
    const { crashes } = await api("GET", "/api/crashes");
    if (!box) return;
    box.innerHTML = crashes.length ? `<div class="panel"><h2 style="margin-bottom:12px">Crash reports</h2>
      <div class="rows">${crashes.sort().reverse().map(c => {
        const when = new Date(parseInt(c.slice(6), 10) * 1000).toLocaleString();
        return `<div class="row-item"><div class="info"><div class="file">${esc(when)}</div><div class="dir">${esc(c)}</div></div>
          <div class="acts"><button class="btn sm" data-act="view-crash" data-name="${esc(c)}">View</button></div></div>`;
      }).join("")}</div></div>` : "";
  } catch (e) { if (box) box.innerHTML = ""; }
}
