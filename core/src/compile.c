// compile.c - Lingo code to the tree the runner runs: every lingo in the lingos folder, read, listed and compiled, and a
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
#include "lingo.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *KEYWORDS[] = { "if", "else", "while", "for", "in", "break", "continue", "fn", "return", "tool", "try", "catch",
                                  "fail", "and", "or", "not", "true", "false", "nothing", "null", NULL };
static const char *SYMBOLS[] = { "..", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "??", "&&", "||", "=", "+", "-", "*", "/", "%",
                                 "<", ">", "!", "?", ":", "(", ")", "[", "]", "{", "}", ",", ".", ";", NULL };

////////// HELPERS //////////

static char *fmt(arena *a, const char *f, ...) {
  va_list ap, again;
  va_start(ap, f);
  va_copy(again, ap);
  int n = vsnprintf(NULL, 0, f, ap);
  va_end(ap);
  char *s = arena_get(a, (size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, f, again);
  va_end(again);
  return s;
}

static char *dup(arena *a, const char *s, size_t n) {
  char *out = arena_get(a, n + 1);
  memcpy(out, s, n);
  return out;
}

static char *buf_dup(arena *a, buf *b) { return dup(a, b->s ? b->s : "", b->n); }
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
  for (size_t i = 0; i < list->l.n; i++) if (!strcmp(list->l.v[i]->t.s, text->t.s)) return;
  list_push(a, list, text);
}

static val *failed(const char **message, char *m) { *message = m; return NULL; }

// One character of UTF-8 at s[i], and how many bytes it takes.
static unsigned code_at(const char *s, size_t i, size_t n, size_t *len) {
  unsigned char c = (unsigned char)s[i];
  size_t w = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
  if (i + w > n) w = 1;
  unsigned cp = w == 1 ? c : w == 2 ? c & 0x1F : w == 3 ? c & 0x0F : c & 0x07;
  for (size_t k = 1; k < w; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  *len = w;
  return cp;
}

////////// CONTEXT //////////

typedef struct { const char *name; int count, line; val *node; } site;

typedef struct {
  arena *a;
  val *phrases;          // id -> phrase, every phrase the code may use
  const char *own;       // the name of the lingo whose code it is, like agent, for setting(...)
  int scan;              // only read for its listing: calls aren't checked, since what they reach isn't known yet
  val *settings;         // the names of its own settings it reads, setting("key"), in order
  val *fn_lines;         // each fn's name -> the line it starts on, for the comments above it
  site *calls;           // calls with no dot, settled once every function is known
  size_t ncalls, room;
  val **marks;           // statement -> its line, a small table keyed by the statement itself
  int *lines;
  size_t nmarks, cap;
  int bad, line;
  const char *message;
} cx;

static void *problem(cx *c, int line, const char *message) {
  if (!c->bad) { c->bad = 1; c->line = line; c->message = message; }
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
      if (c->marks[i]) { size_t h = slot(marks, cap, c->marks[i]); marks[h] = c->marks[i]; lines[h] = c->lines[i]; }
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
  for (size_t i = 0; phrases && i < phrases->l.n; i++) rec_set(a, c->phrases, rec_get(phrases->l.v[i], "id")->t.s, phrases->l.v[i]);
  return c;
}

////////// 1. LEX //////////

enum { NUM, ID, KW, STR, SYM };
typedef struct { const char *s; size_t n; int code, line; } piece;   // code: an expression's source, from {} in text
typedef struct { int k, line; const char *v; double num; piece *parts; size_t count; } token;
typedef struct { token *t; size_t n, cap; } tokens;

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
  return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000 || cp == 0xFEFF ? w : 0;
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
    if (ch == '\n') { line++; i++; continue; }
    if ((w = space_at(s, i, n))) { i += w; continue; }
    if (ch == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
    if (digit(ch)) {
      size_t j = i;
      while (j < n && digit(s[j])) j++;
      if (j + 1 < n && s[j] == '.' && digit(s[j + 1])) { j++; while (j < n && digit(s[j])) j++; }
      add_token(a, out, (token){ .k = NUM, .line = line, .num = strtod(dup(a, s + i, j - i), NULL) });
      i = j;
      continue;
    }
    if (alpha(ch)) {
      size_t j = i;
      while (j < n && (alpha(s[j]) || digit(s[j]))) j++;
      int kw = 0;
      for (int k = 0; KEYWORDS[k] && !kw; k++) kw = strlen(KEYWORDS[k]) == j - i && !memcmp(KEYWORDS[k], s + i, j - i);
      add_token(a, out, (token){ .k = kw ? KW : ID, .line = line, .v = dup(a, s + i, j - i) });
      i = j;
      continue;
    }
    if (ch == '"') {
      // Text: \ escapes, and {expression} puts a value inside. \{ is a plain brace.
      int start = line;
      piece *parts = NULL;
      size_t count = 0, room = 0;
      buf cur = { 0 };
      for (i++;; i++) {
        if (i >= n) { buf_free(&cur); problem(c, start, "This text never ends: a \" is missing"); return 0; }
        char h = s[i];
        if (h == '"') { i++; break; }
        if (h == '\n') line++;
        if (h == '\\') {
          if (++i >= n) continue;   // and the loop says the text never ends
          code_at(s, i, n, &w);
          if (s[i] == 'n') buf_chr(&cur, '\n');
          else if (s[i] == 't') buf_chr(&cur, '\t');
          else buf_add(&cur, s + i, w);
          i += w - 1;
          continue;
        }
        if (h == '{') {
          int depth = 1;
          size_t j = i + 1;
          for (; j < n && depth; j++) {
            if (s[j] == '{') depth++;
            else if (s[j] == '}') depth--;
            else if (s[j] == '"') { buf_free(&cur); problem(c, line, "Text inside {} inside text is not allowed. Put it in a name first"); return 0; }
          }
          if (depth) { buf_free(&cur); problem(c, line, "A { inside text is never closed"); return 0; }
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
      for (int k = 0; k < 10 && j < n; k++) { code_at(s, j, n, &w); j += w; }
      problem(c, line, fmt(a, "I can't read \"%.*s\"", (int)(j - i), s + i));
      return 0;
    }
  }
  return 1;
}

////////// 2. PARSE //////////

typedef struct { cx *c; token *t; size_t n, i; } ps;

static token *peek(ps *p, size_t k) { return p->i + k < p->n ? &p->t[p->i + k] : NULL; }
static int end(ps *p) { return p->i >= p->n; }
static int line_now(ps *p) { token *t = peek(p, 0); if (!t && p->n) t = &p->t[p->n - 1]; return t ? t->line : 1; }
static int is(ps *p, const char *v, size_t k) { token *t = peek(p, k); return t && (t->k == SYM || t->k == KW) && !strcmp(t->v, v); }
static int accept(ps *p, const char *v) { if (!is(p, v, 0)) return 0; p->i++; return 1; }

static const char *show(ps *p, token *t) {
  if (!t) return "the end";
  if (t->k == STR) return "some text";
  if (t->k == NUM) { buf b = { 0 }; buf_num(&b, t->num); char *s = fmt(p->c->a, "\"%s\"", b.s); buf_free(&b); return s; }
  return fmt(p->c->a, "\"%s\"", t->v);
}

static void *expected(ps *p, const char *what) {
  return problem(p->c, line_now(p), fmt(p->c->a, "Expected %s, but found %s", what, show(p, peek(p, 0))));
}

static int expect(ps *p, const char *v, const char *what) {
  if (accept(p, v)) return 1;
  expected(p, what ? what : fmt(p->c->a, "\"%s\"", v));
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

#define GET(x, e) val *x = (e); if (!x) return NULL

static val *expr(ps *p);
static val *statement(ps *p);

// (a, b=10). A default is any expression, worked out when the value is missing.
static val *params(ps *p) {
  arena *a = p->c->a;
  if (!expect(p, "(", NULL)) return NULL;
  val *out = v_list(a);
  if (accept(p, ")")) return out;
  do {
    GET(n, name(p, "the name of a value it takes"));
    val *one = v_list(a);
    list_push(a, one, n);
    if (accept(p, "=")) { GET(d, expr(p)); list_push(a, one, d); }
    list_push(a, out, one);
  } while (accept(p, ","));
  if (!expect(p, ")", NULL)) return NULL;
  return out;
}

static val *block(ps *p) {
  val *out = v_list(p->c->a);
  if (!expect(p, "{", "a { to start the block")) return NULL;
  while (!accept(p, "}")) {
    if (end(p)) return expected(p, "a } to end the block");
    if (accept(p, ";")) continue;
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
  if (accept(p, "if")) {
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
    if (accept(p, "else")) { otherwise = block(p); if (!otherwise) return NULL; }
    return mk(a, "if", 2, branches, otherwise);
  }
  if (accept(p, "while")) { GET(w, cond(p)); GET(b, block(p)); return mk(a, "while", 2, w, b); }
  if (accept(p, "for")) {
    // for x in e {...}, or the older for(x in e){...}, told apart by what follows the (.
    token *t1 = peek(p, 1), *t3 = peek(p, 3);
    int wrapped = is(p, "(", 0) && t1 && t1->k == ID && (is(p, "in", 2) || (is(p, ",", 2) && t3 && t3->k == ID && is(p, "in", 4)));
    if (wrapped) p->i++;
    GET(first, name(p, "a name for each item"));
    val *second = NULL;
    if (accept(p, ",")) { second = name(p, "a second name"); if (!second) return NULL; }
    if (!expect(p, "in", "\"in\"")) return NULL;
    GET(from, expr(p));
    if (wrapped && !expect(p, ")", NULL)) return NULL;
    GET(body, block(p));
    // for x in list names the item. for i, x in list names the position and the item, or a record's part and value.
    return second ? mk(a, "each", 4, second, from, body, first) : mk(a, "each", 3, first, from, body);
  }
  if (accept(p, "break")) return mk(a, "break", 0);
  if (accept(p, "continue")) return mk(a, "next", 0);
  if (accept(p, "return")) {
    if (end(p) || is(p, "}", 0) || is(p, ";", 0)) return mk(a, "return", 1, NOTHING);
    GET(e, expr(p));
    return mk(a, "return", 1, e);
  }
  if (accept(p, "fail")) { GET(e, expr(p)); return mk(a, "fail", 1, e); }
  if (accept(p, "try")) {
    GET(body, block(p));
    if (!expect(p, "catch", "\"catch\" after the try block")) return NULL;
    GET(n, name(p, "a name for the error, like catch e"));
    GET(caught, block(p));
    return mk(a, "try", 3, body, n, caught);
  }
  if (is(p, "tool", 0) || is(p, "fn", 0)) return problem(c, line_now(p), fmt(a, "\"%s\" goes at the top level, not inside a block", peek(p, 0)->v));
  if (is(p, "else", 0)) return problem(c, line_now(p), "\"else\" needs an if block just before it");

  GET(e, expr(p));
  static const char *sets[] = { "=", "+=", "-=", "*=", "/=" };
  for (int k = 0; k < 5; k++) {
    if (!accept(p, sets[k])) continue;
    GET(value, expr(p));
    if (k) value = mk(a, "op", 3, v_text(a, sets[k], 1), e, value);
    if (tagged(e, "var")) return mk(a, "set", 2, e->l.v[1], value);
    if (tagged(e, "get")) return mk(a, "put", 3, e->l.v[1], e->l.v[2], value);
    return problem(c, line_now(p), "Only a name, a part or an item can be set");
  }
  if (!tagged(e, "call") && !tagged(e, "say")) {
    if (tagged(e, "var")) {
      const char *v = e->l.v[1]->t.s;
      return problem(c, line_now(p), fmt(a, "\"%s\" on its own does nothing. Did you mean %s=... or %s(...)?", v, v, v));
    }
    return problem(c, line_now(p), "This line works something out and then throws it away. Set a name to it, or return it");
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
  if (accept(p, close)) return out;
  do {
    if (is(p, close, 0)) break;
    GET(e, expr(p));
    list_push(p->c->a, out, e);
  } while (accept(p, ","));
  if (!expect(p, close, NULL)) return NULL;
  return out;
}

/* use(address, values) is a call of lingo.use, made as the program reaches it: Onsay reaches what the address names
   and gives back its answer. A lingo at an address, like laptop.brandon/test/find, runs there with the
   values, a record by name or a list in order, an MCP tool after a #, any other web address is a plain web request, and
   after an @, where the answer goes. Everything local needs no use: a lingo in the same lingos folder is a dotted call. */
static val *using(cx *c, val *args, int line) {
  arena *a = c->a;
  val *address = at(args, 0), *named = v_rec(a);
  if (!address) return problem(c, line, "use(...) says what it reaches first, like use(\"laptop.brandon/weather/now\", {place: \"Paris\"})");
  if (args->l.n > 2) return problem(c, line, "use(...) takes an address and its values, and no more");
  if (address->k == V_TEXT && !address->t.len) return problem(c, line, "use(...) names what it reaches, like use(\"laptop.brandon/weather/now\", {place: \"Paris\"})");
  rec_set(a, named, "address", address);
  rec_set(a, named, "values", args->l.n > 1 ? args->l.v[1] : NOTHING);
  return mk(a, "say", 2, T(a, "lingo.use"), named);
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
      char *path = fmt(a, "%s", id + first + 1);
      for (char *s = path; *s; s++) if (*s == '.') *s = '/';
      return problem(c, line, fmt(a, "There's no lingo or group called %.*s here. To reach one somewhere else, write use(\"its address\"), like use(\"laptop.brandon/%.*s/%s\")",
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
      if (near++) buf_str(&b, ", "); else buf_fmt(&b, " %.*s has: ", (int)glen, id);
      buf_str(&b, k + glen + 1);
    }
    if (!near) {
      buf_str(&b, " The groups are: ");
      val *groups = v_list(a);
      for (size_t i = 0; i < c->phrases->r.n; i++) {
        const char *k = c->phrases->r.key[i], *d = strrchr(k, '.');
        push_unique(a, groups, d ? v_text(a, k, (size_t)(d - k)) : T(a, k));
      }
      for (size_t i = 0; i < groups->l.n; i++) { if (i) buf_str(&b, ", "); buf_str(&b, groups->l.v[i]->t.s); }
    }
    char *m = buf_dup(a, &b);
    buf_free(&b);
    return problem(c, line, m);
  }
  val *ps_ = rec_get(ph, "params");
  size_t len = ps_ ? ps_->l.n : 0, required = 0;
  for (size_t i = 0; i < len; i++) if (!rec_get(ps_->l.v[i], "default")) required++;
  if (args->l.n < required || args->l.n > len) {
    buf b = { 0 };
    buf_fmt(&b, "%s(", id);
    for (size_t i = 0; i < len; i++) {
      val *d = rec_get(ps_->l.v[i], "default");
      if (i) buf_str(&b, ", ");
      buf_str(&b, rec_get(ps_->l.v[i], "name")->t.s);
      if (d) { buf_chr(&b, '='); buf_json(&b, d); }
    }
    if (required == len) buf_fmt(&b, ") takes %zu values, not %zu", required, args->l.n);
    else buf_fmt(&b, ") takes %zu to %zu values, not %zu", required, len, args->l.n);
    char *m = buf_dup(a, &b);
    buf_free(&b);
    return problem(c, line, m);
  }
  val *named = v_rec(a);
  for (size_t i = 0; i < args->l.n; i++) rec_set(a, named, rec_get(ps_->l.v[i], "name")->t.s, args->l.v[i]);
  return mk(a, "say", 2, T(a, id), named);
}

/* lingo("what it does", {name: value}, "what it gives") is a call of lingo.run, made as the program reaches it: Onsay
   answers it, and the program gets only the answer back. */
static val *wanted(cx *c, val *args, int line) {
  arena *a = c->a;
  val *ask = at(args, 0), *with = at(args, 1), *gives = at(args, 2), *named = v_rec(a);
  if (!ask) return problem(c, line, "lingo(...) says what it needs first, like lingo(\"the weather in a city\", {city: \"Paris\"})");
  if (args->l.n > 3) return problem(c, line, "lingo(...) takes what it needs, its values and what it gives back, and no more");
  if (ask->k == V_TEXT && !ask->t.len) return problem(c, line, "lingo(...) says what it needs in words, like lingo(\"the weather in a city\", {city: \"Paris\"})");
  rec_set(a, named, "ask", ask);
  rec_set(a, named, "values", with ? with : mk(a, "map", 1, v_rec(a)));
  rec_set(a, named, "gives", gives ? gives : T(a, ""));
  return mk(a, "say", 2, T(a, "lingo.run"), named);
}

/* setting("key") is a call of lingo.setting, made as the program reaches it: this computer answers with this lingo's
   setting of that name, kept here, or its default, or nothing (6. in core.c). setting("key", "github") reads another
   lingo's, so one token serves many lingos. */
static val *setting(cx *c, val *args, int line) {
  arena *a = c->a;
  val *key = at(args, 0), *ref = at(args, 1), *named = v_rec(a);
  if (!key) return problem(c, line, "setting(...) names the setting first, like setting(\"key\")");
  if (args->l.n > 2) return problem(c, line, "setting(...) takes a setting's name and whose it is, and no more");
  if (key->k == V_TEXT && (!ref || is_text(ref, c->own))) push_unique(a, c->settings, key);
  rec_set(a, named, "key", key);
  rec_set(a, named, "reference", ref ? ref : T(a, c->own));
  return mk(a, "say", 2, T(a, "lingo.setting"), named);
}

// {name:"Rex", "two words":1}
static val *record(ps *p) {
  arena *a = p->c->a;
  val *parts = v_rec(a);
  if (accept(p, "}")) return mk(a, "map", 1, parts);
  do {
    if (is(p, "}", 0)) break;
    token *t = peek(p, 0);
    const char *key;
    if (t && (t->k == ID || t->k == KW)) key = t->v;
    else if (t && t->k == STR && t->count == 1 && !t->parts[0].code) key = t->parts[0].s;
    else return expected(p, "the name of a part");
    p->i++;
    if (!expect(p, ":", NULL)) return NULL;
    GET(e, expr(p));
    rec_set(a, parts, key, e);
  } while (accept(p, ","));
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
    if (!part->code) e = v_text(a, part->s, part->n);
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
  if (t->k == NUM) { p->i++; return v_num(a, t->num); }
  if (t->k == STR) { p->i++; return text(p, t); }
  if (accept(p, "true")) return VBOOL(1);
  if (accept(p, "false")) return VBOOL(0);
  if (accept(p, "nothing") || accept(p, "null")) return NOTHING;
  if (accept(p, "(")) { GET(e, expr(p)); if (!expect(p, ")", NULL)) return NULL; return e; }
  if (accept(p, "[")) {
    GET(xs, items(p, "]"));
    val *out = mk(a, "list", 0);
    for (size_t i = 0; i < xs->l.n; i++) list_push(a, out, xs->l.v[i]);
    return out;
  }
  if (accept(p, "{")) return record(p);
  if (t->k != ID) return expected(p, "a value");

  // a.b.c(...) is a phrase, f(...) is a function, and anything else is a name, with its parts read by postfix.
  // After a dot any word is a name, keywords too, so a phrase can be called dev.fail or a part in.
  size_t j = p->i + 1;
  while (j + 1 < p->n && p->t[j].k == SYM && !strcmp(p->t[j].v, ".") && (p->t[j + 1].k == ID || p->t[j + 1].k == KW)) j += 2;
  int dotted = j > p->i + 1;
  if (j < p->n && p->t[j].k == SYM && !strcmp(p->t[j].v, "(")) {
    int line = t->line;
    buf b = { 0 };
    for (size_t k = p->i; k < j; k += 2) { if (k > p->i) buf_chr(&b, '.'); buf_str(&b, p->t[k].v); }
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
    if (accept(p, ".")) { GET(w, word(p, "the name of a part after .")); e = mk(p->c->a, "get", 2, e, w); }
    else if (accept(p, "[")) { GET(k, expr(p)); if (!expect(p, "]", NULL)) return NULL; e = mk(p->c->a, "get", 2, e, k); }
    else return e;
  }
}

// Loosest first: pick, ??, or, and, not, comparing, ranges, adding, multiplying, minus, then calls, parts and items.
static val *unary(ps *p) {
  if (accept(p, "-")) { GET(e, unary(p)); return mk(p->c->a, "neg", 1, e); }
  return postfix(p);
}

static val *binary(ps *p, int level);

static val *product(ps *p) {
  GET(e, unary(p));
  for (;;) {
    const char *o = accept(p, "*") ? "*" : accept(p, "/") ? "/" : accept(p, "%") ? "%" : NULL;
    if (!o) return e;
    GET(b, unary(p));
    e = mk(p->c->a, "op", 3, T(p->c->a, o), e, b);
  }
}

static val *sum(ps *p) {
  GET(e, product(p));
  for (;;) {
    const char *o = accept(p, "+") ? "+" : accept(p, "-") ? "-" : NULL;
    if (!o) return e;
    GET(b, product(p));
    e = mk(p->c->a, "op", 3, T(p->c->a, o), e, b);
  }
}

static val *range(ps *p) {
  GET(e, sum(p));
  if (!accept(p, "..")) return e;
  GET(b, sum(p));
  return mk(p->c->a, "range", 2, e, b);
}

static val *compare(ps *p) {
  static const char *ops[] = { "==", "!=", "<=", ">=", "<", ">", "in" };
  GET(e, range(p));
  for (int k = 0; k < 7; k++) {
    if (!accept(p, ops[k])) continue;
    GET(b, range(p));
    return mk(p->c->a, "op", 3, T(p->c->a, ops[k]), e, b);
  }
  return e;
}

static val *not_(ps *p) {
  if (accept(p, "not") || accept(p, "!")) { GET(e, not_(p)); return mk(p->c->a, "not", 1, e); }
  return compare(p);
}

// and, or, then ??: each a run of the one below it.
static val *binary(ps *p, int level) {
  static const char *tags[] = { "and", "or", "else" };
  static const char *words[][2] = { { "and", "&&" }, { "or", "||" }, { "??", NULL } };
  GET(e, level ? binary(p, level - 1) : not_(p));
  while (accept(p, words[level][0]) || (words[level][1] && accept(p, words[level][1]))) {
    GET(b, level ? binary(p, level - 1) : not_(p));
    e = mk(p->c->a, tags[level], 2, e, b);
  }
  return e;
}

static val *expr(ps *p) {
  GET(c, binary(p, 2));
  if (!accept(p, "?")) return c;
  GET(a, expr(p));
  if (!expect(p, ":", "a : between the two choices")) return NULL;
  GET(b, expr(p));
  return mk(p->c->a, "pick", 3, c, a, b);
}

// tool and fn may come in any order. Steps with no tool around them are a one-off tool of their own.
static val *program(ps *p) {
  cx *c = p->c;
  arena *a = c->a;
  val *fns = v_rec(a), *loose = v_list(a), *tool = NULL;
  while (!end(p)) {
    if (accept(p, ";")) continue;
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
      if (rec_get(fns, n->t.s)) return problem(c, line_now(p), fmt(a, "There are two functions called %s", n->t.s));
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
      for (size_t j = 0; j < branches->l.n; j++) { path->n = here; buf_fmt(path, "/%zu", j); walk(c, map, branches->l.v[j]->l.v[1], path); }
      path->n = here;
      buf_fmt(path, "/%zu", branches->l.n);
      walk(c, map, s->l.v[2], path);
    } else if (tagged(s, "while")) { buf_str(path, "/0"); walk(c, map, s->l.v[2], path); }
    else if (tagged(s, "each")) { buf_str(path, "/0"); walk(c, map, s->l.v[3], path); }
    else if (tagged(s, "try")) {
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
  val *map = v_rec(c->a), *fns = rec_get(program, "fns");
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
  val *fns = rec_get(prog, "fns"), *by_name = v_rec(a);
  for (size_t i = 0; i < c->phrases->r.n; i++) {
    const char *id = c->phrases->r.key[i], *d = strrchr(id, '.');
    const char *bare_name = d ? d + 1 : id;
    val *ids = rec_get(by_name, bare_name);
    if (!ids) { ids = v_list(a); rec_set(a, by_name, bare_name, ids); }
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
          const char *id = ids->l.v[k]->t.s, *d = strrchr(id, '.');
          buf_fmt(&b, "%s%.*s", k ? " and " : "", (int)(d - id), id);
        }
        buf_str(&b, ": write ");
        for (size_t k = 0; k < ids->l.n; k++) buf_fmt(&b, "%s%s", k ? " or " : "", ids->l.v[k]->t.s);
        char *m = buf_dup(a, &b);
        buf_free(&b);
        return problem(c, s->line, m);
      }
      if (!ids) return problem(c, s->line, fmt(a, "There's no function or phrase called %s. Write it with fn %s(...){...}", s->name, s->name));
      GET(said, phrase_node(c, ids->l.v[0]->t.s, s->node->l.v[2], s->line));
      for (size_t k = 0; k < 3; k++) s->node->l.v[k] = said->l.v[k];
      continue;
    }
    val *fps = rec_get(fn, "params");
    int required = 0, len = (int)fps->l.n;
    for (int k = 0; k < len; k++) if (fps->l.v[k]->l.n == 1) required++;
    if (s->count < required || s->count > len) {
      if (required == len) return problem(c, s->line, fmt(a, "%s takes %d values, not %d", s->name, required, s->count));
      return problem(c, s->line, fmt(a, "%s takes %d to %d values, not %d", s->name, required, len, s->count));
    }
  }
  rec_set(a, prog, "lines", paths(c, prog));
  return prog;
}

////////// 3. LISTING //////////

typedef struct { const char *s; size_t n; } span;

// One line of code without the spaces around it.
static span trimmed(span l) {
  while (l.n && (*l.s == ' ' || *l.s == '\t')) l.s++, l.n--;
  while (l.n && (l.s[l.n - 1] == ' ' || l.s[l.n - 1] == '\t' || l.s[l.n - 1] == '\r')) l.n--;
  return l;
}

// A line's words as a comment, without its //: 0 when the line isn't only a comment, is an empty //, or is a section's bar.
static int comment(span l, span *words) {
  l = trimmed(l);
  if (l.n < 2 || memcmp(l.s, "//", 2) || (l.n >= 10 && !memcmp(l.s, "//////////", 10))) return 0;
  *words = trimmed((span){ l.s + 2, l.n - 2 });
  return words->n > 0;
}

// The words of the comment lines from first to last, joined by spaces, with a number like 1. or 1-6. at the start, a
// legend's, left off.
static val *words_of(arena *a, span *lines, size_t first, size_t last) {
  buf b = { 0 };
  span w;
  for (size_t i = first; i <= last; i++) if (comment(lines[i], &w)) { if (b.n) buf_chr(&b, ' '); buf_add(&b, w.s, w.n); }
  size_t k = 0;
  while (k < b.n && digit(b.s[k])) k++;
  if (k && k < b.n && b.s[k] == '-') { size_t m = k + 1; while (m < b.n && digit(b.s[m])) m++; if (m > k + 1) k = m; }
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
  size_t fl = strlen(file), n = fl > 3 && !strcmp(file + fl - 3, ".ls") ? fl - 3 : 0;
  int good = n && n <= 64;   // lower case letters, digits, _ and -, neither of those two first or last
  for (size_t i = 0; good && i < n; i++) good = (file[i] >= 'a' && file[i] <= 'z') || digit(file[i]) || ((file[i] == '_' || file[i] == '-') && i && i + 1 < n);
  if (!good) return failed(why, fmt(a, "%s: a lingo's code is named for it, like notes.ls, its name lower case letters, digits, _ and -", file));
  val *name = v_text(a, file, n);
  if (is_text(name, "lingo")) return failed(why, fmt(a, "%s: lingo is the language's own name, for lingo(...)", file));
  cx *c = context(a, NULL);
  c->own = name->t.s;
  c->scan = 1;
  val *read = parse_with(c, code->t.s, code->t.len);
  if (!read) return failed(why, fmt(a, "%s line %d: %s", file, c->line, c->message));
  val *tool = rec_get(read, "tool"), *fns = rec_get(read, "fns");
  if (!is_text(rec_get(tool, "name"), "steps") || rec_get(tool, "body")->l.n) return failed(why, fmt(a, "%s: only fn blocks belong in it, with no tool and no steps outside them", file));
  if (!fns->r.n) return failed(why, fmt(a, "%s: a lingo shares its fn blocks, and it has none", file));

  size_t count = 1;
  for (size_t i = 0; i < code->t.len; i++) if (code->t.s[i] == '\n') count++;
  span *lines = arena_get(a, count * sizeof *lines), w;
  for (size_t i = 0, from = 0, l = 0; i <= code->t.len; i++)
    if (i == code->t.len || code->t.s[i] == '\n') { lines[l++] = (span){ code->t.s + from, i - from }; from = i + 1; }
  size_t top = 0, end;
  while (top < count && !trimmed(lines[top]).n) top++;
  for (end = top; end < count && comment(lines[end], &w);) end++;
  val *out = v_rec(a), *phrases = v_list(a);
  rec_set(a, out, "name", name);
  if (end > top) {
    const char *s = words_of(a, lines, top, end - 1)->t.s, *lead[] = { fmt(a, "%s - ", file), fmt(a, "%s - ", name->t.s) };
    for (int k = 0; k < 2; k++) if (!strncmp(s, lead[k], strlen(lead[k]))) s += strlen(lead[k]);
    rec_set(a, out, "about", T(a, s));
  }
  for (size_t i = 0; i < fns->r.n; i++) {
    const char *fname = fns->r.key[i];
    size_t here = (size_t)rec_get(c->fn_lines, fname)->n - 1, first = here;
    while (first > 0 && comment(lines[first - 1], &w)) first--;
    val *given = rec_get(fns->r.v[i], "params"), *params = v_list(a), *ph = v_rec(a);
    for (size_t k = 0; k < given->l.n; k++) {
      // A default worked out as it runs, like o={a: 1}, is nothing in its listing, and -2 is its number.
      val *p = v_rec(a), *d = at(given->l.v[k], 1);
      if (tagged(d, "neg") && at(d, 1)->k == V_NUM) d = v_num(a, -at(d, 1)->n);
      else if (d && d->k == V_LIST) d = NOTHING;
      rec_set(a, p, "name", given->l.v[k]->l.v[0]);
      if (d) rec_set(a, p, "default", d);
      list_push(a, params, p);
    }
    rec_set(a, ph, "id", T(a, fmt(a, "%s.%s", name->t.s, fname)));
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
    val *parts = e->l.v[1], *out = v_rec(a);
    for (size_t i = 0; i < parts->r.n; i++) rec_set(a, out, parts->r.key[i], rw_ex(r, parts->r.v[i]));
    return mk(a, "map", 1, out);
  }
  if (tagged(e, "op")) return mk(a, "op", 3, e->l.v[1], rw_ex(r, e->l.v[2]), rw_ex(r, e->l.v[3]));
  if (tagged(e, "call")) {
    val *args = v_list(a), *given = e->l.v[2];
    for (size_t i = 0; i < given->l.n; i++) list_push(a, args, rw_ex(r, given->l.v[i]));
    val *swap = r->call ? r->call(r, e->l.v[1], args) : NULL;
    return swap ? swap : mk(a, "call", 2, e->l.v[1], args);
  }
  if (tagged(e, "say")) {
    val *args = v_rec(a), *given = e->l.v[2];
    for (size_t i = 0; i < given->r.n; i++) rec_set(a, args, given->r.key[i], rw_ex(r, given->r.v[i]));
    val *swap = r->say ? r->say(r, e->l.v[1], args) : NULL;
    return swap ? swap : mk(a, "say", 2, e->l.v[1], args);
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
    val *s = b->l.v[i], *x;
    if (tagged(s, "set")) x = mk(a, "set", 2, s->l.v[1], rw_ex(r, s->l.v[2]));
    else if (tagged(s, "put")) x = mk(a, "put", 3, rw_ex(r, s->l.v[1]), rw_ex(r, s->l.v[2]), rw_ex(r, s->l.v[3]));
    else if (tagged(s, "if")) {
      val *branches = v_list(a);
      for (size_t j = 0; j < s->l.v[1]->l.n; j++) {
        val *br = s->l.v[1]->l.v[j], *nb = v_list(a);
        list_push(a, nb, rw_ex(r, br->l.v[0]));
        list_push(a, nb, rw_body(r, br->l.v[1]));
        list_push(a, branches, nb);
      }
      x = mk(a, "if", 2, branches, rw_body(r, s->l.v[2]));
    } else if (tagged(s, "while")) x = mk(a, "while", 2, rw_ex(r, s->l.v[1]), rw_body(r, s->l.v[2]));
    else if (tagged(s, "each")) {
      x = mk(a, "each", 3, s->l.v[1], rw_ex(r, s->l.v[2]), rw_body(r, s->l.v[3]));
      for (size_t k = 4; k < s->l.n; k++) list_push(a, x, s->l.v[k]);
    } else if (tagged(s, "try")) x = mk(a, "try", 3, rw_body(r, s->l.v[1]), s->l.v[2], rw_body(r, s->l.v[3]));
    else if (tagged(s, "return") || tagged(s, "fail") || tagged(s, "do")) x = mk(a, s->l.v[0]->t.s, 1, rw_ex(r, s->l.v[1]));
    else x = s;   // break, continue
    list_push(a, out, x);
  }
  return out;
}

static val *rw_fn(rw *r, val *f) {
  arena *a = r->a;
  val *ps_ = v_list(a), *given = rec_get(f, "params"), *out = v_rec(a);
  for (size_t i = 0; i < given->l.n; i++) {
    val *p = given->l.v[i], *np = v_list(a);
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
static val *named(arena *a, val *read, val *names, const char *file) {
  val *fns = rec_get(read, "fns"), *out_fns = v_rec(a), *out_lines = v_rec(a);
  rw r = { a, NULL, renamed, names };
  for (size_t i = 0; i < fns->r.n; i++) rec_set(a, out_fns, rec_get(names, fns->r.key[i])->t.s, rw_fn(&r, fns->r.v[i]));
  val *lines = rec_get(read, "lines");
  for (size_t i = 0; i < lines->r.n; i++) {
    const char *path = lines->r.key[i];
    if (strncmp(path, "fn:", 3)) continue;
    const char *slash = strchr(path + 3, '/');
    if (slash == path + 3) continue;
    size_t nlen = slash ? (size_t)(slash - path - 3) : strlen(path + 3);
    val *to = rec_get(names, dup(a, path + 3, nlen));
    rec_set(a, out_lines, fmt(a, "fn:%s%s", to->t.s, slash ? slash : ""), T(a, fmt(a, "%s line %g", file, lines->r.v[i]->n)));
  }
  val *out = v_rec(a);
  rec_set(a, out, "fns", out_fns);
  rec_set(a, out, "lines", out_lines);
  return out;
}

// A Lingo lingo's code compiled against what it may call, every fn under its lasting name, like memo.recall, with its
// lines as places in its file, for errors at run time.
static val *compile_code(arena *a, val *lingo, val *visible, const char **message) {
  const char *file = rec_get(lingo, "file")->t.s, *lname = rec_get(lingo, "name")->t.s;
  val *code = rec_get(lingo, "code");
  cx *c = context(a, visible);
  c->own = lname;
  val *read = parse_with(c, code->t.s, code->t.len);
  if (!read) { *message = fmt(a, "%s line %d: %s", file, c->line, c->message); return NULL; }
  val *fns = rec_get(read, "fns"), *names = v_rec(a);
  for (size_t i = 0; i < fns->r.n; i++) rec_set(a, names, fns->r.key[i], T(a, fmt(a, "%s.%s", lname, fns->r.key[i])));
  return named(a, read, names, file);
}

// stack is {phrases: id -> params, fns, lines}. Each stacked phrase call becomes a call to its body, values in order and
// nothing for any left out so its defaults apply, and only the functions something reaches are pulled in.
typedef struct { val *stack, *want; } linking;

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
  val *lingos = rec_get(inst, "lingos"), *stack = rec_get(inst, "stack"), *own = NULL;
  for (size_t i = 0; i < lingos->r.n && !own; i++) {
    val *ps = rec_get(lingos->r.v[i], "phrases");
    for (size_t k = 0; k < ps->l.n && !own; k++) if (is_text(rec_get(ps->l.v[k], "id"), fn)) own = ps->l.v[k];
  }
  if (!own) return NULL;
  val *given = rec_get(own, "params"), *params = v_list(a), *args = v_rec(a), *step = v_list(a), *tool = v_rec(a);
  for (size_t i = 0; i < given->l.n; i++) {
    val *name = T(a, fmt(a, "a%zu", i)), *p = v_list(a);
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
  val *fns = v_rec(a), *lines = v_rec(a), *sfns = rec_get(stack, "fns"), *slines = rec_get(stack, "lines"), *out = v_rec(a);
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
      if (!strncmp(path, "fn:", 3) && !strncmp(path + 3, k, kn) && path[3 + kn] == '/') rec_set(a, lines, path, slines->r.v[i]);
    }
  }
  rec_set(a, out, "tool", tool);
  rec_set(a, out, "fns", fns);
  rec_set(a, out, "lines", lines);
  return out;
}

////////// 5. LOADING //////////

// lingos: its lingo(...) calls. needs: what the native phrases it reaches need, from native, each phrase's by its id.
typedef struct { val *fns, *seen, *native, *needs; double lingos; } reaching;

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
  val *lingos = v_rec(a), *stacked_list = v_list(a);
  for (size_t i = 0; i < natives->l.n; i++) {
    val *l = natives->l.v[i];
    rec_set(a, l, "native", VBOOL(1));
    rec_set(a, lingos, rec_get(l, "name")->t.s, l);
  }
  for (size_t i = 0; i < stacked->l.n; i++) {
    val *file = rec_get(stacked->l.v[i], "file");
    val *l = listing(a, file->t.s, rec_get(stacked->l.v[i], "code"), message), *name = l ? rec_get(l, "name") : NULL;
    if (!l) return NULL;
    // Never over one of the computer's native lingos or their groups, like windows' input, which it would hide.
    for (size_t k = 0; k < natives->l.n; k++) {
      val *n = natives->l.v[k], *ps = rec_get(n, "phrases");
      int taken = is_text(rec_get(n, "name"), name->t.s);
      for (size_t m = 0; m < ps->l.n && !taken; m++) {
        const char *id = rec_get(ps->l.v[m], "id")->t.s;
        taken = strlen(id) > name->t.len && !strncmp(id, name->t.s, name->t.len) && id[name->t.len] == '.';
      }
      if (taken) { *message = fmt(a, "%s: %s is already a part of %s, this computer's own lingo", file->t.s, name->t.s, rec_get(n, "name")->t.s); return NULL; }
    }
    rec_set(a, l, "file", file);
    rec_set(a, l, "code", rec_get(stacked->l.v[i], "code"));
    list_push(a, stacked_list, l);
    rec_set(a, lingos, name->t.s, l);
  }

  // Each is compiled against every phrase here: the native lingos', core's among them, and every Lingo lingo's in this
  // lingos folder, so one is called by its dotted name, like text.slug(title).
  val *stack = v_rec(a), *sphrases = v_rec(a), *sfns = v_rec(a), *slines = v_rec(a), *visible = v_list(a), *native = v_rec(a);
  rec_set(a, stack, "phrases", sphrases);
  rec_set(a, stack, "fns", sfns);
  rec_set(a, stack, "lines", slines);
  for (size_t i = 0; i < lingos->r.n; i++) {
    val *theirs = rec_get(lingos->r.v[i], "phrases");
    for (size_t k = 0; k < theirs->l.n; k++) {
      // Its values as an inputSchema, those with no default required.
      val *p = theirs->l.v[k], *params = rec_get(p, "params"), *schema = v_rec(a), *props = v_rec(a), *required = v_list(a);
      if (rec_get(p, "needs")) rec_set(a, native, rec_get(p, "id")->t.s, rec_get(p, "needs"));
      for (size_t m = 0; m < params->l.n; m++) {
        val *one = v_rec(a), *d = rec_get(params->l.v[m], "default"), *pn = rec_get(params->l.v[m], "name");
        if (d) rec_set(a, one, "default", d); else list_push(a, required, pn);
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
    val *l = stacked_list->l.v[i], *own = rec_get(l, "phrases");
    val *done = compile_code(a, l, visible, message);
    if (!done) return NULL;
    val *fns = rec_get(done, "fns"), *lines = rec_get(done, "lines");
    for (size_t k = 0; k < fns->r.n; k++) rec_set(a, sfns, fns->r.key[k], fns->r.v[k]);
    for (size_t k = 0; k < lines->r.n; k++) rec_set(a, slines, lines->r.key[k], lines->r.v[k]);
    for (size_t k = 0; k < own->l.n; k++) rec_set(a, sphrases, rec_get(own->l.v[k], "id")->t.s, rec_get(own->l.v[k], "params"));
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

