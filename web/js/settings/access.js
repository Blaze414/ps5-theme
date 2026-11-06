"use strict";

/* ================================================================ access */

/* Who can open the manager (the plugin's access.c): the devices you allow (a code from the console, once), any device
   on your network, or only the console. A device not allowed sees how to be, and nothing else. */
const ACCESS_MODES = [
  ["paired", "Devices you allow", "A phone or computer enters a code shown on the console, once."],
  ["network", "Any device on your network", "Anyone on your Wi-Fi or network can open it and change your themes."],
  ["console", "Only this console", "Phones and computers can't open it."],
];
let gated = "";

/* Before anything else: whether this device may open the manager. */
async function accessReady() {
  try {
    state.access = await api("GET", "/api/access");
  } catch (e) { return !gated; }   /* (the console didn't answer: the usual errors say so) */
  if (state.access.allowed) return true;
  showGate(state.access.mode === "console" ? "console" : "pair");
  return false;
}

/* Instead of the manager: how to open it here. "pair": a code is needed; "console": it opens only there. */
function showGate(kind) {
  if (kind !== "pair" && kind !== "console") return;
  if (gated === kind) return;
  gated = kind;
  stopPreview();
  if ($("#pv-overlay")) previewClose();
  bgeClose();
  document.body.classList.add("gated");
  document.title = "PS5 Themes";
  view.innerHTML = kind === "console" ? `
    <div class="gate"><h1>Opens on the console only</h1>
      <p class="note">This manager is set to open only on the PS5 itself. To open it here, open Themes on the console, then choose who can open it in Settings › Devices.</p></div>` : `
    <div class="gate"><h1>Allow this device</h1>
      <p class="note">This manager opens on the phones and computers you allow. The console shows a code to enter here, once.</p>
      <button class="btn primary" data-act="pair-start">Show a code on the console</button>
      <form id="pair" hidden>
        <label class="field"><span>Code shown on the console</span>
          <input type="text" id="pair-code" inputmode="numeric" autocomplete="one-time-code" maxlength="7" spellcheck="false"></label>
        <button class="btn primary" type="submit">Allow this device</button>
      </form>
      <p class="note" id="pair-msg" role="status" aria-live="polite"></p></div>`;
  const first = view.querySelector("button, input");
  if (first) first.focus();
}

const said = m => m ? m[0].toUpperCase() + m.slice(1).replace(/[^.]$/, c => c + ".") : "";

async function pairStart(btn) {
  const msg = $("#pair-msg");
  btn.disabled = true;
  try {
    await api("POST", "/api/pair/start");
    $("#pair").hidden = false;
    btn.textContent = "Show the code again";
    btn.classList.remove("primary");
    msg.classList.remove("err");
    msg.textContent = "The code is in a notification on the console's screen. It works for 2 minutes.";
    $("#pair-code").focus();
  } catch (e) { if (msg.isConnected) { msg.textContent = said(e.message); msg.classList.add("err"); } }
  finally { btn.disabled = false; }
}

async function pairFinish() {
  const code = $("#pair-code").value.replace(/\D/g, ""), msg = $("#pair-msg");
  msg.classList.toggle("err", code.length !== 6);
  if (code.length !== 6) { msg.textContent = "Enter the 6 digits the console shows."; return; }
  try {
    await api("POST", "/api/pair/finish", { code });
    msg.textContent = "Allowed. Opening the manager…";
    location.reload();
  } catch (e) {
    if (!msg.isConnected) return;
    msg.textContent = said(e.message);
    msg.classList.add("err");
    $("#pair-code").select();
  }
}

const dayOf = s => new Date(s * 1000).toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" });

async function loadAccess() {
  const box = $("#access");
  let a;
  try { a = state.access = await api("GET", "/api/access"); }
  catch (e) { if (box && box.isConnected) box.innerHTML = `<p class="note">${esc(said(e.message))}</p>`; return; }
  if (!box || !box.isConnected || !a.devices) return;
  box.innerHTML = `
    <div class="choices" role="radiogroup" aria-label="Who can open this manager">${ACCESS_MODES.map(([v, name, what]) =>
      `<button type="button" class="choice" role="radio" aria-checked="${a.mode === v}" data-act="access-mode" data-v="${v}">
        <span><b>${esc(name)}</b><small>${esc(v === "console" && a.here === "device" ? "Phones and computers can't open it, this one included." : what)}</small></span><i class="radio"></i></button>`).join("")}</div>
    <p class="note" id="access-confirm" role="status" aria-live="polite" style="margin-top:10px">The console itself can always open it.</p>
    <h3 class="pane-sub">Allowed devices</h3>${a.devices.length ? `<div class="rows">${a.devices
      .sort((x, y) => y.seen - x.seen).map(d => `
      <div class="row-item"><div class="info"><div class="file">${esc(d.name)}${d.thisOne ? ' <span class="tag on">This device</span>' : ""}</div>
        <div class="dir">${esc(d.addr)} · allowed ${esc(dayOf(d.added))} · last used ${esc(dayOf(d.seen))}</div></div>
        <div class="acts"><button class="btn sm danger" data-act="access-forget" data-v="${esc(d.id)}">Forget</button></div></div>`).join("")}</div>`
      : `<p class="note">None yet. To allow a phone or computer, open the manager on it: it shows the steps.</p>`}`;
}

/* A choice that would shut this device out is asked twice. */
async function accessMode(btn) {
  const a = state.access, v = btn.dataset.v;
  if (!a || a.mode === v) return;
  const self = a.devices && a.devices.some(d => d.thisOne);
  const shut = a.here === "device" && (v === "console" || (v === "paired" && !self));
  if (shut && !armed.get("access")) {
    armed.set("access", v);
    $("#access-confirm").textContent = v === "console"
      ? "This device won't be able to open the manager. Choose Only this console again to confirm."
      : "This device isn't one you allowed: it will need a code from the console. Choose Devices you allow again to confirm.";
    setTimeout(() => { if (armed.get("access") === v) { armed.delete("access"); const c = $("#access-confirm"); if (c) c.textContent = "The console itself can always open it."; } }, 8000);
    return;
  }
  if (shut && armed.get("access") !== v) { armed.delete("access"); return accessMode(btn); }
  armed.delete("access");
  try { await api("POST", "/api/access", { mode: v }); }
  catch (e) { toast(e.message, "err"); return; }
  if (shut) return showGate(v === "console" ? "console" : "pair");
  toast(v === "paired" ? "Only the devices you allow can open the manager." : v === "network"
    ? "Any device on your network can open the manager." : "Only the console can open the manager.", "ok");
  await loadAccess();
  const again = document.querySelector(`.choice[data-act="access-mode"][data-v="${v}"]`);
  if (again) again.focus();
}

async function accessForget(btn) {
  const id = btn.dataset.v, key = `forget:${id}`;
  const self = state.access && state.access.devices.some(d => d.id === id && d.thisOne) && state.access.mode === "paired";
  if (!armed.get(key)) {
    armed.set(key, true);
    btn.classList.add("armed");
    btn.textContent = self ? "Press again: this device" : "Press again to forget";
    setTimeout(() => { armed.delete(key); if (btn.isConnected) { btn.classList.remove("armed"); btn.textContent = "Forget"; } }, 6000);
    return;
  }
  armed.delete(key);
  try { await api("DELETE", "/api/access/devices", { id }); }
  catch (e) { toast(e.message, "err"); return; }
  if (self) return showGate("pair");
  toast("Forgotten: it needs a new code to open the manager.", "ok");
  return loadAccess();
}
