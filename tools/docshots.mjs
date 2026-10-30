// Screenshots of the manager for the docs, from the PS5's own manager, read-only, with headless Brave over CDP.
//
// Nothing on the console changes: every request but GET is refused here before it leaves the browser (the page's
// own uploads and saves, which it may try on opening a theme, fail and change nothing), and so are the two GETs
// that write a file on the console (the title lookup and the diagnostics).
//
// No picture of Sony's (or of other publishers' games) is captured: the console's own interface icons
// (/api/ui-icon) are refused, so the page draws its own; every picture read from the console's files
// (/api/asset?path=<picture>, /api/asset/picture) is answered with a neutral stand-in, the same size where it
// matters; textures and the 30th Anniversary theme's pictures likewise, and so are a theme's styled system-tile icons
// (the icon style makes them from the console's own glyphs). The theme's own pictures (the author's test themes) are
// shown as they are.
//
// The manager is opened at PS5_THEME_URL (http://<console-ip>:8095) in BROWSER (any Chromium: Chrome, Edge, Brave),
// headless. The themes it opens (yakuza-0, snoopy) are the author's; change them below for yours.
//
//   PS5_THEME_URL=http://192.168.1.20:8095 node tools/docshots.mjs <out dir> [shots|probe] [only name,...]
// then copy the pictures wanted into docs/screenshots/.
import { spawn } from "node:child_process";
import { writeFileSync, mkdirSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import zlib from "node:zlib";

const [OUT, MODE = "shots", ONLY = ""] = process.argv.slice(2);
const PROFILE = mkdtempSync(join(tmpdir(), "docshots-"));
const PORT = 9341, CONSOLE = process.env.PS5_THEME_URL;
const BROWSER = process.env.BROWSER || "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser";
if (!OUT || !CONSOLE) { console.log("usage: PS5_THEME_URL=http://<console-ip>:8095 node tools/docshots.mjs <out dir> [shots|probe] [only,...]"); process.exit(2); }
const only = new Set(ONLY.split(",").filter(Boolean));
mkdirSync(OUT, { recursive: true });

/* ---- neutral stand-ins ---- */
function png(w, h, pixel) {
  const rows = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    rows[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; x++) {
      const [r, g, b] = pixel(x, y);
      const o = y * (w * 3 + 1) + 1 + x * 3;
      rows[o] = r; rows[o + 1] = g; rows[o + 2] = b;
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(rows)), chunk("IEND", Buffer.alloc(0))]);
}
/* a soft slate gradient with faint diagonal stripes: plainly a stand-in, not a broken picture */
const neutral = (x, y, w, h) => {
  const t = (x / w + y / h) / 2, stripe = ((x + y) >> 4) % 2 ? 6 : 0;
  return [Math.round(52 - 20 * t + stripe), Math.round(62 - 22 * t + stripe), Math.round(84 - 26 * t + stripe)];
};
const STAND_IN = png(256, 256, (x, y) => neutral(x, y, 256, 256));
const STAND_IN_WIDE = png(480, 270, (x, y) => neutral(x, y, 480, 270));
function picture(w, h) {   /* /api/asset/picture: width, height (LE), then RGBA */
  const b = Buffer.alloc(8 + w * h * 4);
  b.writeUInt32LE(w, 0); b.writeUInt32LE(h, 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const [r, g, bl] = neutral(x, y, w, h), o = 8 + (y * w + x) * 4;
    b[o] = r; b[o + 1] = g; b[o + 2] = bl; b[o + 3] = 255;
  }
  return b;
}
const PICTURE = picture(96, 54);

/* ---- the browser ---- */
const brave = spawn(BROWSER, ["--headless=new", `--remote-debugging-port=${PORT}`,
  `--user-data-dir=${PROFILE}`, "--hide-scrollbars", "--no-first-run", "--no-default-browser-check", "--window-size=1600,1000", "about:blank"], { stdio: "ignore" });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let targets = [];
for (let i = 0; i < 100 && !targets.some(t => t.type === "page"); i++) {
  try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch { /* starting */ }
  await sleep(200);
}
const ws = new WebSocket(targets.find(t => t.type === "page").webSocketDebuggerUrl);
await new Promise(r => { ws.onopen = r; });
let seq = 0, staged = null;
const waiting = new Map(), refused = [], errors = [];
const send = (method, params = {}) => new Promise((res, rej) => { const i = ++seq; waiting.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); });
const reply = (requestId, status, body, type) => send("Fetch.fulfillRequest", { requestId, responseCode: status,
  responseHeaders: [{ name: "Content-Type", value: type }, { name: "Cache-Control", value: "no-store" }], body: Buffer.from(body).toString("base64") });

ws.onmessage = async e => {
  const m = JSON.parse(e.data);
  const w = m.id && waiting.get(m.id);
  if (w) { waiting.delete(m.id); m.error ? w.rej(new Error(m.error.message)) : w.res(m.result); return; }
  if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  if (m.method !== "Fetch.requestPaused") return;
  const { requestId, request } = m.params;
  const url = new URL(request.url), path = url.pathname, asset = url.searchParams.get("path") || "", id = url.searchParams.get("id") || "";
  try {
    if (request.method !== "GET" || /^\/api\/(title-where|diag\/)/.test(path)) {   /* nothing on the console changes */
      refused.push(`${request.method} ${path}`);
      return await send("Fetch.failRequest", { requestId, errorReason: "BlockedByClient" });
    }
    if (path === "/api/ui-icon") return await reply(requestId, 404, '{"error":"left out of the docs"}', "application/json");
    if (path === "/api/asset/picture") return await reply(requestId, 200, PICTURE, "application/octet-stream");
    if (path === "/api/asset" && /\.(png|jpe?g|webp|gif|bmp|svg)$/i.test(asset)) return await reply(requestId, 200, /bg|background|wallpaper|4k|hub/i.test(asset) ? STAND_IN_WIDE : STAND_IN, "image/png");
    if (path === "/api/asset" && /\.dds$/i.test(asset)) return await reply(requestId, 404, '{"error":"left out of the docs"}', "application/json");
    if (/^\/api\/theme\/(file|source)$/.test(path) && /^\/user\//.test(asset) && /\.(png|jpe?g|webp|dds)$/i.test(asset)) {
      return await reply(requestId, 200, /pic\d|background/i.test(asset) ? STAND_IN_WIDE : STAND_IN, "image/png");
    }
    /* A system tile's icon in a theme: styled from the console's own glyph (the icon style), so the console's picture
       is in it. Stood in like the console's own; the tile's background, cut from the theme's picture, stays. */
    if (/^\/api\/theme\/(file|source)$/.test(path) && /^\/(system_ex|mnt)\/rnps\/apps\/NPXS\d+\//.test(asset) &&
        /icon[^/]*\.(png|jpe?g|webp|dds)$/i.test(asset)) {
      return await reply(requestId, 200, STAND_IN, "image/png");
    }
    if (/^\/api\/theme\/(preview|file|source)$/.test(path) && id === "playstation-30th-anniversary" &&
        (path !== "/api/theme/file" || /\.(png|jpe?g|webp|dds|svg)$/i.test(asset))) {
      return await reply(requestId, 200, STAND_IN_WIDE, "image/png");
    }
    if (path === "/api/status" && staged) {
      const s = await (await fetch(`${CONSOLE}/api/status`)).json();   /* a GET of our own, then shown changed */
      return await reply(requestId, 200, JSON.stringify(staged(s)), "application/json");
    }
    await send("Fetch.continueRequest", { requestId });
  } catch (err) { errors.push(`intercept ${path}: ${err.message}`); }
};

const run = async expr => {
  const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text);
  return r.result.value;
};
const waitFor = async (expr, what, ms = 120000) => {
  for (const t0 = Date.now(); Date.now() - t0 < ms; await sleep(300)) if (await run(expr).catch(() => false)) return;
  throw new Error(`timed out waiting for ${what}`);
};
async function viewport(w, h, phone = false) {
  await send("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: phone ? 2 : 1, mobile: phone });
}
/* the page as it would look with no message open and nothing focused */
async function tidy(extraCss = "") {
  await run(`(() => { let s = document.getElementById("shot-css"); if (!s) { s = document.createElement("style"); s.id = "shot-css"; document.head.append(s); }
    s.textContent = "header{position:static !important} .toasts,.tip{display:none !important} ${extraCss.replace(/`/g, "")}";
    if (document.activeElement) document.activeElement.blur(); return 1; })()`);
  await sleep(500);
}
async function shoot(name, rect, scale = 1) {
  const { data } = await send("Page.captureScreenshot", { format: "png", captureBeyondViewport: true,
    clip: { x: Math.max(0, rect.x), y: Math.max(0, rect.y), width: rect.w, height: rect.h, scale } });
  writeFileSync(`${OUT}/${name}`, Buffer.from(data, "base64"));
  console.log(`wrote ${name} ${Math.round(rect.w * scale)} x ${Math.round(rect.h * scale)}`);
}
/* an element's box on the page (scrolled into view first), padded, at most `maxH` tall */
const boxOf = async (sel, pad = 20, maxH = 4000, pick = "") => run(`(() => { const el = ${pick || `document.querySelector(${JSON.stringify(sel)})`};
  if (!el) return null; el.scrollIntoView({ block: "start" }); const b = el.getBoundingClientRect();
  return { x: b.left + scrollX - ${pad}, y: b.top + scrollY - ${pad}, w: b.width + ${2 * pad}, h: Math.min(b.height + ${2 * pad}, ${maxH}) }; })()`);
async function go(hash, wait, what) {
  await run(`location.hash = ${JSON.stringify(hash)}; 1`);
  await sleep(600);
  if (wait) await waitFor(wait, what);
  await sleep(1500);
}
const partReady = part => `typeof state !== "undefined" && state.route === "edit" && (state.section || "theme") === ${JSON.stringify(part)} && !!document.querySelector("#pane") && !document.querySelector("#pane [data-loading]")`;

try {
  await send("Page.enable"); await send("Runtime.enable");
  await send("Fetch.enable", { patterns: [{ urlPattern: "*", requestStage: "Request" }] });
  await viewport(1600, 1000);
  await send("Page.navigate", { url: `${CONSOLE}/?r=${Date.now()}#/library` });
  await waitFor(`typeof state !== "undefined" && !!state.status && !!state.status.fw && !!document.querySelector(".card")`, "the library");
  await sleep(2500);

  if (MODE === "probe") {
    for (const [theme, part] of [["yakuza-0", "theme"], ["yakuza-0", "background"], ["yakuza-0", "tiles"], ["yakuza-0", "titles"], ["yakuza-0", "music"],
                                 ["yakuza-0", "sounds"], ["yakuza-0", "fonts"], ["yakuza-0", "appearance"], ["yakuza-0", "interface"], ["yakuza-0", "icons"],
                                 ["snoopy", "icons"], ["snoopy", "tiles"]]) {
      await go(`#/edit/${theme}/${part}`, partReady(part), `${theme} ${part}`);
      await tidy();
      const info = await run(`(() => [...document.querySelector("#pane").children].map(c => c.tagName + (c.id ? "#" + c.id : "") + "." + [...c.classList].join(".") + " h" + Math.round(c.getBoundingClientRect().height)).join(" | "))()`);
      console.log(`${theme}/${part}: ${info}`);
      const r = await boxOf("#pane", 10, 3000);
      if (r) await shoot(`probe-${theme}-${part}.png`, r, 0.5);
    }
  } else {
    const want = n => !only.size || only.has(n);

    if (want("library.png")) {
      await go("#/library", `!!document.querySelector(".card img")`, "the cards");
      await tidy();
      await run("scrollTo(0, 0); 1");
      const h = await run(`Math.min(document.documentElement.scrollHeight, 1780)`);
      await shoot("library.png", { x: 0, y: 0, w: 1600, h });
    }
    if (want("new-theme.png")) {
      await run(`(() => { state.creating = true; state.mix = null; renderLibrary(); scrollTo(0, 0); return 1; })()`);
      await sleep(800); await tidy();
      await shoot("new-theme.png", await boxOf("#create", 0));
      await run(`(() => { state.creating = false; renderLibrary(); return 1; })()`);
    }
    if (want("mix.png")) {
      await run(`(() => { openMix(); return 1; })()`);
      await waitFor(`!!document.querySelector("form#mix")`, "the mix form", 300000);
      await sleep(1500); await tidy();
      await shoot("mix.png", await boxOf("form#mix", 0, 1300));
      await run(`(() => { state.mix = null; renderLibrary(); return 1; })()`);
    }
    if (want("applying.png")) {
      staged = s => ({ ...s, applying: "yakuza-0", progress: { text: "Checking it on the console (11 of 15)", percent: 72 } });
      await go("#/library", `!!document.querySelector(".banner .progress")`, "the applying banner");
      await sleep(1500); await tidy();
      await run("scrollTo(0, 0); 1");
      await shoot("applying.png", { x: 0, y: 0, w: 1600, h: 875 });
      staged = null;
      await sleep(2500);
    }
    if (want("edit-top.png")) {
      await go("#/edit/yakuza-0/theme", partReady("theme"), "Name and picture");
      await tidy("header{position:static !important} .savebar{display:none !important}");
      await run("scrollTo(0, 0); 1");
      await shoot("edit-top.png", { x: 0, y: 0, w: 1600, h: 1000 });
    }
    if (want("manager-originals.png")) {
      await go("#/edit/yakuza-0/theme", partReady("theme"), "Name and picture");
      await tidy(".savebar{display:none !important}");
      await shoot("manager-originals.png", await boxOf("#originals-field", 18, 560, `document.querySelector("#originals-field").closest(".panel")`));
    }
    for (const [file, theme, part, maxH] of [["background.png", "yakuza-0", "background", 1250], ["manager-fonts.png", "yakuza-0", "fonts", 1300],
                                             ["manager-appearance.png", "yakuza-0", "appearance", 1450], ["manager-interface.png", "yakuza-0", "interface", 1150],
                                             ["manager-tiles.png", "yakuza-0", "tiles", 1100], ["icons.png", "snoopy", "icons", 1000]]) {
      if (!want(file)) continue;
      await go(`#/edit/${theme}/${part}`, partReady(part), `${theme} ${part}`);
      if (part === "tiles") await waitFor(`!document.querySelector("#pane").textContent.includes("Reading…")`, "the tiles' names", 180000).catch(() => console.log("tile names still loading"));
      await tidy(".savebar{display:none !important} header{position:static !important}");
      await shoot(file, await boxOf("#pane", 0, maxH));
    }
    if (want("sounds.png") || want("interface-sounds.png")) {
      await go("#/edit/yakuza-0/sounds", partReady("sounds"), "Sounds");
      await waitFor(`!!document.querySelector('#ui-sounds .rows') && !document.querySelector('#ui-sounds [data-loading]')`, "the interface sounds", 600000);
      await sleep(1000);
      await tidy(".savebar{display:none !important} header{position:static !important}");
      if (want("sounds.png")) {   /* Other sounds: from where the interface sounds end */
        const r = await run(`(() => { const p = document.querySelector("#pane"), u = document.querySelector("#ui-sounds"), a = p.getBoundingClientRect();
          u.scrollIntoView({ block: "end" }); const b = u.getBoundingClientRect(), end = p.querySelector(".aspect").getBoundingClientRect();
          return { x: a.left + scrollX, y: b.bottom + scrollY, w: a.width, h: Math.min(end.bottom - b.bottom + 4, 760) }; })()`);
        await shoot("sounds.png", r);
      }
      if (want("interface-sounds.png")) await shoot("interface-sounds.png", await boxOf("#ui-sounds", 0, 1300));
    }
    if (want("home-music.png") || want("home-music-playlist.png")) {
      await go("#/edit/yakuza-0/music", partReady("music"), "Home music");
      await tidy(".savebar{display:none !important} header{position:static !important}");
      if (want("home-music.png")) await shoot("home-music.png", await boxOf("#pane", 0, 520));
      if (want("home-music-playlist.png")) {   /* from the playlist's own line to its joins and end */
        const r = await run(`(() => { const p = document.querySelector("#pane"), head = [...p.querySelectorAll("b, strong, h3, h4, .title")].find(e => /^Playlist/.test(e.textContent.trim()));
          const top = (head ? head.closest("div,section") : p).getBoundingClientRect(), a = p.getBoundingClientRect();
          return { x: a.left + scrollX, y: top.top + scrollY - 12, w: a.width, h: 700 }; })()`);
        await shoot("home-music-playlist.png", r);
      }
    }
    if (want("manager-games.png") || want("icon-style.png")) {
      await go("#/edit/yakuza-0/titles", partReady("titles") + ` && !!document.querySelector(".icon-style")`, "Games and apps");
      /* a shape chosen in the page only (nothing is saved), to show the style's options */
      await run(`(() => { state.choices.iconStyle = { ...iconStyle(), shape: "rounded" }; renderAspect(aspectById("titles")); return 1; })()`);
      await sleep(800);
      await waitFor(`!document.querySelector(".icon-style .opts[aria-busy]")`, "the shapes drawn");
      await sleep(1500);
      await tidy(".savebar{display:none !important} header{position:static !important}");
      if (want("icon-style.png")) await shoot("icon-style.png", await boxOf(".icon-style", 0));
      if (want("manager-games.png")) {
        /* searched, as the earlier picture was: the theme's own games (their icons are stand-ins, never a publisher's art) */
        await run(`(() => { state.filters.titles = "Yakuza"; renderAspect(aspectById("titles")); return 1; })()`);
        await sleep(2500);
        await tidy(".savebar{display:none !important} header{position:static !important}");
        await shoot("manager-games.png", await boxOf("#pane", 0, 1500));
        await run(`(() => { state.filters.titles = ""; renderAspect(aspectById("titles")); return 1; })()`);
      }
    }
    if (want("picture-editor.png")) {
      await go("#/edit/yakuza-0/titles", partReady("titles"), "Games and apps");
      /* the theme's own wide background, a little zoomed: the bright square, and the rest dimmed */
      await run(`(async () => { await openBgEditor("/api/theme/preview?" + q({ id: "yakuza-0", kind: "background" }) + "&v=" + Date.now()); bgeZoom(1.25); return 1; })()`);
      await waitFor(`!!document.querySelector("#bge .bge-dialog")`, "the picture editor");
      await sleep(1500); await tidy();
      await shoot("picture-editor.png", await boxOf("#bge .bge-dialog", 16));
      await run(`(() => { bgeClose(); return 1; })()`);
    }
    if (want("preview.png")) {
      await go("#/edit/yakuza-0/theme", partReady("theme"), "Name and picture");
      await run(`(async () => { await openPreview(); return 1; })()`);
      await waitFor(`!!document.querySelector("#pv-overlay .pv-dialog") && !document.querySelector("#pv-overlay [data-loading]")`, "the preview", 300000);
      await sleep(3000); await tidy();
      await shoot("preview.png", await boxOf("#pv-overlay .pv-dialog", 16));
      await run(`(() => { previewClose(); return 1; })()`);
    }
    if (want("save-bar.png")) {
      await go("#/edit/yakuza-0/theme", partReady("theme"), "Name and picture");
      await run(`(() => { const b = document.querySelector("#savebar"); b.hidden = false; return 1; })()`);
      await tidy();
      await shoot("save-bar.png", await boxOf("#savebar", 0));
    }
    if (want("activity.png")) {
      await go("#/activity", `!!document.querySelector("#log") && document.querySelector("#log").textContent.length > 200`, "the log");
      /* only the plugin's own lines: none naming a game or app of the console's library */
      await run(`(() => { const l = document.querySelector("#log");
        l.textContent = l.textContent.split("\\n").filter(x => !/\\/user\\/|appmeta|[A-Z]{4}\\d{5}/.test(x)).join("\\n"); return 1; })()`);
      await tidy();
      await run("scrollTo(0, 0); 1");
      await shoot("activity.png", { x: 0, y: 0, w: 1600, h: 1125 });
    }
    if (want("phone-library.png") || want("phone-edit.png")) {
      await viewport(390, 844, true);
      await send("Page.navigate", { url: `${CONSOLE}/?r=${Date.now()}#/library` });
      await waitFor(`typeof state !== "undefined" && !!state.status && !!document.querySelector(".card img")`, "the library on a phone");
      await sleep(2500);
      if (want("phone-library.png")) {
        await tidy();
        await run("scrollTo(0, 0); 1");
        await shoot("phone-library.png", { x: 0, y: 0, w: 390, h: 1250 }, 2);
      }
      if (want("phone-edit.png")) {
        await go("#/edit/yakuza-0", `typeof state !== "undefined" && state.route === "edit" && !!document.querySelector("#secnav")`, "the parts list on a phone");
        await tidy(".savebar{display:none !important}");
        await run("scrollTo(0, 0); 1");
        await shoot("phone-edit.png", { x: 0, y: 0, w: 390, h: 1250 }, 2);
      }
    }
  }
} catch (e) { console.log("ERROR", e.message); }
if (refused.length) console.log("refused (nothing reached the console):", [...new Set(refused)].join(", "));
if (errors.length) console.log("page errors:", errors.slice(0, 6).join(" || "));
ws.close(); brave.kill(); process.exit(0);
