// value.c - Lingo's values in C: their arena, making, copying and comparing them, and JSON in and out.
//
// 1. Arena       memory for one program, freed all at once
// 2. Values      making, reading, copying, comparing, their kinds, and headers by lower case name
// 3. UTF-8       counting characters, so text is indexed by character, not by byte, and checking bytes are text
// 4. Buffers     growing text
// 5. Numbers     the shortest digits that read back exactly, laid out as JavaScript does
// 6. JSON out    a value as JSON, or as text
// 7. JSON in     JSON text as a value
#include "lingo.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////// 1. ARENA //////////

typedef struct chunk { struct chunk *next; size_t used, size; _Alignas(16) char data[]; } chunk;
struct arena { chunk *head; size_t total; };

#define CHUNK 65536

arena *arena_new(void) {
  arena *a = calloc(1, sizeof *a);
  if (!a) abort();
  return a;
}

void arena_free(arena *a) {
  if (!a) return;
  for (chunk *c = a->head, *n; c; c = n) { n = c->next; free(c); }
  free(a);
}

// Zeroed memory. Anything big gets a chunk of its own, behind the current one, so the current one keeps filling.
void *arena_get(arena *a, size_t size) {
  size = (size + 15) & ~(size_t)15;
  chunk *c = a->head;
  if (size > CHUNK / 4) {
    chunk *own = malloc(sizeof(chunk) + size);
    if (!own) abort();
    own->size = own->used = size;
    if (c) { own->next = c->next; c->next = own; } else { own->next = NULL; a->head = own; }
    a->total += size;
    memset(own->data, 0, size);
    return own->data;
  }
  if (!c || c->used + size > c->size) {
    c = malloc(sizeof(chunk) + CHUNK);
    if (!c) abort();
    c->size = CHUNK;
    c->used = 0;
    c->next = a->head;
    a->head = c;
  }
  void *p = c->data + c->used;
  c->used += size;
  a->total += size;
  memset(p, 0, size);
  return p;
}

size_t arena_used(arena *a) { return a->total; }

////////// 2. VALUES //////////

val V_NOTHING_VAL = { .k = V_NOTHING }, V_TRUE_VAL = { .k = V_BOOL, .b = 1 }, V_FALSE_VAL = { .k = V_BOOL, .b = 0 };

val *v_num(arena *a, double n) {
  val *v = arena_get(a, sizeof *v);
  v->k = V_NUM;
  v->n = n;
  return v;
}

val *v_text(arena *a, const char *s, size_t len) {
  val *v = arena_get(a, sizeof *v);
  v->k = V_TEXT;
  v->t.s = arena_get(a, len + 1);
  if (len) memcpy(v->t.s, s, len);
  v->t.len = len;
  return v;
}

val *v_cstr(arena *a, const char *s) { return v_text(a, s, strlen(s)); }

val *v_list(arena *a) {
  val *v = arena_get(a, sizeof *v);
  v->k = V_LIST;
  return v;
}

val *v_rec(arena *a) {
  val *v = arena_get(a, sizeof *v);
  v->k = V_REC;
  return v;
}

void list_push(arena *a, val *list, val *x) {
  if (list->l.n == list->l.cap) {
    size_t cap = list->l.cap ? list->l.cap * 2 : 8;
    val **v = arena_get(a, cap * sizeof *v);
    if (list->l.n) memcpy(v, list->l.v, list->l.n * sizeof *v);
    list->l.v = v;
    list->l.cap = cap;
  }
  list->l.v[list->l.n++] = x;
}

val *rec_get(val *r, const char *key) {
  for (size_t i = 0; r && r->k == V_REC && i < r->r.n; i++) if (!strcmp(r->r.key[i], key)) return r->r.v[i];
  return NULL;
}

void rec_set(arena *a, val *r, const char *key, val *x) {
  for (size_t i = 0; i < r->r.n; i++) if (!strcmp(r->r.key[i], key)) { r->r.v[i] = x; return; }
  if (r->r.n == r->r.cap) {
    size_t cap = r->r.cap ? r->r.cap * 2 : 8;
    char **k = arena_get(a, cap * sizeof *k);
    val **v = arena_get(a, cap * sizeof *v);
    if (r->r.n) { memcpy(k, r->r.key, r->r.n * sizeof *k); memcpy(v, r->r.v, r->r.n * sizeof *v); }
    r->r.key = k;
    r->r.v = v;
    r->r.cap = cap;
  }
  size_t len = strlen(key);
  char *own = arena_get(a, len + 1);
  memcpy(own, key, len);
  r->r.key[r->r.n] = own;
  r->r.v[r->r.n++] = x;
}

const char *text_or(val *r, const char *key, const char *fallback) {
  val *v = rec_get(r, key);
  return v && v->k == V_TEXT ? v->t.s : fallback;
}

void header_add(arena *a, val *headers, const char *name, size_t n, const char *value, size_t vn) {
  buf low = { 0 }, both = { 0 };
  for (size_t i = 0; i < n; i++) buf_chr(&low, (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]));
  val *had = rec_get(headers, low.s ? low.s : "");
  if (had) { buf_add(&both, had->t.s, had->t.len); buf_str(&both, ", "); }
  buf_add(&both, value, vn);
  rec_set(a, headers, low.s ? low.s : "", v_text(a, both.s ? both.s : "", both.n));
  buf_free(&low), buf_free(&both);
}

val *header_lines(arena *a, const char *s) {
  val *out = v_rec(a);
  for (const char *end; *s; s = *end ? end + 2 : end) {
    if (!(end = strstr(s, "\r\n"))) end = s + strlen(s);
    const char *colon = memchr(s, ':', (size_t)(end - s)), *v = colon, *stop = end;
    if (!colon || colon == s) continue;   // the status line
    while (++v < end && (*v == ' ' || *v == '\t')) {}
    while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
    header_add(a, out, s, (size_t)(colon - s), v, (size_t)(stop - v));
  }
  return out;
}

// Text and numbers never change once made, so only lists and records need copying.
val *v_copy(arena *a, val *x) {
  if (x->k == V_LIST) {
    val *c = v_list(a);
    for (size_t i = 0; i < x->l.n; i++) list_push(a, c, v_copy(a, x->l.v[i]));
    return c;
  }
  if (x->k == V_REC) {
    val *c = v_rec(a);
    for (size_t i = 0; i < x->r.n; i++) rec_set(a, c, x->r.key[i], v_copy(a, x->r.v[i]));
    return c;
  }
  return x;
}

int v_same(val *a, val *b) {
  if (a == b) return 1;
  if (a->k != b->k) return 0;
  switch (a->k) {
    case V_NOTHING: return 1;
    case V_BOOL: return a->b == b->b;
    case V_NUM: return a->n == b->n;
    case V_TEXT: return a->t.len == b->t.len && !memcmp(a->t.s, b->t.s, a->t.len);
    case V_LIST:
      if (a->l.n != b->l.n) return 0;
      for (size_t i = 0; i < a->l.n; i++) if (!v_same(a->l.v[i], b->l.v[i])) return 0;
      return 1;
    case V_REC:
      if (a->r.n != b->r.n) return 0;
      for (size_t i = 0; i < a->r.n; i++) {
        val *o = rec_get(b, a->r.key[i]);
        if (!o || !v_same(a->r.v[i], o)) return 0;
      }
      return 1;
  }
  return 0;
}

int truthy(val *x) { return x->k != V_NOTHING && !(x->k == V_BOOL && !x->b); }

const char *kind_name(val *x) {
  static const char *names[] = { "nothing", "true/false", "number", "text", "list", "record" };
  return names[x->k];
}

////////// 3. UTF-8 //////////

static int cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t utf8_count(const char *s, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++) if (!cont((unsigned char)s[i])) n++;
  return n;
}

// The byte where character index starts, or len when it is past the end.
size_t utf8_offset(const char *s, size_t len, size_t index) {
  size_t i = 0;
  for (size_t seen = 0; i < len; i++) {
    if (cont((unsigned char)s[i])) continue;
    if (seen++ == index) return i;
  }
  return len;
}

// 1 when every character is whole, 0 for bytes that aren't text.
int utf8_ok(const char *s, size_t len) {
  for (size_t i = 0; i < len;) {
    unsigned char c = (unsigned char)s[i];
    size_t w = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!w || i + w > len) return 0;
    for (size_t k = 1; k < w; k++) if (!cont((unsigned char)s[i + k])) return 0;
    i += w;
  }
  return 1;
}

////////// 4. BUFFERS //////////

void buf_add(buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + n + 1) cap *= 2;
    char *p = realloc(b->s, cap);
    if (!p) abort();
    b->s = p;
    b->cap = cap;
  }
  if (n) memcpy(b->s + b->n, s, n);
  b->n += n;
  b->s[b->n] = 0;
}

void buf_str(buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_chr(buf *b, char c) { buf_add(b, &c, 1); }

void buf_fmt(buf *b, const char *fmt, ...) {
  char small[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(small, sizeof small, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof small) { buf_add(b, small, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  if (!big) abort();
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  buf_add(b, big, (size_t)n);
  free(big);
}

void buf_free(buf *b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }

////////// 5. NUMBERS //////////

/* JavaScript's rule: the fewest digits that read back as the same number, then laid out plainly between 1e-7 and 1e21,
   and with an exponent outside that. 3 is "3", 0.1 is "0.1", 1e21 is "1e+21". */
void buf_num(buf *b, double n) {
  if (isnan(n)) { buf_str(b, "NaN"); return; }
  if (isinf(n)) { buf_str(b, n < 0 ? "-Infinity" : "Infinity"); return; }
  if (n == 0) { buf_chr(b, '0'); return; }
  char tmp[40];
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, n);
    if (strtod(tmp, NULL) == n) break;
  }
  // tmp is like -1.2345e+05: the sign, the digits without the point, and the exponent.
  const char *s = tmp;
  if (*s == '-') { buf_chr(b, '-'); s++; }
  char digits[24];
  int k = 0;
  for (; *s && *s != 'e'; s++) if (*s != '.') digits[k++] = *s;
  while (k > 1 && digits[k - 1] == '0') k--;
  digits[k] = 0;
  int point = atoi(s + 1) + 1;   // where the point goes, counted from the first digit
  if (k <= point && point <= 21) {
    buf_add(b, digits, (size_t)k);
    for (int i = k; i < point; i++) buf_chr(b, '0');
  } else if (0 < point && point <= 21) {
    buf_add(b, digits, (size_t)point);
    buf_chr(b, '.');
    buf_add(b, digits + point, (size_t)(k - point));
  } else if (-6 < point && point <= 0) {
    buf_str(b, "0.");
    for (int i = 0; i < -point; i++) buf_chr(b, '0');
    buf_add(b, digits, (size_t)k);
  } else {
    buf_chr(b, digits[0]);
    if (k > 1) { buf_chr(b, '.'); buf_add(b, digits + 1, (size_t)(k - 1)); }
    buf_fmt(b, "e%c%d", point - 1 < 0 ? '-' : '+', abs(point - 1));
  }
}

////////// 6. JSON OUT //////////

void buf_json_str(buf *b, const char *s, size_t n) {
  buf_chr(b, '"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': buf_str(b, "\\\""); break;
      case '\\': buf_str(b, "\\\\"); break;
      case '\n': buf_str(b, "\\n"); break;
      case '\r': buf_str(b, "\\r"); break;
      case '\t': buf_str(b, "\\t"); break;
      case '\b': buf_str(b, "\\b"); break;
      case '\f': buf_str(b, "\\f"); break;
      default:
        if (c < 0x20) buf_fmt(b, "\\u%04x", c);
        else buf_chr(b, (char)c);
    }
  }
  buf_chr(b, '"');
}

void buf_json(buf *b, val *x) {
  switch (x->k) {
    case V_NOTHING: buf_str(b, "null"); break;
    case V_BOOL: buf_str(b, x->b ? "true" : "false"); break;
    case V_NUM:
      if (isnan(x->n) || isinf(x->n)) buf_str(b, "null");
      else buf_num(b, x->n);
      break;
    case V_TEXT: buf_json_str(b, x->t.s, x->t.len); break;
    case V_LIST:
      buf_chr(b, '[');
      for (size_t i = 0; i < x->l.n; i++) { if (i) buf_chr(b, ','); buf_json(b, x->l.v[i]); }
      buf_chr(b, ']');
      break;
    case V_REC:
      buf_chr(b, '{');
      for (size_t i = 0; i < x->r.n; i++) {
        if (i) buf_chr(b, ',');
        buf_json_str(b, x->r.key[i], strlen(x->r.key[i]));
        buf_chr(b, ':');
        buf_json(b, x->r.v[i]);
      }
      buf_chr(b, '}');
      break;
  }
}

void buf_text(buf *b, val *x) {
  switch (x->k) {
    case V_NOTHING: break;
    case V_BOOL: buf_str(b, x->b ? "true" : "false"); break;
    case V_NUM: buf_num(b, x->n); break;
    case V_TEXT: buf_add(b, x->t.s, x->t.len); break;
    default: buf_json(b, x);
  }
}

////////// 7. JSON IN //////////

typedef struct { arena *a; const char *s, *end; const char *error; int depth; } reader;

static void skip(reader *r) { while (r->s < r->end && (*r->s == ' ' || *r->s == '\n' || *r->s == '\r' || *r->s == '\t')) r->s++; }
static val *bad(reader *r, const char *why) { if (!r->error) r->error = why; return NULL; }

static void put_utf8(buf *b, uint32_t c) {
  char o[4];
  if (c < 0x80) { o[0] = (char)c; buf_add(b, o, 1); }
  else if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); buf_add(b, o, 2); }
  else if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); buf_add(b, o, 3); }
  else { o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63)); buf_add(b, o, 4); }
}

static int hex4(reader *r, uint32_t *out) {
  if (r->end - r->s < 4) return 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = r->s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  r->s += 4;
  *out = v;
  return 1;
}

// A JSON string, with its escapes, as plain UTF-8 in b. r->s is just past the opening quote.
static int string_in(reader *r, buf *b) {
  while (r->s < r->end) {
    char c = *r->s++;
    if (c == '"') return 1;
    if (c != '\\') { buf_chr(b, c); continue; }
    if (r->s >= r->end) return 0;
    char e = *r->s++;
    switch (e) {
      case '"': case '\\': case '/': buf_chr(b, e); break;
      case 'n': buf_chr(b, '\n'); break;
      case 't': buf_chr(b, '\t'); break;
      case 'r': buf_chr(b, '\r'); break;
      case 'b': buf_chr(b, '\b'); break;
      case 'f': buf_chr(b, '\f'); break;
      case 'u': {
        uint32_t c1, c2;
        if (!hex4(r, &c1)) return 0;
        if (c1 >= 0xD800 && c1 < 0xDC00 && r->end - r->s >= 6 && r->s[0] == '\\' && r->s[1] == 'u') {
          r->s += 2;
          if (!hex4(r, &c2)) return 0;
          c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
        }
        put_utf8(b, c1);
        break;
      }
      default: return 0;
    }
  }
  return 0;
}

static val *value_in(reader *r) {
  skip(r);
  if (r->s >= r->end) return bad(r, "the JSON ends too soon");
  if (++r->depth > 500) return bad(r, "the JSON is nested too deep");
  val *out = NULL;
  char c = *r->s;
  if (c == '{') {
    r->s++;
    out = v_rec(r->a);
    skip(r);
    if (r->s < r->end && *r->s == '}') { r->s++; r->depth--; return out; }
    for (;;) {
      skip(r);
      if (r->s >= r->end || *r->s != '"') return bad(r, "a part name must be text");
      r->s++;
      buf key = { 0 };
      if (!string_in(r, &key)) { buf_free(&key); return bad(r, "a part name never ends"); }
      skip(r);
      if (r->s >= r->end || *r->s != ':') { buf_free(&key); return bad(r, "a : is missing"); }
      r->s++;
      val *v = value_in(r);
      if (!v) { buf_free(&key); return NULL; }
      rec_set(r->a, out, key.s ? key.s : "", v);
      buf_free(&key);
      skip(r);
      if (r->s < r->end && *r->s == ',') { r->s++; continue; }
      if (r->s < r->end && *r->s == '}') { r->s++; break; }
      return bad(r, "a , or } is missing");
    }
  } else if (c == '[') {
    r->s++;
    out = v_list(r->a);
    skip(r);
    if (r->s < r->end && *r->s == ']') { r->s++; r->depth--; return out; }
    for (;;) {
      val *v = value_in(r);
      if (!v) return NULL;
      list_push(r->a, out, v);
      skip(r);
      if (r->s < r->end && *r->s == ',') { r->s++; continue; }
      if (r->s < r->end && *r->s == ']') { r->s++; break; }
      return bad(r, "a , or ] is missing");
    }
  } else if (c == '"') {
    r->s++;
    buf t = { 0 };
    if (!string_in(r, &t)) { buf_free(&t); return bad(r, "text never ends"); }
    out = v_text(r->a, t.s ? t.s : "", t.n);
    buf_free(&t);
  } else if (r->end - r->s >= 4 && !memcmp(r->s, "true", 4)) { r->s += 4; out = VBOOL(1); }
  else if (r->end - r->s >= 5 && !memcmp(r->s, "false", 5)) { r->s += 5; out = VBOOL(0); }
  else if (r->end - r->s >= 4 && !memcmp(r->s, "null", 4)) { r->s += 4; out = NOTHING; }
  else if (c == '-' || (c >= '0' && c <= '9')) {
    char tmp[64];
    size_t n = 0;
    while (r->s < r->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", *r->s)) tmp[n++] = *r->s++;
    tmp[n] = 0;
    char *stop;
    double d = strtod(tmp, &stop);
    if (stop != tmp + n) return bad(r, "a number is not written right");
    out = v_num(r->a, d);
  } else return bad(r, "something that isn't JSON");
  r->depth--;
  return out;
}

val *json_parse(arena *a, const char *s, size_t n, const char **error) {
  reader r = { a, s, s + n, NULL, 0 };
  val *v = value_in(&r);
  skip(&r);
  if (v && r.s != r.end) { v = NULL; r.error = "there is more after the JSON"; }
  if (error) *error = v ? NULL : r.error;
  return v;
}
