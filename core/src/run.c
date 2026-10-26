// run.c - runs a Lingo tree by the same rules, with the same errors, as test/reference/run.mjs.
//
// 1. fail(r, code, fmt), tick(r)      -> errors, and the step, time and memory limits
// 2. op(r, o, a, b), part, order      -> how values meet
// 3. exec(r, body, frame, back)       -> statements, keeping where they are for errors
// 4. eval(r, e, frame)                -> expressions
// 5. run_program(...), run_phrase(...) -> a whole program, or one phrase, to a result message
#include "lingo.h"
#include "platform.h"
#include "jump.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MEMORY (512u * 1024 * 1024)   // a program that makes more than this is stopped
#define DEEP 1000                      // how deep functions may call each other

enum { T_VAR = 1, T_LIST, T_MAP, T_GET, T_OP, T_AND, T_OR, T_NOT, T_NEG, T_PICK, T_ELSE, T_RANGE, T_CALL, T_SAY,
       T_SET, T_PUT, T_IF, T_WHILE, T_EACH, T_BREAK, T_NEXT, T_RETURN, T_FAIL, T_TRY, T_DO, T_BAD };
static const char *TAGS[] = { "", "var", "list", "map", "get", "op", "and", "or", "not", "neg", "pick", "else", "range", "call",
                              "say", "set", "put", "if", "while", "each", "break", "next", "return", "fail", "try", "do" };

enum { GO, BREAK, NEXT, BACK };   // why a block ended
typedef struct { jmp_buf j; } trap_t;

static val *eval(run *r, val *e, val *frame);
static int exec(run *r, val *body, val *frame, val **back);

// Each node's name is read once and kept as a number in it, so running it again is a switch.
static int tag_of(val *node) {
  if (node->tag) return node->tag;
  node->tag = T_BAD;
  if (node->l.n && node->l.v[0]->k == V_TEXT)
    for (int i = 1; i < T_BAD; i++) if (!strcmp(node->l.v[0]->t.s, TAGS[i])) { node->tag = i; break; }
  return node->tag;
}

static val *item(val *node, size_t i) { return i < node->l.n ? node->l.v[i] : NOTHING; }
static const char *name_of(val *v) { return v->k == V_TEXT ? v->t.s : ""; }

////////// 1. ERRORS AND LIMITS //////////

static const char *phrase_now;   // the phrase running, so its errors say which it was
const char *phrase_running(void) { return phrase_now; }

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

double num_of(run *r, val *x, const char *what) {
  if (x->k != V_NUM || isnan(x->n)) {
    if (what) fail(r, "type", "Can't use %s with %s", what, kind_name(x));
    fail(r, "type", "Expected a number, but this is %s", kind_name(x));
  }
  return x->n;
}

void note(run *r, val *x) {
  buf t = { 0 };
  buf_text(&t, x);
  list_push(r->a, r->log, v_text(r->a, t.s ? t.s : "", t.n));
  buf_free(&t);
}

int picture(run *r, val *x) {
  list_push(r->a, r->pictures, x);
  return (int)r->pictures->l.n;
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
  if ((r->left & 1023) == 0 && pl_stopped()) fail(r, "stopped", "Stopped by the person");
}

static void at_cut(run *r, size_t n) { r->at.n = n; if (r->at.s) r->at.s[n] = 0; }

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

// Reading something missing gives nothing, never an error. Negative positions count from the end.
static val *part(run *r, val *from, val *key) {
  if (from->k == V_LIST || from->k == V_TEXT) {
    if (!whole(key)) return NOTHING;
    double len = from->k == V_LIST ? (double)from->l.n : (double)utf8_count(from->t.s, from->t.len);
    double i = key->n < 0 ? len + key->n : key->n;
    if (i < 0 || i >= len) return NOTHING;
    if (from->k == V_LIST) return from->l.v[(size_t)i];
    size_t a = utf8_offset(from->t.s, from->t.len, (size_t)i), b = utf8_offset(from->t.s, from->t.len, (size_t)i + 1);
    return v_text(r->a, from->t.s + a, b - a);
  }
  if (from->k == V_REC) { val *v = rec_get(from, text_of(r, key)->t.s); return v ? v : NOTHING; }
  return NOTHING;
}

int order(run *r, val *a, val *b) {
  if (a->k == V_NUM && b->k == V_NUM) return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
  if (a->k == V_TEXT && b->k == V_TEXT) {
    size_t n = a->t.len < b->t.len ? a->t.len : b->t.len;
    int c = memcmp(a->t.s, b->t.s, n);
    return c ? (c < 0 ? -1 : 1) : (a->t.len < b->t.len ? -1 : a->t.len > b->t.len ? 1 : 0);
  }
  fail(r, "type", "Can't compare %s with %s", kind_name(a), kind_name(b));
  return 0;
}

static int contains(const char *hay, size_t n, const char *needle, size_t m) {
  if (!m) return 1;
  for (size_t i = 0; i + m <= n; i++) if (!memcmp(hay + i, needle, m)) return 1;
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
    double x = num_of(r, a, o), y = num_of(r, b, o);
    if (y == 0) fail(r, "type", "Can't divide by zero");
    return v_num(r->a, x / y);
  }
  if (!strcmp(o, "%")) {
    double x = num_of(r, a, o), y = num_of(r, b, o);
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
    if (b->k == V_LIST) { for (size_t i = 0; i < b->l.n; i++) if (v_same(b->l.v[i], a)) return VBOOL(1); return VBOOL(0); }
    if (b->k == V_TEXT) { val *t = text_of(r, a); return VBOOL(contains(b->t.s, b->t.len, t->t.s, t->t.len)); }
    if (b->k == V_REC) return VBOOL(rec_get(b, text_of(r, a)->t.s) != NULL);
    return VBOOL(0);
  }
  fail(r, "type", "There is no operator %s", o);
  return NOTHING;
}

////////// 3. STATEMENTS //////////

// The values a function or tool was given, by position or by name, and its defaults for the rest, worked out in order so
// a default can use an earlier value.
static void bind(run *r, val *params, val *given, int named, val *frame) {
  for (size_t i = 0; i < params->l.n; i++) {
    val *p = params->l.v[i];
    const char *name = name_of(item(p, 0));
    val *v = named ? (given && given->k == V_REC ? rec_get(given, name) : NULL) : (given && i < given->l.n ? given->l.v[i] : NULL);
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
        val *value = item(s, 2), *now = rec_get(frame, name);
        if (value->k == V_LIST && tag_of(value) == T_OP && !strcmp(name_of(item(value, 1)), "+") && item(value, 2)->k == V_LIST &&
            tag_of(item(value, 2)) == T_VAR && !strcmp(name_of(item(item(value, 2), 1)), name) && now && now->k == V_LIST &&
            item(value, 3)->k == V_LIST && tag_of(item(value, 3)) == T_LIST) {
          val *adds = item(value, 3);
          for (size_t j = 1; j < adds->l.n; j++) list_push(r->a, now, v_copy(r->a, eval(r, adds->l.v[j], frame)));
        } else rec_set(r->a, frame, name, v_copy(r->a, eval(r, value, frame)));
        break;
      }
      case T_PUT: {
        val *into = eval(r, item(s, 1), frame), *key = eval(r, item(s, 2), frame), *v = v_copy(r->a, eval(r, item(s, 3), frame));
        if (into->k == V_LIST && whole(key)) {
          double n = (double)into->l.n, at = key->n < 0 ? n + key->n : key->n;
          if (at < 0 || at > n) { buf k = { 0 }; buf_num(&k, key->n); fail(r, "type", "There is no item %s in a list of %zu", k.s, into->l.n); }
          if (at == n) list_push(r->a, into, v); else into->l.v[(size_t)at] = v;
        } else if (into->k == V_REC) rec_set(r->a, into, text_of(r, key)->t.s, v);
        else fail(r, "type", "Can't set %s of %s", text_of(r, key)->t.s, kind_name(into));
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
        val *source = item(s, 2), *loop = item(s, 3);
        const char *key = s->l.n > 4 ? name_of(s->l.v[4]) : NULL;
        size_t mark = r->at.n;
        size_t count = 0;
        val **keys = NULL, **items = NULL;
        double from = 0, to = -1;
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
            for (size_t j = 0; j < count; j++) { items[j] = x->l.v[j]; keys[j] = v_num(r->a, (double)j); }
          } else if (x->k == V_REC) {
            count = x->r.n;
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0; j < count; j++) {
              val *k = v_cstr(r->a, x->r.key[j]);
              if (key) { keys[j] = k; items[j] = x->r.v[j]; } else { keys[j] = NOTHING; items[j] = k; }
            }
          } else if (x->k == V_TEXT) {
            count = utf8_count(x->t.s, x->t.len);
            items = arena_get(r->a, (count + 1) * sizeof *items);
            keys = arena_get(r->a, (count + 1) * sizeof *keys);
            for (size_t j = 0, at = 0; j < count; j++) {
              size_t next = at + utf8_offset(x->t.s + at, x->t.len - at, 1);   // from the last, so a long text is gone through once
              items[j] = v_text(r->a, x->t.s + at, next - at);
              keys[j] = v_num(r->a, (double)j);
              at = next;
            }
          } else if (x->k != V_NOTHING) fail(r, "type", "Can't go through each item of %s", kind_name(x));
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
        size_t mark = r->at.n;
        r->trap = &t;
        if (!setjmp(t.j)) {
          buf_str(&r->at, "/0");
          int out = exec(r, item(s, 1), frame, back);
          r->trap = prev;
          if (out) return out;
          break;
        }
        r->trap = prev;
        r->depth = depth;
        if (!strcmp(r->code, "limit") || !strcmp(r->code, "denied") || !strcmp(r->code, "stopped")) rethrow(r);
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
  val *fns = rec_get(r->program, "fns"), *fn = fns ? rec_get(fns, name) : NULL;
  if (!fn) fail(r, "type", "There is no function %s", name);
  if (++r->depth > DEEP) fail(r, "limit", "A function called itself more than 1000 deep, so it was stopped");
  buf saved = { 0 };
  buf_add(&saved, r->at.s ? r->at.s : "", r->at.n);
  val *frame = v_rec(r->a), *back = NOTHING;
  bind(r, rec_get(fn, "params"), args, 0, frame);
  at_cut(r, 0);
  buf_fmt(&r->at, "fn:%s", name);
  int out = exec(r, rec_get(fn, "body"), frame, &back);
  at_cut(r, 0);
  buf_add(&r->at, saved.s ? saved.s : "", saved.n);   // only on success, so an error inside still points inside
  buf_free(&saved);
  r->depth--;
  return out == BACK ? back : NOTHING;
}

// Whether the n bytes at need name a permission given. none always is.
static int allowed(run *r, const char *need, size_t n) {
  if (n == 4 && !memcmp(need, "none", 4)) return 1;
  for (int i = 0; i < r->allowed; i++) if (strlen(r->allow[i]) == n && !memcmp(r->allow[i], need, n)) return 1;
  return 0;
}

static val *say(run *r, const char *id, val *args, val *frame) {
  phrase *p = phrase_find(id);
  if (!p) fail(r, "unknown", "This device doesn't have the phrase %s", id);
  for (const char *n = p->needs; *n; n += strspn(n, ", ")) {   // every permission it needs, like screen.act, screen.read
    size_t len = strcspn(n, ",");
    if (!allowed(r, n, len)) fail(r, "denied", "%s needs permission for %.*s, which wasn't given", id, (int)len, n);
    n += len;
  }
  val **values = arena_get(r->a, (size_t)(p->count + 1) * sizeof *values);
  for (int i = 0; i < p->count; i++) {
    val *given = args && args->k == V_REC ? rec_get(args, p->params[i]) : NULL;
    values[i] = given ? (frame ? eval(r, given, frame) : given) : p->defaults[i] ? p->defaults[i] : NOTHING;
  }
  const char *was = phrase_now;   // a phrase may run others inside it, like lingo.run the calls Onsay sends while it waits
  phrase_now = id;
  val *out = p->fn(r, values);
  phrase_now = was;
  return out ? out : NOTHING;
}

// 4.
static val *eval(run *r, val *e, val *frame) {
  if (e->k != V_LIST) return e;   // nothing, true, false, a number or text
  tick(r);
  switch (tag_of(e)) {
    case T_VAR: { val *v = rec_get(frame, name_of(item(e, 1))); return v ? v : NOTHING; }
    case T_LIST: {
      val *out = v_list(r->a);
      for (size_t i = 1; i < e->l.n; i++) list_push(r->a, out, eval(r, e->l.v[i], frame));
      return out;
    }
    case T_MAP: {
      val *parts = item(e, 1), *out = v_rec(r->a);
      if (parts->k == V_REC) for (size_t i = 0; i < parts->r.n; i++) rec_set(r->a, out, parts->r.key[i], eval(r, parts->r.v[i], frame));
      return out;
    }
    case T_GET: { val *from = eval(r, item(e, 1), frame); return part(r, from, eval(r, item(e, 2), frame)); }
    case T_OP: { val *a = eval(r, item(e, 2), frame); return op(r, name_of(item(e, 1)), a, eval(r, item(e, 3), frame)); }
    case T_AND: return truthy(eval(r, item(e, 1), frame)) ? eval(r, item(e, 2), frame) : VBOOL(0);
    case T_OR: { val *a = eval(r, item(e, 1), frame); return truthy(a) ? a : eval(r, item(e, 2), frame); }
    case T_NOT: return VBOOL(!truthy(eval(r, item(e, 1), frame)));
    case T_NEG: return v_num(r->a, -num_of(r, eval(r, item(e, 1), frame), NULL));
    case T_PICK: return eval(r, truthy(eval(r, item(e, 1), frame)) ? item(e, 2) : item(e, 3), frame);
    case T_ELSE: { val *a = eval(r, item(e, 1), frame); return a->k == V_NOTHING ? eval(r, item(e, 2), frame) : a; }
    case T_RANGE: {
      double from = num_of(r, eval(r, item(e, 1), frame), NULL), to = num_of(r, eval(r, item(e, 2), frame), NULL);
      val *out = v_list(r->a);
      for (double i = from; i <= to; i++) { list_push(r->a, out, v_num(r->a, i)); tick(r); }
      return out;
    }
    case T_CALL: {
      val *given = item(e, 2), *args = v_list(r->a);
      for (size_t i = 0; i < given->l.n; i++) list_push(r->a, args, eval(r, given->l.v[i], frame));
      return call_fn(r, name_of(item(e, 1)), args);
    }
    case T_SAY: return say(r, name_of(item(e, 1)), item(e, 2), frame);
    default: fail(r, "type", "This lingo doesn't know the expression \"%s\"", name_of(item(e, 0)));
  }
  return NOTHING;
}

////////// 5. RUN //////////

static void begin(run *r, arena *a, volatile int *stop, const char *const *allow, int count) {
  memset(r, 0, sizeof *r);
  r->a = a;
  r->stop = stop;
  r->allow = allow;
  r->allowed = count;
  r->log = v_list(a);
  r->pictures = v_list(a);
}

static void finish(run *r, const char *id, int ok, val *value, buf *out) {
  buf_fmt(out, "{\"type\":\"result\",\"id\":%s,\"ok\":%s", id, ok ? "true" : "false");
  if (ok) { buf_str(out, ",\"value\":"); buf_json(out, value); }
  else {
    buf_str(out, ",\"error\":{\"code\":");
    buf_json_str(out, r->code, strlen(r->code));
    buf_str(out, ",\"message\":");
    buf_json_str(out, r->message.s ? r->message.s : "", r->message.n);
    buf_str(out, ",\"at\":");
    buf_json_str(out, r->at_error.s ? r->at_error.s : "", r->at_error.n);
    val *lines = r->program ? rec_get(r->program, "lines") : NULL, *line = lines && r->at_error.s ? rec_get(lines, r->at_error.s) : NULL;
    buf_str(out, ",\"line\":");
    buf_json(out, line ? line : NOTHING);
    buf_chr(out, '}');
  }
  buf_str(out, ",\"log\":");
  buf_json(out, r->log);
  if (r->pictures->l.n) { buf_str(out, ",\"pictures\":"); buf_json(out, r->pictures); }
  buf_chr(out, '}');
  buf_free(&r->message);
  buf_free(&r->at);
  buf_free(&r->at_error);
}

// 5.
void run_program(arena *a, const char *id, val *program, val *input, long long steps, double ms, volatile int *stop,
                 const char *const *allow, int count, buf *out) {
  run r;
  begin(&r, a, stop, allow, count);
  r.program = program;
  r.left = steps;
  r.until = pl_now() * 1000 + ms;
  trap_t t;
  r.trap = &t;
  val *back = NOTHING;
  if (!setjmp(t.j)) {
    val *tool = program->k == V_REC ? rec_get(program, "tool") : NULL;
    if (!tool || tool->k != V_REC) fail(&r, "type", "That isn't a Lingo program: it has no tool");
    val *frame = v_rec(a);
    buf_str(&r.at, "tool");
    bind(&r, rec_get(tool, "params"), input, 1, frame);
    int done = exec(&r, rec_get(tool, "body"), frame, &back);
    finish(&r, id, 1, done == BACK ? back : NOTHING, out);
  } else finish(&r, id, 0, NOTHING, out);
}

void run_phrase(arena *a, const char *id, const char *phrase_id, val *values, volatile int *stop,
                const char *const *allow, int count, buf *out) {
  run r;
  begin(&r, a, stop, allow, count);
  r.left = 1000000;
  r.until = pl_now() * 1000 + 600000;
  trap_t t;
  r.trap = &t;
  if (!setjmp(t.j)) {
    // A call gives its values in order, so they are named here by the phrase's own params.
    phrase *p = phrase_find(phrase_id);
    val *args = v_rec(a);
    if (p && values && values->k == V_LIST)
      for (int i = 0; i < p->count && (size_t)i < values->l.n; i++) rec_set(a, args, p->params[i], values->l.v[i]);
    finish(&r, id, 1, say(&r, phrase_id, args, NULL), out);
  } else finish(&r, id, 0, NOTHING, out);
}
