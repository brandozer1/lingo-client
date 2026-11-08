// phrase.c - the table of phrases this computer has, each added with its line, like
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
#include "lingo.h"
#include <stdlib.h>
#include <string.h>

static phrase *phrases;
static size_t count, room, adding;   // adding: the native lingo whose phrases are being added
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
  if (count == room) {
    room = room ? room * 2 : 64;
    phrases = realloc(phrases, room * sizeof *phrases);
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
