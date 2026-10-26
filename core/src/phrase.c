// phrase.c - the table of phrases this device has, each added with its manifest line exactly as Onsay reads it.
//
// 1. native_begin(name, version, uses), group_add, shape_add, phrase_add  -> a native lingo and its groups, shapes, phrases
// 2. phrase_find(id)                        -> a phrase by id, like std.length
// 3. native_count(), phrase_manifest(b, i)  -> each native lingo's manifest as text, for hello
#include "lingo.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name, *line; size_t lingo; } group;
typedef struct { const char *name, *version, *uses; } native;

static phrase *phrases;
static size_t count, room;
static group groups[256], shapes[128];   // a shape is a name and its fields, like window = {id,title}
static size_t group_count, shape_count;
static native natives[64];
static size_t native_total;
static arena *kept;   // defaults and names, kept for as long as the core runs

////////// 1. ADDING //////////

// uses is the lingo it is built on, like "core 0.1", or NULL for core itself.
void native_begin(const char *name, const char *version, const char *uses) {
  if (native_total < sizeof natives / sizeof *natives) natives[native_total++] = (native){ name, version, uses };
}

void group_add(const char *name, const char *line) {
  if (group_count < sizeof groups / sizeof *groups) groups[group_count++] = (group){ name, line, native_total - 1 };
}

void shape_add(const char *name, const char *fields) {
  if (shape_count < sizeof shapes / sizeof *shapes) shapes[shape_count++] = (group){ name, fields, native_total - 1 };
}

static char *copy(const char *s, size_t n) {
  char *out = arena_get(kept, n + 1);
  memcpy(out, s, n);
  out[n] = 0;
  return out;
}

static size_t trimmed(const char **s, size_t n) {
  while (n && (**s == ' ')) { (*s)++; n--; }
  while (n && (*s)[n - 1] == ' ') n--;
  return n;
}

// One param, like sep="," or to=nothing. A default is a plain value, written as JSON, or nothing.
static void param(phrase *p, const char *s, size_t n) {
  n = trimmed(&s, n);
  const char *eq = memchr(s, '=', n);
  size_t name = eq ? (size_t)(eq - s) : n;
  const char *start = s;
  p->params[p->count] = copy(start, trimmed(&start, name));
  p->defaults[p->count] = NULL;
  if (eq) {
    const char *d = eq + 1;
    size_t len = trimmed(&d, n - name - 1);
    const char *problem = NULL;
    if (!(len == 7 && !memcmp(d, "nothing", 7))) {
      val *v = json_parse(kept, d, len, &problem);
      if (!problem) p->defaults[p->count] = v;
    }
  }
  p->count++;
}

void phrase_add(const char *group_name, const char *needs, const char *line, phrase_fn fn) {
  if (!kept) kept = arena_new();
  if (count == room) {
    room = room ? room * 2 : 64;
    phrases = realloc(phrases, room * sizeof *phrases);
  }
  phrase *p = &phrases[count++];
  const char *open = strchr(line, '(');
  size_t name = open ? (size_t)(open - line) : strlen(line);
  size_t glen = strlen(group_name);
  char *id = arena_get(kept, glen + name + 2);
  memcpy(id, group_name, glen);
  id[glen] = '.';
  memcpy(id + glen + 1, line, name);
  id[glen + 1 + name] = 0;
  *p = (phrase){ .id = id, .line = line, .needs = needs, .fn = fn };
  p->params = arena_get(kept, 16 * sizeof *p->params);
  p->defaults = arena_get(kept, 16 * sizeof *p->defaults);
  if (!open) return;
  // Params are split at commas outside quotes and brackets, so a default like sep="," stays whole.
  const char *at = open + 1, *from = at;
  int depth = 0, quoted = 0;
  for (; *at; at++) {
    if (quoted) { if (*at == '\\' && at[1]) at++; else if (*at == '"') quoted = 0; continue; }
    if (*at == '"') quoted = 1;
    else if (*at == '[' || *at == '{' || *at == '(') depth++;
    else if ((*at == ']' || *at == '}') && depth) depth--;
    else if (*at == ')' && depth) depth--;
    else if ((*at == ',' || *at == ')') && !depth) {
      if (at > from && p->count < 16) param(p, from, (size_t)(at - from));
      from = at + 1;
      if (*at == ')') break;
    }
  }
}

////////// 2. FINDING //////////

phrase *phrase_find(const char *id) {
  for (size_t i = 0; i < count; i++) if (!strcmp(phrases[i].id, id)) return &phrases[i];
  return NULL;
}

////////// 3. THE MANIFEST //////////

size_t native_count(void) { return native_total; }

void phrase_manifest(buf *b, size_t lingo) {
  native *n = &natives[lingo];
  buf_fmt(b, "lingo %s %s, Lingo 1.0%s%s\n", n->name, n->version, n->uses ? ", uses " : "", n->uses ? n->uses : "");
  for (size_t s = 0; s < shape_count; s++) if (shapes[s].lingo == lingo) buf_fmt(b, "%s = %s\n", shapes[s].name, shapes[s].line);
  for (size_t g = 0; g < group_count; g++) {
    if (groups[g].lingo != lingo) continue;
    buf_fmt(b, "\n%s: %s\n", groups[g].name, groups[g].line);
    size_t glen = strlen(groups[g].name);
    for (size_t i = 0; i < count; i++)
      if (!strncmp(phrases[i].id, groups[g].name, glen) && phrases[i].id[glen] == '.') buf_fmt(b, "%s\n", phrases[i].line);
  }
}
