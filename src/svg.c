/* SVG pictures: a deliberately small subset, parsed; anything outside it is refused by name.

   The console draws SVG in its interface (the icons in its resource file, and two pictures of its apps). A theme's
   picture goes where the console's own is, so it may use what the console's own use and nothing more. This is not a
   filter that looks for dangerous strings: the document is parsed as XML, and every element, attribute, link and
   style rule must be one this file names. What is accepted, from 13.42's 1,092 SVG pictures:

   - UTF-8 text, an optional <?xml?> declaration (version 1.0, UTF-8), comments; no DOCTYPE, entity declarations,
     CDATA sections or other processing instructions, and no character references but the five XML ones and
     numeric ones (decoded before anything is checked, so none can hide a word);
   - one <svg> root in the SVG namespace, holding only drawing elements (shapes, groups, gradients, masks, clip
     paths, a few filters, <use>, <image>, <title>, <desc>, <style>), no nested <svg>, nothing animated or
     scripted, no text drawn;
   - only presentation and geometry attributes, and the SVG and xlink namespaces, nothing else;
   - links only inside the picture (href="#name" on <use>, url(#name) in a paint, mask, clip path or filter), and a
     picture only as embedded PNG or JPEG data on <image>;
   - style rules of plain class, id or element selectors, each a list of presentation properties with simple values.

   It reads the document once, front to back, with no allocation but one buffer for decoded values, and a fixed
   depth, so a hostile file costs no more than its length (fuzzed: tests/fuzz). */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"


#define SVG_MAX_DEPTH 64
#define SVG_MAX_ATTRS 64

static const char *const elements[] = {
  "svg", "g", "defs", "title", "desc", "style", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
  "linearGradient", "radialGradient", "stop", "clipPath", "mask", "use", "image", "filter", "feColorMatrix",
  "feFlood", "feOffset", "feGaussianBlur", "feComposite", "feBlend",
};

/* Presentation properties: allowed as attributes and in style. */
static const char *const properties[] = {
  "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap",
  "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "opacity", "clip-path",
  "clip-rule", "mask", "filter", "display", "visibility", "color", "stop-color", "stop-opacity", "flood-color",
  "flood-opacity", "color-interpolation-filters", "enable-background", "mix-blend-mode", "isolation",
};

/* Geometry and structure. */
static const char *const attributes[] = {
  "id", "class", "data-name", "version", "x", "y", "width", "height", "viewBox", "preserveAspectRatio",
  "transform", "d", "points", "cx", "cy", "r", "rx", "ry", "fx", "fy", "x1", "y1", "x2", "y2", "offset",
  "gradientUnits", "gradientTransform", "spreadMethod", "clipPathUnits", "maskUnits", "maskContentUnits",
  "filterUnits", "primitiveUnits", "in", "in2", "result", "type", "values", "stdDeviation", "dx", "dy",
  "operator", "k1", "k2", "k3", "k4", "mode",
};

typedef struct {
  const uint8_t *p, *end;
  char *val;        /* a decoded value or text (as long as the document) */
  char *why;
  size_t len;
} svg_t;

typedef struct {
  const uint8_t *s;
  size_t n;
} span_t;


static int
fail(svg_t *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s->why, s->len, fmt, ap);
  va_end(ap);
  return -1;
}

static int
span_is(span_t a, const char *b) {
  return a.n == strlen(b) && !memcmp(a.s, b, a.n);
}

static int
listed(span_t name, const char *const *list, size_t n) {
  for(size_t i = 0; i < n; i++) if(span_is(name, list[i])) return 1;
  return 0;
}
#define LISTED(name, list) listed(name, list, sizeof(list) / sizeof(*list))

/* At most 40 characters of a name, for a message. */
#define SHOW(sp) (int)((sp).n > 40 ? 40 : (sp).n), (const char *)(sp).s

static int
is_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void
skip_space(svg_t *s) {
  while(s->p < s->end && is_space(*s->p)) s->p++;
}

static int
starts(svg_t *s, const char *lit) {
  size_t n = strlen(lit);
  return (size_t)(s->end - s->p) >= n && !memcmp(s->p, lit, n);
}

static int
name_start(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int
name_char(int c) {
  return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':';
}

/* An XML name, ASCII (every name the console's pictures use is). */
static int
read_name(svg_t *s, span_t *out) {
  if(s->p >= s->end || !name_start(*s->p)) return -1;
  out->s = s->p;
  while(s->p < s->end && name_char(*s->p)) s->p++;
  out->n = (size_t)(s->p - out->s);
  return 0;
}


/* Well-formed UTF-8 with no control characters but tab and line ends (and no NUL): checked over the whole
   document first, so everything after can treat it as text. */
static int
text_ok(svg_t *s) {
  const uint8_t *p = s->p, *e = s->end;
  while(p < e) {
    uint8_t c = *p;
    if(c < 0x80) {
      if(c < 0x20 && c != '\t' && c != '\n' && c != '\r') return fail(s, "the SVG holds binary data, not text");
      if(c == 0x7f) return fail(s, "the SVG holds binary data, not text");
      p++;
      continue;
    }
    int n = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
    if(c > 0xf4) n = -1;
    if(n < 0 || e - p <= n) return fail(s, "the SVG is not UTF-8 text");
    uint32_t cp = c & (0x3f >> n);
    for(int i = 1; i <= n; i++) {
      if((p[i] & 0xc0) != 0x80) return fail(s, "the SVG is not UTF-8 text");
      cp = cp << 6 | (p[i] & 0x3f);
    }
    if((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff) ||
       cp == 0xfffe || cp == 0xffff) {
      return fail(s, "the SVG is not UTF-8 text");
    }
    p += n + 1;
  }
  return 0;
}


/* `raw` with its character references decoded into s->val (NUL-terminated); -1 for any reference but the five XML
   ones and numeric ones, or one decoding to a character not allowed in text. */
static int
decode(svg_t *s, span_t raw, size_t *out_len) {
  size_t o = 0;
  for(size_t i = 0; i < raw.n; ) {
    uint8_t c = raw.s[i];
    if(c != '&') {
      s->val[o++] = (char)c;
      i++;
      continue;
    }
    const uint8_t *semi = memchr(raw.s + i, ';', raw.n - i);
    if(!semi || semi - (raw.s + i) > 12) return fail(s, "the SVG has a stray \"&\" (write &amp;)");
    span_t ref = { raw.s + i + 1, (size_t)(semi - (raw.s + i + 1)) };
    uint32_t cp = 0;
    if(span_is(ref, "amp")) cp = '&';
    else if(span_is(ref, "lt")) cp = '<';
    else if(span_is(ref, "gt")) cp = '>';
    else if(span_is(ref, "quot")) cp = '"';
    else if(span_is(ref, "apos")) cp = '\'';
    else if(ref.n >= 2 && ref.s[0] == '#') {
      int hex = ref.s[1] == 'x';
      size_t k = hex ? 2 : 1;
      if(k >= ref.n) return fail(s, "the SVG has a broken character reference");
      for(; k < ref.n; k++) {
        int d = ref.s[k];
        int v = d >= '0' && d <= '9' ? d - '0' : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10 :
                hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
        if(v < 0 || cp > 0x10ffff) return fail(s, "the SVG has a broken character reference");
        cp = cp * (hex ? 16 : 10) + (uint32_t)v;
      }
      if(cp > 0x10ffff || (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') || cp == 0x7f ||
         (cp >= 0xd800 && cp <= 0xdfff) || cp == 0xfffe || cp == 0xffff) {
        return fail(s, "the SVG has a character reference to a character not allowed in text");
      }
    } else {
      return fail(s, "the SVG uses the entity &%.*s;, which isn't allowed", SHOW(ref));
    }
    if(cp < 0x80) s->val[o++] = (char)cp;
    else if(cp < 0x800) { s->val[o++] = (char)(0xc0 | cp >> 6); s->val[o++] = (char)(0x80 | (cp & 0x3f)); }
    else if(cp < 0x10000) {
      s->val[o++] = (char)(0xe0 | cp >> 12); s->val[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      s->val[o++] = (char)(0x80 | (cp & 0x3f));
    } else {
      s->val[o++] = (char)(0xf0 | cp >> 18); s->val[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
      s->val[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); s->val[o++] = (char)(0x80 | (cp & 0x3f));
    }
    i = (size_t)(semi - raw.s) + 1;
  }
  s->val[o] = 0;
  if(out_len) *out_len = o;
  return 0;
}


static int
lower(int c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* A character of a name linked to (#name): ASCII letters, digits and _-.: , or any non-ASCII character (the document
   is already known to be UTF-8; some of the console's own pictures name their gradients in Japanese). */
static int
id_char(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         c == '.' || c == ':' || c >= 0x80;
}

/* "#name": a link to something inside the picture. Returns the characters taken, 0 if it isn't one. */
static size_t
local_ref(const char *v) {
  if(v[0] != '#' || !id_char((unsigned char)v[1])) return 0;
  size_t n = 1;
  while(id_char((unsigned char)v[n])) n++;
  return n;
}

/* Every url( in `v` (any case) is url(#name), spaces allowed inside: only links inside the picture. */
static int
urls_local(svg_t *s, const char *v, const char *where) {
  for(const char *p = v; *p; p++) {
    if(lower(p[0]) != 'u' || lower(p[1]) != 'r' || lower(p[2]) != 'l' || p[3] != '(') continue;
    const char *q = p + 4;
    while(is_space(*q)) q++;
    size_t n = local_ref(q);
    q += n;
    while(is_space(*q)) q++;
    if(!n || *q != ')') {
      return fail(s, "the SVG's %s links outside the picture (url(...)); only url(#name) is allowed", where);
    }
  }
  return 0;
}

/* A property's value in style: plain words, numbers, colours, lists, and the functions rgb(), rgba(), hsl(),
   hsla() and url(#name), nothing else (no quotes, escapes, comments, @ rules or !important). */
static int
css_value_ok(svg_t *s, const char *v, size_t n, const char *prop) {
  for(size_t i = 0; i < n; i++) {
    char c = v[i];
    if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || is_space(c) ||
         c == '#' || c == '%' || c == '.' || c == ',' || c == '-' || c == '+' || c == '(' || c == ')' || c == '_')) {
      return fail(s, "the SVG's style for %.40s has \"%c\", which isn't allowed in a value", prop, c);
    }
    if(c == '(') {
      size_t k = i;
      while(k > 0 && ((v[k - 1] >= 'a' && v[k - 1] <= 'z') || (v[k - 1] >= 'A' && v[k - 1] <= 'Z'))) k--;
      span_t fn = { (const uint8_t *)v + k, i - k };
      char f[8] = "";
      if(fn.n < sizeof(f)) for(size_t j = 0; j < fn.n; j++) f[j] = (char)lower(fn.s[j]);
      if(strcmp(f, "url") && strcmp(f, "rgb") && strcmp(f, "rgba") && strcmp(f, "hsl") && strcmp(f, "hsla")) {
        return fail(s, "the SVG's style for %.40s uses %.*s(), which isn't allowed", prop, SHOW(fn));
      }
    }
  }
  char copy[256];
  if(n >= sizeof(copy)) return fail(s, "the SVG's style for %.40s is too long", prop);
  memcpy(copy, v, n);
  copy[n] = 0;
  return urls_local(s, copy, "style");
}

/* "prop: value; prop: value" with every prop a presentation property. */
static int
css_declarations(svg_t *s, const char *v, size_t n) {
  size_t i = 0;
  while(i < n) {
    while(i < n && (is_space(v[i]) || v[i] == ';')) i++;
    if(i >= n) break;
    size_t a = i;
    while(i < n && ((v[i] >= 'a' && v[i] <= 'z') || v[i] == '-')) i++;
    span_t prop = { (const uint8_t *)v + a, i - a };
    while(i < n && is_space(v[i])) i++;
    if(!prop.n || i >= n || v[i] != ':') return fail(s, "the SVG has a style the console's pictures don't use");
    if(!LISTED(prop, properties)) {
      return fail(s, "the SVG's style sets %.*s, which isn't allowed (only how shapes are painted)", SHOW(prop));
    }
    i++;
    size_t b = i;
    while(i < n && v[i] != ';') i++;
    char name[40];
    snprintf(name, sizeof(name), "%.*s", SHOW(prop));
    if(css_value_ok(s, v + b, i - b, name)) return -1;
  }
  return 0;
}

/* A <style> element's text: rules "selectors { declarations }", each selector a plain class, id or element. */
static int
css_rules(svg_t *s, const char *v, size_t n) {
  size_t i = 0;
  for(;;) {
    while(i < n && is_space(v[i])) i++;
    if(i >= n) return 0;
    for(;;) {   /* the selectors */
      if(v[i] == '.' || v[i] == '#') i++;
      size_t a = i;
      while(i < n && ((v[i] >= 'a' && v[i] <= 'z') || (v[i] >= 'A' && v[i] <= 'Z') || (v[i] >= '0' && v[i] <= '9') ||
                      v[i] == '-' || v[i] == '_')) i++;
      if(i == a) return fail(s, "the SVG's <style> has a rule the console's pictures don't use");
      while(i < n && is_space(v[i])) i++;
      if(i < n && v[i] == ',') {
        i++;
        while(i < n && is_space(v[i])) i++;
        continue;
      }
      break;
    }
    if(i >= n || v[i] != '{') return fail(s, "the SVG's <style> has a rule the console's pictures don't use");
    const char *close = memchr(v + i, '}', n - i);
    if(!close) return fail(s, "the SVG's <style> has a rule that isn't closed");
    if(css_declarations(s, v + i + 1, (size_t)(close - (v + i + 1)))) return -1;
    i = (size_t)(close - v) + 1;
  }
}

/* base64, nothing else. */
static int
base64_only(const char *v) {
  if(!*v) return 0;
  for(; *v; v++) {
    char c = *v;
    if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
         c == '=')) {
      return 0;
    }
  }
  return 1;
}

/* One attribute of `el`, its value decoded. */
static int
attribute_ok(svg_t *s, span_t el, span_t name, const char *v, size_t vn) {
  if(span_is(name, "xmlns")) {
    if(strcmp(v, "http://www.w3.org/2000/svg")) return fail(s, "the SVG puts <%.*s> in another namespace", SHOW(el));
    return 0;
  }
  if(span_is(name, "xmlns:xlink")) {
    if(strcmp(v, "http://www.w3.org/1999/xlink")) return fail(s, "the SVG redefines the xlink namespace");
    return 0;
  }
  if(span_is(name, "xml:space")) {
    if(strcmp(v, "preserve") && strcmp(v, "default")) return fail(s, "the SVG has xml:space=\"%.20s\"", v);
    return 0;
  }
  if(span_is(name, "href") || span_is(name, "xlink:href")) {
    if(span_is(el, "use") && local_ref(v) == vn) return 0;
    if(span_is(el, "image") && (!strncmp(v, "data:image/png;base64,", 22) || !strncmp(v, "data:image/jpeg;base64,", 23)) &&
       base64_only(strchr(v, ',') + 1)) {
      return 0;
    }
    return fail(s, "the SVG's <%.*s> links to \"%.40s\"; only links inside the picture (#name), and PNG or JPEG "
                "pictures embedded in it, are allowed", SHOW(el), v);
  }
  if(span_is(name, "style")) return css_declarations(s, v, vn);
  if(span_is(name, "type") && span_is(el, "style") && strcmp(v, "text/css")) {
    return fail(s, "the SVG's <style> is of type \"%.20s\", not CSS", v);
  }
  if(!LISTED(name, properties) && !LISTED(name, attributes)) {
    if(name.n > 2 && lower(name.s[0]) == 'o' && lower(name.s[1]) == 'n') {
      return fail(s, "the SVG has an event handler (%.*s), which isn't allowed", SHOW(name));
    }
    return fail(s, "the SVG's <%.*s> has the attribute %.*s, which isn't allowed", SHOW(el), SHOW(name));
  }
  return urls_local(s, v, "paint or mask");
}


/* The attributes of a start tag, up to its end ("/>" sets *empty). */
static int
attributes_of(svg_t *s, span_t el, int *empty, int *has_ns) {
  span_t seen[SVG_MAX_ATTRS];
  int nseen = 0;
  for(;;) {
    const uint8_t *before = s->p;
    skip_space(s);
    if(s->p >= s->end) return fail(s, "the SVG ends inside <%.*s>", SHOW(el));
    if(*s->p == '>') { s->p++; *empty = 0; return 0; }
    if(starts(s, "/>")) { s->p += 2; *empty = 1; return 0; }
    span_t name;
    if(s->p == before || read_name(s, &name)) return fail(s, "the SVG isn't well-formed XML (in <%.*s>)", SHOW(el));
    for(int i = 0; i < nseen; i++) {
      if(seen[i].n == name.n && !memcmp(seen[i].s, name.s, name.n)) {
        return fail(s, "the SVG's <%.*s> has %.*s twice", SHOW(el), SHOW(name));
      }
    }
    if(nseen == SVG_MAX_ATTRS) return fail(s, "the SVG's <%.*s> has too many attributes", SHOW(el));
    seen[nseen++] = name;
    skip_space(s);
    if(s->p >= s->end || *s->p != '=') return fail(s, "the SVG isn't well-formed XML (in <%.*s>)", SHOW(el));
    s->p++;
    skip_space(s);
    if(s->p >= s->end || (*s->p != '"' && *s->p != '\'')) {
      return fail(s, "the SVG isn't well-formed XML (in <%.*s>)", SHOW(el));
    }
    uint8_t q = *s->p++;
    const uint8_t *close = memchr(s->p, q, (size_t)(s->end - s->p));
    if(!close) return fail(s, "the SVG ends inside <%.*s>", SHOW(el));
    span_t raw = { s->p, (size_t)(close - s->p) };
    if(memchr(raw.s, '<', raw.n)) return fail(s, "the SVG isn't well-formed XML (\"<\" in a value)");
    s->p = close + 1;
    size_t vn;
    if(decode(s, raw, &vn)) return -1;
    if(strlen(s->val) != vn) return fail(s, "the SVG holds binary data, not text");
    if(attribute_ok(s, el, name, s->val, vn)) return -1;
    if(span_is(name, "xmlns")) *has_ns = 1;
  }
}


/* <?xml version="1.0" encoding="UTF-8" standalone="..."?>, at the very start. */
static int
declaration(svg_t *s) {
  s->p += 5;
  for(;;) {
    skip_space(s);
    if(starts(s, "?>")) { s->p += 2; return 0; }
    span_t name;
    if(read_name(s, &name)) return fail(s, "the SVG's <?xml?> declaration isn't one this tool reads");
    skip_space(s);
    if(s->p >= s->end || *s->p != '=') return fail(s, "the SVG's <?xml?> declaration isn't one this tool reads");
    s->p++;
    skip_space(s);
    if(s->p >= s->end || (*s->p != '"' && *s->p != '\'')) return fail(s, "the SVG's <?xml?> declaration isn't one this tool reads");
    uint8_t q = *s->p++;
    const uint8_t *close = memchr(s->p, q, (size_t)(s->end - s->p));
    if(!close || close - s->p > 16) return fail(s, "the SVG's <?xml?> declaration isn't one this tool reads");
    char v[17];
    size_t n = (size_t)(close - s->p);
    for(size_t i = 0; i < n; i++) v[i] = (char)lower(s->p[i]);
    v[n] = 0;
    s->p = close + 1;
    if(span_is(name, "version") ? strcmp(v, "1.0") :
       span_is(name, "encoding") ? strcmp(v, "utf-8") :
       span_is(name, "standalone") ? strcmp(v, "yes") && strcmp(v, "no") : 1) {
      return fail(s, "the SVG's <?xml?> declaration says %.*s=\"%s\"; only UTF-8 XML 1.0 is read", SHOW(name), v);
    }
  }
}


/* Text between tags, decoded: only whitespace, but in <title>, <desc> (any text) and <style> (style rules). */
static int
text_node(svg_t *s, span_t raw, span_t parent, int in_root) {
  size_t vn;
  if(decode(s, raw, &vn)) return -1;
  int blank = 1;
  for(size_t i = 0; i < vn && blank; i++) blank = is_space(s->val[i]);
  if(blank) return 0;
  if(!in_root) return fail(s, "the SVG has text outside its <svg>");
  if(span_is(parent, "title") || span_is(parent, "desc")) return 0;
  if(span_is(parent, "style")) return css_rules(s, s->val, vn);
  return fail(s, "the SVG has text in <%.*s>; pictures on the console draw no text", SHOW(parent));
}


static int
parse(svg_t *s) {
  span_t stack[SVG_MAX_DEPTH];
  int depth = 0, root_done = 0;

  if(starts(s, "\xef\xbb\xbf")) s->p += 3;   /* a UTF-8 byte order mark */
  if(starts(s, "<?xml") && s->end - s->p > 5 && is_space(s->p[5]) && declaration(s)) return -1;
  for(;;) {
    const uint8_t *t = s->p;
    while(s->p < s->end && *s->p != '<') s->p++;
    if(s->p > t) {
      span_t parent = depth ? stack[depth - 1] : (span_t){ (const uint8_t *)"", 0 };
      if(text_node(s, (span_t){ t, (size_t)(s->p - t) }, parent, depth > 0)) return -1;
    }
    if(s->p >= s->end) break;
    if(starts(s, "<!--")) {
      const uint8_t *e = s->p + 4;
      for(;; e++) {
        if(s->end - e < 3) return fail(s, "the SVG has a comment that isn't closed");
        if(e[0] == '-' && e[1] == '-') {
          if(e[2] != '>') return fail(s, "the SVG isn't well-formed XML (\"--\" in a comment)");
          break;
        }
      }
      s->p = e + 3;
      continue;
    }
    if(starts(s, "<!")) return fail(s, "the SVG declares a DOCTYPE, entities or CDATA, which aren't allowed");
    if(starts(s, "<?")) return fail(s, "the SVG has a processing instruction, which isn't allowed");
    if(starts(s, "</")) {
      s->p += 2;
      span_t name;
      if(read_name(s, &name)) return fail(s, "the SVG isn't well-formed XML (a broken end tag)");
      skip_space(s);
      if(s->p >= s->end || *s->p != '>') return fail(s, "the SVG isn't well-formed XML (a broken end tag)");
      s->p++;
      if(!depth || stack[depth - 1].n != name.n || memcmp(stack[depth - 1].s, name.s, name.n)) {
        return fail(s, "the SVG isn't well-formed XML (</%.*s> closes nothing open)", SHOW(name));
      }
      if(!--depth) root_done = 1;
      continue;
    }
    s->p++;
    span_t name;
    if(read_name(s, &name)) return fail(s, "the SVG isn't well-formed XML (a broken tag)");
    if(root_done) return fail(s, "the SVG has more after its </svg>");
    if(!LISTED(name, elements)) {
      return fail(s, "the SVG has <%.*s>, which isn't allowed (only shapes, groups, gradients, masks and embedded "
                  "pictures)", SHOW(name));
    }
    if(!depth && !span_is(name, "svg")) return fail(s, "not an SVG picture: it starts with <%.*s>", SHOW(name));
    if(depth && span_is(name, "svg")) return fail(s, "the SVG has an <svg> inside it, which isn't allowed");
    int empty = 0, has_ns = 0;
    if(attributes_of(s, name, &empty, &has_ns)) return -1;
    if(!depth && !has_ns) return fail(s, "not an SVG picture: its <svg> isn't in the SVG namespace");
    if(empty) {
      if(!depth) root_done = 1;
      continue;
    }
    if(depth == SVG_MAX_DEPTH) return fail(s, "the SVG nests deeper than any picture of the console's");
    stack[depth++] = name;
  }
  if(depth) return fail(s, "the SVG ends before its <%.*s> is closed", SHOW(stack[depth - 1]));
  if(!root_done) return fail(s, "not an SVG picture");
  return 0;
}


/* SVG text in memory (a file, or a picture inside a resource file): 0 if it is in the subset above, else -1 with
   what is outside it in `why`. */
int
svg_check_mem(const uint8_t *data, size_t n, char *why, size_t len) {
  svg_t s = { data, data + n, NULL, why, len };
  if(!n) return fail(&s, "not an SVG picture: the file is empty");
  if(text_ok(&s)) return -1;
  s.val = malloc(n + 1);
  if(!s.val) return fail(&s, "cannot read the SVG");
  int rc = parse(&s);
  free(s.val);
  return rc;
}
