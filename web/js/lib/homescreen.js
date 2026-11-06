"use strict";

/* ---- The home screen and the Control Center as the console draws them ----

   At the console's own size, 1920 x 1080 (it lays its interface out at that size and doubles it for 4K), measured
   from captures of 13.42's home screen: where each thing sits, its size and corners, the text's sizes and
   baselines, the shade over the background. Drawn on a canvas, so the same drawing is the preview and the picture
   on the theme's card in the library. */
const HS = {
  tabs: { x: 92, y: 72.5, size: 35, gap: 84, dim: .82 },                    /* Games, Media: baseline and size in px */
  top: { y: 63, search: 1417.5, settings: 1522.5, avatar: 1625.75, icon: 48, face: 48 },   /* centres, top right */
  clock: { right: 1834, y: 72.7, size: 33.8 },
  row: { y: 126, x: 172, big: 168, bigR: 30, small: 106, smallR: 20, pitch: 114, gap: 16 },
  label: { y: 272.3, size: 29.5 },                                           /* the selected tile's name */
  hub: { x: 172, y: 767.8, size: 53.6, button: { y: 846, w: 372, h: 72, size: 28.3, base: 891, fill: .17 } },
  /* The shade over the background, a blue-black: across the top behind the bar, over the lower left behind the
     selected title's name and button, and along the bottom. */
  shade: "6,7,14",
  shadeTop: { h: 240, a: .42 },
  shadeLeft: { from: 600, full: 780, stops: [[0, .62], [.57, .6], [.67, .45], [.75, .33], [.83, .2], [.9, 0]] },
  shadeBottom: { from: 760, a: .45 },
};
/* Where tile `i` starts, with tile `focus` selected: the selected one always at the same place (the row scrolls), the
   ones after it past the space for its name, the ones before it to its left. */
function hsTileX(i, focus) {
  const r = HS.row;
  if (i === focus) return r.x;
  return i > focus ? r.x + r.big + r.gap + r.pitch * (i - focus - 1) : r.x - r.gap - r.small - r.pitch * (focus - 1 - i);
}
/* Which tile is under a point of the screen (1920 x 1080 units), or -1. */
function hsTileAt(x, y, focus, count) {
  for (let i = 0; i < count; i++) {
    const size = i === focus ? HS.row.big : HS.row.small, X = hsTileX(i, focus);
    if (x >= X && x < X + size && y >= HS.row.y && y < HS.row.y + size) return i;
  }
  return -1;
}
/* A picture filling a box as the console fills one: scaled to cover it, centred. */
function hsCover(ctx, img, x, y, w, h) {
  const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
  if (!iw || !ih) return;
  const sc = Math.max(w / iw, h / ih), sw = w / sc, sh = h / sc;
  ctx.drawImage(img, (iw - sw) / 2, (ih - sh) / 2, sw, sh, x, y, w, h);
}
function hsRound(ctx, x, y, w, h, r) {
  ctx.beginPath();
  if (ctx.roundRect) ctx.roundRect(x, y, w, h, r);
  else { ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r); ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath(); }
}
let hsShade = null;
function hsLeftShade() {
  if (hsShade) return hsShade;
  const c = document.createElement("canvas"), L = HS.shadeLeft;
  c.width = 480; c.height = 270;
  const g = c.getContext("2d"), h = g.createLinearGradient(0, 0, c.width, 0);
  for (const [at, a] of L.stops) h.addColorStop(at, `rgba(${HS.shade},${a})`);
  g.fillStyle = h; g.fillRect(0, 0, c.width, c.height);
  const v = g.createLinearGradient(0, L.from / 4, 0, L.full / 4);
  v.addColorStop(0, "rgba(0,0,0,0)"); v.addColorStop(1, "#000");
  g.globalCompositeOperation = "destination-in";
  g.fillStyle = v; g.fillRect(0, 0, c.width, c.height);
  return (hsShade = c);
}
const hsFont = (family, size) => `${size}px ${family ? `"${family}", ` : ""}"SST", system-ui, sans-serif`;
/* The clock at the top right, its digits all one width as the console's are. */
function hsClockDraw(ctx, text, family) {
  ctx.fillStyle = "#fff";
  ctx.font = hsFont(family, HS.clock.size);
  ctx.textAlign = "center";
  const cell = Math.max(...[..."0123456789"].map(d => ctx.measureText(d).width));
  let right = HS.clock.right;
  for (const ch of [...text].reverse()) {
    const w = /\d/.test(ch) ? cell : ctx.measureText(ch).width;
    ctx.fillText(ch, right - w / 2, HS.clock.y);
    right -= w;
  }
}
/* Draws the home screen on `ctx`, already scaled so that 1920 x 1080 fills it. m: { bg, art (pictures, or null),
   tab ("games" or "media"), tabs ([games, media] names), tiles ([{ img, name }]), focus, clock, search, settings,
   hub ({ name, button } or null), font (a loaded family, or ""), dim (how much darker the background is: 0 to 1) } */
function homeScreenDraw(ctx, m) {
  const r = HS.row, t = HS.top;
  const font = size => hsFont(m.font, size);
  ctx.save();
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, 1920, 1080);
  if (m.bg) hsCover(ctx, m.bg, 0, 0, 1920, 1080);
  ctx.drawImage(hsLeftShade(), 0, 0, 1920, 1080);
  const low = ctx.createLinearGradient(0, HS.shadeBottom.from, 0, 1080);
  low.addColorStop(0, `rgba(${HS.shade},0)`); low.addColorStop(1, `rgba(${HS.shade},${HS.shadeBottom.a})`);
  ctx.fillStyle = low;
  ctx.fillRect(0, HS.shadeBottom.from, 1920, 1080 - HS.shadeBottom.from);
  const top = ctx.createLinearGradient(0, 0, 0, HS.shadeTop.h);
  top.addColorStop(0, `rgba(${HS.shade},${HS.shadeTop.a})`); top.addColorStop(1, `rgba(${HS.shade},0)`);
  ctx.fillStyle = top;
  ctx.fillRect(0, 0, 1920, HS.shadeTop.h);
  if (m.dim) {
    ctx.fillStyle = `rgba(0,0,0,${m.dim})`;
    ctx.fillRect(0, 0, 1920, 1080);
  }
  if (m.art) {   /* the Appearance theme's artwork, hanging from the top at the screen's width */
    const w = m.art.naturalWidth || m.art.width, h = m.art.naturalHeight || m.art.height;
    if (w && h) ctx.drawImage(m.art, 0, 0, 1920, 1920 * h / w);
  }
  ctx.textBaseline = "alphabetic";
  ctx.fillStyle = "#fff";
  ctx.font = font(HS.tabs.size);
  ctx.textAlign = "left";
  let x = HS.tabs.x;
  const tabs = m.tabs.map((name, i) => {
    ctx.globalAlpha = (i === 0) === (m.tab === "games") ? 1 : HS.tabs.dim;
    ctx.fillText(name, x, HS.tabs.y);
    const w = ctx.measureText(name).width, at = [x, x + w];
    x += w + HS.tabs.gap;
    return at;
  });
  ctx.globalAlpha = 1;
  for (const [img, cx] of [[m.search, t.search], [m.settings, t.settings]]) if (img) ctx.drawImage(img, cx - t.icon / 2, t.y - t.icon / 2, t.icon, t.icon);
  ctx.fillStyle = "#5b606b";   /* (your profile picture on the console: not one the plugin reads) */
  ctx.beginPath(); ctx.arc(t.avatar, t.y, t.face / 2, 0, 2 * Math.PI); ctx.fill();
  hsClockDraw(ctx, m.clock, m.font);
  m.tiles.forEach((tile, i) => {
    const big = i === m.focus, size = big ? r.big : r.small, X = hsTileX(i, m.focus);
    if (X > 1920 || X + size < 0) return;
    ctx.save();
    hsRound(ctx, X, r.y, size, size, big ? r.bigR : r.smallR);
    ctx.clip();
    ctx.fillStyle = "rgba(255,255,255,.12)";
    ctx.fillRect(X, r.y, size, size);
    if (tile.img) hsCover(ctx, tile.img, X, r.y, size, size);
    ctx.restore();
  });
  const sel = m.tiles[m.focus];
  ctx.textAlign = "left";
  if (sel && sel.name) {
    ctx.font = font(HS.label.size);
    ctx.fillText(sel.name, hsTileX(m.focus + 1, m.focus), HS.label.y);
  }
  if (m.hub) {
    const b = HS.hub.button;
    ctx.font = font(HS.hub.size);
    ctx.fillText(m.hub.name, HS.hub.x, HS.hub.y);
    ctx.fillStyle = `rgba(255,255,255,${b.fill})`;
    hsRound(ctx, HS.hub.x, b.y, b.w, b.h, b.h / 2);
    ctx.fill();
    ctx.fillStyle = "#fff";
    ctx.font = font(b.size);
    ctx.textAlign = "center";
    ctx.fillText(m.hub.button, HS.hub.x + b.w / 2, b.base);
  }
  ctx.restore();
  return { tabs };   /* where each tab's name is drawn, [from, to] across */
}

/* ---- The Control Center over it (the PS button), as 13.42 draws it ----

   Measured from a capture with the PlayStation 2 look. The home screen stays behind without its Appearance artwork,
   its background darker, and all of it under the palette's backdrop: nearly opaque behind the top bar, lighter from
   a third of the way down. The look's two pieces sit at three quarters of their size, as all of the console's
   2560-wide pictures do at 1920: one in the top right corner, one along the bottom. Over them the clock, the focused
   card (the screenshot taken last) and the row of icons, none of them selected. */
const CC = {
  dim: .47,   /* the home screen's background alone, this much darker */
  backdrop: [[0, .95], [110, .95], [130, .91], [150, .87], [170, .81], [190, .75], [210, .7], [230, .64], [250, .59],
    [270, .53], [290, .45], [310, .4], [330, .36], [350, .34], [1080, .34]],   /* [y, opacity] */
  scale: .75,
  card: {
    x: 84, y: 370, w: 432, h: 520, r: 13,
    base: [[129, 143, 153], [57, 62, 78]], baseA: .92,   /* a light fill under the palette's card colour: left, right */
    /* the focus ring, 3 apart and 3 wide, its light sweeping round: [angle from the right, clockwise; opacity] */
    ring: { gap: 3, w: 3, rgb: "215,222,250", stops: [[0, .14], [30, .45], [60, .47], [135, .5], [190, .38], [230, .44],
      [250, .34], [270, .17], [285, .1], [340, .08], [360, .14]] },
    icon: { x: 100, y: 386, size: 48, r: 8, glyph: 44, ink: "#1c1d20" },
    shot: { x: 108, y: 450, w: 384, h: 216, r: 4 },
    /* [baseline, size, opacity, letter spacing]; the time 16 after the date */
    text: { x: 108, lines: [[776, 22.5, .78, .75], [818, 26.5, 1, 1.1], [864, 22.5, .78, 0]], gap: 16 },
  },
  row: { y: 1011, pitch: 112, size: 44, alpha: .55, face: 48, dot: { dx: 21, dy: -21.5, r: 5, color: "#009aff" } },
};
/* Where a piece goes, from its size on the console (w x h in the texture's pixels). */
function ccPieceBox(which, w, h) {
  const W = w * CC.scale, H = h * CC.scale;
  return which === "top" ? { x: 1920 - W, y: 0, w: W, h: H } : { x: (1920 - W) / 2, y: 1080 - H, w: W, h: H };
}
/* The centre of icon `i` of `n` in the row. */
const ccIconX = (i, n) => 960 + (i - (n - 1) / 2) * CC.row.pitch;
const ccRgba = (argb, k = 1) => { const v = argb >>> 0; return `rgba(${(v >> 16) & 255},${(v >> 8) & 255},${v & 255},${(v >>> 24) / 255 * k})`; };
/* Draws the Control Center on `ctx`, already scaled so that 1920 x 1080 fills it. m: { home (homeScreenDraw's), top
   and bottom ({ img, w, h }: the picture and its size on the console, or null), icons (the row: { img, dot (a
   notification not yet seen) }, null where the profile picture goes), capture (the card's icon), colors ({ backdrop,
   card }: 0xAARRGGBB), clock, shot ({ day, time }: when the card's screenshot was taken) } */
function ccScreenDraw(ctx, m) {
  const c = CC.card, font = size => hsFont(m.home.font, size);
  homeScreenDraw(ctx, { ...m.home, art: null, dim: CC.dim });
  ctx.save();
  ctx.textBaseline = "alphabetic";
  const shade = ctx.createLinearGradient(0, 0, 0, 1080);
  for (const [y, a] of CC.backdrop) shade.addColorStop(y / 1080, ccRgba(m.colors.backdrop, a));
  ctx.fillStyle = shade;
  ctx.fillRect(0, 0, 1920, 1080);
  for (const which of ["bottom", "top"]) {
    const p = m[which];
    if (!p) continue;
    const b = ccPieceBox(which, p.w, p.h);
    ctx.drawImage(p.img, b.x, b.y, b.w, b.h);
  }
  hsClockDraw(ctx, m.clock, m.home.font);

  /* the card: its light fill, the palette's colour over it, the ring around */
  hsRound(ctx, c.x, c.y, c.w, c.h, c.r);
  const base = ctx.createLinearGradient(c.x, 0, c.x + c.w, 0);
  c.base.forEach((rgb, i) => base.addColorStop(i, `rgba(${rgb},${c.baseA})`));
  ctx.fillStyle = base;
  ctx.fill();
  ctx.fillStyle = ccRgba(m.colors.card);
  ctx.fill();
  const R = c.ring, out = R.gap + R.w / 2;
  if (ctx.createConicGradient) {
    const g = ctx.createConicGradient(0, c.x + c.w / 2, c.y + c.h / 2);
    for (const [deg, a] of R.stops) g.addColorStop(deg / 360, `rgba(${R.rgb},${a})`);
    ctx.strokeStyle = g;
  } else ctx.strokeStyle = `rgba(${R.rgb},.35)`;
  ctx.lineWidth = R.w;
  hsRound(ctx, c.x - out, c.y - out, c.w + 2 * out, c.h + 2 * out, c.r + out);
  ctx.stroke();
  const I = c.icon;
  ctx.fillStyle = "#fff";
  hsRound(ctx, I.x, I.y, I.size, I.size, I.r);
  ctx.fill();
  if (m.capture) ctx.drawImage(ccInk(m.capture, I.glyph, I.ink), I.x + (I.size - I.glyph) / 2, I.y + (I.size - I.glyph) / 2, I.glyph, I.glyph);
  const S = c.shot;
  ctx.save();
  hsRound(ctx, S.x, S.y, S.w, S.h, S.r);
  ctx.clip();
  ctx.translate(S.x, S.y);
  ctx.scale(S.w / 1920, S.h / 1080);
  homeScreenDraw(ctx, m.home);   /* the screenshot: the home screen, artwork and all */
  ctx.restore();
  ctx.fillStyle = "#fff";
  ctx.textAlign = "left";
  ["Recently created", "New screenshot", m.shot.day].forEach((text, i) => {
    const [y, size, a, track] = c.text.lines[i];
    ctx.globalAlpha = a;
    ctx.font = font(size);
    ctx.letterSpacing = `${track}px`;
    ctx.fillText(text, c.text.x, y);
    if (i === 2) ctx.fillText(m.shot.time, c.text.x + ctx.measureText(text).width + c.text.gap, y);
  });
  ctx.letterSpacing = "0px";

  /* the row of icons, dimmed: none is selected while the card is */
  const r = CC.row, n = m.icons.length;
  m.icons.forEach((icon, i) => {
    const x = ccIconX(i, n);
    if (!icon) {   /* (your profile picture on the console: not one the plugin reads) */
      ctx.globalAlpha = 1;
      ctx.fillStyle = "#5b606b";
      ctx.beginPath(); ctx.arc(x, r.y, r.face / 2, 0, 2 * Math.PI); ctx.fill();
      return;
    }
    ctx.globalAlpha = r.alpha;
    if (!icon.img) return;
    ctx.drawImage(icon.img, x - r.size / 2, r.y - r.size / 2, r.size, r.size);
    if (icon.dot) {
      ctx.globalAlpha = 1;
      ctx.fillStyle = r.dot.color;
      ctx.beginPath(); ctx.arc(x + r.dot.dx, r.y + r.dot.dy, r.dot.r, 0, 2 * Math.PI); ctx.fill();
    }
  });
  ctx.restore();
}
/* A white icon in another colour, drawn at `size`. */
const ccInks = new WeakMap();   /* picture -> "ink|size" -> canvas */
function ccInk(img, size, ink) {
  const key = `${ink}|${size}`;
  let byImg = ccInks.get(img);
  if (!byImg) ccInks.set(img, byImg = new Map());
  if (byImg.has(key)) return byImg.get(key);
  const cv = document.createElement("canvas");
  cv.width = cv.height = size * 2;
  const g = cv.getContext("2d");
  g.drawImage(img, 0, 0, cv.width, cv.height);
  g.globalCompositeOperation = "source-in";
  g.fillStyle = ink;
  g.fillRect(0, 0, cv.width, cv.height);
  byImg.set(key, cv);
  return cv;
}
