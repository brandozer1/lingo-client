// phrase.c - the table of phrases this computer has, each added with its line, like "keys(prefix=\"\") -> [text]: sorted".
//
// 1. native_begin(lingo), phrase_add(group, needs, line, fn)  -> the phrases of native lingo number lingo, as it loads
// 2. phrase_find(id)                                          -> a phrase by its id, like std.length
// 3. native_phrases(a, lingo)                                 -> a native lingo's phrases, as the compiler reads them,
//                                                                with what each needs
#include "lingo.h"
#include <stdlib.h>
#include <string.h>

static phrase *phrases;
static size_t count, room, adding;   // adding: the native lingo whose phrases are being added
static arena *kept;                  // ids, names and defaults, kept for as long as the core runs

////////// 1. ADDING //////////

void native_begin(size_t lingo) { adding = lingo; }

static size_t trimmed(const char **s, size_t n) {
  while (n && **s == ' ') { (*s)++; n--; }
  while (n && (*s)[n - 1] == ' ') n--;
  return n;
}

// One param, like sep="," or to=nothing. A default is a plain value, written as JSON, or nothing.
static void param(phrase *p, const char *s, size_t n) {
  n = trimmed(&s, n);
  const char *eq = memchr(s, '=', n), *name = s;
  size_t len = trimmed(&name, eq ? (size_t)(eq - s) : n);
  p->params[p->count] = arena_get(kept, len + 1);
  memcpy(p->params[p->count], name, len);
  p->defaults[p->count] = NULL;
  if (eq) {
    const char *d = eq + 1, *problem = NULL;
    size_t dn = trimmed(&d, n - (size_t)(eq - s) - 1);
    val *v = dn == 7 && !memcmp(d, "nothing", 7) ? NOTHING : json_parse(kept, d, dn, &problem);
    if (!problem) p->defaults[p->count] = v;
  }
  p->count++;
}

// 1.
void phrase_add(const char *group, const char *needs, const char *line, phrase_fn fn) {
  if (!kept) kept = arena_new();
  if (count == room) {
    room = room ? room * 2 : 64;
    phrases = realloc(phrases, room * sizeof *phrases);
  }
  phrase *p = &phrases[count++];
  const char *open = strchr(line, '('), *at = open ? open + 1 : line + strlen(line);
  size_t name = (size_t)(at - line) - (open != NULL), glen = strlen(group);
  char *id = arena_get(kept, glen + name + 2);
  memcpy(id, group, glen);
  id[glen] = '.';
  memcpy(id + glen + 1, line, name);
  *p = (phrase){ .id = id, .needs = needs, .fn = fn, .lingo = adding };
  p->params = arena_get(kept, 16 * sizeof *p->params);
  p->defaults = arena_get(kept, 16 * sizeof *p->defaults);
  // Params are split at commas outside quotes and brackets, so a default like sep="," stays whole.
  int depth = 0, quoted = 0;
  for (const char *from = at; open && *at; at++) {
    if (quoted) { if (*at == '\\' && at[1]) at++; else if (*at == '"') quoted = 0; continue; }
    if (*at == '"') quoted = 1;
    else if (*at == '[' || *at == '{' || *at == '(') depth++;
    else if ((*at == ']' || *at == '}' || *at == ')') && depth) depth--;
    else if ((*at == ',' || *at == ')') && !depth) {
      if (at > from && p->count < 16) param(p, from, (size_t)(at - from));
      from = at + 1;
      if (*at == ')') { at++; break; }
    }
  }
  while (*at == ' ' || *at == ':') at++;
  p->does = at;
}

////////// 2. FINDING //////////

phrase *phrase_find(const char *id) {
  for (size_t i = 0; i < count; i++) if (!strcmp(phrases[i].id, id)) return &phrases[i];
  return NULL;
}

////////// 3. AS THE COMPILER READS THEM //////////

val *native_phrases(arena *a, size_t lingo) {
  val *out = v_list(a);
  for (size_t i = 0; i < count; i++) {
    if (phrases[i].lingo != lingo) continue;
    val *one = v_rec(a), *params = v_list(a), *needs = v_list(a);
    for (int k = 0; k < phrases[i].count; k++) {
      val *p = v_rec(a);
      rec_set(a, p, "name", v_cstr(a, phrases[i].params[k]));
      if (phrases[i].defaults[k]) rec_set(a, p, "default", phrases[i].defaults[k]);
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
    rec_set(a, one, "needs", needs);
    list_push(a, out, one);
  }
  return out;
}
