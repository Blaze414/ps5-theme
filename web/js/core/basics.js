"use strict";

/* ================================================================ basics */

/* Where a theme .zip keeps its files: the console's themable folders (theme.c). */
const ASSET_ROOTS = ["system_ex/rnps/", "mnt/rnps/", "system_ex/vsh_asset/", "user/appmeta/", "user/app/", "user/catalog_downloader/"];
const IMAGE_MIME = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", webp: "image/webp" };
const HOME_MUSIC = "/system_ex/vsh_asset/bgm_home.at9";  /* the console's home screen music */
const MUSIC_RATE = 48000;                                 /* a theme keeps its music source at this rate */
const MUSIC_SOURCE_MAX_BYTES = 128 * 1048576 - 1024;      /* the plugin's limit for that WAV */
const BG_RE = /(^|[\/_.\-])(bg|bgs|background|backgrounds|wallpaper|wallpapers|backdrop|home_?bg)([\/_.\-@]|$)/i;
const ICON_RE = /(^|[\/_.\-])(icon|icons|ico|glyph|glyphs|logo|logos|badge|badges|symbol|symbols)([\/_.\-@]|$)|\/ic_[^\/]*$/i;

const state = {
  status: { fw: "", ip: "", active: "none", applied: "", result: "", themes: [] },
  route: "library",
  editId: "",
  creating: false,
  assets: null,          /* all themable system assets, loaded once */
  assetsError: "",
  protectedCount: 0,
  files: new Set(),      /* paths replaced in the theme being edited */
  sources: new Set(),    /* of those, the ones whose original picture is kept in the theme (see keepSource) */
  keepOriginals: true,   /* the theme keeps originals of what is attached (off: to save space) */
  originalsBytes: 0,     /* what the kept originals take on the console */
  fit: "cover",          /* how pictures are fitted: the open theme's choice (loadChoices) */
  choices: {},           /* the open theme's choices (see "the editor's choices") */
  choicesId: "",
  confirming: "",        /* aspect waiting for "turn off" confirmation */
  pages: {},             /* per-aspect page size */
  filters: {},           /* per-aspect search text */
  trim: null,            /* home music part being chosen (see openTrim) */
};
/* Bumped when this page changes a theme's files, so a picture it shows again is the new one (a page keeps its own
   copy of a picture it has shown). The same at every load: the browser asks the console each time (no-cache), and an
   unchanged picture is "not modified" in a few bytes instead of sent again. */
let stamp = 0;

/* A map that keeps its newest `max` entries only: the oldest goes first, handed to `drop` (to free it) if given. */
class LimitedCache {
  constructor(max, drop = null) {
    this.max = max;
    this.drop = drop;
    this.map = new Map();
  }
  has(key) { return this.map.has(key); }
  get(key) { return this.map.get(key); }
  set(key, value) {
    this.map.delete(key);
    this.map.set(key, value);
    while (this.map.size > this.max) {
      const [k, v] = this.map.entries().next().value;
      this.map.delete(k);
      if (this.drop) this.drop(v);
    }
    return value;
  }
  clear() {
    if (this.drop) for (const v of this.map.values()) this.drop(v);
    this.map.clear();
  }
  /* The promise make() gives, made once for `key`; one that fails is forgotten, so the next ask tries again. */
  once(key, make) {
    if (!this.map.has(key)) {
      const p = this.set(key, make());
      p.catch(() => { if (this.map.get(key) === p) this.map.delete(key); });
    }
    return this.map.get(key);
  }
}

const $ = (sel, root = document) => root.querySelector(sel);
const view = $("#view");

function localGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function localSet(k, v) { try { if (v === null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function q(params) { return Object.entries(params).map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join("&"); }
function fmtSize(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}
function fmtTime(s) { s = Math.round(s); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; }
function plural(n, word, many = word + "s") { return `${n} ${n === 1 ? word : many}`; }
function ext(path) { const m = /\.([a-z0-9]+)$/i.exec(path); return m ? m[1].toLowerCase() : ""; }
function baseName(path) { return path.slice(path.lastIndexOf("/") + 1); }
function dirName(path) { return path.slice(0, path.lastIndexOf("/")); }
function themeById(id) { return state.status.themes.find(t => t.id === id); }
const sleep = ms => new Promise(r => setTimeout(r, ms));
/* Let the page repaint between chunks of long work. A message rather than a
   timer: timers in background or embedded views can be throttled to one a
   second, which made encoding 30 times slower. */
const nextTask = () => new Promise(r => { const ch = new MessageChannel(); ch.port1.onmessage = () => r(); ch.port2.postMessage(0); });
const isLocal = () => ["127.0.0.1", "localhost"].includes(location.hostname);

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
const hex8 = n => n.toString(16).padStart(8, "0");

/* Changes being sent to the console right now: the page doesn't redraw from the console's side meanwhile. */
let writing = 0;
/* `timeout` (ms): given up after that long without a whole answer. For the status, read every few seconds: a request
   left hanging (the console went into rest mode as it answered) would otherwise hold every later look back for good. */
async function api(method, path, params = {}, body, { timeout = 0 } = {}) {
  const url = path + (Object.keys(params).length ? "?" + q(params) : "");
  const opts = { method, headers: {} };
  if (method !== "GET") opts.headers["X-PS5-Theme"] = "1";
  if (body !== undefined) opts.body = body;
  let res, timer = 0;
  if (timeout && typeof AbortController === "function") {
    const ctl = new AbortController();
    opts.signal = ctl.signal;
    timer = setTimeout(() => ctl.abort(), timeout);
  }
  if (method !== "GET") writing++;
  try {
    for (let attempt = 0; ; attempt++) {
      res = await fetch(url, opts);
      if (res.status !== 503 || attempt === 4) break;  /* console busy: retry briefly */
      await sleep(400 * (attempt + 1));
    }
  } catch (e) {
    clearTimeout(timer);
    if (e && e.name === "AbortError") throw new Error("the console didn't answer in time");
    throw e;
  } finally { if (method !== "GET") writing--; }
  const type = res.headers.get("Content-Type") || "";
  let data;
  try { data = type.includes("json") ? await res.json() : await res.text(); }
  catch (e) { throw e && e.name === "AbortError" ? new Error("the console didn't answer in time") : e; }
  finally { clearTimeout(timer); }
  if (!res.ok) {
    if (data && data.access) showGate(data.access);
    throw new Error((data && data.error) || `HTTP ${res.status}`);
  }
  homeTouched(method, path, params);
  return data;
}

/* Upload with an end-to-end checksum: the console verifies it on receipt and
   reports what it stored, which must match what we computed. */
async function uploadVerified(path, params, blob) {
  /* Not sent when it can't fit: the console keeps part of its storage free for itself (the plugin refuses it too) */
  const room = state.status && state.status.room;
  if (typeof room === "number" && blob.size > room) throw new Error("the console's storage is nearly full: free some space, then try again");
  const bytes = new Uint8Array(await blob.arrayBuffer());
  const crc = hex8(crc32(bytes));
  const shown = progressStack.filter(x => x.el.isConnected).pop();
  let r;
  writing++;
  try {
    r = shown && typeof XMLHttpRequest === "function"
      ? await putWatched(path, { ...params, crc }, bytes, f => shown.upload(f))
      : await api("PUT", path, { ...params, crc }, bytes);
  } finally { writing--; }
  if (r.crc32 !== crc || r.size !== bytes.length) throw new Error("the console stored something different from what was sent");
  homeTouched("PUT", path, params);
  return r;
}

/* A PUT as api() makes it (the same header, the same retries while the console is busy, the same errors), with
   the upload's progress reported as it goes. */
async function putWatched(path, params, bytes, onProgress) {
  const url = path + (Object.keys(params).length ? "?" + q(params) : "");
  for (let attempt = 0; ; attempt++) {
    const x = await new Promise((resolve, reject) => {
      const r = new XMLHttpRequest();
      r.open("PUT", url);
      r.setRequestHeader("X-PS5-Theme", "1");
      if (r.upload) r.upload.onprogress = e => { if (e.lengthComputable && e.total) onProgress(e.loaded / e.total); };
      r.onload = () => resolve(r);
      r.onerror = () => reject(new Error("the console could not be reached"));
      r.onabort = () => reject(new Error("the upload was stopped"));
      r.send(bytes);
    });
    if (x.status === 503 && attempt < 4) { await sleep(400 * (attempt + 1)); continue; }
    let data = x.responseText;
    if ((x.getResponseHeader("Content-Type") || "").includes("json")) { try { data = JSON.parse(data); } catch (e) { /* as text */ } }
    if (x.status < 200 || x.status >= 300) {
      if (data && data.access) showGate(data.access);
      throw new Error((data && data.error) || `HTTP ${x.status}`);
    }
    return data;
  }
}
