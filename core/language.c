// language.c - Lingo itself, shared by every lingo client built here: values and JSON, the phrases a computer offers,
// the compiler, the runner and its limits, the standard calls, and signing. Included whole by floor.c and lingo.c, which
// each give it may(r, id), whether a call may be made, searched(r, id, values, code, message), what is done with a
// call nothing here answers, and searchable(r, id), whether one that failed is too.
//
// 1. Values      nothing, true/false, numbers, text, lists, records and bytes, in arenas, and JSON
// 2. Phrases     what this computer can do: each call's line, what it needs and its C
// 3. Compiling   Lingo code to the tree the runner runs, every lingo listed and linked
// 4. Running     a tree to its value or its error, within its limits
// 5. Standard    the std calls: length, slice, find, text, JSON and bytes, and compute: the clock, a pause, a log line
// 6. Signing     SHA-256, base64 and Ed25519

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

// Whether x is of the JSON Schema type a type's words make: its type or any of them, its items and its parts.
static int fits_schema(val *x, val *schema) {
  val *any = rec_get(schema, "anyOf");
  val *type = rec_get(schema, "type");
  for (size_t i = 0; any && any->k == V_LIST && i < any->l.n; i++)
    if (fits_schema(x, any->l.v[i])) return 1;
  if (any) return 0;
  int ok = !type;
  for (size_t i = 0; type && !ok && i < (type->k == V_LIST ? type->l.n : 1); i++) {
    const char *t = (type->k == V_LIST ? type->l.v[i] : type)->t.s;
    ok = (!strcmp(t, "string") && x->k == V_TEXT) || (!strcmp(t, "number") && x->k == V_NUM) ||
        (!strcmp(t, "integer") && x->k == V_NUM && x->n == floor(x->n)) || (!strcmp(t, "boolean") && x->k == V_BOOL) ||
        (!strcmp(t, "array") && (x->k == V_LIST || x->k == V_BYTES)) || (!strcmp(t, "object") && x->k == V_REC) ||
        (!strcmp(t, "null") && x->k == V_NOTHING);
  }
  val *items = rec_get(schema, "items");
  for (size_t i = 0; ok && items && x->k == V_LIST && i < x->l.n; i++) ok = fits_schema(x->l.v[i], items);
  val *parts = rec_get(schema, "properties");
  val *required = rec_get(schema, "required");
  for (size_t i = 0; ok && required && x->k == V_REC && i < required->l.n; i++) ok = rec_get(x, required->l.v[i]->t.s) != NULL;
  for (size_t i = 0; ok && parts && x->k == V_REC && i < parts->r.n; i++) {
    val *v = rec_get(x, parts->r.key[i]);
    ok = !v || fits_schema(v, parts->r.v[i]);
  }
  return ok;
}

// Whether x is of the type its words say, like text, [number] or {name: text, age?: whole}.
static int fits(arena *a, val *x, const char *words) {
  return fits_schema(x, type_schema(a, words));
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

static const char *KEYWORDS[] = { "if", "else", "while", "for", "in", "break", "continue", "fn", "return", "try",
  "catch", "fail", "and", "or", "not", "true", "false", "nothing", "null", NULL };
static const char *SYMBOLS[] = { "..", "->", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "??", "&&", "||", "=", "+",
  "-", "*", "/", "%", "<", ">", "!", "?", ":", "(", ")", "[", "]", "{", "}", ",", ".", ";", "|", NULL };

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
/* Plain text, with no {} in it, saying what something is: its words, or NULL when the next token isn't one. */
static const char *says(ps *p) {
  token *t = peek(p, 0);
  if (!t || t->k != STR || t->count > 1 || (t->count == 1 && t->parts[0].code)) return NULL;
  p->i++;
  return t->count ? compile_dup(p->c->a, t->parts[0].s, t->parts[0].n) : "";
}

static int type_words(ps *p, buf *out);

// One type in a signature, written into out as its words: a word like text or path, [type], {part: type, maybe?: type},
// each with a ? after it when it may be nothing. 0 with the problem kept when it isn't one.
static int type_one(ps *p, buf *out) {
  token *t;
  if (parse_accept(p, "[")) {
    buf_chr(out, '[');
    if (!type_words(p, out) || !expect(p, "]", "a ] to end the list's type")) return 0;
    buf_chr(out, ']');
  } else if (parse_accept(p, "{")) {
    buf_chr(out, '{');
    for (int first = 1; !parse_accept(p, "}"); first = 0) {
      if (!first && !expect(p, ",", "a , between the parts")) return 0;
      if (!first) buf_str(out, ", ");
      t = peek(p, 0);
      if (!t || (t->k != ID && t->k != KW)) return expected(p, "a part's name, like {name: text}") != NULL;
      p->i++;
      buf_str(out, t->v);
      if (parse_accept(p, "?")) buf_chr(out, '?');
      if (!expect(p, ":", "a : before the part's type")) return 0;
      buf_str(out, ": ");
      if (!type_words(p, out)) return 0;
    }
    buf_chr(out, '}');
  } else {
    t = peek(p, 0);
    if (!t || (t->k != ID && t->k != KW)) return expected(p, "a type, like text, number, [text] or {name: text}") != NULL;
    p->i++;
    buf_str(out, t->v);
  }
  if (parse_accept(p, "?")) buf_chr(out, '?');
  return 1;
}

// A type, or several joined by |, either of them.
static int type_words(ps *p, buf *out) {
  if (!type_one(p, out)) return 0;
  while (parse_accept(p, "|")) {
    buf_chr(out, '|');
    if (!type_one(p, out)) return 0;
  }
  return 1;
}

/* A function's values: each a name, then as its maker wants its type, its words and its default, like
   (city: text "a city's name", days: whole = 1). Each is [name, default?] in the tree, and its type and words go in types
   and words, by name. */
static val *params(ps *p, val *types, val *words) {
  arena *a = p->c->a;
  if (!expect(p, "(", NULL)) return NULL;
  val *out = v_list(a);
  if (parse_accept(p, ")")) return out;
  do {
    GET(n, name(p, "the name of a value it takes"));
    val *one = v_list(a);
    list_push(a, one, n);
    if (parse_accept(p, ":")) {
      buf t = { 0 };
      int ok = type_words(p, &t);
      if (ok) rec_set(a, types, n->t.s, v_text(a, t.s, t.n));
      buf_free(&t);
      if (!ok) return NULL;
    }
    const char *w = says(p);
    if (w) rec_set(a, words, n->t.s, v_cstr(a, w));
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
  if (is(p, "fn", 0)) return problem(c, line_now(p), "\"fn\" goes at the top level, not inside a block");
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

/* fn blocks in any order, and steps outside them, which are the program's tool. A fn is
   fn name(values) -> type "what it does" {...}, its type and words as its maker wants them. Text before the first fn is
   what the lingo is, its about. */
static val *program(ps *p) {
  cx *c = p->c;
  arena *a = c->a;
  val *fns = v_rec(a);
  val *loose = v_list(a);
  val *tool = v_rec(a);
  val *out = v_rec(a);
  token *next = peek(p, 1);
  if (next && next->k == KW && !strcmp(next->v, "fn")) {
    const char *about = says(p);
    if (about) rec_set(a, out, "about", v_cstr(a, about));
  }
  while (!end(p)) {
    if (parse_accept(p, ";")) continue;
    if (is(p, "fn", 0)) {
      p->i++;
      GET(n, name(p, "the name of the function"));
      if (rec_get(fns, n->t.s))
        return problem(c, line_now(p), arena_fmt(a, "There are two functions called %s", n->t.s));
      val *f = v_rec(a);
      val *types = v_rec(a);
      val *words = v_rec(a);
      GET(ps_, params(p, types, words));
      if (parse_accept(p, "->")) {
        buf t = { 0 };
        int ok = type_words(p, &t);
        if (ok) rec_set(a, f, "gives", v_text(a, t.s, t.n));
        buf_free(&t);
        if (!ok) return NULL;
      }
      const char *does = says(p);
      GET(body, block(p));
      rec_set(a, f, "params", ps_);
      rec_set(a, f, "body", body);
      rec_set(a, f, "types", types);
      rec_set(a, f, "words", words);
      if (does) rec_set(a, f, "does", v_cstr(a, does));
      rec_set(a, fns, n->t.s, f);
    } else {
      GET(s, statement(p));
      list_push(a, loose, s);
    }
  }
  rec_set(a, tool, "name", T(a, "steps"));
  rec_set(a, tool, "params", v_list(a));
  rec_set(a, tool, "body", loose);
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

/* 3. A lingo written in Lingo is its code, <name>.ls, and its listing is made from it, never written by hand:
     each fn        a function it shares, called at its route, like /memo/recall, with its values, each its default,
                    type and words, its type, and the text after its signature, what it does. One whose name starts with
                    _ is the lingo's own, internal, never listed by Onsay
     its about      the text before its first fn, what the lingo is
     settings       the names of its own settings it reads with setting("key")
   Comments are only comments. Gives {name, about, phrases: [{id, params, does, gives, internal}], settings}, or NULL with
   the problem, starting with the file. */
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
  if (rec_get(tool, "body")->l.n)
    return compile_failed(why, arena_fmt(a, "%s: only fn blocks belong in it, with no steps outside them", file));
  if (!fns->r.n) return compile_failed(why, arena_fmt(a, "%s: a lingo shares its fn blocks, and it has none", file));

  val *out = v_rec(a);
  val *phrases = v_list(a);
  rec_set(a, out, "name", name);
  if (rec_get(read, "about")) rec_set(a, out, "about", rec_get(read, "about"));
  for (size_t i = 0; i < fns->r.n; i++) {
    const char *fname = fns->r.key[i];
    val *fn = fns->r.v[i];
    val *given = rec_get(fn, "params");
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
      const char *pname = given->l.v[k]->l.v[0]->t.s;
      rec_set(a, p, "name", given->l.v[k]->l.v[0]);
      if (d) rec_set(a, p, "default", d);
      if (rec_get(rec_get(fn, "types"), pname)) rec_set(a, p, "type", rec_get(rec_get(fn, "types"), pname));
      if (rec_get(rec_get(fn, "words"), pname)) rec_set(a, p, "words", rec_get(rec_get(fn, "words"), pname));
      list_push(a, params, p);
    }
    rec_set(a, ph, "id", T(a, arena_fmt(a, "%s.%s", name->t.s, fname)));
    rec_set(a, ph, "params", params);
    rec_set(a, ph, "does", rec_get(fn, "does") ? rec_get(fn, "does") : T(a, ""));
    if (rec_get(fn, "gives")) rec_set(a, ph, "gives", rec_get(fn, "gives"));
    if (fname[0] == '_') rec_set(a, ph, "internal", VBOOL(1));
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
  val *checks = v_list(a);   // [name, type] for each value with a type, checked as the call comes in
  for (size_t i = 0; i < given->l.n; i++) {
    val *name = T(a, arena_fmt(a, "a%zu", i));
    val *p = v_list(a);
    val *type = rec_get(given->l.v[i], "type");
    list_push(a, p, name);
    list_push(a, params, p);
    rec_set(a, args, rec_get(given->l.v[i], "name")->t.s, mk(a, "var", 1, name));
    val *check = v_list(a);
    list_push(a, check, rec_get(given->l.v[i], "name"));
    list_push(a, check, type ? type : NOTHING);
    list_push(a, checks, check);
  }
  rec_set(a, tool, "checks", checks);
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
        val *said = rec_get(params->l.v[m], "words");
        if (said) rec_set(a, one, "description", said);
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

static _Thread_local const char *phrase_now;   // the phrase running on this thread, so its errors say which it was
static _Thread_local void *volatile running;   // the innermost run on this thread, so a crash inside it ends only it
static void *running_now(void) { return running; }

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
  if (strcmp(p->needs, "none")) may(r, id);   // anything but pure computing is a request to this computer
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
  if (!guarded) pl_faults(running_now, crashed);
  guarded = 1;
  running = r;
  if (!setjmp(t.j)) {
    val *tool = rec_get(program, "tool");
    val *frame = v_rec(a);
    buf_str(&r->at, "tool");
    bind_args(r, rec_get(tool, "params"), input, 1, frame);
    // A value given from outside must be of the type its function says, when it says one and the value is there.
    val *checks = rec_get(tool, "checks");
    for (size_t i = 0; checks && i < checks->l.n; i++) {
      val *type = checks->l.v[i]->l.v[1];
      val *v = rec_get(frame, arena_fmt(a, "a%zu", i));
      if (type->k == V_TEXT && v && v->k != V_NOTHING && !fits(a, v, type->t.s))
        fail(r, "type", "%s is %s, but this is %s", checks->l.v[i]->l.v[0]->t.s, type->t.s, kind_name(v));
    }
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

// compute: the clock, a pause and a line in the log, which every lingo client has, the standard functions too.
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
  core_say("%s", text_of(r, a[0])->t.s);
  return NOTHING;
}

void std_phrases(void) {
  phrase_add("compute", "none", "now() -> number: seconds since 1970", time_);
  phrase_add("compute", "none", "wait(seconds: number): at most 60", wait_);
  phrase_add("compute", "none", "log(x: any): a line in the lingo client's log", log_);
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

