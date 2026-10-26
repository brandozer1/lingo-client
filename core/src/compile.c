// compile.c - Lingo code to the tree the runner runs, the same C on every device, native or as WebAssembly.
//
// 1. lex(c, s, n, out)                      -> text to tokens
// 2. lingo_parse(a, code, n, phrases, own, ...) -> tokens to a tree, checked against the phrases it may use. own: the
//                                              name of the lingo it is, like openrouter, or NULL for a program
// 3. lingo_manifest(a, text, n, message)    -> a native lingo's manifest read: its phrases, groups and shapes, each phrase
//                                              with its inputSchema and outputSchema too
// 4. lingo_read(a, manifest, folder, why)   -> a native or host lingo's lingo.json read to the same record, its functions'
//                                              heads and gives from their schemas: lingo_schema and lingo_gives, the two
//                                              ways, and lingo_line(b, phrase, group_needs)
//    listing(a, file, code, why)            -> a lingo written in Lingo, its listing made from its code: every
//                                              fn, the comments above each, its about, and the settings it reads
// 5. lingo_link(a, program, stack)          -> Lingo lingos' phrases swapped for their code, until only native ones are left
// 6. lingo_load, lingo_face, lingo_prepare  -> every lingo a device has, what Onsay is shown, and code made ready to run
// 7. lingo_request(json, n, out)            -> the whole compiler behind one JSON call
// It answers word for word as test/reference/code.mjs does (test/compiler.mjs checks). Errors are passed back up, never
// jumped, so it builds for any target: every parse function gives NULL and the first problem is kept in the context.
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
static const char *PERMISSIONS[] = { "none", "screen.read", "screen.act", "files.read", "files.write", "run", "net", NULL };

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

static int listed(const char **words, const char *s, size_t n) {
  for (int i = 0; words[i]; i++) if (strlen(words[i]) == n && !memcmp(words[i], s, n)) return 1;
  return 0;
}

static int in_list(val *list, val *text) {
  for (size_t i = 0; i < list->l.n; i++) if (!strcmp(list->l.v[i]->t.s, text->t.s)) return 1;
  return 0;
}

static void push_unique(arena *a, val *list, val *text) { if (!in_list(list, text)) list_push(a, list, text); }

static void join_needs(buf *b, val *needs) {
  if (!needs->l.n) { buf_str(b, "none"); return; }
  for (size_t i = 0; i < needs->l.n; i++) buf_fmt(b, "%s%s", i ? ", " : "", needs->l.v[i]->t.s);
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
  val *used;             // ids of the phrases it does use, in order
  const char *own;       // the name of the lingo whose code it is, like openrouter, for setting(...), or NULL
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
  c->used = v_list(a);
  c->settings = v_list(a);
  c->fn_lines = v_rec(a);
  if (phrases && phrases->k == V_LIST)
    for (size_t i = 0; i < phrases->l.n; i++) {
      val *id = rec_get(phrases->l.v[i], "id");
      if (id && id->k == V_TEXT) rec_set(a, c->phrases, id->t.s, phrases->l.v[i]);
    }
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
      add_token(a, out, (token){ .k = listed(KEYWORDS, s + i, j - i) ? KW : ID, .line = line, .v = dup(a, s + i, j - i) });
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

static int word_char(char c) { return alpha(c) || digit(c); }

// A name a program can write before a dot: letters, digits and _, not starting with a digit, and not a word of Lingo.
static int plain_name(const char *s) {
  if (!s || !alpha(*s)) return 0;
  for (const char *p = s; *p; p++) if (!alpha(*p) && !digit(*p)) return 0;
  return !listed(KEYWORDS, s, strlen(s));
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
   and gives back its answer. A lingo at an address, like laptop.brandon/lingos/test.lingo/find, runs there with the
   values, a record by name or a list in order, an MCP tool after a #, any other web address is a plain web request, and
   after an @, where the answer goes. Everything local needs no use: a lingo in the same lingos folder is a dotted call. */
static val *using(cx *c, val *args, int line) {
  arena *a = c->a;
  val *address = at(args, 0), *named = v_rec(a);
  if (!address) return problem(c, line, "use(...) says what it reaches first, like use(\"laptop.brandon/lingos/weather.lingo/now\", {place: \"Paris\"})");
  if (args->l.n > 2) return problem(c, line, "use(...) takes an address and its values, and no more");
  if (address->k == V_TEXT && !address->t.len) return problem(c, line, "use(...) names what it reaches, like use(\"laptop.brandon/lingos/weather.lingo/now\", {place: \"Paris\"})");
  rec_set(a, named, "address", address);
  rec_set(a, named, "values", args->l.n > 1 ? args->l.v[1] : NOTHING);
  push_unique(a, c->used, T(a, "lingo.use"));
  return mk(a, "say", 2, T(a, "lingo.use"), named);
}

// Values in the order the manifest lists them, checked against it, then named for the lingo. While only reading a lingo
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
      return problem(c, line, fmt(a, "There's no lingo or group called %.*s here. To reach one somewhere else, write use(\"its address\"), like use(\"laptop.brandon/lingos/%.*s.lingo/%s\")",
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
  val *to = rec_get(ph, "to");   // one seen under a short name is said by its own
  if (to) id = to->t.s;
  push_unique(a, c->used, T(a, id));
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
  push_unique(a, c->used, T(a, "lingo.run"));
  return mk(a, "say", 2, T(a, "lingo.run"), named);
}

/* setting("key") is a call of lingo.setting, made as the program reaches it: the device answers with this lingo's setting
   of that name, kept there, or its default, or nothing (6. in core.c). setting("key", "github") reads another lingo's,
   so one token serves many lingos. A program that is no lingo always says whose. */
static val *setting(cx *c, val *args, int line) {
  arena *a = c->a;
  val *key = at(args, 0), *ref = at(args, 1), *named = v_rec(a);
  if (!key) return problem(c, line, "setting(...) names the setting first, like setting(\"key\")");
  if (args->l.n > 2) return problem(c, line, "setting(...) takes a setting's name and whose it is, and no more");
  if (!ref && !c->own) return problem(c, line, "setting(...) outside a lingo says whose setting it is too, like setting(\"key\", \"openrouter\")");
  if (key->k == V_TEXT && (!ref || (c->own && is_text(ref, c->own)))) push_unique(a, c->settings, key);
  rec_set(a, named, "key", key);
  rec_set(a, named, "reference", ref ? ref : T(a, c->own));
  push_unique(a, c->used, T(a, "lingo.setting"));
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
  rec_set(a, out, "lingo", v_num(a, 1));
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
  rec_set(a, prog, "needs", c->used);
  rec_set(a, prog, "lines", paths(c, prog));
  return prog;
}

// phrases is every phrase the program may use: [{id, params: [{name, default?}]}]. Gives the program, ready to send, or
// NULL with the first problem's line and message, in words.
// 2.
val *lingo_parse(arena *a, const char *code, size_t n, val *phrases, const char *own, int *line, const char **message) {
  cx *c = context(a, phrases);
  c->own = own;
  val *out = parse_with(c, code, n);
  if (!out) { *line = c->line; *message = c->message; }
  return out;
}

////////// 3. MANIFEST //////////

/* A native lingo's manifest, as phrase.c writes it, one line each, laid out the way the phrases are called:
     lingo windows 1.0, Lingo 1.0, uses core 0.1    its name, its version, the Lingo it speaks, and the lingos it is built on
     file = {name,path,size,folder,changed}         a record shape, named once and used in what phrases give
     fs: files.read                                 a group, and the permission its phrases need. , pure marks them pure
     write(path, text) [files.write]: replaces it   a phrase: its signature, -> what it gives, a [permission] of its own,
                                                    and : what it does. Only the signature is required
   // starts a comment line. Blank lines are ignored. */

// One phrase line's signature, read with this same parser, so a manifest is written in Lingo's own syntax. Defaults must
// be plain values, because a manifest is data, not code.
static val *signature(arena *a, const char *s, size_t n, const char **message) {
  cx *c = context(a, NULL);
  tokens ts;
  if (!lex(c, s, n, &ts)) { *message = c->message; return NULL; }
  ps p = { c, ts.t, ts.n, 0 };
  val *nm = word(&p, "the name of the phrase"), *raw = nm ? params(&p) : NULL;
  if (!raw) { *message = c->message; return NULL; }
  val *list = v_list(a);
  for (size_t i = 0; i < raw->l.n; i++) {
    val *one = v_rec(a), *d = at(raw->l.v[i], 1);
    rec_set(a, one, "name", raw->l.v[i]->l.v[0]);
    if (d) {
      if (tagged(d, "neg") && at(d, 1)->k == V_NUM) d = v_num(a, -at(d, 1)->n);
      if (d->k == V_LIST) {
        *message = fmt(a, "the default for %s must be a plain value, like 10, \"*\" or nothing", raw->l.v[i]->l.v[0]->t.s);
        return NULL;
      }
      rec_set(a, one, "default", d);
    }
    list_push(a, list, one);
  }
  if (!end(&p)) { expected(&p, "nothing after the )"); *message = c->message; return NULL; }
  val *out = v_rec(a);
  rec_set(a, out, "name", nm);
  rec_set(a, out, "params", list);
  return out;
}

// Where the ( at col closes, wherever text holds one. -1 when it never does.
static long close_paren(const char *s, size_t n, long col) {
  int depth = 0, quoted = 0;
  for (long j = col < 0 ? 0 : col; j < (long)n; j++) {
    char c = s[j];
    if (quoted) { if (c == '\\') j++; else if (c == '"') quoted = 0; }
    else if (c == '"') quoted = 1;
    else if (c == '(') depth++;
    else if (c == ')' && --depth == 0) return j + 1;
  }
  return -1;
}

typedef struct { const char *s; size_t n; } span;

static int perm_at(const char *s, size_t n, size_t *len) {
  for (int k = 0; PERMISSIONS[k]; k++) {
    size_t m = strlen(PERMISSIONS[k]);
    if (n >= m && !memcmp(s, PERMISSIONS[k], m)) { *len = m; return 1; }
  }
  return 0;
}

/* What follows a signature: -> what it gives, [permissions] and : what it does, each one optional and in that order.
   Read the way a regular expression with lazy parts reads it, so the shortest gives wins. */
typedef struct { const char *gives, *needs, *does; size_t gn, nn, dn; } tail;

static int tail_does(const char *s, size_t n, tail *t) {
  t->does = n > 2 && !memcmp(s, ": ", 2) ? s + 2 : NULL;
  t->dn = t->does ? n - 2 : 0;
  return t->does || !n;
}

static int tail_needs(const char *s, size_t n, tail *t) {
  if (n > 2 && !memcmp(s, " [", 2)) {
    size_t i = 2, len;
    if (perm_at(s + i, n - i, &len)) {
      i += len;
      for (;;) {
        if (i < n && s[i] == ']') {
          if (tail_does(s + i + 1, n - i - 1, t)) { t->needs = s + 2; t->nn = i - 2; return 1; }
          break;
        }
        if (i < n && s[i] == ',') {
          size_t j = i + 1;
          if (j < n && s[j] == ' ') j++;
          if (!perm_at(s + j, n - j, &len)) break;
          i = j + len;
          continue;
        }
        break;
      }
    }
  }
  t->needs = NULL;
  return tail_does(s, n, t);
}

static int tail_read(const char *s, size_t n, tail *t) {
  memset(t, 0, sizeof *t);
  if (n > 4 && !memcmp(s, " -> ", 4))
    for (size_t k = 1; k + 4 <= n; k++)
      if (tail_needs(s + 4 + k, n - 4 - k, t)) { t->gives = s + 4; t->gn = k; return 1; }
  t->gives = NULL;
  return tail_needs(s, n, t);
}

static val *split_needs(arena *a, const char *s, size_t n) {
  val *out = v_list(a);
  size_t from = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || s[i] == ',') {
      list_push(a, out, v_text(a, s + from, i - from));
      from = i + 1;
      if (from < n && s[from] == ' ') from++, i++;
    }
  }
  return out;
}

// The words of a group line, like none, pure: a to z and dots, between ", ". NULL when it isn't one.
static val *group_words(arena *a, const char *w, size_t n) {
  val *out = v_list(a);
  size_t from = 0;
  for (size_t m = 0; m <= n; m++) {
    if (m == n || w[m] == ',') {
      if (m == from) return NULL;
      list_push(a, out, v_text(a, w + from, m - from));
      if (m < n) { if (m + 1 >= n || w[m + 1] != ' ') return NULL; m++; }
      from = m + 1;
    } else if (!((w[m] >= 'a' && w[m] <= 'z') || w[m] == '.')) return NULL;
  }
  return out;
}

static val *input_of(arena *a, val *params);   // 4. LINGO.JSON

// 3.
val *lingo_manifest(arena *a, const char *text, size_t n, const char **message) {
  val *out = v_rec(a), *uses = v_list(a), *shapes = v_rec(a), *groups = v_list(a), *phrases = v_list(a);
  const char *perms = "none, screen.read, screen.act, files.read, files.write, run, net";
  rec_set(a, out, "name", T(a, ""));
  rec_set(a, out, "version", T(a, ""));
  rec_set(a, out, "language", T(a, ""));
  rec_set(a, out, "uses", uses);
  rec_set(a, out, "shapes", shapes);
  rec_set(a, out, "groups", groups);
  rec_set(a, out, "phrases", phrases);

  // Lines split at \n, with a \r before it dropped.
  size_t count = 1;
  for (size_t i = 0; i < n; i++) if (text[i] == '\n') count++;
  span *lines = arena_get(a, count * sizeof *lines);
  for (size_t i = 0, from = 0, l = 0; i <= n; i++)
    if (i == n || text[i] == '\n') {
      size_t len = i - from;
      if (i < n && len && text[i - 1] == '\r') len--;
      lines[l++] = (span){ text + from, len };
      from = i + 1;
    }

  val *group = NULL;
  int named = 0;
  for (size_t i = 0; i < count; i++) {
    const char *line = lines[i].s;
    size_t ln = lines[i].n;
    while (ln && space_at(line, 0, ln)) { size_t w = space_at(line, 0, ln); line += w; ln -= w; }
    while (ln) {
      size_t k = ln - 1;
      while (k > 0 && ((unsigned char)line[k] & 0xC0) == 0x80) k--;
      if (!space_at(line, k, ln)) break;
      ln = k;
    }
    char *where = fmt(a, "line %zu", i + 1);
    if (!ln || (ln >= 2 && line[0] == '/' && line[1] == '/')) continue;

    if (!named) {
      // lingo NAME VERSION, Lingo LANGUAGE[, uses A 1.0, B 2.0]
      const char *bad = "the first line must be like: lingo notes 1.0, Lingo 1.0, uses windows 1.0";
      const char *mark = NULL;
      for (size_t k = 0; k + 8 <= ln; k++) if (!memcmp(line + k, ", Lingo ", 8)) { mark = line + k; break; }
      if (ln < 6 || memcmp(line, "lingo ", 6) || !mark) return failed(message, fmt(a, "%s: %s", where, bad));
      const char *head = line + 6, *sp = memchr(head, ' ', (size_t)(mark - head));
      if (!sp || sp == head || sp + 1 == mark || memchr(sp + 1, ' ', (size_t)(mark - sp - 1)) || memchr(head, '\t', (size_t)(mark - head)))
        return failed(message, fmt(a, "%s: %s", where, bad));
      const char *rest = mark + 8, *stop = line + ln, *use = NULL;
      for (const char *q = rest; q + 7 <= stop; q++) if (!memcmp(q, ", uses ", 7)) { use = q; break; }
      const char *lang_end = use ? use : stop;
      if (lang_end == rest || memchr(rest, ' ', (size_t)(lang_end - rest)) || (use && use + 7 == stop)) return failed(message, fmt(a, "%s: %s", where, bad));
      rec_set(a, out, "name", v_text(a, head, (size_t)(sp - head)));
      rec_set(a, out, "version", v_text(a, sp + 1, (size_t)(mark - sp - 1)));
      rec_set(a, out, "language", v_text(a, rest, (size_t)(lang_end - rest)));
      if (use) {
        const char *q = use + 7;
        while (q < stop) {
          const char *comma = memchr(q, ',', (size_t)(stop - q));
          const char *e = comma ? comma : stop, *s = q;
          while (s < e && space_at(s, 0, (size_t)(e - s))) s++;
          const char *f = e;
          while (f > s && space_at(f - 1, 0, 1)) f--;
          if (f > s) {
            const char *gap = s;
            while (gap < f && !space_at(gap, 0, (size_t)(f - gap))) gap++;
            const char *v = gap;
            while (v < f && space_at(v, 0, (size_t)(f - v))) v++;
            if (v >= f) return failed(message, fmt(a, "%s: each lingo it uses needs a version, like uses windows 1.0", where));
            const char *ve = v;
            while (ve < f && !space_at(ve, 0, (size_t)(f - ve))) ve++;
            val *u = v_rec(a);
            rec_set(a, u, "name", v_text(a, s, (size_t)(gap - s)));
            rec_set(a, u, "version", v_text(a, v, (size_t)(ve - v)));
            list_push(a, uses, u);
          }
          q = comma ? comma + 1 : stop;
        }
      }
      named = 1;
      continue;
    }

    {
      size_t k = 0;
      while (k < ln && alpha(line[k])) k++;
      // A shape: name = {...}
      if (k && ln >= k + 5 && !memcmp(line + k, " = {", 4) && line[ln - 1] == '}') {
        rec_set(a, shapes, dup(a, line, k), v_text(a, line + k + 3, ln - k - 3));
        continue;
      }
      // A group: name: permission[, permission][, pure]. Its name may have as many levels as it needs, like office.pc.memo.
      while (k && k + 1 < ln && line[k] == '.' && alpha(line[k + 1])) for (k++; k < ln && alpha(line[k]); k++) {}
      val *words = k && ln > k + 2 && line[k] == ':' && line[k + 1] == ' ' ? group_words(a, line + k + 2, ln - k - 2) : NULL;
      if (words) {
        int pure = is_text(words->l.v[words->l.n - 1], "pure");
        val *needs = v_list(a);
        for (size_t m = 0; m < words->l.n - (pure ? 1 : 0); m++) list_push(a, needs, words->l.v[m]);
        const char *wrong = NULL;
        for (size_t m = 0; m < needs->l.n; m++) if (!listed(PERMISSIONS, needs->l.v[m]->t.s, needs->l.v[m]->t.len)) { wrong = needs->l.v[m]->t.s; break; }
        if (wrong || !needs->l.n) return failed(message, fmt(a, "%s: %s is not a permission. They are %s", where, wrong ? wrong : "pure", perms));
        group = v_rec(a);
        rec_set(a, group, "name", v_text(a, line, k));
        rec_set(a, group, "needs", needs);
        rec_set(a, group, "pure", VBOOL(pure));
        list_push(a, groups, group);
        continue;
      }
    }

    // A phrase.
    if (!group) return failed(message, fmt(a, "%s: a phrase needs a group line above it, like files: files.read", where));
    const char *open = memchr(line, '(', ln);
    long close = close_paren(line, ln, open ? (long)(open - line) : -1);
    tail t;
    if (close <= 0 || !tail_read(line + close, ln - (size_t)close, &t))
      return failed(message, fmt(a, "%s: a phrase is like: name(a, b=1) -> what it gives [permission]: what it does", where));
    const char *why = NULL;
    val *sig = signature(a, line, (size_t)close, &why);
    if (!sig) return failed(message, fmt(a, "%s: %s", where, why));
    const char *gname = rec_get(group, "name")->t.s;
    val *ph = v_rec(a);
    rec_set(a, ph, "id", T(a, fmt(a, "%s.%s", gname, rec_get(sig, "name")->t.s)));
    rec_set(a, ph, "group", T(a, gname));
    rec_set(a, ph, "params", rec_get(sig, "params"));
    rec_set(a, ph, "gives", t.gives ? v_text(a, t.gives, t.gn) : T(a, "nothing"));
    rec_set(a, ph, "needs", t.needs ? split_needs(a, t.needs, t.nn) : v_copy(a, rec_get(group, "needs")));
    rec_set(a, ph, "pure", rec_get(group, "pure"));
    rec_set(a, ph, "does", t.does ? v_text(a, t.does, t.dn) : T(a, ""));
    rec_set(a, ph, "head", v_text(a, line, (size_t)close));
    rec_set(a, ph, "inputSchema", input_of(a, rec_get(sig, "params")));
    if (t.gives) rec_set(a, ph, "outputSchema", lingo_schema(a, t.gives, t.gn, shapes));
    list_push(a, phrases, ph);
  }
  return out;
}

////////// 4. LINGO.JSON //////////

/* lingo.json, a native or host lingo's manifest, read to the same record a native lingo's text is read to (see
   docs/lingo.md). A lingo written in Lingo needs none: its listing is made from its code (listing, below), and a lingo.json
   beside its code only says its settings, read by the core (core.c).
     name, version   lower case letters, digits, _ and -, and a version like 1.2.0
     runs            native: a library, its library. host: run by the program around the core
     shapes, groups  a host lingo's records and groups, as in a listing. groups: [{name, needs: [permission]}]
     functions       a host lingo's, each shaped like an MCP tool: name, group, description, inputSchema, outputSchema.
                     A native's come from its library, so it lists none
   A function's head comes from its inputSchema, its properties in order with their defaults, what it gives from its
   outputSchema, and what it does from its description. Its needs are its group's, or in onsay.needs. */

// A gives, like [{id,name}]? or window, as JSON Schema. Text, number, bool, nothing, list, record and any are its types,
// and a shape is an object titled with its name. What doesn't read as one is any.
typedef struct { arena *a; const char *s; size_t i, n; val *shapes; int bad; } gp;

static val *gp_alt(gp *g);

static int gp_at(gp *g, char c) {
  while (g->i < g->n && g->s[g->i] == ' ') g->i++;
  if (g->i < g->n && g->s[g->i] == c) { g->i++; return 1; }
  return 0;
}

static val *typed(arena *a, const char *type) { val *o = v_rec(a); rec_set(a, o, "type", T(a, type)); return o; }
static val *either(arena *a, val *list) { val *o = v_rec(a); rec_set(a, o, "anyOf", list); return o; }

static val *gp_word(gp *g) {
  while (g->i < g->n && g->s[g->i] == ' ') g->i++;
  size_t from = g->i;
  while (g->i < g->n && word_char(g->s[g->i])) g->i++;
  if (g->i == from) { g->bad = 1; return NULL; }
  return v_text(g->a, g->s + from, g->i - from);
}

static val *gp_base(gp *g) {
  static const char *TYPES[][2] = { { "text", "string" }, { "number", "number" }, { "bool", "boolean" }, { "nothing", "null" },
                                    { "list", "array" }, { "record", "object" } };
  arena *a = g->a;
  if (gp_at(g, '[')) {
    val *o = typed(a, "array"), *items = gp_alt(g);
    if (!items || !gp_at(g, ']')) { g->bad = 1; return NULL; }
    rec_set(a, o, "items", items);
    return o;
  }
  if (gp_at(g, '{')) {
    val *o = typed(a, "object"), *props = v_rec(a);
    rec_set(a, o, "properties", props);
    if (gp_at(g, '}')) return o;
    do {
      val *k = gp_word(g), *v = !k ? NULL : gp_at(g, ':') ? gp_alt(g) : v_rec(a);
      if (!v) { g->bad = 1; return NULL; }
      rec_set(a, props, k->t.s, v);
    } while (gp_at(g, ','));
    if (!gp_at(g, '}')) { g->bad = 1; return NULL; }
    return o;
  }
  val *w = gp_word(g);
  if (!w) return NULL;
  for (size_t i = 0; i < sizeof TYPES / sizeof *TYPES; i++) if (!strcmp(w->t.s, TYPES[i][0])) return typed(a, TYPES[i][1]);
  if (!strcmp(w->t.s, "any")) return v_rec(a);
  val *shape = g->shapes ? rec_get(g->shapes, w->t.s) : NULL, *o = shape ? typed(a, "object") : v_rec(a);
  rec_set(a, o, "title", w);
  val *fields = shape ? rec_get(lingo_schema(a, shape->t.s, shape->t.len, NULL), "properties") : NULL;
  if (fields) rec_set(a, o, "properties", fields);
  return o;
}

static val *gp_opt(gp *g) {
  val *b = gp_base(g);
  if (!b || !gp_at(g, '?')) return b;
  val *both = v_list(g->a);
  list_push(g->a, both, b);
  list_push(g->a, both, typed(g->a, "null"));
  return either(g->a, both);
}

static val *gp_alt(gp *g) {
  val *x = gp_opt(g);
  if (!x || !gp_at(g, '|')) return x;
  val *all = v_list(g->a);
  list_push(g->a, all, x);
  do { val *y = gp_opt(g); if (!y) return NULL; list_push(g->a, all, y); } while (gp_at(g, '|'));
  return either(g->a, all);
}

// 4.
val *lingo_schema(arena *a, const char *s, size_t n, val *shapes) {
  gp g = { a, s, 0, n, shapes, 0 };
  val *o = gp_alt(&g);
  while (g.i < n && s[g.i] == ' ') g.i++;
  return !o || g.bad || g.i != n ? v_rec(a) : o;
}

// The way back, JSON Schema as a gives: an object titled with one of the lingo's shapes is that shape, and a part whose
// type is plain is written as its name alone, {id,name}.
static void gives_of(arena *a, buf *b, val *s, val *shapes) {
  val *any = s && s->k == V_REC ? rec_get(s, "anyOf") : NULL;
  if (s && s->k == V_REC && !any) any = rec_get(s, "oneOf");
  if (any && any->k == V_LIST && any->l.n) {
    val *last = any->l.v[any->l.n - 1];
    int maybe = any->l.n == 2 && last->k == V_REC && is_text(rec_get(last, "type"), "null");
    for (size_t i = 0; i < any->l.n - (size_t)maybe; i++) { if (i) buf_chr(b, '|'); gives_of(a, b, any->l.v[i], shapes); }
    if (maybe) buf_chr(b, '?');
    return;
  }
  val *type = s && s->k == V_REC ? rec_get(s, "type") : NULL, *title = s && s->k == V_REC ? rec_get(s, "title") : NULL;
  if (type && type->k == V_LIST) {   // like ["string", "null"]
    val *some = v_list(a);
    int maybe = 0;
    for (size_t i = 0; i < type->l.n; i++)
      if (is_text(type->l.v[i], "null")) maybe = 1;
      else list_push(a, some, typed(a, type->l.v[i]->k == V_TEXT ? type->l.v[i]->t.s : ""));
    if (maybe) list_push(a, some, typed(a, "null"));
    gives_of(a, b, some->l.n > 1 ? either(a, some) : some->l.n ? some->l.v[0] : NULL, shapes);
    return;
  }
  const char *t = type && type->k == V_TEXT ? type->t.s : "";
  if (title && title->k == V_TEXT && (!*t || (shapes && rec_get(shapes, title->t.s)))) { buf_str(b, title->t.s); return; }
  val *items = s && s->k == V_REC ? rec_get(s, "items") : NULL, *props = s && s->k == V_REC ? rec_get(s, "properties") : NULL;
  if (!strcmp(t, "string")) buf_str(b, "text");
  else if (!strcmp(t, "number") || !strcmp(t, "integer")) buf_str(b, "number");
  else if (!strcmp(t, "boolean")) buf_str(b, "bool");
  else if (!strcmp(t, "null")) buf_str(b, "nothing");
  else if (!strcmp(t, "array") && items && items->k == V_REC) { buf_chr(b, '['); gives_of(a, b, items, shapes); buf_chr(b, ']'); }
  else if (!strcmp(t, "array")) buf_str(b, "list");
  else if (!strcmp(t, "object") && props && props->k == V_REC) {
    buf_chr(b, '{');
    for (size_t i = 0; i < props->r.n; i++) {
      buf part = { 0 };
      gives_of(a, &part, props->r.v[i], shapes);
      int plain = !strcmp(part.s, "text") || !strcmp(part.s, "number") || !strcmp(part.s, "bool") || !strcmp(part.s, "any") || !strcmp(part.s, "nothing");
      buf_fmt(b, "%s%s%s%s", i ? "," : "", props->r.key[i], plain ? "" : ":", plain ? "" : part.s);
      buf_free(&part);
    }
    buf_chr(b, '}');
  } else if (!strcmp(t, "object")) buf_str(b, "record");
  else buf_str(b, "any");
}

// 4. An outputSchema as a gives: nothing when there is none.
val *lingo_gives(arena *a, val *schema, val *shapes) {
  if (!schema) return T(a, "nothing");
  buf b = { 0 };
  gives_of(a, &b, schema, shapes);
  val *out = T(a, b.s);
  buf_free(&b);
  return out;
}

// A function's values, [{name, default?}], as an inputSchema: those with no default required.
static val *input_of(arena *a, val *params) {
  val *o = typed(a, "object"), *props = v_rec(a), *required = v_list(a);
  for (size_t i = 0; i < params->l.n; i++) {
    val *one = v_rec(a), *d = rec_get(params->l.v[i], "default"), *n = rec_get(params->l.v[i], "name");
    if (d) rec_set(a, one, "default", d); else list_push(a, required, n);
    rec_set(a, props, n->t.s, one);
  }
  rec_set(a, o, "properties", props);
  rec_set(a, o, "required", required);
  return o;
}

// The way back: an inputSchema's properties in order as a function's values, and its head, like now(place=nothing). A
// value with a default isn't required, and one without is.
static val *params_of(arena *a, const char *fname, val *schema, buf *head, const char **why) {
  val *params = v_list(a), *props = schema && schema->k == V_REC ? rec_get(schema, "properties") : NULL;
  val *required = schema && schema->k == V_REC ? rec_get(schema, "required") : NULL;
  buf_fmt(head, "%s(", fname);
  if (schema && (schema->k != V_REC || !is_text(rec_get(schema, "type"), "object") || (props && props->k != V_REC)))
    return failed(why, "its inputSchema must be like {\"type\": \"object\", \"properties\": {...}, \"required\": [...]}");
  if (required && required->k != V_LIST) return failed(why, "its inputSchema's required must be a list of names");
  for (size_t i = 0; props && i < props->r.n; i++) {
    const char *n = props->r.key[i];
    val *p = props->r.v[i], *d = p->k == V_REC ? rec_get(p, "default") : NULL, *one = v_rec(a);
    int needed = 0;
    for (size_t k = 0; required && k < required->l.n; k++) needed |= is_text(required->l.v[k], n);
    if (!plain_name(n)) return failed(why, fmt(a, "%s isn't a name a value can have: letters, digits and _", n));
    if (p->k != V_REC) return failed(why, fmt(a, "its value %s must be described as JSON Schema, like {\"type\": \"string\"}", n));
    if (d && (d->k == V_LIST || d->k == V_REC)) return failed(why, fmt(a, "the default for %s must be a plain value, like 10, \"*\" or null", n));
    if (!d && !needed) return failed(why, fmt(a, "%s has no default, so its inputSchema must list it in required", n));
    if (d && needed) return failed(why, fmt(a, "%s has a default, so it can't be required", n));
    rec_set(a, one, "name", T(a, n));
    if (d) rec_set(a, one, "default", d);
    list_push(a, params, one);
    buf_fmt(head, "%s%s", i ? ", " : "", n);
    if (d) { buf_chr(head, '='); if (d->k == V_NOTHING) buf_str(head, "nothing"); else buf_json(head, d); }
  }
  for (size_t k = 0; required && k < required->l.n; k++)
    if (required->l.v[k]->k != V_TEXT || !props || !rec_get(props, required->l.v[k]->t.s)) return failed(why, "everything its inputSchema requires must be one of its properties");
  buf_chr(head, ')');
  return params;
}

// A version's numbers, like 1.2.0 or 0.1, as many as it has, up to three. -1 when it isn't one.
static int numbers(const char *s, long out[3]) {
  int n = 0;
  out[0] = out[1] = out[2] = 0;
  while (n < 3 && digit(*s)) {
    for (out[n] = 0; digit(*s); s++) out[n] = out[n] * 10 + (*s - '0');
    n++;
    if (*s != '.' || !digit(s[1])) break;
    s++;
  }
  return n && (!*s || *s == '-' || *s == '+') ? n : -1;
}

// Lower case letters, digits, _ and -, neither of those two first or last.
static int lingo_name(val *n) {
  if (!n || n->k != V_TEXT || !n->t.len || n->t.len > 64) return 0;
  for (size_t i = 0; i < n->t.len; i++) {
    char c = n->t.s[i];
    if (!((c >= 'a' && c <= 'z') || digit(c) || ((c == '_' || c == '-') && i && i + 1 < n->t.len))) return 0;
  }
  return 1;
}

static int file_name(val *f) { return f && f->k == V_TEXT && f->t.len && !strpbrk(f->t.s, "/\\") && strcmp(f->t.s, "..") && strcmp(f->t.s, "."); }

// Permissions as a list, each one of PERMISSIONS, or NULL.
static val *permissions(val *v) {
  if (!v || v->k != V_LIST || !v->l.n) return NULL;
  for (size_t i = 0; i < v->l.n; i++) if (v->l.v[i]->k != V_TEXT || !listed(PERMISSIONS, v->l.v[i]->t.s, v->l.v[i]->t.len)) return NULL;
  return v;
}

// One host function, shaped like an MCP tool, as the phrase record a listing's line is read to.
static val *function_of(arena *a, val *l, val *f, const char **why) {
  val *fname = f->k == V_REC ? rec_get(f, "name") : NULL;
  if (!fname || fname->k != V_TEXT || !plain_name(fname->t.s)) return failed(why, "each of its functions needs a name of letters, digits and _, like forecast");
  val *groups = rec_get(l, "groups"), *group = rec_get(f, "group"), *g = NULL, *onsay = rec_get(f, "onsay"), *does = rec_get(f, "description");
  for (size_t i = 0; i < groups->l.n && !g; i++) if (group && group->k == V_TEXT && is_text(rec_get(groups->l.v[i], "name"), group->t.s)) g = groups->l.v[i];
  if (!g) return failed(why, fmt(a, "function %s needs a group, one of its groups", fname->t.s));
  val *needs = onsay && onsay->k == V_REC && rec_get(onsay, "needs") ? permissions(rec_get(onsay, "needs")) : rec_get(g, "needs");
  if (!needs) return failed(why, fmt(a, "function %s: onsay.needs must list permissions. They are %s", fname->t.s, "none, screen.read, screen.act, files.read, files.write, run and net"));
  buf head = { 0 };
  const char *problem = NULL;
  val *params = params_of(a, fname->t.s, rec_get(f, "inputSchema"), &head, &problem), *ph = v_rec(a);
  if (!params) { buf_free(&head); return failed(why, fmt(a, "function %s: %s", fname->t.s, problem)); }
  const char *gname = rec_get(g, "name")->t.s;
  rec_set(a, ph, "id", T(a, fmt(a, "%s.%s", gname, fname->t.s)));
  rec_set(a, ph, "group", T(a, gname));
  rec_set(a, ph, "params", params);
  rec_set(a, ph, "gives", lingo_gives(a, rec_get(f, "outputSchema"), rec_get(l, "shapes")));
  rec_set(a, ph, "needs", needs);
  rec_set(a, ph, "pure", VBOOL(0));
  rec_set(a, ph, "does", does && does->k == V_TEXT ? does : T(a, ""));
  rec_set(a, ph, "head", T(a, buf_dup(a, &head)));
  if (rec_get(f, "inputSchema")) rec_set(a, ph, "inputSchema", rec_get(f, "inputSchema"));
  buf_free(&head);
  return ph;
}

// 4. folder is where it was found, or NULL. Gives the record, or NULL with the problem in words.
val *lingo_read(arena *a, val *m, const char *folder, const char **why) {
  if (!m || m->k != V_REC) return failed(why, "lingo.json is missing, or isn't a JSON record");
  val *name = rec_get(m, "name"), *version = rec_get(m, "version"), *runs = rec_get(m, "runs"), *about = rec_get(m, "about");
  long n[3];
  if (!is_text(runs, "native") && !is_text(runs, "host"))
    return failed(why, fmt(a, "its runs is native or host, or it is a lingo written in Lingo, with its code beside it in %s.lingo",
                           lingo_name(name) ? name->t.s : folder ? folder : "its_name"));
  if (!lingo_name(name)) return failed(why, "its name must be lower case letters, digits, _ and -, like notes");
  if (folder && strcmp(name->t.s, folder)) return failed(why, fmt(a, "its name is %s, so its folder must be called %s too", name->t.s, name->t.s));
  if (!version || version->k != V_TEXT || numbers(version->t.s, n) != 3) return failed(why, "its version must be like 1.0.0");
  int host = is_text(runs, "host");
  if (is_text(name, "lingo") && !host) return failed(why, "lingo is the language's own name, for lingo(...)");
  val *out = v_rec(a), *shapes = v_rec(a), *groups = v_list(a), *phrases = v_list(a);
  rec_set(a, out, "name", name);
  rec_set(a, out, "version", version);
  rec_set(a, out, "language", T(a, "1.0"));
  if (about && about->k == V_TEXT) rec_set(a, out, "about", about);
  rec_set(a, out, "runs", runs);
  val *library = rec_get(m, "library");
  if (!host && !file_name(library)) return failed(why, "library must name its library, in its folder, like windows.dll");
  if (library) rec_set(a, out, "main", library);
  val *s = rec_get(m, "shapes");
  for (size_t i = 0; s && s->k == V_REC && i < s->r.n; i++) {
    val *fields = s->r.v[i];
    if (fields->k != V_TEXT || !fields->t.len || fields->t.s[0] != '{' || fields->t.s[fields->t.len - 1] != '}')
      return failed(why, "each shape's fields are text, like \"tab\": \"{id,title}\"");
    rec_set(a, shapes, s->r.key[i], fields);
  }
  rec_set(a, out, "shapes", shapes);
  rec_set(a, out, "groups", groups);
  rec_set(a, out, "phrases", phrases);
  if (!host) return out;   // its library adds its groups and functions

  val *gs = rec_get(m, "groups");
  for (size_t i = 0; gs && gs->k == V_LIST && i < gs->l.n; i++) {
    val *g = gs->l.v[i], *gn = g->k == V_REC ? rec_get(g, "name") : NULL, *needs = g->k == V_REC ? permissions(rec_get(g, "needs")) : NULL, *one = v_rec(a);
    int ok = gn && gn->k == V_TEXT && gn->t.len;
    for (size_t k = 0; ok && k < gn->t.len; k++) ok = word_char(gn->t.s[k]) || (gn->t.s[k] == '.' && k && k + 1 < gn->t.len);
    if (!ok || !needs) return failed(why, "each group needs a name and what it needs, like {\"name\": \"tabs\", \"needs\": [\"screen.read\"]}");
    rec_set(a, one, "name", gn);
    rec_set(a, one, "needs", needs);
    rec_set(a, one, "pure", VBOOL(0));
    list_push(a, groups, one);
  }
  // A host lingo may have none yet, like a device with no lingos of its own.
  val *fs = rec_get(m, "functions");
  if (fs && fs->k != V_LIST) return failed(why, "functions must list what it shares");
  for (size_t i = 0; fs && i < fs->l.n; i++) {
    val *ph = function_of(a, out, fs->l.v[i], why);
    if (!ph) return NULL;
    for (size_t k = 0; k < phrases->l.n; k++)
      if (is_text(rec_get(phrases->l.v[k], "id"), rec_get(ph, "id")->t.s)) return failed(why, fmt(a, "it lists the function %s twice", rec_get(ph, "id")->t.s));
    list_push(a, phrases, ph);
  }
  return out;
}

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

// A plain default as a tree has it, -2 for ["neg", 2], or NULL when it isn't one.
static val *plain(arena *a, val *d) {
  if (tagged(d, "neg") && at(d, 1)->k == V_NUM) return v_num(a, -at(d, 1)->n);
  return d && d->k != V_LIST && d->k != V_REC ? d : NULL;
}

/* 4. A lingo written in Lingo is its code, <name>.lingo, and its listing is made from it, never written by hand:
     each fn        a function it shares, called at its address, like laptop.brandon/lingos/memo.lingo/recall, its values
                    and their defaults its fn's own
     the comments   right above a fn, what it does. The first lines of the file, when they are comments, up to a blank
                    line or an empty //, are what the lingo is, its about, "memo - " or "memo.lingo - " at their start
                    left off, and never a fn's too
     settings       the names of its own settings it reads with setting("key")
   Its group is its name, and what each function needs is worked out as it loads (lingo_load). Gives the record lingo_read
   gives, with its settings, or NULL with the problem, starting with the file. */
static val *listing(arena *a, const char *file, val *code, const char **why) {
  size_t fl = strlen(file);
  val *name = fl > 6 && !strcmp(file + fl - 6, ".lingo") ? v_text(a, file, fl - 6) : NULL;
  if (!lingo_name(name)) return failed(why, fmt(a, "%s: a lingo is a file named for it, like notes.lingo, its name lower case letters, digits, _ and -", file));
  if (is_text(name, "lingo")) return failed(why, fmt(a, "%s: lingo is the language's own name, for lingo(...)", file));
  if (!code || code->k != V_TEXT) return failed(why, fmt(a, "%s: its code couldn't be read", file));
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
  val *out = v_rec(a), *groups = v_list(a), *phrases = v_list(a), *group = v_rec(a), *none = v_list(a);
  rec_set(a, out, "name", name);
  rec_set(a, out, "language", T(a, "1.0"));
  if (end > top) {
    const char *s = words_of(a, lines, top, end - 1)->t.s, *lead[] = { fmt(a, "%s - ", file), fmt(a, "%s - ", name->t.s) };
    for (int k = 0; k < 2; k++) if (!strncmp(s, lead[k], strlen(lead[k]))) s += strlen(lead[k]);
    rec_set(a, out, "about", T(a, s));
  }
  rec_set(a, out, "runs", T(a, "lingo"));
  rec_set(a, out, "shapes", v_rec(a));
  list_push(a, none, T(a, "none"));
  rec_set(a, group, "name", name);
  rec_set(a, group, "needs", none);
  rec_set(a, group, "pure", VBOOL(0));
  list_push(a, groups, group);
  rec_set(a, out, "groups", groups);
  for (size_t i = 0; i < fns->r.n; i++) {
    const char *fname = fns->r.key[i];
    size_t here = (size_t)rec_get(c->fn_lines, fname)->n - 1, first = here;
    while (first > 0 && comment(lines[first - 1], &w)) first--;
    val *given = rec_get(fns->r.v[i], "params"), *params = v_list(a), *ph = v_rec(a);
    buf head = { 0 };
    buf_fmt(&head, "%s(", fname);
    for (size_t k = 0; k < given->l.n; k++) {
      val *p = v_rec(a), *d = at(given->l.v[k], 1) ? plain(a, at(given->l.v[k], 1)) : NULL;
      if (at(given->l.v[k], 1) && !d) d = NOTHING;   // worked out as it runs, like o={a: 1}: nothing in its listing
      rec_set(a, p, "name", given->l.v[k]->l.v[0]);
      buf_fmt(&head, "%s%s", k ? ", " : "", given->l.v[k]->l.v[0]->t.s);
      if (d) { rec_set(a, p, "default", d); buf_chr(&head, '='); if (d->k == V_NOTHING) buf_str(&head, "nothing"); else buf_json(&head, d); }
      list_push(a, params, p);
    }
    buf_chr(&head, ')');
    rec_set(a, ph, "id", T(a, fmt(a, "%s.%s", name->t.s, fname)));
    rec_set(a, ph, "group", name);
    rec_set(a, ph, "params", params);
    rec_set(a, ph, "needs", v_copy(a, none));
    rec_set(a, ph, "pure", VBOOL(0));
    rec_set(a, ph, "does", first < here && first != top ? words_of(a, lines, first, here - 1) : T(a, ""));
    rec_set(a, ph, "head", T(a, buf_dup(a, &head)));
    rec_set(a, ph, "inputSchema", input_of(a, params));
    buf_free(&head);
    list_push(a, phrases, ph);
  }
  rec_set(a, out, "phrases", phrases);
  rec_set(a, out, "settings", c->settings);
  return out;
}

// 4. A phrase as a listing's line: its head, -> what it gives, [what it needs] when that isn't its group's, : what it does.
// A Lingo lingo's function says what it gives in its comments, so it has no gives.
void lingo_line(buf *b, val *p, const char *group_needs) {
  buf needs = { 0 };
  join_needs(&needs, rec_get(p, "needs"));
  const char *gives = rec_get(p, "gives") ? rec_get(p, "gives")->t.s : "nothing", *does = rec_get(p, "does")->t.s;
  buf_str(b, rec_get(p, "head")->t.s);
  if (strcmp(gives, "nothing")) buf_fmt(b, " -> %s", gives);
  if (strcmp(needs.s, group_needs)) buf_fmt(b, " [%s]", needs.s);
  if (*does) buf_fmt(b, ": %s", does);
  buf_free(&needs);
}

////////// 5. LINK //////////

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
  const char *file = rec_get(lingo, "file")->t.s, *lname = rec_get(lingo, "name")->t.s, *why = NULL;
  val *code = rec_get(lingo, "code");
  int bad = 0;
  val *read = lingo_parse(a, code->t.s, code->t.len, visible, lname, &bad, &why);
  if (!read) { *message = fmt(a, "%s line %d: %s", file, bad, why); return NULL; }
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

static val *collect(rw *r, val *id, val *args) { (void)args; push_unique(r->a, r->data, id); return NULL; }

// 5.
val *lingo_link(arena *a, val *program, val *stack) {
  linking l = { stack, v_list(a) };
  rw r = { a, link_say, link_call, &l };
  val *fns = v_rec(a), *lines = v_copy(a, rec_get(program, "lines")), *pfns = rec_get(program, "fns");
  val *tool = v_copy(a, rec_get(program, "tool")), *done = rw_fn(&r, tool);
  rec_set(a, tool, "params", rec_get(done, "params"));
  rec_set(a, tool, "body", rec_get(done, "body"));
  for (size_t i = 0; i < pfns->r.n; i++) rec_set(a, fns, pfns->r.key[i], rw_fn(&r, pfns->r.v[i]));
  val *sfns = rec_get(stack, "fns"), *slines = rec_get(stack, "lines");
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
  val *needs = v_list(a);
  rw find = { a, collect, NULL, needs };
  rw_fn(&find, tool);
  for (size_t i = 0; i < fns->r.n; i++) rw_fn(&find, fns->r.v[i]);
  val *out = v_rec(a);
  for (size_t i = 0; i < program->r.n; i++) rec_set(a, out, program->r.key[i], program->r.v[i]);
  rec_set(a, out, "tool", tool);
  rec_set(a, out, "fns", fns);
  rec_set(a, out, "needs", needs);
  rec_set(a, out, "lines", lines);
  return out;
}

////////// 6. INSTANCE //////////

typedef struct { val *native_needs, *fns, *seen, *found; double lingos; } reaching;   // lingos: its lingo(...) calls

static void reach(arena *a, reaching *g, const char *key);

static val *reach_say(rw *r, val *id, val *args) {
  reaching *g = r->data;
  (void)args;
  val *needs = rec_get(g->native_needs, id->t.s);
  if (!strcmp(id->t.s, "lingo.run")) g->lingos++;
  if (needs) for (size_t i = 0; i < needs->l.n; i++) push_unique(r->a, g->found, needs->l.v[i]);
  else if (rec_get(g->fns, id->t.s)) reach(r->a, g, id->t.s);
  return NULL;
}

static val *reach_call(rw *r, val *name, val *args) {
  reaching *g = r->data;
  (void)args;
  if (rec_get(g->fns, name->t.s)) reach(r->a, g, name->t.s);
  return NULL;
}

// What a stacked phrase really needs: every native phrase down every path, through helpers and other stacked phrases,
// and how many lingo(...) calls it makes on the way, each one written counted once.
static void reach(arena *a, reaching *g, const char *key) {
  if (rec_get(g->seen, key)) return;
  rec_set(a, g->seen, key, VBOOL(1));
  rw r = { a, reach_say, reach_call, g };
  rw_fn(&r, rec_get(g->fns, key));
}

static int text_order(const void *x, const void *y) { return strcmp((*(val *const *)x)->t.s, (*(val *const *)y)->t.s); }

// natives: native manifests as text. stacked: [{file, code}] Lingo lingos, each compiled as it loads. Gives
// the instance, or NULL with a message that starts with the file at fault.
// 6.
val *lingo_load(arena *a, val *natives, val *stacked, const char **message) {
  val *lingos = v_rec(a), *native_list = v_list(a), *stacked_list = v_list(a);
  for (size_t i = 0; natives && i < natives->l.n; i++) {
    const char *why = NULL;
    val *l = lingo_manifest(a, natives->l.v[i]->t.s, natives->l.v[i]->t.len, &why);
    if (!l) { *message = fmt(a, "the device's own lingo: %s", why); return NULL; }
    rec_set(a, l, "native", VBOOL(1));
    list_push(a, native_list, l);
    rec_set(a, lingos, rec_get(l, "name")->t.s, l);
  }
  for (size_t i = 0; stacked && i < stacked->l.n; i++) {
    val *file = rec_get(stacked->l.v[i], "file");
    if (!file || file->k != V_TEXT) { *message = "each Lingo lingo is given as {file, code}, like {file: \"memo.lingo\", code}"; return NULL; }
    val *l = listing(a, file->t.s, rec_get(stacked->l.v[i], "code"), message), *name = l ? rec_get(l, "name") : NULL;
    if (!l) return NULL;
    // Never over one of the device's own lingos or their groups, like windows' input, which it would hide.
    for (size_t k = 0; k < native_list->l.n; k++) {
      val *n = native_list->l.v[k], *gs = rec_get(n, "groups");
      int taken = !strcmp(rec_get(n, "name")->t.s, name->t.s);
      for (size_t m = 0; m < gs->l.n && !taken; m++) taken = !strcmp(rec_get(gs->l.v[m], "name")->t.s, name->t.s);
      if (taken) { *message = fmt(a, "%s: %s is already a part of %s, this device's own lingo", file->t.s, name->t.s, rec_get(n, "name")->t.s); return NULL; }
    }
    rec_set(a, l, "file", file);
    rec_set(a, l, "code", rec_get(stacked->l.v[i], "code"));
    list_push(a, stacked_list, l);
    rec_set(a, lingos, rec_get(l, "name")->t.s, l);
  }

  // Each is compiled against every phrase here: the native lingos', core's among them, and every Lingo lingo's in this
  // lingos folder, so one is called by its dotted name, like text.slug(title). Its lingo(...) calls are lingo.run, and its
  // use(...) calls lingo.use, made as it runs.
  val *stack = v_rec(a), *sphrases = v_rec(a), *sfns = v_rec(a), *slines = v_rec(a), *visible = v_list(a);
  rec_set(a, stack, "phrases", sphrases);
  rec_set(a, stack, "fns", sfns);
  rec_set(a, stack, "lines", slines);
  for (size_t i = 0; i < lingos->r.n; i++) {
    val *theirs = rec_get(lingos->r.v[i], "phrases");
    for (size_t k = 0; k < theirs->l.n; k++) list_push(a, visible, theirs->l.v[k]);
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

  // A stacked phrase needs exactly what it reaches. Its file can't claim less, so these are what it shows and asks for.
  val *native_needs = v_rec(a);
  for (size_t i = 0; i < native_list->l.n; i++) {
    val *phrases = rec_get(native_list->l.v[i], "phrases");
    for (size_t k = 0; k < phrases->l.n; k++) rec_set(a, native_needs, rec_get(phrases->l.v[k], "id")->t.s, rec_get(phrases->l.v[k], "needs"));
  }
  for (size_t i = 0; i < stacked_list->l.n; i++) {
    val *phrases = rec_get(stacked_list->l.v[i], "phrases");
    for (size_t k = 0; k < phrases->l.n; k++) {
      reaching g = { native_needs, sfns, v_rec(a), v_list(a), 0 };
      reach(a, &g, rec_get(phrases->l.v[k], "id")->t.s);
      val *needs = v_list(a);
      for (size_t m = 0; m < g.found->l.n; m++) if (!is_text(g.found->l.v[m], "none")) list_push(a, needs, g.found->l.v[m]);
      if (needs->l.n > 1) qsort(needs->l.v, needs->l.n, sizeof(val *), text_order);
      rec_set(a, phrases->l.v[k], "needs", needs);
      rec_set(a, phrases->l.v[k], "lingos", v_num(a, g.lingos));
    }
  }

  val *inst = v_rec(a);
  rec_set(a, inst, "lingos", lingos);
  rec_set(a, inst, "stack", stack);
  rec_set(a, inst, "natives", native_list);
  return inst;
}

// Names are lingos, like notes, or groups of a native lingo, like std or fs. Groups chosen from the same lingo share
// one entry, so a face names each lingo once. Gives [{lingo, groups}], or NULL.
static val *chosen(arena *a, val *inst, val *names, const char **message) {
  val *out = v_list(a), *lingos = rec_get(inst, "lingos"), *natives = rec_get(inst, "natives");
  for (size_t i = 0; names && i < names->l.n; i++) {
    const char *n = names->l.v[i]->t.s;
    val *l = rec_get(lingos, n), *groups = v_list(a);
    if (l) { val *gs = rec_get(l, "groups"); for (size_t k = 0; k < gs->l.n; k++) list_push(a, groups, rec_get(gs->l.v[k], "name")); }
    else {
      for (size_t k = 0; k < natives->l.n && !l; k++) {
        val *gs = rec_get(natives->l.v[k], "groups");
        for (size_t m = 0; m < gs->l.n; m++) if (is_text(rec_get(gs->l.v[m], "name"), n)) { l = natives->l.v[k]; list_push(a, groups, T(a, n)); break; }
      }
      if (!l) {
        buf b = { 0 };
        buf_fmt(&b, "There's no lingo or group called %s. There are: ", n);
        for (size_t k = 0; k < lingos->r.n; k++) buf_fmt(&b, "%s%s", k ? ", " : "", lingos->r.key[k]);
        for (size_t k = 0; k < natives->l.n; k++) {
          val *gs = rec_get(natives->l.v[k], "groups");
          for (size_t m = 0; m < gs->l.n; m++) if (!rec_get(lingos, rec_get(gs->l.v[m], "name")->t.s)) buf_fmt(&b, ", %s", rec_get(gs->l.v[m], "name")->t.s);
        }
        *message = buf_dup(a, &b);
        buf_free(&b);
        return NULL;
      }
    }
    val *had = NULL;
    for (size_t k = 0; k < out->l.n; k++) if (rec_get(out->l.v[k], "lingo") == l) had = out->l.v[k];
    if (had) { val *hg = rec_get(had, "groups"); for (size_t k = 0; k < groups->l.n; k++) push_unique(a, hg, groups->l.v[k]); }
    else {
      val *e = v_rec(a), *gs = v_list(a);
      for (size_t k = 0; k < groups->l.n; k++) list_push(a, gs, groups->l.v[k]);
      rec_set(a, e, "lingo", l);
      rec_set(a, e, "groups", gs);
      list_push(a, out, e);
    }
  }
  return out;
}

// Whether word stands alone in s, as a regular expression's \bword\b finds it. Shape names are all word characters.
static int has_word(const char *s, const char *word) {
  size_t n = strlen(word);
  for (const char *q = strstr(s, word); q; q = strstr(q + 1, word))
    if ((q == s || !word_char(q[-1])) && !word_char(q[n])) return 1;
  return 0;
}

// What Onsay is shown for a task: only those lingos, without their code, with only the shapes they mention, and
// every stacked phrase's real permissions.
val *lingo_face(arena *a, val *inst, val *names, const char **message) {
  val *picked = chosen(a, inst, names, message);
  if (!picked) return NULL;
  buf b = { 0 };
  for (size_t i = 0; i < picked->l.n; i++) {
    val *l = rec_get(picked->l.v[i], "lingo"), *groups = rec_get(picked->l.v[i], "groups");
    val *all = rec_get(l, "phrases"), *shapes = rec_get(l, "shapes"), *lgroups = rec_get(l, "groups");
    int native = rec_get(l, "native") != NULL;
    if (i) buf_str(&b, "\n\n");
    buf_fmt(&b, "lingo %s", rec_get(l, "name")->t.s);
    if (rec_get(l, "version")) buf_fmt(&b, " %s", rec_get(l, "version")->t.s);
    // A Lingo lingo's about is how its maker wants it used, so Onsay reads it with its phrases.
    if (rec_get(l, "about")) buf_fmt(&b, "\n// %s", rec_get(l, "about")->t.s);
    for (size_t s = 0; s < shapes->r.n; s++) {
      int mentioned = 0;
      for (size_t k = 0; k < all->l.n && !mentioned; k++) {
        val *p = all->l.v[k];
        if (!in_list(groups, rec_get(p, "group"))) continue;
        mentioned = has_word(fmt(a, "%s %s", rec_get(p, "gives")->t.s, rec_get(p, "does")->t.s), shapes->r.key[s]);
      }
      if (mentioned) buf_fmt(&b, "\n%s = %s", shapes->r.key[s], shapes->r.v[s]->t.s);
    }
    for (size_t g = 0; g < lgroups->l.n; g++) {
      val *grp = lgroups->l.v[g], *gname = rec_get(grp, "name");
      if (!in_list(groups, gname)) continue;
      // A native group keeps its own line, as written. A stacked group's line carries what most of its phrases really
      // need, and only a phrase that differs shows its own.
      buf gneeds = { 0 };
      if (native) join_needs(&gneeds, rec_get(grp, "needs"));
      else {
        val *keys = v_list(a);
        int counts[256] = { 0 };
        for (size_t k = 0; k < all->l.n; k++) {
          val *p = all->l.v[k];
          if (!is_text(rec_get(p, "group"), gname->t.s)) continue;
          buf one = { 0 };
          join_needs(&one, rec_get(p, "needs"));
          size_t m = 0;
          while (m < keys->l.n && strcmp(keys->l.v[m]->t.s, one.s)) m++;
          if (m == keys->l.n && m < 256) list_push(a, keys, T(a, one.s));
          if (m < 256) counts[m]++;
          buf_free(&one);
        }
        size_t best = 0;
        for (size_t m = 1; m < keys->l.n; m++) if (counts[m] > counts[best]) best = m;
        buf_str(&gneeds, keys->l.n ? keys->l.v[best]->t.s : "none");
      }
      buf_fmt(&b, "\n%s: %s%s", gname->t.s, gneeds.s, truthy(rec_get(grp, "pure")) ? ", pure" : "");
      for (size_t k = 0; k < all->l.n; k++) {
        val *p = all->l.v[k];
        if (!is_text(rec_get(p, "group"), gname->t.s)) continue;
        buf_chr(&b, '\n');
        lingo_line(&b, p, gneeds.s);
      }
      buf_free(&gneeds);
    }
  }
  val *out = v_text(a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return out;
}

// Code may only call what was in its face, then is linked down to native phrases.
val *lingo_prepare(arena *a, val *inst, const char *code, size_t n, val *names, int *line, const char **message) {
  *line = 0;
  val *picked = chosen(a, inst, names, message);
  if (!picked) return NULL;
  val *allowed = v_list(a);
  for (size_t i = 0; i < picked->l.n; i++) {
    val *all = rec_get(rec_get(picked->l.v[i], "lingo"), "phrases"), *groups = rec_get(picked->l.v[i], "groups");
    for (size_t k = 0; k < all->l.n; k++) if (in_list(groups, rec_get(all->l.v[k], "group"))) list_push(a, allowed, all->l.v[k]);
  }
  cx *c = context(a, allowed);
  val *program = parse_with(c, code, n);
  if (!program) { *line = c->line; *message = c->message; return NULL; }
  return lingo_link(a, program, rec_get(inst, "stack"));
}

////////// 7. REQUESTS //////////

/* The whole compiler as one call, JSON in and JSON out:
     {op:"manifest", text}                          -> {manifest} or {error:{message}}
     {op:"parse", code, phrases}                    -> {program} or {error:{line, message}}
     {op:"face", natives, stacked, names}           -> {face} or {error:{message}}
     {op:"prepare", natives, stacked, names, code}  -> {program} or {error:{line?, message}}
     {op:"read", manifest}                          -> {lingo} or {error:{message}}: a native or host lingo's lingo.json
     {op:"read", file, code}                           read as the compiler holds it, or a Lingo lingo's listing made
                                                       from its code */
static void answer_error(buf *out, int line, const char *message) {
  buf_str(out, "{\"error\":{");
  if (line) buf_fmt(out, "\"line\":%d,", line);
  buf_str(out, "\"message\":");
  buf_json_str(out, message, strlen(message));
  buf_str(out, "}}");
}

static void answer(buf *out, const char *key, val *v) {
  buf_fmt(out, "{\"%s\":", key);
  buf_json(out, v);
  buf_chr(out, '}');
}

// 7.
void lingo_request(const char *json, size_t n, buf *out) {
  arena *a = arena_new();
  const char *why = NULL;
  int line = 0;
  val *q = json_parse(a, json, n, &why);
  if (why || q->k != V_REC) { answer_error(out, 0, fmt(a, "the request isn't JSON: %s", why ? why : "not a record")); arena_free(a); return; }
  val *op = rec_get(q, "op"), *text = rec_get(q, "text"), *code = rec_get(q, "code");
  if (is_text(op, "manifest") && text && text->k == V_TEXT) {
    val *m = lingo_manifest(a, text->t.s, text->t.len, &why);
    if (m) answer(out, "manifest", m); else answer_error(out, 0, why);
  } else if (is_text(op, "parse") && code && code->k == V_TEXT) {
    val *p = lingo_parse(a, code->t.s, code->t.len, rec_get(q, "phrases"), NULL, &line, &why);
    if (p) answer(out, "program", p); else answer_error(out, line, why);
  } else if (is_text(op, "face") || (is_text(op, "prepare") && code && code->k == V_TEXT)) {
    val *inst = lingo_load(a, rec_get(q, "natives"), rec_get(q, "stacked"), &why), *done = NULL;
    if (inst && is_text(op, "face")) done = lingo_face(a, inst, rec_get(q, "names"), &why);
    if (inst && !is_text(op, "face")) done = lingo_prepare(a, inst, code->t.s, code->t.len, rec_get(q, "names"), &line, &why);
    if (done) answer(out, is_text(op, "face") ? "face" : "program", done);
    else answer_error(out, line, why);
  } else if (is_text(op, "read")) {
    val *file = rec_get(q, "file");
    val *l = file && file->k == V_TEXT ? listing(a, file->t.s, code, &why) : lingo_read(a, rec_get(q, "manifest"), NULL, &why);
    if (l) answer(out, "lingo", l); else answer_error(out, 0, why);
  } else answer_error(out, 0, "the request needs an op: manifest, parse, face, prepare or read, with what that op takes");
  arena_free(a);
}
