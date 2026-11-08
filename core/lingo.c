// lingo.c - the lingo client, whole: the language, its routes and the policies guarding them, the floor calls, its link to
// Onsay with the lingo search and the log of every request, and the floor under it on Windows and on Linux. Lingos attach
// through lingo.h. Built by build.ps1 with zig, beside the code it takes as it came (vendor/).
//
// 1. Values      nothing, true/false, numbers, text, lists, records and bytes, in arenas, and JSON
// 2. Phrases     what this computer can do: each call's line, what it needs and its C
// 3. Compiling   Lingo code to the tree the runner runs, every lingo listed and linked
// 4. Running     a tree to its value or its error, within its limits
// 5. Standard    the std calls: length, slice, find, text, JSON and bytes
// 6. Signing     SHA-256, base64 and Ed25519
// 7. Zips        a .lingo file read and made
// 8. Engine      settings, the floor calls, lingos loaded, policies, calls, routes and every request answered
// 9. Onsay       the link to Onsay, signing in, keys, policy sync, the log sent, prompts and screens
// 10. Host       the program: its threads, its port, the command line and main
// 11. Windows    the floor on Windows
// 12. Linux      the floor on Linux and Android

#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lingo.h"
#include "monocypher-ed25519.h"
#include "puff.h"
#ifndef __ANDROID__
#include <ffi.h>
#endif

#define CORE_VERSION "0.24.6"
#define CORE_LOOK 5   // seconds between looks at the lingos folder

/* Where Onsay is, built in: its API, which the onsay lingo talks to and whose resolve endpoint the lingo client asks where
   an Onsay name goes (../../docs/lingo.md, section 15), and its public key, which everything Onsay signs is checked
   against (Onsay, section 9). The test build asks test/server.mjs, which signs with the test key. */
#ifdef TEST
#define ONSAY "http://127.0.0.1:8787"
#define ONSAY_KEY "ed25519:6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw="
#else
#define ONSAY "https://api.onsay.dev"
#define ONSAY_KEY "ed25519:4DnM32U1FzXfVh3ahkdILlt7yNwfM84Ofj7Aohoe1QE="
#endif
#define RESOLVE "/resolve/"   // on ONSAY, then the name

typedef struct {
  val *all;                           // the whole file, as read
  const char *home, *root, *lingos;   // root: home as a whole path
  int port;
  long long steps;
  double ms;
} core_settings;

const core_settings *core_read(const char *text, size_t n);
int core_begin(int serving);
int core_look(void);
void core_serve(const char *request, size_t n, const char *caller, buf *out);
void core_stop(const char *id);
val *core_control(arena *a, const char *verb, val *values, const char **why);
int core_pack(const char *folder, const char *to);

int zip_file(const char *zip, size_t n, const char *name, buf *out);
void zip_make(buf *out, int count, const char *const *names, const buf *files);

//////////////////// 1. VALUES ////////////////////

// Lingo's values in C: their arena, making, copying and comparing them, and JSON in and out.
//
// 1. Arena       memory for one program, freed all at once
// 2. Values      making, reading, copying, comparing, their kinds, and headers by lower case name
// 3. UTF-8       counting characters, so text is indexed by character, not by byte, and checking bytes are text
// 4. Buffers     growing text
// 5. Numbers     the shortest digits that read back exactly, laid out as JavaScript does
// 6. JSON out    a value as JSON, or as text
// 7. JSON in     JSON text as a value

////////// 1. ARENA //////////

typedef struct chunk {
  struct chunk *next;
  size_t used, size;
  _Alignas(16) char data[];
} chunk;
struct arena {
  chunk *head;
  size_t total;
};

#define CHUNK 65536

// The computer is out of memory. A crash, not an abort, so it stops only the request that was running (pl_faults).
static void no_memory(void) { *(volatile char *)0 = 0; }

arena *arena_new(void) {
  arena *a = calloc(1, sizeof *a);
  if (!a) no_memory();
  return a;
}

void arena_free(arena *a) {
  if (!a) return;
  for (chunk *c = a->head, *n; c; c = n) {
    n = c->next;
    free(c);
  }
  free(a);
}

// Zeroed memory. Anything big gets a chunk of its own, behind the current one, so the current one keeps filling.
void *arena_get(arena *a, size_t size) {
  size = (size + 15) & ~(size_t)15;
  chunk *c = a->head;
  if (size > CHUNK / 4) {
    chunk *own = malloc(sizeof(chunk) + size);
    if (!own) no_memory();
    own->size = own->used = size;
    if (c) {
      own->next = c->next;
      c->next = own;
    } else {
      own->next = NULL;
      a->head = own;
    }
    a->total += size;
    memset(own->data, 0, size);
    return own->data;
  }
  if (!c || c->used + size > c->size) {
    c = malloc(sizeof(chunk) + CHUNK);
    if (!c) no_memory();
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

char *arena_fmt(arena *a, const char *fmt, ...) {
  va_list ap, again;
  va_start(ap, fmt);
  va_copy(again, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *s = arena_get(a, (size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, fmt, again);
  va_end(again);
  return s;
}

////////// 2. VALUES //////////

val V_NOTHING_VAL = { .k = V_NOTHING };
val V_TRUE_VAL = { .k = V_BOOL, .b = 1 };
val V_FALSE_VAL = { .k = V_BOOL, .b = 0 };

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

val *v_bytes(arena *a, const char *s, size_t len) {
  val *v = v_text(a, s, len);
  v->k = V_BYTES;
  return v;
}

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
  for (size_t i = 0; r && r->k == V_REC && i < r->r.n; i++)
    if (!strcmp(r->r.key[i], key)) return r->r.v[i];
  return NULL;
}

void rec_set(arena *a, val *r, const char *key, val *x) {
  for (size_t i = 0; i < r->r.n; i++)
    if (!strcmp(r->r.key[i], key)) {
      r->r.v[i] = x;
      return;
    }
  if (r->r.n == r->r.cap) {
    size_t cap = r->r.cap ? r->r.cap * 2 : 8;
    char **k = arena_get(a, cap * sizeof *k);
    val **v = arena_get(a, cap * sizeof *v);
    if (r->r.n) {
      memcpy(k, r->r.key, r->r.n * sizeof *k);
      memcpy(v, r->r.v, r->r.n * sizeof *v);
    }
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
  buf low = { 0 };
  buf both = { 0 };
  for (size_t i = 0; i < n; i++) buf_chr(&low, (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]));
  val *had = rec_get(headers, low.s ? low.s : "");
  if (had) {
    buf_add(&both, had->t.s, had->t.len);
    buf_str(&both, ", ");
  }
  buf_add(&both, value, vn);
  rec_set(a, headers, low.s ? low.s : "", v_text(a, both.s ? both.s : "", both.n));
  buf_free(&low);
  buf_free(&both);
}

val *header_lines(arena *a, const char *s) {
  val *out = v_rec(a);
  for (const char *end; *s; s = *end ? end + 2 : end) {
    if (!(end = strstr(s, "\r\n"))) end = s + strlen(s);
    const char *colon = memchr(s, ':', (size_t)(end - s));
    const char *v = colon;
    const char *stop = end;
    if (!colon || colon == s) continue;   // the status line
    while (++v < end && (*v == ' ' || *v == '\t')) {}
    while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
    header_add(a, out, s, (size_t)(colon - s), v, (size_t)(stop - v));
  }
  return out;
}

// Text, bytes and numbers never change once made, so only lists and records need copying.
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

// Bytes are the same as a list of the same numbers, since a lingo reads them as one.
static int bytes_list(val *bytes, val *list) {
  if (bytes->t.len != list->l.n) return 0;
  for (size_t i = 0; i < list->l.n; i++)
    if (list->l.v[i]->k != V_NUM || list->l.v[i]->n != (unsigned char)bytes->t.s[i]) return 0;
  return 1;
}

int v_same(val *a, val *b) {
  if (a == b) return 1;
  if (a->k == V_BYTES && b->k == V_LIST) return bytes_list(a, b);
  if (a->k == V_LIST && b->k == V_BYTES) return bytes_list(b, a);
  if (a->k != b->k) return 0;
  switch (a->k) {
    case V_NOTHING: return 1;
    case V_BOOL: return a->b == b->b;
    case V_NUM: return a->n == b->n;
    case V_TEXT:
    case V_BYTES: return a->t.len == b->t.len && !memcmp(a->t.s, b->t.s, a->t.len);
    case V_LIST:
      if (a->l.n != b->l.n) return 0;
      for (size_t i = 0; i < a->l.n; i++)
        if (!v_same(a->l.v[i], b->l.v[i])) return 0;
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
  static const char *names[] = { "nothing", "true/false", "number", "text", "list", "record", "bytes" };
  return names[x->k];
}

////////// 3. UTF-8 //////////

static int cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t utf8_count(const char *s, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++)
    if (!cont((unsigned char)s[i])) n++;
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

// How many bytes the character starting with this byte has, or 0 when no character starts with it.
size_t utf8_width(unsigned char first) {
  if (first < 0x80) return 1;
  if ((first & 0xE0) == 0xC0) return 2;
  if ((first & 0xF0) == 0xE0) return 3;
  if ((first & 0xF8) == 0xF0) return 4;
  return 0;
}

// 1 when every character is whole, 0 for bytes that aren't text.
int utf8_ok(const char *s, size_t len) {
  for (size_t i = 0; i < len;) {
    unsigned char c = (unsigned char)s[i];
    size_t w = utf8_width(c);
    if (!w || i + w > len) return 0;
    for (size_t k = 1; k < w; k++)
      if (!cont((unsigned char)s[i + k])) return 0;
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
    if (!p) no_memory();
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
  if ((size_t)n < sizeof small) {
    buf_add(b, small, (size_t)n);
    return;
  }
  char *big = malloc((size_t)n + 1);
  if (!big) no_memory();
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  buf_add(b, big, (size_t)n);
  free(big);
}

void buf_free(buf *b) {
  free(b->s);
  b->s = NULL;
  b->n = b->cap = 0;
}

////////// 5. NUMBERS //////////

/* JavaScript's rule: the fewest digits that read back as the same number, then laid out plainly between 1e-7 and 1e21,
   and with an exponent outside that. 3 is "3", 0.1 is "0.1", 1e21 is "1e+21". */
void buf_num(buf *b, double n) {
  if (isnan(n)) {
    buf_str(b, "NaN");
    return;
  }
  if (isinf(n)) {
    buf_str(b, n < 0 ? "-Infinity" : "Infinity");
    return;
  }
  if (n == 0) {
    buf_chr(b, '0');
    return;
  }
  char tmp[40];
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, n);
    if (strtod(tmp, NULL) == n) break;
  }
  // tmp is like -1.2345e+05: the sign, the digits without the point, and the exponent.
  const char *s = tmp;
  if (*s == '-') {
    buf_chr(b, '-');
    s++;
  }
  char digits[24];
  int k = 0;
  for (; *s && *s != 'e'; s++)
    if (*s != '.') digits[k++] = *s;
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
    if (k > 1) {
      buf_chr(b, '.');
      buf_add(b, digits + 1, (size_t)(k - 1));
    }
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
        if (c < 0x20)
          buf_fmt(b, "\\u%04x", c);
        else
          buf_chr(b, (char)c);
    }
  }
  buf_chr(b, '"');
}

void buf_json(buf *b, val *x) {
  switch (x->k) {
    case V_NOTHING: buf_str(b, "null"); break;
    case V_BOOL: buf_str(b, x->b ? "true" : "false"); break;
    case V_NUM:
      if (isnan(x->n) || isinf(x->n))
        buf_str(b, "null");
      else
        buf_num(b, x->n);
      break;
    case V_TEXT: buf_json_str(b, x->t.s, x->t.len); break;
    case V_BYTES:
      buf_chr(b, '[');
      for (size_t i = 0; i < x->t.len; i++) {
        if (i) buf_chr(b, ',');
        buf_fmt(b, "%u", (unsigned char)x->t.s[i]);
      }
      buf_chr(b, ']');
      break;
    case V_LIST:
      buf_chr(b, '[');
      for (size_t i = 0; i < x->l.n; i++) {
        if (i) buf_chr(b, ',');
        buf_json(b, x->l.v[i]);
      }
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

typedef struct {
  arena *a;
  const char *s, *end;
  const char *error;
  int depth;
} reader;

static void skip(reader *r) {
  while (r->s < r->end && (*r->s == ' ' || *r->s == '\n' || *r->s == '\r' || *r->s == '\t')) r->s++;
}
static val *bad(reader *r, const char *why) {
  if (!r->error) r->error = why;
  return NULL;
}

static void put_utf8(buf *b, uint32_t c) {
  char o[4];
  if (c < 0x80) {
    o[0] = (char)c;
    buf_add(b, o, 1);
  } else if (c < 0x800) {
    o[0] = (char)(0xC0 | c >> 6);
    o[1] = (char)(0x80 | (c & 63));
    buf_add(b, o, 2);
  } else if (c < 0x10000) {
    o[0] = (char)(0xE0 | c >> 12);
    o[1] = (char)(0x80 | (c >> 6 & 63));
    o[2] = (char)(0x80 | (c & 63));
    buf_add(b, o, 3);
  } else {
    o[0] = (char)(0xF0 | c >> 18);
    o[1] = (char)(0x80 | (c >> 12 & 63));
    o[2] = (char)(0x80 | (c >> 6 & 63));
    o[3] = (char)(0x80 | (c & 63));
    buf_add(b, o, 4);
  }
}

static int hex4(reader *r, uint32_t *out) {
  if (r->end - r->s < 4) return 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = r->s[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (uint32_t)(c - 'A' + 10);
    else
      return 0;
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
    if (c != '\\') {
      buf_chr(b, c);
      continue;
    }
    if (r->s >= r->end) return 0;
    char e = *r->s++;
    switch (e) {
      case '"':
      case '\\':
      case '/': buf_chr(b, e); break;
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
    if (r->s < r->end && *r->s == '}') {
      r->s++;
      r->depth--;
      return out;
    }
    for (;;) {
      skip(r);
      if (r->s >= r->end || *r->s != '"') return bad(r, "a part name must be text");
      r->s++;
      buf key = { 0 };
      if (!string_in(r, &key)) {
        buf_free(&key);
        return bad(r, "a part name never ends");
      }
      skip(r);
      if (r->s >= r->end || *r->s != ':') {
        buf_free(&key);
        return bad(r, "a : is missing");
      }
      r->s++;
      val *v = value_in(r);
      if (!v) {
        buf_free(&key);
        return NULL;
      }
      rec_set(r->a, out, key.s ? key.s : "", v);
      buf_free(&key);
      skip(r);
      if (r->s < r->end && *r->s == ',') {
        r->s++;
        continue;
      }
      if (r->s < r->end && *r->s == '}') {
        r->s++;
        break;
      }
      return bad(r, "a , or } is missing");
    }
  } else if (c == '[') {
    r->s++;
    out = v_list(r->a);
    skip(r);
    if (r->s < r->end && *r->s == ']') {
      r->s++;
      r->depth--;
      return out;
    }
    for (;;) {
      val *v = value_in(r);
      if (!v) return NULL;
      list_push(r->a, out, v);
      skip(r);
      if (r->s < r->end && *r->s == ',') {
        r->s++;
        continue;
      }
      if (r->s < r->end && *r->s == ']') {
        r->s++;
        break;
      }
      return bad(r, "a , or ] is missing");
    }
  } else if (c == '"') {
    r->s++;
    buf t = { 0 };
    if (!string_in(r, &t)) {
      buf_free(&t);
      return bad(r, "text never ends");
    }
    out = v_text(r->a, t.s ? t.s : "", t.n);
    buf_free(&t);
  } else if (r->end - r->s >= 4 && !memcmp(r->s, "true", 4)) {
    r->s += 4;
    out = VBOOL(1);
  } else if (r->end - r->s >= 5 && !memcmp(r->s, "false", 5)) {
    r->s += 5;
    out = VBOOL(0);
  } else if (r->end - r->s >= 4 && !memcmp(r->s, "null", 4)) {
    r->s += 4;
    out = NOTHING;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    char tmp[64];
    size_t n = 0;
    while (r->s < r->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", *r->s)) tmp[n++] = *r->s++;
    tmp[n] = 0;
    char *stop;
    double d = strtod(tmp, &stop);
    if (stop != tmp + n) return bad(r, "a number is not written right");
    out = v_num(r->a, d);
  } else
    return bad(r, "something that isn't JSON");
  r->depth--;
  return out;
}

val *json_parse(arena *a, const char *s, size_t n, const char **error) {
  reader r = { a, s, s + n, NULL, 0 };
  val *v = value_in(&r);
  skip(&r);
  if (v && r.s != r.end) {
    v = NULL;
    r.error = "there is more after the JSON";
  }
  if (error) *error = v ? NULL : r.error;
  return v;
}

//////////////////// 2. PHRASES ////////////////////

// The table of phrases this computer has, each added with its line, like
// "keys(prefix: text=\"\") -> [text]: sorted", and the words for a type in such a line as JSON Schema.
//
// 1. native_begin(lingo), phrase_add(group, needs, line, fn)  -> the phrases of native lingo number lingo, as it loads
// 2. phrase_find(id)                                          -> a phrase by its id, like std.length
// 3. native_phrases(a, lingo)                                 -> a native lingo's phrases, as the compiler reads them,
//                                                                with what each needs
// 4. type_schema(a, words)                                    -> a type in words as JSON Schema: text, number, whole, bool,
//                                                                list, record, any, nothing, a meaning like path, [one],
//                                                                {field: type, maybe?: type}, one|other, and one? for
//                                                                one or nothing

static phrase *phrases;
static size_t count, phrase_room, adding;   // adding: the native lingo whose phrases are being added
static arena *kept;   // ids, names and defaults, kept for as long as the core runs

////////// 1. ADDING //////////

void native_begin(size_t lingo) { adding = lingo; }

static size_t trimmed(const char **s, size_t n) {
  while (n && **s == ' ') {
    (*s)++;
    n--;
  }
  while (n && (*s)[n - 1] == ' ') n--;
  return n;
}

// One param, like sep: text="," or to: path=nothing. A default is a plain value, written as JSON, or nothing.
static void param(phrase *p, const char *s, size_t n) {
  n = trimmed(&s, n);
  const char *eq = memchr(s, '=', n);
  const char *colon = memchr(s, ':', eq ? (size_t)(eq - s) : n);
  const char *name = s;
  size_t before = eq ? (size_t)(eq - s) : n;
  size_t len = trimmed(&name, colon ? (size_t)(colon - s) : before);
  p->types[p->count] = NULL;
  if (colon) {
    const char *type = colon + 1;
    size_t tlen = trimmed(&type, before - (size_t)(colon - s) - 1);
    p->types[p->count] = arena_get(kept, tlen + 1);
    memcpy(p->types[p->count], type, tlen);
  }
  p->params[p->count] = arena_get(kept, len + 1);
  memcpy(p->params[p->count], name, len);
  p->defaults[p->count] = NULL;
  if (eq) {
    const char *d = eq + 1;
    const char *problem = NULL;
    size_t dn = trimmed(&d, n - (size_t)(eq - s) - 1);
    val *v = dn == 7 && !memcmp(d, "nothing", 7) ? NOTHING : json_parse(kept, d, dn, &problem);
    if (!problem) p->defaults[p->count] = v;
  }
  p->count++;
}

// 1.
void phrase_add(const char *group, const char *needs, const char *line, phrase_fn fn) {
  if (!kept) kept = arena_new();
  if (count == phrase_room) {
    phrase_room = phrase_room ? phrase_room * 2 : 64;
    phrases = realloc(phrases, phrase_room * sizeof *phrases);
  }
  phrase *p = &phrases[count++];
  const char *open = strchr(line, '(');
  const char *at = open ? open + 1 : line + strlen(line);
  size_t name = (size_t)(at - line) - (open != NULL);
  size_t glen = strlen(group);
  char *id = arena_get(kept, glen + name + 2);
  memcpy(id, group, glen);
  id[glen] = '.';
  memcpy(id + glen + 1, line, name);
  *p = (phrase){ .id = id, .needs = needs, .fn = fn, .lingo = adding };
  p->params = arena_get(kept, 16 * sizeof *p->params);
  p->defaults = arena_get(kept, 16 * sizeof *p->defaults);
  p->types = arena_get(kept, 16 * sizeof *p->types);
  // Params are split at commas outside quotes and brackets, so a default like sep="," stays whole.
  int depth = 0;
  int quoted = 0;
  for (const char *from = at; open && *at; at++) {
    if (quoted) {
      if (*at == '\\' && at[1])
        at++;
      else if (*at == '"')
        quoted = 0;
      continue;
    }
    if (*at == '"')
      quoted = 1;
    else if (*at == '[' || *at == '{' || *at == '(')
      depth++;
    else if ((*at == ']' || *at == '}' || *at == ')') && depth)
      depth--;
    else if ((*at == ',' || *at == ')') && !depth) {
      if (at > from && p->count < 16) param(p, from, (size_t)(at - from));
      from = at + 1;
      if (*at == ')') {
        at++;
        break;
      }
    }
  }
  while (*at == ' ' || *at == ':') at++;
  p->gives = NULL;
  if (!strncmp(at, "->", 2)) {
    // What it gives runs to the first colon outside its brackets, or the end.
    const char *from = at + 2;
    while (*from == ' ') from++;
    const char *end = from;
    int inside = 0;
    for (; *end && (inside || *end != ':'); end++) {
      if (*end == '[' || *end == '{') inside++;
      if (*end == ']' || *end == '}') inside--;
    }
    size_t gives = (size_t)(end - from);
    char *copy = arena_get(kept, gives + 1);
    memcpy(copy, from, gives);
    p->gives = copy;
    at = end;
    while (*at == ' ' || *at == ':') at++;
  }
  p->does = at;
}

////////// 2. FINDING //////////

phrase *phrase_find(const char *id) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(phrases[i].id, id)) return &phrases[i];
  return NULL;
}

////////// 3. AS THE COMPILER READS THEM //////////

val *native_phrases(arena *a, size_t lingo) {
  val *out = v_list(a);
  for (size_t i = 0; i < count; i++) {
    if (phrases[i].lingo != lingo) continue;
    val *one = v_rec(a);
    val *params = v_list(a);
    val *needs = v_list(a);
    for (int k = 0; k < phrases[i].count; k++) {
      val *p = v_rec(a);
      rec_set(a, p, "name", v_cstr(a, phrases[i].params[k]));
      if (phrases[i].defaults[k]) rec_set(a, p, "default", phrases[i].defaults[k]);
      if (phrases[i].types[k]) rec_set(a, p, "type", v_cstr(a, phrases[i].types[k]));
      list_push(a, params, p);
    }
    for (const char *n = phrases[i].needs; *n; n += strspn(n, ", ")) {
      size_t len = strcspn(n, ",");
      if (len != 4 || memcmp(n, "none", 4)) list_push(a, needs, v_text(a, n, len));
      n += len;
    }
    rec_set(a, one, "id", v_cstr(a, phrases[i].id));
    rec_set(a, one, "params", params);
    rec_set(a, one, "does", v_cstr(a, phrases[i].does));
    if (phrases[i].gives) rec_set(a, one, "gives", v_cstr(a, phrases[i].gives));
    rec_set(a, one, "needs", needs);
    list_push(a, out, one);
  }
  return out;
}

////////// 4. TYPES AS JSON SCHEMA //////////

// What a word in a line means. A meaning is text with a format, since "text" alone can't tell a file's path from a sentence.
static const struct { const char *word, *type, *format; } TYPES[] = {
  { "text", "string", NULL }, { "number", "number", NULL }, { "whole", "integer", NULL }, { "bool", "boolean", NULL },
  { "list", "array", NULL }, { "bytes", "array", "bytes" }, { "record", "object", NULL }, { "nothing", "null", NULL }, { "any", NULL, NULL },
  { "path", "string", "path" }, { "folder", "string", "folder" }, { "command", "string", "command" }, { "url", "string", "url" },
  { "pattern", "string", "pattern" }, { "html", "string", "html" }, { "combo", "string", "combo" }, { "app", "string", "app" },
  { "control", "object", "control" },
};

static val *one_or_other(arena *a, const char **s);

// A word: its type and format. A window is its id, something matching it, or the screen when nothing.
static val *word_type(arena *a, const char *word, size_t n) {
  val *out = v_rec(a);
  if (n == 6 && !memcmp(word, "window", 6)) {
    val *types = v_list(a);
    list_push(a, types, v_cstr(a, "integer"));
    list_push(a, types, v_cstr(a, "object"));
    list_push(a, types, v_cstr(a, "null"));
    rec_set(a, out, "type", types);
    rec_set(a, out, "format", v_cstr(a, "window"));
    return out;
  }
  for (size_t i = 0; i < sizeof TYPES / sizeof *TYPES; i++) {
    if (strlen(TYPES[i].word) != n || memcmp(TYPES[i].word, word, n)) continue;
    if (TYPES[i].type) rec_set(a, out, "type", v_cstr(a, TYPES[i].type));
    if (TYPES[i].format) rec_set(a, out, "format", v_cstr(a, TYPES[i].format));
    return out;
  }
  return out;
}

static void skip_spaces(const char **s) {
  while (**s == ' ') (*s)++;
}

// {field: type, maybe?: type}. A field with no type takes anything.
static val *record_type(arena *a, const char **s) {
  val *out = v_rec(a);
  val *fields = v_rec(a);
  val *required = v_list(a);
  (*s)++;
  while (**s && **s != '}') {
    skip_spaces(s);
    const char *name = *s;
    size_t n = strcspn(name, ":?,}");
    *s += n;
    val *field_name = v_text(a, name, n);
    int maybe = **s == '?';
    if (maybe) (*s)++;
    val *type = v_rec(a);
    if (**s == ':') {
      (*s)++;
      type = one_or_other(a, s);
    }
    rec_set(a, fields, field_name->t.s, type);
    if (!maybe) list_push(a, required, field_name);
    skip_spaces(s);
    if (**s == ',') (*s)++;
  }
  if (**s == '}') (*s)++;
  rec_set(a, out, "type", v_cstr(a, "object"));
  rec_set(a, out, "properties", fields);
  rec_set(a, out, "required", required);
  return out;
}

// One type, with a ? after it when it may be nothing.
static val *single_type(arena *a, const char **s) {
  skip_spaces(s);
  val *out;
  if (**s == '[') {
    (*s)++;
    out = v_rec(a);
    rec_set(a, out, "type", v_cstr(a, "array"));
    rec_set(a, out, "items", one_or_other(a, s));
    skip_spaces(s);
    if (**s == ']') (*s)++;
  } else if (**s == '{') {
    out = record_type(a, s);
  } else {
    const char *word = *s;
    size_t n = strcspn(word, " ,:|?]}");
    *s += n;
    out = word_type(a, word, n);
  }
  skip_spaces(s);
  if (**s != '?') return out;
  (*s)++;
  val *both = v_rec(a);
  val *ways = v_list(a);
  val *nothing = v_rec(a);
  rec_set(a, nothing, "type", v_cstr(a, "null"));
  list_push(a, ways, out);
  list_push(a, ways, nothing);
  rec_set(a, both, "anyOf", ways);
  return both;
}

static val *one_or_other(arena *a, const char **s) {
  val *first = single_type(a, s);
  skip_spaces(s);
  if (**s != '|') return first;
  val *both = v_rec(a);
  val *ways = v_list(a);
  list_push(a, ways, first);
  while (**s == '|') {
    (*s)++;
    list_push(a, ways, single_type(a, s));
    skip_spaces(s);
  }
  rec_set(a, both, "anyOf", ways);
  return both;
}

// 4.
val *type_schema(arena *a, const char *words) {
  return one_or_other(a, &words);
}

//////////////////// 3. COMPILING ////////////////////

// Lingo code to the tree the runner runs: every lingo in the lingos folder, read, listed and compiled, and a
// call of one of their functions linked down to native phrases. Errors are passed back up, never jumped: every parse
// function gives NULL and the first problem is kept in the context.
//
// 1. lex(c, s, n, out)                  -> text to tokens
// 2. parse_with(c, code, n)             -> tokens to a tree, checked against the phrases it may use
// 3. listing(a, file, code, why)        -> a lingo written in Lingo, its listing made from its code: every fn, the
//                                          comments above each, its about, and the settings it reads
// 4. lingo_call(a, inst, fn)            -> a program that calls one function, every Lingo function it reaches pulled in
// 5. lingo_load(a, natives, stacked)    -> every lingo a computer has, each compiled against all of them, with what each
//                                          function needs and how many lingo(...) calls it makes

static const char *KEYWORDS[] = { "if", "else", "while", "for", "in", "break", "continue", "fn", "return", "tool",
  "try", "catch", "fail", "and", "or", "not", "true", "false", "nothing", "null", NULL };
static const char *SYMBOLS[] = { "..", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "??", "&&", "||", "=", "+", "-",
  "*", "/", "%", "<", ">", "!", "?", ":", "(", ")", "[", "]", "{", "}", ",", ".", ";", NULL };

////////// HELPERS //////////

static char *compile_dup(arena *a, const char *s, size_t n) {
  char *out = arena_get(a, n + 1);
  memcpy(out, s, n);
  return out;
}

static char *buf_dup(arena *a, buf *b) { return compile_dup(a, b->s ? b->s : "", b->n); }
static val *T(arena *a, const char *s) { return v_cstr(a, s); }
static int is_text(val *v, const char *s) { return v && v->k == V_TEXT && !strcmp(v->t.s, s); }
static int tagged(val *v, const char *tag) { return v && v->k == V_LIST && v->l.n && is_text(v->l.v[0], tag); }
static val *at(val *v, size_t i) { return v && v->k == V_LIST && i < v->l.n ? v->l.v[i] : NULL; }

static val *mk(arena *a, const char *tag, int n, ...) {
  val *out = v_list(a);
  list_push(a, out, T(a, tag));
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) list_push(a, out, va_arg(ap, val *));
  va_end(ap);
  return out;
}

static void push_unique(arena *a, val *list, val *text) {
  for (size_t i = 0; i < list->l.n; i++)
    if (!strcmp(list->l.v[i]->t.s, text->t.s)) return;
  list_push(a, list, text);
}

static val *compile_failed(const char **message, char *m) {
  *message = m;
  return NULL;
}

// One character of UTF-8 at s[i], and how many bytes it takes.
static unsigned code_at(const char *s, size_t i, size_t n, size_t *len) {
  unsigned char c = (unsigned char)s[i];
  size_t w = utf8_width(c);
  if (!w || i + w > n) w = 1;
  unsigned cp = c;
  if (w == 2) cp = c & 0x1F;
  if (w == 3) cp = c & 0x0F;
  if (w == 4) cp = c & 0x07;
  for (size_t k = 1; k < w; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  *len = w;
  return cp;
}

////////// CONTEXT //////////

typedef struct {
  const char *name;
  int count, line;
  val *node;
} site;

typedef struct {
  arena *a;
  val *phrases;   // id -> phrase, every phrase the code may use
  const char *own;   // the name of the lingo whose code it is, like agent, for setting(...)
  int scan;   // only read for its listing: calls aren't checked, since what they reach isn't known yet
  val *settings;   // the names of its own settings it reads, setting("key"), in order
  val *fn_lines;   // each fn's name -> the line it starts on, for the comments above it
  site *calls;   // calls with no dot, settled once every function is known
  size_t ncalls, room;
  val **marks;   // statement -> its line, a small table keyed by the statement itself
  int *lines;
  size_t nmarks, cap;
  int bad, line;
  const char *message;
} cx;

static void *problem(cx *c, int line, const char *message) {
  if (!c->bad) {
    c->bad = 1;
    c->line = line;
    c->message = message;
  }
  return NULL;
}

static size_t slot(val **marks, size_t cap, val *s) {
  size_t h = ((uintptr_t)s >> 4) & (cap - 1);
  while (marks[h] && marks[h] != s) h = (h + 1) & (cap - 1);
  return h;
}

static void mark(cx *c, val *s, int line) {
  if ((c->nmarks + 1) * 2 > c->cap) {
    size_t cap = c->cap ? c->cap * 2 : 64;
    val **marks = arena_get(c->a, cap * sizeof *marks);
    int *lines = arena_get(c->a, cap * sizeof *lines);
    for (size_t i = 0; i < c->cap; i++)
      if (c->marks[i]) {
        size_t h = slot(marks, cap, c->marks[i]);
        marks[h] = c->marks[i];
        lines[h] = c->lines[i];
      }
    c->marks = marks;
    c->lines = lines;
    c->cap = cap;
  }
  size_t h = slot(c->marks, c->cap, s);
  if (!c->marks[h]) c->nmarks++;
  c->marks[h] = s;
  c->lines[h] = line;
}

static int line_of_mark(cx *c, val *s) {
  if (!c->cap) return 0;
  size_t h = slot(c->marks, c->cap, s);
  return c->marks[h] ? c->lines[h] : 0;
}

static cx *context(arena *a, val *phrases) {
  cx *c = arena_get(a, sizeof *c);
  c->a = a;
  c->phrases = v_rec(a);
  c->settings = v_list(a);
  c->fn_lines = v_rec(a);
  for (size_t i = 0; phrases && i < phrases->l.n; i++)
    rec_set(a, c->phrases, rec_get(phrases->l.v[i], "id")->t.s, phrases->l.v[i]);
  return c;
}

////////// 1. LEX //////////

enum { NUM, ID, KW, STR, SYM };
typedef struct {
  const char *s;
  size_t n;
  int code, line;
} piece;   // code: an expression's source, from {} in text
typedef struct {
  int k, line;
  const char *v;
  double num;
  piece *parts;
  size_t count;
} token;
typedef struct {
  token *t;
  size_t n, cap;
} tokens;

static void add_token(arena *a, tokens *ts, token t) {
  if (ts->n == ts->cap) {
    size_t cap = ts->cap ? ts->cap * 2 : 64;
    token *more = arena_get(a, cap * sizeof *more);
    if (ts->n) memcpy(more, ts->t, ts->n * sizeof *more);
    ts->t = more;
    ts->cap = cap;
  }
  ts->t[ts->n++] = t;
}

static void add_piece(arena *a, piece **ps, size_t *n, size_t *cap, piece p) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    piece *more = arena_get(a, *cap * sizeof *more);
    if (*n) memcpy(more, *ps, *n * sizeof *more);
    *ps = more;
  }
  (*ps)[(*n)++] = p;
}

// What JavaScript's \s takes as space.
static size_t space_at(const char *s, size_t i, size_t n) {
  unsigned char c = (unsigned char)s[i];
  if (c == ' ' || (c >= 9 && c <= 13)) return 1;
  if (c < 0x80) return 0;
  size_t w;
  unsigned cp = code_at(s, i, n, &w);
  int space = cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
      cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
  return space ? w : 0;
}

static int digit(char c) { return c >= '0' && c <= '9'; }
static int alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// 1.
static int lex(cx *c, const char *s, size_t n, tokens *out) {
  arena *a = c->a;
  int line = 1;
  memset(out, 0, sizeof *out);
  for (size_t i = 0; i < n;) {
    char ch = s[i];
    size_t w;
    if (ch == '\n') {
      line++;
      i++;
      continue;
    }
    if ((w = space_at(s, i, n))) {
      i += w;
      continue;
    }
    if (ch == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') i++;
      continue;
    }
    if (digit(ch)) {
      size_t j = i;
      while (j < n && digit(s[j])) j++;
      if (j + 1 < n && s[j] == '.' && digit(s[j + 1])) {
        j++;
        while (j < n && digit(s[j])) j++;
      }
      add_token(a, out, (token){ .k = NUM, .line = line, .num = strtod(compile_dup(a, s + i, j - i), NULL) });
      i = j;
      continue;
    }
    if (alpha(ch)) {
      size_t j = i;
      while (j < n && (alpha(s[j]) || digit(s[j]))) j++;
      int kw = 0;
      for (int k = 0; KEYWORDS[k] && !kw; k++) kw = strlen(KEYWORDS[k]) == j - i && !memcmp(KEYWORDS[k], s + i, j - i);
      add_token(a, out, (token){ .k = kw ? KW : ID, .line = line, .v = compile_dup(a, s + i, j - i) });
      i = j;
      continue;
    }
    if (ch == '"') {
      // Text: \ escapes, and {expression} puts a value inside. \{ is a plain brace.
      int start = line;
      piece *parts = NULL;
      size_t count = 0;
      size_t room = 0;
      buf cur = { 0 };
      for (i++;; i++) {
        if (i >= n) {
          buf_free(&cur);
          problem(c, start, "This text never ends: a \" is missing");
          return 0;
        }
        char h = s[i];
        if (h == '"') {
          i++;
          break;
        }
        if (h == '\n') line++;
        if (h == '\\') {
          if (++i >= n) continue;   // and the loop says the text never ends
          code_at(s, i, n, &w);
          if (s[i] == 'n')
            buf_chr(&cur, '\n');
          else if (s[i] == 't')
            buf_chr(&cur, '\t');
          else
            buf_add(&cur, s + i, w);
          i += w - 1;
          continue;
        }
        if (h == '{') {
          int depth = 1;
          size_t j = i + 1;
          for (; j < n && depth; j++) {
            if (s[j] == '{')
              depth++;
            else if (s[j] == '}')
              depth--;
            else if (s[j] == '"') {
              buf_free(&cur);
              problem(c, line, "Text inside {} inside text is not allowed. Put it in a name first");
              return 0;
            }
          }
          if (depth) {
            buf_free(&cur);
            problem(c, line, "A { inside text is never closed");
            return 0;
          }
          if (cur.n) add_piece(a, &parts, &count, &room, (piece){ buf_dup(a, &cur), cur.n, 0, 0 });
          cur.n = 0;
          add_piece(a, &parts, &count, &room, (piece){ s + i + 1, j - 1 - (i + 1), 1, line });
          i = j - 1;
          continue;
        }
        buf_chr(&cur, h);
      }
      if (cur.n || !count) add_piece(a, &parts, &count, &room, (piece){ buf_dup(a, &cur), cur.n, 0, 0 });
      buf_free(&cur);
      add_token(a, out, (token){ .k = STR, .line = start, .parts = parts, .count = count });
      continue;
    }
    int found = 0;
    for (int k = 0; SYMBOLS[k]; k++) {
      size_t len = strlen(SYMBOLS[k]);
      if (n - i >= len && !memcmp(s + i, SYMBOLS[k], len)) {
        add_token(a, out, (token){ .k = SYM, .line = line, .v = SYMBOLS[k] });
        i += len;
        found = 1;
        break;
      }
    }
    if (!found) {
      size_t j = i;
      for (int k = 0; k < 10 && j < n; k++) {
        code_at(s, j, n, &w);
        j += w;
      }
      problem(c, line, arena_fmt(a, "I can't read \"%.*s\"", (int)(j - i), s + i));
      return 0;
    }
  }
  return 1;
}

////////// 2. PARSE //////////

typedef struct {
  cx *c;
  token *t;
  size_t n, i;
} ps;

static token *peek(ps *p, size_t k) { return p->i + k < p->n ? &p->t[p->i + k] : NULL; }
static int end(ps *p) { return p->i >= p->n; }
static int line_now(ps *p) {
  token *t = peek(p, 0);
  if (!t && p->n) t = &p->t[p->n - 1];
  return t ? t->line : 1;
}
static int is(ps *p, const char *v, size_t k) {
  token *t = peek(p, k);
  return t && (t->k == SYM || t->k == KW) && !strcmp(t->v, v);
}
static int parse_accept(ps *p, const char *v) {
  if (!is(p, v, 0)) return 0;
  p->i++;
  return 1;
}

static const char *show(ps *p, token *t) {
  if (!t) return "the end";
  if (t->k == STR) return "some text";
  if (t->k == NUM) {
    buf b = { 0 };
    buf_num(&b, t->num);
    char *s = arena_fmt(p->c->a, "\"%s\"", b.s);
    buf_free(&b);
    return s;
  }
  return arena_fmt(p->c->a, "\"%s\"", t->v);
}

static void *expected(ps *p, const char *what) {
  return problem(p->c, line_now(p), arena_fmt(p->c->a, "Expected %s, but found %s", what, show(p, peek(p, 0))));
}

static int expect(ps *p, const char *v, const char *what) {
  if (parse_accept(p, v)) return 1;
  expected(p, what ? what : arena_fmt(p->c->a, "\"%s\"", v));
  return 0;
}

static val *name(ps *p, const char *what) {
  token *t = peek(p, 0);
  if (!t || t->k != ID) return expected(p, what);
  p->i++;
  return T(p->c->a, t->v);
}

static val *word(ps *p, const char *what) {
  token *t = peek(p, 0);
  if (!t || (t->k != ID && t->k != KW)) return expected(p, what);
  p->i++;
  return T(p->c->a, t->v);
}

#define GET(x, e) \
  val *x = (e); \
  if (!x) return NULL

static val *expr(ps *p);
static val *statement(ps *p);

// (a, b=10). A default is any expression, worked out when the value is missing.
static val *params(ps *p) {
  arena *a = p->c->a;
  if (!expect(p, "(", NULL)) return NULL;
  val *out = v_list(a);
  if (parse_accept(p, ")")) return out;
  do {
    GET(n, name(p, "the name of a value it takes"));
    val *one = v_list(a);
    list_push(a, one, n);
    if (parse_accept(p, "=")) {
      GET(d, expr(p));
      list_push(a, one, d);
    }
    list_push(a, out, one);
  } while (parse_accept(p, ","));
  if (!expect(p, ")", NULL)) return NULL;
  return out;
}

static val *block(ps *p) {
  val *out = v_list(p->c->a);
  if (!expect(p, "{", "a { to start the block")) return NULL;
  while (!parse_accept(p, "}")) {
    if (end(p)) return expected(p, "a } to end the block");
    if (parse_accept(p, ";")) continue;
    GET(s, statement(p));
    list_push(p->c->a, out, s);
  }
  return out;
}

// A condition needs no parentheses: if x>1 {...}. Its { can't start a record, since nothing continues a whole value with {.
static val *cond(ps *p) {
  if (is(p, "{", 0)) return expected(p, "a condition before the {");
  return expr(p);
}

static val *bare(ps *p) {
  cx *c = p->c;
  arena *a = c->a;
  if (parse_accept(p, "if")) {
    val *branches = v_list(a);
    do {
      GET(when, cond(p));
      GET(then, block(p));
      val *br = v_list(a);
      list_push(a, br, when);
      list_push(a, br, then);
      list_push(a, branches, br);
    } while (is(p, "else", 0) && is(p, "if", 1) && (p->i += 2));
    val *otherwise = v_list(a);
    if (parse_accept(p, "else")) {
      otherwise = block(p);
      if (!otherwise) return NULL;
    }
    return mk(a, "if", 2, branches, otherwise);
  }
  if (parse_accept(p, "while")) {
    GET(w, cond(p));
    GET(b, block(p));
    return mk(a, "while", 2, w, b);
  }
  if (parse_accept(p, "for")) {
    // for x in e {...}, or the older for(x in e){...}, told apart by what follows the (.
    token *t1 = peek(p, 1);
    token *t3 = peek(p, 3);
    int wrapped = is(p, "(", 0) && t1 && t1->k == ID &&
        (is(p, "in", 2) || (is(p, ",", 2) && t3 && t3->k == ID && is(p, "in", 4)));
    if (wrapped) p->i++;
    GET(first, name(p, "a name for each item"));
    val *second = NULL;
    if (parse_accept(p, ",")) {
      second = name(p, "a second name");
      if (!second) return NULL;
    }
    if (!expect(p, "in", "\"in\"")) return NULL;
    GET(from, expr(p));
    if (wrapped && !expect(p, ")", NULL)) return NULL;
    GET(body, block(p));
    // for x in list names the item. for i, x in list names the position and the item, or a record's part and value.
    return second ? mk(a, "each", 4, second, from, body, first) : mk(a, "each", 3, first, from, body);
  }
  if (parse_accept(p, "break")) return mk(a, "break", 0);
  if (parse_accept(p, "continue")) return mk(a, "next", 0);
  if (parse_accept(p, "return")) {
    if (end(p) || is(p, "}", 0) || is(p, ";", 0)) return mk(a, "return", 1, NOTHING);
    GET(e, expr(p));
    return mk(a, "return", 1, e);
  }
  if (parse_accept(p, "fail")) {
    GET(e, expr(p));
    return mk(a, "fail", 1, e);
  }
  if (parse_accept(p, "try")) {
    val *body = block(p);
    if (!body) return NULL;
    if (!expect(p, "catch", "\"catch\" after the try block")) return NULL;
    GET(n, name(p, "a name for the error, like catch e"));
    GET(caught, block(p));
    return mk(a, "try", 3, body, n, caught);
  }
  if (is(p, "tool", 0) || is(p, "fn", 0))
    return problem(c, line_now(p), arena_fmt(a, "\"%s\" goes at the top level, not inside a block", peek(p, 0)->v));
  if (is(p, "else", 0)) return problem(c, line_now(p), "\"else\" needs an if block just before it");

  GET(e, expr(p));
  static const char *sets[] = { "=", "+=", "-=", "*=", "/=" };
  for (int k = 0; k < 5; k++) {
    if (!parse_accept(p, sets[k])) continue;
    GET(value, expr(p));
    if (k) value = mk(a, "op", 3, v_text(a, sets[k], 1), e, value);
    if (tagged(e, "var")) return mk(a, "set", 2, e->l.v[1], value);
    if (tagged(e, "get")) return mk(a, "put", 3, e->l.v[1], e->l.v[2], value);
    return problem(c, line_now(p), "Only a name, a part or an item can be set");
  }
  if (!tagged(e, "call") && !tagged(e, "say")) {
    if (tagged(e, "var")) {
      const char *v = e->l.v[1]->t.s;
      return problem(
          c, line_now(p), arena_fmt(a, "\"%s\" on its own does nothing. Did you mean %s=... or %s(...)?", v, v, v));
    }
    return problem(
        c, line_now(p), "This line works something out and then throws it away. Set a name to it, or return it");
  }
  return mk(a, "do", 1, e);
}

static val *statement(ps *p) {
  int line = line_now(p);
  GET(s, bare(p));
  mark(p->c, s, line);
  return s;
}

static val *items(ps *p, const char *close) {
  val *out = v_list(p->c->a);
  if (parse_accept(p, close)) return out;
  do {
    if (is(p, close, 0)) break;
    GET(e, expr(p));
    list_push(p->c->a, out, e);
  } while (parse_accept(p, ","));
  if (!expect(p, close, NULL)) return NULL;
  return out;
}

/* use(address, values) is a call of lingo.use, made as the program reaches it: Onsay reaches what the address names
   and gives back its answer. A lingo at an address, like laptop.brandon/test/find, runs there with the
   values, a record by name or a list in order, an MCP tool after a #, any other web address is a plain web request, and
   after an @, where the answer goes. Everything local needs no use: a lingo in the same lingos folder is a dotted call. */
static val *using(cx *c, val *args, int line) {
  arena *a = c->a;
  val *address = at(args, 0);
  val *compile_named = v_rec(a);
  if (!address)
    return problem(
        c, line, "use(...) says what it reaches first, like use(\"laptop.brandon/weather/now\", {place: \"Paris\"})");
  if (args->l.n > 2) return problem(c, line, "use(...) takes an address and its values, and no more");
  if (address->k == V_TEXT && !address->t.len)
    return problem(
        c, line, "use(...) names what it reaches, like use(\"laptop.brandon/weather/now\", {place: \"Paris\"})");
  rec_set(a, compile_named, "address", address);
  rec_set(a, compile_named, "values", args->l.n > 1 ? args->l.v[1] : NOTHING);
  return mk(a, "say", 2, T(a, "lingo.use"), compile_named);
}

/* A call this computer hasn't got, its values in order: it compiles, so it can always be made, and as it runs it asks the
   lingo search (searched), or fails with why, what it would have said here. */
static val *unheard(cx *c, const char *id, val *args, const char *why) {
  val *compile_named = v_rec(c->a);
  for (size_t i = 0; i < args->l.n; i++) rec_set(c->a, compile_named, arena_fmt(c->a, "a%zu", i), args->l.v[i]);
  return mk(c->a, "say", 3, T(c->a, id), compile_named, T(c->a, why));
}

// Values in the order the listing gives them, checked against it, then named for the phrase. While only reading a lingo
// for its listing, what a call reaches isn't known yet, so it is kept as it is.
static val *phrase_node(cx *c, const char *id, val *args, int line) {
  arena *a = c->a;
  val *ph = rec_get(c->phrases, id);
  if (c->scan) return mk(a, "say", 2, T(a, id), args);
  if (!ph) {
    size_t first = strcspn(id, ".");
    int known = 0;
    for (size_t i = 0; i < c->phrases->r.n && !known; i++) known = !strncmp(c->phrases->r.key[i], id, first + 1);
    if (!known) {
      char *path = arena_fmt(a, "%s", id + first + 1);
      for (char *s = path; *s; s++)
        if (*s == '.') *s = '/';
      return unheard(c, id, args,
          arena_fmt(a,
              "There's no lingo or group called %.*s here. To reach one somewhere else, write use(\"its address\"), like use(\"laptop.brandon/%.*s/%s\")",
              (int)first, id, (int)first, id, path));
    }
    const char *dot = strrchr(id, '.');
    size_t glen = dot ? (size_t)(dot - id) : strlen(id);
    buf b = { 0 };
    buf_fmt(&b, "There's no phrase %s.", id);
    int near = 0;
    for (size_t i = 0; i < c->phrases->r.n; i++) {
      const char *k = c->phrases->r.key[i];
      if (strncmp(k, id, glen) || k[glen] != '.') continue;
      if (near++)
        buf_str(&b, ", ");
      else
        buf_fmt(&b, " %.*s has: ", (int)glen, id);
      buf_str(&b, k + glen + 1);
    }
    if (!near) {
      buf_str(&b, " The groups are: ");
      val *groups = v_list(a);
      for (size_t i = 0; i < c->phrases->r.n; i++) {
        const char *k = c->phrases->r.key[i];
        const char *d = strrchr(k, '.');
        push_unique(a, groups, d ? v_text(a, k, (size_t)(d - k)) : T(a, k));
      }
      for (size_t i = 0; i < groups->l.n; i++) {
        if (i) buf_str(&b, ", ");
        buf_str(&b, groups->l.v[i]->t.s);
      }
    }
    char *m = buf_dup(a, &b);
    buf_free(&b);
    return unheard(c, id, args, m);
  }
  val *ps_ = rec_get(ph, "params");
  size_t len = ps_ ? ps_->l.n : 0;
  size_t required = 0;
  for (size_t i = 0; i < len; i++)
    if (!rec_get(ps_->l.v[i], "default")) required++;
  if (args->l.n < required || args->l.n > len) {
    buf b = { 0 };
    buf_fmt(&b, "%s(", id);
    for (size_t i = 0; i < len; i++) {
      val *d = rec_get(ps_->l.v[i], "default");
      if (i) buf_str(&b, ", ");
      buf_str(&b, rec_get(ps_->l.v[i], "name")->t.s);
      if (d) {
        buf_chr(&b, '=');
        buf_json(&b, d);
      }
    }
    if (required == len)
      buf_fmt(&b, ") takes %zu values, not %zu", required, args->l.n);
    else
      buf_fmt(&b, ") takes %zu to %zu values, not %zu", required, len, args->l.n);
    char *m = buf_dup(a, &b);
    buf_free(&b);
    return problem(c, line, m);
  }
  val *compile_named = v_rec(a);
  for (size_t i = 0; i < args->l.n; i++) rec_set(a, compile_named, rec_get(ps_->l.v[i], "name")->t.s, args->l.v[i]);
  return mk(a, "say", 2, T(a, id), compile_named);
}

/* lingo("what it does", {name: value}, "what it gives") is a call of lingo.run, made as the program reaches it: Onsay
   answers it, and the program gets only the answer back. */
static val *wanted(cx *c, val *args, int line) {
  arena *a = c->a;
  val *ask = at(args, 0);
  val *with = at(args, 1);
  val *gives = at(args, 2);
  val *compile_named = v_rec(a);
  if (!ask)
    return problem(
        c, line, "lingo(...) says what it needs first, like lingo(\"the weather in a city\", {city: \"Paris\"})");
  if (args->l.n > 3)
    return problem(c, line, "lingo(...) takes what it needs, its values and what it gives back, and no more");
  if (ask->k == V_TEXT && !ask->t.len)
    return problem(
        c, line, "lingo(...) says what it needs in words, like lingo(\"the weather in a city\", {city: \"Paris\"})");
  rec_set(a, compile_named, "ask", ask);
  rec_set(a, compile_named, "values", with ? with : mk(a, "map", 1, v_rec(a)));
  rec_set(a, compile_named, "gives", gives ? gives : T(a, ""));
  return mk(a, "say", 2, T(a, "lingo.run"), compile_named);
}

/* setting("key") is a call of lingo.setting, made as the program reaches it: this computer answers with this lingo's
   setting of that name, kept here, or its default, or nothing (Lingo settings, in the engine). setting("key", "github") reads another
   lingo's, so one token serves many lingos, but never a secret one. */
static val *setting(cx *c, val *args, int line) {
  arena *a = c->a;
  val *key = at(args, 0);
  val *ref = at(args, 1);
  val *compile_named = v_rec(a);
  if (!key) return problem(c, line, "setting(...) names the setting first, like setting(\"key\")");
  if (args->l.n > 2) return problem(c, line, "setting(...) takes a setting's name and whose it is, and no more");
  if (key->k == V_TEXT && (!ref || is_text(ref, c->own))) push_unique(a, c->settings, key);
  rec_set(a, compile_named, "key", key);
  rec_set(a, compile_named, "reference", ref ? ref : T(a, c->own));
  return mk(a, "say", 2, T(a, "lingo.setting"), compile_named);
}

// {name:"Rex", "two words":1}
static val *record(ps *p) {
  arena *a = p->c->a;
  val *parts = v_rec(a);
  if (parse_accept(p, "}")) return mk(a, "map", 1, parts);
  do {
    if (is(p, "}", 0)) break;
    token *t = peek(p, 0);
    const char *key;
    if (t && (t->k == ID || t->k == KW))
      key = t->v;
    else if (t && t->k == STR && t->count == 1 && !t->parts[0].code)
      key = t->parts[0].s;
    else
      return expected(p, "the name of a part");
    p->i++;
    if (!expect(p, ":", NULL)) return NULL;
    GET(e, expr(p));
    rec_set(a, parts, key, e);
  } while (parse_accept(p, ","));
  if (!expect(p, "}", NULL)) return NULL;
  return mk(a, "map", 1, parts);
}

// "Hi {name}" becomes "Hi " + name. Text first, so + always joins as text.
static val *text(ps *p, token *t) {
  arena *a = p->c->a;
  if (t->count == 1 && !t->parts[0].code) return v_text(a, t->parts[0].s, t->parts[0].n);
  val *out = v_text(a, "", 0);
  for (size_t i = 0; i < t->count; i++) {
    piece *part = &t->parts[i];
    val *e;
    if (!part->code)
      e = v_text(a, part->s, part->n);
    else {
      tokens ts;
      if (!lex(p->c, part->s, part->n, &ts)) return NULL;
      for (size_t k = 0; k < ts.n; k++) ts.t[k].line = part->line;
      ps inner = { p->c, ts.t, ts.n, 0 };
      e = expr(&inner);
      if (!e) return NULL;
      if (!end(&inner)) return expected(&inner, "a } after the value inside the text");
    }
    out = mk(a, "op", 3, T(a, "+"), out, e);
  }
  return out;
}

static val *primary(ps *p) {
  cx *c = p->c;
  arena *a = c->a;
  token *t = peek(p, 0);
  if (!t) return expected(p, "a value");
  if (t->k == NUM) {
    p->i++;
    return v_num(a, t->num);
  }
  if (t->k == STR) {
    p->i++;
    return text(p, t);
  }
  if (parse_accept(p, "true")) return VBOOL(1);
  if (parse_accept(p, "false")) return VBOOL(0);
  if (parse_accept(p, "nothing") || parse_accept(p, "null")) return NOTHING;
  if (parse_accept(p, "(")) {
    GET(e, expr(p));
    if (!expect(p, ")", NULL)) return NULL;
    return e;
  }
  if (parse_accept(p, "[")) {
    GET(xs, items(p, "]"));
    val *out = mk(a, "list", 0);
    for (size_t i = 0; i < xs->l.n; i++) list_push(a, out, xs->l.v[i]);
    return out;
  }
  if (parse_accept(p, "{")) return record(p);
  if (t->k != ID) return expected(p, "a value");

  // a.b.c(...) is a phrase, f(...) is a function, and anything else is a name, with its parts read by postfix.
  // After a dot any word is a name, keywords too, so a phrase can be called dev.fail or a part in.
  size_t j = p->i + 1;
  while (j + 1 < p->n && p->t[j].k == SYM && !strcmp(p->t[j].v, ".") && (p->t[j + 1].k == ID || p->t[j + 1].k == KW))
    j += 2;
  int dotted = j > p->i + 1;
  if (j < p->n && p->t[j].k == SYM && !strcmp(p->t[j].v, "(")) {
    int line = t->line;
    buf b = { 0 };
    for (size_t k = p->i; k < j; k += 2) {
      if (k > p->i) buf_chr(&b, '.');
      buf_str(&b, p->t[k].v);
    }
    char *full = buf_dup(a, &b);
    buf_free(&b);
    p->i = j + 1;
    GET(args, items(p, ")"));
    if (dotted) return phrase_node(c, full, args, line);
    if (!strcmp(full, "lingo")) return wanted(c, args, line);
    if (!strcmp(full, "use")) return using(c, args, line);
    if (!strcmp(full, "setting")) return setting(c, args, line);
    val *node = mk(a, "call", 2, T(a, full), args);
    if (c->ncalls == c->room) {
      c->room = c->room ? c->room * 2 : 16;
      site *more = arena_get(a, c->room * sizeof *more);
      if (c->ncalls) memcpy(more, c->calls, c->ncalls * sizeof *more);
      c->calls = more;
    }
    c->calls[c->ncalls++] = (site){ full, (int)args->l.n, line, node };
    return node;
  }
  p->i++;
  return mk(a, "var", 1, T(a, t->v));
}

static val *postfix(ps *p) {
  GET(e, primary(p));
  for (;;) {
    if (parse_accept(p, ".")) {
      GET(w, word(p, "the name of a part after ."));
      e = mk(p->c->a, "get", 2, e, w);
    } else if (parse_accept(p, "[")) {
      GET(k, expr(p));
      if (!expect(p, "]", NULL)) return NULL;
      e = mk(p->c->a, "get", 2, e, k);
    } else
      return e;
  }
}

// Loosest first: pick, ??, or, and, not, comparing, ranges, adding, multiplying, minus, then calls, parts and items.
static val *unary(ps *p) {
  if (parse_accept(p, "-")) {
    GET(e, unary(p));
    return mk(p->c->a, "neg", 1, e);
  }
  return postfix(p);
}

static val *binary(ps *p, int level);

static val *product(ps *p) {
  GET(e, unary(p));
  for (;;) {
    const char *o = parse_accept(p, "*") ? "*" : parse_accept(p, "/") ? "/" : parse_accept(p, "%") ? "%" : NULL;
    if (!o) return e;
    GET(b, unary(p));
    e = mk(p->c->a, "op", 3, T(p->c->a, o), e, b);
  }
}

static val *sum(ps *p) {
  GET(e, product(p));
  for (;;) {
    const char *o = parse_accept(p, "+") ? "+" : parse_accept(p, "-") ? "-" : NULL;
    if (!o) return e;
    GET(b, product(p));
    e = mk(p->c->a, "op", 3, T(p->c->a, o), e, b);
  }
}

static val *range(ps *p) {
  GET(e, sum(p));
  if (!parse_accept(p, "..")) return e;
  GET(b, sum(p));
  return mk(p->c->a, "range", 2, e, b);
}

static val *compare(ps *p) {
  static const char *ops[] = { "==", "!=", "<=", ">=", "<", ">", "in" };
  GET(e, range(p));
  for (int k = 0; k < 7; k++) {
    if (!parse_accept(p, ops[k])) continue;
    GET(b, range(p));
    return mk(p->c->a, "op", 3, T(p->c->a, ops[k]), e, b);
  }
  return e;
}

static val *not_(ps *p) {
  if (parse_accept(p, "not") || parse_accept(p, "!")) {
    GET(e, not_(p));
    return mk(p->c->a, "not", 1, e);
  }
  return compare(p);
}

// and, or, then ??: each a run of the one below it.
static val *binary(ps *p, int level) {
  static const char *tags[] = { "and", "or", "else" };
  static const char *words[][2] = { { "and", "&&" }, { "or", "||" }, { "??", NULL } };
  GET(e, level ? binary(p, level - 1) : not_(p));
  while (parse_accept(p, words[level][0]) || (words[level][1] && parse_accept(p, words[level][1]))) {
    GET(b, level ? binary(p, level - 1) : not_(p));
    e = mk(p->c->a, tags[level], 2, e, b);
  }
  return e;
}

static val *expr(ps *p) {
  GET(c, binary(p, 2));
  if (!parse_accept(p, "?")) return c;
  GET(a, expr(p));
  if (!expect(p, ":", "a : between the two choices")) return NULL;
  GET(b, expr(p));
  return mk(p->c->a, "pick", 3, c, a, b);
}

// tool and fn may come in any order. Steps with no tool around them are a one-off tool of their own.
static val *program(ps *p) {
  cx *c = p->c;
  arena *a = c->a;
  val *fns = v_rec(a);
  val *loose = v_list(a);
  val *tool = NULL;
  while (!end(p)) {
    if (parse_accept(p, ";")) continue;
    if (is(p, "tool", 0)) {
      int line = line_now(p);
      p->i++;
      if (tool) return problem(c, line, "A program has one tool. Make the other one a fn");
      GET(n, name(p, "the name of the tool"));
      GET(ps_, params(p));
      GET(body, block(p));
      tool = v_rec(a);
      rec_set(a, tool, "name", n);
      rec_set(a, tool, "params", ps_);
      rec_set(a, tool, "body", body);
    } else if (is(p, "fn", 0)) {
      int line = line_now(p);
      p->i++;
      GET(n, name(p, "the name of the function"));
      if (rec_get(fns, n->t.s))
        return problem(c, line_now(p), arena_fmt(a, "There are two functions called %s", n->t.s));
      GET(ps_, params(p));
      GET(body, block(p));
      val *f = v_rec(a);
      rec_set(a, f, "params", ps_);
      rec_set(a, f, "body", body);
      rec_set(a, fns, n->t.s, f);
      rec_set(a, c->fn_lines, n->t.s, v_num(a, line));
    } else {
      GET(s, statement(p));
      list_push(a, loose, s);
    }
  }
  if (tool && loose->l.n) {
    int line = line_of_mark(c, loose->l.v[0]);
    return problem(c, line ? line : 1, "Steps outside the tool. Put them inside it, or inside a fn");
  }
  if (!tool) {
    tool = v_rec(a);
    rec_set(a, tool, "name", T(a, "steps"));
    rec_set(a, tool, "params", v_list(a));
    rec_set(a, tool, "body", loose);
  }
  val *out = v_rec(a);
  rec_set(a, out, "tool", tool);
  rec_set(a, out, "fns", fns);
  return out;
}

// Each statement's place in the tree, like tool/3/0/1, mapped to its line, for errors at run time.
static void walk(cx *c, val *map, val *body, buf *path) {
  size_t base = path->n;
  for (size_t i = 0; body && i < body->l.n; i++) {
    val *s = body->l.v[i];
    path->n = base;
    buf_fmt(path, "/%zu", i);
    size_t here = path->n;
    int line = line_of_mark(c, s);
    if (line) rec_set(c->a, map, path->s, v_num(c->a, line));
    if (tagged(s, "if")) {
      val *branches = s->l.v[1];
      for (size_t j = 0; j < branches->l.n; j++) {
        path->n = here;
        buf_fmt(path, "/%zu", j);
        walk(c, map, branches->l.v[j]->l.v[1], path);
      }
      path->n = here;
      buf_fmt(path, "/%zu", branches->l.n);
      walk(c, map, s->l.v[2], path);
    } else if (tagged(s, "while")) {
      buf_str(path, "/0");
      walk(c, map, s->l.v[2], path);
    } else if (tagged(s, "each")) {
      buf_str(path, "/0");
      walk(c, map, s->l.v[3], path);
    } else if (tagged(s, "try")) {
      buf_str(path, "/0");
      walk(c, map, s->l.v[1], path);
      path->n = here;
      buf_str(path, "/1");
      walk(c, map, s->l.v[3], path);
    }
  }
  path->n = base;
}

static val *paths(cx *c, val *program) {
  val *map = v_rec(c->a);
  val *fns = rec_get(program, "fns");
  buf path = { 0 };
  buf_str(&path, "tool");
  walk(c, map, rec_get(rec_get(program, "tool"), "body"), &path);
  for (size_t i = 0; i < fns->r.n; i++) {
    path.n = 0;
    buf_fmt(&path, "fn:%s", fns->r.key[i]);
    walk(c, map, rec_get(fns->r.v[i], "body"), &path);
  }
  buf_free(&path);
  return map;
}

// 2.
static val *parse_with(cx *c, const char *code, size_t n) {
  arena *a = c->a;
  tokens ts;
  if (!lex(c, code, n, &ts)) return NULL;
  ps p = { c, ts.t, ts.n, 0 };
  GET(prog, program(&p));
  // Bare calls are settled once every function is known: the program's own first, then a phrase whose bare name only
  // one group has, so length(x) is std.length(x).
  val *fns = rec_get(prog, "fns");
  val *by_name = v_rec(a);
  for (size_t i = 0; i < c->phrases->r.n; i++) {
    const char *id = c->phrases->r.key[i];
    const char *d = strrchr(id, '.');
    const char *bare_name = d ? d + 1 : id;
    val *ids = rec_get(by_name, bare_name);
    if (!ids) {
      ids = v_list(a);
      rec_set(a, by_name, bare_name, ids);
    }
    list_push(a, ids, T(a, id));
  }
  for (size_t i = 0; i < c->ncalls && !c->scan; i++) {
    site *s = &c->calls[i];
    val *fn = rec_get(fns, s->name);
    if (!fn) {
      val *ids = rec_get(by_name, s->name);
      if (ids && ids->l.n > 1) {
        buf b = { 0 };
        buf_fmt(&b, "%s is in ", s->name);
        for (size_t k = 0; k < ids->l.n; k++) {
          const char *id = ids->l.v[k]->t.s;
          const char *d = strrchr(id, '.');
          buf_fmt(&b, "%s%.*s", k ? " and " : "", (int)(d - id), id);
        }
        buf_str(&b, ": write ");
        for (size_t k = 0; k < ids->l.n; k++) buf_fmt(&b, "%s%s", k ? " or " : "", ids->l.v[k]->t.s);
        char *m = buf_dup(a, &b);
        buf_free(&b);
        return problem(c, s->line, m);
      }
      val *said = ids ? phrase_node(c, ids->l.v[0]->t.s, s->node->l.v[2], s->line)
                      : unheard(c, s->name, s->node->l.v[2],
                            arena_fmt(a, "There's no function or phrase called %s. Write it with fn %s(...){...}", s->name, s->name));
      if (!said) return NULL;
      s->node->l.n = 0;
      for (size_t k = 0; k < said->l.n; k++) list_push(a, s->node, said->l.v[k]);
      continue;
    }
    val *fps = rec_get(fn, "params");
    int required = 0;
    int len = (int)fps->l.n;
    for (int k = 0; k < len; k++)
      if (fps->l.v[k]->l.n == 1) required++;
    if (s->count < required || s->count > len) {
      if (required == len)
        return problem(c, s->line, arena_fmt(a, "%s takes %d values, not %d", s->name, required, s->count));
      return problem(c, s->line, arena_fmt(a, "%s takes %d to %d values, not %d", s->name, required, len, s->count));
    }
  }
  rec_set(a, prog, "lines", paths(c, prog));
  return prog;
}

////////// 3. LISTING //////////

typedef struct {
  const char *s;
  size_t n;
} span;

// One line of code without the spaces around it.
static span compile_trimmed(span l) {
  while (l.n && (*l.s == ' ' || *l.s == '\t')) {
    l.s++;
    l.n--;
  }
  while (l.n && (l.s[l.n - 1] == ' ' || l.s[l.n - 1] == '\t' || l.s[l.n - 1] == '\r')) l.n--;
  return l;
}

// A line's words as a comment, without its //: 0 when the line isn't only a comment, is an empty //, or is a section's bar.
static int comment(span l, span *words) {
  l = compile_trimmed(l);
  if (l.n < 2 || memcmp(l.s, "//", 2) || (l.n >= 10 && !memcmp(l.s, "//////////", 10))) return 0;
  *words = compile_trimmed((span){ l.s + 2, l.n - 2 });
  return words->n > 0;
}

// The words of the comment lines from first to last, joined by spaces, with a number like 1. or 1-6. at the start, a
// legend's, left off.
static val *words_of(arena *a, span *lines, size_t first, size_t last) {
  buf b = { 0 };
  span w;
  for (size_t i = first; i <= last; i++)
    if (comment(lines[i], &w)) {
      if (b.n) buf_chr(&b, ' ');
      buf_add(&b, w.s, w.n);
    }
  size_t k = 0;
  while (k < b.n && digit(b.s[k])) k++;
  if (k && k < b.n && b.s[k] == '-') {
    size_t m = k + 1;
    while (m < b.n && digit(b.s[m])) m++;
    if (m > k + 1) k = m;
  }
  size_t skip = k && k < b.n && b.s[k] == '.' && (k + 1 == b.n || b.s[k + 1] == ' ') ? k + 1 : 0;
  while (skip && skip < b.n && b.s[skip] == ' ') skip++;
  val *out = v_text(a, b.s ? b.s + skip : "", b.n - skip);
  buf_free(&b);
  return out;
}

/* 3. A lingo written in Lingo is its code, <name>.ls, and its listing is made from it, never written by hand:
     each fn        a function it shares, called at its route, like /memo/recall, its values and their defaults its fn's own
     the comments   right above a fn, what it does. The first lines of the file, when they are comments, up to a blank
                    line or an empty //, are what the lingo is, its about, "memo - " or "memo.ls - " at their start left
                    off, and never a fn's too
     settings       the names of its own settings it reads with setting("key")
   Gives {name, about, phrases: [{id, params, does}], settings}, or NULL with the problem, starting with the file. */
static val *listing(arena *a, const char *file, val *code, const char **why) {
  size_t fl = strlen(file);
  size_t n = fl > 3 && !strcmp(file + fl - 3, ".ls") ? fl - 3 : 0;
  int good = n && n <= 64;   // lower case letters, digits, _ and -, neither of those two first or last
  for (size_t i = 0; good && i < n; i++)
    good = (file[i] >= 'a' && file[i] <= 'z') || digit(file[i]) ||
        ((file[i] == '_' || file[i] == '-') && i && i + 1 < n);
  if (!good)
    return compile_failed(why,
        arena_fmt(a, "%s: a lingo's code is named for it, like notes.ls, its name lower case letters, digits, _ and -",
            file));
  val *name = v_text(a, file, n);
  if (is_text(name, "lingo"))
    return compile_failed(why, arena_fmt(a, "%s: lingo is the language's own name, for lingo(...)", file));
  cx *c = context(a, NULL);
  c->own = name->t.s;
  c->scan = 1;
  val *read = parse_with(c, code->t.s, code->t.len);
  if (!read) return compile_failed(why, arena_fmt(a, "%s line %d: %s", file, c->line, c->message));
  val *tool = rec_get(read, "tool");
  val *fns = rec_get(read, "fns");
  if (!is_text(rec_get(tool, "name"), "steps") || rec_get(tool, "body")->l.n)
    return compile_failed(why, arena_fmt(a, "%s: only fn blocks belong in it, with no tool and no steps outside them", file));
  if (!fns->r.n) return compile_failed(why, arena_fmt(a, "%s: a lingo shares its fn blocks, and it has none", file));

  size_t count = 1;
  for (size_t i = 0; i < code->t.len; i++)
    if (code->t.s[i] == '\n') count++;
  span *lines = arena_get(a, count * sizeof *lines);
  span w;
  for (size_t i = 0, from = 0, l = 0; i <= code->t.len; i++)
    if (i == code->t.len || code->t.s[i] == '\n') {
      lines[l++] = (span){ code->t.s + from, i - from };
      from = i + 1;
    }
  size_t top = 0;
  size_t end;
  while (top < count && !compile_trimmed(lines[top]).n) top++;
  for (end = top; end < count && comment(lines[end], &w);) end++;
  val *out = v_rec(a);
  val *phrases = v_list(a);
  rec_set(a, out, "name", name);
  if (end > top) {
    const char *s = words_of(a, lines, top, end - 1)->t.s;
    const char *lead[] = { arena_fmt(a, "%s - ", file), arena_fmt(a, "%s - ", name->t.s) };
    for (int k = 0; k < 2; k++)
      if (!strncmp(s, lead[k], strlen(lead[k]))) s += strlen(lead[k]);
    rec_set(a, out, "about", T(a, s));
  }
  for (size_t i = 0; i < fns->r.n; i++) {
    const char *fname = fns->r.key[i];
    size_t here = (size_t)rec_get(c->fn_lines, fname)->n - 1;
    size_t first = here;
    while (first > 0 && comment(lines[first - 1], &w)) first--;
    val *given = rec_get(fns->r.v[i], "params");
    val *params = v_list(a);
    val *ph = v_rec(a);
    for (size_t k = 0; k < given->l.n; k++) {
      // A default worked out as it runs, like o={a: 1}, is nothing in its listing, and -2 is its number.
      val *p = v_rec(a);
      val *d = at(given->l.v[k], 1);
      if (tagged(d, "neg") && at(d, 1)->k == V_NUM)
        d = v_num(a, -at(d, 1)->n);
      else if (d && d->k == V_LIST)
        d = NOTHING;
      rec_set(a, p, "name", given->l.v[k]->l.v[0]);
      if (d) rec_set(a, p, "default", d);
      list_push(a, params, p);
    }
    rec_set(a, ph, "id", T(a, arena_fmt(a, "%s.%s", name->t.s, fname)));
    rec_set(a, ph, "params", params);
    rec_set(a, ph, "does", first < here && first != top ? words_of(a, lines, first, here - 1) : T(a, ""));
    list_push(a, phrases, ph);
  }
  rec_set(a, out, "phrases", phrases);
  rec_set(a, out, "settings", c->settings);
  return out;
}

////////// 4. LINK //////////

// Rewrites every expression by its node's shape, so a value that reads "call" is never taken for one. A hook gives a
// node to use instead, or NULL to keep it.
typedef struct rw {
  arena *a;
  val *(*say)(struct rw *r, val *id, val *args);
  val *(*call)(struct rw *r, val *name, val *args);
  void *data;
} rw;

static val *rw_ex(rw *r, val *e) {
  arena *a = r->a;
  if (!e || e->k != V_LIST || !e->l.n) return e;
  if (tagged(e, "var")) return e;
  if (tagged(e, "map")) {
    val *parts = e->l.v[1];
    val *out = v_rec(a);
    for (size_t i = 0; i < parts->r.n; i++) rec_set(a, out, parts->r.key[i], rw_ex(r, parts->r.v[i]));
    return mk(a, "map", 1, out);
  }
  if (tagged(e, "op")) return mk(a, "op", 3, e->l.v[1], rw_ex(r, e->l.v[2]), rw_ex(r, e->l.v[3]));
  if (tagged(e, "call")) {
    val *args = v_list(a);
    val *given = e->l.v[2];
    for (size_t i = 0; i < given->l.n; i++) list_push(a, args, rw_ex(r, given->l.v[i]));
    val *swap = r->call ? r->call(r, e->l.v[1], args) : NULL;
    return swap ? swap : mk(a, "call", 2, e->l.v[1], args);
  }
  if (tagged(e, "say")) {
    val *args = v_rec(a);
    val *given = e->l.v[2];
    for (size_t i = 0; i < given->r.n; i++) rec_set(a, args, given->r.key[i], rw_ex(r, given->r.v[i]));
    val *swap = r->say ? r->say(r, e->l.v[1], args) : NULL;
    if (swap) return swap;
    return e->l.n > 3 ? mk(a, "say", 3, e->l.v[1], args, e->l.v[3]) : mk(a, "say", 2, e->l.v[1], args);
  }
  val *out = v_list(a);   // list, get, and, or, not, neg, pick, else, range: all values
  list_push(a, out, e->l.v[0]);
  for (size_t i = 1; i < e->l.n; i++) list_push(a, out, rw_ex(r, e->l.v[i]));
  return out;
}

static val *rw_body(rw *r, val *b) {
  arena *a = r->a;
  val *out = v_list(a);
  for (size_t i = 0; i < b->l.n; i++) {
    val *s = b->l.v[i];
    val *x;
    if (tagged(s, "set"))
      x = mk(a, "set", 2, s->l.v[1], rw_ex(r, s->l.v[2]));
    else if (tagged(s, "put"))
      x = mk(a, "put", 3, rw_ex(r, s->l.v[1]), rw_ex(r, s->l.v[2]), rw_ex(r, s->l.v[3]));
    else if (tagged(s, "if")) {
      val *branches = v_list(a);
      for (size_t j = 0; j < s->l.v[1]->l.n; j++) {
        val *br = s->l.v[1]->l.v[j];
        val *nb = v_list(a);
        list_push(a, nb, rw_ex(r, br->l.v[0]));
        list_push(a, nb, rw_body(r, br->l.v[1]));
        list_push(a, branches, nb);
      }
      x = mk(a, "if", 2, branches, rw_body(r, s->l.v[2]));
    } else if (tagged(s, "while"))
      x = mk(a, "while", 2, rw_ex(r, s->l.v[1]), rw_body(r, s->l.v[2]));
    else if (tagged(s, "each")) {
      x = mk(a, "each", 3, s->l.v[1], rw_ex(r, s->l.v[2]), rw_body(r, s->l.v[3]));
      for (size_t k = 4; k < s->l.n; k++) list_push(a, x, s->l.v[k]);
    } else if (tagged(s, "try"))
      x = mk(a, "try", 3, rw_body(r, s->l.v[1]), s->l.v[2], rw_body(r, s->l.v[3]));
    else if (tagged(s, "return") || tagged(s, "fail") || tagged(s, "do"))
      x = mk(a, s->l.v[0]->t.s, 1, rw_ex(r, s->l.v[1]));
    else
      x = s;   // break, continue
    list_push(a, out, x);
  }
  return out;
}

static val *rw_fn(rw *r, val *f) {
  arena *a = r->a;
  val *ps_ = v_list(a);
  val *given = rec_get(f, "params");
  val *out = v_rec(a);
  for (size_t i = 0; i < given->l.n; i++) {
    val *p = given->l.v[i];
    val *np = v_list(a);
    list_push(a, np, p->l.v[0]);
    if (p->l.n > 1) list_push(a, np, rw_ex(r, p->l.v[1]));
    list_push(a, ps_, np);
  }
  rec_set(a, out, "params", ps_);
  rec_set(a, out, "body", rw_body(r, rec_get(f, "body")));
  return out;
}

// named: a call to a lingo's own fn gets that fn's lasting name.
static val *renamed(rw *r, val *name, val *args) {
  val *to = rec_get(r->data, name->t.s);
  return to ? mk(r->a, "call", 2, to, args) : NULL;
}

// A compiled lingo's functions under their lasting names, and their lines as places in its file, for errors at run time.
static val *compile_named(arena *a, val *read, val *names, const char *file) {
  val *fns = rec_get(read, "fns");
  val *out_fns = v_rec(a);
  val *out_lines = v_rec(a);
  rw r = { a, NULL, renamed, names };
  for (size_t i = 0; i < fns->r.n; i++) rec_set(a, out_fns, rec_get(names, fns->r.key[i])->t.s, rw_fn(&r, fns->r.v[i]));
  val *lines = rec_get(read, "lines");
  for (size_t i = 0; i < lines->r.n; i++) {
    const char *path = lines->r.key[i];
    if (strncmp(path, "fn:", 3)) continue;
    const char *slash = strchr(path + 3, '/');
    if (slash == path + 3) continue;
    size_t nlen = slash ? (size_t)(slash - path - 3) : strlen(path + 3);
    val *to = rec_get(names, compile_dup(a, path + 3, nlen));
    rec_set(a, out_lines, arena_fmt(a, "fn:%s%s", to->t.s, slash ? slash : ""),
        T(a, arena_fmt(a, "%s line %g", file, lines->r.v[i]->n)));
  }
  val *out = v_rec(a);
  rec_set(a, out, "fns", out_fns);
  rec_set(a, out, "lines", out_lines);
  return out;
}

// A Lingo lingo's code compiled against what it may call, every fn under its lasting name, like memo.recall, with its
// lines as places in its file, for errors at run time.
static val *compile_code(arena *a, val *lingo, val *visible, const char **message) {
  const char *file = rec_get(lingo, "file")->t.s;
  const char *lname = rec_get(lingo, "name")->t.s;
  val *code = rec_get(lingo, "code");
  cx *c = context(a, visible);
  c->own = lname;
  val *read = parse_with(c, code->t.s, code->t.len);
  if (!read) {
    *message = arena_fmt(a, "%s line %d: %s", file, c->line, c->message);
    return NULL;
  }
  val *fns = rec_get(read, "fns");
  val *names = v_rec(a);
  for (size_t i = 0; i < fns->r.n; i++)
    rec_set(a, names, fns->r.key[i], T(a, arena_fmt(a, "%s.%s", lname, fns->r.key[i])));
  return compile_named(a, read, names, file);
}

// stack is {phrases: id -> params, fns, lines}. Each stacked phrase call becomes a call to its body, values in order and
// nothing for any left out so its defaults apply, and only the functions something reaches are pulled in.
typedef struct {
  val *stack, *want;
} linking;

static val *link_say(rw *r, val *id, val *args) {
  linking *l = r->data;
  val *params = rec_get(rec_get(l->stack, "phrases"), id->t.s);
  if (!params) return NULL;
  list_push(r->a, l->want, id);
  val *values = v_list(r->a);
  for (size_t i = 0; i < params->l.n; i++) {
    val *v = rec_get(args, rec_get(params->l.v[i], "name")->t.s);
    list_push(r->a, values, v ? v : NOTHING);
  }
  return mk(r->a, "call", 2, id, values);
}

static val *link_call(rw *r, val *name, val *args) {
  linking *l = r->data;
  (void)args;
  if (rec_get(rec_get(l->stack, "fns"), name->t.s)) list_push(r->a, l->want, name);
  return NULL;
}

// 4. A tool of one step, giving what fn gives for the tool's values, a0, a1 and on, in fn's order, with every function
// written in Lingo it reaches pulled in, so only native phrases are left to call.
val *lingo_call(arena *a, val *inst, const char *fn) {
  val *lingos = rec_get(inst, "lingos");
  val *stack = rec_get(inst, "stack");
  val *own = NULL;
  for (size_t i = 0; i < lingos->r.n && !own; i++) {
    val *ps = rec_get(lingos->r.v[i], "phrases");
    for (size_t k = 0; k < ps->l.n && !own; k++)
      if (is_text(rec_get(ps->l.v[k], "id"), fn)) own = ps->l.v[k];
  }
  if (!own) return NULL;
  val *given = rec_get(own, "params");
  val *params = v_list(a);
  val *args = v_rec(a);
  val *step = v_list(a);
  val *tool = v_rec(a);
  for (size_t i = 0; i < given->l.n; i++) {
    val *name = T(a, arena_fmt(a, "a%zu", i));
    val *p = v_list(a);
    list_push(a, p, name);
    list_push(a, params, p);
    rec_set(a, args, rec_get(given->l.v[i], "name")->t.s, mk(a, "var", 1, name));
  }
  linking l = { stack, v_list(a) };
  rw r = { a, link_say, link_call, &l };
  list_push(a, step, mk(a, "return", 1, mk(a, "say", 2, T(a, fn), args)));
  rec_set(a, tool, "name", T(a, "call"));
  rec_set(a, tool, "params", params);
  rec_set(a, tool, "body", rw_body(&r, step));
  val *fns = v_rec(a);
  val *lines = v_rec(a);
  val *sfns = rec_get(stack, "fns");
  val *slines = rec_get(stack, "lines");
  val *out = v_rec(a);
  while (l.want->l.n) {
    const char *k = l.want->l.v[--l.want->l.n]->t.s;
    if (rec_get(fns, k)) continue;
    val *held = v_rec(a);   // held first, so a body that reaches itself doesn't loop here
    rec_set(a, held, "params", v_list(a));
    rec_set(a, held, "body", v_list(a));
    rec_set(a, fns, k, held);
    rec_set(a, fns, k, rw_fn(&r, rec_get(sfns, k)));
    size_t kn = strlen(k);
    for (size_t i = 0; i < slines->r.n; i++) {
      const char *path = slines->r.key[i];
      if (!strncmp(path, "fn:", 3) && !strncmp(path + 3, k, kn) && path[3 + kn] == '/')
        rec_set(a, lines, path, slines->r.v[i]);
    }
  }
  rec_set(a, out, "tool", tool);
  rec_set(a, out, "fns", fns);
  rec_set(a, out, "lines", lines);
  return out;
}

////////// 5. LOADING //////////

// lingos: its lingo(...) calls. needs: what the native phrases it reaches need, from native, each phrase's by its id.
typedef struct {
  val *fns, *seen, *native, *needs;
  double lingos;
} reaching;

static void reach(arena *a, reaching *g, const char *key);

static val *reach_say(rw *r, val *id, val *args) {
  reaching *g = r->data;
  val *needs = rec_get(g->native, id->t.s);
  (void)args;
  if (!strcmp(id->t.s, "lingo.run")) g->lingos++;
  if (rec_get(g->fns, id->t.s)) reach(r->a, g, id->t.s);
  for (size_t i = 0; needs && i < needs->l.n; i++) push_unique(r->a, g->needs, needs->l.v[i]);
  return NULL;
}

static val *reach_call(rw *r, val *name, val *args) {
  reaching *g = r->data;
  (void)args;
  if (rec_get(g->fns, name->t.s)) reach(r->a, g, name->t.s);
  return NULL;
}

// How many lingo(...) calls a stacked function makes, through every function it reaches, each one written counted once,
// and everything it needs on the device.
static void reach(arena *a, reaching *g, const char *key) {
  if (rec_get(g->seen, key)) return;
  rec_set(a, g->seen, key, VBOOL(1));
  rw r = { a, reach_say, reach_call, g };
  rw_fn(&r, rec_get(g->fns, key));
}

// 5.
val *lingo_load(arena *a, val *natives, val *stacked, const char **message) {
  val *lingos = v_rec(a);
  val *stacked_list = v_list(a);
  for (size_t i = 0; i < natives->l.n; i++) {
    val *l = natives->l.v[i];
    rec_set(a, l, "native", VBOOL(1));
    rec_set(a, lingos, rec_get(l, "name")->t.s, l);
  }
  for (size_t i = 0; i < stacked->l.n; i++) {
    val *file = rec_get(stacked->l.v[i], "file");
    val *l = listing(a, file->t.s, rec_get(stacked->l.v[i], "code"), message);
    val *name = l ? rec_get(l, "name") : NULL;
    if (!l) return NULL;
    // Never over one of the computer's native lingos or their groups, like the base calls' input, which it would hide.
    for (size_t k = 0; k < natives->l.n; k++) {
      val *n = natives->l.v[k];
      val *ps = rec_get(n, "phrases");
      int taken = is_text(rec_get(n, "name"), name->t.s);
      for (size_t m = 0; m < ps->l.n && !taken; m++) {
        const char *id = rec_get(ps->l.v[m], "id")->t.s;
        taken = strlen(id) > name->t.len && !strncmp(id, name->t.s, name->t.len) && id[name->t.len] == '.';
      }
      if (taken) {
        *message = arena_fmt(a, "%s: %s is already a part of %s, this computer's own lingo", file->t.s, name->t.s,
            rec_get(n, "name")->t.s);
        return NULL;
      }
    }
    rec_set(a, l, "file", file);
    rec_set(a, l, "code", rec_get(stacked->l.v[i], "code"));
    list_push(a, stacked_list, l);
    rec_set(a, lingos, name->t.s, l);
  }

  // Each is compiled against every phrase here: the native lingos', core's among them, and every Lingo lingo's in this
  // lingos folder, so one is called by its dotted name, like text.slug(title).
  val *stack = v_rec(a);
  val *sphrases = v_rec(a);
  val *sfns = v_rec(a);
  val *slines = v_rec(a);
  val *visible = v_list(a);
  val *native = v_rec(a);
  rec_set(a, stack, "phrases", sphrases);
  rec_set(a, stack, "fns", sfns);
  rec_set(a, stack, "lines", slines);
  for (size_t i = 0; i < lingos->r.n; i++) {
    val *theirs = rec_get(lingos->r.v[i], "phrases");
    for (size_t k = 0; k < theirs->l.n; k++) {
      // Its values as an inputSchema, those with no default required.
      val *p = theirs->l.v[k];
      val *params = rec_get(p, "params");
      val *schema = v_rec(a);
      val *props = v_rec(a);
      val *required = v_list(a);
      if (rec_get(p, "needs")) rec_set(a, native, rec_get(p, "id")->t.s, rec_get(p, "needs"));
      for (size_t m = 0; m < params->l.n; m++) {
        val *given = rec_get(params->l.v[m], "type");
        val *one = given ? type_schema(a, given->t.s) : v_rec(a);
        val *d = rec_get(params->l.v[m], "default");
        val *pn = rec_get(params->l.v[m], "name");
        if (d)
          rec_set(a, one, "default", d);
        else
          list_push(a, required, pn);
        rec_set(a, props, pn->t.s, one);
      }
      rec_set(a, schema, "type", T(a, "object"));
      rec_set(a, schema, "properties", props);
      rec_set(a, schema, "required", required);
      rec_set(a, p, "inputSchema", schema);
      list_push(a, visible, p);
    }
  }
  for (size_t i = 0; i < stacked_list->l.n; i++) {
    val *l = stacked_list->l.v[i];
    val *own = rec_get(l, "phrases");
    val *done = compile_code(a, l, visible, message);
    if (!done) return NULL;
    val *fns = rec_get(done, "fns");
    val *lines = rec_get(done, "lines");
    for (size_t k = 0; k < fns->r.n; k++) rec_set(a, sfns, fns->r.key[k], fns->r.v[k]);
    for (size_t k = 0; k < lines->r.n; k++) rec_set(a, slines, lines->r.key[k], lines->r.v[k]);
    for (size_t k = 0; k < own->l.n; k++)
      rec_set(a, sphrases, rec_get(own->l.v[k], "id")->t.s, rec_get(own->l.v[k], "params"));
  }
  for (size_t i = 0; i < stacked_list->l.n; i++) {
    val *phrases = rec_get(stacked_list->l.v[i], "phrases");
    for (size_t k = 0; k < phrases->l.n; k++) {
      reaching g = { sfns, v_rec(a), native, v_list(a), 0 };
      reach(a, &g, rec_get(phrases->l.v[k], "id")->t.s);
      rec_set(a, phrases->l.v[k], "lingos", v_num(a, g.lingos));
      rec_set(a, phrases->l.v[k], "needs", g.needs);
    }
  }

  val *inst = v_rec(a);
  rec_set(a, inst, "lingos", lingos);
  rec_set(a, inst, "stack", stack);
  return inst;
}

//////////////////// 4. RUNNING ////////////////////

// Runs a Lingo tree: its statements, expressions and phrases, within its limits.
//
// 1. fail(r, code, fmt), tick(r)       -> errors, and the step, time and memory limits
// 2. op(r, o, a, b), part, order       -> how values meet
// 3. exec(r, body, frame, back)        -> statements, keeping where they are for errors
// 4. eval(r, e, frame)                 -> expressions
// 5. run_program(a, program, input, l) -> a whole program, to its value or its error

#define MEMORY (512u * 1024 * 1024)   // a program that makes more than this is stopped
#define DEEP 1000   // how deep functions may call each other

enum {
  T_VAR = 1,
  T_LIST,
  T_MAP,
  T_GET,
  T_OP,
  T_AND,
  T_OR,
  T_NOT,
  T_NEG,
  T_PICK,
  T_ELSE,
  T_RANGE,
  T_CALL,
  T_SAY,
  T_SET,
  T_PUT,
  T_IF,
  T_WHILE,
  T_EACH,
  T_BREAK,
  T_NEXT,
  T_RETURN,
  T_FAIL,
  T_TRY,
  T_DO,
  T_BAD
};
static const char *TAGS[] = { "", "var", "list", "map", "get", "op", "and", "or", "not", "neg", "pick", "else", "range",
  "call", "say", "set", "put", "if", "while", "each", "break", "next", "return", "fail", "try", "do" };

enum { GO, BREAK, NEXT, BACK };   // why a block ended
typedef struct {
  jmp_buf j;
} trap_t;

static val *eval(run *r, val *e, val *frame);
static int exec(run *r, val *body, val *frame, val **back);

// Each node's name is read once and kept as a number in it, so running it again is a switch.
static int tag_of(val *node) {
  if (node->tag) return node->tag;
  node->tag = T_BAD;
  if (node->l.n && node->l.v[0]->k == V_TEXT)
    for (int i = 1; i < T_BAD; i++)
      if (!strcmp(node->l.v[0]->t.s, TAGS[i])) {
        node->tag = i;
        break;
      }
  return node->tag;
}

static val *item(val *node, size_t i) { return i < node->l.n ? node->l.v[i] : NOTHING; }
static const char *name_of(val *v) { return v->k == V_TEXT ? v->t.s : ""; }

////////// 1. ERRORS AND LIMITS //////////

static const char *phrase_now;   // the phrase running, so its errors say which it was
static void *volatile running;   // the innermost run on the job thread, so a crash inside it ends only it

// 1.
_Noreturn void fail(run *r, const char *code, const char *fmt, ...) {
  snprintf(r->code, sizeof r->code, "%s", code);
  r->message.n = 0;
  if (!strcmp(code, "phrase") && phrase_now) buf_fmt(&r->message, "%s: ", phrase_now);
  char text[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  buf_str(&r->message, text);
  r->at_error.n = 0;
  buf_add(&r->at_error, r->at.s ? r->at.s : "", r->at.n);
  phrase_now = NULL;
  longjmp(((trap_t *)r->trap)->j, 1);
}

_Noreturn static void rethrow(run *r) { longjmp(((trap_t *)r->trap)->j, 1); }

// The code under a phrase crashed: this request ends with an error, and everything else goes on.
static void crashed(const char *what) {
  const char *where = phrase_now ? phrase_now : "This lingo";
  fail(running, "crash", "%s crashed on %s, so it was stopped. It can be tried again", where, what);
}

double num_of(run *r, val *x, const char *what) {
  if (x->k != V_NUM || isnan(x->n)) {
    if (what) fail(r, "type", "Can't use %s with %s", what, kind_name(x));
    fail(r, "type", "Expected a number, but this is %s", kind_name(x));
  }
  return x->n;
}

// Before anything big is made, so a list that doubles each step is stopped before it is made, not after.
static void room(run *r, size_t more) {
  if (arena_used(r->a) + more > MEMORY) fail(r, "limit", "It used too much memory, so it was stopped");
}

static void tick(run *r) {
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (--r->left < 0) fail(r, "limit", "It took too many steps, so it was stopped. Is a loop never ending?");
  room(r, 0);
  if ((r->left & 1023) == 0 && pl_now() * 1000 > r->until) fail(r, "limit", "It ran too long, so it was stopped");
}

static void at_cut(run *r, size_t n) {
  r->at.n = n;
  if (r->at.s) r->at.s[n] = 0;
}

////////// 2. VALUES MEETING //////////

val *text_of(run *r, val *x) {
  if (x->k == V_TEXT) return x;
  buf b = { 0 };
  buf_text(&b, x);
  val *t = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return t;
}

static int whole(val *x) { return x->k == V_NUM && x->n == floor(x->n) && fabs(x->n) < 9007199254740992.0; }

val *bytes_of(run *r, val *x) {
  if (x->k == V_BYTES) return x;
  if (x->k == V_TEXT) return v_bytes(r->a, x->t.s, x->t.len);
  if (x->k != V_LIST) fail(r, "phrase", "Expected bytes, but this is %s", kind_name(x));
  room(r, x->l.n);
  char *s = arena_get(r->a, x->l.n + 1);
  for (size_t i = 0; i < x->l.n; i++) {
    val *n = x->l.v[i];
    if (!whole(n) || n->n < 0 || n->n > 255) fail(r, "phrase", "a byte is a whole number from 0 to 255");
    s[i] = (char)(unsigned char)n->n;
  }
  val *out = v_bytes(r->a, "", 0);
  out->t.s = s;
  out->t.len = x->l.n;
  return out;
}

// Reading something missing gives nothing, never an error. Negative positions count from the end.
static val *part(run *r, val *from, val *key) {
  if (from->k == V_LIST || from->k == V_TEXT) {
    if (!whole(key)) return NOTHING;
    double len = from->k == V_LIST ? (double)from->l.n : (double)utf8_count(from->t.s, from->t.len);
    double i = key->n < 0 ? len + key->n : key->n;
    if (i < 0 || i >= len) return NOTHING;
    if (from->k == V_LIST) return from->l.v[(size_t)i];
    size_t a = utf8_offset(from->t.s, from->t.len, (size_t)i);
    size_t b = utf8_offset(from->t.s, from->t.len, (size_t)i + 1);
    return v_text(r->a, from->t.s + a, b - a);
  }
  if (from->k == V_BYTES) {
    if (!whole(key)) return NOTHING;
    double i = key->n < 0 ? (double)from->t.len + key->n : key->n;
    if (i < 0 || i >= (double)from->t.len) return NOTHING;
    return v_num(r->a, (unsigned char)from->t.s[(size_t)i]);
  }
  if (from->k == V_REC) {
    val *v = rec_get(from, text_of(r, key)->t.s);
    return v ? v : NOTHING;
  }
  return NOTHING;
}

static int order(run *r, val *a, val *b) {
  if (a->k == V_NUM && b->k == V_NUM) {
    if (a->n < b->n) return -1;
    return a->n > b->n;
  }
  if (a->k == V_TEXT && b->k == V_TEXT) {
    size_t n = a->t.len < b->t.len ? a->t.len : b->t.len;
    int c = memcmp(a->t.s, b->t.s, n);
    if (c) return c < 0 ? -1 : 1;
    if (a->t.len < b->t.len) return -1;
    return a->t.len > b->t.len;
  }
  fail(r, "type", "Can't compare %s with %s", kind_name(a), kind_name(b));
  return 0;
}

static int contains(const char *hay, size_t n, const char *needle, size_t m) {
  if (!m) return 1;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m)) return 1;
  return 0;
}

// 2.
static val *op(run *r, const char *o, val *a, val *b) {
  if (!strcmp(o, "+")) {
    if (a->k == V_NUM && b->k == V_NUM) return v_num(r->a, a->n + b->n);
    if (a->k == V_LIST && b->k == V_LIST) {
      room(r, 2 * (a->l.n + b->l.n) * sizeof(val *));
      val *out = v_list(r->a);
      for (size_t i = 0; i < a->l.n; i++) list_push(r->a, out, a->l.v[i]);
      for (size_t i = 0; i < b->l.n; i++) list_push(r->a, out, b->l.v[i]);
      return out;
    }
    if (a->k == V_BYTES && (b->k == V_BYTES || b->k == V_LIST)) {
      val *more = bytes_of(r, b);
      room(r, a->t.len + more->t.len);
      val *out = v_bytes(r->a, "", 0);
      out->t.s = arena_get(r->a, a->t.len + more->t.len + 1);
      memcpy(out->t.s, a->t.s, a->t.len);
      memcpy(out->t.s + a->t.len, more->t.s, more->t.len);
      out->t.len = a->t.len + more->t.len;
      return out;
    }
    if (a->k == V_TEXT || b->k == V_TEXT) {
      room(r, 2 * ((a->k == V_TEXT ? a->t.len : 0) + (b->k == V_TEXT ? b->t.len : 0)));
      buf t = { 0 };
      buf_text(&t, a);
      buf_text(&t, b);
      val *out = v_text(r->a, t.s ? t.s : "", t.n);
      buf_free(&t);
      return out;
    }
    fail(r, "type", "Can't add %s and %s", kind_name(a), kind_name(b));
  }
  if (!strcmp(o, "-")) return v_num(r->a, num_of(r, a, o) - num_of(r, b, o));
  if (!strcmp(o, "*")) return v_num(r->a, num_of(r, a, o) * num_of(r, b, o));
  if (!strcmp(o, "/")) {
    double x = num_of(r, a, o);
    double y = num_of(r, b, o);
    if (y == 0) fail(r, "type", "Can't divide by zero");
    return v_num(r->a, x / y);
  }
  if (!strcmp(o, "%")) {
    double x = num_of(r, a, o);
    double y = num_of(r, b, o);
    if (y == 0) fail(r, "type", "Can't divide by zero");
    // The divisor's sign, added only when needed, so n - n % 1 is exactly the whole number below n.
    double m = fmod(x, y);
    return v_num(r->a, m != 0 && (m < 0) != (y < 0) ? m + y : m);
  }
  if (!strcmp(o, "==")) return VBOOL(v_same(a, b));
  if (!strcmp(o, "!=")) return VBOOL(!v_same(a, b));
  if (!strcmp(o, "<")) return VBOOL(order(r, a, b) < 0);
  if (!strcmp(o, ">")) return VBOOL(order(r, a, b) > 0);
  if (!strcmp(o, "<=")) return VBOOL(order(r, a, b) <= 0);
  if (!strcmp(o, ">=")) return VBOOL(order(r, a, b) >= 0);
  if (!strcmp(o, "in")) {
    if (b->k == V_LIST) {
      for (size_t i = 0; i < b->l.n; i++)
        if (v_same(b->l.v[i], a)) return VBOOL(1);
      return VBOOL(0);
    }
    if (b->k == V_TEXT) {
      val *t = text_of(r, a);
      return VBOOL(contains(b->t.s, b->t.len, t->t.s, t->t.len));
    }
    if (b->k == V_REC) return VBOOL(rec_get(b, text_of(r, a)->t.s) != NULL);
    if (b->k == V_BYTES) {
      if (a->k == V_NUM) return VBOOL(whole(a) && a->n >= 0 && a->n <= 255 && memchr(b->t.s, (int)a->n, b->t.len));
      val *m = bytes_of(r, a);
      return VBOOL(contains(b->t.s, b->t.len, m->t.s, m->t.len));
    }
    return VBOOL(0);
  }
  fail(r, "type", "There is no operator %s", o);
  return NOTHING;
}

////////// 3. STATEMENTS //////////

// The values a function or tool was given, by position or by name, and its defaults for the rest, worked out in order so
// a default can use an earlier value.
static void bind_args(run *r, val *params, val *given, int named, val *frame) {
  for (size_t i = 0; i < params->l.n; i++) {
    val *p = params->l.v[i];
    const char *name = name_of(item(p, 0));
    val *v = NULL;
    if (named && given && given->k == V_REC) v = rec_get(given, name);
    if (!named && given && i < given->l.n) v = given->l.v[i];
    if (!v || v->k == V_NOTHING) v = p->l.n > 1 ? eval(r, p->l.v[1], frame) : NOTHING;
    rec_set(r->a, frame, name, v_copy(r->a, v));
  }
}

// 3.
static int exec(run *r, val *body, val *frame, val **back) {
  size_t base = r->at.n;
  for (size_t i = 0; i < body->l.n; i++) {
    at_cut(r, base);
    buf_fmt(&r->at, "/%zu", i);
    val *s = body->l.v[i];
    tick(r);
    switch (tag_of(s)) {
      case T_SET: {
        // list+=[x] grows the list where it is, instead of copying it every time.
        const char *name = name_of(item(s, 1));
        val *value = item(s, 2);
        val *now = rec_get(frame, name);
        if (value->k == V_LIST && tag_of(value) == T_OP && !strcmp(name_of(item(value, 1)), "+") &&
            item(value, 2)->k == V_LIST && tag_of(item(value, 2)) == T_VAR &&
            !strcmp(name_of(item(item(value, 2), 1)), name) && now && now->k == V_LIST && item(value, 3)->k == V_LIST &&
            tag_of(item(value, 3)) == T_LIST) {
          val *adds = item(value, 3);
          for (size_t j = 1; j < adds->l.n; j++) list_push(r->a, now, v_copy(r->a, eval(r, adds->l.v[j], frame)));
        } else
          rec_set(r->a, frame, name, v_copy(r->a, eval(r, value, frame)));
        break;
      }
      case T_PUT: {
        val *into = eval(r, item(s, 1), frame);
        val *key = eval(r, item(s, 2), frame);
        val *v = v_copy(r->a, eval(r, item(s, 3), frame));
        if (into->k == V_LIST && whole(key)) {
          double n = (double)into->l.n;
          double at = key->n < 0 ? n + key->n : key->n;
          if (at < 0 || at > n) {
            buf k = { 0 };
            buf_num(&k, key->n);
            fail(r, "type", "There is no item %s in a list of %zu", k.s, into->l.n);
          }
          if (at == n)
            list_push(r->a, into, v);
          else
            into->l.v[(size_t)at] = v;
        } else if (into->k == V_REC)
          rec_set(r->a, into, text_of(r, key)->t.s, v);
        else
          fail(r, "type", "Can't set %s of %s", text_of(r, key)->t.s, kind_name(into));
        break;
      }
      case T_IF: {
        val *branches = item(s, 1);
        size_t mark = r->at.n;
        int done = 0;
        for (size_t j = 0; j < branches->l.n && !done; j++) {
          val *br = branches->l.v[j];
          if (truthy(eval(r, item(br, 0), frame))) {
            at_cut(r, mark);
            buf_fmt(&r->at, "/%zu", j);
            int out = exec(r, item(br, 1), frame, back);
            if (out) return out;
            done = 1;
          }
        }
        if (!done) {
          at_cut(r, mark);
          buf_fmt(&r->at, "/%zu", branches->l.n);
          int out = exec(r, item(s, 2), frame, back);
          if (out) return out;
        }
        break;
      }
      case T_WHILE: {
        size_t mark = r->at.n;
        while (truthy(eval(r, item(s, 1), frame))) {
          at_cut(r, mark);
          buf_str(&r->at, "/0");
          int out = exec(r, item(s, 2), frame, back);
          if (out == BREAK) break;
          if (out == BACK) return out;
          tick(r);
          at_cut(r, mark);
        }
        break;
      }
      case T_EACH: {
        // ['each', item, source, body, key?]: a list gives position and item, a record part and value, a range each count.
        const char *as = name_of(item(s, 1));
        val *source = item(s, 2);
        val *loop = item(s, 3);
        const char *key = s->l.n > 4 ? name_of(s->l.v[4]) : NULL;
        size_t mark = r->at.n;
        size_t count = 0;
        val **keys = NULL;
        val **items = NULL;
        double from = 0;
        double to = -1;
        int counted = source->k == V_LIST && tag_of(source) == T_RANGE;
        if (counted) {
          from = num_of(r, eval(r, item(source, 1), frame), NULL);
          to = num_of(r, eval(r, item(source, 2), frame), NULL);
        } else {
          // What it goes through is fixed before the first turn, so changing it inside the loop changes nothing here.
          val *x = eval(r, source, frame);
          if (x->k == V_LIST) {
            count = x->l.n;
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0; j < count; j++) {
              items[j] = x->l.v[j];
              keys[j] = v_num(r->a, (double)j);
            }
          } else if (x->k == V_REC) {
            count = x->r.n;
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0; j < count; j++) {
              val *k = v_cstr(r->a, x->r.key[j]);
              if (key) {
                keys[j] = k;
                items[j] = x->r.v[j];
              } else {
                keys[j] = NOTHING;
                items[j] = k;
              }
            }
          } else if (x->k == V_BYTES) {
            count = x->t.len;
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0; j < count; j++) {
              items[j] = v_num(r->a, (unsigned char)x->t.s[j]);
              keys[j] = v_num(r->a, (double)j);
            }
          } else if (x->k == V_TEXT) {
            count = utf8_count(x->t.s, x->t.len);
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0, at = 0; j < count; j++) {
              size_t next = at +
                  utf8_offset(x->t.s + at, x->t.len - at, 1);   // from the last, so a long text is gone through once
              items[j] = v_text(r->a, x->t.s + at, next - at);
              keys[j] = v_num(r->a, (double)j);
              at = next;
            }
          } else if (x->k != V_NOTHING)
            fail(r, "type", "Can't go through each item of %s", kind_name(x));
        }
        double turns = counted ? (to >= from ? floor(to - from) + 1 : 0) : (double)count;
        for (double t = 0; t < turns; t++) {
          if (counted) {
            rec_set(r->a, frame, as, v_num(r->a, from + t));
            if (key) rec_set(r->a, frame, key, v_num(r->a, t));
          } else {
            rec_set(r->a, frame, as, v_copy(r->a, items[(size_t)t]));
            if (key) rec_set(r->a, frame, key, keys[(size_t)t]);
          }
          at_cut(r, mark);
          buf_str(&r->at, "/0");
          int out = exec(r, loop, frame, back);
          tick(r);
          if (out == BREAK) break;
          if (out == BACK) return out;
        }
        break;
      }
      case T_BREAK: return BREAK;
      case T_NEXT: return NEXT;
      case T_RETURN: *back = eval(r, item(s, 1), frame); return BACK;
      case T_FAIL: fail(r, "fail", "%s", text_of(r, eval(r, item(s, 1), frame))->t.s); break;
      case T_TRY: {
        trap_t t;
        void *volatile prev = r->trap;
        volatile int depth = r->depth;
        const char *volatile lingo = r->lingo;
        volatile int catching = r->catching;
        size_t mark = r->at.n;
        r->trap = &t;
        r->catching++;
        if (!setjmp(t.j)) {
          buf_str(&r->at, "/0");
          int out = exec(r, item(s, 1), frame, back);
          r->trap = prev;
          r->catching = catching;
          if (out) return out;
          break;
        }
        r->trap = prev;
        r->catching = catching;
        r->depth = depth;
        r->lingo = lingo;   // an error caught from another lingo's function never leaves this code running as it
        if (!strcmp(r->code, "limit") || !strcmp(r->code, "denied") || !strcmp(r->code, "stopped") ||
            !strcmp(r->code, "crash"))
          rethrow(r);
        rec_set(r->a, frame, name_of(item(s, 2)), v_text(r->a, r->message.s ? r->message.s : "", r->message.n));
        at_cut(r, mark);
        buf_str(&r->at, "/1");
        int out = exec(r, item(s, 3), frame, back);
        if (out) return out;
        break;
      }
      case T_DO: eval(r, item(s, 1), frame); break;
      default: fail(r, "type", "This lingo doesn't know the step \"%s\"", name_of(item(s, 0)));
    }
  }
  return GO;
}

////////// 4. EXPRESSIONS //////////

// A call of another lingo's function in the same lingos folder, by its dotted name.
static val *call_fn(run *r, const char *name, val *args) {
  val *fns = rec_get(r->program, "fns");
  val *fn = fns ? rec_get(fns, name) : NULL;
  if (!fn) fail(r, "type", "There is no function %s", name);
  if (++r->depth > DEEP) fail(r, "limit", "A function called itself more than 1000 deep, so it was stopped");
  buf saved = { 0 };
  buf_add(&saved, r->at.s ? r->at.s : "", r->at.n);
  val *frame = v_rec(r->a);
  val *back = NOTHING;
  const char *was = r->lingo;
  bind_args(r, rec_get(fn, "params"), args, 0, frame);
  r->lingo = v_text(r->a, name, strcspn(name, "."))->t.s;   // memo.recall is memo's
  at_cut(r, 0);
  buf_fmt(&r->at, "fn:%s", name);
  int out = exec(r, rec_get(fn, "body"), frame, &back);
  at_cut(r, 0);
  buf_add(&r->at, saved.s ? saved.s : "", saved.n);   // only on success, so an error inside still points inside
  buf_free(&saved);
  r->lingo = was;
  r->depth--;
  return out == BACK ? back : NOTHING;
}

static int searchable(run *r, const char *id);
static val *searched(run *r, const char *id, val *values, const char *code, const char *message);

/* A phrase, or a call this computer hasn't got, which asks the lingo search, as one whose phrase fails does: why is what
   the compiler would have said of a missing one. */
static val *say(run *r, const char *id, val *args, val *frame, val *why) {
  phrase *p = phrase_find(id);
  if (!p) {
    val *given = v_list(r->a);
    for (size_t i = 0; args && args->k == V_REC && i < args->r.n; i++) list_push(r->a, given, eval(r, args->r.v[i], frame));
    return searched(r, id, given, "unknown", why ? why->t.s : arena_fmt(r->a, "This computer doesn't have the phrase %s", id));
  }
  for (const char *n = p->needs; *n; n += strspn(n, ", ")) {   // everything it needs, like screen.act, screen.read
    char need[32];
    size_t len = strcspn(n, ",");
    snprintf(need, sizeof need, "%.*s", (int)len, n);
    if (strcmp(need, "none")) may(r, need, NULL);
    n += len;
  }
  // A value left out, or nothing, takes its default, as a function's does.
  val **values = arena_get(r->a, (size_t)(p->count + 1) * sizeof *values);
  for (int i = 0; i < p->count; i++) {
    val *given = args && args->k == V_REC ? rec_get(args, p->params[i]) : NULL;
    values[i] = given ? eval(r, given, frame) : NOTHING;
    if (values[i]->k == V_NOTHING && p->defaults[i]) values[i] = p->defaults[i];
  }
  const char *was =
      phrase_now;   // a phrase may run others inside it, like lingo.run the requests Onsay sends meanwhile
  const char *was_lingo = r->lingo;
  phrase_now = id;
  if (p->lingo) r->lingo = native_name(p->lingo);   // a native lingo's phrase runs as that lingo
  val *out;
  if (!searchable(r, id))
    out = p->fn(r, values);
  else {
    trap_t t;
    void *volatile prev = r->trap;
    volatile int depth = r->depth;
    r->trap = &t;
    if (!setjmp(t.j)) {
      out = p->fn(r, values);
      r->trap = prev;
    } else {
      r->trap = prev;
      r->depth = depth;
      phrase_now = was;
      r->lingo = was_lingo;
      if (strcmp(r->code, "phrase")) rethrow(r);
      val *named = v_rec(r->a);
      for (int i = 0; i < p->count; i++) rec_set(r->a, named, p->params[i], values[i]);
      out = searched(r, id, named, "phrase", arena_fmt(r->a, "%.*s", (int)r->message.n, r->message.s ? r->message.s : ""));
    }
  }
  phrase_now = was;
  r->lingo = was_lingo;
  return out ? out : NOTHING;
}

// 4.
static val *eval(run *r, val *e, val *frame) {
  if (e->k != V_LIST) return e;   // nothing, true, false, a number or text
  tick(r);
  switch (tag_of(e)) {
    case T_VAR: {
      val *v = rec_get(frame, name_of(item(e, 1)));
      return v ? v : NOTHING;
    }
    case T_LIST: {
      val *out = v_list(r->a);
      for (size_t i = 1; i < e->l.n; i++) list_push(r->a, out, eval(r, e->l.v[i], frame));
      return out;
    }
    case T_MAP: {
      val *parts = item(e, 1);
      val *out = v_rec(r->a);
      if (parts->k == V_REC)
        for (size_t i = 0; i < parts->r.n; i++) rec_set(r->a, out, parts->r.key[i], eval(r, parts->r.v[i], frame));
      return out;
    }
    case T_GET: {
      val *from = eval(r, item(e, 1), frame);
      return part(r, from, eval(r, item(e, 2), frame));
    }
    case T_OP: {
      val *a = eval(r, item(e, 2), frame);
      return op(r, name_of(item(e, 1)), a, eval(r, item(e, 3), frame));
    }
    case T_AND: return truthy(eval(r, item(e, 1), frame)) ? eval(r, item(e, 2), frame) : VBOOL(0);
    case T_OR: {
      val *a = eval(r, item(e, 1), frame);
      return truthy(a) ? a : eval(r, item(e, 2), frame);
    }
    case T_NOT: return VBOOL(!truthy(eval(r, item(e, 1), frame)));
    case T_NEG: return v_num(r->a, -num_of(r, eval(r, item(e, 1), frame), NULL));
    case T_PICK: return eval(r, truthy(eval(r, item(e, 1), frame)) ? item(e, 2) : item(e, 3), frame);
    case T_ELSE: {
      val *a = eval(r, item(e, 1), frame);
      return a->k == V_NOTHING ? eval(r, item(e, 2), frame) : a;
    }
    case T_RANGE: {
      double from = num_of(r, eval(r, item(e, 1), frame), NULL);
      double to = num_of(r, eval(r, item(e, 2), frame), NULL);
      val *out = v_list(r->a);
      for (double i = from; i <= to; i++) {
        list_push(r->a, out, v_num(r->a, i));
        tick(r);
      }
      return out;
    }
    case T_CALL: {
      val *given = item(e, 2);
      val *args = v_list(r->a);
      for (size_t i = 0; i < given->l.n; i++) list_push(r->a, args, eval(r, given->l.v[i], frame));
      return call_fn(r, name_of(item(e, 1)), args);
    }
    case T_SAY: return say(r, name_of(item(e, 1)), item(e, 2), frame, e->l.n > 3 ? item(e, 3) : NULL);
    default: fail(r, "type", "This lingo doesn't know the expression \"%s\"", name_of(item(e, 0)));
  }
  return NOTHING;
}

////////// 5. RUN //////////

// 5. The run is kept in the arena, never on the stack, so what an error left in it is read whole after the jump back.
val *run_program(arena *a, val *program, val *input, limits l, const char **code, const char **message) {
  run *r = arena_get(a, sizeof *r);
  *r = (run){ .a = a, .program = program, .left = l.steps, .until = pl_now() * 1000 + l.ms, .stop = l.stop };
  trap_t t;
  r->trap = &t;
  val *back = NOTHING;
  val *volatile out = NULL;
  void *volatile outer = running;
  static int guarded;
  if (!guarded) pl_faults(&running, crashed);
  guarded = 1;
  running = r;
  if (!setjmp(t.j)) {
    val *tool = rec_get(program, "tool");
    val *frame = v_rec(a);
    buf_str(&r->at, "tool");
    bind_args(r, rec_get(tool, "params"), input, 1, frame);
    out = exec(r, rec_get(tool, "body"), frame, &back) == BACK ? back : NOTHING;
  } else {
    const char *line = text_or(rec_get(program, "lines"), r->at_error.s ? r->at_error.s : "", NULL);
    buf m = { 0 };
    if (line) buf_fmt(&m, "%s: ", line);
    buf_add(&m, r->message.s ? r->message.s : "", r->message.n);
    *code = v_cstr(a, r->code)->t.s;
    *message = v_text(a, m.s, m.n)->t.s;
    buf_free(&m);
    pl_fault_over();
  }
  running = outer;
  buf_free(&r->message);
  buf_free(&r->at);
  buf_free(&r->at_error);
  return out;
}

//////////////////// 5. STANDARD ////////////////////

// The std group of the base calls: what Lingo can't write for itself, reaching inside values and converting them.
// Everything else for text, lists, numbers and dates is written in Lingo, in ../../lingos/standard.
//
// 1. Inside values   length, keys, slice, find: counted by characters for text, not bytes
// 2. Converting      number, text, to_json, from_json, bytes, from_bytes
// 3. std_phrases()   -> adds them all

////////// 1. INSIDE VALUES //////////

static val *list_of(run *r, val *x) {
  if (x->k != V_LIST) fail(r, "type", "Expected a list, but this is %s", kind_name(x));
  return x;
}

static val *len(run *r, val **a) {
  val *x = a[0];
  size_t n = 0;
  if (x->k == V_LIST) n = x->l.n;
  if (x->k == V_TEXT) n = utf8_count(x->t.s, x->t.len);
  if (x->k == V_BYTES) n = x->t.len;
  if (x->k == V_REC) n = x->r.n;
  return v_num(r->a, (double)n);
}

static val *std_keys(run *r, val **a) {
  val *out = v_list(r->a);
  if (a[0]->k == V_REC)
    for (size_t i = 0; i < a[0]->r.n; i++) list_push(r->a, out, v_cstr(r->a, a[0]->r.key[i]));
  return out;
}

// Like JavaScript's slice: negative counts from the end, and both ends are kept inside the list or text.
static size_t edge(run *r, val *x, double n, size_t fallback) {
  if (x->k == V_NOTHING) return fallback;
  double i = trunc(num_of(r, x, NULL));
  if (i < 0) i += n;
  if (i < 0) return 0;
  if (i > n) return (size_t)n;
  return (size_t)i;
}

static val *slice(run *r, val **a) {
  val *x = a[0];
  if (x->k == V_TEXT) {
    size_t n = utf8_count(x->t.s, x->t.len);
    size_t from = edge(r, a[1], (double)n, 0);
    size_t to = edge(r, a[2], (double)n, n);
    if (to <= from) return v_text(r->a, "", 0);
    size_t at = utf8_offset(x->t.s, x->t.len, from);
    return v_text(r->a, x->t.s + at, utf8_offset(x->t.s + at, x->t.len - at, to - from));
  }
  if (x->k == V_BYTES) {
    size_t from = edge(r, a[1], (double)x->t.len, 0);
    size_t to = edge(r, a[2], (double)x->t.len, x->t.len);
    return v_bytes(r->a, x->t.s + from, to > from ? to - from : 0);
  }
  list_of(r, x);
  size_t from = edge(r, a[1], (double)x->l.n, 0);
  size_t to = edge(r, a[2], (double)x->l.n, x->l.n);
  val *out = v_list(r->a);
  for (size_t i = from; i < to; i++) list_push(r->a, out, x->l.v[i]);
  return out;
}

// Where some bytes first are inside bytes, from a place on: what a protocol needs to find the end of a line or a header
// in what came in without going through it a byte at a time.
static val *find(run *r, val **a) {
  val *in = bytes_of(r, a[0]);
  val *what = bytes_of(r, a[1]);
  size_t from = edge(r, a[2], (double)in->t.len, 0);
  for (size_t i = from; i + what->t.len <= in->t.len; i++)
    if (!memcmp(in->t.s + i, what->t.s, what->t.len)) return v_num(r->a, (double)i);
  return NOTHING;
}

////////// 2. CONVERTING //////////

// "$1,204.50" is 1204.5: money signs, commas, spaces and percent signs are ignored, and only the number at the start counts.
static val *std_num(run *r, val **a) {
  if (a[0]->k == V_NUM) return a[0];
  val *t = text_of(r, a[0]);
  buf b = { 0 };
  for (size_t i = 0; i < t->t.len; i++) {
    const char *c = t->t.s + i;
    if (*c == '$' || *c == ',' || *c == '%' || isspace((unsigned char)*c)) continue;
    if (!memcmp(c, "\xE2\x82\xAC", 3) && i + 3 <= t->t.len) {
      i += 2;
      continue;
    }   // euro
    if (!memcmp(c, "\xC2\xA3", 2) && i + 2 <= t->t.len) {
      i += 1;
      continue;
    }   // pound
    buf_chr(&b, *c);
  }
  buf_chr(&b, 0);
  const char *s = b.s;
  // strtod also reads hex, inf and nan, which JavaScript's parseFloat doesn't, so only a plain decimal start is allowed.
  int ok = (s[0] >= '0' && s[0] <= '9') ||
      ((s[0] == '-' || s[0] == '+' || s[0] == '.') &&
          ((s[1] >= '0' && s[1] <= '9') || (s[1] == '.' && s[2] >= '0' && s[2] <= '9')));
  if (ok && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    buf_free(&b);
    return v_num(r->a, 0);
  }
  double n = ok ? strtod(s, NULL) : NAN;
  buf_free(&b);
  return isfinite(n) ? v_num(r->a, n) : NOTHING;
}

static val *std_text(run *r, val **a) { return text_of(r, a[0]); }

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

// Text as its UTF-8 bytes and back: how a lingo reaches a character's code, and what a connection sends and receives.
static val *bytes(run *r, val **a) {
  if (a[0]->k == V_LIST || a[0]->k == V_BYTES) return bytes_of(r, a[0]);
  return bytes_of(r, text_of(r, a[0]));
}

static val *from_bytes(run *r, val **a) {
  val *b = bytes_of(r, a[0]);
  if (!utf8_ok(b->t.s, b->t.len)) fail(r, "phrase", "those bytes aren't UTF-8 text");
  return v_text(r->a, b->t.s, b->t.len);
}

////////// 3. STD_PHRASES //////////

void std_phrases(void) {
  phrase_add("std", "none", "length(x: any) -> number: items, characters or parts", len);
  phrase_add("std", "none", "keys(r: record) -> [text]: a record's part names", std_keys);
  phrase_add(
      "std", "none", "slice(x: any, from: whole, to: whole=nothing) -> list|text|bytes: to not included, negative counts from the end", slice);
  phrase_add("std", "none", "find(in: bytes, what: bytes, from: whole=0) -> whole?: where what first is in in, from there on", find);
  phrase_add("std", "none", "number(x: any) -> number?: from text like \"$1,204.50\"", std_num);
  phrase_add("std", "none", "text(x: any) -> text", std_text);
  phrase_add("std", "none", "to_json(x: any) -> text", json);
  phrase_add("std", "none", "from_json(text: text) -> any: from JSON", parse);
  phrase_add("std", "none", "bytes(x: any) -> bytes: text's UTF-8 bytes, or a list of numbers 0 to 255 as bytes", bytes);
  phrase_add("std", "none", "from_bytes(b: bytes) -> text: UTF-8 bytes back to text", from_bytes);
}

//////////////////// 6. SIGNING ////////////////////

// Signing and checking: Ed25519 (RFC 8032) on Monocypher, which comes as it is (../../NOTICE), SHA-256, and
// base64 for both.
//
// 1. sha256(data, n, out)              the SHA-256 of bytes
//    sha1(data, n, out)                the SHA-1 of bytes, only for a WebSocket's handshake
// 2. base64(out, data, n)              bytes as base64, with its = at the end
//    unbase64(s, n, out)               and back. 0 when it isn't base64
// 3. ed25519_key(seed, key)            the public key of a private one, its 32 byte seed
//    ed25519_sign(seed, msg, n, sig)   a signature of msg
//    ed25519_check(key, msg, n, sig)   1 when key signed msg

////////// 1. SHA-256 //////////

static const uint32_t K[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
  0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
  0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138,
  0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70,
  0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
  0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

// One 64 byte block into the hash so far.
static void sha_block(uint32_t h[8], const unsigned char *b) {
  uint32_t w[64], s[8];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; i++)
    w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
        (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
  memcpy(s, h, sizeof s);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] +
        w[i];
    uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
    memmove(s + 1, s, 7 * sizeof *s);
    s[4] += t1;
    s[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) h[i] += s[i];
}

// 1.
void sha256(const void *data, size_t n, unsigned char out[32]) {
  uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  const unsigned char *p = data;
  unsigned char last[128] = { 0 };
  size_t whole = n / 64 * 64;
  size_t rest = n - whole;
  size_t size = rest < 56 ? 64 : 128;
  for (size_t i = 0; i < whole; i += 64) sha_block(h, p + i);
  if (rest) memcpy(last, p + whole, rest);
  last[rest] = 0x80;
  for (int i = 0; i < 8; i++) last[size - 1 - i] = (unsigned char)((unsigned long long)n * 8 >> (8 * i));
  sha_block(h, last);
  if (size == 128) sha_block(h, last + 64);
  for (int i = 0; i < 32; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

// SHA-1, only for a WebSocket's Sec-WebSocket-Accept, base64 of SHA-1 of its key and the protocol's own GUID.
void sha1(const unsigned char *s, size_t n, unsigned char digest[20]) {
  unsigned h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  size_t total = (n + 9 + 63) / 64 * 64;
  unsigned char *m = calloc(total, 1);
  memcpy(m, s, n);
  m[n] = 0x80;
  for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)((unsigned long long)n * 8 >> (8 * i));
  for (size_t at = 0; at < total; at += 64) {
    unsigned w[80];
    unsigned a = h[0];
    unsigned b = h[1];
    unsigned c = h[2];
    unsigned d = h[3];
    unsigned e = h[4];
    for (int i = 0; i < 16; i++)
      w[i] = (unsigned)m[at + 4 * i] << 24 | (unsigned)m[at + 4 * i + 1] << 16 | (unsigned)m[at + 4 * i + 2] << 8 |
          m[at + 4 * i + 3];
    for (int i = 16; i < 80; i++) {
      unsigned x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = x << 1 | x >> 31;
    }
    for (int i = 0; i < 80; i++) {
      unsigned f = b ^ c ^ d;
      unsigned k = 0xCA62C1D6;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      }
      unsigned t = (a << 5 | a >> 27) + f + e + k + w[i];
      e = d;
      d = c;
      c = b << 30 | b >> 2;
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  free(m);
  for (int i = 0; i < 20; i++) digest[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

////////// 2. BASE64 //////////

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 2.
void base64(buf *out, const void *data, size_t n) {
  const unsigned char *s = data;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
    char four[4] = { ALPHABET[v >> 18 & 63], ALPHABET[v >> 12 & 63], i + 1 < n ? ALPHABET[v >> 6 & 63] : '=',
      i + 2 < n ? ALPHABET[v & 63] : '=' };
    buf_add(out, four, 4);
  }
  if (!out->s) buf_add(out, "", 0);
}

// 2.
int unbase64(const char *s, size_t n, buf *out) {
  unsigned v = 0;
  int bits = 0;
  while (n && s[n - 1] == '=') n--;
  for (size_t i = 0; i < n; i++) {
    const char *at = s[i] ? strchr(ALPHABET, s[i]) : NULL;
    if (!at) return 0;
    v = v << 6 | (unsigned)(at - ALPHABET);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      buf_chr(out, (char)(v >> bits & 255));
    }
  }
  if (!out->s) buf_add(out, "", 0);
  return 1;
}

////////// 3. ED25519 //////////

// 3.
void ed25519_key(const unsigned char seed[32], unsigned char key[32]) {
  unsigned char copy[32], secret[64];
  memcpy(copy, seed, 32);   // Monocypher wipes the seed it is given
  crypto_ed25519_key_pair(secret, key, copy);
  crypto_wipe(secret, sizeof secret);
}

// 3.
void ed25519_sign(const unsigned char seed[32], const void *msg, size_t n, unsigned char sig[64]) {
  unsigned char copy[32], secret[64], key[32];
  memcpy(copy, seed, 32);
  crypto_ed25519_key_pair(secret, key, copy);
  crypto_ed25519_sign(sig, secret, msg, n);
  crypto_wipe(secret, sizeof secret);
}

// 3.
int ed25519_check(const unsigned char key[32], const void *msg, size_t n, const unsigned char sig[64]) {
  return crypto_ed25519_check(sig, key, msg, n) == 0;
}

//////////////////// 7. ZIPS ////////////////////

// A .lingo file is a zip: one file read out of one, stored or deflated (puff.c), and one made, its files stored.
//
// 1. zip_file(zip, n, name, out)        -> 1 with the file called name in out, 0 when it isn't there or isn't whole
// 2. zip_make(out, count, names, files) -> a zip of those files, each stored as it is

#define ZIP_MOST (64u << 20)   // the most one file in a zip may be, unpacked

static uint32_t crc(const unsigned char *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0 - (c & 1)));
  }
  return ~c;
}

static uint32_t num(const unsigned char *p, int bytes) {
  uint32_t v = 0;
  for (int i = bytes - 1; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

static void zip_put(buf *b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++) buf_chr(b, (char)(v >> (8 * i)));
}

////////// 1. READING //////////

// Its end record names where the list of its files is, and each in the list where the file is.
int zip_file(const char *zip, size_t n, const char *name, buf *out) {
  const unsigned char *z = (const unsigned char *)zip;
  const unsigned char *end = NULL;
  for (size_t i = n >= 22 ? n - 22 : 0; n >= 22 && !end; i--) {
    if (num(z + i, 4) == 0x06054b50) end = z + i;
    if (!i || n - i > 22 + 65535) break;
  }
  if (!end) return 0;
  size_t at = num(end + 16, 4);
  size_t count = num(end + 10, 2);
  size_t len = strlen(name);
  for (size_t k = 0; k < count && at + 46 <= n && num(z + at, 4) == 0x02014b50; k++) {
    const unsigned char *e = z + at;
    size_t nl = num(e + 28, 2);
    size_t method = num(e + 10, 2);
    size_t packed = num(e + 20, 4);
    size_t size = num(e + 24, 4);
    size_t local = num(e + 42, 4);
    at += 46 + nl + num(e + 30, 2) + num(e + 32, 2);
    if (at > n || nl != len || memcmp(e + 46, name, len)) continue;
    if (local + 30 > n || num(z + local, 4) != 0x04034b50 || size > ZIP_MOST) return 0;
    size_t from = local + 30 + num(z + local + 26, 2) + num(z + local + 28, 2);
    if (from > n || packed > n - from || (method != 0 && method != 8)) return 0;
    unsigned char *data = malloc(size + 1);
    unsigned long got = size;
    unsigned long used = packed;
    int ok;
    if (method == 0) {
      ok = packed == size;
      if (ok) memcpy(data, z + from, size);
    } else {
      ok = !puff(data, &got, z + from, &used) && got == size;
    }
    ok = ok && crc(data, size) == num(e + 16, 4);
    if (ok) buf_add(out, (const char *)data, size);
    if (ok && !out->s) buf_add(out, "", 0);
    free(data);
    return ok;
  }
  return 0;
}

////////// 2. MAKING //////////

// Every time is 1980-01-01, the first a zip can say, so the same files always make the same zip.
void zip_make(buf *out, int count, const char *const *names, const buf *files) {
  buf list = { 0 };
  for (int i = 0; i < count; i++) {
    const unsigned char *data = (const unsigned char *)(files[i].s ? files[i].s : "");
    uint32_t sum = crc(data, files[i].n);
    uint32_t size = (uint32_t)files[i].n;
    uint32_t at = (uint32_t)out->n;
    uint32_t nl = (uint32_t)strlen(names[i]);
    zip_put(out, 0x04034b50, 4);
    zip_put(out, 20, 2);
    zip_put(out, 0, 2);
    zip_put(out, 0, 2);
    zip_put(out, 0, 2);
    zip_put(out, 33, 2);
    zip_put(out, sum, 4);
    zip_put(out, size, 4);
    zip_put(out, size, 4);
    zip_put(out, nl, 2);
    zip_put(out, 0, 2);
    buf_add(out, names[i], nl);
    buf_add(out, (const char *)data, size);
    zip_put(&list, 0x02014b50, 4);
    zip_put(&list, 20, 2);
    zip_put(&list, 20, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 33, 2);
    zip_put(&list, sum, 4);
    zip_put(&list, size, 4);
    zip_put(&list, size, 4);
    zip_put(&list, nl, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 4);
    zip_put(&list, at, 4);
    buf_add(&list, names[i], nl);
  }
  uint32_t at = (uint32_t)out->n;
  buf_add(out, list.s ? list.s : "", list.n);
  zip_put(out, 0x06054b50, 4);
  zip_put(out, 0, 2);
  zip_put(out, 0, 2);
  zip_put(out, (uint32_t)count, 2);
  zip_put(out, (uint32_t)count, 2);
  zip_put(out, (uint32_t)list.n, 4);
  zip_put(out, at, 4);
  zip_put(out, 0, 2);
  buf_free(&list);
}

//////////////////// 8. ENGINE ////////////////////

// The lingo client's engine: its settings, the lingos it loads, who may call what, and every request it answers.
//
// 1. Settings        the settings file, read once, and JSON files in its home
// 2. Floor           the compute, store, http, socket, connection and settings phrases every core has, and lingo(...) and use(...),
//                    asked of the native lingo that answers them, or of the settings' ask
// 3. Lingos          .lingo files and folders: read, loaded, their OpenAPI manifests, running or stopped, the catalog,
//                    and the hooks of native lingos that run
// 4. Policy          policy.json and the account's: callers, signatures and rules, the ceiling, the root, asks and the log
// 5. Stops           stopping a running request, and those running inside its wait
// 6. Calls           one function of a lingo, run
// 7. Lingo settings  setting(...), settings.get and settings.set, kept on this computer, never on Onsay
// 8. Control         start, stop, the person's answers, settings, manifests, the policy and call, at /.lingo/ and on the
//                    command line
// 9. Hooks           what native lingos are offered back: the account's policy kept, settings, this device's keys, its
//                    home, and looking at the lingos folder again
// 10. Requests       web requests, from this computer's port or handed in by a native lingo, each answered by the lingo
//                    at its route when the caller may reach it
// 11. Beginning, and packing a lingo
// It starts no thread and holds no connection but each http.send's, each socket's and each connection's, and core_stop may run
// on another thread, so those and the running request's stop are kept behind one gate.
#include "tlds.h"   // TLDS, the web's top level domains as one text, " aaa aarp ... ", made by build.ps1 from tlds.txt

#define say core_say

/* The request running now, for the checks at each phrase: its caller as the log names them, the lingo and function it
   reached, like agent and agent.ask, its path, what that function's manifest says it needs, the policy in force, and
   what the person answered this time, [{what, yes}], and its id as JSON, which each lingo(...) and use(...) it asks
   carries as for. lingo is NULL outside a request, at the command line. */
static struct {
  const char *caller, *lingo, *fn, *path, *id;
  val *needs, *policy, *answers;
} job;

/* What each native lingo, by its number, filled with lingo_hook as it loaded, and whether it runs in a lingo client that
   serves, when they are used (lingo.h, section 7). */
static lingo_hooks hooks[64];
static int on[64], live;   // live: it serves requests, so its running native lingos are told
static size_t loading;   // the native lingo loading now

// Whether a list holds this text.
static int has(val *list, const char *s) {
  for (size_t i = 0; list && list->k == V_LIST && i < list->l.n; i++)
    if (list->l.v[i]->k == V_TEXT && !strcmp(list->l.v[i]->t.s, s)) return 1;
  return 0;
}

////////// 1. SETTINGS //////////

static core_settings set;

// A lingo's name: lower case letters, digits, _ and -, neither of those two first or last, and never lingo, the
// language's own name.
static int named(const char *s) {
  size_t n = strlen(s);
  return n && n <= 64 && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == n && !strchr("_-", s[0]) &&
      !strchr("_-", s[n - 1]) && strcmp(s, "lingo");
}

// 1. Kept in their own arena for as long as the core runs.
const core_settings *core_read(const char *text, size_t n) {
  arena *core_kept = arena_new();
  const char *problem = NULL;
  val *s = json_parse(core_kept, text, n, &problem);
  val *port;
  val *steps;
  val *ms;
  if (problem || s->k != V_REC) {
    say("The settings aren't a JSON record: %s", problem ? problem : "they're another kind");
    return NULL;
  }
  buf root = { 0 };
  set.all = s;
  set.home = text_or(s, "home", "onsay-data");
  set.lingos = text_or(s, "lingos", NULL) ? text_or(s, "lingos", NULL) : arena_fmt(core_kept, "%s/lingos", set.home);
  pl_folder(set.home);
  pl_whole(set.home, &root);
  set.root = v_text(core_kept, root.s, root.n)->t.s;
  buf_free(&root);
  port = rec_get(s, "port");
  steps = rec_get(s, "steps");
  ms = rec_get(s, "ms");
  set.port = port && port->k == V_NUM ? (int)port->n : 6969;
  set.steps = steps && steps->k == V_NUM ? (long long)steps->n : 5000000;
  set.ms = ms && ms->k == V_NUM ? ms->n : 600000;
  return &set;
}

// A JSON file in home, like policy.json or settings/agent.json, or NULL when it isn't there or isn't JSON.
static val *home_json(arena *a, const char *name) {
  buf path = { 0 };
  buf text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  val *v = pl_read(path.s, &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  buf_free(&path);
  buf_free(&text);
  return v;
}

// Keeps one, its folder made when missing. 1 when it was kept.
static int keep_json(const char *name, val *v) {
  buf path = { 0 };
  buf text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  buf_json(&text, v);
  char *slash = strrchr(path.s, '/');
  *slash = 0;
  pl_folder(path.s);
  *slash = '/';
  int ok = pl_write(path.s, text.s, text.n);
  buf_free(&path);
  buf_free(&text);
  return ok;
}

////////// 2. FLOOR //////////

// compute: the clock, waiting, and a line in the lingo client's log.

static val *time_(run *r, val **a) {
  (void)a;
  return v_num(r->a, pl_now());
}

static val *wait_(run *r, val **a) {
  double s = num_of(r, a[0], NULL);
  double until = pl_now() + (s > 60 ? 60 : s);
  // Short sleeps, so a stop still lands quickly.
  while (pl_now() < until) {
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    double left = until - pl_now();
    int never = 0;
    lingo_wait(&never, r->stop, left < 0.05 ? left : 0.05);
  }
  return NOTHING;
}

static val *log_(run *r, val **a) {
  say("%s", text_of(r, a[0])->t.s);
  return NOTHING;
}

// store: Onsay's own storage, one file per key in home/store. A key's name is written so any key is a safe file name on
// any system: only a to z, 0 to 9, _ and - stay, everything else becomes %XX, so case and dots can never clash.

// Each lingo's keys start with its name and a slash, like memo/today, so no lingo reads or changes another's. The person,
// at the command line or through /onsay, reaches any.
static void core_mine(run *r, val *key) {
  size_t n = r->lingo ? strlen(r->lingo) : 0;
  if (n && (key->t.len <= n || memcmp(key->t.s, r->lingo, n) || key->t.s[n] != '/'))
    fail(r, "phrase", "%s's keys start with %s/, like %s/notes", r->lingo, r->lingo, r->lingo);
}

static void file_of(run *r, val *key, buf *path) {
  if (key->k != V_TEXT || !key->t.len) fail(r, "phrase", "a key is text, like notes/today");
  if (key->t.len > 100) fail(r, "phrase", "a key is at most 100 characters");
  core_mine(r, key);
  buf_fmt(path, "%s/store/k", set.home);
  for (size_t i = 0; i < key->t.len; i++) {
    unsigned char c = (unsigned char)key->t.s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
      buf_chr(path, (char)c);
    else
      buf_fmt(path, "%%%02X", c);
  }
}

static int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static val *get(run *r, val **a) {
  buf path = { 0 };
  buf text = { 0 };
  file_of(r, a[0], &path);
  int found = pl_read(path.s, &text);
  val *out = found ? v_text(r->a, text.s, text.n) : NOTHING;
  buf_free(&path);
  buf_free(&text);
  return out;
}

static val *core_put(run *r, val **a) {
  buf path = { 0 };
  buf text = { 0 };
  buf folder = { 0 };
  file_of(r, a[0], &path);
  buf_text(&text, a[1]);
  buf_fmt(&folder, "%s/store", set.home);
  int ok = pl_folder(folder.s) && pl_write(path.s, text.s ? text.s : "", text.n);
  buf_free(&path);
  buf_free(&text);
  buf_free(&folder);
  if (!ok) fail(r, "phrase", "this computer couldn't save it");
  return NOTHING;
}

static val *delete_(run *r, val **a) {
  buf path = { 0 };
  file_of(r, a[0], &path);
  int ok = pl_delete(path.s);
  buf_free(&path);
  if (!ok) fail(r, "phrase", "this computer couldn't delete it");
  return NOTHING;
}

typedef struct {
  run *r;
  val *prefix, *out;
} key_listing;

static void each_key(const char *name, int folder, double size, double changed, void *arg) {
  key_listing *l = arg;
  (void)size;
  (void)changed;
  if (folder || name[0] != 'k') return;
  buf key = { 0 };
  for (const char *p = name + 1; *p; p++) {
    int hi, lo;
    if (*p == '%' && (hi = hex(p[1])) >= 0 && (lo = hex(p[2])) >= 0) {
      buf_chr(&key, (char)(hi * 16 + lo));
      p += 2;
    } else if (*p == '%') {
      buf_free(&key);
      return;
    }   // not one of ours
    else
      buf_chr(&key, *p);
  }
  if (key.n >= l->prefix->t.len && !memcmp(key.s, l->prefix->t.s, l->prefix->t.len))
    list_push(l->r->a, l->out, v_text(l->r->a, key.s, key.n));
  buf_free(&key);
}

static val *keys(run *r, val **a) {
  val *prefix = a[0]->k == V_TEXT ? a[0] : v_cstr(r->a, "");
  if (r->lingo && !prefix->t.len) prefix = v_cstr(r->a, arena_fmt(r->a, "%s/", r->lingo));   // every key of its own
  core_mine(r, prefix);
  key_listing l = { r, prefix, v_list(r->a) };
  buf folder = { 0 };
  buf_fmt(&folder, "%s/store", set.home);
  pl_list(folder.s, each_key, &l);
  buf_free(&folder);
  // Sorted, so every computer lists the same keys in the same order.
  val **v = l.out->l.v;
  for (size_t i = 1; i < l.out->l.n; i++)
    for (size_t j = i; j > 0 && strcmp(v[j - 1]->t.s, v[j]->t.s) > 0; j--) {
      val *t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return l.out;
}

/* A web address as a lingo wrote it, made the one to send, by the one rule every Onsay repo keeps (onsay's
   server/lib/names.js web): a host is on the web when it is an IP, localhost, or a name with a dot ending in one of the
   web's top level domains, like example.com, and is itself, http:// or https:// left off when it likes, http for an IP.
   Any other, like laptop.brandon, is an Onsay name: Onsay's resolve endpoint (ONSAY and RESOLVE, at the top) gives its web
   address, and what followed the name goes on after it. ws, for a WebSocket: ws:// and wss:// read as http:// and
   https://, and the address given is ws:// or wss://, but never a computer's. NULL, with why, when it has none. */
static const char *reached(arena *a, const char *url, int ws, buf *why) {
  size_t s = strspn(url, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
  size_t n;
  size_t dots = 0;
  size_t digits = 1;
  const char *rest = s && !strncmp(url + s, "://", 3) ? url + s + 3 : url;
  char host[256], word[80];
  if ((n = strcspn(rest, "/?#")) >= sizeof host || !n) return url;
  for (size_t i = 0; i <= n; i++) host[i] = i < n ? (char)tolower((unsigned char)rest[i]) : 0;
  char *port = strrchr(host, ':');
  char *last = strrchr(host, '.');
  if (port && host[0] != '[' && strspn(port + 1, "0123456789") == strlen(port + 1)) *port = 0;
  for (const char *c = host; *c; c++)
    if (*c == '.')
      dots++;
    else if (!isdigit((unsigned char)*c))
      digits = 0;
  int ip = host[0] == '[' || !strcmp(host, "localhost") || (digits && dots == 3);
  snprintf(word, sizeof word, " %s ", last ? last + 1 : "");
  if (ip || (last && strlen(last) < 70 && strstr(TLDS, word)))
    return rest != url ? url : arena_fmt(a, "%s%s://%s", ws ? "ws" : "http", ip ? "" : "s", url);
  buf at = { 0 };
  buf_fmt(&at, "%s%s%s", ONSAY, RESOLVE, host);
  val *got = pl_http(a, "GET", at.s, v_rec(a), "", 0, 30, why);
  val *text = rec_get(got, "text");
  val *said = text && text->k == V_TEXT ? json_parse(a, text->t.s, text->t.len, NULL) : NULL;
  const char *address = said && said->k == V_REC ? text_or(said, "address", NULL) : NULL;
  buf_free(&at);
  if (!address && got) buf_fmt(why, "%s", said && said->k == V_REC ? text_or(said, "error", "") : "");
  if (!address && got && !why->n) buf_fmt(why, "%s has nowhere to send a request yet", host);
  if (!address) return NULL;
  if (!ws || strncmp(address, "http", 4)) return arena_fmt(a, "%s%s", address, rest + n);
  // Onsay's proxy address for a computer, or a route to one, ends with its name (onsay's server/lib/proxy.js address).
  size_t length = strlen(address);
  size_t name = strlen(host);
  if (length > name && address[length - name - 1] == '/' && !strcmp(address + length - name, host)) {
    buf_fmt(why, "%s is on a computer, which Onsay reaches one request at a time, so a WebSocket can't reach it yet", host);
    return NULL;
  }
  return arena_fmt(a, "ws%s%s", address + 4, rest + n);   // http://x is ws://x, https://x wss://x
}

static pl_gate *core_gate;

#define CALL_BODY 4096   // the most bytes of a body a call keeps (lingo_called)

// A header that carries a key, or a query value's name that says it is one: never kept in a call.
static int secret(const char *name, int query) {
  static const char *headers[] = { "authorization", "cookie", "set-cookie", "proxy-authorization", "x-api-key", "api-key" };
  static const char *words[] = { "key", "token", "secret", "password", "sig", "auth" };
  char low[128];
  size_t n = 0;
  for (; name[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
  low[n] = 0;
  if (!query) {
    for (size_t i = 0; i < sizeof headers / sizeof *headers; i++)
      if (!strcmp(low, headers[i])) return 1;
    return 0;
  }
  for (size_t i = 0; i < sizeof words / sizeof *words; i++)
    if (strstr(low, words[i])) return 1;
  return 0;
}

// Headers as a call keeps them, every secret one (redacted).
static val *kept_headers(arena *a, val *headers) {
  val *out = v_rec(a);
  for (size_t i = 0; headers && headers->k == V_REC && i < headers->r.n; i++)
    rec_set(a, out, headers->r.key[i], secret(headers->r.key[i], 0) ? v_cstr(a, "(redacted)") : headers->r.v[i]);
  return out;
}

// lingo.h: one request made, as a line of JSON, to each running native lingo that keeps calls.
void lingo_called(const char *kind, const char *address, const char *ask, const char *method, const char *url, val *headers,
    const char *body, size_t n, val *got, const char *error, int stopped, double ms) {
  arena *a = arena_new();
  val *line = v_rec(a);
  val *request = v_rec(a);
  val *response = v_rec(a);
  buf safe = { 0 };
  buf text = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  const char *query = strchr(url, '?');
  // The address with each secret query value taken out.
  buf_add(&safe, url, query ? (size_t)(query - url) : strlen(url));
  for (const char *p = query; p && *p;) {
    size_t len = strcspn(p + 1, "&");
    size_t name = strcspn(p + 1, "=&");
    char key[128];
    snprintf(key, sizeof key, "%.*s", (int)(name < sizeof key ? name : sizeof key - 1), p + 1);
    buf_chr(&safe, *p);
    if (name < len && secret(key, 1))
      buf_fmt(&safe, "%s=(redacted)", key);
    else
      buf_add(&safe, p + 1, len);
    p = p[1 + len] ? p + 1 + len : NULL;
  }
  buf_chr(&safe, 0);
  const char *host = strstr(safe.s, "://");
  const char *path = host ? strchr(host + 3, '/') : safe.s;
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  rec_set(a, request, "method", v_cstr(a, method));
  rec_set(a, request, "url", v_cstr(a, safe.s));
  rec_set(a, request, "path", v_cstr(a, path && *path ? path : "/"));
  rec_set(a, request, "headers", kept_headers(a, headers));
  rec_set(a, request, "body", v_text(a, body ? body : "", body ? (n < CALL_BODY ? n : CALL_BODY) : 0));
  val *said = got ? rec_get(got, "text") : NULL;   // text, or for a socket a record of what went each way, kept whole
  val *status = got ? rec_get(got, "status") : NULL;
  if (said && said->k == V_TEXT && said->t.len > CALL_BODY) said = v_text(a, said->t.s, CALL_BODY);
  rec_set(a, response, "status", status && status->k == V_NUM ? status : v_num(a, 0));
  rec_set(a, response, "headers", kept_headers(a, got ? rec_get(got, "headers") : NULL));
  rec_set(a, response, "body", said ? said : NOTHING);
  rec_set(a, line, "at", v_cstr(a, at));
  rec_set(a, line, "ms", v_num(a, ms));
  rec_set(a, line, "kind", v_cstr(a, kind));
  rec_set(a, line, "address", v_cstr(a, address ? address : safe.s));
  rec_set(a, line, "ask", v_cstr(a, ask ? ask : ""));
  rec_set(a, line, "request", request);
  rec_set(a, line, "response", response);
  rec_set(a, line, "error", v_cstr(a, error ? error : ""));
  rec_set(a, line, "stopped", VBOOL(stopped));
  buf_json(&text, line);
  for (size_t i = 1; i < 64; i++)
    if (on[i] && hooks[i].called) hooks[i].called(text.s);
  buf_free(&safe);
  buf_free(&text);
  arena_free(a);
}

// One http.send on a thread of its own, with its own memory, so other requests are answered while it waits.
typedef struct {
  arena *a;
  const char *method, *url, *body;
  size_t n;
  val *headers, *got;
  double seconds;
  buf why;
  int done, finished, left;
} trip;

static void trip_free(trip *t) {
  buf_free(&t->why);
  arena_free(t->a);
  free(t);
}

static void travel(void *arg) {
  trip *t = arg;
  const char *to = reached(t->a, t->url, 0, &t->why);
  if (to) t->got = pl_http(t->a, t->method, to, t->headers, t->body, t->n, t->seconds, &t->why);
  pl_enter(core_gate);
  t->finished = 1;
  int left = t->left;
  pl_leave(core_gate);
  if (left)
    trip_free(t);
  else
    lingo_done(&t->done);
}

// http.send: one web request, made by the computer itself (pl_http), its method a word and each header one whole line.
static val *core_send(run *r, val **a) {
  buf method = { 0 };
  buf url = { 0 };
  buf body = { 0 };
  buf why = { 0 };
  trip *t = calloc(1, sizeof *t);
  val *got = NULL;
  t->a = arena_new();
  t->headers = v_rec(t->a);
  buf_text(&method, a[0]);
  buf_text(&url, a[1]);
  buf_text(&body, a[3]);
  buf_chr(&method, 0);
  buf_chr(&url, 0);   // so neither is ever empty of bytes
  for (size_t i = 0; method.s[i]; i++)
    if (method.s[i] >= 'a' && method.s[i] <= 'z') method.s[i] -= 32;
  if (strchr(url.s, '#')) *strchr(url.s, '#') = 0;   // the part after # is the page's own, never sent
  if (!*method.s || strspn(method.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(method.s))
    buf_str(&why, "the method is a word, like GET or POST");
  for (size_t i = 0; a[2]->k == V_REC && i < a[2]->r.n && !why.n; i++) {
    val *value = text_of(r, a[2]->r.v[i]);
    if (strpbrk(a[2]->r.key[i], "\r\n:") || strpbrk(value->t.s, "\r\n"))
      buf_fmt(&why, "the header %s has a line break or a colon in it", a[2]->r.key[i]);
    else
      rec_set(t->a, t->headers, a[2]->r.key[i], v_cstr(t->a, value->t.s));
  }
  double seconds = a[4]->k == V_NUM && a[4]->n > 0 && a[4]->n < 3600 ? a[4]->n : 30;
  val *problem = v_text(r->a, why.s ? why.s : "", why.n);
  if (!why.n) {
    t->method = v_cstr(t->a, method.s)->t.s;
    t->url = v_cstr(t->a, url.s)->t.s;
    t->body = v_text(t->a, body.s ? body.s : "", body.n)->t.s;
    t->n = body.n;
    t->seconds = seconds;
  }
  int asked_ = !why.n;
  buf_free(&method);
  buf_free(&url);
  buf_free(&body);
  buf_free(&why);
  if (!asked_) {
    trip_free(t);
    fail(r, "phrase", "%s", problem->t.s);
  }
  double began = pl_now();
  pl_thread(travel, t);
  while (!t->done && !*r->stop) lingo_wait(&t->done, r->stop, 1);
  // Kept as a call once: stopped here while the trip still runs, with no answer, else below with its answer.
  int stopped = !t->done;
  if (stopped) lingo_called("web", NULL, NULL, t->method, t->url, t->headers, t->body, t->n, NULL, "Stopped by the person", 1, (pl_now() - began) * 1000);
  pl_enter(core_gate);
  int gone = !t->finished;   // stopped first: it frees itself when it ends
  t->left = gone;
  pl_leave(core_gate);
  if (gone) fail(r, "stopped", "Stopped by the person");
  val *status = t->got ? rec_get(t->got, "status") : NULL;
  const char *error = t->got ? NULL : t->why.s ? t->why.s : "No answer";
  if (status && status->k == V_NUM && (status->n < 200 || status->n > 399)) error = "It answered with an error";
  if (!stopped) lingo_called("web", NULL, NULL, t->method, t->url, t->headers, t->body, t->n, t->got, error, 0, (pl_now() - began) * 1000);
  // Written out and read back, since a copy would still point at text in the trip's memory, freed below.
  buf whole = { 0 };
  if (t->got) buf_json(&whole, t->got);
  got = t->got ? json_parse(r->a, whole.s, whole.n, NULL) : NULL;
  buf_free(&whole);
  problem = v_text(r->a, t->why.s ? t->why.s : "", t->why.n);
  trip_free(t);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got) fail(r, "phrase", "%s", problem->t.s);
  return got;
}

/* socket: WebSockets a run opens, each opened and read on a thread of its own (talk), every message that comes kept in
   waiting until socket.receive takes it. Every socket a run opened is closed as that run ends (core_call). talk frees a
   socket its run let go of once talk ends, else the run frees it, the way a trip is freed. Each is kept as a call of
   kind socket as its run lets go of it, and one that couldn't open at once (let_go). */
typedef struct sock {
  struct sock *next;
  long id;
  char *url, *headers;   // url as the lingo wrote it, headers whole lines, or NULL
  pl_link *l;            // set once, as it opens, and freed with it
  buf waiting;           // each message come and not yet taken: its length, a size_t, then its text
  buf why;               // why it didn't open
  volatile int news;     // set as it opens or ends and as each message comes, for lingo_wait
  int ended, finished, left;   // ended: closed, or never opened. left: let go by its run
  double began, over;          // when it was asked for, and when it ended
  double sent, sent_bytes, came, came_bytes;   // messages each way, and their size
} sock;

static sock *socks;   // the open ones, newest first, so those a run opened are on top as it ends
static long socks_made;

static void sock_free(sock *s) {
  if (s->l) pl_free(s->l);
  buf_free(&s->waiting);
  buf_free(&s->why);
  free(s->url);
  free(s->headers);
  free(s);
}

static void talk(void *arg) {
  sock *s = arg;
  buf in = { 0 };
  arena *a = arena_new();
  const char *to = reached(a, s->url, 1, &s->why);
  pl_link *l = to ? pl_connect(to, s->headers, &s->why) : NULL;
  arena_free(a);
  pl_enter(core_gate);
  s->l = l;
  if (l && s->left) pl_close(l);
  pl_leave(core_gate);
  lingo_done(&s->news);
  while (l && pl_receive(l, &in)) {
    pl_enter(core_gate);
    buf_add(&s->waiting, (const char *)&in.n, sizeof in.n);
    buf_add(&s->waiting, in.s ? in.s : "", in.n);
    s->came++;
    s->came_bytes += (double)in.n;
    pl_leave(core_gate);
    lingo_done(&s->news);
    in.n = 0;
  }
  buf_free(&in);
  pl_enter(core_gate);
  s->ended = 1;
  s->over = pl_now();
  pl_leave(core_gate);
  lingo_done(&s->news);
  pl_enter(core_gate);
  s->finished = 1;
  int left = s->left;
  pl_leave(core_gate);
  if (left) sock_free(s);
}

/* Kept as a call, closed and taken off the open ones, freed now when talk has ended, else by talk as it ends. closed: by
   the lingo, or as the run ended, unless the other side closed it first. stopped: the person stopped it opening. */
static void let_go(sock *s, const char *closed, int stopped) {
  sock **at = &socks;
  arena *a = arena_new();
  val *got = NULL;
  while (*at != s) at = &(*at)->next;
  *at = s->next;
  pl_enter(core_gate);
  int opened = s->l != NULL;
  if (opened && s->ended) closed = "by the other side";
  double ms = ((opened && s->ended ? s->over : pl_now()) - s->began) * 1000;
  if (opened) {
    val *sent = v_rec(a);
    val *received = v_rec(a);
    val *body = v_rec(a);
    rec_set(a, sent, "messages", v_num(a, s->sent));
    rec_set(a, sent, "bytes", v_num(a, s->sent_bytes));
    rec_set(a, received, "messages", v_num(a, s->came));
    rec_set(a, received, "bytes", v_num(a, s->came_bytes));
    rec_set(a, body, "sent", sent);
    rec_set(a, body, "received", received);
    rec_set(a, body, "closed", v_cstr(a, closed));
    got = v_rec(a);
    rec_set(a, got, "status", v_num(a, 101));
    rec_set(a, got, "text", body);
  }
  const char *why = "Stopped by the person";
  if (!stopped) why = s->why.n ? v_cstr(a, s->why.s)->t.s : "It couldn't open";
  pl_leave(core_gate);
  lingo_called("socket", NULL, NULL, "GET", s->url, header_lines(a, s->headers ? s->headers : ""), NULL, 0, got,
      opened ? NULL : why, stopped, ms);
  arena_free(a);
  pl_enter(core_gate);
  s->left = 1;
  if (s->l) pl_close(s->l);
  int finished = s->finished;
  pl_leave(core_gate);
  if (finished) sock_free(s);
}

static sock *sock_of(run *r, val *id) {
  sock *s = socks;
  while (s && !(id->k == V_NUM && s->id == id->n)) s = s->next;
  if (!s) fail(r, "phrase", "no socket %s is open in this run", text_of(r, id)->t.s);
  return s;
}

// socket.open: its address ws://, wss:// or with none, an Onsay name too (reached), and each header one whole line, as
// for http.send.
static val *socket_open(run *r, val **a) {
  val *url = text_of(r, a[0]);
  buf lines = { 0 };
  size_t scheme = strspn(url->t.s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
  if (!strncmp(url->t.s + scheme, "://", 3) && strncmp(url->t.s, "ws://", 5) && strncmp(url->t.s, "wss://", 6))
    fail(r, "phrase", "a WebSocket's address starts with ws:// or wss://, or leaves it off");
  for (size_t i = 0; a[1]->k == V_REC && i < a[1]->r.n; i++) {
    val *value = text_of(r, a[1]->r.v[i]);
    if (strpbrk(a[1]->r.key[i], "\r\n:") || strpbrk(value->t.s, "\r\n")) {
      buf_free(&lines);
      fail(r, "phrase", "the header %s has a line break or a colon in it", a[1]->r.key[i]);
    }
    buf_fmt(&lines, "%s: %s\r\n", a[1]->r.key[i], value->t.s);
  }
  sock *s = calloc(1, sizeof *s);
  s->id = ++socks_made;
  s->url = strdup(url->t.s);
  s->headers = lines.s;
  s->began = pl_now();
  s->next = socks;
  socks = s;
  pl_thread(talk, s);
  while (!s->news && !*r->stop) lingo_wait(&s->news, r->stop, 1);
  pl_enter(core_gate);
  int open = s->l != NULL;
  pl_leave(core_gate);
  if (open) return v_num(r->a, (double)s->id);
  val *why = v_text(r->a, s->why.s ? s->why.s : "", s->why.n);
  let_go(s, "as the run ended", *r->stop);   // closed so, should it open just as it is stopped
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  fail(r, "phrase", "%s", why->t.s);
}

static val *socket_send(run *r, val **a) {
  sock *s = sock_of(r, a[0]);
  val *text = text_of(r, a[1]);
  if (!pl_send(s->l, text->t.s, text->t.len)) fail(r, "phrase", "the socket was closed");
  s->sent++;
  s->sent_bytes += (double)text->t.len;
  return NOTHING;
}

// socket.receive: the oldest message waiting, else the next to come within seconds, else nothing.
static val *socket_receive(run *r, val **a) {
  sock *s = sock_of(r, a[0]);
  double seconds = a[1]->k == V_NUM && a[1]->n >= 0 && a[1]->n < 3600 ? a[1]->n : 30;
  double until = pl_now() + seconds;
  for (;;) {
    val *got = NULL;
    pl_enter(core_gate);
    if (s->waiting.n) {
      size_t n;
      memcpy(&n, s->waiting.s, sizeof n);
      got = v_text(r->a, s->waiting.s + sizeof n, n);
      s->waiting.n -= sizeof n + n;
      memmove(s->waiting.s, s->waiting.s + sizeof n + n, s->waiting.n);
    }
    int ended = s->ended;
    if (!got && !ended) s->news = 0;
    pl_leave(core_gate);
    if (got) return got;
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    if (ended) fail(r, "phrase", "the socket was closed");
    if (pl_now() >= until) return NOTHING;
    lingo_wait(&s->news, r->stop, until - pl_now());
  }
}

static val *socket_close(run *r, val **a) {
  let_go(sock_of(r, a[0]), "by the lingo", 0);
  return NOTHING;
}

/* connection: the floor under every protocol, bytes over a connection a run opens, tcp or udp to a host and port, or a
   listener taking tcp connections coming in to this computer. Its address says its kind, like tcp://example.com:80, and
   the kinds a port opens itself are PL_KINDS. Each is opened and read on a thread of its own (pull), what comes kept in
   waiting until connection.receive or connection.accept takes it, and the reading pauses while a lot is waiting. Every
   connection a run opened is closed as that run ends (core_call). pull frees one its run let go of once pull ends, else
   the run frees it, the way a socket is freed. */
typedef struct cn {
  struct cn *next;
  long id;
  char kind[8], *host;
  int port, listening;
  pl_conn *c;              // set once, as it opens, and freed with it
  buf waiting;             // bytes come and not yet taken, or for a listener each connection come in, a pl_conn *
  buf why;                 // why it didn't open
  volatile int news;       // set as it opens or ends and as anything comes, for lingo_wait
  int ended, finished, left;
} cn;

#define CN_HELD (4 << 20)   // the most kept waiting before reading pauses
static cn *cns;
static long cns_made;

static void cn_free(cn *k) {
  if (k->listening)
    for (size_t i = 0; i + sizeof(pl_conn *) <= k->waiting.n; i += sizeof(pl_conn *)) {
      pl_conn *c;
      memcpy(&c, k->waiting.s + i, sizeof c);
      pl_drop(c);
    }
  if (k->c) pl_drop(k->c);
  buf_free(&k->waiting);
  buf_free(&k->why);
  free(k->host);
  free(k);
}

static int cn_left(cn *k) {
  pl_enter(core_gate);
  int left = k->left;
  pl_leave(core_gate);
  return left;
}

static void pull(void *arg) {
  cn *k = arg;
  buf in = { 0 };
  buf from = { 0 };
  pl_conn *c = k->c;
  if (!c) c = k->listening ? pl_bind(k->kind, k->port, &k->why) : pl_dial(k->kind, k->host, k->port, &k->why);
  pl_enter(core_gate);
  k->c = c;
  if (c && k->left) pl_shut(c);
  pl_leave(core_gate);
  lingo_done(&k->news);
  while (c && !cn_left(k)) {
    pl_conn *came = NULL;
    if (k->listening && !(came = pl_next(c, &from, 1))) continue;
    if (!k->listening && !pl_get(c, &in)) break;
    pl_enter(core_gate);
    if (came) buf_add(&k->waiting, (const char *)&came, sizeof came);
    if (!came) buf_add(&k->waiting, in.s, in.n);
    int full = k->waiting.n > CN_HELD;
    pl_leave(core_gate);
    lingo_done(&k->news);
    in.n = 0;
    from.n = 0;
    while (full && !cn_left(k)) {
      pl_sleep(0.01);
      pl_enter(core_gate);
      full = k->waiting.n > CN_HELD;
      pl_leave(core_gate);
    }
  }
  buf_free(&in);
  buf_free(&from);
  pl_enter(core_gate);
  k->ended = 1;
  pl_leave(core_gate);
  lingo_done(&k->news);
  pl_enter(core_gate);
  k->finished = 1;
  int left = k->left;
  pl_leave(core_gate);
  if (left) cn_free(k);
}

static void cn_let_go(cn *k) {
  cn **at = &cns;
  while (*at != k) at = &(*at)->next;
  *at = k->next;
  pl_enter(core_gate);
  k->left = 1;
  if (k->c) pl_shut(k->c);
  int finished = k->finished;
  pl_leave(core_gate);
  if (finished) cn_free(k);
}

static cn *cn_of(run *r, val *id, int listening) {
  cn *k = cns;
  while (k && !(id->k == V_NUM && k->id == id->n)) k = k->next;
  if (!k) fail(r, "phrase", "no connection %s is open in this run", text_of(r, id)->t.s);
  if (k->listening != listening)
    fail(r, "phrase", listening ? "that's a connection: send to it and receive from it" : "that's a listener: accept from it");
  return k;
}

// One started and its thread waited for until it opens or can't. c is one already open, come in to a listener.
static long cn_start(run *r, const char *kind, const char *host, int port, int listening, pl_conn *c) {
  cn *k = calloc(1, sizeof *k);
  k->id = ++cns_made;
  snprintf(k->kind, sizeof k->kind, "%s", kind);
  k->host = strdup(host);
  k->port = port;
  k->listening = listening;
  k->c = c;
  k->next = cns;
  cns = k;
  pl_thread(pull, k);
  while (!k->news && !*r->stop) lingo_wait(&k->news, r->stop, 1);
  pl_enter(core_gate);
  int open = k->c != NULL;
  pl_leave(core_gate);
  if (open) return k->id;
  val *why = v_text(r->a, k->why.s ? k->why.s : "", k->why.n);
  cn_let_go(k);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  fail(r, "phrase", "%s", why->t.s);
  return 0;
}

// kind://host:port into its parts, a host in brackets for IPv6, like tcp://[::1]:80. An address with no kind is an Onsay
// address, a lingo on a computer, which isn't reached this way yet.
static void cn_address(run *r, val *address, char kind[8], buf *host, int *port) {
  const char *s = text_of(r, address)->t.s;
  const char *rest = strstr(s, "://");
  if (!rest) fail(r, "phrase", "a connection to a lingo on a computer, like %s, isn't built yet: give a kind, like tcp://%s", s, s);
  if (rest - s >= 8 || rest == s) fail(r, "phrase", "%s has no kind of connection this computer knows", s);
  memcpy(kind, s, (size_t)(rest - s));
  kind[rest - s] = 0;
  char words[16];
  snprintf(words, sizeof words, " %s ", kind);
  if (!strstr(" " PL_KINDS " ", words)) fail(r, "phrase", "this computer can't open %s connections", kind);
  rest += 3;
  const char *colon = rest[0] == '[' ? strstr(rest, "]:") : strrchr(rest, ':');
  if (rest[0] == '[' && colon) colon++;
  char *end = NULL;
  long n = colon ? strtol(colon + 1, &end, 10) : 0;
  if (!colon || !end || *end || n < 1 || n > 65535)
    fail(r, "phrase", "%s needs a port, like %s://example.com:80", s, kind);
  if (rest[0] == '[')
    buf_add(host, rest + 1, (size_t)(colon - rest - 2));
  else
    buf_add(host, rest, (size_t)(colon - rest));
  buf_chr(host, 0);
  *port = (int)n;
}

static val *connection_open(run *r, val **a) {
  char kind[8];
  buf host = { 0 };
  int port;
  cn_address(r, a[0], kind, &host, &port);
  val *where = v_cstr(r->a, host.s);
  buf_free(&host);
  return v_num(r->a, (double)cn_start(r, kind, where->t.s, port, 0, NULL));
}

// Only on this computer: what comes from elsewhere comes through the port, where the policies stand.
static val *connection_listen(run *r, val **a) {
  char kind[8];
  buf host = { 0 };
  int port;
  cn_address(r, a[0], kind, &host, &port);
  int here = !host.s[0] || !strcmp(host.s, "127.0.0.1") || !strcmp(host.s, "localhost");
  buf_free(&host);
  if (!here) fail(r, "phrase", "a lingo listens on this computer alone, like %s://127.0.0.1:%d", kind, port);
  return v_num(r->a, (double)cn_start(r, kind, "127.0.0.1", port, 1, NULL));
}

static val *connection_send(run *r, val **a) {
  cn *k = cn_of(r, a[0], 0);
  val *data = bytes_of(r, a[1]);
  if (!pl_put(k->c, data->t.s, data->t.len)) fail(r, "phrase", "the connection was closed");
  return NOTHING;
}

// Everything that came and wasn't taken, else what comes within seconds, else nothing. For a listener, the next
// connection come in, as a connection of this run.
static val *taken(run *r, cn *k, double seconds) {
  double until = pl_now() + (seconds >= 0 && seconds < 3600 ? seconds : 30);
  for (;;) {
    val *got = NULL;
    pl_conn *came = NULL;
    pl_enter(core_gate);
    if (k->listening && k->waiting.n >= sizeof came) {
      memcpy(&came, k->waiting.s, sizeof came);
      k->waiting.n -= sizeof came;
      memmove(k->waiting.s, k->waiting.s + sizeof came, k->waiting.n);
    } else if (!k->listening && k->waiting.n) {
      got = v_bytes(r->a, k->waiting.s, k->waiting.n);
      k->waiting.n = 0;
    }
    int ended = k->ended;
    if (!got && !came && !ended) k->news = 0;
    pl_leave(core_gate);
    if (came) return v_num(r->a, (double)cn_start(r, k->kind, "127.0.0.1", k->port, 0, came));
    if (got) return got;
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    if (ended) fail(r, "phrase", "the connection was closed");
    if (pl_now() >= until) return NOTHING;
    lingo_wait(&k->news, r->stop, until - pl_now());
  }
}

static val *connection_receive(run *r, val **a) { return taken(r, cn_of(r, a[0], 0), a[1]->k == V_NUM ? a[1]->n : 30); }

static val *connection_accept(run *r, val **a) { return taken(r, cn_of(r, a[0], 1), a[1]->k == V_NUM ? a[1]->n : 30); }

static val *connection_close(run *r, val **a) {
  cn *k = cns;
  while (k && !(a[0]->k == V_NUM && k->id == a[0]->n)) k = k->next;
  if (!k) fail(r, "phrase", "no connection %s is open in this run", text_of(r, a[0])->t.s);
  cn_let_go(k);
  return NOTHING;
}

/* lingo(...) and use(...) as a program reaches them, in, with for, the request it runs for: asked of the running native
   lingo that answers them, which waits, else POSTed to the settings' ask, a web address that answers the same way. */
static val *core_asked(run *r, buf *in) {
  buf out = { 0 };
  buf why = { 0 };
  size_t k = 1;
  const char *at = text_or(set.all, "ask", NULL);
  val *got = NULL;
  val *answer;
  val *text;
  if (job.id) {
    in->n--;
    buf_str(in, ",\"for\":");
    buf_str(in, job.id);
    buf_chr(in, '}');
  }
  while (k < 64 && !(on[k] && hooks[k].ask)) k++;
  if (k < 64 && hooks[k].ask(in->s, in->n, r->stop, &out))
    got = json_parse(r->a, out.s ? out.s : "", out.n, NULL);
  else if (at) {
    val *headers = v_rec(r->a);
    rec_set(r->a, headers, "content-type", v_cstr(r->a, "application/json"));
    if ((at = reached(r->a, at, 0, &why)) &&
        (answer = pl_http(r->a, "POST", at, headers, in->s, in->n, set.ms / 1000, &why)) &&
        (text = rec_get(answer, "text")) && text->k == V_TEXT)
      got = json_parse(r->a, text->t.s, text->t.len, NULL);
  }
  const char *problem = why.n ? v_text(r->a, why.s, why.n)->t.s : "This lingo client isn't connected to Onsay";
  buf_free(in);
  buf_free(&out);
  buf_free(&why);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got || got->k != V_REC) fail(r, "phrase", "%s", problem);
  const char *error = text_or(got, "error", NULL);
  if (error) fail(r, text_or(got, "code", "phrase"), "%s", error);
  return rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
}

// lingo.h: a line said by the running request, sent on by the running native lingo that sends lines on, if any.
int lingo_said(const char *text) {
  size_t k = 1;
  while (k < 64 && !(on[k] && hooks[k].said)) k++;
  return k < 64 && job.id && hooks[k].said(job.id, text);
}

const char *lingo_job(void) { return job.id; }

/* A request of the lingo function, with what was around it (pl_context) and the function asking, never the answer
   itself. gives is what lingo.run says it wants, NULL for lingo.ways, which asks for the ways, nothing run. */
static val *lingo_asked(run *r, val *ask, val *values, val *gives) {
  buf in = { 0 };
  buf_str(&in, "{\"ask\":");
  buf_json(&in, ask);
  buf_str(&in, ",\"values\":");
  buf_json(&in, values);
  if (gives) {
    buf_str(&in, ",\"gives\":");
    buf_json(&in, gives);
  } else
    buf_str(&in, ",\"mode\":\"list\"");
  buf_str(&in, ",\"context\":");
  pl_context(&in);
  if (job.fn) {
    buf_str(&in, ",\"asking\":");
    buf_json_str(&in, job.fn, strlen(job.fn));
  }
  buf_chr(&in, '}');
  return core_asked(r, &in);
}

/* Whether a call that is missing or fails may ask the lingo search: never inside a try, which wants the error, never in a
   request Onsay itself sent, so a search never asks itself back, and never for lingo(...) and use(...) themselves. */
static int searchable(run *r, const char *id) {
  return !r->catching && strcmp(job.caller ? job.caller : "", "onsay") && strncmp(id, "lingo.", 6);
}

/* A call this computer hasn't got (code unknown), or one whose phrase failed (code phrase), with its values: asked of the
   lingo search, which finds a way and gives back its answer, else the error it always was, message. Offline, or with the
   onsay lingo's setting search_calls false, it is that error at once. */
static val *searched(run *r, const char *id, val *values, const char *code, const char *message) {
  val *off = lingo_setting(r->a, "onsay", "search_calls");
  int no = off && (off->k == V_BOOL ? !truthy(off) : off->k == V_TEXT && !strcmp(off->t.s, "false"));
  if (no || !searchable(r, id) || !onsay_ready()) fail(r, code, "%s", message);
  val *with = v_rec(r->a);
  rec_set(r->a, with, "values", values);
  const char *ask = strcmp(code, "unknown") ? arena_fmt(r->a, "%s, which failed here: %s", id, message) : id;
  trap_t t;
  void *volatile prev = r->trap;
  volatile int depth = r->depth;
  const char *volatile lingo = r->lingo;
  r->trap = &t;
  if (!setjmp(t.j)) {
    val *out = lingo_asked(r, v_cstr(r->a, ask), with, v_cstr(r->a, ""));
    r->trap = prev;
    return out;
  }
  r->trap = prev;
  r->depth = depth;
  r->lingo = lingo;
  if (!strcmp(r->code, "stopped") || !strcmp(r->code, "limit") || !strcmp(r->code, "denied")) rethrow(r);
  fail(r, code, "%s", message);
  return NOTHING;
}

// lingo.run(ask, values, gives).
static val *lingo_run(run *r, val **a) { return lingo_asked(r, a[0], a[1], a[2]); }

// lingo.ways(ask, values).
static val *lingo_ways(run *r, val **a) { return lingo_asked(r, a[0], a[1], NULL); }

// lingo.use(address, values).
static val *lingo_use(run *r, val **a) {
  buf in = { 0 };
  buf_str(&in, "{\"address\":");
  buf_json(&in, a[0]);
  buf_str(&in, ",\"values\":");
  buf_json(&in, a[1]);
  buf_chr(&in, '}');
  return core_asked(r, &in);
}

static val *setting_(run *r, val **a), *settings_get(run *r, val **a), *settings_set(run *r, val **a);   // 7.
static val *lingo_list(run *r, val **a), *lingo_code(run *r, val **a), *lingo_save(run *r, val **a);   // 8.

#ifndef __ANDROID__
#if defined(__aarch64__) && !defined(_WIN32)
// libffi's arm64 closures want this from a part of it not in vendor/. native.call never makes a closure.
void *ffi_data_to_code_pointer(void *data) { return data; }
#endif

// A layout name to its libffi type. text and cstring are a pointer; a number is sent and read as that width.
static ffi_type *native_type(const char *name) {
  if (!strcmp(name, "none")) return &ffi_type_void;
  if (!strcmp(name, "i8")) return &ffi_type_sint8;
  if (!strcmp(name, "u8")) return &ffi_type_uint8;
  if (!strcmp(name, "i16")) return &ffi_type_sint16;
  if (!strcmp(name, "u16")) return &ffi_type_uint16;
  if (!strcmp(name, "i32")) return &ffi_type_sint32;
  if (!strcmp(name, "u32")) return &ffi_type_uint32;
  if (!strcmp(name, "i64")) return &ffi_type_sint64;
  if (!strcmp(name, "u64")) return &ffi_type_uint64;
  if (!strcmp(name, "f32")) return &ffi_type_float;
  if (!strcmp(name, "f64")) return &ffi_type_double;
  if (!strcmp(name, "pointer") || !strcmp(name, "text") || !strcmp(name, "cstring")) return &ffi_type_pointer;
  return NULL;
}

// One argument's room, big enough for any scalar or a pointer, and its address is what libffi is given.
typedef union {
  int8_t i8;
  uint8_t u8;
  int16_t i16;
  uint16_t u16;
  int32_t i32;
  uint32_t u32;
  int64_t i64;
  uint64_t u64;
  float f32;
  double f64;
  void *ptr;
} native_slot;

// A number put into a slot as the layout's width, or a text's bytes as a pointer.
static void native_put(run *r, native_slot *s, const char *t, val *v) {
  if (!strcmp(t, "text") || !strcmp(t, "cstring")) {
    s->ptr = (void *)text_of(r, v)->t.s;
    return;
  }
  if (!strcmp(t, "pointer")) {
    s->ptr = (void *)(uintptr_t)num_of(r, v, "a pointer");
    return;
  }
  double n = num_of(r, v, "an argument");
  if (!strcmp(t, "i8"))
    s->i8 = (int8_t)n;
  else if (!strcmp(t, "u8"))
    s->u8 = (uint8_t)n;
  else if (!strcmp(t, "i16"))
    s->i16 = (int16_t)n;
  else if (!strcmp(t, "u16"))
    s->u16 = (uint16_t)n;
  else if (!strcmp(t, "i32"))
    s->i32 = (int32_t)n;
  else if (!strcmp(t, "u32"))
    s->u32 = (uint32_t)n;
  else if (!strcmp(t, "i64"))
    s->i64 = (int64_t)n;
  else if (!strcmp(t, "u64"))
    s->u64 = (uint64_t)n;
  else if (!strcmp(t, "f32"))
    s->f32 = (float)n;
  else if (!strcmp(t, "f64"))
    s->f64 = n;
}

/* native.call(library, function, returns, args): the floor's one call into the system, any function of it or of a
   library by name and the shape of what it takes and returns, arranged by libffi (vendor/libffi). Not on Android yet:
   its 32-bit chips' parts of libffi aren't in vendor/. */
static val *native_call(run *r, val **a) {
  const char *library = text_of(r, a[0])->t.s;
  const char *function = text_of(r, a[1])->t.s;
  const char *ret = text_of(r, a[2])->t.s;
  val *args = a[3] && a[3]->k == V_LIST ? a[3] : NULL;
  size_t n = args ? args->l.n : 0;
  ffi_type *rtype = native_type(ret);
  if (!rtype) fail(r, "phrase", "native.call doesn't know the return type %s", ret);
  void *sym = pl_symbol(library, function);
  if (!sym) fail(r, "phrase", "%s was not found%s%s", function, library[0] ? " in " : "", library);
  ffi_type **atypes = arena_get(r->a, (n ? n : 1) * sizeof *atypes);
  void **avalues = arena_get(r->a, (n ? n : 1) * sizeof *avalues);
  native_slot *slots = arena_get(r->a, (n ? n : 1) * sizeof *slots);
  for (size_t i = 0; i < n; i++) {
    val *arg = args->l.v[i];
    if (arg->k != V_REC) fail(r, "phrase", "each argument is a record like {type: \"i32\", value: 5}");
    const char *t = text_of(r, rec_get(arg, "type") ? rec_get(arg, "type") : NOTHING)->t.s;
    atypes[i] = native_type(t);
    if (!atypes[i] || atypes[i] == &ffi_type_void)
      fail(r, "phrase", "native.call doesn't know the argument type %s", t);
    native_put(r, &slots[i], t, rec_get(arg, "value") ? rec_get(arg, "value") : NOTHING);
    avalues[i] = &slots[i];
  }
  ffi_cif cif;
  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, atypes) != FFI_OK)
    fail(r, "phrase", "native.call couldn't arrange the call to %s", function);
  union {
    ffi_arg i;
    float f32;
    double f64;
    void *ptr;
  } out;
  ffi_call(&cif, FFI_FN(sym), &out, avalues);
  if (!strcmp(ret, "none")) return NOTHING;
  if (!strcmp(ret, "f32")) return v_num(r->a, out.f32);
  if (!strcmp(ret, "f64")) return v_num(r->a, out.f64);
  if (!strcmp(ret, "cstring")) return out.ptr ? v_cstr(r->a, (const char *)out.ptr) : NOTHING;
  if (!strcmp(ret, "pointer")) return v_num(r->a, (double)(uintptr_t)out.ptr);
  if (!strcmp(ret, "i8")) return v_num(r->a, (int8_t)out.i);
  if (!strcmp(ret, "i16")) return v_num(r->a, (int16_t)out.i);
  if (!strcmp(ret, "i32")) return v_num(r->a, (int32_t)out.i);
  if (!strcmp(ret, "i64")) return v_num(r->a, (double)(int64_t)out.i);
  if (!strcmp(ret, "u64")) return v_num(r->a, (double)(uint64_t)out.i);
  return v_num(r->a, (double)(uint64_t)(uint32_t)out.i);   // u8, u16, u32
}
#endif

// lingo.run, lingo.use and lingo.setting are lingo(...), use(...) and setting(...), the language's own, in no listing.
static void floor_phrases(void) {
  phrase_add("compute", "none", "now() -> number: seconds since 1970", time_);
  phrase_add("compute", "none", "wait(seconds: number): at most 60", wait_);
  phrase_add("compute", "none", "log(x: any): a line in the lingo client's log", log_);
  phrase_add("store", "store", "get(key: text) -> text?: Onsay's own storage on this computer", get);
  phrase_add("store", "store", "put(key: text, value: any): kept as text", core_put);
  phrase_add("store", "store", "keys(prefix: text=\"\") -> [text]: sorted", keys);
  phrase_add("store", "store", "delete(key: text)", delete_);
  phrase_add("http", "net",
      "send(method: text, url: url, headers: record=nothing, body: text=nothing, timeout: number=30) -> {status: whole, headers: record, text: text}: one request, "
      "headers a record, body text, redirects followed, at most 4 MB back",
      core_send);
  phrase_add("socket", "net",
      "open(url: url, headers: record=nothing) -> whole: a WebSocket to a ws:// or wss:// address or an Onsay name, its "
      "number for the other socket phrases. Closed when the run ends, if not before",
      socket_open);
  phrase_add("socket", "net", "send(socket: whole, text: text): one message", socket_send);
  phrase_add("socket", "net",
      "receive(socket: whole, seconds: number=30) -> text?: the next message, or nothing when none came in seconds",
      socket_receive);
  phrase_add("socket", "net", "close(socket: whole)", socket_close);
  phrase_add("connection", "net",
      "open(address: text) -> whole: bytes over a connection, its kind first, like tcp://example.com:80 or udp://10.0.0.5:53, "
      "its number for the other connection phrases. Closed when the run ends, if not before",
      connection_open);
  phrase_add("connection", "net", "send(connection: whole, data: bytes): all of it, text as its UTF-8 bytes, for udp one packet",
      connection_send);
  phrase_add("connection", "net",
      "receive(connection: whole, seconds: number=30) -> bytes?: everything come and not yet taken, else what comes within "
      "seconds, else nothing",
      connection_receive);
  phrase_add("connection", "net", "close(connection: whole)", connection_close);
#ifndef __ANDROID__
  phrase_add("native", "run",
      "call(library: text, function: text, returns: text, args: list=nothing) -> any: calls any function of this "
      "computer's system or a library by name. library is its file, like \"kernel32.dll\" or \"libc.so.6\", or \"\" for "
      "this program. returns and each argument's type is one of i8 u8 i16 u16 i32 u32 i64 u64 (whole numbers), f32 f64 "
      "(decimals), pointer, text, cstring, or none. args is a list of {type, value}",
      native_call);
#endif
  phrase_add("connection", "net",
      "listen(address: text) -> whole: connections coming in on this computer, like tcp://127.0.0.1:7000, its number for "
      "connection.accept",
      connection_listen);
  phrase_add("connection", "net",
      "accept(listener: whole, seconds: number=30) -> whole?: the next connection come in, else nothing within seconds",
      connection_accept);
  phrase_add("settings", "none",
      "get(lingo: text) -> [record]: every setting of a lingo, as its openapi.json and its code say, each "
      "with its value now",
      settings_get);
  phrase_add("settings", "none", "set(lingo: text, key: text, value: any): kept wherever that lingo keeps its settings", settings_set);
  phrase_add("lingo", "none", "run(ask: text, values: record=nothing, gives: text=\"\")", lingo_run);
  phrase_add("lingo", "none",
      "ways(ask: text, values: record=nothing) -> [{does: text, where: text, address: url, inputs: record, needs?: [text]}]: the best ways to do it, best first, nothing run",
      lingo_ways);
  phrase_add("lingo", "none", "use(address: url, values: record=nothing)", lingo_use);
  phrase_add("lingo", "none", "setting(key: text, reference: text)", setting_);
  phrase_add("lingo", "none",
      "list() -> [{name: text, about: text, functions: [{name: text, does: text}]}]: every lingo running on this computer "
      "but those built into it, what it is and its functions",
      lingo_list);
  phrase_add("lingo", "none",
      "code(name: text) -> {code: text, openapi: record}: a lingo of this computer as its maker writes it, to change it: its "
      "code, and its openapi.json's info and x-lingo, such as its settings",
      lingo_code);
  phrase_add("lingo", "lingos",
      "save(name: text, code: text, openapi: record=nothing) -> [{name: text, does: text}]: a lingo written or changed and "
      "running on this computer at once, as a folder of its code and, when given, its openapi.json: its functions, or why "
      "it can't run, like its code's first mistake",
      lingo_save);
}

////////// 3. LINGOS //////////

/* Core, and the lingos built into this program for its platform (BUILTINS), are always there and always running. Every
   other is a .lingo file in the lingos folder, a zip of its openapi.json and its program, or the same files in a folder
   of its name, as a lingo is written: its program is <name>.ls, in Lingo, or a native library, <name>.dll or <name>.so.
   One that can't load is set aside with its reason, and the rest still load. A lingo from the folder is running, its
   routes answering and in the catalog, or stopped, as home/running.json says. */
static arena *library;
static int serving;   // how many requests are being answered, one inside another's wait
static arena *retired[16];   // what lingos were read into while a request ran, freed once none runs
static int retiring;
static val *device;   // the compiler's instance of every lingo that loaded
static val *found;   // every lingo read: {name, from, manifest, program, file and code or library,
    // native, running, fresh, error}. fresh: its paths are made from its code
static val *catalog;   // every running lingo, with its manifest or why it can't load
static unsigned long long looked;   // a fingerprint of the lingos folder and running.json when they were last read
#define FNV 14695981039346656037ULL

/* The lingos built into this program, each from ../lingos/<name>: its name, its openapi.json as text, and the function
   in its C that adds its phrases. builtins.h is made by build.ps1:
   onsay alone today, on every platform. Each is never read from the lingos folder, always runs, and can't be stopped. */
typedef struct {
  const char *name;
  const char *manifest;
  lingo_entry entry;
} builtin;
#include "builtins.h"
#define BUILTIN "built in"   // what a built-in lingo's from and program say

// A call of the onsay lingo no request ever reaches: what would sign this computer out, reach a setting's value by its
// lingo's reference, or what a caller can as well do itself.
static int private_call(const char *id) {
  return !strcmp(id, "onsay.sign_in") || !strcmp(id, "onsay.sign_out") || !strcmp(id, "lingo.setting") ||
      !strcmp(id, "lingo.use");
}

/* The standard functions every lingo can call with nothing to install, like text.split, list.sort and request.get, from
   ../lingos/standard: each group's name and its Lingo code (standard.h, made by build.ps1). They compile with every lingo
   and run as part of the lingo calling them, under its permissions, and are no app: no route, no catalog entry, no switch. */
typedef struct {
  const char *name;
  const char *code;
} standard;
#include "standard.h"

// Whether name is a group of standard functions.
static int is_standard(const char *name) {
  for (size_t i = 0; i < sizeof STANDARD / sizeof *STANDARD; i++) {
    if (!strcmp(STANDARD[i].name, name)) return 1;
  }
  return 0;
}

// The built-in lingo of that name, or NULL.
static const builtin *builtin_of(const char *name) {
  for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
    if (!strcmp(BUILTINS[i].name, name)) return &BUILTINS[i];
  }
  return NULL;
}

// Every native lingo by its number, core first, by its name. A library is loaded once and kept, since its phrases are in
// the table from then on: a changed one is used after a restart.
static const char *natives[64] = { "core" };
static size_t native_total = 1;

const char *native_name(size_t lingo) { return lingo < native_total ? natives[lingo] : "core"; }

static unsigned long long fnv(unsigned long long h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void print_file(const char *name, int folder, double size, double changed, void *arg) {
  unsigned long long *h = arg;
  *h = fnv(*h, name, strlen(name));
  *h = fnv(*h, &folder, sizeof folder);
  *h = fnv(*h, &size, sizeof size);
  *h = fnv(*h, &changed, sizeof changed);
}

typedef struct {
  arena *a;
  val *names;
  unsigned long long *print;
} seen;

// Each folder, and each .lingo file, fingerprinted as it is listed.
static void collect(const char *name, int folder, double size, double changed, void *arg) {
  seen *f = arg;
  size_t len = strlen(name);
  if (name[0] == '.' || (!folder && (len <= 6 || strcmp(name + len - 6, ".lingo")))) return;
  list_push(f->a, f->names, v_cstr(f->a, name));
  print_file(name, folder, size, changed, f->print);
}

// What is in the lingos folder now, and a fingerprint of every file of it, names, sizes and times, and of running.json,
// so any change shows, a lingo started or stopped from the command line too.
static val *look(arena *a, unsigned long long *print) {
  buf path = { 0 };
  buf running = { 0 };
  *print = FNV;
  seen f = { a, v_list(a), print };
  pl_list(set.lingos, collect, &f);
  for (size_t i = 0; i < f.names->l.n; i++) {
    path.n = 0;
    buf_fmt(&path, "%s/%s", set.lingos, f.names->l.v[i]->t.s);
    pl_list(path.s, print_file, print);   // nothing for a file
  }
  path.n = 0;
  buf_fmt(&path, "%s/running.json", set.home);
  if (pl_read(path.s, &running)) *print = fnv(*print, running.s, running.n);
  buf_free(&path);
  buf_free(&running);
  return f.names;
}

// Sets one aside, with why.
static void set_aside(arena *a, val *e, const char *why) {
  rec_set(a, e, "error", v_cstr(a, why));
  say("Lingo %s can't be used: %s", text_or(e, "from", ""), why);
}

// One file of a lingo, from its .lingo file's zip, or from its folder when zip is NULL.
static int lingo_piece(const char *path, buf *zip, const char *file, buf *out) {
  buf at = { 0 };
  buf_fmt(&at, "%s/%s", path, file);
  int got = zip ? zip_file(zip->s, zip->n, file, out) : pl_read(at.s, out);
  buf_free(&at);
  return got;
}

static int native_called(const char *name) {
  for (size_t i = 0; i < native_total; i++)
    if (!strcmp(natives[i], name)) return (int)i;
  return -1;
}

/* 3. One lingo, from its .lingo file or a folder of its files, at path: {name, from, manifest, program, and file and code
   or library}, with error when it can't be used. NULL for a folder with neither an openapi.json nor a program, which
   isn't a lingo. Its program is the one its openapi.json's x-lingo names, else whichever of <name>.ls, <name>.dll and
   <name>.so is there. A library in a .lingo file is put in home/libraries to be loaded, unless it is loaded already. */
static val *read_lingo(arena *a, const char *path) {
  const char *base = path + strlen(path);
  const char *problem = NULL;
  while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
  size_t len = strlen(base);
  int zipped = len > 6 && !strcmp(base + len - 6, ".lingo");
  val *e = v_rec(a);
  val *m = NULL;
  val *x;
  val *settings;
  val *keep;
  buf zip = { 0 };
  buf text = { 0 };
  buf program = { 0 };
  buf code = { 0 };
  rec_set(a, e, "name", v_text(a, base, zipped ? len - 6 : len));
  rec_set(a, e, "from", v_cstr(a, base));
  const char *name = text_or(e, "name", "");
  const char *own = text_or(e, "from", "");
  const char *route;
  const char *wanted;
  const char *programs[] = { arena_fmt(a, "%s.ls", name), arena_fmt(a, "%s.dll", name), arena_fmt(a, "%s.so", name) };
  if (zipped) pl_read(path, &zip);
  int manifest = lingo_piece(path, zipped ? &zip : NULL, "openapi.json", &text);
  int k = 0;
  m = manifest ? json_parse(a, text.s, text.n, &problem) : v_rec(a);
  x = rec_get(m, "x-lingo");
  settings = rec_get(x, "settings");
  keep = rec_get(x, "keep");
  route = text_or(x, "route", NULL);
  wanted = text_or(x, "program", NULL);
  while (k < 3 && !((!wanted || !strcmp(wanted, programs[k])) && lingo_piece(path, zipped ? &zip : NULL, programs[k], &code)))
    k++;
  if (k < 3) buf_str(&program, programs[k]);
  rec_set(a, e, "manifest", m ? m : v_rec(a));

  int listed = !settings || settings->k == V_LIST;
  for (size_t i = 0; listed && settings && i < settings->l.n; i++)
    listed = settings->l.v[i]->k == V_TEXT || text_or(settings->l.v[i], "name", NULL);
  if (!zipped && !manifest && !program.n)
    e = NULL;
  else if (zipped && !manifest)
    set_aside(a, e, "it isn't a .lingo file: a zip of its openapi.json and its program, made with pack");
  else if (problem || m->k != V_REC)
    set_aside(a, e, "its openapi.json isn't a JSON record");
  else if (!named(name))
    set_aside(a, e, "its name is lower case letters, digits, _ and -, like notes, and never lingo");
  else if (text_or(x, "name", NULL) && strcmp(text_or(x, "name", ""), name))
    set_aside(a, e, arena_fmt(a, "its openapi.json calls it %s, and its file %s", text_or(x, "name", ""), own));
  else if (wanted && !program.n)
    set_aside(a, e,
        arena_fmt(a, "its openapi.json's program, %s, isn't beside it: it is %s.ls, or a library like %s.dll", wanted,
            name, name));
  else if (!program.n)
    set_aside(a, e,
        arena_fmt(
            a, "its program goes beside its openapi.json, named for it: %s.ls, or a library like %s.dll", name, name));
  else if (!listed)
    set_aside(a, e,
        "its settings are a list, each a name or a record with its name, like {\"name\": \"key\", \"secret\": true}");
  else if (keep && (!text_or(keep, "read", NULL) || !text_or(keep, "write", NULL)))
    set_aside(a, e,
        "keep names its own functions that read and write its settings, like {\"read\": \"read\", \"write\": \"write\"}");
  else if (route &&
      (route[0] != '/' || !route[1] || strpbrk(route, "?# ") || route[strlen(route) - 1] == '/' ||
          !strncmp(route, "/.lingo", 7)))
    set_aside(a, e, "its route is a path of its own, like /onsay");
  else {
    rec_set(a, e, "program", v_text(a, program.s, program.n));
    if (k == 0) {
      rec_set(a, e, "file", v_text(a, program.s, program.n));
      rec_set(a, e, "code", v_text(a, code.s ? code.s : "", code.n));
    } else if (!zipped)
      rec_set(a, e, "library", v_cstr(a, arena_fmt(a, "%s/%s", path, program.s)));
    else {
      // Written only when it changed, so a library another lingo client here has loaded is left as it is.
      buf at = { 0 };
      buf now = { 0 };
      buf_fmt(&at, "%s/libraries/%s", set.home, name);
      pl_folder(at.s);
      buf_fmt(&at, "/%s", program.s);
      int same = pl_read(at.s, &now) && now.n == code.n && !memcmp(now.s, code.s, code.n);
      if (native_called(name) < 0 && !same && !pl_write(at.s, code.s, code.n))
        set_aside(a, e, arena_fmt(a, "its library couldn't be put in %s", at.s));
      else
        rec_set(a, e, "library", v_text(a, at.s, at.n));
      buf_free(&at);
      buf_free(&now);
    }
  }
  buf_free(&zip);
  buf_free(&text);
  buf_free(&program);
  buf_free(&code);
  return e;
}

// A lingo read, by its name, or NULL.
static val *entry_of(const char *name) {
  for (size_t i = 0; found && i < found->l.n; i++)
    if (!strcmp(text_or(found->l.v[i], "name", ""), name)) return found->l.v[i];
  return NULL;
}

static val *manifest_of(const char *lingo) { return rec_get(entry_of(lingo), "manifest"); }

// Where a lingo answers: the route its manifest's x-lingo names, like /onsay, else /<name>.
static const char *route_of(arena *a, val *e) {
  const char *own = text_or(rec_get(rec_get(e, "manifest"), "x-lingo"), "route", NULL);
  return own ? own : arena_fmt(a, "/%s", text_or(e, "name", ""));
}

// The functions a lingo shares at its routes: serve alone when it has serve(request), else every one.
static val *shared(arena *a, const char *name) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), name), "phrases");
  val *out = v_list(a);
  const char *serve = arena_fmt(a, "%s.serve", name);
  // The onsay lingo holds the base calls, its own and the core's, every one at /onsay beside its door for prompts.
  if (!strcmp(name, "onsay")) {
    val *lists[2] = { own, rec_get(rec_get(rec_get(device, "lingos"), "core"), "phrases") };
    for (int k = 0; k < 2; k++)
      for (size_t i = 0; lists[k] && i < lists[k]->l.n; i++)
        if (!private_call(text_or(lists[k]->l.v[i], "id", ""))) list_push(a, out, lists[k]->l.v[i]);
    return out;
  }
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), serve)) {
      list_push(a, out, own->l.v[i]);
      return out;
    }
  for (size_t i = 0; own && i < own->l.n; i++) list_push(a, out, own->l.v[i]);
  return out;
}

// What a manifest says fn needs, the x-lingo-needs of the operation whose x-lingo-function it is, or nothing listed.
static val *said_needs(arena *a, val *m, const char *fn) {
  val *paths = rec_get(m, "paths");
  for (size_t i = 0; paths && paths->k == V_REC && i < paths->r.n; i++)
    for (size_t k = 0; paths->r.v[i]->k == V_REC && k < paths->r.v[i]->r.n; k++) {
      val *op = paths->r.v[i]->r.v[k];
      val *needs = rec_get(op, "x-lingo-needs");
      if (!strcmp(text_or(op, "x-lingo-function", ""), fn)) return needs && needs->k == V_LIST ? needs : v_list(a);
    }
  return v_list(a);
}

// Why a Lingo lingo's openapi.json says less than its code needs, or NULL when it says enough, or says no paths, which
// are then made from its code.
static const char *understated(arena *a, val *e) {
  val *fns = shared(a, text_or(e, "name", ""));
  if (!rec_get(rec_get(e, "manifest"), "paths") || rec_get(e, "fresh")) return NULL;
  for (size_t i = 0; i < fns->l.n; i++) {
    const char *id = text_or(fns->l.v[i], "id", "");
    val *needs = rec_get(fns->l.v[i], "needs");
    val *said = said_needs(a, rec_get(e, "manifest"), id);
    for (size_t k = 0; needs && k < needs->l.n; k++)
      if (!has(said, needs->l.v[k]->t.s))
        return arena_fmt(a, "%s: %s needs %s, which its openapi.json doesn't say. Pack it again to say all it needs",
            text_or(e, "file", ""), id, needs->l.v[k]->t.s);
  }
  return NULL;
}

/* 3. A lingo's whole OpenAPI manifest, as pack writes it: what its openapi.json says, its name, program and route in
   x-lingo, and each function it shares as a path under its route, its words, its values as JSON Schema, x-lingo-function,
   x-lingo-needs and x-lingo-calls, and x-lingo-internal on a helper whose comment says it is for the lingo's own use, like
   "For agent's own use", which Onsay lists nowhere. /name alone is its main, and a lingo with serve(request) shares serve
   alone, at its route. */
static val *described(arena *a, val *e) {
  const char *name = text_or(e, "name", "");
  const char *route = route_of(a, e);
  val *given = rec_get(e, "manifest");
  val *l = rec_get(rec_get(device, "lingos"), name);
  val *fns = shared(a, name);
  val *out = v_rec(a);
  val *info = v_rec(a);
  val *x = v_rec(a);
  val *paths = v_rec(a);
  val *was;
  for (size_t i = 0; (was = rec_get(given, "info")) && was->k == V_REC && i < was->r.n; i++)
    rec_set(a, info, was->r.key[i], was->r.v[i]);
  for (size_t i = 0; (was = rec_get(given, "x-lingo")) && was->k == V_REC && i < was->r.n; i++)
    rec_set(a, x, was->r.key[i], was->r.v[i]);
  if (!rec_get(info, "title")) rec_set(a, info, "title", v_cstr(a, name));
  if (!rec_get(info, "version")) rec_set(a, info, "version", v_cstr(a, "1.0.0"));
  if (!rec_get(info, "description") && rec_get(l, "about")) rec_set(a, info, "description", rec_get(l, "about"));
  rec_set(a, x, "name", v_cstr(a, name));
  rec_set(a, x, "program", rec_get(e, "program"));
  rec_set(a, x, "route", v_cstr(a, route));
  rec_set(a, out, "openapi", v_cstr(a, "3.1.0"));
  rec_set(a, out, "info", info);
  for (size_t i = 0; given && i < given->r.n; i++)
    if (strcmp(given->r.key[i], "openapi") && strcmp(given->r.key[i], "info") && strcmp(given->r.key[i], "paths") &&
        strcmp(given->r.key[i], "x-lingo"))
      rec_set(a, out, given->r.key[i], given->r.v[i]);
  for (size_t i = 0; i < fns->l.n; i++) {
    val *f = fns->l.v[i];
    val *op = v_rec(a);
    val *item = v_rec(a);
    val *says = rec_get(f, "does");
    val *schema = rec_get(f, "inputSchema");
    val *takes = rec_get(schema, "properties");
    const char *id = text_or(f, "id", "");
    const char *local = !strncmp(id, name, strlen(name)) && id[strlen(name)] == '.' ? id + strlen(name) + 1 : id;
    const char *does = text_or(f, "does", "");
    const char *words = does;
    // A lingo written in Lingo has no types in its code, so its openapi.json may say them, by function and value, its
    // answer as "->". A native's are in its signatures.
    val *own = rec_get(rec_get(rec_get(given, "x-lingo"), "types"), local);
    for (size_t k = 0; own && own->k == V_REC && takes && k < takes->r.n; k++) {
      val *type = rec_get(own, takes->r.key[k]);
      val *fallback = rec_get(takes->r.v[k], "default");
      if (!type || type->k != V_TEXT) continue;
      val *made = type_schema(a, type->t.s);
      if (fallback) rec_set(a, made, "default", fallback);
      takes->r.v[k] = made;
    }
    const char *gives = text_or(f, "gives", NULL);
    if (!gives && own && own->k == V_REC) gives = text_or(own, "->", NULL);
    int serve = !strcmp(local, "serve");
    buf path = { 0 };
    buf_str(&path, route);
    if (!serve && strcmp(local, "main")) {
      buf_chr(&path, '/');
      for (const char *c = local; *c; c++) buf_chr(&path, *c == '.' ? '/' : *c);
    }
    if (*words)
      rec_set(a, op, "summary",
          v_text(a, words, strstr(words, ". ") ? (size_t)(strstr(words, ". ") - words + 1) : strlen(words)));
    if (*does) rec_set(a, op, "description", says);
    if (!serve && takes && takes->k == V_REC && takes->r.n) {
      val *body = v_rec(a);
      val *content = v_rec(a);
      val *json = v_rec(a);
      rec_set(a, json, "schema", schema);
      rec_set(a, content, "application/json", json);
      rec_set(a, body, "content", content);
      rec_set(a, op, "requestBody", body);
    }
    val *responses = v_rec(a);
    val *ok = v_rec(a);
    rec_set(a, ok, "description",
        v_cstr(a,
            serve ? "What serve gives, for any method and any path under its route" : "The function's value, as JSON"));
    if (gives) {
      val *answer = v_rec(a);
      val *json = v_rec(a);
      rec_set(a, json, "schema", type_schema(a, gives));
      rec_set(a, answer, "application/json", json);
      rec_set(a, ok, "content", answer);
    }
    rec_set(a, responses, "200", ok);
    rec_set(a, op, "responses", responses);
    rec_set(a, op, "x-lingo-function", v_cstr(a, id));
    rec_set(a, op, "x-lingo-needs", rec_get(f, "needs") ? rec_get(f, "needs") : v_list(a));
    rec_set(a, op, "x-lingo-calls", rec_get(f, "lingos") ? rec_get(f, "lingos") : v_num(a, 0));
    if (strstr(does, "'s own use") || (serve && !strcmp(name, "onsay"))) rec_set(a, op, "x-lingo-internal", VBOOL(1));
    rec_set(a, item, serve ? "get" : "post", op);
    rec_set(a, paths, v_text(a, path.s, path.n)->t.s, item);
    buf_free(&path);
  }
  rec_set(a, out, "paths", paths);
  rec_set(a, out, "x-lingo", x);
  return out;
}

static val *declared(arena *a, const char *lingo, val **keep);   // 7.

/* Its manifest's x-lingo says missing, the names of its required settings with no value on this computer, never a value,
   so Onsay knows it can't run here yet. Settings a lingo keeps itself can't be seen without running it, so they count as
   set. */
static void say_missing(arena *a, val *e) {
  const char *name = text_or(e, "name", "");
  val *keep = NULL;
  val *all = declared(a, name, &keep);
  val *missing = v_list(a);
  if (keep) return;
  for (size_t i = 0; i < all->l.n; i++) {
    val *s = all->l.v[i];
    val *wanted = rec_get(s, "required");
    val *now = lingo_setting(a, name, text_or(s, "name", ""));
    if (!wanted || !truthy(wanted) || rec_get(s, "default")) continue;
    if (!now || now->k == V_NOTHING || (now->k == V_TEXT && !now->t.len)) list_push(a, missing, rec_get(s, "name"));
  }
  if (missing->l.n) rec_set(a, rec_get(rec_get(e, "manifest"), "x-lingo"), "missing", missing);
}

// Reads everything again: every lingo, the natives, the graph, the manifests and the catalog, with extra, a folder being
// packed, in place of any lingo of its name. Built beside what there was, which stays in use when even core's own lingos
// can't load now. 1 when it loaded.
static int load(const char *extra) {
  arena *was = library;
  val *was_device = device;
  val *was_catalog = catalog;
  val *was_found = found;
  arena *a = library = arena_new();
  val *names = look(a, &looked);
  val *stacked = v_list(a);
  val *offered = v_list(a);
  val *running = home_json(a, "running.json");
  val *more = NULL;
  found = v_list(a);

  // The built-in lingos first, so one in the folder with the same name is set aside.
  for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
    val *e = v_rec(a);
    rec_set(a, e, "name", v_cstr(a, BUILTINS[i].name));
    rec_set(a, e, "from", v_cstr(a, BUILTIN));
    rec_set(a, e, "program", v_cstr(a, BUILTIN));
    rec_set(a, e, "manifest", json_parse(a, BUILTINS[i].manifest, strlen(BUILTINS[i].manifest), NULL));
    rec_set(a, e, "running", VBOOL(1));
    list_push(a, found, e);
  }
  if (extra) {
    buf path = { 0 };
    buf_str(&path, extra);
    while (path.n > 1 && (path.s[path.n - 1] == '/' || path.s[path.n - 1] == '\\')) path.s[--path.n] = 0;
    if ((more = read_lingo(a, path.s))) rec_set(a, more, "fresh", VBOOL(1));
    buf_free(&path);
  }
  for (size_t i = 0; i <= names->l.n; i++) {
    val *e = more;
    if (i < names->l.n) {
      buf path = { 0 };
      buf_fmt(&path, "%s/%s", set.lingos, names->l.v[i]->t.s);
      e = read_lingo(a, path.s);
      buf_free(&path);
      if (e && more && !strcmp(text_or(e, "name", ""), text_or(more, "name", ""))) continue;
    }
    if (!e) continue;
    if (!rec_get(e, "error") && (builtin_of(text_or(e, "name", "")) || is_standard(text_or(e, "name", ""))))
      set_aside(a, e, "something built into this program is already called that");
    else if (!rec_get(e, "error") && entry_of(text_or(e, "name", "")))
      set_aside(a, e, "another lingo here is already called that");
    rec_set(a, e, "running", VBOOL(has(running, text_or(e, "name", ""))));
    list_push(a, found, e);
  }

  // core, then every native here, each library loaded the first time it is seen. One whose lingo is gone stays loaded,
  // but isn't offered.
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *name = text_or(e, "name", "");
    const char *path = text_or(e, "library", NULL);
    const builtin *built = strcmp(text_or(e, "from", ""), BUILTIN) ? NULL : builtin_of(name);
    if (rec_get(e, "error") || (!path && !built)) continue;
    int k = native_called(name);
    if (k < 0 && native_total == sizeof natives / sizeof *natives) {
      set_aside(a, e, "this core can't take more native lingos");
      continue;
    }
    if (k < 0) {
      lingo_entry entry = NULL;
      if (built)
        entry = built->entry;
      else {
        buf why = { 0 };
        entry = pl_library(path, &why);
        if (!entry) set_aside(a, e, arena_fmt(a, "%s: %s", text_or(e, "program", ""), why.s));
        buf_free(&why);
      }
      if (!entry) continue;
      natives[native_total] = strdup(name);   // kept for as long as the core runs, like the library itself
      native_begin(loading = native_total);
      entry();
      say("Lingo %s is loaded from %s", name, text_or(e, "program", ""));
      k = (int)native_total++;
    }
    rec_set(a, e, "native", v_num(a, k));
  }
  for (size_t i = 0; i < native_total; i++) {
    val *one = v_rec(a);
    val *e = i ? entry_of(natives[i]) : NULL;
    if (i && (!e || rec_get(e, "error") || !rec_get(e, "native"))) continue;
    rec_set(a, one, "name", v_cstr(a, natives[i]));
    rec_set(a, one, "phrases", native_phrases(a, i));
    list_push(a, offered, one);
  }
  for (size_t i = 0; i < sizeof STANDARD / sizeof *STANDARD; i++) {
    val *e = v_rec(a);
    rec_set(a, e, "name", v_cstr(a, STANDARD[i].name));
    rec_set(a, e, "file", v_cstr(a, arena_fmt(a, "%s.ls", STANDARD[i].name)));
    rec_set(a, e, "code", v_cstr(a, STANDARD[i].code));
    list_push(a, stacked, e);
  }
  for (size_t i = 0; i < found->l.n; i++)
    if (!rec_get(found->l.v[i], "error") && rec_get(found->l.v[i], "code")) list_push(a, stacked, found->l.v[i]);

  // Loaded together, then each checked for saying all its code needs. While one is at fault, it is set aside with its
  // reason, and the rest load again without it.
  for (;;) {
    const char *why = NULL;
    device = lingo_load(a, offered, stacked, &why);
    for (size_t k = 0; device && k < stacked->l.n && !why; k++) why = understated(a, stacked->l.v[k]);
    if (!why) break;
    size_t k = 0;
    for (; k < stacked->l.n; k++) {
      const char *f = text_or(stacked->l.v[k], "file", "");
      size_t n = strlen(f);
      if (!strncmp(why, f, n) && (why[n] == ':' || why[n] == ' ')) break;
    }
    if (k == stacked->l.n) {
      say("The core's own lingos can't load: %s", why);
      arena_free(a);
      library = was;
      device = was_device;
      catalog = was_catalog;
      found = was_found;
      return 0;
    }
    set_aside(a, stacked->l.v[k], why);
    memmove(&stacked->l.v[k], &stacked->l.v[k + 1], (stacked->l.n - k - 1) * sizeof *stacked->l.v);
    stacked->l.n--;
  }

  // Each lingo's whole manifest: its own when it says its paths, else made from what loaded. Then the catalog: every
  // running lingo by its name, with its manifest, or why it can't load. Never its code.
  val *all = rec_get(device, "lingos");
  catalog = v_list(a);
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    val *one = v_rec(a);
    val *error = rec_get(e, "error");
    if (!error && (!rec_get(rec_get(e, "manifest"), "paths") || rec_get(e, "fresh")))
      rec_set(a, e, "manifest", described(a, e));
    if (!truthy(rec_get(e, "running"))) continue;
    if (!error) say_missing(a, e);
    rec_set(a, one, "name", rec_get(e, "name"));
    rec_set(a, one, error ? "error" : "openapi", error ? error : rec_get(e, "manifest"));
    list_push(a, catalog, one);
  }
  say("%zu lingos are ready", all->r.n - 1);
  // A request running now may be inside a lingo read before, so what that was read into is kept till none runs.
  if (serving && retiring < (int)(sizeof retired / sizeof *retired))
    retired[retiring++] = was;
  else if (!serving)
    arena_free(was);

  // Each native lingo that runs, in a lingo client that serves, told the catalog, and each that stopped told so.
  buf now = { 0 };
  buf_json(&now, catalog);
  for (size_t i = 1; i < native_total; i++) {
    val *e = entry_of(natives[i]);
    int runs = live && e && !rec_get(e, "error") && truthy(rec_get(e, "running"));
    if (hooks[i].running && (runs || on[i])) hooks[i].running(runs ? now.s : NULL);
    on[i] = runs;
  }
  buf_free(&now);
  return 1;
}

// 3.
void lingo_hook(const lingo_hooks *h) { hooks[loading] = *h; }

////////// 4. POLICY //////////

/* Who may reach what, and what each lingo may do here. A request's caller is this computer, which reaches every running
   lingo, or me, the account's own call handed in by a native lingo, a device that signed it, or anyone else by their
   address: they reach a lingo when a rule lets them in, and anyone else gets a 404, as if it weren't there.
   At every phrase, a need must be under the ceiling, the most this computer ever allows, and said in the lingo's
   manifest. Anything under it is allowed. net, store and files inside the root, this lingo client's home, are routine;
   files outside it, run, screen.read, screen.act and lingos, making and changing lingos, are logged. Every decision but
   a routine one goes in home/decisions.log. */
static const char *NEEDS[] = { "net", "store", "files.read", "files.write", "screen.read", "screen.act", "run",
  "lingos", NULL };   // the first four routine
#define ATTACHED 10   // seconds an app counts as open after it last asked for the questions waiting

// The questions waiting for the person's answer, each {id, lingo, caller} and either {need, path}, whether a lingo may,
// or {questions}, a native lingo's for the chat (lingo_ask), in the run's arena that waits.
static struct {
  val *ask;
  arena *a;
  volatile int done;
} asks[16];
static int waiting;   // how many wait, the last asked last
static long questions;   // their ids
static double polled;   // when an app last asked for them

/* 4. The policy in force, read as each request comes, so a change from the command line or by hand holds at once:
   {ceiling, default, rules, device, local, account}. Its rules are the account's, home/policy.account.json, as Onsay
   signed them, and its ceiling is this computer's, home/policy.json, the most any lingo may ever do here. Nothing is
   merged in: the server's policy already carries me, my devices and onsay. local and account are the two files as they
   are. */
static val *policy(arena *a) {
  val *local = home_json(a, "policy.json");
  val *account = home_json(a, "policy.account.json");
  val *out = v_rec(a);
  val *ceiling;
  if (!local || local->k != V_REC) local = v_rec(a);
  if (account && account->k != V_REC) account = NULL;
  ceiling = rec_get(local, "ceiling");
  if (!ceiling || ceiling->k != V_LIST) {
    ceiling = v_list(a);
    for (int i = 0; NEEDS[i]; i++) list_push(a, ceiling, v_cstr(a, NEEDS[i]));
  }
  rec_set(a, out, "ceiling", ceiling);
  rec_set(a, out, "default", v_cstr(a, "closed"));
  rec_set(a, out, "rules", account && rec_get(account, "rules") ? rec_get(account, "rules") : v_list(a));
  rec_set(a, out, "device", v_cstr(a, account ? text_or(account, "device", "") : ""));
  rec_set(a, out, "local", local);
  rec_set(a, out, "account", account ? account : NOTHING);
  return out;
}

// An address as 16 bytes, IPv4 as IPv6 writes it, ::ffff:1.2.3.4. 0 when it isn't one.
static int ip_of(const char *s, unsigned char out[16]) {
  unsigned b[4];
  unsigned v[8] = { 0 };
  int n = 0;
  int gap = -1;
  char end;
  memset(out, 0, 16);
  if (!strchr(s, ':')) {
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &b[0], &b[1], &b[2], &b[3], &end) != 4 || b[0] > 255 || b[1] > 255 ||
        b[2] > 255 || b[3] > 255)
      return 0;
    out[10] = out[11] = 255;
    for (int i = 0; i < 4; i++) out[12 + i] = (unsigned char)b[i];
    return 1;
  }
  if (s[0] == ':' && s[1] == ':') {
    gap = 0;
    s += 2;
  }
  while (*s) {
    char *after;
    unsigned long g = strtoul(s, &after, 16);
    if (n == 8 || after == s || after - s > 4 || g > 0xFFFF) return 0;
    v[n++] = (unsigned)g;
    s = after;
    if (*s && *s++ != ':') return 0;
    if (*s == ':' && s[-1] == ':') {
      if (gap >= 0) return 0;
      gap = n;
      s++;
    } else if (!*s && s[-1] == ':')
      return 0;
  }
  if (gap < 0 ? n != 8 : n > 7) return 0;
  for (int i = 0; i < n; i++) {
    int at = gap < 0 || i < gap ? i : 8 - (n - gap) + (i - gap);
    out[2 * at] = (unsigned char)(v[i] >> 8);
    out[2 * at + 1] = (unsigned char)v[i];
  }
  return 1;
}

/* A request's caller: this computer, me, Onsay acting for the account, a device that signed it, or anyone else by their
   address, ip. name is how the log names them: this computer, me, onsay, ip 203.0.113.7, or a signer's keyid, like
   laptop.brandon. account is a signer's account id, and onsay when Onsay itself sent the call. */
typedef struct {
  const char *name, *ip, *handle, *account;
  int here, me, mine, onsay;
} caller;

// Whether who, a rule's, names the caller: public, anyone, me, my devices, onsay, Onsay's own search and agent.
static int names(const char *who, const caller *c) {
  if (!strcmp(who, "public")) return 1;
  if (!strcmp(who, "me")) return c->me;
  if (!strcmp(who, "my devices")) return c->mine;
  if (!strcmp(who, "onsay")) return c->onsay;
  return 0;
}

// Whether an address or range, like 10.0.0.0/8, holds the caller's address.
static int addresses(const char *ip, const caller *c) {
  unsigned char want[16], have[16];
  char address[64];
  const char *slash = strchr(ip, '/');
  snprintf(address, sizeof address, "%.*s", slash ? (int)(slash - ip) : 63, ip);
  int bits = slash ? atoi(slash + 1) + (strchr(address, ':') ? 0 : 96) : 128;
  if (!c->ip || !ip_of(address, want) || !ip_of(c->ip, have)) return 0;
  for (int i = 0; i < bits && i < 128; i++)
    if ((want[i / 8] ^ have[i / 8]) & (0x80 >> (i % 8))) return 0;
  return 1;
}

// Whether path is route itself or inside it, / separating, so /memo/recall is under /memo but /memoirs is not.
static int under(const char *path, const char *route) {
  size_t n = strlen(route);
  if (n == 1 && route[0] == '/') return path[0] == '/';
  return n && !strncmp(path, route, n) && (!path[n] || path[n] == '/');
}

/* Whether the caller may reach route, a lingo's own, or any of it when rest is NULL (a listing), by the policy p: a rule
   that names them, names their account, or names their address lets them in when allow is true and keeps them out when
   it is false, and deny wins. A rule naming devices holds only on those computers, and one naming routes only on those
   paths. No rule lets nobody. A listing shows a lingo when an allow rule's routes touch its route, and hides it only
   when a deny rule's routes cover the whole of it. why says which rule decided. */
static int let_in(arena *a, val *p, const caller *c, const char *route, const char *rest, const char **why) {
  val *rules = rec_get(p, "rules");
  const char *device = text_or(p, "device", "");
  const char *full = rest ? arena_fmt(a, "%s%s", route, rest) : NULL;
  int allowed = 0;
  const char *by = NULL;
  for (size_t i = 0; rules && rules->k == V_LIST && i < rules->l.n; i++) {
    val *rule = rules->l.v[i];
    val *devices = rec_get(rule, "devices");
    val *routes = rec_get(rule, "routes");
    const char *who = text_or(rule, "who", NULL);
    const char *account = text_or(rule, "account", NULL);
    const char *handle = text_or(rule, "handle", NULL);
    const char *ip = text_or(rule, "ip", NULL);
    const char *label = handle ? handle : who ? who : account ? account : ip ? ip : "";
    int allow = truthy(rec_get(rule, "allow"));
    int hit = 1;
    if (who) hit = names(who, c);
    else if (account) hit = c->account && !strcmp(account, c->account);
    else if (ip) hit = addresses(ip, c);
    else hit = 0;
    if (!hit) continue;
    if (devices && devices->k == V_LIST && !has(devices, device)) continue;
    if (routes && routes->k == V_LIST) {
      hit = 0;
      for (size_t k = 0; !hit && k < routes->l.n; k++) {
        const char *r = routes->l.v[k]->k == V_TEXT ? routes->l.v[k]->t.s : "";
        if (full) hit = under(full, r);
        else if (allow) hit = under(r, route) || under(route, r);
        else hit = under(route, r);
      }
      if (!hit) continue;
    }
    if (!allow) {
      *why = arena_fmt(a, "deny: %s", label);
      return 0;
    }
    allowed = 1;
    by = label;
  }
  if (allowed) {
    *why = arena_fmt(a, "rule: %s", by);
    return 1;
  }
  *why = "no rule lets them in";
  return 0;
}

static struct {
  char *seen;
  double at;
} * nonces;   // each signature's keyid and nonce, kept 5 minutes, so none is used twice
static size_t nonce_count;

/* 4. Who signed a request, by HTTP Message Signatures (RFC 9421) with Ed25519. Its Signature-Input covers @method,
   @target-uri and content-digest at least, and says when it was made, its nonce and the signer's keyid, like
   laptop.brandon, whose key the running native lingo that looks keys up gives (key in lingo_hooks). It must check out,
   be made within 5 minutes, its nonce new in that time, and its Content-Digest the body's sha-256. Its @target-uri is
   x-forwarded-proto (else http), :// x-forwarded-host (else host), and x-forwarded-uri (else the path), so one passed on
   by a proxy is checked as it was sent. Fills in c, or gives why not. */
static const char *signer(arena *a, val *h, const char *method, const char *path, val *body, caller *c) {
  const char *input = text_or(h, "signature-input", "");
  const char *eq = strchr(input, '=');
  const char *close = eq ? strchr(eq, ')') : NULL;
  const char *p;
  const char *keyid = NULL;
  const char *nonce = NULL;
  const char *uri = text_or(h, "x-forwarded-uri", path);
  const char *host = text_or(h, "x-forwarded-host", text_or(h, "host", ""));
  const char *why = NULL;
  const char *missing = NULL;
  buf base = { 0 };
  buf raw = { 0 };
  buf digest = { 0 };
  buf key = { 0 };
  unsigned char sum[32];
  int covers = 0;
  double created = 0;
  double now = pl_now();
  if (!eq || eq[1] != '(' || !close)
    return "Its Signature-Input isn't one, like on=(\"@method\" \"@target-uri\" \"content-digest\");created=...;nonce=\"...\";keyid=\"...\"";
  for (p = eq + 2; p < close; p++) {
    if (*p != '"') continue;
    size_t n = strcspn(p + 1, "\"");
    const char *id = v_text(a, p + 1, n)->t.s;
    const char *value = NULL;
    if (!strcmp(id, "@method"))
      value = method;
    else if (!strcmp(id, "@target-uri"))
      value = arena_fmt(a, "%s://%s%s", text_or(h, "x-forwarded-proto", "http"), host, uri);
    else if (!strcmp(id, "@authority"))
      value = host;
    else if (!strcmp(id, "@path"))
      value = v_text(a, uri, strcspn(uri, "?"))->t.s;
    else if (!strcmp(id, "@query"))
      value = strchr(uri, '?') ? strchr(uri, '?') : "?";
    else if (id[0] != '@')
      value = text_or(h, id, NULL);
    if (!value) missing = id;
    covers += !strcmp(id, "@method") + !strcmp(id, "@target-uri") + !strcmp(id, "content-digest");
    buf_fmt(&base, "\"%s\": %s\n", id, value ? value : "");
    p += n + 1;
  }
  buf_fmt(&base, "\"@signature-params\": %s", eq + 1);
  for (p = close + 1; *p == ';'; p += 1 + strcspn(p + 1, ";")) {
    const char *value = p + 1 + strcspn(p + 1, "=;");
    size_t n = *value == '=' ? strcspn(++value, ";") : 0;
    const char *text = n >= 2 && value[0] == '"' ? v_text(a, value + 1, n - 2)->t.s : v_text(a, value, n)->t.s;
    if (!strncmp(p + 1, "created=", 8))
      created = atof(text);
    else if (!strncmp(p + 1, "nonce=", 6))
      nonce = text;
    else if (!strncmp(p + 1, "keyid=", 6))
      keyid = text;
  }
  const char *label = arena_fmt(a, "%.*s=:", (int)(eq - input), input);
  const char *sig = strstr(text_or(h, "signature", ""), label);
  const char *said = strstr(text_or(h, "content-digest", ""), "sha-256=:");
  size_t k = 1;
  while (k < 64 && !(on[k] && hooks[k].key)) k++;
  val *who = !missing && keyid && k < 64 ? hooks[k].key(a, keyid) : NULL;
  const char *their = text_or(who, "key", "");
  sha256(body->t.s, body->t.len, sum);
  base64(&digest, sum, 32);
  if (missing)
    why = arena_fmt(a, "Its signature covers %s, which it hasn't", missing);
  else if (covers < 3)
    why = "Its signature must cover @method, @target-uri and content-digest";
  else if (!keyid || !nonce || !created)
    why = "Its Signature-Input needs created, nonce and keyid";
  else if (created < now - 300 || created > now + 300)
    why = "Its signature wasn't made within 5 minutes";
  else if (!said || strncmp(said + 9, digest.s, digest.n) || said[9 + digest.n] != ':')
    why = "Its Content-Digest isn't its body's sha-256";
  else if (!who)
    why = arena_fmt(a, "There's no key known for %s", keyid);
  else if (!sig || !unbase64(sig + strlen(label), strcspn(sig + strlen(label), ":"), &raw) || raw.n != 64 ||
      strncmp(their, "ed25519:", 8) || !unbase64(their + 8, strlen(their + 8), &key) || key.n != 32 ||
      !ed25519_check((unsigned char *)key.s, base.s, base.n, (unsigned char *)raw.s))
    why = arena_fmt(a, "Its signature doesn't check out for %s", keyid);
  else {
    const char *seen = arena_fmt(a, "%s %s", keyid, nonce);
    size_t core_kept = 0;
    for (size_t i = 0; i < nonce_count; i++) {
      if (nonces[i].at < now - 300) {
        free(nonces[i].seen);
        continue;
      }
      if (!strcmp(nonces[i].seen, seen)) why = "Its nonce was used already";
      nonces[core_kept++] = nonces[i];
    }
    nonce_count = core_kept;
    if (!why) {
      nonces = realloc(nonces, (nonce_count + 1) * sizeof *nonces);
      nonces[nonce_count].seen = strdup(seen);
      nonces[nonce_count++].at = now;
      c->name = keyid;
      c->handle = text_or(who, "handle", NULL);
      c->account = text_or(who, "account", NULL);
      c->mine = truthy(rec_get(who, "mine") ? rec_get(who, "mine") : NOTHING);
    }
  }
  buf_free(&base);
  buf_free(&raw);
  buf_free(&digest);
  buf_free(&key);
  return why;
}

// One decision in the log, home/decisions.log, a line of JSON: {at, caller, lingo, route, need, allowed, why}, and to
// each running native lingo that wants it. need is nothing for whether the caller may reach the lingo at all.
static void core_decided(const char *need, int allowed, const char *why) {
  arena *a = arena_new();
  val *line = v_rec(a);
  buf text = { 0 };
  buf path = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  rec_set(a, line, "at", v_cstr(a, at));
  rec_set(a, line, "caller", v_cstr(a, job.caller ? job.caller : "this computer"));
  rec_set(a, line, "lingo", job.lingo ? v_cstr(a, job.lingo) : NOTHING);
  rec_set(a, line, "route", job.path ? v_cstr(a, job.path) : NOTHING);
  rec_set(a, line, "need", need ? v_cstr(a, need) : NOTHING);
  rec_set(a, line, "allowed", VBOOL(allowed));
  rec_set(a, line, "why", v_cstr(a, why));
  buf_json(&text, line);
  for (size_t i = 1; i < 64; i++)
    if (on[i] && hooks[i].decided) hooks[i].decided(text.s);
  buf_chr(&text, '\n');
  buf_fmt(&path, "%s/decisions.log", set.home);
  pl_append(path.s, text.s, text.n);
  buf_free(&text);
  buf_free(&path);
  arena_free(a);
}

_Noreturn static void refuse(run *r, const char *need, const char *why, const char *words) {
  core_decided(need, 0, why);
  fail(r, "denied", "%s", words);
}

// Whether path is folder or inside it, / and \ the same, and case aside in a Windows path, like C:\Users.
static int inside(const char *path, const char *folder) {
  size_t n = strlen(folder);
  int windows = folder[0] && folder[1] == ':';
  while (n > 1 && (folder[n - 1] == '/' || folder[n - 1] == '\\') && !(windows && n == 3)) n--;
  for (size_t i = 0; i < n; i++) {
    char x = path[i] == '\\' ? '/' : path[i];
    char y = folder[i] == '\\' ? '/' : folder[i];
    if (windows) {
      x = (char)tolower((unsigned char)x);
      y = (char)tolower((unsigned char)y);
    }
    if (!x || x != y) return 0;
  }
  return !path[n] || path[n] == '/' || path[n] == '\\' || folder[n - 1] == '/' || folder[n - 1] == '\\';
}

/* 4. A need under the ceiling, and said in the lingo's manifest, is allowed. The ceiling is the one hard local limit:
   net, store and files inside the root, this lingo client's home, are routine; anything else is logged so the person
   can see it. There is no per-caller grant and no prompt. */
void may(run *r, const char *need, const char *path) {
  val *p = job.policy ? job.policy : policy(r->a);
  const char *lingo = job.lingo;
  const char *what = path ? arena_fmt(r->a, "%s %s", need, path) : need;
  int k = 0;
  while (NEEDS[k] && strcmp(NEEDS[k], need)) k++;
  if (!has(rec_get(p, "ceiling"), need))
    refuse(r, what, "the ceiling",
        arena_fmt(r->a, "%s needs %s, which this computer never allows", lingo ? lingo : "It", need));
  if (!lingo) return;
  if (!has(job.needs, need))
    refuse(r, what, "not declared", arena_fmt(r->a, "%s needs %s, which its openapi.json doesn't say", lingo, need));
  if (k < 2 || (k < 4 && (!path || inside(path, set.root)))) return;
  core_decided(what, 1, "allowed");
}

/* 4. A question for the person, with its id, the lingo asking and its caller, and waited for in asks until an app
      answers it, stop is set or seconds pass, while other requests are answered. Its answer, nothing when none came, or
      NULL when no app is open to ask. */
val *lingo_ask(run *r, val *question, double seconds) {
  if (pl_now() - polled > ATTACHED || waiting == 16) return NULL;
  int slot = waiting++;
  val *ask = v_rec(r->a);
  rec_set(r->a, ask, "id", v_num(r->a, (double)++questions));
  rec_set(r->a, ask, "lingo", v_cstr(r->a, job.lingo ? job.lingo : ""));
  for (size_t i = 0; i < question->r.n; i++) rec_set(r->a, ask, question->r.key[i], question->r.v[i]);
  rec_set(r->a, ask, "caller", v_cstr(r->a, job.caller ? job.caller : "this computer"));
  asks[slot].ask = ask;
  asks[slot].a = r->a;
  asks[slot].done = 0;
  lingo_wait(&asks[slot].done, r->stop, seconds);
  waiting--;
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  val *answer = rec_get(ask, "answer");
  return answer ? answer : NOTHING;
}

////////// 5. STOPS //////////

// Each request running, the newest on top, one run inside another's wait: its id, as JSON, so a stop can name it, and
// its stop flag. gate keeps them, since core_stop may set one from another thread while a request runs.
typedef struct frame {
  struct frame *up;
  char id[128];
  volatile int stop;
} frame;
static frame *top;

void core_say(const char *fmt_, ...) {
  va_list ap;
  va_start(ap, fmt_);
  fprintf(stderr, "onsay: ");
  vfprintf(stderr, fmt_, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// 5. At once, even while the request runs on another thread, with every request running inside its wait.
void core_stop(const char *id) {
  pl_enter(core_gate);
  frame *hit = top;
  while (hit && strcmp(hit->id, id)) hit = hit->up;
  for (frame *f = top; hit && f != hit->up; f = f->up) f->stop = 1;
  pl_leave(core_gate);
}

////////// 6. CALLS //////////

// 6. fn, a function of a lingo here, linked from its code and run with its values in order, a value left out taking its
// default. Its value, or NULL with the error's code and message.
static val *core_call(
    arena *a, const char *fn, val *values, volatile int *stop, const char **code, const char **message) {
  val *program = lingo_call(a, device, fn);
  val *input = v_rec(a);
  char name[24];
  if (!program) {
    *code = "unknown";
    *message = v_cstr(a, fn)->t.s;
    return NULL;
  }
  for (size_t i = 0; i < values->l.n; i++) {
    snprintf(name, sizeof name, "a%zu", i);
    rec_set(a, input, name, values->l.v[i]);
  }
  long before = socks_made;
  long opened = cns_made;
  val *got = run_program(a, program, input, (limits){ set.steps, set.ms, stop }, code, message);
  while (socks && socks->id > before) let_go(socks, "as the run ended", 0);
  while (cns && cns->id > opened) cn_let_go(cns);
  return got;
}

////////// 7. LINGO SETTINGS //////////

// A lingo's settings are kept on this computer, never on Onsay: in home/settings/<lingo>.json, a record by name, or by
// the lingo's own functions when its manifest's x-lingo says keep: {read, write}, so its maker decides where they live.
// setting(...) is lingo.setting(key, reference), its value the one kept, else its manifest's default, else nothing.

static const char
    *keeping;   // the lingo whose own read or write runs now, so its setting(...) reads this computer's file

/* One lingo's settings, as a form shows them: each its manifest's x-lingo lists, a name alone or a record of its name,
   title, about, default, required and secret, then each other one its code reads, by name. keep, when asked for, is
   where its x-lingo says they are kept, {read, write}, its own functions, or NULL for this computer's file of them, as it
   is inside those functions too. */
static val *declared(arena *a, const char *lingo, val **keep) {
  val *out = v_list(a);
  val *x = rec_get(manifest_of(lingo), "x-lingo");
  val *own = rec_get(rec_get(device, "lingos"), lingo);
  val *listed = rec_get(x, "settings");
  val *read = rec_get(own, "settings");
  for (size_t i = 0; listed && listed->k == V_LIST && i < listed->l.n; i++) {
    val *s = listed->l.v[i];
    val *one = v_rec(a);
    if (s->k == V_TEXT)
      rec_set(a, one, "name", s);
    else
      for (size_t k = 0; k < s->r.n; k++) rec_set(a, one, s->r.key[k], s->r.v[k]);
    list_push(a, out, one);
  }
  for (size_t i = 0; read && i < read->l.n; i++) {
    int had = 0;
    for (size_t k = 0; k < out->l.n && !had; k++) had = !strcmp(text_or(out->l.v[k], "name", ""), read->l.v[i]->t.s);
    if (!had) {
      val *one = v_rec(a);
      rec_set(a, one, "name", read->l.v[i]);
      list_push(a, out, one);
    }
  }
  if (keep) *keep = !(keeping && !strcmp(keeping, lingo)) ? rec_get(x, "keep") : NULL;
  return out;
}

// The lingo a setting is of, checked, since it names a file, and the setting's name when there is one.
static const char *whose(run *r, val *lingo, val *key) {
  if (lingo->k != V_TEXT || !named(lingo->t.s))
    fail(r, "phrase", "a lingo's settings are named by the lingo, like agent");
  if (key && (key->k != V_TEXT || !key->t.len)) fail(r, "phrase", "a setting is named by its name, like key");
  return lingo->t.s;
}

// 7. The settings this computer keeps for a lingo, in home/settings/<lingo>.json, a record by name.
val *lingo_setting(arena *a, const char *lingo, const char *key) {
  val *s = named(lingo) ? home_json(a, arena_fmt(a, "settings/%s.json", lingo)) : NULL;
  return s && s->k == V_REC ? rec_get(s, key) : NULL;
}

// 7.
int lingo_set(const char *lingo, const char *key, val *value) {
  arena *a = arena_new();
  val *s = named(lingo) ? home_json(a, arena_fmt(a, "settings/%s.json", lingo)) : NULL;
  if (!s || s->k != V_REC) s = v_rec(a);
  rec_set(a, s, key, value);
  int ok = named(lingo) && keep_json(arena_fmt(a, "settings/%s.json", lingo), s);
  arena_free(a);
  looked = 0;   // read everything again, so the catalog says what is still missing
  return ok;
}

// A lingo's own function that keeps its settings, read(key) or write(key, value), run as that lingo's own. Its value.
static val *core_kept(run *r, const char *lingo, const char *fn, val *values) {
  const char *was = keeping;
  const char *code = NULL;
  const char *message = NULL;
  const char *was_lingo = job.lingo;
  const char *was_fn = job.fn;
  const char *id = arena_fmt(r->a, "%s.%s", lingo, fn);
  val *was_needs = job.needs;
  keeping = lingo;
  if (job.lingo) {
    job.lingo = lingo;
    job.fn = id;
    job.needs = said_needs(r->a, manifest_of(lingo), id);
  }
  val *got = core_call(r->a, id, values, r->stop, &code, &message);
  keeping = was;
  job.lingo = was_lingo;
  job.fn = was_fn;
  job.needs = was_needs;
  if (!got)
    fail(r, code, "%s, which keeps %s's settings: %s", id, lingo,
        !strcmp(code, "unknown") ? "it has no function by that name" : message);
  return got;
}

// A setting's value, kept wherever its lingo keeps it, else its manifest's default, else nothing. all: its settings, as
// declared gives them.
static val *setting_value(run *r, const char *lingo, val *key, val *all, val *keep) {
  val *v;
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, key);
    v = core_kept(r, lingo, text_or(keep, "read", ""), values);
  } else
    v = lingo_setting(r->a, lingo, key->t.s);
  for (size_t i = 0; (!v || v->k == V_NOTHING) && i < all->l.n; i++)
    if (!strcmp(text_or(all->l.v[i], "name", ""), key->t.s)) v = rec_get(all->l.v[i], "default");
  return v ? v : NOTHING;
}

// Whether the lingo running is another than lingo. The person, at the command line or through /onsay, is none.
static int other(run *r, const char *lingo) { return r->lingo && strcmp(r->lingo, lingo); }

// lingo.setting(key, reference). Another lingo's setting is read too, so one token serves many, but never one its
// manifest calls secret.
static val *setting_(run *r, val **a) {
  const char *lingo = whose(r, a[1], a[0]);
  val *keep = NULL;
  val *all = declared(r->a, lingo, &keep);
  for (size_t i = 0; other(r, lingo) && i < all->l.n; i++) {
    val *secret = rec_get(all->l.v[i], "secret");
    if (!strcmp(text_or(all->l.v[i], "name", ""), a[0]->t.s) && secret && truthy(secret))
      fail(r, "phrase", "%s is a secret of %s's, and only %s reads it", a[0]->t.s, lingo, lingo);
  }
  return setting_value(r, lingo, a[0], all, keep);
}

// settings.get(lingo), for a form of them: each its manifest lists, with what it says, then each other one its code
// reads, each with its value now.
static val *settings_get(run *r, val **a) {
  const char *lingo = whose(r, a[0], NULL);
  val *keep = NULL;
  val *all = declared(r->a, lingo, &keep);
  for (size_t i = 0; i < all->l.n; i++) {
    val *secret = rec_get(all->l.v[i], "secret");
    val *now = setting_value(r, lingo, rec_get(all->l.v[i], "name"), all, keep);
    // A secret's value goes only to its own lingo and to the person's form here, never by request, where a model may
    // read it, nor to another lingo: they get whether it is set.
    int hidden = other(r, lingo) || (job.path && job.lingo && !strcmp(job.lingo, "onsay"));
    if (hidden && secret && truthy(secret) && now->k != V_NOTHING) now = v_cstr(r->a, "set");
    rec_set(r->a, all->l.v[i], "value", now);
  }
  return all;
}

// settings.set(lingo, key, value), wherever that lingo keeps them. Only the lingo itself and the person change them.
static val *settings_set(run *r, val **a) {
  const char *lingo = whose(r, a[0], a[1]);
  if (other(r, lingo)) fail(r, "phrase", "only %s, or the person, changes %s's settings", lingo, lingo);
  val *keep = NULL;
  declared(r->a, lingo, &keep);
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, a[1]);
    list_push(r->a, values, a[2]);
    core_kept(r, lingo, text_or(keep, "write", ""), values);
    return NOTHING;
  }
  if (!lingo_set(lingo, a[1]->t.s, a[2])) fail(r, "phrase", "this computer couldn't save the setting");
  return NOTHING;
}

// A lingo's functions from its manifest, each its name and what it does, but its helpers for its own use.
static val *functions_of(arena *a, val *manifest) {
  val *paths = rec_get(manifest, "paths");
  val *out = v_list(a);
  for (size_t i = 0; paths && paths->k == V_REC && i < paths->r.n; i++) {
    val *op = rec_get(paths->r.v[i], "post");
    val *helper = rec_get(op, "x-lingo-internal");
    val *one;
    if (!op || (helper && truthy(helper))) continue;
    one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, strrchr(paths->r.key[i], '/') + 1));
    rec_set(a, one, "does", v_cstr(a, text_or(op, "summary", "")));
    list_push(a, out, one);
  }
  return out;
}

// lingo.list(): from the catalog, each running lingo that loaded, but those built in, whose calls every lingo has.
static val *lingo_list(run *r, val **a) {
  val *out = v_list(r->a);
  (void)a;
  for (size_t i = 0; catalog && i < catalog->l.n; i++) {
    val *c = catalog->l.v[i];
    val *m = rec_get(c, "openapi");
    val *one;
    if (!m || builtin_of(text_or(c, "name", ""))) continue;
    one = v_rec(r->a);
    rec_set(r->a, one, "name", v_cstr(r->a, text_or(c, "name", "")));
    rec_set(r->a, one, "about", v_cstr(r->a, text_or(rec_get(m, "info"), "description", "")));
    rec_set(r->a, one, "functions", functions_of(r->a, m));
    list_push(r->a, out, one);
  }
  return out;
}

// lingo.code(name): its code, and of its manifest what its maker writes, info and x-lingo, but what this computer adds.
static val *lingo_code(run *r, val **a) {
  const char *name = a[0]->t.s;
  val *e = entry_of(name);
  val *out = v_rec(r->a);
  val *made = v_rec(r->a);
  val *x = v_rec(r->a);
  val *m;
  val *own;
  if (!e || builtin_of(name)) fail(r, "unknown", "There's no lingo called %s here", name);
  if (!rec_get(e, "code")) fail(r, "phrase", "%s isn't written in Lingo, so it has no code to read", name);
  m = rec_get(e, "manifest");
  own = rec_get(m, "x-lingo");
  for (size_t i = 0; own && own->k == V_REC && i < own->r.n; i++)
    if (strcmp(own->r.key[i], "missing")) rec_set(r->a, x, own->r.key[i], v_copy(r->a, own->r.v[i]));
  if (rec_get(m, "info")) rec_set(r->a, made, "info", v_copy(r->a, rec_get(m, "info")));
  rec_set(r->a, made, "x-lingo", x);
  rec_set(r->a, out, "code", v_copy(r->a, rec_get(e, "code")));
  rec_set(r->a, out, "openapi", made);
  return out;
}

/* lingo.save(name, code, openapi): written as a folder in the lingos folder, in place of a .lingo file of its name, put
   among those that run, and every lingo read again at once, so it answers and Onsay is told it while the request that
   saved it still runs. Its functions, each its name and what it does, or why it can't run, in its own words. */
static val *lingo_save(run *r, val **a) {
  arena *ra = r->a;
  const char *name = a[0]->t.s;
  const char *folder = arena_fmt(ra, "%s/%s", set.lingos, name);
  val *running = home_json(ra, "running.json");
  val *e;
  may(r, "lingos", NULL);
  if (!named(name)) fail(r, "type", "A lingo's name is lower case letters, digits, _ and -, like chess-site, and never lingo");
  if (builtin_of(name) || is_standard(name)) fail(r, "denied", "%s is built into this program, so it can't be changed", name);
  if (retiring == (int)(sizeof retired / sizeof *retired))
    fail(r, "phrase", "That is as many lingos as can be saved in one go. Save it again once this is done");
  pl_folder(folder);
  if (!pl_write(arena_fmt(ra, "%s/%s.ls", folder, name), a[1]->t.s, a[1]->t.len))
    fail(r, "phrase", "this computer couldn't write %s", folder);
  if (a[2]->k != V_NOTHING) {
    buf json = { 0 };
    buf_json(&json, a[2]);
    int ok = pl_write(arena_fmt(ra, "%s/openapi.json", folder), json.s, json.n);
    buf_free(&json);
    if (!ok) fail(r, "phrase", "this computer couldn't write %s/openapi.json", folder);
  }
  pl_delete(arena_fmt(ra, "%s/%s.lingo", set.lingos, name));
  if (!running || running->k != V_LIST) running = v_list(ra);
  if (!has(running, name)) list_push(ra, running, v_cstr(ra, name));
  if (!keep_json("running.json", running)) fail(r, "phrase", "This computer couldn't save which lingos run");
  load(NULL);
  e = entry_of(name);
  if (!e) fail(r, "phrase", "%s was saved in %s, but it wasn't read back", name, folder);
  if (rec_get(e, "error")) fail(r, "type", "%s was saved, but it can't run: %s", name, text_or(e, "error", ""));
  return functions_of(ra, rec_get(e, "manifest"));
}

////////// 8. CONTROL //////////

/* 8. One thing an app or the person does, from /.lingo/<verb> or the command line, its values by name. Its answer, or
   NULL with why in words.
     start, stop {lingo}             a lingo running or stopped, kept in home/running.json
     asks                            the questions waiting for the person, and the app asking counts as open to ask
     answer {id, answer}             once, always or no to whether a lingo may, or the list of answers to questions
     settings {lingo}                its settings, each with what it is and its value now
     set {lingo, key, value}         one of them changed
     manifest {lingo}                its whole OpenAPI manifest, as the catalog has it, or why it can't load
     policy                          the policy in force, and the files it comes from
     ceiling {needs}                 the most this computer ever allows
     call {fn, values}               a function of any lingo here, or a phrase, run as this computer, its values in order,
                                     like onsay.sign_in, its value */
val *core_control(arena *a, const char *verb, val *v, const char **why) {
  const char *lingo = text_or(v, "lingo", "");
  const char *code = NULL;
  const char *message = NULL;
  val *out = NOTHING;
  val *local = home_json(a, "policy.json");
  val *values = v_list(a);
  volatile int stop = 0;
  if (!local || local->k != V_REC) local = v_rec(a);
  *why = NULL;
  if (!strcmp(verb, "start") || !strcmp(verb, "stop")) {
    val *was = home_json(a, "running.json");
    val *now = v_list(a);
    if (!entry_of(lingo)) {
      *why = arena_fmt(a, "There's no lingo called %s here", lingo);
      return NULL;
    }
    if (builtin_of(lingo)) {
      *why = arena_fmt(a, "%s is built into this program, so it always runs", lingo);
      return NULL;
    }
    for (size_t i = 0; was && was->k == V_LIST && i < was->l.n; i++)
      if (was->l.v[i]->k == V_TEXT && strcmp(was->l.v[i]->t.s, lingo)) list_push(a, now, was->l.v[i]);
    if (!strcmp(verb, "start")) list_push(a, now, v_cstr(a, lingo));
    if (!keep_json("running.json", now)) {
      *why = "This computer couldn't save which lingos run";
      return NULL;
    }
    rec_set(a, out = v_rec(a), "lingo", v_cstr(a, lingo));
    rec_set(a, out, "running", VBOOL(!strcmp(verb, "start")));
    if (serving <= 1) core_look();   // at once, unless a request waits inside, which the next look waits for
  } else if (!strcmp(verb, "asks")) {
    polled = pl_now();
    out = v_list(a);
    for (int i = 0; i < waiting; i++) list_push(a, out, v_copy(a, asks[i].ask));
  } else if (!strcmp(verb, "answer")) {
    val *id = rec_get(v, "id");
    val *answer = rec_get(v, "answer");
    const char *said = text_or(v, "answer", "");
    int i = 0;
    while (i < waiting && !(id && id->k == V_NUM && rec_get(asks[i].ask, "id")->n == id->n)) i++;
    if (i == waiting) {
      *why = "That question isn't waiting any more";
      return NULL;
    }
    if (rec_get(asks[i].ask, "need") && strcmp(said, "once") && strcmp(said, "always") && strcmp(said, "no")) {
      *why = "The answer is once, always or no";
      return NULL;
    }
    if (!rec_get(asks[i].ask, "need") && (!answer || answer->k != V_LIST)) {
      *why = "The answer is a list, one for each question, in order";
      return NULL;
    }
    // Read again into the arena of the run that waits, since this request's own, which holds its texts, ends first.
    buf core_kept = { 0 };
    buf_json(&core_kept, answer);
    rec_set(asks[i].a, asks[i].ask, "answer", json_parse(asks[i].a, core_kept.s, core_kept.n, NULL));
    buf_free(&core_kept);
    asks[i].done = 1;
  } else if (!strcmp(verb, "settings") || !strcmp(verb, "set")) {
    list_push(a, values, v_cstr(a, lingo));
    if (!strcmp(verb, "set")) {
      list_push(a, values, rec_get(v, "key") ? rec_get(v, "key") : NOTHING);
      list_push(a, values, rec_get(v, "value") ? rec_get(v, "value") : NOTHING);
    }
    out = core_call(a, !strcmp(verb, "set") ? "settings.set" : "settings.get", values, &stop, &code, &message);
    if (!out) {
      *why = message;
      return NULL;
    }
  } else if (!strcmp(verb, "manifest")) {
    val *e = entry_of(lingo);
    if (!e) {
      *why = arena_fmt(a, "There's no lingo called %s here", lingo);
      return NULL;
    }
    if (rec_get(e, "error")) {
      *why = text_or(e, "error", "");
      return NULL;
    }
    out = rec_get(e, "manifest");
  } else if (!strcmp(verb, "policy"))
    out = policy(a);
  else if (!strcmp(verb, "ceiling")) {
    val *needs = rec_get(v, "needs");
    int good = needs && needs->k == V_LIST;
    for (size_t i = 0; good && i < needs->l.n; i++) {
      int k = 0;
      while (NEEDS[k] && !(needs->l.v[i]->k == V_TEXT && !strcmp(NEEDS[k], needs->l.v[i]->t.s))) k++;
      good = NEEDS[k] != NULL;
    }
    if (!good) {
      *why = "The ceiling is a list of needs: net, store, files.read, files.write, screen.read, screen.act, run and lingos";
      return NULL;
    }
    rec_set(a, local, "ceiling", needs);
    if (!keep_json("policy.json", local)) {
      *why = "This computer couldn't save policy.json";
      return NULL;
    }
    out = policy(a);
  } else if (!strcmp(verb, "call")) {
    const char *fn = text_or(v, "fn", "");
    val *given = rec_get(v, "values");
    if (!(out = core_call(a, fn, given && given->k == V_LIST ? given : values, &stop, &code, &message))) {
      *why = !strcmp(code, "unknown")
          ? arena_fmt(a, "There's no function or phrase called %s here, like memo.recall", fn)
          : message;
      return NULL;
    }
  } else {
    *why = arena_fmt(a,
        "There's nothing called %s here. It does start, stop, asks, answer, settings, set, manifest, policy, ceiling and call",
        verb);
    return NULL;
  }
  return out;
}

////////// 9. HOOKS //////////

/* 9. The account's policy as this computer keeps it, home/policy.account.json: device first, then the policy as it came.
   A version older than the one kept is refused. It returns an empty record, kept for callers that report what was
   clamped: there is nothing to clamp any more, since the ceiling is checked at each phrase. */
val *lingo_policy(arena *a, val *account, const char *device, const char **why) {
  val *core_kept = home_json(a, "policy.account.json");
  val *v = rec_get(account, "version");
  val *was = rec_get(core_kept, "version");
  val *out = v_rec(a);
  if (!v || v->k != V_NUM) {
    *why = "The account's policy has no version";
    return NULL;
  }
  if (was && was->k == V_NUM && v->n < was->n) {
    *why = arena_fmt(a, "The account's policy %g is older than the one kept here, %g", v->n, was->n);
    return NULL;
  }
  rec_set(a, out, "device", v_cstr(a, device));
  for (size_t i = 0; i < account->r.n; i++)
    if (strcmp(account->r.key[i], "device")) rec_set(a, out, account->r.key[i], account->r.v[i]);
  if (!keep_json("policy.account.json", out)) {
    *why = "This computer couldn't save policy.account.json";
    return NULL;
  }
  say("The account's policy %g is in force here", v->n);
  return v_rec(a);
}

// 9. The seed of this device's key called name, in home/keys, made and sealed first when missing and make is set.
static int seed_of(const char *name, int make, unsigned char seed[32]) {
  buf path = { 0 };
  buf got = { 0 };
  buf_fmt(&path, "%s/keys", set.home);
  int ok = named(name);
  if (ok) {
    if (make) pl_folder(path.s);
    buf_fmt(&path, "/%s", name);
    ok = pl_unseal(path.s, &got) && got.n == 32;
  }
  if (ok)
    memcpy(seed, got.s, 32);
  else if (named(name) && make) {
    pl_random(seed, 32);
    ok = pl_seal(path.s, seed, 32);
  }
  if (got.s) memset(got.s, 0, got.n);
  buf_free(&path);
  buf_free(&got);
  return ok;
}

// 9.
int key_of(const char *name, int make, unsigned char key[32]) {
  unsigned char seed[32];
  if (!seed_of(name, make, seed)) return 0;
  ed25519_key(seed, key);
  memset(seed, 0, sizeof seed);
  return 1;
}

// 9.
int key_sign(const char *name, const void *msg, size_t n, unsigned char sig[64]) {
  unsigned char seed[32];
  if (!seed_of(name, 0, seed)) return 0;
  ed25519_sign(seed, msg, n, sig);
  memset(seed, 0, sizeof seed);
  return 1;
}

// 9.
void key_forget(const char *name) {
  buf path = { 0 };
  buf_fmt(&path, "%s/keys/%s", set.home, name);
  if (named(name)) pl_delete(path.s);
  buf_free(&path);
}

// 9.
const char *lingo_home(void) { return set.root; }
int lingo_serves(void) { return live; }

// When anything in the lingos folder or running.json changed: everything read again, and each running native lingo told
// the new catalog. 1 when a rule in force allows public or an address, so the port opens to other computers. A rule for
// an account never opens it: that call arrives over Onsay's connection, signed by the person's device.
int core_look(void) {
  arena *a = arena_new();
  unsigned long long now;
  val *rules = rec_get(policy(a), "rules");
  int outside = 0;
  for (size_t i = 0; rules && rules->k == V_LIST && i < rules->l.n; i++) {
    val *rule = rules->l.v[i];
    const char *who = text_or(rule, "who", "");
    outside |= truthy(rec_get(rule, "allow")) && (!strcmp(who, "public") || rec_get(rule, "ip"));
  }
  look(a, &now);
  arena_free(a);
  if (now != looked) load(NULL);
  return outside;
}

////////// 10. REQUESTS //////////

// A web request reaches the running lingo at the longest route its path starts with: the route its manifest names, like
// "/onsay", else /<name>, when the caller may reach it. A lingo with serve(request) gets every request to its route, and
// any other is called as /<name>/<function>, or a native's /<name>/<group>/<function>, its values a JSON record by name
// or a list in order, or the query for a GET. GET / lists the lingos the caller may reach, and /.lingo/ is for apps on
// this computer (8. CONTROL).

// Headers as a record by lower case name, each value text.
static val *header_names(arena *a, val *h) {
  val *out = v_rec(a);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    val *v = h->r.v[i]->k == V_TEXT ? h->r.v[i] : NULL;
    buf b = { 0 };
    if (!v) buf_text(&b, h->r.v[i]);
    const char *text = b.s ? b.s : "";
    if (v) text = v->t.s;
    header_add(a, out, h->r.key[i], strlen(h->r.key[i]), text, v ? v->t.len : b.n);
    buf_free(&b);
  }
  return out;
}

// Part of a query with its %XX and + undone, kept as it came when that isn't text.
static val *unescape(arena *a, const char *s, size_t n) {
  buf b = { 0 };
  for (size_t i = 0; i < n; i++) {
    char x = i + 2 < n ? s[i + 1] : 0;
    char y = i + 2 < n ? s[i + 2] : 0;
    int hi = s[i] == '%' ? hex(x >= 'a' ? x - 32 : x) : -1;
    int lo = hex(y >= 'a' ? y - 32 : y);
    if (hi >= 0 && lo >= 0) {
      buf_chr(&b, (char)(hi * 16 + lo));
      i += 2;
    } else
      buf_chr(&b, s[i] == '+' ? ' ' : s[i]);
  }
  val *v = utf8_ok(b.s ? b.s : "", b.n) ? v_text(a, b.s ? b.s : "", b.n) : v_text(a, s, n);
  buf_free(&b);
  return v;
}

// A function's listing, if the lingo has one by that id, like memo.recall of memo or files.read of onsay.
static val *listed(const char *lingo, const char *id) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), lingo), "phrases");
  int base = !strcmp(lingo, "onsay");
  if (base && private_call(id)) return NULL;
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  if (!base) return NULL;
  own = rec_get(rec_get(rec_get(device, "lingos"), "core"), "phrases");
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  return NULL;
}

// Whether a request carries this computer's prompt key, the onsay lingo's setting, as Authorization: Bearer <key>.
static int prompt_keyed(arena *a, val *headers) {
  val *key = lingo_setting(a, "onsay", "prompt_key");
  const char *sent = text_or(headers, "authorization", "");
  return key && key->k == V_TEXT && key->t.len && !strncmp(sent, "Bearer ", 7) && !strcmp(sent + 7, key->t.s);
}

void core_serve(const char *text, size_t n, const char *given, buf *out) {
  arena *a = arena_new();
  const char *problem = NULL;
  const char *lingo = NULL;
  const char *why = NULL;
  val *m = json_parse(a, text, n, NULL);
  val *query = v_rec(a);
  val *head = v_rec(a);
  val *got = NOTHING;
  val *values = v_list(a);
  val *f = NULL;
  val *pol = policy(a);
  if (!m || m->k != V_REC) m = v_rec(a);
  val *headers = header_names(a, rec_get(m, "headers"));
  val *body = rec_get(m, "body");
  const char *p = text_or(m, "path", "");
  const char *method = text_or(m, "method", "GET");
  const char *ip = given ? NULL : text_or(m, "ip", "");
  const char *refused = NULL;
  caller c = { "this computer", ip, NULL, NULL, 0, 0, 0, 0 };
  int status = 200;
  int json = 1;   // json: the body is written as JSON
  size_t at = strcspn(p, "?");
  size_t best = 0;
  buf id = { 0 };
  buf error = { 0 };
  buf call = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  if (!body || body->k != V_TEXT) body = v_cstr(a, "");
  serving++;

  /* The caller. A request to the port from 127.0.0.1 or ::1 is this computer's when it names this computer as its host
     and no page elsewhere sent it, since a page in a browser here can send requests too. One handed in is me, the
     account's own, or by its address. Anyone else who signed it is its signer, and a signature that doesn't check out
     is a 401. */
  if (given && (!strcmp(given, "me") || !strcmp(given, "onsay"))) {
    c.me = !strcmp(given, "me");
    c.onsay = !strcmp(given, "onsay");
    c.name = given;
  } else if (given)
    c.ip = !strncmp(given, "ip ", 3) ? given + 3 : "";
  else if (!strcmp(ip, "::1") || !strncmp(ip, "127.", 4)) {
    const char *host = text_or(headers, "host", "");
    const char *origin = text_or(headers, "origin", NULL);
    const char *site = text_or(headers, "sec-fetch-site", "none");
    size_t end = host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":");
    c.here = (!end ||
                 (((end == 9 && (!strncmp(host, "127.0.0.1", 9) || !strncmp(host, "localhost", 9))) ||
                      (end == 5 && !strncmp(host, "[::1]", 5))) &&
                     (!host[end] || host[end] == ':'))) &&
        !(origin && (strncmp(origin, "http://", 7) || strcmp(origin + 7, host))) &&
        (!strcmp(site, "none") || !strcmp(site, "same-origin"));
  }
  if (!c.here && !c.me && !c.onsay) c.name = arena_fmt(a, "ip %s", c.ip);
  if (!c.here && !c.me && !c.onsay && rec_get(headers, "signature-input")) refused = signer(a, headers, method, p, body, &c);
  for (const char *q = p + at + (p[at] == '?'); *q;) {
    size_t len = strcspn(q, "&");
    size_t eq = strcspn(q, "=&");
    if (len) rec_set(a, query, unescape(a, q, eq)->t.s, unescape(a, q + eq + (eq < len), eq < len ? len - eq - 1 : 0));
    q += len + (q[len] == '&');
  }
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *route = route_of(a, e);
    size_t r = strlen(route);
    if (!rec_get(e, "error") && truthy(rec_get(e, "running")) && r > best && at >= r && !strncmp(p, route, r) &&
        (at == r || p[r] == '/')) {
      lingo = text_or(e, "name", "");
      best = r;
    }
  }

  if (p[0] != '/') {
    status = 400;
    buf_str(&error, "A path starts with /, like /memo/recall");
  } else if (truthy(rec_get(m, "base64") ? rec_get(m, "base64") : NOTHING) || !utf8_ok(body->t.s, body->t.len)) {
    status = 400;
    buf_str(&error, "Its body isn't text, and lingos take text, like JSON");
  } else if (refused) {
    status = 401;
    buf_str(&error, refused);
  } else if (!strncmp(p, "/.lingo/", 8) && c.here) {
    // Control, for an app on this computer only: never one handed in, since nothing from elsewhere may change what runs here.
    val *given = strcmp(method, "GET") && body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : query;
    if (problem || !given || given->k != V_REC) {
      status = 400;
      buf_str(&error, "Its values are a JSON record by name, like {\"lingo\": \"memo\"}");
    } else if (!(got = core_control(a, v_text(a, p + 8, at - 8)->t.s, given, &why))) {
      status = 400;
      buf_str(&error, why);
      got = NOTHING;
    }
  } else if (at == 1) {
    // Every lingo for this computer, running or not, and a broken one with why. Anyone else, each they may reach.
    val *list = v_list(a);
    for (size_t i = 0; i < found->l.n; i++) {
      val *e = found->l.v[i];
      val *one = v_rec(a);
      const char *name = text_or(e, "name", "");
      int runs = truthy(rec_get(e, "running"));
      if (!c.here && (rec_get(e, "error") || !runs || !let_in(a, pol, &c, route_of(a, e), NULL, &why))) continue;
      rec_set(a, one, "name", v_cstr(a, name));
      if (!rec_get(e, "error")) rec_set(a, one, "route", v_cstr(a, route_of(a, e)));
      rec_set(a, one, "running", VBOOL(runs));
      if (rec_get(e, "error")) rec_set(a, one, "error", rec_get(e, "error"));
      list_push(a, list, one);
    }
    rec_set(a, got = v_rec(a), "lingos", list);
  } else if (!lingo) {
    status = 404;
    buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
  } else {
    // /name alone is name.main, /name/function name.function, and a native's /name/group/function group.function.
    const char *rest = p + best;
    const char *name = rest + (at > best);
    const char *was_caller = job.caller;
    const char *was_lingo = job.lingo;
    const char *was_fn = job.fn;
    const char *was_path = job.path;
    const char *was_id = job.id;
    val *was_needs = job.needs;
    val *was_policy = job.policy;
    val *was_answers = job.answers;
    size_t left = at - best;
    size_t nl = left ? left - 1 : 0;
    int in = c.here || let_in(a, pol, &c, v_text(a, p, best)->t.s, v_text(a, rest, left)->t.s, &why);
    // /onsay is open, for its door checks its own key, but a base call is only for this account's own devices or that key.
    int base = 0;
    if (!strcmp(lingo, "onsay") && nl) {
      buf id = { 0 };
      if (memchr(name, '/', nl))
        buf_add(&id, name, nl);
      else
        buf_fmt(&id, "onsay.%.*s", (int)nl, name);
      for (size_t i = 0; i < id.n; i++)
        if (id.s[i] == '/') id.s[i] = '.';
      base = id.s && listed(lingo, id.s) && strcmp(id.s, "onsay.serve");
      buf_free(&id);
    }
    if (base && in && !c.here && !names("me", &c) && !names("my devices", &c) && !prompt_keyed(a, headers)) {
      in = 0;
      why = "a base call needs this account's own device or the prompt key";
    }
    job.caller = c.name;
    job.lingo = lingo;
    job.path = v_text(a, p, at)->t.s;
    job.policy = pol;
    job.answers = v_list(a);
    job.id = id.s;
    if (!c.here) core_decided(NULL, in, why);
    buf_fmt(&call, "%s.serve", lingo);
    if (!in) {
      status = 404;
      buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
    } else if (!base && listed(lingo, call.s)) {
      val *request = v_rec(a);
      val *parsed = body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : NOTHING;
      rec_set(a, request, "method", v_cstr(a, method));
      rec_set(a, request, "path", left ? v_text(a, rest, left) : v_cstr(a, "/"));
      rec_set(a, request, "query", query);
      rec_set(a, request, "headers", headers);
      rec_set(a, request, "body", body);
      rec_set(a, request, "json", problem ? NOTHING : parsed);
      rec_set(a, request, "caller", v_cstr(a, c.here ? "this computer" : c.name));
      list_push(a, values, request);
    } else {
      call.n = 0;
      if (!nl)
        buf_fmt(&call, "%s.main", lingo);
      else if (memchr(name, '/', nl))
        buf_add(&call, name, nl);
      else
        buf_fmt(&call, "%s.%.*s", lingo, (int)nl, name);
      for (size_t i = 0; i < call.n; i++)
        if (call.s[i] == '/') call.s[i] = '.';
      val *given = query;
      if (strcmp(method, "GET")) given = body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : v_rec(a);
      f = listed(lingo, call.s);
      val *params = f ? rec_get(f, "params") : NULL;
      if (!f) {
        status = 404;
        buf_fmt(&error, "%s has no function %s", lingo, call.s);
      } else if (problem || (given->k != V_REC && given->k != V_LIST)) {
        status = 400;
        buf_str(&error, "Its values are a JSON record by name, like {\"text\": \"hello\"}");
      } else if (given->k == V_LIST)
        values = given;
      else {
        // In the function's order, nothing for one left out, so its default applies.
        for (size_t i = 0; i < params->l.n; i++) {
          val *v = rec_get(given, text_or(params->l.v[i], "name", ""));
          list_push(a, values, v ? v : NOTHING);
        }
        for (size_t k = 0; k < given->r.n && !error.n; k++) {
          int known = 0;
          for (size_t i = 0; i < params->l.n && !known; i++)
            known = !strcmp(given->r.key[k], text_or(params->l.v[i], "name", ""));
          if (!known) {
            status = 400;
            buf_fmt(&error, "%s doesn't take a value called %s", call.s, given->r.key[k]);
          }
        }
      }
    }
    if (!error.n) {
      // The request is the running job: a stop with its id ends it, and each lingo(...) it asks says which function asks.
      const char *code = NULL;
      const char *message = NULL;
      double started = pl_now();
      job.fn = call.s;
      job.needs = said_needs(a, manifest_of(lingo), call.s);
      frame me = { top, "", 0 };
      snprintf(me.id, sizeof me.id, "%s", id.s);
      pl_enter(core_gate);
      top = &me;
      pl_leave(core_gate);
      got = core_call(a, call.s, values, &me.stop, &code, &message);
      pl_enter(core_gate);
      top = me.up;
      pl_leave(core_gate);
      say("%s answered in %.0f ms", call.s, (pl_now() - started) * 1000);
      if (!got) {
        status = !strcmp(code, "unknown") ? 404 : !strcmp(code, "denied") ? 403 : 500;
        buf_str(&error, message);
        got = NOTHING;
      }
      // What serve gives: a record with a number status is the response, and anything else the body of a 200.
      else if (!f) {
        val *s = got->k == V_REC ? rec_get(got, "status") : NULL;
        if (!s || s->k != V_NUM)
          json = got->k != V_TEXT;
        else if (s->n < 100 || s->n > 599 || s->n != (int)s->n) {
          status = 500;
          buf_str(&error, "serve gave a status that isn't one, like 200 or 404");
        } else {
          status = (int)s->n;
          head = header_names(a, rec_get(got, "headers"));
          got = rec_get(got, "body") ? rec_get(got, "body") : NOTHING;
          json = got->k != V_TEXT && got->k != V_NOTHING;
        }
      }
    }
    job.caller = was_caller;
    job.lingo = was_lingo;
    job.fn = was_fn;
    job.path = was_path;
    job.needs = was_needs;
    job.policy = was_policy;
    job.answers = was_answers;
    job.id = was_id;
  }

  if (!--serving)
    while (retiring) arena_free(retired[--retiring]);
  if (error.n) {
    rec_set(a, got = v_rec(a), "error", v_text(a, error.s, error.n));
    json = 1;
  }
  buf b = { 0 };
  if (json)
    buf_json(&b, got);
  else
    buf_text(&b, got);
  if (!rec_get(head, "content-type"))
    rec_set(a, head, "content-type", v_cstr(a, json ? "application/json" : "text/plain; charset=utf-8"));
  buf_fmt(out, "{\"id\":%s,\"status\":%d,\"headers\":", id.s, status);
  buf_json(out, head);
  buf_str(out, ",\"body\":");
  buf_json_str(out, b.s ? b.s : "", b.n);
  buf_str(out, ",\"base64\":false}");
  buf_free(&b);
  buf_free(&id);
  buf_free(&error);
  buf_free(&call);
  arena_free(a);
}

////////// 11. BEGINNING, AND PACKING //////////

int core_begin(int serves) {
  live = serves;
  core_gate = pl_gate_new();
  native_begin(0);
  std_phrases();
  floor_phrases();
  return load(NULL);
}

/* 11. A folder of a lingo's files, its program and the openapi.json its maker wrote, made into its .lingo file: a zip of
   its program and its whole openapi.json, its paths made from its code, or a native library's phrases. It is loaded
   with every lingo here first, so what it reaches in them counts. The file goes in to, or beside the folder. */
int core_pack(const char *folder, const char *to) {
  arena *a = arena_new();
  const char *base = folder + strlen(folder);
  buf zip = { 0 };
  buf files[2] = { { 0 }, { 0 } };
  buf path = { 0 };
  buf_str(&path, folder);
  while (path.n > 1 && (path.s[path.n - 1] == '/' || path.s[path.n - 1] == '\\')) path.s[--path.n] = 0;
  base = path.s + path.n;
  while (base > path.s && base[-1] != '/' && base[-1] != '\\') base--;
  val *e = load(path.s) ? entry_of(base) : NULL;
  int ok = e && rec_get(e, "fresh") && !rec_get(e, "error");
  if (!e || !rec_get(e, "fresh"))
    say("%s isn't a folder of a lingo's files: its program, like %s.ls, and its openapi.json", folder, base);
  else if (!ok)
    say("%s can't be packed: %s", base, text_or(e, "error", ""));
  if (ok) {
    const char *names[2] = { "openapi.json", text_or(e, "program", "") };
    buf_json(&files[0], rec_get(e, "manifest"));
    ok = pl_read(arena_fmt(a, "%s/%s", path.s, names[1]), &files[1]);
    zip_make(&zip, 2, names, files);
    if (to) pl_folder(to);
    const char *into = to ? arena_fmt(a, "%s/%s.lingo", to, base) : arena_fmt(a, "%s.lingo", path.s);
    ok = ok && pl_write(into, zip.s, zip.n);
    if (ok)
      say("Packed %s into %s", base, into);
    else
      say("%s couldn't be written", into);
  }
  buf_free(&zip);
  buf_free(&files[0]);
  buf_free(&files[1]);
  buf_free(&path);
  arena_free(a);
  return !ok;
}

//////////////////// 9. ONSAY ////////////////////

// The onsay lingo, built into the lingo client on every computer (build.ps1), which makes it Onsay's: its
// one connection to Onsay, signing in and this device's key, what Onsay signs checked against its key built in, the
// account's policy kept, decisions sent for the activity feed, and every request this computer makes sent in chunks.
// The link is offered to the distribution's private lingos too (lingo.h, Onsay), like the agent loop at /onsay
// (lingos/onsay/agent.c). Everything it sends Onsay is kept in home/sent.log for the person to read. See
// docs/security.md in the onsay repo, The onsay lingo and the wire.
//
// 1. Onsay          where it is, its public key, checking what it signed, and sent.log
// 2. Connection     hello, each message from Onsay, the heartbeat and reconnecting, on threads of its own
// 3. Hooks          lingo(...) and use(...), a signer's key, decisions, and starting and stopping
// 4. Policy         fetched, checked and handed to the core
// 5. Signing in     sign_in, sign_out and status, from an app through /.lingo/call or from onsay-core <settings> call,
//                   the account's devices, and where its trial of Onsay's AI stands
// 6. onsay_lingo()  adds its phrases and the base calls, as the lingo client starts

////////// 1. ONSAY //////////

// Where Onsay is, built in, never a setting: its API, its resolve endpoint and its public key, at the top of this file (ONSAY, RESOLVE
// and ONSAY_KEY). Every statement, request and policy Onsay signs is checked against that
// key, never against a key only told over the wire. Its live connection is its API's address with ws for http, like
// wss://api.onsay.dev/live.
#define BEAT 20   // seconds between pings
#define SILENT 60   // seconds without a word before the link is taken as dead
#define FRESH 300   // seconds a signature, or a key looked up, holds
#define LOG_MOST (4 << 20)   // bytes sent.log may grow to before it is cut to its newest half
#define CALLS_CHUNK 100   // calls waiting before they are sent; the rest go with the next heartbeat

#define say core_say

// value as Onsay signs it: JSON with every record's keys sorted and no spaces.
static void canonical(buf *b, val *x) {
  if (x->k == V_LIST) {
    buf_chr(b, '[');
    for (size_t i = 0; i < x->l.n; i++) {
      if (i) buf_chr(b, ',');
      canonical(b, x->l.v[i]);
    }
    buf_chr(b, ']');
  } else if (x->k == V_REC) {
    size_t order[256];
    size_t n = x->r.n < 256 ? x->r.n : 256;
    for (size_t i = 0; i < n; i++) {
      size_t j = i;
      for (; j > 0 && strcmp(x->r.key[order[j - 1]], x->r.key[i]) > 0; j--) order[j] = order[j - 1];
      order[j] = i;
    }
    buf_chr(b, '{');
    for (size_t i = 0; i < n; i++) {
      if (i) buf_chr(b, ',');
      buf_json_str(b, x->r.key[order[i]], strlen(x->r.key[order[i]]));
      buf_chr(b, ':');
      canonical(b, x->r.v[order[i]]);
    }
    buf_chr(b, '}');
  } else
    buf_json(b, x);
}

// A key, ed25519:<base64>, as its 32 bytes. 0 when it isn't one.
static int key_bytes(const char *text, unsigned char key[32]) {
  buf raw = { 0 };
  int ok = text && !strncmp(text, "ed25519:", 8) && unbase64(text + 8, strlen(text + 8), &raw) && raw.n == 32;
  if (ok) memcpy(key, raw.s, 32);
  buf_free(&raw);
  return ok;
}

// Whether signed, <anything>:<base64>, is a signature over value as Onsay signs it, by key, Onsay's when NULL.
static int signed_by(val *value, val *signed_, const char *key) {
  unsigned char k[32];
  const char *s = signed_ && signed_->k == V_TEXT ? signed_->t.s : "";
  const char *colon = strchr(s, ':');
  buf text = { 0 };
  buf raw = { 0 };
  canonical(&text, value);
  int ok = colon && key_bytes(key ? key : ONSAY_KEY, k) && unbase64(colon + 1, strlen(colon + 1), &raw) &&
      raw.n == 64 && ed25519_check(k, text.s, text.n, (unsigned char *)raw.s);
  buf_free(&text);
  buf_free(&raw);
  return ok;
}

// Seconds since 1970 of an ISO time, like 2026-10-28T14:00:00.000Z, or 0.
static double seconds_of(const char *iso) {
  int y, mo, d, h, mi;
  double s;
  if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  y -= mo <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return (double)(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468) * 86400 + h * 3600 + mi * 60 + s;
}

/* Each message sent to Onsay, a line of home/sent.log, {at, message}, every token in it masked, so the person can read
   what went. Pings are left out. Cut to its newest half when it passes LOG_MOST. */
static pl_gate *logging;
static void sent(const char *m, size_t n) {
  static long size = -1;
  buf line = { 0 };
  buf path = { 0 };
  buf all = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  buf_fmt(&line, "{\"at\":\"%s\",\"message\":", at);
  for (size_t i = 0; i < n; i++)
    if (n - i > 9 && !memcmp(m + i, "\"token\":\"", 9)) {
      buf_str(&line, "\"token\":\"(hidden)\"");
      for (i += 9; i < n && m[i] != '"'; i++)
        if (m[i] == '\\') i++;
    } else
      buf_chr(&line, m[i]);
  buf_str(&line, "}\n");
  buf_fmt(&path, "%s/sent.log", lingo_home());
  pl_enter(logging);
  if (size < 0 || size > LOG_MOST) {
    size = pl_read(path.s, &all) ? (long)all.n : 0;
    const char *half = size > LOG_MOST ? memchr(all.s + size / 2, '\n', size - size / 2) : NULL;
    if (half && pl_write(path.s, half + 1, all.s + size - half - 1)) size = all.s + size - half - 1;
  }
  if (pl_append(path.s, line.s, line.n)) size += (long)line.n;
  pl_leave(logging);
  buf_free(&line);
  buf_free(&path);
  buf_free(&all);
}

static int tell(pl_link *l, const char *m, size_t n) {
  int ok = pl_send(l, m, n);
  if (ok) sent(m, n);
  return ok;
}

// One request to Onsay's API: its answer as a record, and its status, or NULL with why in error when it wasn't reached.
val *onsay_api(arena *a, const char *method, const char *path, const char *body, double *status, buf *error) {
  buf url = { 0 };
  buf_fmt(&url, "{\"type\":\"api\",\"method\":\"%s\",\"path\":", method);
  buf_json_str(&url, path, strlen(path));
  buf_fmt(&url, ",\"body\":%s}", body ? body : "null");
  sent(url.s, url.n);
  url.n = 0;
  val *headers = v_rec(a);
  val *token = lingo_setting(a, "onsay", "token");
  val *said = NULL;
  rec_set(a, headers, "content-type", v_cstr(a, "application/json"));
  if (token && token->k == V_TEXT && token->t.len) {
    buf bearer = { 0 };
    buf_fmt(&bearer, "Bearer %s", token->t.s);
    rec_set(a, headers, "authorization", v_text(a, bearer.s, bearer.n));
    buf_free(&bearer);
  }
  buf_fmt(&url, "%s%s", ONSAY, path);
  val *got = pl_http(a, method, url.s, headers, body ? body : "", body ? strlen(body) : 0, 30, error);
  val *text = rec_get(got, "text");
  buf_free(&url);
  if (got) {
    *status = rec_get(got, "status")->n;
    said = json_parse(a, text->t.s, text->t.len, NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  }
  return said;
}

////////// 2. CONNECTION //////////

// One message sent and its answer waited for, like a lingo(...) or a decision, and who hears what Onsay says of it
// meanwhile. hearing: how many of those are being heard now, outside the lock.
typedef struct wait {
  struct wait *next;
  long id;
  volatile int done;
  buf reply;
  onsay_heard heard;
  void *arg;
  int hearing;
} wait;

static pl_gate *lock;   // keeps everything below, which the connection, the heartbeat and the job thread share
static pl_link *link_;   // the open link, after its hello, or NULL
static int onsay_on, ready, ponged;   // on: the lingo runs. ready: Onsay took the hello, and its statement checked out
static double heard;   // when Onsay last said anything
static char handle[64], onsay_device[64], account[64];   // this computer's handle, its name and its account id, as ready said
static unsigned char onsay_mine[32];   // this device's public key
static buf onsay_catalog, pending, calls_;   // the catalog now, and decisions and calls not sent yet, a line each
static wait *waits;
static onsay_listener listening;   // the private lingo that hears what the link doesn't know (onsay_listen)
typedef struct streamed {
  struct streamed *next;
  char id[80], path[256];
} streamed;
static streamed *streams;   // streams Onsay asked of this computer for another of the account, each frame passed on
static long ids;

int onsay_send(const char *text, size_t n) {
  pl_enter(lock);
  int ok = link_ && tell(link_, text, n);
  pl_leave(lock);
  return ok;
}

/* Lines not sent yet, from q, as messages {type, field: [...]} of up to 1000 each, once Onsay is ready: decisions as
   log entries, calls as calls. Inside the lock. */
static void flush(buf *q, const char *type, const char *field) {
  while (ready && link_ && q->n) {
    buf m = { 0 };
    size_t at = 0;
    buf_fmt(&m, "{\"type\":\"%s\",\"%s\":[", type, field);
    for (int k = 0; at < q->n && k < 1000; k++) {
      size_t end = strcspn(q->s + at, "\n");
      if (k) buf_chr(&m, ',');
      buf_add(&m, q->s + at, end);
      at += end + 1;
    }
    buf_str(&m, "]}");
    int ok = tell(link_, m.s, m.n);
    buf_free(&m);
    if (!ok) return;
    memmove(q->s, q->s + at, q->n - at);
    q->n -= at;
  }
}

// A response sent back down the link, from the core's answer {id, status, headers, body, base64}.
static void onsay_answered(void *arg, const char *response, size_t n) {
  buf m = { 0 };
  buf_str(&m, "{\"type\":\"response\",");
  buf_add(&m, response + 1, n - 1);
  onsay_send(m.s, m.n);
  buf_free(&m);
}

// One message of a stream this computer serves another, passed on down the link as a frame of its request.
static void relay(void *arg, const char *text, size_t n) {
  streamed *s = arg;
  buf m = { 0 };
  buf_fmt(&m, "{\"type\":\"frame\",\"id\":%s,\"text\":", s->id);
  buf_json_str(&m, text, n);
  buf_chr(&m, '}');
  onsay_send(m.s, m.n);
  buf_free(&m);
}

/* A stream Onsay asked for answered like any request, its answer the first message, and on a 200 kept open, each
   message after a frame (relay), until Onsay says stop or the link drops. */
static void opened(void *arg, const char *response, size_t n) {
  streamed *s = arg;
  arena *a = arena_new();
  val *status = rec_get(json_parse(a, response, n, NULL), "status");
  onsay_answered(NULL, response, n);
  if (status && status->k == V_NUM && status->n == 200) {
    pl_enter(lock);
    s->next = streams;
    streams = s;
    pl_leave(lock);
    lingo_watch(s->path, relay, s);
  } else
    free(s);
  arena_free(a);
}

// The streams this computer serves others that match id, as JSON, or every one when id is NULL, ended.
static void unstreamed(const char *id) {
  streamed *gone = NULL;
  pl_enter(lock);
  for (streamed **at = &streams; *at;)
    if (!id || !strcmp((*at)->id, id)) {
      streamed *s = *at;
      *at = s->next;
      s->next = gone;
      gone = s;
    } else
      at = &(*at)->next;
  pl_leave(lock);
  while (gone) {
    streamed *s = gone;
    gone = s->next;
    lingo_unwatch(s);
    free(s);
  }
}

static void policy_pull(void);

/* A request from Onsay. as: "me" is the account's own call, and as: "onsay" Onsay's own search or agent, both signed over
   {as, at, device, id, method, path, sha256}, sha256 its body's: handed in as that caller when it checks out, made within
   5 minutes for this device, else a 401. Any other is one the proxy passed on, its caller the first address of its
   x-forwarded-for, or its signer, which the core checks. */
static void request(arena *a, val *m, const char *text, size_t n) {
  val *body = rec_get(m, "body");
  val *b64 = rec_get(m, "base64");
  val *signed_ = v_rec(a);
  const char *as = text_or(m, "as", NULL);
  const char *forwarded = text_or(rec_get(m, "headers"), "x-forwarded-for", "");
  if (!as) {
    buf caller = { 0 };
    buf_fmt(&caller, "ip %.*s", (int)strcspn(forwarded, ", "), forwarded);
    lingo_hand(text, n, caller.s, onsay_answered, NULL);
    buf_free(&caller);
    return;
  }
  buf bytes = { 0 };
  buf sum64 = { 0 };
  buf out = { 0 };
  unsigned char sum[32];
  if (b64 && truthy(b64))
    unbase64(body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0, &bytes);
  else
    buf_add(&bytes, body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0);
  sha256(bytes.s, bytes.n, sum);
  base64(&sum64, sum, 32);
  rec_set(a, signed_, "as", v_cstr(a, as));
  rec_set(a, signed_, "at", rec_get(m, "at") ? rec_get(m, "at") : NOTHING);
  pl_enter(lock);
  rec_set(a, signed_, "device", v_cstr(a, onsay_device));
  pl_leave(lock);
  rec_set(a, signed_, "id", rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  rec_set(a, signed_, "method", rec_get(m, "method") ? rec_get(m, "method") : NOTHING);
  rec_set(a, signed_, "path", rec_get(m, "path") ? rec_get(m, "path") : NOTHING);
  rec_set(a, signed_, "sha256", v_text(a, sum64.s, sum64.n));
  double at = seconds_of(text_or(m, "at", NULL));
  double now = pl_now();
  int stream = rec_get(m, "stream") && truthy(rec_get(m, "stream"));
  if ((!strcmp(as, "me") || !strcmp(as, "onsay")) && at > now - FRESH && at < now + FRESH &&
      signed_by(signed_, rec_get(m, "signed"), NULL) && stream) {
    streamed *s = calloc(1, sizeof *s);
    buf j = { 0 };
    const char *p = text_or(m, "path", "/");
    buf_json(&j, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    snprintf(s->id, sizeof s->id, "%s", j.s);
    snprintf(s->path, sizeof s->path, "%.*s", (int)strcspn(p, "?"), p);
    buf_free(&j);
    lingo_hand(text, n, as, opened, s);
  } else if ((!strcmp(as, "me") || !strcmp(as, "onsay")) && at > now - FRESH && at < now + FRESH &&
      signed_by(signed_, rec_get(m, "signed"), NULL))
    lingo_hand(text, n, as, onsay_answered, NULL);
  else {
    buf_str(&out, "{\"type\":\"response\",\"id\":");
    buf_json(&out, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    buf_str(&out,
        ",\"status\":401,\"headers\":{\"content-type\":\"application/json\"},\"body\":\"{\\\"error\\\":\\\"Onsay's signature on this "
        "request doesn't check out, so it didn't run\\\"}\",\"base64\":false}");
    onsay_send(out.s, out.n);
    say("A request said to be from Onsay didn't check out, so it didn't run");
  }
  buf_free(&bytes);
  buf_free(&sum64);
  buf_free(&out);
}

// Each message from Onsay, on the connection's thread, so a stop or an answer lands while a request runs.
static void heard_(pl_link *l, const char *text, size_t n) {
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL);
  val *id = rec_get(m, "id");
  const char *type = text_or(m, "type", "");
  if (!strcmp(type, "ready")) {
    val *statement = v_rec(a);
    buf key = { 0 };
    buf_str(&key, "ed25519:");
    base64(&key, onsay_mine, 32);
    rec_set(a, statement, "added", rec_get(m, "added") ? rec_get(m, "added") : NOTHING);
    rec_set(a, statement, "device", rec_get(m, "device") ? rec_get(m, "device") : NOTHING);
    rec_set(a, statement, "handle", rec_get(m, "handle") ? rec_get(m, "handle") : NOTHING);
    rec_set(a, statement, "account", rec_get(m, "account") ? rec_get(m, "account") : NOTHING);
    rec_set(a, statement, "key", v_text(a, key.s, key.n));
    buf_free(&key);
    if (!signed_by(statement, rec_get(m, "statement"), NULL)) {
      say("Onsay's statement on this computer's key doesn't check out, so it isn't trusted. Trying again");
      pl_close(l);
    } else {
      pl_enter(lock);
      snprintf(handle, sizeof handle, "%s", text_or(m, "handle", ""));
      snprintf(onsay_device, sizeof onsay_device, "%s", text_or(m, "device", ""));
      snprintf(account, sizeof account, "%s", text_or(m, "account", ""));
      ready = 1;
      flush(&pending, "log", "entries");
      flush(&calls_, "calls", "calls");
      pl_leave(lock);
      say("Signed in. This computer is %s.%s", onsay_device, handle);
      // Always, the default too: the rules in force here are only ever the account's, as Onsay signed them.
      policy_pull();
    }
  } else if (!strcmp(type, "request") && ready)
    request(a, m, text, n);
  else if (!strcmp(type, "stop")) {
    buf j = { 0 };
    buf_json(&j, id ? id : NOTHING);
    unstreamed(j.s);
    lingo_stop(j.s);
    buf_free(&j);
  } else if ((!strcmp(type, "lingo") || !strcmp(type, "decision")) && id && id->k == V_NUM) {
    pl_enter(lock);
    for (wait *w = waits; w; w = w->next)
      if ((double)w->id == id->n) {
        buf_add(&w->reply, text, n);
        lingo_done(&w->done);
      }
    pl_leave(lock);
  } else if (!strcmp(type, "step") && id && id->k == V_NUM) {
    pl_enter(lock);
    wait *w = waits;
    while (w && !((double)w->id == id->n && w->heard)) w = w->next;
    if (w) w->hearing++;
    pl_leave(lock);
    if (w) {
      w->heard(w->arg, text, n);
      pl_enter(lock);
      w->hearing--;
      pl_leave(lock);
    }
  } else if (!strcmp(type, "policy"))
    policy_pull();
  else if (!strcmp(type, "pong")) {
    pl_enter(lock);
    ponged = 1;
    pl_leave(lock);
  } else if (!listening.heard || !listening.heard(text, n))
    say("Onsay sent a message this lingo client doesn't know: %s", *type ? type : "not JSON");
  arena_free(a);
}

// Connects to Onsay while the lingo runs and has a token, and again after a drop, waiting longer each time it fails.
static void connection(void *arg) {
  for (double wait_ = 1;; pl_sleep(wait_), wait_ = wait_ * 2 > 30 ? 30 : wait_ * 2) {
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    buf error = { 0 };
    buf hello = { 0 };
    if (!onsay_on || !token || token->k != V_TEXT || !token->t.len) {
      arena_free(a);
      wait_ = 1;
      continue;
    }
    if (!key_of("device", 1, onsay_mine)) {
      say("This computer couldn't make or keep its key, so it can't sign in");
      arena_free(a);
      continue;
    }
    buf live = { 0 };
    buf_fmt(&live, "ws%s/live", &ONSAY[strlen("http")]);   // https://x is wss://x/live, http://x ws://x/live
    pl_link *l = pl_connect(live.s, NULL, &error);
    buf_free(&live);
    if (!l) say("%s. Trying again in %.0f seconds", error.s ? error.s : "It couldn't connect to Onsay", wait_);
    buf_free(&error);
    if (l) {
      buf_str(&hello, "{\"type\":\"hello\",\"token\":");
      buf_json_str(&hello, token->t.s, token->t.len);
      buf_str(&hello, ",\"client\":\"lingo-client\",\"version\":\"" CORE_VERSION "\",\"key\":\"ed25519:");
      base64(&hello, onsay_mine, 32);
      buf_str(&hello, "\",\"catalog\":");
      pl_enter(lock);
      buf_add(&hello, onsay_catalog.s ? onsay_catalog.s : "[]", onsay_catalog.s ? onsay_catalog.n : 2);
      buf_chr(&hello, '}');
      link_ = l;
      heard = pl_now();
      ponged = ready = 0;
      tell(l, hello.s, hello.n);
      pl_leave(lock);
      buf in = { 0 };
      while (pl_receive(l, &in)) {
        pl_enter(lock);
        heard = pl_now();
        pl_leave(lock);
        heard_(l, in.s, in.n);
        in.n = 0;
      }
      buf_free(&in);
      pl_enter(lock);
      if (ready) wait_ = 0.5;   // so the next drop starts its waiting over
      link_ = NULL;
      ready = 0;
      for (wait *w = waits; w; w = w->next) lingo_done(&w->done);
      pl_leave(lock);
      pl_free(l);
      unstreamed(NULL);
      if (listening.heard) listening.heard(NULL, 0);
      say("The connection to Onsay closed");
    }
    buf_free(&hello);
    arena_free(a);
  }
}

// Pings every BEAT seconds, drops a link silent for SILENT, and one whose token is gone, signed out elsewhere.
static void heartbeat(void *arg) {
  for (;;) {
    pl_sleep(BEAT);
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    pl_enter(lock);
    if (link_ && ((ponged && pl_now() - heard > SILENT) || !token || token->k != V_TEXT || !token->t.len))
      pl_close(link_);
    else if (link_) {
      pl_send(link_, "{\"type\":\"ping\"}", 15);
      flush(&calls_, "calls", "calls");
    }
    pl_leave(lock);
    arena_free(a);
  }
}

////////// 3. HOOKS //////////

/* A message to Onsay, {"type": type, "id": a new number, ...rest}, rest a JSON record, and its answer by that id waited
   for, answering requests meanwhile. 1 with the whole answer in reply, 0 when Onsay isn't ready, the link closed, stop
   was set or seconds passed. heard, when set, gets each step Onsay says of it meanwhile, with arg, and one stopped here
   is stopped at Onsay too. */
int onsay_exchange(const char *type, const char *rest, size_t n, volatile int *stop, double seconds, buf *reply,
    onsay_heard heard, void *arg) {
  wait w = { .heard = heard, .arg = arg };
  buf m = { 0 };
  pl_enter(lock);
  int going = ready && link_;
  if (going) {
    w.id = ++ids;
    w.next = waits;
    waits = &w;
    buf_fmt(&m, "{\"type\":\"%s\",\"id\":%ld,", type, w.id);
    buf_add(&m, rest + 1, n - 1);
    going = tell(link_, m.s, m.n);
  }
  pl_leave(lock);
  if (going) lingo_wait(&w.done, stop, seconds);
  pl_enter(lock);
  for (wait **at = &waits; *at; at = &(*at)->next)
    if (*at == &w) {
      *at = w.next;
      break;
    }
  while (w.hearing) {
    pl_leave(lock);
    pl_sleep(0.01);
    pl_enter(lock);
  }
  pl_leave(lock);
  int got = w.reply.n > 0;
  if (got)
    buf_add(reply, w.reply.s, w.reply.n);
  else if (heard && *stop) {
    m.n = 0;
    buf_fmt(&m, "{\"type\":\"stop\",\"id\":%ld}", w.id);
    onsay_send(m.s, m.n);
  }
  buf_free(&m);
  buf_free(&w.reply);
  return got;
}

// A lingo(...) or use(...) a program reaches, heard by whoever follows the request it runs in (onsay_listen).
static int ask(const char *values, size_t n, volatile int *stop, buf *answer) {
  void *arg = NULL;
  onsay_heard heard = listening.follows ? listening.follows(lingo_job(), &arg) : NULL;
  return onsay_exchange("lingo", values, n, stop, 1e12, answer, heard, arg);
}

/* A line a request's run says for the person, sent to Onsay as a step of that request when Onsay sent it, so whoever
   asked follows along from any computer, and Onsay knows it is still working. This computer's own requests, from its
   port or its prompts, "port <n>" and "onsay <n>", are shown here instead. */
static int said(const char *for_, const char *text) {
  if (!strncmp(for_, "\"port ", 6) || !strncmp(for_, "\"onsay ", 7)) return 0;
  buf m = { 0 };
  buf_fmt(&m, "{\"type\":\"step\",\"id\":%s,\"text\":", for_);
  buf_json_str(&m, text, strlen(text));
  buf_chr(&m, '}');
  int ok = onsay_send(m.s, m.n);
  buf_free(&m);
  return ok;
}

/* A signer's key, by its keyid, a device and its handle like laptop.brandon: from Onsay's /resolve, its statement checked,
   and kept for 5 minutes. mine when it is a device of this computer's own account. */
static struct {
  char keyid[128], key[64], handle[64], account[64];
  double until;
} known[32];

static val *key(arena *a, const char *keyid) {
  const char *dot = strchr(keyid, '.');
  val *out = NULL;
  int k = 0;
  if (!dot || strchr(dot + 1, '.') || strlen(keyid) >= sizeof known[0].keyid) return NULL;
  pl_enter(lock);
  while (k < 32 && !(!strcmp(known[k].keyid, keyid) && known[k].until > pl_now())) k++;
  pl_leave(lock);
  if (k == 32) {
    buf path = { 0 };
    buf error = { 0 };
    double status = 0;
    buf_fmt(&path, RESOLVE "%s", keyid);
    val *said = onsay_api(a, "GET", path.s, NULL, &status, &error);
    val *devices = rec_get(said, "devices");
    for (size_t i = 0; status == 200 && devices && devices->k == V_LIST && i < devices->l.n && k == 32; i++) {
      val *d = devices->l.v[i];
      val *statement = v_rec(a);
      if (strncmp(text_or(d, "name", ""), keyid, (size_t)(dot - keyid)) || text_or(d, "name", "")[dot - keyid])
        continue;
      rec_set(a, statement, "added", rec_get(d, "added") ? rec_get(d, "added") : NOTHING);
      rec_set(a, statement, "device", v_text(a, keyid, (size_t)(dot - keyid)));
      rec_set(a, statement, "handle", v_cstr(a, dot + 1));
      rec_set(a, statement, "account", rec_get(d, "account") ? rec_get(d, "account") : NOTHING);
      rec_set(a, statement, "key", rec_get(d, "key") ? rec_get(d, "key") : NOTHING);
      if (strlen(text_or(d, "key", "")) >= sizeof known[0].key || !signed_by(statement, rec_get(d, "statement"), NULL))
        continue;
      pl_enter(lock);
      for (k = 0; k < 31 && known[k].until > pl_now(); k++) {}
      snprintf(known[k].keyid, sizeof known[k].keyid, "%s", keyid);
      snprintf(known[k].key, sizeof known[k].key, "%s", text_or(d, "key", ""));
      snprintf(known[k].handle, sizeof known[k].handle, "%s", dot + 1);
      snprintf(known[k].account, sizeof known[k].account, "%s", text_or(d, "account", ""));
      known[k].until = pl_now() + FRESH;
      pl_leave(lock);
    }
    buf_free(&path);
    buf_free(&error);
  }
  if (k < 32) {
    out = v_rec(a);
    pl_enter(lock);
    rec_set(a, out, "key", v_cstr(a, known[k].key));
    rec_set(a, out, "handle", v_cstr(a, known[k].handle));
    rec_set(a, out, "account", v_cstr(a, known[k].account));
    rec_set(a, out, "mine", VBOOL(*account && !strcmp(known[k].account, account)));
    pl_leave(lock);
  }
  return out;
}

static void onsay_decided(const char *line) {
  pl_enter(lock);
  buf_str(&pending, line);
  buf_chr(&pending, '\n');
  flush(&pending, "log", "entries");
  pl_leave(lock);
}

/* A request this computer made, kept to send Onsay: sent once CHUNK wait, else with the next heartbeat. Offline, the
   newest half is kept once they pass LOG_MOST bytes. */
static void onsay_kept(const char *line) {
  size_t lines = 0;
  pl_enter(lock);
  buf_str(&calls_, line);
  buf_chr(&calls_, '\n');
  if (calls_.n > LOG_MOST) {
    size_t cut = calls_.n / 2;
    cut += strcspn(calls_.s + cut, "\n") + 1;
    if (cut > calls_.n) cut = calls_.n;
    memmove(calls_.s, calls_.s + cut, calls_.n - cut);
    calls_.n -= cut;
  }
  for (size_t i = 0; i < calls_.n; i++) lines += calls_.s[i] == '\n';
  if (lines >= CALLS_CHUNK) flush(&calls_, "calls", "calls");
  pl_leave(lock);
}

// Started, the first time: the connection's threads, and the private lingo listening told. Each time, the catalog sent
// on, as the core gives it. Stopped, the link closes.
static void onsay_running(const char *now) {
  static int started;
  pl_enter(lock);
  onsay_on = now != NULL;
  onsay_catalog.n = 0;
  if (now) buf_str(&onsay_catalog, now);
  if (link_ && !onsay_on)
    pl_close(link_);
  else if (link_) {
    buf m = { 0 };
    buf_str(&m, "{\"type\":\"catalog\",\"catalog\":");
    buf_str(&m, now);
    buf_chr(&m, '}');
    tell(link_, m.s, m.n);
    buf_free(&m);
  }
  pl_leave(lock);
  if (!onsay_on || started) return;
  started = 1;
  if (listening.started) listening.started();
  pl_thread(connection, NULL);
  pl_thread(heartbeat, NULL);
}

long onsay_id(void) {
  pl_enter(lock);
  long id = ++ids;
  pl_leave(lock);
  return id;
}

int onsay_ready(void) {
  pl_enter(lock);
  int is = ready && link_;
  pl_leave(lock);
  return is;
}

void onsay_name(buf *out) {
  pl_enter(lock);
  if (*onsay_device && *handle) buf_fmt(out, "%s.%s", onsay_device, handle);
  pl_leave(lock);
}

void onsay_listen(const onsay_listener *l) { listening = *l; }

////////// 4. POLICY //////////

/* The account's policy, from GET /policy: {version, policy, signed}, signed over {policy, version}. Checked, then handed to
   the core, which keeps it unless its version is older than the one kept, and what it applied sent back: the version and
   clamped, which is empty since the ceiling is checked at each phrase. Anything that doesn't check out is refused, and
   said. */
static void policy_pull(void) {
  arena *a = arena_new();
  buf error = { 0 };
  buf m = { 0 };
  double status = 0;
  val *said = onsay_api(a, "GET", "/policy", NULL, &status, &error);
  val *both = v_rec(a);
  val *version = rec_get(said, "version");
  val *policy = rec_get(said, "policy");
  const char *why = NULL;
  rec_set(a, both, "policy", policy ? policy : NOTHING);
  rec_set(a, both, "version", version ? version : NOTHING);
  pl_enter(lock);
  const char *name = v_cstr(a, onsay_device)->t.s;
  pl_leave(lock);
  if (!said || status != 200)
    say("The account's policy couldn't be fetched: %s", error.n ? error.s : text_or(said, "error", "Onsay said no"));
  else if (!policy || policy->k != V_REC || !version || version->k != V_NUM ||
      !signed_by(both, rec_get(said, "signed"), NULL))
    say("The account's policy from Onsay doesn't check out, so it is refused");
  else {
    rec_set(a, policy, "version", version);
    val *clamped = lingo_policy(a, policy, name, &why);
    if (!clamped)
      say("%s, so it is refused", why);
    else {
      buf_fmt(&m, "{\"type\":\"applied\",\"version\":");
      buf_json(&m, version);
      buf_str(&m, ",\"clamped\":");
      buf_json(&m, clamped);
      buf_chr(&m, '}');
      onsay_send(m.s, m.n);
    }
  }
  buf_free(&error);
  buf_free(&m);
  arena_free(a);
}

/* Sets a rule's identity: who is public, me, my devices or onsay; acct_... is the account; anything else is a handle,
   handle#tag, or a name Onsay resolves, and Onsay keeps the account it names. */
static void identity(arena *a, val *rule, const char *who) {
  if (!strcmp(who, "public") || !strcmp(who, "me") || !strcmp(who, "my devices") || !strcmp(who, "onsay"))
    rec_set(a, rule, "who", v_cstr(a, who));
  else if (!strncmp(who, "acct_", 5))
    rec_set(a, rule, "account", v_cstr(a, who));
  else
    rec_set(a, rule, "handle", v_cstr(a, who));
}

// Whether a rule names who, by its built-in word, its account id, or its handle.
static int names_who(val *rule, const char *who) {
  return !strcmp(text_or(rule, "who", ""), who) || !strcmp(text_or(rule, "account", ""), who) ||
      !strcmp(text_or(rule, "handle", ""), who);
}

/* The account's policy from Onsay, checked and kept to what share and unshare change. Its rules are the ones to save. */
static val *policy_now(run *r, const char *what, buf *error) {
  double status = 0;
  val *said = onsay_api(r->a, "GET", "/policy", NULL, &status, error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to %s", what);
  if (status != 200 || !rec_get(said, "policy") || rec_get(said, "policy")->k != V_REC)
    fail(r, "phrase", "Onsay couldn't read the policy: %s", text_or(said, "error", "it said no"));
  return rec_get(said, "policy");
}

// Saves the policy at Onsay and keeps it here.
static val *policy_save(run *r, val *policy) {
  buf body = { 0 };
  buf error = { 0 };
  double status = 0;
  buf_str(&body, "{\"policy\":");
  buf_json(&body, policy);
  buf_chr(&body, '}');
  val *saved = onsay_api(r->a, "PUT", "/policy", body.s, &status, &error);
  buf_free(&body);
  buf_free(&error);
  if (!saved) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 200) fail(r, "phrase", "Onsay didn't keep the policy: %s", text_or(saved, "error", "it said no"));
  policy_pull();
  return saved;
}

/* share(values: record) -> the policy: shares with who, at routes, full paths like /photos/share, or every route when
   none are named, on devices when they are. who is public, me, my devices, onsay, a handle like james, handle#tag, or
   acct_.... One more allow rule is added to the account's policy, and every computer is told. */
static val *share(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *policy;
  val *rule;
  val *rules;
  val *old;
  const char *who = values->k == V_REC ? text_or(values, "who", "") : "";
  val *routes = values->k == V_REC ? rec_get(values, "routes") : NULL;
  val *devices = values->k == V_REC ? rec_get(values, "devices") : NULL;
  if (!*who)
    fail(r, "phrase", "Say who to share with: a handle like james, handle#tag, acct_..., public, me, my devices or onsay");
  policy = policy_now(r, "share", &error);
  buf_free(&error);
  rule = v_rec(a);
  rec_set(a, rule, "allow", VBOOL(1));
  identity(a, rule, who);
  if (routes && routes->k == V_LIST) {
    for (size_t i = 0; i < routes->l.n; i++)
      if (routes->l.v[i]->k != V_TEXT || routes->l.v[i]->t.s[0] != '/')
        fail(r, "phrase", "A route is a full path, like /photos/share, each starting with /");
    rec_set(a, rule, "routes", routes);
  }
  if (devices && devices->k == V_LIST) rec_set(a, rule, "devices", devices);
  rules = v_list(a);
  old = rec_get(policy, "rules");
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++) list_push(a, rules, old->l.v[i]);
  list_push(a, rules, rule);
  rec_set(a, policy, "default", v_cstr(a, "closed"));
  rec_set(a, policy, "rules", rules);
  return policy_save(r, policy);
}

/* unshare(values: record) -> the policy: stops sharing with who, taking out every rule that names them, whether by
   public, me, my devices, onsay, an account id, or a handle. */
static val *onsay_unshare(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *policy;
  val *onsay_kept;
  val *old;
  const char *who = values->k == V_REC ? text_or(values, "who", "") : "";
  if (!*who) fail(r, "phrase", "Say who to stop sharing with: a handle like james, acct_..., public, me, my devices or onsay");
  policy = policy_now(r, "share", &error);
  buf_free(&error);
  onsay_kept = v_list(a);
  old = rec_get(policy, "rules");
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++)
    if (!names_who(old->l.v[i], who)) list_push(a, onsay_kept, old->l.v[i]);
  if (!old || old->k != V_LIST || onsay_kept->l.n == old->l.n)
    fail(r, "phrase", "Nothing is shared with %s", who);
  rec_set(a, policy, "default", v_cstr(a, "closed"));
  rec_set(a, policy, "rules", onsay_kept);
  return policy_save(r, policy);
}

////////// 7. SIGNING IN //////////

static int attempt,
    signing;   // the sign in now, by its number, so a newer one or signing out ends it; signing: it waits
static char onsay_failed[256];   // why the last sign in in the background didn't finish, for status

/* Asks Onsay every two seconds whether the person pressed Connect, for as long as the code lasts, 10 minutes, then keeps
   the token as this lingo's secret setting and makes this device's key. The account it signed in as, or NULL with why.
   stop is the command line's. In the background it is NULL, and a newer sign in, or signing out, ends it instead. */
static val *connect_(arena *a, const char *body, volatile int *stop, int onsay_mine, buf *why) {
  volatile int never = 0;
  unsigned char k[32];
  double status = 0;
  for (double until = pl_now() + 600; pl_now() < until;) {
    if (stop)
      lingo_wait(&never, stop, 2);
    else
      pl_sleep(2);
    pl_enter(lock);
    int ended = stop ? *stop : onsay_mine != attempt;
    pl_leave(lock);
    if (ended) {
      buf_str(why, stop ? "Stopped by the person" : "Signing in again, or signing out, ended this sign in");
      return NULL;
    }
    buf error = { 0 };
    val *poll = onsay_api(a, "POST", "/connect/poll", body, &status, &error);
    buf_free(&error);
    if (!poll || !strcmp(text_or(poll, "status", ""), "waiting"))
      continue;   // a moment without the web is waited out too
    if (strcmp(text_or(poll, "status", ""), "done") || !*text_or(poll, "token", ""))
      buf_fmt(why, "Signing in didn't finish: %s", text_or(poll, "error", "Onsay sent no token"));
    else if (!lingo_set("onsay", "token", rec_get(poll, "token")))
      buf_str(why, "Signed in, but this computer couldn't keep its token");
    else if (!key_of("device", 1, k))
      buf_str(why, "Signed in, but this computer couldn't make or keep its key");
    else
      return rec_get(poll, "account") ? rec_get(poll, "account") : v_rec(a);
    return NULL;
  }
  buf_str(why, "The code ran out before Connect was pressed. Sign in again");
  return NULL;
}

typedef struct {
  int mine;
  char body[];
} onsay_waiting;

// A sign in finished in the background, while the lingo client runs on. status says when it is done.
static void background(void *arg) {
  onsay_waiting *w = arg;
  arena *a = arena_new();
  buf why = { 0 };
  int done = connect_(a, w->body, NULL, w->mine, &why) != NULL;
  if (!done) say("%s", why.s);
  pl_enter(lock);
  if (w->mine == attempt) {
    signing = 0;
    snprintf(onsay_failed, sizeof onsay_failed, "%s", done ? "" : why.s);
  }
  pl_leave(lock);
  buf_free(&why);
  arena_free(a);
  free(w);
}

/* Signs this computer in from any device's browser: Onsay gives an address and a code to open there, and the person
   signs in and presses Connect. In a lingo client that serves, as an app calls it, the address and code come back at once
   and the rest waits in the background, which status follows, even with this lingo stopped, so a request never waits on it. On the command line it says them on the log and waits,
   giving the handle and this device's key. Its install id, kept in the store, gets it its own instance back when it signs
   in again. name is what Onsay calls it, its host name when left out. */
static val *sign_in(run *r, val **v) {
  arena *a = r->a;
  buf body = { 0 };
  buf error = { 0 };
  buf shown = { 0 };
  double status = 0;
  unsigned char key_[32], random[16];
  val *place[2] = { v_cstr(a, "onsay/install"), NOTHING };
  val *install = phrase_find("store.get")->fn(r, place);
  val *out = v_rec(a);
  if (install->k != V_TEXT) {
    buf made = { 0 };
    pl_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++) buf_fmt(&made, "%02x", random[i]);
    install = v_text(a, made.s, made.n);
    place[1] = install;
    phrase_find("store.put")->fn(r, place);
    buf_free(&made);
  }
  const char *name = "Server";
  if (getenv("HOSTNAME")) name = getenv("HOSTNAME");
  if (getenv("COMPUTERNAME")) name = getenv("COMPUTERNAME");
  if (v[0]->k == V_TEXT) name = v[0]->t.s;
  buf_str(&body, "{\"name\":");
  buf_json_str(&body, name, strlen(name));
  buf_str(&body, ",\"install\":");
  buf_json(&body, install);
  buf_chr(&body, '}');
  val *start = onsay_api(a, "POST", "/connect", body.s, &status, &error);
  val *account;
  buf_free(&error);
  if (!start) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 201) fail(r, "phrase", "Onsay didn't start signing in: %s", text_or(start, "error", "it said no"));
  say("Open %s on any device, sign in, and press Connect. Your code is %s", text_or(start, "url", ""),
      text_or(start, "code", ""));
  body.n = 0;
  buf_str(&body, "{\"poll\":");
  buf_json_str(&body, text_or(start, "poll", ""), strlen(text_or(start, "poll", "")));
  buf_chr(&body, '}');
  if (onsay_on || lingo_serves()) {
    onsay_waiting *w = malloc(sizeof *w + body.n + 1);
    memcpy(w->body, body.s, body.n + 1);
    pl_enter(lock);
    w->mine = ++attempt;
    signing = 1;
    *onsay_failed = 0;
    pl_leave(lock);
    pl_thread(background, w);
    rec_set(a, out, "url", rec_get(start, "url") ? rec_get(start, "url") : NOTHING);
    rec_set(a, out, "code", rec_get(start, "code") ? rec_get(start, "code") : NOTHING);
    buf_free(&body);
    buf_free(&error);
    return out;
  }
  error.n = 0;
  account = connect_(a, body.s, r->stop, 0, &error);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!account) fail(r, "phrase", "%s", error.s);
  key_of("device", 0, key_);
  buf_str(&shown, "ed25519:");
  base64(&shown, key_, 32);
  rec_set(a, out, "handle", rec_get(account, "handle") ? rec_get(account, "handle") : NOTHING);
  rec_set(a, out, "key", v_text(a, shown.s, shown.n));
  buf_free(&body);
  buf_free(&error);
  buf_free(&shown);
  return out;
}

// Forgets the token and this device's key, so signing in again makes a new one, and drops the connection.
static val *sign_out(run *r, val **v) {
  if (!lingo_set("onsay", "token", NOTHING)) fail(r, "phrase", "This computer couldn't forget its token");
  key_forget("device");
  pl_enter(lock);
  attempt++;
  signing = 0;
  *handle = *onsay_device = *account = 0;
  if (link_) pl_close(link_);
  pl_leave(lock);
  return NOTHING;
}

/* {signed_in, signing_in, failed, running, connected, handle, device, account, key}: failed is why the last sign in
   didn't finish, or nothing, and running whether this lingo runs. signing_in, failed, connected, handle, device and
   account only in a lingo client that serves. */
static val *status(run *r, val **v) {
  arena *a = r->a;
  val *out = v_rec(a);
  val *token = lingo_setting(a, "onsay", "token");
  unsigned char key_[32];
  buf shown = { 0 };
  if (key_of("device", 0, key_)) {
    buf_str(&shown, "ed25519:");
    base64(&shown, key_, 32);
  }
  rec_set(a, out, "signed_in", VBOOL(token && token->k == V_TEXT && token->t.len));
  pl_enter(lock);
  rec_set(a, out, "signing_in", VBOOL(signing));
  rec_set(a, out, "failed", *onsay_failed ? v_cstr(a, onsay_failed) : NOTHING);
  rec_set(a, out, "running", VBOOL(onsay_on));
  rec_set(a, out, "connected", VBOOL(ready));
  rec_set(a, out, "handle", *handle ? v_cstr(a, handle) : NOTHING);
  rec_set(a, out, "device", *onsay_device ? v_cstr(a, onsay_device) : NOTHING);
  rec_set(a, out, "account", *account ? v_cstr(a, account) : NOTHING);
  pl_leave(lock);
  rec_set(a, out, "key", shown.n ? v_text(a, shown.s, shown.n) : NOTHING);
  buf_free(&shown);
  return out;
}

// The account's computers as Onsay knows them, the oldest first: [{name, online, seen}], seen when each last said anything.
static val *devices(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  double status = 0;
  val *said = onsay_api(a, "GET", "/lingos", NULL, &status, &error);
  val *computers = rec_get(said, "computers");
  val *out = v_list(a);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to see your devices");
  if (status != 200 || !computers || computers->k != V_LIST)
    fail(r, "phrase", "Onsay couldn't list your devices: %s", text_or(said, "error", "it said no"));
  for (size_t i = 0; i < computers->l.n; i++) {
    val *c = computers->l.v[i];
    val *one = v_rec(a);
    rec_set(a, one, "name", rec_get(c, "name") ? rec_get(c, "name") : NOTHING);
    rec_set(a, one, "online", VBOOL(truthy(rec_get(c, "online"))));
    rec_set(a, one, "seen", rec_get(c, "seen") ? rec_get(c, "seen") : NOTHING);
    list_push(a, out, one);
  }
  return out;
}

// Onsay's AI for the account, as Onsay says: {plan, ends, over}, so an app can offer the subscription once the trial ends.
static val *ai(run *r, val **v) {
  buf error = { 0 };
  double status = 0;
  val *said = onsay_api(r->a, "GET", "/ai", NULL, &status, &error);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to use Onsay's AI");
  if (status != 200) fail(r, "phrase", "Onsay couldn't say: %s", text_or(said, "error", "it said no"));
  return said;
}

////////// 8. ITS PHRASES //////////

void base_calls(void);   // the calls every computer has, built for its platform: windows.c or linux.c

// Called once as the lingo client starts (the engine, BUILTINS).
void onsay_lingo(void) {
  static const lingo_hooks hooks = { onsay_running, onsay_decided, ask, key, said, onsay_kept };
  lock = pl_gate_new();
  logging = pl_gate_new();
  lingo_hook(&hooks);
  phrase_add("onsay", "net, store",
      "sign_in(name: text=nothing) -> {url,code}, or {handle,key} on the command line: signs this computer in to "
      "Onsay from any device's browser, at the address given with the code; status says when it is done",
      sign_in);
  phrase_add("onsay", "none", "sign_out(): this computer forgets its sign in and its key", sign_out);
  phrase_add("onsay", "net",
      "devices() -> [{name: text, online: bool, seen?: text}]: the account's computers, whether each is online, and when each was last seen",
      devices);
  phrase_add("onsay", "none",
      "status() -> {signed_in: bool, signing_in?: bool, failed?: text, running: bool, connected?: bool, handle?: text, device?: text, key?: text}: whether this computer is signed in to "
      "Onsay, and as what",
      status);
  phrase_add("onsay", "net",
      "ai() -> {plan: text, ends?: text, over: bool}: Onsay's AI for this account: its plan, trial or paid, when its free "
      "trial ends, and whether it is over",
      ai);
  phrase_add("onsay", "net",
      "share(values: record) -> {version, policy, signed}: shares with who at routes, full paths like [\"/photos/share\"] "
      "or every route when none, on devices when named. who is a handle, handle#tag, acct_..., public, me, my devices or "
      "onsay. Every computer is told",
      share);
  phrase_add("onsay", "net",
      "unshare(values: record) -> {version, policy, signed}: stops sharing with who, taking out every rule that names them",
      onsay_unshare);
  base_calls();
}

//////////////////// 10. HOST ////////////////////

// The lingo client as a program of its own, on a device whose floor has threads and a port.
//
//   onsay-core settings.json                 runs it: its port, and its lingos, any of which may connect it to more,
//                                            like the onsay lingo to Onsay
//   onsay-core settings.json <verb> ...      one thing done, whether or not it runs, its answer printed as JSON:
//     pack <folder> [<to>]                   a folder of a lingo's files made into its .lingo file
//     start <lingo>, stop <lingo>            a lingo running or stopped
//     ceiling <needs...>                     the most this computer ever allows
//     policy                                 the policy in force
//     settings <lingo>                       its settings, each with its value now
//     set <lingo> <key> <value>              one changed. value is JSON, or text as it is when it isn't JSON
//     manifest <lingo>                       its whole OpenAPI manifest, or why it can't load
//     call <lingo.function> [values...]      a function of a lingo, or a phrase, run here, like onsay.sign_in. Each
//                                            value is JSON, or text as it is when it isn't JSON
//
// Threads: main (runs queued requests one at a time, in order, and looks at the lingos folder), listener (takes each
// connection to the port), and one for each web request (reads it, queues it, and writes its answer, or keeps it open
// as a stream). A native lingo
// hands in requests from its own threads (lingo_hand). A request that waits, on lingo(...) or the person's answer, waits
// on main, answering the requests that come meanwhile (lingo_wait), and so does one in a command, a web request or a
// pause. A crash under a phrase ends only its request, with an error (pl_faults, in running).

#define HEAD (64 << 10)   // the most a web request's first line and headers may be
#define BODY_ROOM (4 << 20)   // and its body

// One request for main: its text, its caller (NULL for the port, its ip in it), and who gets its answer.
typedef struct message {
  struct message *next;
  char *text, *caller;
  size_t n;
  void (*answered)(void *arg, const char *response, size_t n);
  void *arg;
} message;

static pl_gate *host_gate;
static pl_gate *watch;   // keeps watchers, and each frame whole on its connection
static message *first, *last;
static long requests;   // web requests taken from the port, for their ids
static int port, outside;   // the port, and whether it is open to other computers, as core_look says
static char outermost[128];   // the id, as JSON, of the request main answers now, with every other waiting inside it

////////// JOBS //////////

// One request answered, on main. On Android each is written to the log as it starts and ends, but the app's polls, so
// one that never ends shows there, since nobody there sees the program's own window.
static void host_handle(message *m) {
  buf out = { 0 };
#ifdef __ANDROID__
  arena *a = arena_new();
  val *request = json_parse(a, m->text, m->n, NULL);
  const char *path = request ? text_or(request, "path", "") : "";
  const char *method = request ? text_or(request, "method", "") : "";
  int logged = strncmp(path, "/.lingo/asks", 12) && !strstr(path, "/live");
  double began = pl_now();
  if (logged) core_say("answering %s %s", method, path);
#endif
  core_serve(m->text, m->n, m->caller, &out);
#ifdef __ANDROID__
  if (logged) core_say("answered %s %s in %.1f seconds", method, path, pl_now() - began);
  arena_free(a);
#endif
  m->answered(m->arg, out.s, out.n);
  buf_free(&out);
  free(m->text);
  free(m->caller);
  free(m);
}

static void queue(
    char *text, size_t n, const char *caller, void (*host_answered)(void *arg, const char *response, size_t n), void *arg) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, caller ? strdup(caller) : NULL, n, host_answered, arg };
  pl_enter(host_gate);
  if (last)
    last->next = m;
  else
    first = m;
  last = m;
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// The next request, waiting up to seconds for one, or NULL.
static message *next(double seconds, volatile int *done, volatile int *stop) {
  pl_enter(host_gate);
  for (double until = pl_now() + seconds; !first && !(done && *done) && !(stop && *stop) && pl_now() < until;)
    pl_wait(host_gate, until - pl_now() < 1 ? until - pl_now() : 1);
  message *m = done && *done ? NULL : first;
  if (m) {
    first = m->next;
    if (!first) last = NULL;
  }
  pl_leave(host_gate);
  return m;
}

// lingo.h: a request from elsewhere, copied, in the queue.
void lingo_hand(const char *request, size_t n, const char *caller,
    void (*host_answered)(void *arg, const char *response, size_t n), void *arg) {
  char *text = malloc(n + 1);
  memcpy(text, request, n);
  text[n] = 0;
  queue(text, n, caller, host_answered, arg);
}

// lingo.h
void lingo_stop(const char *id) {
  core_stop(id);
  pl_enter(host_gate);
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// lingo.h: requests that come meanwhile are answered here, inside the one that waits, since what answers may be here.
void lingo_wait(volatile int *done, volatile int *stop, double seconds) {
  for (double until = pl_now() + seconds; !*done && !*stop && pl_now() < until;) {
    message *m = next(until - pl_now(), done, stop);
    if (m) host_handle(m);
  }
}

// lingo.h
void lingo_done(volatile int *done) {
  pl_enter(host_gate);
  *done = 1;
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// main: every request in turn, and a look at the lingos folder every CORE_LOOK seconds, however busy.
static void jobs(void) {
  for (double look = pl_now() + CORE_LOOK;;) {
    message *m = next(CORE_LOOK, NULL, NULL);
    if (pl_now() >= look) {
      double began = pl_now();
      int wide = core_look();
      if (pl_now() - began > 2) core_say("looking at the lingos folder took %.1f seconds", pl_now() - began);
      pl_enter(host_gate);
      outside = wide;
      pl_leave(host_gate);
      look = pl_now() + CORE_LOOK;
    }
    if (!m) continue;
    arena *a = arena_new();
    val *request = json_parse(a, m->text, m->n, NULL);
    val *id = request ? rec_get(request, "id") : NULL;
    buf said = { 0 };
    if (id) buf_json(&said, id);
    pl_enter(host_gate);
    snprintf(outermost, sizeof outermost, "%s", said.s ? said.s : "");
    pl_leave(host_gate);
    buf_free(&said);
    arena_free(a);
    host_handle(m);
    pl_enter(host_gate);
    outermost[0] = 0;
    pl_leave(host_gate);
  }
}

#ifdef __ANDROID__
// Stops everything running, from any thread. On Android the core lives inside the app, so its Stop can't start the
// program again the way Desktop's does.
void host_stop(void) {
  char id[sizeof outermost];
  pl_enter(host_gate);
  snprintf(id, sizeof id, "%s", outermost);
  pl_leave(host_gate);
  if (id[0]) lingo_stop(id);
}
#endif

////////// STREAMS //////////

/* A web request asking to be a WebSocket, a GET with Upgrade: websocket, is answered like any other first, so every
   check applies, and on a 200 it stays open: its body is the first message, then each the lingo at its path pushes
   (lingo_push), until either side closes it. What the other side sends is not read, but its close. */
typedef struct watcher {
  struct watcher *next;
  pl_socket *c;   // a connection to the port, or NULL for a native lingo's (lingo_watch)
  void (*send)(void *arg, const char *text, size_t n);
  void *arg;
  char path[256];
} watcher;
static watcher *watchers;

// One frame, unmasked, as a server sends it: text, opcode 1, or a ping, 9. Under watch.
static int ws_frame(pl_socket *c, int opcode, const char *text, size_t n) {
  unsigned char head[10];
  size_t k = 2;
  head[0] = (unsigned char)(0x80 | opcode);
  if (n < 126)
    head[1] = (unsigned char)n;
  else if (n < 65536) {
    head[1] = 126;
    head[2] = (unsigned char)(n >> 8);
    head[3] = (unsigned char)n;
    k = 4;
  } else {
    head[1] = 127;
    for (int i = 0; i < 8; i++) head[2 + i] = (unsigned char)((unsigned long long)n >> (56 - 8 * i));
    k = 10;
  }
  return pl_give(c, (const char *)head, k) && (!n || pl_give(c, text, n));
}

// lingo.h
void lingo_push(const char *path, const char *text, size_t n) {
  pl_enter(watch);
  for (watcher *w = watchers; w; w = w->next)
    if (strcmp(w->path, path))
      continue;
    else if (w->c)
      ws_frame(w->c, 1, text, n);
    else
      w->send(w->arg, text, n);
  pl_leave(watch);
}

// lingo.h
void lingo_watch(const char *path, void (*send)(void *arg, const char *text, size_t n), void *arg) {
  watcher *w = calloc(1, sizeof *w);
  w->send = send;
  w->arg = arg;
  snprintf(w->path, sizeof w->path, "%s", path);
  pl_enter(watch);
  w->next = watchers;
  watchers = w;
  pl_leave(watch);
}

// lingo.h
void lingo_unwatch(void *arg) {
  pl_enter(watch);
  for (watcher **at = &watchers; *at; at = &(*at)->next)
    if (!(*at)->c && (*at)->arg == arg) {
      watcher *gone = *at;
      *at = gone->next;
      free(gone);
      break;
    }
  pl_leave(watch);
}

// On the visitor's thread, until it closes: a quiet 10 seconds is asked about with a ping, and nothing coming back at
// once means it closed.
static void stream(pl_socket *c, const char *path, size_t n, const char *key, const char *first, size_t size) {
  buf joined = { 0 };
  buf accept = { 0 };
  buf head = { 0 };
  unsigned char digest[20];
  char chunk[4096];
  watcher w = { NULL, c, NULL, NULL, "" };
  size_t len = 0;
  while (len < n && path[len] != '?') len++;
  snprintf(w.path, sizeof w.path, "%.*s", (int)len, path);
  buf_fmt(&joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
  sha1((const unsigned char *)joined.s, joined.n, digest);
  base64(&accept, digest, sizeof digest);
  buf_fmt(&head,
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n\r\n", accept.s);
  pl_enter(watch);
  int open = pl_give(c, head.s, head.n) && ws_frame(c, 1, first, size);
  if (open) {
    w.next = watchers;
    watchers = &w;
  }
  pl_leave(watch);
  for (int got; open;) {
    double began = pl_now();
    if ((got = pl_take(c, chunk, sizeof chunk)) > 0) {
      open = (chunk[0] & 0x0f) != 8;
      continue;
    }
    pl_enter(watch);
    open = pl_now() - began > 1 && ws_frame(c, 9, "", 0);
    pl_leave(watch);
  }
  pl_enter(watch);
  for (watcher **at = &watchers; *at; at = &(*at)->next)
    if (*at == &w) {
      *at = w.next;
      break;
    }
  pl_leave(watch);
  buf_free(&joined);
  buf_free(&accept);
  buf_free(&head);
}

////////// THE PORT //////////

// An answer written to a web request's connection: its status, its headers by lower case name, and its body.
static void respond(pl_socket *c, int status, val *h, const char *body, size_t n) {
  buf out = { 0 };
  buf_fmt(&out, "HTTP/1.1 %d \r\n", status);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    const char *name = h->r.key[i];
    const char *value = h->r.v[i]->k == V_TEXT ? h->r.v[i]->t.s : "";
    // Its length and closing are written below, and a line break would start a header of its own.
    if (!*name || strpbrk(name, "\r\n: ") || strpbrk(value, "\r\n") || !strcmp(name, "content-length") ||
        !strcmp(name, "connection") || !strcmp(name, "transfer-encoding"))
      continue;
    buf_fmt(&out, "%s: %s\r\n", name, value);
  }
  buf_fmt(&out, "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
  buf_add(&out, body, n);
  pl_give(c, out.s, out.n);
  buf_free(&out);
}

typedef struct {
  buf response;
  int done;
} host_answer;   // a web request's answer, which its thread waits for

static void host_answered(void *arg, const char *response, size_t n) {
  host_answer *to = arg;
  buf_add(&to->response, response, n);
  lingo_done(&to->done);
}

typedef struct {
  pl_socket *c;
  buf from;
} visit;

/* One web request to the port, on its own thread: its first line, headers and a body of Content-Length, read whole,
   answered on main in its turn (core_serve), and closed. One that isn't a web request is answered here with why, and
   one that went quiet before its headers ended is closed. */
static void visitor(void *arg) {
  visit v = *(visit *)arg;
  free(arg);
  arena *a = arena_new();
  buf in = { 0 };
  char chunk[16384];
  const char *end = NULL;
  const char *bad = NULL;
  int status = 400;
  int got = 1;
  size_t method = 0;
  size_t path = 0;
  size_t size = 0;
  size_t from = 0;
  val *headers = v_rec(a);
  while (got && !(end = in.s ? strstr(in.s, "\r\n\r\n") : NULL) && in.n < HEAD)
    if ((got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
  if (!end && in.n >= HEAD) {
    status = 431;
    bad = "Its headers are more than 64 KB";
  } else if (end) {
    method = strspn(in.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (method && method < 16 && in.s[method] == ' ' && in.s[method + 1] == '/') {
      path = strcspn(in.s + method + 1, " \r\n");
    }
    const char *version = in.s + method + 1 + path;
    if (!path || strncmp(version, " HTTP/1.", 8) || (version[8] != '0' && version[8] != '1') ||
        strncmp(version + 9, "\r\n", 2) || !utf8_ok(in.s, (size_t)(end - in.s)))
      bad = "That isn't a web request. Its first line is like GET /memo HTTP/1.1";
    for (const char *s = strstr(in.s, "\r\n") + 2; !bad && s < end; s = strstr(s, "\r\n") + 2) {
      const char *eol = strstr(s, "\r\n");
      const char *colon = memchr(s, ':', (size_t)(eol - s));
      const char *value = colon;
      const char *stop = eol;
      if (!colon || colon == s || memchr(s, ' ', (size_t)(colon - s))) {
        bad = "A header isn't a name, a colon and its value";
        break;
      }
      while (++value < eol && (*value == ' ' || *value == '\t')) {}
      while (stop > value && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
      header_add(a, headers, s, (size_t)(colon - s), value, (size_t)(stop - value));
    }
    const char *length = text_or(headers, "content-length", "0");
    if (!bad && rec_get(headers, "transfer-encoding")) {
      status = 411;
      bad = "Send its body with a Content-Length, not in chunks";
    } else if (!bad && (!*length || strspn(length, "0123456789") != strlen(length)))
      bad = "Its Content-Length isn't a number";
    else if (!bad && (strlen(length) > 9 || (size = strtoul(length, NULL, 10)) > BODY_ROOM)) {
      status = 413;
      bad = "Its body is more than 4 MB";
    }
    from = (size_t)(end - in.s) + 4;
    if (!bad && !strcmp(text_or(headers, "expect", ""), "100-continue") && in.n - from < size)
      pl_give(v.c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (!bad && in.n - from < size && (got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
    if (!bad && in.n - from < size)
      bad = "Its body was shorter than its Content-Length";
    else if (!bad && !utf8_ok(in.s + from, size))
      bad = "Its body isn't text, and lingos take text, like JSON";
  }

  if (bad) {
    buf body = { 0 };
    val *h = v_rec(a);
    rec_set(a, h, "content-type", v_cstr(a, "application/json"));
    buf_str(&body, "{\"error\":");
    buf_json_str(&body, bad, strlen(bad));
    buf_chr(&body, '}');
    respond(v.c, status, h, body.s, body.n);
    buf_free(&body);
  } else if (end) {
    host_answer to = { { 0 }, 0 };
    buf msg = { 0 };
    const char *ip = v.from.s ? v.from.s : "";
#ifdef __ANDROID__
    // Every app on a phone can reach its 127.0.0.1, so only Onsay's own, sending the key it gave the core, is this phone.
    const char *app_key = getenv("ONSAY_APP_KEY");
    if (!app_key || !*app_key || strcmp(text_or(headers, "x-onsay-app", ""), app_key)) ip = "another app";
#endif
    pl_enter(host_gate);
    buf_fmt(&msg, "{\"id\":\"port %ld\",\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(host_gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"ip\":");
    buf_json_str(&msg, ip, strlen(ip));
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_chr(&msg, '}');
    queue(msg.s, msg.n, NULL, host_answered, &to);   // the queue owns the text now
    pl_enter(host_gate);
    while (!to.done) pl_wait(host_gate, 1);
    pl_leave(host_gate);
    val *r = json_parse(a, to.response.s ? to.response.s : "", to.response.n, NULL);
    val *status_ = rec_get(r, "status");
    val *body = rec_get(r, "body");
    const char *key = text_or(headers, "sec-websocket-key", "");
    int ok = status_ && status_->k == V_NUM && status_->n == 200;
    if (ok && *key && method == 3 && !strncmp(in.s, "GET", 3) && strstr(text_or(headers, "upgrade", ""), "ebsocket"))
      stream(v.c, in.s + method + 1, path, key, body ? body->t.s : "", body ? body->t.len : 0);
    else
      respond(v.c, status_ && status_->k == V_NUM ? (int)status_->n : 500, rec_get(r, "headers"), body ? body->t.s : "",
        body ? body->t.len : 0);
    buf_free(&to.response);
  }
  pl_hang_up(v.c);
  buf_free(&in);
  buf_free(&v.from);
  arena_free(a);
}

// The port, on every address this computer has when the policy lets anyone else in, else on this computer alone, so
// nothing asks to be let through its firewall until something is shared. NULL when it can't be, said when say is set.
static pl_socket *listen_(int everywhere, int loud) {
  buf error = { 0 };
  pl_socket *p = pl_listen(port, everywhere, &error);
  if (p)
    core_say("Answering web requests on port %d, %s", port,
        everywhere ? "from other computers too" : "from this computer only");
  else if (loud)
    core_say("%s, so it answers no web requests", error.s);
  buf_free(&error);
  return p;
}

// Takes each connection to the port, and reads it on a thread of its own. When the policy changes who it lets in, the
// port is opened again to fit, within a second of main's look, and tried again each second while it can't be.
static void listener(void *arg) {
  pl_socket *p = arg;
  int wide = outside;
  for (;;) {
    pl_enter(host_gate);
    int want = outside;
    pl_leave(host_gate);
    if (want != wide || !p) {
      if (p) pl_hang_up(p);
      p = listen_(want, want != wide);
      wide = want;
      if (!p) {
        pl_sleep(1);
        continue;
      }
    }
    visit *v = calloc(1, sizeof *v);
    if ((v->c = pl_accept(p, &v->from)))
      pl_thread(visitor, v);
    else {
      buf_free(&v->from);
      free(v);
    }
  }
}

////////// MAIN //////////

// What each verb takes, by name, in order. The last takes the rest of the words, joined by spaces, or for ceiling and
// call as a list, and set's value and call's values are read as JSON when they are JSON.
static const char *TAKES[][4] = { { "start", "lingo" }, { "stop", "lingo" }, { "ceiling", "needs" },
  { "policy" }, { "settings", "lingo" }, { "set", "lingo", "key", "value" }, { "manifest", "lingo" },
  { "call", "fn", "values" } };

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "onsay.json";
  const char *verb = argc > 2 ? argv[2] : NULL;
  size_t k = 0;
  size_t count = sizeof TAKES / sizeof *TAKES;
  while (verb && k < count && strcmp(TAKES[k][0], verb)) k++;
  if (verb && k == count && strcmp(verb, "pack")) {
    core_say(
        "onsay-core %s %s isn't something it does. It does pack, start, stop, ceiling, policy, settings, set, manifest and call",
        path, verb);
    return 1;
  }
  buf text = { 0 };
  if (!pl_read(path, &text)) {
    core_say("Can't read the settings file %s", path);
    return 1;
  }
  const core_settings *s = core_read(text.s, text.n);
  buf_free(&text);
  if (!s) return 1;
  host_gate = pl_gate_new();
  watch = pl_gate_new();
  if (!core_begin(!verb)) return 1;
  if (verb && !strcmp(verb, "pack")) {
    if (argc < 4) {
      core_say("Say which folder, like pack lingos/weather");
      return 1;
    }
    return core_pack(argv[3], argc > 4 ? argv[4] : NULL);
  }
  if (verb) {
    arena *a = arena_new();
    val *values = v_rec(a);
    val *done;
    const char *why = NULL;
    int names = 1;
    while (names < 4 && TAKES[k][names]) names++;
    for (int i = 1; i < names && 2 + i < argc; i++) {
      buf rest = { 0 };
      const char *take = TAKES[k][i];
      val *list = v_list(a);
      val *v;
      val *parsed;
      for (int j = 2 + i; j < (i == names - 1 ? argc : 3 + i); j++) {
        if (rest.n) buf_chr(&rest, ' ');
        buf_str(&rest, argv[j]);
        parsed = !strcmp(take, "values") ? json_parse(a, argv[j], strlen(argv[j]), NULL) : NULL;
        list_push(a, list, parsed ? parsed : v_cstr(a, argv[j]));
      }
      v = !strcmp(take, "needs") || !strcmp(take, "values") ? list : v_text(a, rest.s, rest.n);
      if (!strcmp(take, "value") && (parsed = json_parse(a, rest.s, rest.n, NULL))) v = parsed;
      rec_set(a, values, take, v);
      buf_free(&rest);
    }
    if (!(done = core_control(a, verb, values, &why))) {
      core_say("%s", why);
      return 1;
    }
    buf shown = { 0 };
    buf_json(&shown, done);
    buf_chr(&shown, '\n');
    fwrite(shown.s, 1, shown.n, stdout);
    return 0;
  }
  port = s->port;
  outside = core_look();
  pl_thread(listener, listen_(outside, 1));
  jobs();
  return 0;
}

//////////////////// 11. WINDOWS ////////////////////

#ifdef _WIN32

// Platform.h for Windows, on WinHTTP, so proxies and certificates work as for any Windows program.
//
// 1. Text      UTF-8 to and from UTF-16, shared with windows.c
// 2. Time and threads
// 3. Links     connect, send, receive, close and free
// 4. Files and device, and loading a native lingo
// 5. Web       http.send, one request through WinHTTP
// 6. Port      web requests coming in, through Winsock
// 7. Connections  tcp and udp, out and in, through Winsock
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////// 1. TEXT //////////

wchar_t *win_wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = malloc((size_t)(n ? n : 1) * sizeof *w);
  if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
  return w;
}

void win_narrow(buf *out, const wchar_t *w, int len) {
  if (len < 0) len = (int)wcslen(w);
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
  char *s = malloc((size_t)n + 1);
  WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
  buf_add(out, s, (size_t)n);
  free(s);
}

////////// 2. TIME AND THREADS //////////

double pl_now(void) {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (double)(t - 116444736000000000ULL) / 1e7;
}

void pl_sleep(double seconds) { Sleep(seconds > 0 ? (DWORD)(seconds * 1000) : 0); }

typedef struct {
  void (*fn)(void *);
  void *arg;
} start;

static DWORD WINAPI begin(LPVOID p) {
  start s = *(start *)p;
  free(p);
  s.fn(s.arg);
  return 0;
}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  HANDLE h = CreateThread(NULL, 0, begin, s, 0, NULL);
  if (h) CloseHandle(h);
}

static void *volatile *fault_active;
static void (*fault_recover)(const char *what);
static const char *fault_what;
static DWORD fault_thread;
static int fault_overflow;

// Where a crash continues, as though the instruction that crashed had called it, so the jump out of it unwinds as usual.
__attribute__((force_align_arg_pointer)) static void fault_landing(void) { fault_recover(fault_what); }

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
  const char *what = NULL;
  switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: what = "a bad memory address"; break;
    case EXCEPTION_IN_PAGE_ERROR: what = "memory it couldn't read"; break;
    case EXCEPTION_STACK_OVERFLOW: what = "the stack running out"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: what = "a bad instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "dividing by zero"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: what = "misaligned memory"; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
  }
  if (GetCurrentThreadId() != fault_thread || !*fault_active) return EXCEPTION_CONTINUE_SEARCH;
  fault_what = what;
  fault_overflow = e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
  CONTEXT *c = e->ContextRecord;
  c->Rsp -= 8;
  *(DWORD64 *)c->Rsp = c->Rip;
  c->Rip = (DWORD64)fault_landing;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void pl_faults(void *volatile *active, void (*recover)(const char *what)) {
  ULONG room = 64 << 10;   // what the stack keeps back to handle running out
  fault_active = active;
  fault_recover = recover;
  fault_thread = GetCurrentThreadId();
  SetThreadStackGuarantee(&room);
  SetUnhandledExceptionFilter(on_fault);
}

void pl_fault_over(void) {
  if (fault_overflow) _resetstkoflw();
  fault_overflow = 0;
}

struct pl_gate {
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
};

pl_gate *pl_gate_new(void) {
  pl_gate *g = malloc(sizeof *g);
  InitializeCriticalSection(&g->lock);
  InitializeConditionVariable(&g->wake);
  return g;
}
void pl_enter(pl_gate *g) { EnterCriticalSection(&g->lock); }
void pl_leave(pl_gate *g) { LeaveCriticalSection(&g->lock); }
void pl_wait(pl_gate *g, double seconds) { SleepConditionVariableCS(&g->wake, &g->lock, (DWORD)(seconds * 1000)); }
void pl_wake(pl_gate *g) { WakeAllConditionVariable(&g->wake); }

////////// 3. LINKS //////////

struct pl_link {
  HINTERNET session, connection, socket;
  CRITICAL_SECTION sending;   // WinHTTP allows one send at a time
  volatile LONG closed;
};

static void why(buf *error, const char *what, DWORD code) {
  const char *plain = "Windows error";
  if (code == ERROR_WINHTTP_CANNOT_CONNECT) plain = "nothing answered at that address";
  if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) plain = "that server name wasn't found";
  if (code == ERROR_WINHTTP_TIMEOUT) plain = "it took too long";
  if (code == ERROR_WINHTTP_SECURE_FAILURE) plain = "its certificate wasn't trusted";
  if (code == ERROR_WINHTTP_UNRECOGNIZED_SCHEME || code == ERROR_WINHTTP_INVALID_URL) {
    plain = "that isn't a whole web address";
  }
  buf_fmt(error, "%s: %s (%lu)", what, plain, (unsigned long)code);
}

// The address cracked, and a session and connection to its host. 0 with the step that failed and why.
static DWORD open_url(
    const wchar_t *w, URL_COMPONENTS *parts, HINTERNET *session, HINTERNET *connection, const char **step) {
  memset(parts, 0, sizeof *parts);
  parts->dwStructSize = sizeof *parts;
  parts->dwHostNameLength = parts->dwUrlPathLength = parts->dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(w, 0, 0, parts)) return GetLastError();
  wchar_t host[256] = { 0 };
  wcsncpy(host, parts->lpszHostName, parts->dwHostNameLength < 255 ? parts->dwHostNameLength : 255);
  *step = "Connecting";
  *session = WinHttpOpen(
      L"onsay-core", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!*session) return GetLastError();
  WinHttpSetTimeouts(*session, 10000, 10000, 30000, 30000);
  *connection = WinHttpConnect(*session, host, parts->nPort, 0);
  return *connection ? 0 : GetLastError();
}

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  // WinHTTP reads the address as http or https, then upgrades the connection to a WebSocket.
  buf http = { 0 };
  if (!strncmp(url, "wss://", 6)) {
    buf_str(&http, "https://");
    buf_str(&http, url + 6);
  } else if (!strncmp(url, "ws://", 5)) {
    buf_str(&http, "http://");
    buf_str(&http, url + 5);
  } else
    buf_str(&http, url);
  wchar_t *w = win_wide(http.s);
  wchar_t *extra = headers && *headers ? win_wide(headers) : NULL;
  buf_free(&http);

  pl_link *l = calloc(1, sizeof *l);
  InitializeCriticalSection(&l->sending);
  URL_COMPONENTS parts;
  HINTERNET request = NULL;
  const char *step = "The server address";
  DWORD failed = open_url(w, &parts, &l->session, &l->connection, &step);
  if (failed) goto done;
  // The path runs to the end of the address, so its query, like ?room=1, stays on it.
  request = WinHttpOpenRequest(l->connection, L"GET", parts.dwUrlPathLength ? parts.lpszUrlPath : L"/", NULL,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
      !WinHttpSendRequest(request, extra ? extra : WINHTTP_NO_ADDITIONAL_HEADERS, extra ? (DWORD)-1L : 0,
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, NULL)) {
    failed = GetLastError();
    goto done;
  }

  DWORD status = 0;
  DWORD size = sizeof status;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
      &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 101) {
    buf_fmt(error, "The server answered %lu instead of opening a connection", (unsigned long)status);
    goto done;
  }
  l->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
  if (!l->socket) {
    failed = GetLastError();
    step = "Opening the connection";
    goto done;
  }
  DWORD forever = 0;
  WinHttpSetOption(
      l->socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof forever);   // the heartbeat watches it instead

done:
  if (failed) why(error, step, failed);
  if (request) WinHttpCloseHandle(request);
  free(w);
  free(extra);
  if (!l->socket) {
    pl_free(l);
    return NULL;
  }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) {
  if (l->closed) return 0;
  EnterCriticalSection(&l->sending);
  DWORD e = l->closed
      ? 1
      : WinHttpWebSocketSend(l->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text, (DWORD)n);
  LeaveCriticalSection(&l->sending);
  return e == 0;
}

int pl_receive(pl_link *l, buf *out) {
  char chunk[16384];
  for (;;) {
    if (l->closed) return 0;
    DWORD got = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    if (WinHttpWebSocketReceive(l->socket, chunk, sizeof chunk, &got, &type)) return 0;
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return 0;
    buf_add(out, chunk, got);
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
      return 1;
  }
}

// Closing the socket's handle ends a receive waiting on it at once, which is how the heartbeat drops a dead link.
void pl_close(pl_link *l) {
  if (InterlockedExchange(&l->closed, 1)) return;
  if (l->socket) WinHttpCloseHandle(l->socket);
}

void pl_free(pl_link *l) {
  pl_close(l);
  if (l->connection) WinHttpCloseHandle(l->connection);
  if (l->session) WinHttpCloseHandle(l->session);
  DeleteCriticalSection(&l->sending);
  free(l);
}

////////// 4. FILES AND DEVICE //////////

int pl_read(const char *path, buf *out) {
  wchar_t *w = win_wide(path);
  FILE *f = _wfopen(w, L"rb");
  free(w);
  if (!f) return 0;
  char chunk[16384];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(out, chunk, n);
  fclose(f);
  if (!out->s) buf_add(out, "", 0);
  return 1;
}

int pl_write(const char *path, const char *data, size_t n) {
  buf temp = { 0 };
  buf_fmt(&temp, "%s.new", path);
  wchar_t *wt = win_wide(temp.s);
  wchar_t *w = win_wide(path);
  buf_free(&temp);
  FILE *f = _wfopen(wt, L"wb");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  ok = ok && MoveFileExW(wt, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(wt);
  free(wt);
  free(w);
  return ok;
}

int pl_append(const char *path, const char *data, size_t n) {
  wchar_t *w = win_wide(path);
  FILE *f = _wfopen(w, L"ab");
  free(w);
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  return ok;
}

int pl_delete(const char *path) {
  wchar_t *w = win_wide(path);
  int ok = DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND;
  free(w);
  return ok;
}

void pl_whole(const char *path, buf *out) {
  wchar_t *w = win_wide(path);
  wchar_t full[MAX_PATH * 4];
  if (GetFullPathNameW(w, sizeof full / sizeof *full, full, NULL))
    win_narrow(out, full, -1);
  else
    buf_str(out, path);
  free(w);
}

int pl_folder(const char *path) {
  wchar_t *w = win_wide(path);
  for (wchar_t *p = w; *p; p++) {
    if ((*p == L'/' || *p == L'\\') && p > w && p[-1] != L':') {
      wchar_t c = *p;
      *p = 0;
      CreateDirectoryW(w, NULL);
      *p = c;
    }
  }
  int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
  free(w);
  return ok;
}

void pl_list(
    const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
  buf pattern = { 0 };
  buf_fmt(&pattern, "%s/*", folder);
  wchar_t *w = win_wide(pattern.s);
  buf_free(&pattern);
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileW(w, &found);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
    buf name = { 0 };
    win_narrow(&name, found.cFileName, -1);
    unsigned long long t = ((unsigned long long)found.ftLastWriteTime.dwHighDateTime << 32) |
        found.ftLastWriteTime.dwLowDateTime;
    double size = (double)(((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow);
    each(name.s, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, size,
        (double)(t - 116444736000000000ULL) / 1e7, arg);
    buf_free(&name);
  } while (FindNextFileW(h, &found));
  FindClose(h);
}

// Sealed by DPAPI to this Windows user: the file holds only what Windows can open again, for that user on this computer.
int pl_seal(const char *path, const void *data, size_t n) {
  DATA_BLOB in = { (DWORD)n, (BYTE *)data };
  DATA_BLOB out = { 0 };
  if (!CryptProtectData(&in, L"lingo client key", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
  int ok = pl_write(path, (const char *)out.pbData, out.cbData);
  LocalFree(out.pbData);
  return ok;
}

int pl_unseal(const char *path, buf *out) {
  buf sealed = { 0 };
  DATA_BLOB in = { 0 };
  DATA_BLOB open = { 0 };
  int ok = pl_read(path, &sealed);
  in.cbData = (DWORD)sealed.n;
  in.pbData = (BYTE *)sealed.s;
  ok = ok && CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &open);
  if (ok) {
    buf_add(out, (const char *)open.pbData, open.cbData);
    SecureZeroMemory(open.pbData, open.cbData);
    LocalFree(open.pbData);
  }
  buf_free(&sealed);
  return ok;
}

void pl_random(void *to, size_t n) { BCryptGenRandom(NULL, to, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

// One text field of the context, left out when empty.
static void context_field(buf *out, const char *name, const wchar_t *w) {
  if (!w[0]) return;
  buf t = { 0 };
  win_narrow(&t, w, -1);
  buf_fmt(out, ",\"%s\":", name);
  buf_json_str(out, t.s, t.n);
  buf_free(&t);
}

// The app in front is its program's file name, like chrome, as window.list names it.
void pl_context(buf *out) {
  SYSTEMTIME t;
  TIME_ZONE_INFORMATION z;
  GetLocalTime(&t);
  DWORD kind = GetTimeZoneInformation(&z);
  long offset = -(z.Bias + (kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : z.StandardBias));
  long at = labs(offset);
  buf_fmt(out, "{\"os\":\"windows\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.wYear, t.wMonth, t.wDay,
      t.wHour, t.wMinute, t.wSecond, offset < 0 ? '-' : '+', at / 60, at % 60);
  context_field(out, "zone", kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightName : z.StandardName);
  wchar_t language[LOCALE_NAME_MAX_LENGTH] = L"";
  wchar_t app[MAX_PATH] = L"";
  wchar_t title[512] = L"";
  GetUserDefaultLocaleName(language, LOCALE_NAME_MAX_LENGTH);
  context_field(out, "language", language);
  HWND h = GetForegroundWindow();
  DWORD pid = 0;
  DWORD n = MAX_PATH;
  if (h) GetWindowThreadProcessId(h, &pid);
  HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
  if (p && QueryFullProcessImageNameW(p, 0, app, &n)) {
    wchar_t *slash = wcsrchr(app, L'\\');
    wchar_t *dot;
    if (slash) memmove(app, slash + 1, (wcslen(slash + 1) + 1) * sizeof *app);
    if ((dot = wcsrchr(app, L'.'))) *dot = 0;
  }
  if (p) CloseHandle(p);
  if (h) GetWindowTextW(h, title, 512);
  context_field(out, "app", app);
  context_field(out, "window", title);
  buf_chr(out, '}');
}

// Its own folder is searched first for anything it needs in turn. Never freed: its phrases are in the table from now on.
void *pl_symbol(const char *library, const char *function) {
  HMODULE m = library[0] ? LoadLibraryA(library) : GetModuleHandleA(NULL);
  return m ? (void *)(void (*)(void))GetProcAddress(m, function) : NULL;
}

lingo_entry pl_library(const char *path, buf *error) {
  wchar_t *w = win_wide(path);
  HMODULE m = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  free(w);
  if (!m) {
    wchar_t why[256] = L"";
    DWORD code = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, why, 256, NULL);
    for (size_t n = wcslen(why); n && (why[n - 1] == L'\r' || why[n - 1] == L'\n' || why[n - 1] == L'.'); n--)
      why[n - 1] = 0;
    buf_fmt(error, "Windows can't load it (%lu): ", code);
    win_narrow(error, why, -1);
    return NULL;
  }
  lingo_entry entry = (lingo_entry)(void (*)(void))GetProcAddress(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

////////// 5. WEB //////////

#define ANSWER_ROOM (4u << 20)   // the most an answer may be

// WinHTTP looks at its own timeouts only every 4 seconds or so, so a 1 second one ends after 4. A timer closes the
// request when its time is up instead, which ends whatever call it is waiting in. closed: whoever closes it first.
typedef struct {
  HINTERNET q;
  volatile LONG closed;
} win_watch;

static void CALLBACK too_long(PVOID arg, BOOLEAN fired) {
  win_watch *w = arg;
  (void)fired;
  if (!InterlockedExchange(&w->closed, 1)) WinHttpCloseHandle(w->q);
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  buf head = { 0 };
  buf text = { 0 };
  buf raw = { 0 };
  for (size_t i = 0; i < headers->r.n; i++) buf_fmt(&head, "%s: %s\r\n", headers->r.key[i], headers->r.v[i]->t.s);
  DWORD ms = (DWORD)(seconds * 1000);
  DWORD status = 0;
  DWORD size = sizeof status;
  DWORD failed = 0;
  wchar_t *w = win_wide(url);
  wchar_t *mw = win_wide(method);
  wchar_t *hw = head.n ? win_wide(head.s) : NULL;
  URL_COMPONENTS parts;
  HINTERNET s = NULL;
  HINTERNET c = NULL;
  HINTERNET q = NULL;
  HANDLE timer = NULL;
  win_watch late = { NULL, 0 };
  const char *step = "";
  if (!(failed = open_url(w, &parts, &s, &c, &step))) {
    q = late.q = WinHttpOpenRequest(c, mw, parts.dwUrlPathLength ? parts.lpszUrlPath : L"/", NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (q) CreateTimerQueueTimer(&timer, NULL, too_long, &late, ms, 0, WT_EXECUTEONLYONCE);
    if (!q || !WinHttpSetTimeouts(q, (int)ms, (int)ms, (int)ms, (int)ms) ||
        !WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms) ||
        !WinHttpSendRequest(
            q, hw ? hw : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L, (LPVOID)body, (DWORD)n, (DWORD)n, 0) ||
        !WinHttpReceiveResponse(q, NULL))
      failed = GetLastError();
  }
  if (q && !failed) {
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
        &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(
        q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t *lines = calloc(size / sizeof *lines + 1, sizeof *lines);
    if (WinHttpQueryHeaders(
            q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, lines, &size, WINHTTP_NO_HEADER_INDEX))
      win_narrow(&raw, lines, -1);
    free(lines);
    char chunk[16384];
    for (DWORD got = 1; got && text.n <= ANSWER_ROOM;) {
      if (!WinHttpReadData(q, chunk, sizeof chunk, &got)) {
        failed = GetLastError();
        break;
      }
      buf_add(&text, chunk, got);
    }
  }
  if (timer)
    DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);   // waits for it, if it is closing the request now
  if (late.closed)
    failed = ERROR_WINHTTP_TIMEOUT;
  else if (q)
    WinHttpCloseHandle(q);
  val *out = NULL;
  if (failed) {
    buf where = { 0 };
    buf_fmt(&where, "%s couldn't be reached", url);
    why(error, where.s, failed);
    buf_free(&where);
  } else if (text.n > ANSWER_ROOM)
    buf_fmt(error, "%s answered with more than 4 MB", url);
  else if (!utf8_ok(text.s ? text.s : "", text.n))
    buf_fmt(error, "%s answered with something that isn't text, like a picture or a file", url);
  else {
    out = v_rec(a);
    rec_set(a, out, "status", v_num(a, status));
    rec_set(a, out, "headers", header_lines(a, raw.s ? raw.s : ""));
    rec_set(a, out, "text", v_text(a, text.s ? text.s : "", text.n));
  }
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  free(w);
  free(mw);
  free(hw);
  buf_free(&head);
  buf_free(&text);
  buf_free(&raw);
  return out;
}

////////// 6. PORT //////////

struct pl_socket {
  SOCKET s;
};

// Everywhere: IPv6 and IPv4 on one socket, or IPv4 alone on a computer without IPv6. Else 127.0.0.1 alone, which
// Windows' firewall never asks about.
pl_socket *pl_listen(int port, int everywhere, buf *error) {
  WSADATA w;
  struct sockaddr_in four = { 0 };
  struct sockaddr_in6 six = { 0 };   // its address all zeros: every one, IPv4 too once V6ONLY is off
  DWORD no = 0;
  DWORD yes = 1;
  four.sin_family = AF_INET;
  four.sin_port = htons((u_short)port);
  four.sin_addr.s_addr = htonl(everywhere ? INADDR_ANY : INADDR_LOOPBACK);
  six.sin6_family = AF_INET6;
  six.sin6_port = htons((u_short)port);
  SOCKET s = WSAStartup(MAKEWORD(2, 2), &w) || !everywhere ? INVALID_SOCKET
                                                           : socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  int v6 = s != INVALID_SOCKET;
  if (!v6) s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s != INVALID_SOCKET) {
    setsockopt(
        s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof yes);   // so no other program can listen on it too
    if (v6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&no, sizeof no);
  }
  if (s == INVALID_SOCKET ||
      bind(s, v6 ? (struct sockaddr *)&six : (struct sockaddr *)&four, v6 ? sizeof six : sizeof four) ||
      listen(s, SOMAXCONN)) {
    int code = WSAGetLastError();
    buf_fmt(error, "Port %d couldn't be opened: %s (%d)", port,
        code == WSAEADDRINUSE || code == WSAEACCES ? "another program is using it" : "Windows error", code);
    if (s != INVALID_SOCKET) closesocket(s);
    return NULL;
  }
  pl_socket *p = malloc(sizeof *p);
  p->s = s;
  return p;
}

// An IPv4 address as IPv6 writes it, ::ffff:1.2.3.4, is written 1.2.3.4.
pl_socket *pl_accept(pl_socket *port, buf *from) {
  struct sockaddr_storage at;
  struct timeval second = { 1, 0 };
  fd_set ready;
  int n = sizeof at;
  FD_ZERO(&ready);
  FD_SET(port->s, &ready);
  if (select(0, &ready, NULL, NULL, &second) != 1) return NULL;
  SOCKET c = accept(port->s, (struct sockaddr *)&at, &n);
  if (c == INVALID_SOCKET) return NULL;
  DWORD ms = 10000;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&ms, sizeof ms);
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&ms, sizeof ms);
  char text[64] = "";
  inet_ntop(at.ss_family,
      at.ss_family == AF_INET ? (void *)&((struct sockaddr_in *)&at)->sin_addr
                              : (void *)&((struct sockaddr_in6 *)&at)->sin6_addr,
      text, sizeof text);
  buf_str(from, strncmp(text, "::ffff:", 7) || !strchr(text, '.') ? text : text + 7);
  pl_socket *s = malloc(sizeof *s);
  s->s = c;
  return s;
}

int pl_take(pl_socket *c, char *to, size_t n) {
  int got = recv(c->s, to, (int)n, 0);
  return got > 0 ? got : 0;
}

int pl_give(pl_socket *c, const char *data, size_t n) {
  for (int sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, (int)n, 0)) <= 0) return 0;
  return 1;
}

void pl_hang_up(pl_socket *c) {
  shutdown(c->s, SD_SEND);
  closesocket(c->s);
  free(c);
}

////////// 7. CONNECTIONS //////////

struct pl_conn {
  SOCKET s;
  int udp;
};

static pl_conn *conn(SOCKET s, int udp) {
  pl_conn *c = malloc(sizeof *c);
  c->s = s;
  c->udp = udp;
  return c;
}

// Every address the name has is tried in turn, IPv6 and IPv4, until one answers.
pl_conn *pl_dial(const char *kind, const char *host, int port, buf *error) {
  struct addrinfo hints = { 0 };
  struct addrinfo *found = NULL;
  char service[16];
  int udp = !strcmp(kind, "udp");
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  snprintf(service, sizeof service, "%d", port);
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  if (getaddrinfo(host, service, &hints, &found)) {
    buf_fmt(error, "%s couldn't be found", host);
    return NULL;
  }
  SOCKET s = INVALID_SOCKET;
  for (struct addrinfo *at = found; at && s == INVALID_SOCKET; at = at->ai_next) {
    s = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (s != INVALID_SOCKET && connect(s, at->ai_addr, (int)at->ai_addrlen)) {
      closesocket(s);
      s = INVALID_SOCKET;
    }
  }
  freeaddrinfo(found);
  if (s == INVALID_SOCKET) {
    buf_fmt(error, "%s didn't answer on port %d", host, port);
    return NULL;
  }
  return conn(s, udp);
}

// On 127.0.0.1 alone: what comes from elsewhere comes through the port, where the policies stand.
pl_conn *pl_bind(const char *kind, int port, buf *error) {
  struct sockaddr_in here = { 0 };
  if (strcmp(kind, "tcp")) {
    buf_fmt(error, "only tcp connections can come in so far");
    return NULL;
  }
  here.sin_family = AF_INET;
  here.sin_port = htons((unsigned short)port);
  here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&here, sizeof here) || listen(s, 16)) {
    buf_fmt(error, "port %d couldn't be opened: another program may be using it", port);
    if (s != INVALID_SOCKET) closesocket(s);
    return NULL;
  }
  return conn(s, 0);
}

pl_conn *pl_next(pl_conn *listener, buf *from, double seconds) {
  struct timeval wait = { (long)seconds, (long)((seconds - (long)seconds) * 1e6) };
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(listener->s, &ready);
  if (select((int)listener->s + 1, &ready, NULL, NULL, &wait) != 1) return NULL;
  SOCKET s = accept(listener->s, NULL, NULL);
  if (s == INVALID_SOCKET) return NULL;
  buf_str(from, "127.0.0.1");
  return conn(s, 0);
}

int pl_put(pl_conn *c, const char *data, size_t n) {
  if (c->udp) return send(c->s, data, (int)n, 0) == (int)n;
  for (int sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, (int)n, 0)) <= 0) return 0;
  return 1;
}

int pl_get(pl_conn *c, buf *out) {
  char chunk[65536];
  int got = recv(c->s, chunk, sizeof chunk, 0);
  if (got > 0) buf_add(out, chunk, (size_t)got);
  return got > 0;
}

void pl_shut(pl_conn *c) { shutdown(c->s, SD_BOTH); }

void pl_drop(pl_conn *c) {
  closesocket(c->s);
  free(c);
}


#endif

//////////////////// 12. LINUX ////////////////////

#ifndef _WIN32

// Platform.h for Linux, headless servers first, with the web through libcurl loaded as it starts.
//
// 1. Time and threads   clock_gettime, pthreads, and the gate as a mutex and a condition
// 2. Curl               libcurl found at run time, so the core needs no curl to build and says plainly when it's missing
// 3. Links              a WebSocket written here on a connection curl opens (TCP, TLS and proxies), RFC 6455
// 4. Files and device, and loading a native lingo
// 5. Web                http.send, one request through curl
// 6. Port               web requests coming in, on plain sockets
// 7. Connections        tcp and udp, out and in, on plain sockets
// 8. Android            the same file built into the Android app, where the app does links, the web and what is
//                       around (pl_device), in place of 2, 3, 5 and the context
// libcurl's own WebSocket API is left alone, since many distributions build curl without it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

////////// 1. TIME AND THREADS //////////

double pl_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void pl_sleep(double seconds) {
  if (seconds <= 0) return;
  struct timespec t = { (time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9) };
  while (nanosleep(&t, &t) && errno == EINTR) {}
}

typedef struct {
  void (*fn)(void *);
  void *arg;
} start;

static void *begin(void *p) {
  start s = *(start *)p;
  free(p);
  s.fn(s.arg);
  return NULL;
}

static void *volatile *fault_active;
static void (*fault_recover)(const char *what);
static pthread_t fault_thread;

static void on_fault(int number) {
  const char *what = number == SIGFPE ? "dividing by zero" : number == SIGILL ? "a bad instruction" : "a bad memory address";
  if (!pthread_equal(pthread_self(), fault_thread) || !*fault_active) {
#ifdef __ANDROID__
    pl_crashed(what);
#endif
    signal(number, SIG_DFL);
    return;   // the same crash again, now ending the program
  }
  fault_recover(what);
}

void pl_faults(void *volatile *active, void (*recover)(const char *what)) {
  stack_t aside = { .ss_sp = malloc(64 << 10), .ss_size = 64 << 10 };   // so the stack running out can be handled too
  struct sigaction on = { .sa_handler = on_fault, .sa_flags = SA_ONSTACK | SA_NODEFER };
  fault_active = active;
  fault_recover = recover;
  fault_thread = pthread_self();
  sigaltstack(&aside, NULL);
  sigaction(SIGSEGV, &on, NULL);
  sigaction(SIGBUS, &on, NULL);
  sigaction(SIGILL, &on, NULL);
  sigaction(SIGFPE, &on, NULL);
}

void pl_fault_over(void) {}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  pthread_t t;
  if (pthread_create(&t, NULL, begin, s))
    free(s);
  else
    pthread_detach(t);
}

// Recursive, like the Windows gate, and timed on the monotonic clock, so a change of the time of day never ends a wait.
struct pl_gate {
  pthread_mutex_t lock;
  pthread_cond_t wake;
};

pl_gate *pl_gate_new(void) {
  pl_gate *g = malloc(sizeof *g);
  pthread_mutexattr_t m;
  pthread_mutexattr_init(&m);
  pthread_mutexattr_settype(&m, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g->lock, &m);
  pthread_mutexattr_destroy(&m);
  pthread_condattr_t c;
  pthread_condattr_init(&c);
  pthread_condattr_setclock(&c, CLOCK_MONOTONIC);
  pthread_cond_init(&g->wake, &c);
  pthread_condattr_destroy(&c);
  return g;
}
void pl_enter(pl_gate *g) { pthread_mutex_lock(&g->lock); }
void pl_leave(pl_gate *g) { pthread_mutex_unlock(&g->lock); }
void pl_wake(pl_gate *g) { pthread_cond_broadcast(&g->wake); }

void pl_wait(pl_gate *g, double seconds) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  long long ns = (long long)t.tv_nsec + (long long)(seconds * 1e9);
  t.tv_sec += (time_t)(ns / 1000000000);
  t.tv_nsec = (long)(ns % 1000000000);
  pthread_cond_timedwait(&g->wake, &g->lock, &t);
}

#ifndef __ANDROID__
////////// 2. CURL //////////

// The few parts of curl/curl.h the core uses, with their numbers, which curl never changes.
typedef void CURL;
struct curl_slist;
enum {
  CURLE_OK = 0,
  CURLE_UNSUPPORTED_PROTOCOL = 1,
  CURLE_URL_MALFORMAT = 3,
  CURLE_COULDNT_RESOLVE_HOST = 6,
  CURLE_COULDNT_CONNECT = 7,
  CURLE_WRITE_ERROR = 23,
  CURLE_OPERATION_TIMEDOUT = 28,
  CURLE_SSL_CONNECT_ERROR = 35,
  CURLE_TOO_MANY_REDIRECTS = 47,
  CURLE_PEER_FAILED_VERIFICATION = 60,
  CURLE_SSL_CACERT = 77,
  CURLE_AGAIN = 81,
  CURLOPT_WRITEDATA = 10001,
  CURLOPT_URL = 10002,
  CURLOPT_POSTFIELDS = 10015,
  CURLOPT_USERAGENT = 10018,
  CURLOPT_HTTPHEADER = 10023,
  CURLOPT_HEADERDATA = 10029,
  CURLOPT_CUSTOMREQUEST = 10036,
  CURLOPT_WRITEFUNCTION = 20011,
  CURLOPT_HEADERFUNCTION = 20079,
  CURLOPT_NOBODY = 44,
  CURLOPT_FOLLOWLOCATION = 52,
  CURLOPT_POSTFIELDSIZE = 60,
  CURLOPT_MAXREDIRS = 68,
  CURLOPT_NOSIGNAL = 99,
  CURLOPT_CONNECT_ONLY = 141,
  CURLOPT_TIMEOUT_MS = 155,
  CURLOPT_CONNECTTIMEOUT_MS = 156,
  CURLINFO_RESPONSE_CODE = 0x200002,
  CURLINFO_ACTIVESOCKET = 0x500000 + 44,
  CURL_GLOBAL_ALL = 3,
};

static struct {
  int ok;
  int (*global_init)(long flags);
  CURL *(*init)(void);
  void (*cleanup)(CURL *c);
  int (*setopt)(CURL *c, int option, ...);
  int (*getinfo)(CURL *c, int info, ...);
  int (*perform)(CURL *c);
  int (*send)(CURL *c, const void *data, size_t n, size_t *sent);
  int (*recv)(CURL *c, void *data, size_t n, size_t *got);
  const char *(*strerror)(int code);
  struct curl_slist *(*slist_append)(struct curl_slist *list, const char *line);
  void (*slist_free_all)(struct curl_slist *list);
} curl;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

// Once, before any thread uses it. A write to a closed connection is an error here, never a signal that ends the core.
static void curl_load(void) {
  void *m = dlopen("libcurl.so.4", RTLD_NOW);
  if (!m) return;
  *(void **)&curl.global_init = dlsym(m, "curl_global_init");
  *(void **)&curl.init = dlsym(m, "curl_easy_init");
  *(void **)&curl.cleanup = dlsym(m, "curl_easy_cleanup");
  *(void **)&curl.setopt = dlsym(m, "curl_easy_setopt");
  *(void **)&curl.getinfo = dlsym(m, "curl_easy_getinfo");
  *(void **)&curl.perform = dlsym(m, "curl_easy_perform");
  *(void **)&curl.send = dlsym(m, "curl_easy_send");
  *(void **)&curl.recv = dlsym(m, "curl_easy_recv");
  *(void **)&curl.strerror = dlsym(m, "curl_easy_strerror");
  *(void **)&curl.slist_append = dlsym(m, "curl_slist_append");
  *(void **)&curl.slist_free_all = dlsym(m, "curl_slist_free_all");
  curl.ok = curl.global_init && curl.init && curl.cleanup && curl.setopt && curl.getinfo && curl.perform && curl.send &&
      curl.recv && curl.strerror && curl.slist_append && curl.slist_free_all && !curl.global_init(CURL_GLOBAL_ALL);
  signal(SIGPIPE, SIG_IGN);
}

#define NO_CURL \
  "This computer has no libcurl, which the core needs to reach the web. Install it, like apt install libcurl4 (libcurl4t64 on newer Ubuntu) or dnf install libcurl"

static void why(buf *error, const char *what, int code) {
  const char *plain = curl.strerror(code);
  if (code == CURLE_COULDNT_CONNECT) plain = "nothing answered at that address";
  if (code == CURLE_COULDNT_RESOLVE_HOST) plain = "that server name wasn't found";
  if (code == CURLE_OPERATION_TIMEDOUT) plain = "it took too long";
  if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT || code == CURLE_SSL_CONNECT_ERROR) {
    plain = "its certificate wasn't trusted";
  }
  if (code == CURLE_URL_MALFORMAT || code == CURLE_UNSUPPORTED_PROTOCOL) plain = "that isn't a whole web address";
  if (code == CURLE_TOO_MANY_REDIRECTS) plain = "it sent it on too many times";
  buf_fmt(error, "%s: %s (curl %d)", what, plain, code);
}

////////// 3. LINKS //////////

struct pl_link {
  CURL *c;
  int socket;
  pthread_mutex_t io;   // around every curl_easy_send and curl_easy_recv, never held while waiting
  pthread_mutex_t sending;   // one frame at a time, whole
  volatile int closed;
  double until;   // while it opens, when to give up waiting
  buf held;   // what was read and isn't a whole frame yet
};

// Waits for the socket to be ready, a second at a time so a close is seen. 0 once the link is closed, or out of time.
static int link_ready(pl_link *l, short what) {
  struct pollfd p = { l->socket, what, 0 };
  while (!l->closed && !(l->until && pl_now() > l->until))
    if (poll(&p, 1, 1000) != 0) return !l->closed;
  return 0;
}

// More of what the server sent, added to held. Everything curl has buffered is read before waiting on the socket.
static int read_more(pl_link *l) {
  char chunk[16384];
  for (;;) {
    if (l->closed) return 0;
    size_t got = 0;
    pthread_mutex_lock(&l->io);
    int e = curl.recv(l->c, chunk, sizeof chunk, &got);
    pthread_mutex_unlock(&l->io);
    if (e == CURLE_OK) {
      buf_add(&l->held, chunk, got);
      return got > 0;
    }
    if (e != CURLE_AGAIN || !link_ready(l, POLLIN)) return 0;
  }
}

static int write_all(pl_link *l, const char *s, size_t n) {
  while (n) {
    if (l->closed) return 0;
    size_t sent = 0;
    pthread_mutex_lock(&l->io);
    int e = curl.send(l->c, s, n, &sent);
    pthread_mutex_unlock(&l->io);
    if (e == CURLE_OK) {
      s += sent;
      n -= sent;
    } else if (e != CURLE_AGAIN || !link_ready(l, POLLOUT))
      return 0;
  }
  return 1;
}

// One frame from this side, which is always masked.
static int send_frame(pl_link *l, int op, const char *s, size_t n) {
  unsigned char mask[4];
  if (getrandom(mask, 4, 0) != 4) return 0;
  buf f = { 0 };
  buf_chr(&f, (char)(0x80 | op));
  if (n < 126)
    buf_chr(&f, (char)(0x80 | n));
  else if (n < 65536) {
    buf_chr(&f, (char)(0x80 | 126));
    buf_chr(&f, (char)(n >> 8));
    buf_chr(&f, (char)n);
  } else {
    buf_chr(&f, (char)(0x80 | 127));
    for (int i = 7; i >= 0; i--) buf_chr(&f, (char)((unsigned long long)n >> (8 * i)));
  }
  buf_add(&f, (const char *)mask, 4);
  for (size_t i = 0; i < n; i++) buf_chr(&f, (char)(s[i] ^ mask[i & 3]));
  pthread_mutex_lock(&l->sending);
  int ok = write_all(l, f.s, f.n);
  pthread_mutex_unlock(&l->sending);
  buf_free(&f);
  return ok;
}

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  pthread_once(&curl_once, curl_load);
  if (!curl.ok) {
    buf_str(error, NO_CURL);
    return NULL;
  }
  int secure = !strncmp(url, "wss://", 6);
  if (!secure && strncmp(url, "ws://", 5)) {
    buf_str(error, "The server address: that isn't a whole web address");
    return NULL;
  }
  // curl opens the connection to the address as http or https, and the request to upgrade it is written here.
  unsigned char random[16], digest[20];
  if ((size_t)getrandom(random, sizeof random, 0) != sizeof random) {
    buf_str(error, "This computer gave no random numbers");
    return NULL;
  }
  const char *host = url + (secure ? 6 : 5);
  const char *path = strchr(host, '/');
  if (!path) path = host + strlen(host);
  buf http = { 0 };
  buf ask = { 0 };
  buf key = { 0 };
  buf expect = { 0 };
  buf_fmt(&http, "%s://%.*s", secure ? "https" : "http", (int)(path - host), host);
  base64(&key, random, sizeof random);
  buf_fmt(&expect, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key.s);
  sha1((const unsigned char *)expect.s, expect.n, digest);
  expect.n = 0;
  base64(&expect, digest, sizeof digest);
  buf_fmt(&ask,
      "GET %s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\nUser-Agent: onsay-core\r\n%s\r\n",
      *path ? path : "/", (int)(path - host), host, key.s, headers ? headers : "");

  pl_link *l = calloc(1, sizeof *l);
  pthread_mutex_init(&l->io, NULL);
  pthread_mutex_init(&l->sending, NULL);
  l->socket = -1;
  l->c = curl.init();
  long yes = 1;
  long ms = 10000;
  if (!l->c)
    buf_str(error, "Connecting: curl couldn't start");
  else {
    curl.setopt(l->c, CURLOPT_URL, http.s);
    curl.setopt(l->c, CURLOPT_CONNECT_ONLY, yes);
    curl.setopt(l->c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(l->c, CURLOPT_CONNECTTIMEOUT_MS, ms);
    int e = curl.perform(l->c);
    if (e)
      why(error, "Connecting", e);
    else if (curl.getinfo(l->c, CURLINFO_ACTIVESOCKET, &l->socket) || l->socket < 0)
      buf_str(error, "Connecting: curl gave no connection");
  }

  // Its answer is read up to the blank line after its headers, within 10 seconds. Anything after it is the first frames.
  char *end = NULL;
  l->until = pl_now() + 10;
  if (!error->n && !write_all(l, ask.s, ask.n))
    buf_str(error, "Opening the connection: the request to open it couldn't be sent");
  while (!error->n && !(end = l->held.s ? strstr(l->held.s, "\r\n\r\n") : NULL)) {
    if (!read_more(l))
      buf_str(error,
          pl_now() > l->until ? "Opening the connection: it took too long"
                              : "Opening the connection: the server closed it");
    buf_chr(&l->held, 0);
    l->held.n--;   // a 0 after it, for strstr, never counted
  }
  l->until = 0;
  if (!error->n) {
    int status = 0;
    sscanf(l->held.s, "HTTP/%*s %d", &status);
    size_t head = (size_t)(end - l->held.s) + 4;
    int accepted = 0;
    for (char *line = strstr(l->held.s, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n"))
      if (!strncasecmp(line + 2, "sec-websocket-accept:", 21)) {
        char *v = line + 23;
        while (*v == ' ') v++;
        accepted = !strncmp(v, expect.s, expect.n) && (v[expect.n] == '\r' || v[expect.n] == ' ');
      }
    if (status != 101)
      buf_fmt(error, "The server answered %d instead of opening a connection", status);
    else if (!accepted)
      buf_str(error, "The server's answer to opening the connection wasn't right");
    memmove(l->held.s, l->held.s + head, l->held.n - head);
    l->held.n -= head;
  }
  buf_free(&http);
  buf_free(&ask);
  buf_free(&key);
  buf_free(&expect);
  if (error->n) {
    pl_free(l);
    return NULL;
  }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) { return !l->closed && send_frame(l, 1, text, n); }

// Frames until a whole message: pieces of one joined, a ping answered, a close the end.
int pl_receive(pl_link *l, buf *out) {
  for (;;) {
    // Its length is in 7 bits, or in the 2 or 8 bytes after them, then a mask, which a server shouldn't send.
    unsigned char *h = (unsigned char *)l->held.s;
    size_t n = l->held.n >= 2 ? h[1] & 127 : 0;
    size_t size = 0;
    if (n == 126) size = 2;
    if (n == 127) size = 8;
    size_t at = 2 + size + (l->held.n >= 2 && h[1] & 0x80 ? 4 : 0);
    if (l->held.n < at) {
      if (!read_more(l)) return 0;
      continue;
    }
    if (size) {
      n = 0;
      for (size_t i = 0; i < size; i++) n = n << 8 | h[2 + i];
    }
    if (n > (1u << 30)) return 0;   // a message of more than a GB is no message at all
    if (l->held.n < at + n) {
      if (!read_more(l)) return 0;
      continue;
    }
    int op = h[0] & 15;
    int fin = h[0] & 0x80;
    char *body = l->held.s + at;
    if (h[1] & 0x80)
      for (size_t i = 0; i < n; i++) body[i] ^= (char)h[at - 4 + (i & 3)];
    if (op == 8) {
      l->closed = 1;
      return 0;
    }
    if (op == 9 && !send_frame(l, 10, body, n)) return 0;
    if (op <= 2) buf_add(out, body, n);
    memmove(l->held.s, l->held.s + at + n, l->held.n - at - n);
    l->held.n -= at + n;
    if (op <= 2 && fin) {
      if (!out->s) buf_add(out, "", 0);
      return 1;
    }
  }
}

// Shutting the socket ends a receive waiting on it at once, which is how the heartbeat drops a dead link.
void pl_close(pl_link *l) {
  if (__atomic_exchange_n(&l->closed, 1, __ATOMIC_SEQ_CST)) return;
  if (l->socket >= 0) shutdown(l->socket, SHUT_RDWR);
}

void pl_free(pl_link *l) {
  pl_close(l);
  if (l->c) curl.cleanup(l->c);
  pthread_mutex_destroy(&l->io);
  pthread_mutex_destroy(&l->sending);
  buf_free(&l->held);
  free(l);
}
#endif

////////// 4. FILES AND DEVICE //////////

int pl_read(const char *path, buf *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  char chunk[16384];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(out, chunk, n);
  fclose(f);
  if (!out->s) buf_add(out, "", 0);
  return 1;
}

int pl_write(const char *path, const char *data, size_t n) {
  buf temp = { 0 };
  buf_fmt(&temp, "%s.new", path);
  FILE *f = fopen(temp.s, "wb");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fflush(f) && !fsync(fileno(f)) && !fclose(f) && ok;
  ok = ok && !rename(temp.s, path);
  if (!ok) remove(temp.s);
  buf_free(&temp);
  return ok;
}

int pl_append(const char *path, const char *data, size_t n) {
  FILE *f = fopen(path, "ab");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  return ok;
}

int pl_delete(const char *path) { return !remove(path) || errno == ENOENT; }

// A file only its user can read or write, made so from the start.
int pl_seal(const char *path, const void *data, size_t n) {
  buf temp = { 0 };
  buf_fmt(&temp, "%s.new", path);
  remove(temp.s);
  int fd = open(temp.s, O_WRONLY | O_CREAT | O_EXCL, 0600);
  int ok = fd >= 0 && write(fd, data, n) == (ssize_t)n;
  if (fd >= 0) ok = !fsync(fd) && !close(fd) && ok;
  ok = ok && !rename(temp.s, path);
  if (!ok) remove(temp.s);
  buf_free(&temp);
  return ok;
}

int pl_unseal(const char *path, buf *out) { return pl_read(path, out); }

void pl_random(void *to, size_t n) {
  for (size_t got = 0; got < n;) {
    ssize_t k = getrandom((char *)to + got, n - got, 0);
    if (k > 0) got += (size_t)k;
  }
}

void pl_whole(const char *path, buf *out) {
  char *full = realpath(path, NULL);
  buf_str(out, full ? full : path);
  free(full);
}

int pl_folder(const char *path) {
  char *p = strdup(path);
  for (char *c = p + 1; *c; c++) {
    if (*c != '/') continue;
    *c = 0;
    mkdir(p, 0777);
    *c = '/';
  }
  int ok = !mkdir(p, 0777) || errno == EEXIST;
  free(p);
  return ok;
}

void pl_list(
    const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
  DIR *d = opendir(folder);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    buf path = { 0 };
    buf_fmt(&path, "%s/%s", folder, e->d_name);
    struct stat s;
    if (!stat(path.s, &s))
      each(e->d_name, S_ISDIR(s.st_mode), (double)s.st_size, (double)s.st_mtim.tv_sec + (double)s.st_mtim.tv_nsec / 1e9,
          arg);
    buf_free(&path);
  }
  closedir(d);
}

#ifndef __ANDROID__
// A server has no app or window in front. The zone is TZ, or the system's own: /etc/timezone, or where /etc/localtime
// points, like /usr/share/zoneinfo/America/New_York. The language is LANG's, like en_US.UTF-8 as en-US.
void pl_context(buf *out) {
  time_t now = time(NULL);
  struct tm t;
  localtime_r(&now, &t);
  long offset = t.tm_gmtoff / 60;
  long at = labs(offset);
  buf_fmt(out, "{\"os\":\"linux\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.tm_year + 1900,
      t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, offset < 0 ? '-' : '+', at / 60, at % 60);
  char zone[256] = "";
  char link[256] = "";
  char *z = getenv("TZ");
  FILE *f = NULL;
  ssize_t n = 0;
  if (z && *z)
    snprintf(zone, sizeof zone, "%s", z + (*z == ':'));
  else if ((f = fopen("/etc/timezone", "r"))) {
    if (!fgets(zone, sizeof zone, f)) zone[0] = 0;
    fclose(f);
  }
  if (!zone[0] && (n = readlink("/etc/localtime", link, sizeof link - 1)) > 0) {
    link[n] = 0;
    if ((z = strstr(link, "zoneinfo/"))) snprintf(zone, sizeof zone, "%s", z + 9);
  }
  zone[strcspn(zone, "\r\n")] = 0;
  if (zone[0]) {
    buf_str(out, ",\"zone\":");
    buf_json_str(out, zone, strlen(zone));
  }
  char language[64] = "";
  char *lang = getenv("LANG");
  if (lang && *lang && strcmp(lang, "C") && strncmp(lang, "C.", 2) && strcmp(lang, "POSIX"))
    snprintf(language, sizeof language, "%s", lang);
  language[strcspn(language, ".@")] = 0;
  for (char *c = language; *c; c++)
    if (*c == '_') *c = '-';
  if (language[0]) {
    buf_str(out, ",\"language\":");
    buf_json_str(out, language, strlen(language));
  }
  buf_chr(out, '}');
}
#endif

// Never closed: its phrases are in the table from now on. The core is built with -rdynamic, so a library finds its words.
void *pl_symbol(const char *library, const char *function) {
  void *m = dlopen(library[0] ? library : NULL, RTLD_NOW);
  return m ? dlsym(m, function) : NULL;
}

lingo_entry pl_library(const char *path, buf *error) {
  void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!m) {
    buf_fmt(error, "Linux can't load it: %s", dlerror());
    return NULL;
  }
  lingo_entry entry;
  *(void **)&entry = dlsym(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

#ifndef __ANDROID__
////////// 5. WEB //////////

#define ANSWER_ROOM (4u << 20)   // the most an answer may be

// What curl hands over as it reads. The headers start again at each answer's status line, so only the last one's stay.
static size_t got_text(char *s, size_t size, size_t n, void *to) {
  buf *text = to;
  buf_add(text, s, size * n);
  return text->n > ANSWER_ROOM ? 0 : size * n;
}

static size_t got_header(char *s, size_t size, size_t n, void *to) {
  buf *raw = to;
  if (size * n >= 5 && !strncmp(s, "HTTP/", 5))
    raw->n = 0;
  else
    buf_add(raw, s, size * n);
  return size * n;
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  pthread_once(&curl_once, curl_load);
  buf text = { 0 };
  buf raw = { 0 };
  long status = 0;
  long yes = 1;
  long most = 10;
  val *out = NULL;
  if (!curl.ok) {
    buf_str(error, NO_CURL);
    return NULL;
  }
  // Expect: left empty, so curl never waits a second for a go ahead before sending a body.
  struct curl_slist *head = curl.slist_append(NULL, "Expect:");
  for (size_t i = 0; i < headers->r.n; i++) {
    buf line = { 0 };
    buf_fmt(&line, "%s: %s", headers->r.key[i], headers->r.v[i]->t.s);
    head = curl.slist_append(head, line.s);
    buf_free(&line);
  }
  CURL *c = curl.init();
  if (!c)
    buf_str(error, "curl couldn't start");
  else {
    curl.setopt(c, CURLOPT_URL, url);
    curl.setopt(c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(c, CURLOPT_FOLLOWLOCATION, yes);
    curl.setopt(c, CURLOPT_MAXREDIRS, most);
    curl.setopt(c, CURLOPT_TIMEOUT_MS, (long)(seconds * 1000));
    curl.setopt(c, CURLOPT_USERAGENT, "onsay-core");
    curl.setopt(c, CURLOPT_HTTPHEADER, head);
    curl.setopt(c, CURLOPT_WRITEFUNCTION, got_text);
    curl.setopt(c, CURLOPT_WRITEDATA, &text);
    curl.setopt(c, CURLOPT_HEADERFUNCTION, got_header);
    curl.setopt(c, CURLOPT_HEADERDATA, &raw);
    // GET without a body is curl's own. HEAD reads no body. Anything else sends its body, even an empty one.
    if (!strcmp(method, "HEAD"))
      curl.setopt(c, CURLOPT_NOBODY, yes);
    else if (strcmp(method, "GET") || n) {
      curl.setopt(c, CURLOPT_POSTFIELDSIZE, (long)n);
      curl.setopt(c, CURLOPT_POSTFIELDS, body);
      if (strcmp(method, "POST")) curl.setopt(c, CURLOPT_CUSTOMREQUEST, method);
    }
    int e = curl.perform(c);
    curl.getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (text.n > ANSWER_ROOM)
      buf_fmt(error, "%s answered with more than 4 MB", url);
    else if (e) {
      buf where = { 0 };
      buf_fmt(&where, "%s couldn't be reached", url);
      why(error, where.s, e);
      buf_free(&where);
    } else if (!utf8_ok(text.s ? text.s : "", text.n))
      buf_fmt(error, "%s answered with something that isn't text, like a picture or a file", url);
    else {
      out = v_rec(a);
      rec_set(a, out, "status", v_num(a, (double)status));
      rec_set(a, out, "headers", header_lines(a, raw.s ? raw.s : ""));
      rec_set(a, out, "text", v_text(a, text.s ? text.s : "", text.n));
    }
    curl.cleanup(c);
  }
  curl.slist_free_all(head);
  buf_free(&text);
  buf_free(&raw);
  return out;
}
#endif

////////// 6. PORT //////////

struct pl_socket {
  int s;
};

// Everywhere: IPv6 and IPv4 on one socket, or IPv4 alone on a server without IPv6. Else 127.0.0.1 alone.
pl_socket *pl_listen(int port, int everywhere, buf *error) {
  struct sockaddr_in four = { 0 };
  struct sockaddr_in6 six = { 0 };   // its address all zeros: every one, IPv4 too once V6ONLY is off
  int no = 0;
  int yes = 1;
  int s = everywhere ? socket(AF_INET6, SOCK_STREAM, 0) : -1;
  int v6 = s >= 0;
  four.sin_family = AF_INET;
  four.sin_port = htons((unsigned short)port);
  four.sin_addr.s_addr = htonl(everywhere ? INADDR_ANY : INADDR_LOOPBACK);
  six.sin6_family = AF_INET6;
  six.sin6_port = htons((unsigned short)port);
  if (!v6) s = socket(AF_INET, SOCK_STREAM, 0);
  if (s >= 0) {
    setsockopt(
        s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);   // so a restart doesn't wait for the last one's connections
    if (v6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
  }
  if (s < 0 || bind(s, v6 ? (struct sockaddr *)&six : (struct sockaddr *)&four, v6 ? sizeof six : sizeof four) ||
      listen(s, SOMAXCONN)) {
    buf_fmt(error, "Port %d couldn't be opened: %s", port,
        errno == EADDRINUSE ? "another program is using it" : strerror(errno));
    if (s >= 0) close(s);
    return NULL;
  }
  pl_socket *p = malloc(sizeof *p);
  p->s = s;
  return p;
}

// An IPv4 address as IPv6 writes it, ::ffff:1.2.3.4, is written 1.2.3.4.
pl_socket *pl_accept(pl_socket *port, buf *from) {
  struct sockaddr_storage at;
  socklen_t n = sizeof at;
  struct pollfd link_ready = { port->s, POLLIN, 0 };
  if (poll(&link_ready, 1, 1000) != 1) return NULL;
  int c = accept(port->s, (struct sockaddr *)&at, &n);
  if (c < 0) return NULL;
  struct timeval quiet = { 10, 0 };
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &quiet, sizeof quiet);
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &quiet, sizeof quiet);
  char text[64] = "";
  inet_ntop(at.ss_family,
      at.ss_family == AF_INET ? (void *)&((struct sockaddr_in *)&at)->sin_addr
                              : (void *)&((struct sockaddr_in6 *)&at)->sin6_addr,
      text, sizeof text);
  buf_str(from, strncmp(text, "::ffff:", 7) || !strchr(text, '.') ? text : text + 7);
  pl_socket *s = malloc(sizeof *s);
  s->s = c;
  return s;
}

int pl_take(pl_socket *c, char *to, size_t n) {
  ssize_t got = recv(c->s, to, n, 0);
  return got > 0 ? (int)got : 0;
}

int pl_give(pl_socket *c, const char *data, size_t n) {
  for (ssize_t sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, n, MSG_NOSIGNAL)) <= 0) return 0;
  return 1;
}

void pl_hang_up(pl_socket *c) {
  shutdown(c->s, SHUT_WR);
  close(c->s);
  free(c);
}

////////// 7. CONNECTIONS //////////

struct pl_conn {
  int s;
  int udp;
};

static pl_conn *conn(int s, int udp) {
  pl_conn *c = malloc(sizeof *c);
  c->s = s;
  c->udp = udp;
  return c;
}

// Every address the name has is tried in turn, IPv6 and IPv4, until one answers.
pl_conn *pl_dial(const char *kind, const char *host, int port, buf *error) {
  struct addrinfo hints = { 0 };
  struct addrinfo *found = NULL;
  char service[16];
  int udp = !strcmp(kind, "udp");
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  snprintf(service, sizeof service, "%d", port);
  if (getaddrinfo(host, service, &hints, &found)) {
    buf_fmt(error, "%s couldn't be found", host);
    return NULL;
  }
  int s = -1;
  for (struct addrinfo *at = found; at && s == -1; at = at->ai_next) {
    s = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (s != -1 && connect(s, at->ai_addr, (socklen_t)at->ai_addrlen)) {
      close(s);
      s = -1;
    }
  }
  freeaddrinfo(found);
  if (s == -1) {
    buf_fmt(error, "%s didn't answer on port %d", host, port);
    return NULL;
  }
  return conn(s, udp);
}

// On 127.0.0.1 alone: what comes from elsewhere comes through the port, where the policies stand.
pl_conn *pl_bind(const char *kind, int port, buf *error) {
  struct sockaddr_in here = { 0 };
  if (strcmp(kind, "tcp")) {
    buf_fmt(error, "only tcp connections can come in so far");
    return NULL;
  }
  here.sin_family = AF_INET;
  here.sin_port = htons((unsigned short)port);
  here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == -1 || bind(s, (struct sockaddr *)&here, sizeof here) || listen(s, 16)) {
    buf_fmt(error, "port %d couldn't be opened: another program may be using it", port);
    if (s != -1) close(s);
    return NULL;
  }
  return conn(s, 0);
}

pl_conn *pl_next(pl_conn *listener, buf *from, double seconds) {
  struct timeval wait = { (long)seconds, (long)((seconds - (long)seconds) * 1e6) };
  fd_set link_ready;
  FD_ZERO(&link_ready);
  FD_SET(listener->s, &link_ready);
  if (select((int)listener->s + 1, &link_ready, NULL, NULL, &wait) != 1) return NULL;
  int s = accept(listener->s, NULL, NULL);
  if (s == -1) return NULL;
  buf_str(from, "127.0.0.1");
  return conn(s, 0);
}

int pl_put(pl_conn *c, const char *data, size_t n) {
  if (c->udp) return send(c->s, data, (int)n, 0) == (int)n;
  for (int sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, (int)n, 0)) <= 0) return 0;
  return 1;
}

int pl_get(pl_conn *c, buf *out) {
  char chunk[65536];
  int got = recv(c->s, chunk, sizeof chunk, 0);
  if (got > 0) buf_add(out, chunk, (size_t)got);
  return got > 0;
}

void pl_shut(pl_conn *c) { shutdown(c->s, SHUT_RDWR); }

void pl_drop(pl_conn *c) {
  close(c->s);
  free(c);
}

////////// 8. ANDROID //////////

#ifdef __ANDROID__
// An app on Android has no curl, and Android's own does the web, so the app around the core reaches it, holds the links
// and knows what is around (pl_device in lingo.h).

// The app's answer to name, asked with the record in ask, read into a, or NULL with why in error.
static val *ask_app(arena *a, const char *name, buf *ask, buf *error) {
  buf out = { 0 };
  val *got = NULL;
  int ok = pl_device(name, ask->s ? ask->s : "{}", &out);
  if (ok)
    got = json_parse(a, out.s ? out.s : "null", out.s ? out.n : 4, NULL);
  else
    buf_add(error, out.s ? out.s : "", out.n);
  if (ok && !got) buf_fmt(error, "the phone's answer to %s couldn't be read", name);
  buf_free(&out);
  return got;
}

struct pl_link {
  double id;   // the app's number for it
};

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  arena *a = arena_new();
  buf ask = { 0 };
  buf_str(&ask, "{\"url\":");
  buf_json_str(&ask, url, strlen(url));
  if (headers) {
    buf_str(&ask, ",\"headers\":");
    buf_json(&ask, header_lines(a, headers));
  }
  buf_chr(&ask, '}');
  val *got = ask_app(a, "link.open", &ask, error);
  val *id = got ? rec_get(got, "id") : NULL;
  pl_link *l = NULL;
  if (id && id->k == V_NUM) {
    l = malloc(sizeof *l);
    l->id = id->n;
  } else if (!error->n) {
    buf_str(error, "the phone couldn't open the link");
  }
  buf_free(&ask);
  arena_free(a);
  return l;
}

// One of a link's calls after it opened: each names the link, and a send gives its text too.
static val *linux_link_call(arena *a, pl_link *l, const char *name, const char *text, size_t n) {
  buf ask = { 0 };
  buf error = { 0 };
  buf_fmt(&ask, "{\"id\":%.0f", l->id);
  if (text) {
    buf_str(&ask, ",\"text\":");
    buf_json_str(&ask, text, n);
  }
  buf_chr(&ask, '}');
  val *got = ask_app(a, name, &ask, &error);
  buf_free(&ask);
  buf_free(&error);
  return got;
}

int pl_send(pl_link *l, const char *text, size_t n) {
  arena *a = arena_new();
  int sent = linux_link_call(a, l, "link.send", text, n) != NULL;
  arena_free(a);
  return sent;
}

// The app waits for the next message, and gives {} with no text once the link is closed.
int pl_receive(pl_link *l, buf *out) {
  arena *a = arena_new();
  val *got = linux_link_call(a, l, "link.receive", NULL, 0);
  val *text = got ? rec_get(got, "text") : NULL;
  int open = text && text->k == V_TEXT;
  if (open) buf_add(out, text->t.s, text->t.len);
  arena_free(a);
  return open;
}

void pl_close(pl_link *l) {
  arena *a = arena_new();
  linux_link_call(a, l, "link.close", NULL, 0);
  arena_free(a);
}

void pl_free(pl_link *l) {
  arena *a = arena_new();
  linux_link_call(a, l, "link.free", NULL, 0);
  arena_free(a);
  free(l);
}

// The app knows the time, the zone, the language and the app in front.
void pl_context(buf *out) {
  size_t start = out->n;
  if (pl_device("context", "{}", out)) return;
  out->n = start;
  buf_str(out, "{\"os\":\"android\"}");
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  buf ask = { 0 };
  buf_str(&ask, "{\"method\":");
  buf_json_str(&ask, method, strlen(method));
  buf_str(&ask, ",\"url\":");
  buf_json_str(&ask, url, strlen(url));
  buf_str(&ask, ",\"headers\":");
  buf_json(&ask, headers);
  buf_str(&ask, ",\"body\":");
  buf_json_str(&ask, body ? body : "", body ? n : 0);
  buf_fmt(&ask, ",\"seconds\":%g}", seconds);
  val *got = ask_app(a, "http", &ask, error);
  buf_free(&ask);
  return got;
}
#endif

#endif
