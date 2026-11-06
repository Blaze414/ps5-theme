"use strict";

/* ================================================================ toasts */

/* A message at the bottom right. An error stays until it is closed: it may be read on the console with a controller,
   from across the room. Anything else fades after `ms` (0: stays). Every message but a progress bar closes with its ×;
   at most five stay, the oldest going first. */
function toast(msg, kind = "", ms = 4500) {
  const el = document.createElement("div");
  el.className = `toast ${kind}`;
  el.setAttribute("role", kind === "err" ? "alert" : "status");
  const text = document.createElement("div");
  text.className = "msg";
  /* the plugin's reasons start in lower case; a file name stays as it is */
  text.textContent = msg && /^[a-z]/.test(msg) && !/^\S*[._/]\S/.test(msg) ? msg[0].toUpperCase() + msg.slice(1) : msg;
  const x = document.createElement("button");
  x.type = "button";
  x.className = "x";
  x.setAttribute("aria-label", "Close");
  x.textContent = "×";
  x.addEventListener("click", () => leave(el));
  el.append(text, x);
  const box = $("#toasts");
  box.append(el);
  const closable = [...box.children].filter(t => !t.classList.contains("leaving") && !t.querySelector(".progress"));
  for (const old of closable.slice(0, Math.max(0, closable.length - 5))) leave(old);
  if (ms && kind !== "err") setTimeout(() => leave(el), ms);
  return el;
}
/* A message fades out, then is removed (removed at once if the animation can't run). */
function leave(el) {
  if (!el.isConnected || el.classList.contains("leaving")) return;
  el.classList.add("leaving");
  setTimeout(() => el.remove(), 220);
}
/* Help marks: help(text) is a "?" whose explanation shows on hover, keyboard focus or a tap. It never does anything
   else: a tap on one inside a label neither ticks nor opens what the label belongs to, and starts no action. */
const help = text => `<button type="button" class="help" data-help="${esc(text)}" aria-label="${esc(text)}">?</button>`;
let tipEl = null, tipFor = null;
function hideTip() {
  if (tipEl) tipEl.remove();
  if (tipFor) tipFor.classList.remove("open");
  tipEl = tipFor = null;
}
function showTip(btn) {
  hideTip();
  tipFor = btn;
  btn.classList.add("open");
  tipEl = document.createElement("div");
  tipEl.className = "tip";
  tipEl.setAttribute("role", "tooltip");
  tipEl.textContent = btn.dataset.help;
  document.body.append(tipEl);
  const r = btn.getBoundingClientRect(), t = tipEl.getBoundingClientRect(), m = 12;
  const left = Math.max(m, Math.min(r.left + r.width / 2 - t.width / 2, innerWidth - t.width - m));
  let top = r.bottom + 8;
  if (top + t.height > innerHeight - m) top = Math.max(m, r.top - t.height - 8);
  tipEl.style.left = `${left}px`;
  tipEl.style.top = `${top}px`;
}
document.addEventListener("click", e => {
  const b = e.target.closest && e.target.closest(".help");
  if (!b) { if (tipFor) hideTip(); return; }
  e.preventDefault();
  e.stopPropagation();
  if (tipFor === b) hideTip(); else showTip(b);
}, true);
document.addEventListener("mouseover", e => {
  const b = e.target.closest && e.target.closest(".help");
  if (b) { if (b !== tipFor) showTip(b); }
  else if (tipFor && !tipFor.matches(":focus")) hideTip();
});
document.addEventListener("focusin", e => { if (e.target.classList && e.target.classList.contains("help")) showTip(e.target); });
document.addEventListener("focusout", e => { if (e.target === tipFor) hideTip(); });
addEventListener("scroll", hideTip, { passive: true });
addEventListener("resize", hideTip);
/* The colour tests stay open or closed as left when the colours are saved and drawn again. */
document.addEventListener("toggle", e => { if (e.target.matches && e.target.matches("[data-color-tests]")) state.colorTestsOpen = e.target.open; }, true);
/* A folded group stays open or closed as left when its list is drawn again (after a replace, say). */
document.addEventListener("toggle", e => {
  const g = e.target.dataset && (e.target.dataset.soundGroup || e.target.dataset.fold);
  if (!g) return;
  if (e.target.dataset.soundGroup) (state.uiSoundOpen = state.uiSoundOpen || {})[g] = e.target.open;
  else (state.foldOpen = state.foldOpen || {})[g] = e.target.open;
}, true);

/* An action that fails where nothing catches it would leave the page silent, the button doing nothing: say what
   went wrong instead. */
window.addEventListener("unhandledrejection", e => {
  const why = e.reason && e.reason.message ? e.reason.message : String(e.reason || "");
  console.error(e.reason);
  toast(why ? `Something went wrong: ${why}` : "Something went wrong. Reload the page and try again.", "err", 9000);
});

/* The progress messages showing, newest last: an upload made meanwhile reports how far it has got into the newest.
   The bar is the whole task's: set(text, from, to) starts a step at `from`, and an upload made in that step fills
   the bar up to `to` (the end, unless said), so the bar never goes back. */
const progressStack = [];
function progressToast(label) {
  const el = toast("", "", 0);
  el.innerHTML = `<div class="row"><div class="label"></div><div class="pct"></div></div><div class="progress"><i></i></div>`;
  let text = label, frac = 0, upTo = 1;
  const show = (t, f) => {
    const pct = Math.max(0, Math.min(100, Math.round(f * 100)));
    $(".label", el).textContent = t.replace(/\s*\d{1,3}%$/, "");   /* the percentage has its own place */
    $(".pct", el).textContent = f > 0 ? `${pct}%` : "";
    $("i", el).style.width = `${pct}%`;
  };
  const p = {
    el,
    get frac() { return frac; },
    set(t, f, to = 1) { el.classList.remove("waiting"); text = t; frac = f; upTo = Math.max(f, to); show(t, f); },
    /* a step whose length can't be told: the bar sweeps while it runs */
    wait(t) { text = t; el.classList.add("waiting"); show(t, 0); },
    /* how far an upload made in this step has got: the step's own text, and the bar from the step's start to its end */
    upload(f) { show(/^Sending\b/.test(text) ? text : `${text.replace(/\s*\d{1,3}%$/, "")}\nSending to the console…`, frac + (upTo - frac) * f); },
    done(msg, kind) {
      const i = progressStack.indexOf(p);
      if (i >= 0) progressStack.splice(i, 1);
      leave(el);
      if (msg) toast(msg, kind, 7000);
    },
  };
  progressStack.push(p);
  p.set(label, 0);
  return p;
}

/* A progress message for work that is usually quick (saving, discarding): shown once it has taken a moment, so a
   quick one doesn't flash, and saying what it is waiting for. */
function laterProgress(text, after = 350) {
  let p = null, now = text;
  const timer = setTimeout(() => { p = progressToast(now); p.wait(now); }, after);
  return {
    wait(t) { now = t; if (p) p.wait(t); },
    done(msg, kind) { clearTimeout(timer); if (p) p.done(msg, kind); else if (msg) toast(msg, kind); },
  };
}

/* Loading markers: something being read from the console, shown where it will appear, with how far the download
   has got. A marker follows its download even when its section is drawn again meanwhile. */
const loads = new Map();   /* "stock:<path>" or "theme:<path>" -> the part read, 0 to 1 (-1 while the size is unknown) */
function loadFraction(key) {
  if (key !== "rco:all") return loads.has(key) ? loads.get(key) : 0;
  let done = 0, total = 0;   /* every interface file, by size */
  for (const a of aspectAssets(aspectById("interface"))) {
    const f = state.rco && state.rco.has(a.path) ? 1 : Math.max(0, loads.get(`stock:${a.path}`) || 0);
    done += f * a.size; total += a.size;
  }
  return total ? done / total : -1;
}
function loadingMarker(key, label, detail = "") {
  return `<div class="loading" role="status" data-loading="${esc(key)}">
      <div class="row"><span class="label">${esc(label)}</span><span class="pct"></span></div>
      <div class="progress" role="progressbar" aria-label="${esc(label)}" aria-valuemin="0" aria-valuemax="100"><i></i></div>
      ${detail ? `<p class="note">${esc(detail)}</p>` : ""}</div>`;
}
/* The short form, for a line in a row: "Reading… 45%", then "Ready". */
const loadingInline = key => `<span data-loading="${esc(key)}" data-inline></span>`;
function paintLoading(el) {
  const key = el.dataset.loading, f = loadFraction(key), pct = Math.round(Math.max(0, Math.min(1, f)) * 100);
  if (el.hasAttribute("data-inline")) {
    el.textContent = !loads.has(key) ? "Waiting to be read…" : f >= 1 ? "Ready" : f < 0 ? "Reading…" : `Reading… ${pct}%`;
    return;
  }
  el.classList.toggle("unknown", f < 0);
  $(".pct", el).textContent = f > 0 ? `${pct}%` : "";
  $(".progress i", el).style.width = f < 0 ? "" : `${pct}%`;
  const bar = $(".progress", el);
  if (f < 0) bar.removeAttribute("aria-valuenow"); else bar.setAttribute("aria-valuenow", String(pct));
}
function paintLoads(root = document) { for (const el of root.querySelectorAll("[data-loading]")) paintLoading(el); }
function loadProgress(key, f) {
  const before = loads.get(key);
  loads.set(key, f);
  if (before !== undefined && Math.round(before * 100) === Math.round(f * 100)) return;   /* a whole percent at a time */
  for (const el of document.querySelectorAll("[data-loading]")) {
    const k = el.dataset.loading;
    if (k === key || (k === "rco:all" && key.startsWith("stock:"))) paintLoading(el);
  }
}

/* A response's bytes, reporting how far the download has got. Without a size or a stream to follow, the marker
   shows that it is reading without a percentage. A download that stops part way is an error, never a short file. */
async function readBytes(res, onProgress) {
  /* a compressed response announces its compressed size: no percentage then, and no size check */
  const total = res.headers.get("Content-Encoding") ? 0 : Number(res.headers.get("Content-Length")) || 0;
  if (!total || !res.body || typeof res.body.getReader !== "function") {
    if (onProgress) onProgress(-1);
    const bytes = new Uint8Array(await res.arrayBuffer());
    if (onProgress) onProgress(1);
    return bytes;
  }
  const reader = res.body.getReader();
  let buf = new Uint8Array(total), got = 0;
  if (onProgress) onProgress(0);
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (got + value.length > buf.length) {   /* more than announced: keep all of it */
      const bigger = new Uint8Array(Math.max(buf.length * 2, got + value.length));
      bigger.set(buf.subarray(0, got));
      buf = bigger;
    }
    buf.set(value, got);
    got += value.length;
    if (onProgress) onProgress(Math.min(1, got / total));
  }
  if (got < total) throw new Error("the download stopped part way");
  if (onProgress) onProgress(1);
  return got === buf.length ? buf : buf.slice(0, got);
}
