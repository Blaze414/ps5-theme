"use strict";

/* What an entry of an interface file is, as Mix and match sorts them: "picture", "sound", "color", "shape", or ""
   for anything a theme can't change. */
function rcoEntryKind(f) {
  if (!f) return "";
  if (f.elem === "texture" && (f.type === "texture/png" || f.type === "texture/svg")) return "picture";
  if (f.elem === "sounddata" && f.type === "sound/vag" && !/_vibration$/.test(f.id)) return "sound";
  if (f.elem === "file" && f.type === "color") return "color";
  if (f.elem === "file" && f.type === "shape") return "shape";
  return "";
}

/* A mixed interface file's entries: each one changed by any of the themes in `changes` (theme id -> its changed
   entries in this file, "element\tattribute" -> bytes), taken from the theme `sourceOf(kind, key)` names for it
   ("" for the console's own). A picture is chosen whole, with its size variants (key "<path>|<element>"); any other
   entry by itself ("<path>|<element>\t<attribute>"). */
function rcoMixEntries(r, changes, sourceOf, path) {
  const files = new Map(r.files.map(f => [`${f.el}\t${f.attr}`, f]));
  const keys = new Set();
  for (const m of changes.values()) for (const k of m.keys()) keys.add(k);
  const out = new Map();
  for (const k of keys) {
    const f = files.get(k), kind = rcoEntryKind(f);
    if (!kind) continue;
    const src = sourceOf(kind, `${path}|${kind === "picture" ? f.el : k}`), m = src && changes.get(src);
    if (m && m.has(k)) out.set(k, m.get(k));
  }
  return out;
}
