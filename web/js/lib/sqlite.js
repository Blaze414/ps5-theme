"use strict";

/* A table of an SQLite database, read here from the file's bytes: the first table whose name starts with `prefix`,
   as { columns, rows } (each row an array in the columns' order). Read-only and bounded: each page is read once, so
   a damaged file ends the walk rather than looping. The format: page 1 holds the schema (each table's root page);
   a table is a b-tree whose leaf pages hold its rows, and a row too large for its page goes on in overflow pages. */
function sqliteTable(bytes, prefix) {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), utf8 = new TextDecoder();
  if (u8.length < 100 || utf8.decode(u8.subarray(0, 16)) !== "SQLite format 3\0") throw new Error("not an SQLite database");
  let size = dv.getUint16(16);
  if (size === 1) size = 65536;
  if (size < 512 || (size & (size - 1))) throw new Error("not an SQLite database");
  const usable = size - u8[20], count = Math.floor(u8.length / size);
  const varint = (b, p) => {
    let v = 0;
    for (let i = 0; i < 8; i++) {
      if (p + i >= b.length) throw new Error("the database is cut short");
      v = v * 128 + (b[p + i] & 0x7f);
      if (b[p + i] < 0x80) return [v, p + i + 1];
    }
    if (p + 8 >= b.length) throw new Error("the database is cut short");
    return [v * 256 + b[p + 8], p + 9];
  };
  const page = n => { if (!(n >= 1 && n <= count)) throw new Error("the database names a page it doesn't have"); return (n - 1) * size; };
  const payload = p => {
    let n, q;
    [n, q] = varint(u8, p);
    [, q] = varint(u8, q);   /* (the row's id) */
    const max = usable - 35;
    if (n <= max) return u8.subarray(q, q + n);
    const min = Math.floor((usable - 12) * 32 / 255) - 23, k = min + (n - min) % (usable - 4), here = k <= max ? k : min;
    const out = new Uint8Array(n);
    out.set(u8.subarray(q, q + here));
    let got = here, next = dv.getUint32(q + here), hops = 0;
    while (got < n && next && hops++ < count) {
      const at = page(next), take = Math.min(n - got, usable - 4);
      out.set(u8.subarray(at + 4, at + 4 + take), got);
      got += take;
      next = dv.getUint32(at);
    }
    return out;
  };
  const record = b => {
    const bv = new DataView(b.buffer, b.byteOffset, b.byteLength), out = [];
    let [end, p] = varint(b, 0), body = end;
    while (p < end) {
      let t;
      [t, p] = varint(b, p);
      const len = t >= 12 ? (t - 12) >> 1 : [0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0][t];
      if (body + len > b.length) throw new Error("the database is cut short");
      let v = null;
      if (t >= 1 && t <= 6) {
        v = 0;
        for (let i = 0; i < len; i++) v = v * 256 + b[body + i];
        if (b[body] & 0x80) v -= 2 ** (8 * len);
      } else if (t === 7) v = bv.getFloat64(body);
      else if (t === 8 || t === 9) v = t - 8;
      else if (t >= 13 && t % 2) v = utf8.decode(b.subarray(body, body + len));
      else if (t >= 12) v = b.slice(body, body + len);
      out.push(v);
      body += len;
    }
    return out;
  };
  const walk = (root, visit) => {
    const seen = new Set(), stack = [root];
    while (stack.length) {
      const n = stack.pop();
      if (seen.has(n)) continue;
      seen.add(n);
      const at = page(n), h = at + (n === 1 ? 100 : 0), type = u8[h], cells = dv.getUint16(h + 3);
      if (type === 0x0d) for (let i = 0; i < cells; i++) visit(record(payload(at + dv.getUint16(h + 8 + 2 * i))));
      else if (type === 0x05) {
        stack.push(dv.getUint32(h + 8));
        for (let i = 0; i < cells; i++) stack.push(dv.getUint32(at + dv.getUint16(h + 12 + 2 * i)));
      } else throw new Error("the database has a page that isn't a table's");
    }
  };
  let root = 0, sql = "";
  walk(1, r => { if (!root && r[0] === "table" && typeof r[1] === "string" && r[1].startsWith(prefix)) { root = r[3]; sql = String(r[4] || ""); } });
  if (!root) throw new Error(`the database has no ${prefix} table`);
  const parts = [];
  let depth = 0, cur = "";
  for (const ch of sql.slice(sql.indexOf("(") + 1, sql.lastIndexOf(")"))) {
    if (ch === "(") depth++;
    if (ch === ")") depth--;
    if (ch === "," && !depth) { parts.push(cur); cur = ""; } else cur += ch;
  }
  parts.push(cur);
  const columns = parts.map(x => x.trim()).filter(x => x && !/^(PRIMARY|UNIQUE|CHECK|FOREIGN|CONSTRAINT)\b/i.test(x))
    .map(x => x.split(/\s+/)[0].replace(/^["`[]|["`\]]$/g, ""));
  const rows = [];
  walk(root, r => rows.push(r));
  return { columns, rows };
}

/* The home screen's two rows as the console orders them, from its app database's icon table: Games (the first hub
   tile, the games most recently used, Game Library) and Media (All Apps, the apps most recently used, App Library),
   as 13.42 shows them. Where each title shows is its dispLocation: 138 a game, 154 Media Gallery (with the games),
   188 an app. */
function homeRowsOf(table) {
  const col = n => table.columns.indexOf(n);
  const [id, name, last, where, shown] = ["titleId", "titleName", "lastAccessIndex", "dispLocation", "visible"].map(col);
  const items = table.rows.map(r => ({ id: r[id], name: r[name] || "", last: +r[last] || 0, where: +r[where], shown: r[shown] !== 0 }))
    .filter(x => typeof x.id === "string" && /^[A-Z]{4}\d{5}$/.test(x.id) && x.shown);
  const recent = places => items.filter(x => places.includes(x.where)).sort((a, b) => b.last - a.last).slice(0, 12).map(x => x.id);
  return {
    games: [items.some(x => x.id === "NPXS40063") ? "NPXS40063" : "NPXS40047", ...recent([138, 154]), "NPXS40071"],
    media: ["NPXS40056", ...recent([188]), "NPXS40139"],
    apps: new Set(items.filter(x => x.where === 188).map(x => x.id)),
    names: new Map(items.map(x => [x.id, x.name])),
  };
}
