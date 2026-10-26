// std.c - the std group of the core lingo: what Lingo can't write for itself, reaching inside values and converting them.
// test/reference/std.mjs gives the same. Everything else for text, lists, numbers and dates is written in Lingo, in
// ../../lingo-library (text, list, math and time).
//
// 1. Inside values   length, keys, slice: counted by characters for text, not bytes
// 2. Converting      number, text, to_json, from_json, bytes, from_bytes
// 3. std_phrases()   -> adds them all
#include "lingo.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

////////// 1. INSIDE VALUES //////////

static val *list_of(run *r, val *x) {
  if (x->k != V_LIST) fail(r, "type", "Expected a list, but this is %s", kind_name(x));
  return x;
}

static val *len(run *r, val **a) {
  val *x = a[0];
  size_t n = x->k == V_LIST ? x->l.n : x->k == V_TEXT ? utf8_count(x->t.s, x->t.len) : x->k == V_REC ? x->r.n : 0;
  return v_num(r->a, (double)n);
}

static val *keys(run *r, val **a) {
  val *out = v_list(r->a);
  if (a[0]->k == V_REC) for (size_t i = 0; i < a[0]->r.n; i++) list_push(r->a, out, v_cstr(r->a, a[0]->r.key[i]));
  return out;
}

// Like JavaScript's slice: negative counts from the end, and both ends are kept inside the list or text.
static size_t edge(run *r, val *x, double n, size_t fallback) {
  if (x->k == V_NOTHING) return fallback;
  double i = trunc(num_of(r, x, NULL));
  if (i < 0) i += n;
  return i < 0 ? 0 : i > n ? (size_t)n : (size_t)i;
}

static val *slice(run *r, val **a) {
  val *x = a[0];
  if (x->k == V_TEXT) {
    size_t n = utf8_count(x->t.s, x->t.len), from = edge(r, a[1], (double)n, 0), to = edge(r, a[2], (double)n, n);
    if (to <= from) return v_text(r->a, "", 0);
    size_t at = utf8_offset(x->t.s, x->t.len, from);
    return v_text(r->a, x->t.s + at, utf8_offset(x->t.s + at, x->t.len - at, to - from));
  }
  list_of(r, x);
  size_t from = edge(r, a[1], (double)x->l.n, 0), to = edge(r, a[2], (double)x->l.n, x->l.n);
  val *out = v_list(r->a);
  for (size_t i = from; i < to; i++) list_push(r->a, out, x->l.v[i]);
  return out;
}

////////// 2. CONVERTING //////////

// "$1,204.50" is 1204.5: money signs, commas, spaces and percent signs are ignored, and only the number at the start counts.
static val *num(run *r, val **a) {
  if (a[0]->k == V_NUM) return a[0];
  val *t = text_of(r, a[0]);
  buf b = { 0 };
  for (size_t i = 0; i < t->t.len; i++) {
    const char *c = t->t.s + i;
    if (*c == '$' || *c == ',' || *c == '%' || isspace((unsigned char)*c)) continue;
    if (!memcmp(c, "\xE2\x82\xAC", 3) && i + 3 <= t->t.len) { i += 2; continue; }   // euro
    if (!memcmp(c, "\xC2\xA3", 2) && i + 2 <= t->t.len) { i += 1; continue; }       // pound
    buf_chr(&b, *c);
  }
  buf_chr(&b, 0);
  const char *s = b.s;
  // strtod also reads hex, inf and nan, which JavaScript's parseFloat doesn't, so only a plain decimal start is allowed.
  int ok = (s[0] >= '0' && s[0] <= '9') || ((s[0] == '-' || s[0] == '+' || s[0] == '.') && ((s[1] >= '0' && s[1] <= '9') || (s[1] == '.' && s[2] >= '0' && s[2] <= '9')));
  if (ok && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { buf_free(&b); return v_num(r->a, 0); }
  double n = ok ? strtod(s, NULL) : NAN;
  buf_free(&b);
  return isfinite(n) ? v_num(r->a, n) : NOTHING;
}

static val *text(run *r, val **a) { return text_of(r, a[0]); }

static val *json(run *r, val **a) {
  buf b = { 0 };
  buf_json(&b, a[0]);
  val *out = v_text(r->a, b.s, b.n);
  buf_free(&b);
  return out;
}

static val *parse(run *r, val **a) {
  val *t = text_of(r, a[0]);
  const char *problem = NULL;
  val *out = json_parse(r->a, t->t.s, t->t.len, &problem);
  if (problem) fail(r, "phrase", "that text isn't JSON");
  return out;
}

// Text as its UTF-8 bytes and back: how a lingo reaches a character's code, to change its case or encode it.
static val *bytes(run *r, val **a) {
  val *t = text_of(r, a[0]), *out = v_list(r->a);
  for (size_t i = 0; i < t->t.len; i++) list_push(r->a, out, v_num(r->a, (unsigned char)t->t.s[i]));
  return out;
}

static val *from_bytes(run *r, val **a) {
  val *l = list_of(r, a[0]);
  buf b = { 0 };
  for (size_t i = 0; i < l->l.n; i++) {
    double n = l->l.v[i]->k == V_NUM ? l->l.v[i]->n : -1;
    if (n < 0 || n > 255 || n != floor(n)) { buf_free(&b); fail(r, "phrase", "a byte is a whole number from 0 to 255"); }
    buf_chr(&b, (char)(unsigned char)n);
  }
  if (!utf8_ok(b.s ? b.s : "", b.n)) { buf_free(&b); fail(r, "phrase", "those bytes aren't UTF-8 text"); }
  val *out = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return out;
}

////////// 3. STD_PHRASES //////////

void std_phrases(void) {
  group_add("std", "none, pure");
  phrase_add("std", "none", "length(x) -> number: items, characters or parts", len);
  phrase_add("std", "none", "keys(r) -> list: a record's part names", keys);
  phrase_add("std", "none", "slice(x, from, to=nothing) -> list|text: to not included, negative counts from the end", slice);
  phrase_add("std", "none", "number(x) -> number?: from text like \"$1,204.50\"", num);
  phrase_add("std", "none", "text(x) -> text", text);
  phrase_add("std", "none", "to_json(x) -> text", json);
  phrase_add("std", "none", "from_json(text) -> any: from JSON", parse);
  phrase_add("std", "none", "bytes(text) -> [number]: its UTF-8 bytes, each 0 to 255", bytes);
  phrase_add("std", "none", "from_bytes(list) -> text: UTF-8 bytes back to text", from_bytes);
}
