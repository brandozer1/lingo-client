// core.c - the lingo client's engine: its settings, the lingos it loads, who may call what, and every request it answers.
//
// 1. Settings        the settings file, read once, and JSON files in its home
// 2. Floor           the compute, store, http and settings phrases every core has, and lingo(...) and use(...), asked of
//                    the native lingo that answers them, or of the settings' ask
// 3. Lingos          .lingo files and folders: read, loaded, their OpenAPI manifests, running or stopped, the catalog,
//                    and the hooks of native lingos that run
// 4. Policy          policy.json and the account's: callers, signatures and access, the ceiling, grants, the root, asks
//                    and the log
// 5. Stops           stopping the running request
// 6. Calls           one function of a lingo, run
// 7. Lingo settings  setting(...), settings.get and settings.set, kept on this computer, never on Onsay
// 8. Control         start, stop, the person's answers, settings, manifests, the policy and call, at /.lingo/ and on the
//                    command line
// 9. Hooks           what native lingos are offered back: the account's policy kept, settings, this device's keys, its
//                    home, and looking at the lingos folder again
// 10. Requests       web requests, from this computer's port or handed in by a native lingo, each answered by the lingo
//                    at its route when the caller may reach it
// 11. Beginning, and packing a lingo
// It never starts a thread or holds a connection (see core.h), but core_stop may run on another thread, so the running
// request's stop is kept behind one gate.
#include "core.h"
#include "platform.h"
#include "tlds.h"   // TLDS, the web's top level domains as one text, " aaa aarp ... ", made by build.ps1 from tlds.txt
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define say core_say

/* The request running now, for the checks at each phrase: its caller as the log names them, the lingo and function it
   reached, like agent and agent.ask, its path, what that function's manifest says it needs, the policy in force, and
   what the person answered this time, [{what, yes}], and its id as JSON, which each lingo(...) and use(...) it asks
   carries as for. lingo is NULL outside a request, at the command line. */
static struct { const char *caller, *lingo, *fn, *path, *id; val *needs, *policy, *answers; } job;

/* What each native lingo, by its number, filled with lingo_hook as it loaded, and whether it runs in a lingo client that
   serves, when they are used (lingo.h, section 7). */
static lingo_hooks hooks[64];
static int on[64], live;   // live: it serves requests, so its running native lingos are told
static size_t loading;   // the native lingo loading now

// Text made in an arena, like printf.
static const char *fmt(arena *a, const char *f, ...) {
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

// Whether a list holds this text.
static int has(val *list, const char *s) {
  for (size_t i = 0; list && list->k == V_LIST && i < list->l.n; i++) if (list->l.v[i]->k == V_TEXT && !strcmp(list->l.v[i]->t.s, s)) return 1;
  return 0;
}

////////// 1. SETTINGS //////////

static core_settings set;

// A lingo's name: lower case letters, digits, _ and -, neither of those two first or last, and never lingo, the
// language's own name.
static int named(const char *s) {
  size_t n = strlen(s);
  return n && n <= 64 && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == n && !strchr("_-", s[0]) && !strchr("_-", s[n - 1]) && strcmp(s, "lingo");
}

// 1. Kept in their own arena for as long as the core runs.
const core_settings *core_read(const char *text, size_t n) {
  arena *kept = arena_new();
  const char *problem = NULL;
  val *s = json_parse(kept, text, n, &problem), *port, *steps, *ms;
  if (problem || s->k != V_REC) { say("The settings aren't a JSON record: %s", problem ? problem : "they're another kind"); return NULL; }
  buf root = { 0 };
  set.all = s;
  set.home = text_or(s, "home", "onsay-data");
  set.lingos = text_or(s, "lingos", NULL) ? text_or(s, "lingos", NULL) : fmt(kept, "%s/lingos", set.home);
  pl_folder(set.home);
  pl_whole(set.home, &root);
  set.root = v_text(kept, root.s, root.n)->t.s;
  buf_free(&root);
  port = rec_get(s, "port"), steps = rec_get(s, "steps"), ms = rec_get(s, "ms");
  set.port = port && port->k == V_NUM ? (int)port->n : 6969;
  set.steps = steps && steps->k == V_NUM ? (long long)steps->n : 5000000;
  set.ms = ms && ms->k == V_NUM ? ms->n : 600000;
  return &set;
}

// A JSON file in home, like policy.json or settings/agent.json, or NULL when it isn't there or isn't JSON.
static val *home_json(arena *a, const char *name) {
  buf path = { 0 }, text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  val *v = pl_read(path.s, &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  buf_free(&path), buf_free(&text);
  return v;
}

// Keeps one, its folder made when missing. 1 when it was kept.
static int keep_json(const char *name, val *v) {
  buf path = { 0 }, text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  buf_json(&text, v);
  char *slash = strrchr(path.s, '/');
  *slash = 0;
  pl_folder(path.s);
  *slash = '/';
  int ok = pl_write(path.s, text.s, text.n);
  buf_free(&path), buf_free(&text);
  return ok;
}

////////// 2. FLOOR //////////

// compute: the clock, waiting, and a line in the lingo client's log.

static val *time_(run *r, val **a) { (void)a; return v_num(r->a, pl_now()); }

static val *wait_(run *r, val **a) {
  double s = num_of(r, a[0], NULL), until = pl_now() + (s > 60 ? 60 : s);
  // Short sleeps, so a stop still lands quickly.
  while (pl_now() < until) {
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    double left = until - pl_now();
    pl_sleep(left < 0.05 ? left : 0.05);
  }
  return NOTHING;
}

static val *log_(run *r, val **a) {
  say("%s", text_of(r, a[0])->t.s);
  return NOTHING;
}

// store: Onsay's own storage, one file per key in home/store. A key's name is written so any key is a safe file name on
// any system: only a to z, 0 to 9, _ and - stay, everything else becomes %XX, so case and dots can never clash.

static void file_of(run *r, val *key, buf *path) {
  if (key->k != V_TEXT || !key->t.len) fail(r, "phrase", "a key is text, like notes/today");
  if (key->t.len > 100) fail(r, "phrase", "a key is at most 100 characters");
  buf_fmt(path, "%s/store/k", set.home);
  for (size_t i = 0; i < key->t.len; i++) {
    unsigned char c = (unsigned char)key->t.s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') buf_chr(path, (char)c);
    else buf_fmt(path, "%%%02X", c);
  }
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static val *get(run *r, val **a) {
  buf path = { 0 }, text = { 0 };
  file_of(r, a[0], &path);
  int found = pl_read(path.s, &text);
  val *out = found ? v_text(r->a, text.s, text.n) : NOTHING;
  buf_free(&path);
  buf_free(&text);
  return out;
}

static val *put(run *r, val **a) {
  buf path = { 0 }, text = { 0 }, folder = { 0 };
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

typedef struct { run *r; val *prefix, *out; } listing;

static void each_key(const char *name, int folder, double size, double changed, void *arg) {
  listing *l = arg;
  (void)size, (void)changed;
  if (folder || name[0] != 'k') return;
  buf key = { 0 };
  for (const char *p = name + 1; *p; p++) {
    int hi, lo;
    if (*p == '%' && (hi = hex(p[1])) >= 0 && (lo = hex(p[2])) >= 0) { buf_chr(&key, (char)(hi * 16 + lo)); p += 2; }
    else if (*p == '%') { buf_free(&key); return; }   // not one of ours
    else buf_chr(&key, *p);
  }
  if (key.n >= l->prefix->t.len && !memcmp(key.s, l->prefix->t.s, l->prefix->t.len)) list_push(l->r->a, l->out, v_text(l->r->a, key.s, key.n));
  buf_free(&key);
}

static val *keys(run *r, val **a) {
  val *prefix = a[0]->k == V_TEXT ? a[0] : v_cstr(r->a, "");
  listing l = { r, prefix, v_list(r->a) };
  buf folder = { 0 };
  buf_fmt(&folder, "%s/store", set.home);
  pl_list(folder.s, each_key, &l);
  buf_free(&folder);
  // Sorted, so every computer lists the same keys in the same order.
  val **v = l.out->l.v;
  for (size_t i = 1; i < l.out->l.n; i++)
    for (size_t j = i; j > 0 && strcmp(v[j - 1]->t.s, v[j]->t.s) > 0; j--) { val *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  return l.out;
}

/* A web address as a lingo wrote it, made the one to send, by the one rule every Onsay repo keeps (onsay's
   server/lib/names.js web): a host is on the web when it is an IP, localhost, or a name with a dot ending in one of the
   web's top level domains, like example.com, and is itself, http:// or https:// left off when it likes, http for an IP.
   Any other, like laptop.brandon, is an Onsay name: Onsay's resolve endpoint (ONSAY and RESOLVE, core.h) gives its web
   address, and what followed the name goes on after it. NULL, with why, when it has none. */
static const char *reached(arena *a, const char *url, buf *why) {
  size_t s = strspn(url, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-"), n, dots = 0, digits = 1;
  const char *rest = s && !strncmp(url + s, "://", 3) ? url + s + 3 : url;
  char host[256], word[80];
  if ((n = strcspn(rest, "/?#")) >= sizeof host || !n) return url;
  for (size_t i = 0; i <= n; i++) host[i] = i < n ? (char)tolower((unsigned char)rest[i]) : 0;
  char *port = strrchr(host, ':'), *last = strrchr(host, '.');
  if (port && host[0] != '[' && strspn(port + 1, "0123456789") == strlen(port + 1)) *port = 0;
  for (const char *c = host; *c; c++) if (*c == '.') dots++; else if (!isdigit((unsigned char)*c)) digits = 0;
  int ip = host[0] == '[' || !strcmp(host, "localhost") || (digits && dots == 3);
  snprintf(word, sizeof word, " %s ", last ? last + 1 : "");
  if (ip || (last && strlen(last) < 70 && strstr(TLDS, word))) return rest != url ? url : fmt(a, "%s://%s", ip ? "http" : "https", url);
  buf at = { 0 };
  buf_fmt(&at, "%s%s%s", ONSAY, RESOLVE, host);
  val *got = pl_http(a, "GET", at.s, v_rec(a), "", 0, 30, why), *text = rec_get(got, "text");
  val *said = text && text->k == V_TEXT ? json_parse(a, text->t.s, text->t.len, NULL) : NULL;
  const char *address = said && said->k == V_REC ? text_or(said, "address", NULL) : NULL;
  buf_free(&at);
  if (!address && got) buf_fmt(why, "%s", said && said->k == V_REC ? text_or(said, "error", "") : "");
  if (!address && got && !why->n) buf_fmt(why, "%s has nowhere to send a request yet", host);
  return address ? fmt(a, "%s%s", address, rest + n) : NULL;
}

// http.send: one web request, made by the computer itself (pl_http), its method a word and each header one whole line.
static val *send_(run *r, val **a) {
  buf method = { 0 }, url = { 0 }, body = { 0 }, why = { 0 };
  val *headers = v_rec(r->a), *got = NULL;
  const char *to;
  buf_text(&method, a[0]), buf_text(&url, a[1]), buf_text(&body, a[3]);
  buf_chr(&method, 0), buf_chr(&url, 0);   // so neither is ever empty of bytes
  for (size_t i = 0; method.s[i]; i++) if (method.s[i] >= 'a' && method.s[i] <= 'z') method.s[i] -= 32;
  if (strchr(url.s, '#')) *strchr(url.s, '#') = 0;   // the part after # is the page's own, never sent
  if (!*method.s || strspn(method.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(method.s)) buf_str(&why, "the method is a word, like GET or POST");
  for (size_t i = 0; a[2]->k == V_REC && i < a[2]->r.n && !why.n; i++) {
    val *value = text_of(r, a[2]->r.v[i]);
    if (strpbrk(a[2]->r.key[i], "\r\n:") || strpbrk(value->t.s, "\r\n")) buf_fmt(&why, "the header %s has a line break or a colon in it", a[2]->r.key[i]);
    else rec_set(r->a, headers, a[2]->r.key[i], value);
  }
  double seconds = a[4]->k == V_NUM && a[4]->n > 0 && a[4]->n < 3600 ? a[4]->n : 30;
  if (!why.n && (to = reached(r->a, url.s, &why))) got = pl_http(r->a, method.s, to, headers, body.s ? body.s : "", body.n, seconds, &why);
  val *problem = v_text(r->a, why.s ? why.s : "", why.n);
  buf_free(&method), buf_free(&url), buf_free(&body), buf_free(&why);
  if (!got) fail(r, "phrase", "%s", problem->t.s);
  return got;
}

/* lingo(...) and use(...) as a program reaches them, in, with for, the request it runs for: asked of the running native
   lingo that answers them, which waits, else POSTed to the settings' ask, a web address that answers the same way. */
static val *asked(run *r, buf *in) {
  buf out = { 0 }, why = { 0 };
  size_t k = 1;
  const char *at = text_or(set.all, "ask", NULL);
  val *got = NULL, *answer, *text;
  if (job.id) { in->n--; buf_str(in, ",\"for\":"); buf_str(in, job.id); buf_chr(in, '}'); }
  while (k < 64 && !(on[k] && hooks[k].ask)) k++;
  if (k < 64 && hooks[k].ask(in->s, in->n, r->stop, &out)) got = json_parse(r->a, out.s ? out.s : "", out.n, NULL);
  else if (at) {
    val *headers = v_rec(r->a);
    rec_set(r->a, headers, "content-type", v_cstr(r->a, "application/json"));
    if ((at = reached(r->a, at, &why)) && (answer = pl_http(r->a, "POST", at, headers, in->s, in->n, set.ms / 1000, &why)) && (text = rec_get(answer, "text")) && text->k == V_TEXT)
      got = json_parse(r->a, text->t.s, text->t.len, NULL);
  }
  const char *problem = why.n ? v_text(r->a, why.s, why.n)->t.s : "This lingo client isn't connected to Onsay";
  buf_free(in), buf_free(&out), buf_free(&why);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got || got->k != V_REC) fail(r, "phrase", "%s", problem);
  const char *error = text_or(got, "error", NULL);
  if (error) fail(r, text_or(got, "code", "phrase"), "%s", error);
  return rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
}

// lingo.run(ask, values, gives), with what was around it (pl_context) and the function asking, never the answer itself.
static val *lingo_run(run *r, val **a) {
  buf in = { 0 };
  buf_str(&in, "{\"ask\":");
  buf_json(&in, a[0]);
  buf_str(&in, ",\"values\":");
  buf_json(&in, a[1]);
  buf_str(&in, ",\"gives\":");
  buf_json(&in, a[2]);
  buf_str(&in, ",\"context\":");
  pl_context(&in);
  if (job.fn) { buf_str(&in, ",\"asking\":"); buf_json_str(&in, job.fn, strlen(job.fn)); }
  buf_chr(&in, '}');
  return asked(r, &in);
}

// lingo.use(address, values).
static val *lingo_use(run *r, val **a) {
  buf in = { 0 };
  buf_str(&in, "{\"address\":");
  buf_json(&in, a[0]);
  buf_str(&in, ",\"values\":");
  buf_json(&in, a[1]);
  buf_chr(&in, '}');
  return asked(r, &in);
}

static val *setting_(run *r, val **a), *settings_get(run *r, val **a), *settings_set(run *r, val **a);   // 7.

// lingo.run, lingo.use and lingo.setting are lingo(...), use(...) and setting(...), the language's own, in no listing.
static void floor_phrases(void) {
  phrase_add("compute", "none", "now() -> number: seconds since 1970", time_);
  phrase_add("compute", "none", "wait(seconds): at most 60", wait_);
  phrase_add("compute", "none", "log(x): a line in the lingo client's log", log_);
  phrase_add("store", "store", "get(key) -> text?: Onsay's own storage on this computer", get);
  phrase_add("store", "store", "put(key, value): kept as text", put);
  phrase_add("store", "store", "keys(prefix=\"\") -> [text]: sorted", keys);
  phrase_add("store", "store", "delete(key)", delete_);
  phrase_add("http", "net", "send(method, url, headers=nothing, body=nothing, timeout=30) -> {status,headers,text}: one request, "
             "headers a record, body text, redirects followed, at most 4 MB back", send_);
  phrase_add("settings", "none", "get(lingo) -> [record]: every setting of a lingo, as its openapi.json and its code say, each "
             "with its value now", settings_get);
  phrase_add("settings", "none", "set(lingo, key, value): kept wherever that lingo keeps its settings", settings_set);
  phrase_add("lingo", "none", "run(ask, values=nothing, gives=\"\")", lingo_run);
  phrase_add("lingo", "none", "use(address, values=nothing)", lingo_use);
  phrase_add("lingo", "none", "setting(key, reference)", setting_);
}

////////// 3. LINGOS //////////

/* Core is the only lingo built in. Every other is a .lingo file in the lingos folder, a zip of its openapi.json and its
   program, or the same files in a folder of its name, as a lingo is written: its program is <name>.ls, in Lingo, or a
   native library, <name>.dll or <name>.so. One that can't load is set aside with its reason, and the rest still load.
   A lingo is running, its routes answering and in the catalog, or stopped, as home/running.json says. */
static arena *library;
static val *device;                  // the compiler's instance of every lingo that loaded
static val *found;                   // every lingo read: {name, from, manifest, program, file and code or library,
                                     // native, running, fresh, error}. fresh: its paths are made from its code
static val *catalog;                 // every running lingo, with its manifest or why it can't load
static unsigned long long looked;    // a fingerprint of the lingos folder and running.json when they were last read
#define FNV 14695981039346656037ULL

// Every native lingo by its number, core first, by its name. A library is loaded once and kept, since its phrases are in
// the table from then on: a changed one is used after a restart.
static const char *natives[64] = { "core" };
static size_t native_total = 1;

static unsigned long long fnv(unsigned long long h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

static void print_file(const char *name, int folder, double size, double changed, void *arg) {
  unsigned long long *h = arg;
  *h = fnv(*h, name, strlen(name));
  *h = fnv(*h, &folder, sizeof folder);
  *h = fnv(*h, &size, sizeof size);
  *h = fnv(*h, &changed, sizeof changed);
}

typedef struct { arena *a; val *names; unsigned long long *print; } seen;

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
  buf path = { 0 }, running = { 0 };
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
  buf_free(&path), buf_free(&running);
  return f.names;
}

// Sets one aside, with why.
static void set_aside(arena *a, val *e, const char *why) {
  rec_set(a, e, "error", v_cstr(a, why));
  say("Lingo %s can't be used: %s", text_or(e, "from", ""), why);
}

// One file of a lingo, from its .lingo file's zip, or from its folder when zip is NULL.
static int piece(const char *path, buf *zip, const char *file, buf *out) {
  buf at = { 0 };
  buf_fmt(&at, "%s/%s", path, file);
  int got = zip ? zip_file(zip->s, zip->n, file, out) : pl_read(at.s, out);
  buf_free(&at);
  return got;
}

static int native_called(const char *name) {
  for (size_t i = 0; i < native_total; i++) if (!strcmp(natives[i], name)) return (int)i;
  return -1;
}

/* 3. One lingo, from its .lingo file or a folder of its files, at path: {name, from, manifest, program, and file and code
   or library}, with error when it can't be used. NULL for a folder with neither an openapi.json nor a program, which
   isn't a lingo. Its program is the one its openapi.json's x-lingo names, else whichever of <name>.ls, <name>.dll and
   <name>.so is there. A library in a .lingo file is put in home/libraries to be loaded, unless it is loaded already. */
static val *read_lingo(arena *a, const char *path) {
  const char *base = path + strlen(path), *problem = NULL;
  while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
  size_t len = strlen(base);
  int zipped = len > 6 && !strcmp(base + len - 6, ".lingo");
  val *e = v_rec(a), *m = NULL, *x, *settings, *keep;
  buf zip = { 0 }, text = { 0 }, program = { 0 }, code = { 0 };
  rec_set(a, e, "name", v_text(a, base, zipped ? len - 6 : len));
  rec_set(a, e, "from", v_cstr(a, base));
  const char *name = text_or(e, "name", ""), *own = text_or(e, "from", ""), *route, *wanted;
  const char *programs[] = { fmt(a, "%s.ls", name), fmt(a, "%s.dll", name), fmt(a, "%s.so", name) };
  if (zipped) pl_read(path, &zip);
  int manifest = piece(path, zipped ? &zip : NULL, "openapi.json", &text), k = 0;
  m = manifest ? json_parse(a, text.s, text.n, &problem) : v_rec(a);
  x = rec_get(m, "x-lingo"), settings = rec_get(x, "settings"), keep = rec_get(x, "keep"), route = text_or(x, "route", NULL);
  wanted = text_or(x, "program", NULL);
  while (k < 3 && !((!wanted || !strcmp(wanted, programs[k])) && piece(path, zipped ? &zip : NULL, programs[k], &code))) k++;
  if (k < 3) buf_str(&program, programs[k]);
  rec_set(a, e, "manifest", m ? m : v_rec(a));

  int listed = !settings || settings->k == V_LIST;
  for (size_t i = 0; listed && settings && i < settings->l.n; i++) listed = settings->l.v[i]->k == V_TEXT || text_or(settings->l.v[i], "name", NULL);
  if (!zipped && !manifest && !program.n) e = NULL;
  else if (zipped && !manifest) set_aside(a, e, "it isn't a .lingo file: a zip of its openapi.json and its program, made with pack");
  else if (problem || m->k != V_REC) set_aside(a, e, "its openapi.json isn't a JSON record");
  else if (!named(name)) set_aside(a, e, "its name is lower case letters, digits, _ and -, like notes, and never lingo");
  else if (text_or(x, "name", NULL) && strcmp(text_or(x, "name", ""), name)) set_aside(a, e, fmt(a, "its openapi.json calls it %s, and its file %s", text_or(x, "name", ""), own));
  else if (wanted && !program.n)
    set_aside(a, e, fmt(a, "its openapi.json's program, %s, isn't beside it: it is %s.ls, or a library like %s.dll", wanted, name, name));
  else if (!program.n) set_aside(a, e, fmt(a, "its program goes beside its openapi.json, named for it: %s.ls, or a library like %s.dll", name, name));
  else if (!listed) set_aside(a, e, "its settings are a list, each a name or a record with its name, like {\"name\": \"key\", \"secret\": true}");
  else if (keep && (!text_or(keep, "read", NULL) || !text_or(keep, "write", NULL)))
    set_aside(a, e, "keep names its own functions that read and write its settings, like {\"read\": \"read\", \"write\": \"write\"}");
  else if (route && (route[0] != '/' || !route[1] || strpbrk(route, "?# ") || route[strlen(route) - 1] == '/' || !strncmp(route, "/.lingo", 7)))
    set_aside(a, e, "its route is a path of its own, like /onsay");
  else {
    rec_set(a, e, "program", v_text(a, program.s, program.n));
    if (k == 0) {
      rec_set(a, e, "file", v_text(a, program.s, program.n));
      rec_set(a, e, "code", v_text(a, code.s ? code.s : "", code.n));
    } else if (!zipped) rec_set(a, e, "library", v_cstr(a, fmt(a, "%s/%s", path, program.s)));
    else {
      // Written only when it changed, so a library another lingo client here has loaded is left as it is.
      buf at = { 0 }, now = { 0 };
      buf_fmt(&at, "%s/libraries/%s", set.home, name);
      pl_folder(at.s);
      buf_fmt(&at, "/%s", program.s);
      int same = pl_read(at.s, &now) && now.n == code.n && !memcmp(now.s, code.s, code.n);
      if (native_called(name) < 0 && !same && !pl_write(at.s, code.s, code.n)) set_aside(a, e, fmt(a, "its library couldn't be put in %s", at.s));
      else rec_set(a, e, "library", v_text(a, at.s, at.n));
      buf_free(&at), buf_free(&now);
    }
  }
  buf_free(&zip), buf_free(&text), buf_free(&program), buf_free(&code);
  return e;
}

// A lingo read, by its name, or NULL.
static val *entry_of(const char *name) {
  for (size_t i = 0; found && i < found->l.n; i++) if (!strcmp(text_or(found->l.v[i], "name", ""), name)) return found->l.v[i];
  return NULL;
}

static val *manifest_of(const char *lingo) { return rec_get(entry_of(lingo), "manifest"); }

// Where a lingo answers: the route its manifest's x-lingo names, like /onsay, else /<name>.
static const char *route_of(arena *a, val *e) {
  const char *own = text_or(rec_get(rec_get(e, "manifest"), "x-lingo"), "route", NULL);
  return own ? own : fmt(a, "/%s", text_or(e, "name", ""));
}

// The functions a lingo shares at its routes: serve alone when it has serve(request), else every one.
static val *shared(arena *a, const char *name) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), name), "phrases"), *out = v_list(a);
  const char *serve = fmt(a, "%s.serve", name);
  for (size_t i = 0; own && i < own->l.n; i++) if (!strcmp(text_or(own->l.v[i], "id", ""), serve)) { list_push(a, out, own->l.v[i]); return out; }
  for (size_t i = 0; own && i < own->l.n; i++) list_push(a, out, own->l.v[i]);
  return out;
}

// What a manifest says fn needs, the x-lingo-needs of the operation whose x-lingo-function it is, or nothing listed.
static val *said_needs(arena *a, val *m, const char *fn) {
  val *paths = rec_get(m, "paths");
  for (size_t i = 0; paths && paths->k == V_REC && i < paths->r.n; i++)
    for (size_t k = 0; paths->r.v[i]->k == V_REC && k < paths->r.v[i]->r.n; k++) {
      val *op = paths->r.v[i]->r.v[k], *needs = rec_get(op, "x-lingo-needs");
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
    val *needs = rec_get(fns->l.v[i], "needs"), *said = said_needs(a, rec_get(e, "manifest"), id);
    for (size_t k = 0; needs && k < needs->l.n; k++)
      if (!has(said, needs->l.v[k]->t.s))
        return fmt(a, "%s: %s needs %s, which its openapi.json doesn't say. Pack it again to say all it needs", text_or(e, "file", ""), id, needs->l.v[k]->t.s);
  }
  return NULL;
}

/* 3. A lingo's whole OpenAPI manifest, as pack writes it: what its openapi.json says, its name, program and route in
   x-lingo, and each function it shares as a path under its route, its words, its values as JSON Schema, x-lingo-function,
   x-lingo-needs and x-lingo-calls, and x-lingo-internal on a helper whose comment says it is for the lingo's own use, like
   "For agent's own use", which Onsay lists nowhere. /name alone is its main, and a lingo with serve(request) shares serve
   alone, at its route. */
static val *described(arena *a, val *e) {
  const char *name = text_or(e, "name", ""), *route = route_of(a, e);
  val *given = rec_get(e, "manifest"), *l = rec_get(rec_get(device, "lingos"), name), *fns = shared(a, name);
  val *out = v_rec(a), *info = v_rec(a), *x = v_rec(a), *paths = v_rec(a), *was;
  for (size_t i = 0; (was = rec_get(given, "info")) && was->k == V_REC && i < was->r.n; i++) rec_set(a, info, was->r.key[i], was->r.v[i]);
  for (size_t i = 0; (was = rec_get(given, "x-lingo")) && was->k == V_REC && i < was->r.n; i++) rec_set(a, x, was->r.key[i], was->r.v[i]);
  if (!rec_get(info, "title")) rec_set(a, info, "title", v_cstr(a, name));
  if (!rec_get(info, "version")) rec_set(a, info, "version", v_cstr(a, "1.0.0"));
  if (!rec_get(info, "description") && rec_get(l, "about")) rec_set(a, info, "description", rec_get(l, "about"));
  rec_set(a, x, "name", v_cstr(a, name));
  rec_set(a, x, "program", rec_get(e, "program"));
  rec_set(a, x, "route", v_cstr(a, route));
  rec_set(a, out, "openapi", v_cstr(a, "3.1.0"));
  rec_set(a, out, "info", info);
  for (size_t i = 0; given && i < given->r.n; i++)
    if (strcmp(given->r.key[i], "openapi") && strcmp(given->r.key[i], "info") && strcmp(given->r.key[i], "paths") && strcmp(given->r.key[i], "x-lingo"))
      rec_set(a, out, given->r.key[i], given->r.v[i]);
  for (size_t i = 0; i < fns->l.n; i++) {
    val *f = fns->l.v[i], *op = v_rec(a), *item = v_rec(a), *says = rec_get(f, "does"), *schema = rec_get(f, "inputSchema"), *takes = rec_get(schema, "properties");
    const char *id = text_or(f, "id", ""), *local = !strncmp(id, name, strlen(name)) && id[strlen(name)] == '.' ? id + strlen(name) + 1 : id, *does = text_or(f, "does", "");
    const char *words = strncmp(does, "->", 2) ? does : strstr(does, ": ") ? strstr(does, ": ") + 2 : "";   // a native's does starts with what it gives
    int serve = !strcmp(local, "serve") && fns->l.n == 1;
    buf path = { 0 };
    buf_str(&path, route);
    if (!serve && strcmp(local, "main")) { buf_chr(&path, '/'); for (const char *c = local; *c; c++) buf_chr(&path, *c == '.' ? '/' : *c); }
    if (*words) rec_set(a, op, "summary", v_text(a, words, strstr(words, ". ") ? (size_t)(strstr(words, ". ") - words + 1) : strlen(words)));
    if (*does) rec_set(a, op, "description", says);
    if (!serve && takes && takes->k == V_REC && takes->r.n) {
      val *body = v_rec(a), *content = v_rec(a), *json = v_rec(a);
      rec_set(a, json, "schema", schema);
      rec_set(a, content, "application/json", json);
      rec_set(a, body, "content", content);
      rec_set(a, op, "requestBody", body);
    }
    val *responses = v_rec(a), *ok = v_rec(a);
    rec_set(a, ok, "description", v_cstr(a, serve ? "What serve gives, for any method and any path under its route" : "The function's value, as JSON"));
    rec_set(a, responses, "200", ok);
    rec_set(a, op, "responses", responses);
    rec_set(a, op, "x-lingo-function", v_cstr(a, id));
    rec_set(a, op, "x-lingo-needs", rec_get(f, "needs") ? rec_get(f, "needs") : v_list(a));
    rec_set(a, op, "x-lingo-calls", rec_get(f, "lingos") ? rec_get(f, "lingos") : v_num(a, 0));
    if (strstr(does, "'s own use")) rec_set(a, op, "x-lingo-internal", VBOOL(1));
    rec_set(a, item, serve ? "get" : "post", op);
    rec_set(a, paths, v_text(a, path.s, path.n)->t.s, item);
    buf_free(&path);
  }
  rec_set(a, out, "paths", paths);
  rec_set(a, out, "x-lingo", x);
  return out;
}

// Reads everything again: every lingo, the natives, the graph, the manifests and the catalog, with extra, a folder being
// packed, in place of any lingo of its name. Built beside what there was, which stays in use when even core's own lingos
// can't load now. 1 when it loaded.
static int load(const char *extra) {
  arena *was = library;
  val *was_device = device, *was_catalog = catalog, *was_found = found;
  arena *a = library = arena_new();
  val *names = look(a, &looked), *stacked = v_list(a), *offered = v_list(a), *running = home_json(a, "running.json"), *more = NULL;
  found = v_list(a);
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
    if (!rec_get(e, "error") && entry_of(text_or(e, "name", ""))) set_aside(a, e, "another lingo here is already called that");
    rec_set(a, e, "running", VBOOL(has(running, text_or(e, "name", ""))));
    list_push(a, found, e);
  }

  // core, then every native here, each library loaded the first time it is seen. One whose lingo is gone stays loaded,
  // but isn't offered.
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *name = text_or(e, "name", ""), *path = text_or(e, "library", NULL);
    if (rec_get(e, "error") || !path) continue;
    int k = native_called(name);
    if (k < 0 && native_total == sizeof natives / sizeof *natives) { set_aside(a, e, "this core can't take more native lingos"); continue; }
    if (k < 0) {
      buf why = { 0 };
      lingo_entry entry = pl_library(path, &why);
      if (!entry) set_aside(a, e, fmt(a, "%s: %s", text_or(e, "program", ""), why.s));
      buf_free(&why);
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
    val *one = v_rec(a), *e = i ? entry_of(natives[i]) : NULL;
    if (i && (!e || rec_get(e, "error") || !rec_get(e, "native"))) continue;
    rec_set(a, one, "name", v_cstr(a, natives[i]));
    rec_set(a, one, "phrases", native_phrases(a, i));
    list_push(a, offered, one);
  }
  for (size_t i = 0; i < found->l.n; i++) if (!rec_get(found->l.v[i], "error") && rec_get(found->l.v[i], "code")) list_push(a, stacked, found->l.v[i]);

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
      library = was, device = was_device, catalog = was_catalog, found = was_found;
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
    val *e = found->l.v[i], *one = v_rec(a), *error = rec_get(e, "error");
    if (!error && (!rec_get(rec_get(e, "manifest"), "paths") || rec_get(e, "fresh"))) rec_set(a, e, "manifest", described(a, e));
    if (!truthy(rec_get(e, "running"))) continue;
    rec_set(a, one, "name", rec_get(e, "name"));
    rec_set(a, one, error ? "error" : "openapi", error ? error : rec_get(e, "manifest"));
    list_push(a, catalog, one);
  }
  say("%zu lingos are ready", all->r.n - 1);
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
   address: they reach a lingo when an access rule lets them in, and anyone else gets a 404, as if it weren't there.
   At every phrase, a need must be under the ceiling, the most this computer ever allows, and said in the lingo's
   manifest. net, store and files inside the root, this lingo client's home, are given then; files outside it, run,
   screen.read and screen.act need a grant, or the person's yes, asked through an app. Every decision but a routine one
   goes in home/decisions.log. */
static const char *NEEDS[] = { "net", "store", "files.read", "files.write", "screen.read", "screen.act", "run", NULL };   // the first four routine
#define ATTACHED 10   // seconds an app counts as open after it last asked for the questions waiting

// The questions waiting for the person's answer, each {id, lingo, need, path, caller}, in the run's arena that waits.
static struct { val *ask; arena *a; volatile int done; } asks[16];
static int waiting;       // how many wait, the last asked last
static long questions;    // their ids
static double polled;     // when an app last asked for them

/* 4. The policy in force, read as each request comes, so a change from the command line or by hand holds at once:
   {ceiling, access, block, grants, local, account}. Its base is the account's, home/policy.account.json, with its
   grants for this computer by its device, its name on the account, or with none this computer's defaults. The
   overrides of policy.json go first, and every grant is clamped to the ceiling. A rule naming devices holds on those
   alone. After every rule written, the defaults every lingo client has, marked default: me and my devices reach every
   lingo, and a running native lingo may be open to someone of its own (open in lingo_hooks). local and account are the
   two files as they are. */
static val *policy(arena *a) {
  val *local = home_json(a, "policy.json"), *account = home_json(a, "policy.account.json"), *out = v_rec(a), *grants = v_rec(a), *access;
  if (!local || local->k != V_REC) local = v_rec(a);
  if (account && account->k != V_REC) account = NULL;
  val *over = rec_get(local, "overrides"), *base = account ? account : rec_get(local, "defaults"), *ceiling = rec_get(local, "ceiling");
  val *given[2] = { rec_get(over, "grants"), account ? rec_get(rec_get(account, "grants"), text_or(account, "device", "")) : rec_get(base, "grants") };
  const char *parts[] = { "access", "block" };
  if (!ceiling || ceiling->k != V_LIST) { ceiling = v_list(a); for (int i = 0; NEEDS[i]; i++) list_push(a, ceiling, v_cstr(a, NEEDS[i])); }
  rec_set(a, out, "ceiling", ceiling);
  for (int k = 0; k < 2; k++) {
    val *both = v_list(a), *from[2] = { rec_get(over, parts[k]), rec_get(base, parts[k]) };
    for (int s = 0; s < 2; s++)
      for (size_t i = 0; from[s] && from[s]->k == V_LIST && i < from[s]->l.n; i++) {
        val *devices = rec_get(from[s]->l.v[i], "devices");
        if (!devices || devices->k != V_LIST || has(devices, text_or(account, "device", ""))) list_push(a, both, from[s]->l.v[i]);
      }
    rec_set(a, out, parts[k], both);
  }
  access = rec_get(out, "access");
  for (size_t i = 0; i < native_total + 2; i++) {
    const char *who = i == 0 ? "me" : i == 1 ? "my devices" : on[i - 2] ? hooks[i - 2].open : NULL;
    val *rule = v_rec(a);
    if (!who) continue;
    rec_set(a, rule, "who", v_cstr(a, who));
    rec_set(a, rule, "lingos", v_cstr(a, i < 2 ? "*" : natives[i - 2]));
    rec_set(a, rule, "default", VBOOL(1));
    list_push(a, access, rule);
  }
  for (int s = 0; s < 2; s++)
    for (size_t i = 0; given[s] && given[s]->k == V_REC && i < given[s]->r.n; i++) {
      val *list = given[s]->r.v[i], *mine = rec_get(grants, given[s]->r.key[i]);
      if (!mine) rec_set(a, grants, given[s]->r.key[i], mine = v_list(a));
      for (size_t k = 0; list->k == V_LIST && k < list->l.n; k++) {
        val *g = list->l.v[k];
        if (g->k == V_TEXT && has(ceiling, v_text(a, g->t.s, strcspn(g->t.s, " "))->t.s) && !has(mine, g->t.s)) list_push(a, mine, g);
      }
    }
  rec_set(a, out, "grants", grants);
  rec_set(a, out, "local", local);
  rec_set(a, out, "account", account ? account : NOTHING);
  return out;
}

// An address as 16 bytes, IPv4 as IPv6 writes it, ::ffff:1.2.3.4. 0 when it isn't one.
static int ip_of(const char *s, unsigned char out[16]) {
  unsigned b[4], v[8] = { 0 };
  int n = 0, gap = -1;
  char end;
  memset(out, 0, 16);
  if (!strchr(s, ':')) {
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &b[0], &b[1], &b[2], &b[3], &end) != 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255) return 0;
    out[10] = out[11] = 255;
    for (int i = 0; i < 4; i++) out[12 + i] = (unsigned char)b[i];
    return 1;
  }
  if (s[0] == ':' && s[1] == ':') gap = 0, s += 2;
  while (*s) {
    char *after;
    unsigned long g = strtoul(s, &after, 16);
    if (n == 8 || after == s || after - s > 4 || g > 0xFFFF) return 0;
    v[n++] = (unsigned)g, s = after;
    if (*s && *s++ != ':') return 0;
    if (*s == ':' && s[-1] == ':') { if (gap >= 0) return 0; gap = n, s++; }
    else if (!*s && s[-1] == ':') return 0;
  }
  if (gap < 0 ? n != 8 : n > 7) return 0;
  for (int i = 0; i < n; i++) {
    int at = gap < 0 || i < gap ? i : 8 - (n - gap) + (i - gap);
    out[2 * at] = (unsigned char)(v[i] >> 8), out[2 * at + 1] = (unsigned char)v[i];
  }
  return 1;
}

/* A request's caller: this computer, me, a device that signed it, or anyone else by their address, ip. name is how the
   log names them: this computer, me, ip 203.0.113.7, or a signer's keyid, like laptop.brandon. */
typedef struct { const char *name, *ip, *handle; int here, me, mine; } caller;

// Whether who, an access rule's or a block's, names the caller: public, anyone, me, my devices, a device signed on this
// computer's own handle, a handle, any device of it, or ip and an address, or a range of them like 10.0.0.0/8.
static int names(const char *who, const caller *c) {
  unsigned char want[16], have[16];
  char address[64];
  const char *slash = strchr(who, '/');
  if (!strcmp(who, "public")) return 1;
  if (!strcmp(who, "me")) return c->me;
  if (!strcmp(who, "my devices")) return c->mine;
  if (strncmp(who, "ip ", 3)) return c->handle && !strcmp(who, c->handle);
  snprintf(address, sizeof address, "%.*s", slash ? (int)(slash - who - 3) : 63, who + 3);
  int bits = slash ? atoi(slash + 1) + (strchr(address, ':') ? 0 : 96) : 128;
  if (!c->ip || !ip_of(address, want) || !ip_of(c->ip, have)) return 0;
  for (int i = 0; i < bits && i < 128; i++) if ((want[i / 8] ^ have[i / 8]) & (0x80 >> (i % 8))) return 0;
  return 1;
}

/* Whether the caller may reach lingo at rest, the path after its route like /share, by the policy p: never when a block
   names their address, else by the first access rule that names them and the lingo, "*" or its name, and the route when
   it names routes. One that says lingos: "none" lets them reach nothing. rest NULL asks for any of its routes. why says
   which rule decided. */
static int let_in(arena *a, val *p, const caller *c, const char *lingo, const char *rest, const char **why) {
  val *block = rec_get(p, "block"), *access = rec_get(p, "access");
  for (size_t i = 0; i < block->l.n; i++)
    if (block->l.v[i]->k == V_TEXT && names(fmt(a, "ip %s", block->l.v[i]->t.s), c)) { *why = fmt(a, "blocked: %s", block->l.v[i]->t.s); return 0; }
  for (size_t i = 0; i < access->l.n; i++) {
    val *rule = access->l.v[i], *lingos = rec_get(rule, "lingos"), *routes = rec_get(rule, "routes");
    const char *who = text_or(rule, "who", "");
    int hit = !rest || !routes || routes->k != V_LIST;
    if (!names(who, c)) continue;
    if (lingos && lingos->k == V_TEXT && !strcmp(lingos->t.s, "none")) { *why = fmt(a, "rule: %s, none", who); return 0; }
    if (!lingos || !((lingos->k == V_TEXT && (!strcmp(lingos->t.s, "*") || !strcmp(lingos->t.s, lingo))) || has(lingos, lingo))) continue;
    for (size_t k = 0; !hit && k < routes->l.n; k++) {
      const char *route = routes->l.v[k]->k == V_TEXT ? routes->l.v[k]->t.s : "", *at = *rest ? rest : "/";
      size_t n = strlen(route);
      hit = n && !strncmp(at, route, n) && (!at[n] || at[n] == '/');
    }
    if (hit) { *why = fmt(a, "rule: %s", who); return 1; }
  }
  *why = "no rule lets them in";
  return 0;
}

static struct { char *seen; double at; } *nonces;   // each signature's keyid and nonce, kept 5 minutes, so none is used twice
static size_t nonce_count;

/* 4. Who signed a request, by HTTP Message Signatures (RFC 9421) with Ed25519. Its Signature-Input covers @method,
   @target-uri and content-digest at least, and says when it was made, its nonce and the signer's keyid, like
   laptop.brandon, whose key the running native lingo that looks keys up gives (key in lingo_hooks). It must check out,
   be made within 5 minutes, its nonce new in that time, and its Content-Digest the body's sha-256. Its @target-uri is
   x-forwarded-proto (else http), :// x-forwarded-host (else host), and x-forwarded-uri (else the path), so one passed on
   by a proxy is checked as it was sent. Fills in c, or gives why not. */
static const char *signer(arena *a, val *h, const char *method, const char *path, val *body, caller *c) {
  const char *input = text_or(h, "signature-input", ""), *eq = strchr(input, '='), *close = eq ? strchr(eq, ')') : NULL, *p;
  const char *keyid = NULL, *nonce = NULL, *uri = text_or(h, "x-forwarded-uri", path), *host = text_or(h, "x-forwarded-host", text_or(h, "host", "")), *why = NULL, *missing = NULL;
  buf base = { 0 }, raw = { 0 }, digest = { 0 }, key = { 0 };
  unsigned char sum[32];
  int covers = 0;
  double created = 0, now = pl_now();
  if (!eq || eq[1] != '(' || !close) return "Its Signature-Input isn't one, like on=(\"@method\" \"@target-uri\" \"content-digest\");created=...;nonce=\"...\";keyid=\"...\"";
  for (p = eq + 2; p < close; p++) {
    if (*p != '"') continue;
    size_t n = strcspn(p + 1, "\"");
    const char *id = v_text(a, p + 1, n)->t.s, *value = NULL;
    if (!strcmp(id, "@method")) value = method;
    else if (!strcmp(id, "@target-uri")) value = fmt(a, "%s://%s%s", text_or(h, "x-forwarded-proto", "http"), host, uri);
    else if (!strcmp(id, "@authority")) value = host;
    else if (!strcmp(id, "@path")) value = v_text(a, uri, strcspn(uri, "?"))->t.s;
    else if (!strcmp(id, "@query")) value = strchr(uri, '?') ? strchr(uri, '?') : "?";
    else if (id[0] != '@') value = text_or(h, id, NULL);
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
    if (!strncmp(p + 1, "created=", 8)) created = atof(text);
    else if (!strncmp(p + 1, "nonce=", 6)) nonce = text;
    else if (!strncmp(p + 1, "keyid=", 6)) keyid = text;
  }
  const char *label = fmt(a, "%.*s=:", (int)(eq - input), input), *sig = strstr(text_or(h, "signature", ""), label);
  const char *said = strstr(text_or(h, "content-digest", ""), "sha-256=:");
  size_t k = 1;
  while (k < 64 && !(on[k] && hooks[k].key)) k++;
  val *who = !missing && keyid && k < 64 ? hooks[k].key(a, keyid) : NULL;
  const char *their = text_or(who, "key", "");
  sha256(body->t.s, body->t.len, sum);
  base64(&digest, sum, 32);
  if (missing) why = fmt(a, "Its signature covers %s, which it hasn't", missing);
  else if (covers < 3) why = "Its signature must cover @method, @target-uri and content-digest";
  else if (!keyid || !nonce || !created) why = "Its Signature-Input needs created, nonce and keyid";
  else if (created < now - 300 || created > now + 300) why = "Its signature wasn't made within 5 minutes";
  else if (!said || strncmp(said + 9, digest.s, digest.n) || said[9 + digest.n] != ':') why = "Its Content-Digest isn't its body's sha-256";
  else if (!who) why = fmt(a, "There's no key known for %s", keyid);
  else if (!sig || !unbase64(sig + strlen(label), strcspn(sig + strlen(label), ":"), &raw) || raw.n != 64 || strncmp(their, "ed25519:", 8) ||
           !unbase64(their + 8, strlen(their + 8), &key) || key.n != 32 || !ed25519_check((unsigned char *)key.s, base.s, base.n, (unsigned char *)raw.s))
    why = fmt(a, "Its signature doesn't check out for %s", keyid);
  else {
    const char *seen = fmt(a, "%s %s", keyid, nonce);
    size_t kept = 0;
    for (size_t i = 0; i < nonce_count; i++) {
      if (nonces[i].at < now - 300) { free(nonces[i].seen); continue; }
      if (!strcmp(nonces[i].seen, seen)) why = "Its nonce was used already";
      nonces[kept++] = nonces[i];
    }
    nonce_count = kept;
    if (!why) {
      nonces = realloc(nonces, (nonce_count + 1) * sizeof *nonces);
      nonces[nonce_count].seen = strdup(seen), nonces[nonce_count++].at = now;
      c->name = keyid, c->handle = text_or(who, "handle", NULL), c->mine = truthy(rec_get(who, "mine") ? rec_get(who, "mine") : NOTHING);
    }
  }
  buf_free(&base), buf_free(&raw), buf_free(&digest), buf_free(&key);
  return why;
}

// One decision in the log, home/decisions.log, a line of JSON: {at, caller, lingo, route, need, allowed, why}, and to
// each running native lingo that wants it. need is nothing for whether the caller may reach the lingo at all.
static void decided(const char *need, int allowed, const char *why) {
  arena *a = arena_new();
  val *line = v_rec(a);
  buf text = { 0 }, path = { 0 };
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
  for (size_t i = 1; i < 64; i++) if (on[i] && hooks[i].decided) hooks[i].decided(text.s);
  buf_chr(&text, '\n');
  buf_fmt(&path, "%s/decisions.log", set.home);
  pl_append(path.s, text.s, text.n);
  buf_free(&text), buf_free(&path);
  arena_free(a);
}

_Noreturn static void refuse(run *r, const char *need, const char *why, const char *words) {
  decided(need, 0, why);
  fail(r, "denied", "%s", words);
}

// A grant's path, ~ at its start being this computer's person's own folder.
static const char *place(arena *a, const char *path) {
  const char *mine = getenv("USERPROFILE") ? getenv("USERPROFILE") : getenv("HOME");
  return path[0] == '~' && mine ? fmt(a, "%s%s", mine, path + 1) : path;
}

// Whether path is folder or inside it, / and \ the same, and case aside in a Windows path, like C:\Users.
static int inside(const char *path, const char *folder) {
  size_t n = strlen(folder);
  int windows = folder[0] && folder[1] == ':';
  while (n > 1 && (folder[n - 1] == '/' || folder[n - 1] == '\\') && !(windows && n == 3)) n--;
  for (size_t i = 0; i < n; i++) {
    char x = path[i] == '\\' ? '/' : path[i], y = folder[i] == '\\' ? '/' : folder[i];
    if (windows) x = (char)tolower((unsigned char)x), y = (char)tolower((unsigned char)y);
    if (!x || x != y) return 0;
  }
  return !path[n] || path[n] == '/' || path[n] == '\\' || folder[n - 1] == '/' || folder[n - 1] == '\\';
}

// Adds a grant to policy.json's overrides, like files.read C:\Users\me\Pictures for photos. 1 when it was kept.
static int granting(arena *a, const char *lingo, const char *grant) {
  val *local = home_json(a, "policy.json"), *over, *grants, *mine;
  if (!local || local->k != V_REC) local = v_rec(a);
  if (!(over = rec_get(local, "overrides")) || over->k != V_REC) rec_set(a, local, "overrides", over = v_rec(a));
  if (!(grants = rec_get(over, "grants")) || grants->k != V_REC) rec_set(a, over, "grants", grants = v_rec(a));
  if (!(mine = rec_get(grants, lingo)) || mine->k != V_LIST) rec_set(a, grants, lingo, mine = v_list(a));
  if (!has(mine, grant)) list_push(a, mine, v_cstr(a, grant));
  return keep_json("policy.json", local);
}

// 4.
void may(run *r, const char *need, const char *path) {
  val *p = job.policy ? job.policy : policy(r->a), *granted, *ask;
  const char *lingo = job.lingo, *what = path ? fmt(r->a, "%s %s", need, path) : need, *said;
  int k = 0, slot;
  while (NEEDS[k] && strcmp(NEEDS[k], need)) k++;
  if (!has(rec_get(p, "ceiling"), need)) refuse(r, what, "the ceiling", fmt(r->a, "%s needs %s, which this computer never allows", lingo ? lingo : "It", need));
  if (!lingo) return;
  if (!has(job.needs, need)) refuse(r, what, "not declared", fmt(r->a, "%s needs %s, which its openapi.json doesn't say", lingo, need));
  if (k < 2 || (k < 4 && (!path || inside(path, set.root)))) return;
  granted = rec_get(rec_get(p, "grants"), lingo);
  for (size_t i = 0; granted && i < granted->l.n; i++) {
    const char *g = granted->l.v[i]->t.s;
    size_t w = strcspn(g, " ");
    if (w == strlen(need) && !strncmp(g, need, w) && (!g[w] || (path && inside(path, place(r->a, g + w + 1))))) { decided(what, 1, fmt(r->a, "granted: %s", g)); return; }
  }
  for (size_t i = 0; i < job.answers->l.n; i++)
    if (!strcmp(text_or(job.answers->l.v[i], "what", ""), what)) {
      if (truthy(rec_get(job.answers->l.v[i], "yes"))) return;
      refuse(r, what, "asked: no", fmt(r->a, "The person said no to %s using %s", lingo, what));
    }
  if (pl_now() - polled > ATTACHED || waiting == 16)
    refuse(r, what, "no app to ask", fmt(r->a, "%s needs %s here, which takes a yes from the person, and no app is open to ask them. To allow it, grant it: grant %s \"%s\"", lingo, what, lingo, what));

  // Asked through the app, which answers once, always or no, waiting up to a minute while other requests are answered.
  slot = waiting++;
  ask = v_rec(r->a);
  rec_set(r->a, ask, "id", v_num(r->a, (double)++questions));
  rec_set(r->a, ask, "lingo", v_cstr(r->a, lingo));
  rec_set(r->a, ask, "need", v_cstr(r->a, need));
  rec_set(r->a, ask, "path", path ? v_cstr(r->a, path) : NOTHING);
  rec_set(r->a, ask, "caller", v_cstr(r->a, job.caller));
  asks[slot].ask = ask, asks[slot].a = r->a, asks[slot].done = 0;
  lingo_wait(&asks[slot].done, r->stop, 60);
  waiting--;
  said = text_or(ask, "answer", "");
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  val *answer = v_rec(r->a);
  rec_set(r->a, answer, "what", v_cstr(r->a, what));
  rec_set(r->a, answer, "yes", VBOOL(strcmp(said, "once") == 0 || strcmp(said, "always") == 0));
  list_push(r->a, job.answers, answer);
  if (!strcmp(said, "always")) granting(r->a, lingo, what);
  if (truthy(rec_get(answer, "yes"))) { decided(what, 1, fmt(r->a, "asked: %s", said)); return; }
  if (*said) refuse(r, what, "asked: no", fmt(r->a, "The person said no to %s using %s", lingo, what));
  refuse(r, what, "nobody answered", fmt(r->a, "Nobody answered whether %s may use %s, so it didn't", lingo, what));
}

////////// 5. STOPS //////////

static pl_gate *gate;             // keeps the two below, which core_stop may set from another thread while a request runs
static volatile int stopping;     // the running request's stop flag
static char running[128];         // the running request's id, as JSON, so a stop can name it

void core_say(const char *fmt_, ...) {
  va_list ap;
  va_start(ap, fmt_);
  fprintf(stderr, "onsay: ");
  vfprintf(stderr, fmt_, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// 5. At once, even while the request runs on another thread.
void core_stop(const char *id) {
  pl_enter(gate);
  if (!strcmp(id, running)) stopping = 1;
  pl_leave(gate);
}

////////// 6. CALLS //////////

// 6. fn, a function of a lingo here, linked from its code and run with its values in order, a value left out taking its
// default. Its value, or NULL with the error's code and message.
static val *core_call(arena *a, const char *fn, val *values, volatile int *stop, const char **code, const char **message) {
  val *program = lingo_call(a, device, fn), *input = v_rec(a);
  char name[24];
  if (!program) { *code = "unknown", *message = v_cstr(a, fn)->t.s; return NULL; }
  for (size_t i = 0; i < values->l.n; i++) {
    snprintf(name, sizeof name, "a%zu", i);
    rec_set(a, input, name, values->l.v[i]);
  }
  return run_program(a, program, input, (limits){ set.steps, set.ms, stop }, code, message);
}

////////// 7. LINGO SETTINGS //////////

// A lingo's settings are kept on this computer, never on Onsay: in home/settings/<lingo>.json, a record by name, or by
// the lingo's own functions when its manifest's x-lingo says keep: {read, write}, so its maker decides where they live.
// setting(...) is lingo.setting(key, reference), its value the one kept, else its manifest's default, else nothing.

static const char *keeping;   // the lingo whose own read or write runs now, so its setting(...) reads this computer's file

/* One lingo's settings, as a form shows them: each its manifest's x-lingo lists, a name alone or a record of its name,
   title, about, default, required and secret, then each other one its code reads, by name. keep, when asked for, is
   where its x-lingo says they are kept, {read, write}, its own functions, or NULL for this computer's file of them, as it
   is inside those functions too. */
static val *declared(arena *a, const char *lingo, val **keep) {
  val *out = v_list(a), *x = rec_get(manifest_of(lingo), "x-lingo"), *own = rec_get(rec_get(device, "lingos"), lingo);
  val *listed = rec_get(x, "settings"), *read = rec_get(own, "settings");
  for (size_t i = 0; listed && listed->k == V_LIST && i < listed->l.n; i++) {
    val *s = listed->l.v[i], *one = v_rec(a);
    if (s->k == V_TEXT) rec_set(a, one, "name", s);
    else for (size_t k = 0; k < s->r.n; k++) rec_set(a, one, s->r.key[k], s->r.v[k]);
    list_push(a, out, one);
  }
  for (size_t i = 0; read && i < read->l.n; i++) {
    int had = 0;
    for (size_t k = 0; k < out->l.n && !had; k++) had = !strcmp(text_or(out->l.v[k], "name", ""), read->l.v[i]->t.s);
    if (!had) { val *one = v_rec(a); rec_set(a, one, "name", read->l.v[i]); list_push(a, out, one); }
  }
  if (keep) *keep = !(keeping && !strcmp(keeping, lingo)) ? rec_get(x, "keep") : NULL;
  return out;
}

// The lingo a setting is of, checked, since it names a file, and the setting's name when there is one.
static const char *whose(run *r, val *lingo, val *key) {
  if (lingo->k != V_TEXT || !named(lingo->t.s)) fail(r, "phrase", "a lingo's settings are named by the lingo, like agent");
  if (key && (key->k != V_TEXT || !key->t.len)) fail(r, "phrase", "a setting is named by its name, like key");
  return lingo->t.s;
}

// 7. The settings this computer keeps for a lingo, in home/settings/<lingo>.json, a record by name.
val *lingo_setting(arena *a, const char *lingo, const char *key) {
  val *s = named(lingo) ? home_json(a, fmt(a, "settings/%s.json", lingo)) : NULL;
  return s && s->k == V_REC ? rec_get(s, key) : NULL;
}

// 7.
int lingo_set(const char *lingo, const char *key, val *value) {
  arena *a = arena_new();
  val *s = named(lingo) ? home_json(a, fmt(a, "settings/%s.json", lingo)) : NULL;
  if (!s || s->k != V_REC) s = v_rec(a);
  rec_set(a, s, key, value);
  int ok = named(lingo) && keep_json(fmt(a, "settings/%s.json", lingo), s);
  arena_free(a);
  return ok;
}

// A lingo's own function that keeps its settings, read(key) or write(key, value), run as that lingo's own. Its value.
static val *kept(run *r, const char *lingo, const char *fn, val *values) {
  const char *was = keeping, *code = NULL, *message = NULL, *was_lingo = job.lingo, *was_fn = job.fn, *id = fmt(r->a, "%s.%s", lingo, fn);
  val *was_needs = job.needs;
  keeping = lingo;
  if (job.lingo) job.lingo = lingo, job.fn = id, job.needs = said_needs(r->a, manifest_of(lingo), id);
  val *got = core_call(r->a, id, values, r->stop, &code, &message);
  keeping = was, job.lingo = was_lingo, job.fn = was_fn, job.needs = was_needs;
  if (!got) fail(r, code, "%s, which keeps %s's settings: %s", id, lingo, !strcmp(code, "unknown") ? "it has no function by that name" : message);
  return got;
}

// lingo.setting(key, reference).
static val *setting_(run *r, val **a) {
  const char *lingo = whose(r, a[1], a[0]);
  val *keep = NULL, *all = declared(r->a, lingo, &keep), *v;
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, a[0]);
    v = kept(r, lingo, text_or(keep, "read", ""), values);
  } else v = lingo_setting(r->a, lingo, a[0]->t.s);
  for (size_t i = 0; (!v || v->k == V_NOTHING) && i < all->l.n; i++)
    if (!strcmp(text_or(all->l.v[i], "name", ""), a[0]->t.s)) v = rec_get(all->l.v[i], "default");
  return v ? v : NOTHING;
}

// settings.get(lingo), for a form of them: each its manifest lists, with what it says, then each other one its code
// reads, each with its value now.
static val *settings_get(run *r, val **a) {
  val *all = declared(r->a, whose(r, a[0], NULL), NULL), *asked_[2] = { NULL, a[0] };
  for (size_t i = 0; i < all->l.n; i++) {
    asked_[0] = rec_get(all->l.v[i], "name");
    rec_set(r->a, all->l.v[i], "value", setting_(r, asked_));
  }
  return all;
}

// settings.set(lingo, key, value), wherever that lingo keeps them.
static val *settings_set(run *r, val **a) {
  const char *lingo = whose(r, a[0], a[1]);
  val *keep = NULL;
  declared(r->a, lingo, &keep);
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, a[1]);
    list_push(r->a, values, a[2]);
    kept(r, lingo, text_or(keep, "write", ""), values);
    return NOTHING;
  }
  if (!lingo_set(lingo, a[1]->t.s, a[2])) fail(r, "phrase", "this computer couldn't save the setting");
  return NOTHING;
}

////////// 8. CONTROL //////////

static int serving;   // how many requests are being answered, one inside another's wait

/* 8. One thing an app or the person does, from /.lingo/<verb> or the command line, its values by name. Its answer, or
   NULL with why in words.
     start, stop {lingo}             a lingo running or stopped, kept in home/running.json
     asks                            the questions waiting for the person, and the app asking counts as open to ask
     answer {id, answer}             once, always or no to one
     settings {lingo}                its settings, each with what it is and its value now
     set {lingo, key, value}         one of them changed
     manifest {lingo}                its whole OpenAPI manifest, as the catalog has it, or why it can't load
     policy                          the policy in force, and the files it comes from
     share {lingo, who, route}       an access rule added to policy.json's overrides: who may reach lingo, or "*"
     unshare {lingo, who}            every rule there that lets who reach lingo, or "*", taken out
     grant {lingo, need}             a grant added there, like run or files.read C:\Users\me\Pictures
     ungrant {lingo, need}           that grant taken out of there
     ceiling {needs}                 the most this computer ever allows
     call {fn, values}               a function of any lingo here, or a phrase, run as this computer, its values in order,
                                     like onsay.sign_in, its value */
val *core_control(arena *a, const char *verb, val *v, const char **why) {
  const char *lingo = text_or(v, "lingo", ""), *code = NULL, *message = NULL;
  val *out = NOTHING, *local = home_json(a, "policy.json"), *values = v_list(a), *over;
  volatile int stop = 0;
  if (!local || local->k != V_REC) local = v_rec(a);
  if (!(over = rec_get(local, "overrides")) || over->k != V_REC) rec_set(a, local, "overrides", over = v_rec(a));
  *why = NULL;
  if (!strcmp(verb, "start") || !strcmp(verb, "stop")) {
    val *was = home_json(a, "running.json"), *now = v_list(a);
    if (!entry_of(lingo)) { *why = fmt(a, "There's no lingo called %s here", lingo); return NULL; }
    for (size_t i = 0; was && was->k == V_LIST && i < was->l.n; i++) if (was->l.v[i]->k == V_TEXT && strcmp(was->l.v[i]->t.s, lingo)) list_push(a, now, was->l.v[i]);
    if (!strcmp(verb, "start")) list_push(a, now, v_cstr(a, lingo));
    if (!keep_json("running.json", now)) { *why = "This computer couldn't save which lingos run"; return NULL; }
    rec_set(a, out = v_rec(a), "lingo", v_cstr(a, lingo));
    rec_set(a, out, "running", VBOOL(!strcmp(verb, "start")));
    if (serving <= 1) core_look();   // at once, unless a request waits inside, which the next look waits for
  } else if (!strcmp(verb, "asks")) {
    polled = pl_now();
    out = v_list(a);
    for (int i = 0; i < waiting; i++) list_push(a, out, v_copy(a, asks[i].ask));
  } else if (!strcmp(verb, "answer")) {
    val *id = rec_get(v, "id");
    const char *answer = text_or(v, "answer", "");
    int i = 0;
    while (i < waiting && !(id && id->k == V_NUM && rec_get(asks[i].ask, "id")->n == id->n)) i++;
    if (strcmp(answer, "once") && strcmp(answer, "always") && strcmp(answer, "no")) { *why = "The answer is once, always or no"; return NULL; }
    if (i == waiting) { *why = "That question isn't waiting any more"; return NULL; }
    rec_set(asks[i].a, asks[i].ask, "answer", v_cstr(asks[i].a, answer));
    asks[i].done = 1;
  } else if (!strcmp(verb, "settings") || !strcmp(verb, "set")) {
    list_push(a, values, v_cstr(a, lingo));
    if (!strcmp(verb, "set")) list_push(a, values, rec_get(v, "key") ? rec_get(v, "key") : NOTHING), list_push(a, values, rec_get(v, "value") ? rec_get(v, "value") : NOTHING);
    out = core_call(a, !strcmp(verb, "set") ? "settings.set" : "settings.get", values, &stop, &code, &message);
    if (!out) { *why = message; return NULL; }
  } else if (!strcmp(verb, "manifest")) {
    val *e = entry_of(lingo);
    if (!e) { *why = fmt(a, "There's no lingo called %s here", lingo); return NULL; }
    if (rec_get(e, "error")) { *why = text_or(e, "error", ""); return NULL; }
    out = rec_get(e, "manifest");
  } else if (!strcmp(verb, "policy")) out = policy(a);
  else if (!strcmp(verb, "share")) {
    const char *who = text_or(v, "who", ""), *route = text_or(v, "route", NULL), *slash = strchr(who, '/');
    unsigned char ip[16];
    char address[64];
    int known = !strcmp(who, "public") || !strcmp(who, "me") || !strcmp(who, "my devices") || named(who);
    val *rule = v_rec(a), *access = rec_get(over, "access");
    if (!strncmp(who, "ip ", 3)) {
      snprintf(address, sizeof address, "%.*s", slash ? (int)(slash - who - 3) : 63, who + 3);
      known = ip_of(address, ip) && (!slash || (slash[1] && strspn(slash + 1, "0123456789") == strlen(slash + 1) && atoi(slash + 1) <= (strchr(address, ':') ? 128 : 32)));
    }
    if (strcmp(lingo, "*") && !entry_of(lingo)) { *why = fmt(a, "There's no lingo called %s here", lingo); return NULL; }
    if (!known) { *why = "Who is public, me, my devices, a handle like james, or ip and an address like ip 192.168.1.20 or ip 10.0.0.0/8"; return NULL; }
    if (route && (route[0] != '/' || strpbrk(route, "?# "))) { *why = "A route starts with /, like /share"; return NULL; }
    rec_set(a, rule, "who", v_cstr(a, who));
    rec_set(a, rule, "lingos", v_cstr(a, lingo));
    if (route) { val *routes = v_list(a); list_push(a, routes, v_cstr(a, route)); rec_set(a, rule, "routes", routes); }
    if (!access || access->k != V_LIST) rec_set(a, over, "access", access = v_list(a));
    list_push(a, access, rule);
    if (!keep_json("policy.json", local)) { *why = "This computer couldn't save policy.json"; return NULL; }
    out = policy(a);
  } else if (!strcmp(verb, "unshare")) {
    const char *who = text_or(v, "who", "");
    val *access = rec_get(over, "access"), *kept = v_list(a);
    for (size_t i = 0; access && access->k == V_LIST && i < access->l.n; i++)
      if (strcmp(text_or(access->l.v[i], "who", ""), who) || strcmp(text_or(access->l.v[i], "lingos", ""), lingo)) list_push(a, kept, access->l.v[i]);
    if (!access || access->k != V_LIST || kept->l.n == access->l.n) { *why = fmt(a, "Nothing on this computer shares %s with %s", lingo, who); return NULL; }
    rec_set(a, over, "access", kept);
    if (!keep_json("policy.json", local)) { *why = "This computer couldn't save policy.json"; return NULL; }
    out = policy(a);
  } else if (!strcmp(verb, "ungrant")) {
    const char *need = text_or(v, "need", "");
    val *grants = rec_get(over, "grants"), *mine = rec_get(grants, lingo), *kept = v_list(a);
    for (size_t i = 0; mine && mine->k == V_LIST && i < mine->l.n; i++)
      if (mine->l.v[i]->k != V_TEXT || strcmp(mine->l.v[i]->t.s, need)) list_push(a, kept, mine->l.v[i]);
    if (!mine || mine->k != V_LIST || kept->l.n == mine->l.n) { *why = fmt(a, "%s has no grant %s on this computer", lingo, need); return NULL; }
    rec_set(a, grants, lingo, kept);
    if (!keep_json("policy.json", local)) { *why = "This computer couldn't save policy.json"; return NULL; }
    out = policy(a);
  } else if (!strcmp(verb, "grant")) {
    const char *need = text_or(v, "need", "");
    size_t w = strcspn(need, " ");
    int k = 0;
    while (NEEDS[k] && (strlen(NEEDS[k]) != w || strncmp(NEEDS[k], need, w))) k++;
    if (!named(lingo)) { *why = "Say which lingo, like photos"; return NULL; }
    if (!NEEDS[k] || (need[w] && (k < 2 || k > 3 || !need[w + 1])))
      { *why = "A grant is a need, like run or screen.read, or files.read or files.write and a path, like files.read C:\\Users\\me\\Pictures"; return NULL; }
    if (!granting(a, lingo, need)) { *why = "This computer couldn't save policy.json"; return NULL; }
    out = policy(a);
  } else if (!strcmp(verb, "ceiling")) {
    val *needs = rec_get(v, "needs");
    int good = needs && needs->k == V_LIST;
    for (size_t i = 0; good && i < needs->l.n; i++) {
      int k = 0;
      while (NEEDS[k] && !(needs->l.v[i]->k == V_TEXT && !strcmp(NEEDS[k], needs->l.v[i]->t.s))) k++;
      good = NEEDS[k] != NULL;
    }
    if (!good) { *why = "The ceiling is a list of needs: net, store, files.read, files.write, screen.read, screen.act and run"; return NULL; }
    rec_set(a, local, "ceiling", needs);
    if (!keep_json("policy.json", local)) { *why = "This computer couldn't save policy.json"; return NULL; }
    out = policy(a);
  } else if (!strcmp(verb, "call")) {
    const char *fn = text_or(v, "fn", "");
    val *given = rec_get(v, "values");
    if (!(out = core_call(a, fn, given && given->k == V_LIST ? given : values, &stop, &code, &message)))
      { *why = !strcmp(code, "unknown") ? fmt(a, "There's no function or phrase called %s here, like memo.recall", fn) : message; return NULL; }
  } else { *why = fmt(a, "There's nothing called %s here. It does start, stop, asks, answer, settings, set, manifest, policy, share, unshare, grant, ungrant, ceiling and call", verb); return NULL; }
  return out;
}

////////// 9. HOOKS //////////

/* 9. The account's policy as this computer keeps it, home/policy.account.json: device first, then the policy as it came.
   A version older than the one kept is refused. Every grant of this device's that the ceiling refuses is given back, a
   record by lingo, so whoever handed it in can say what was clamped. */
val *lingo_policy(arena *a, val *account, const char *device, const char **why) {
  val *kept = home_json(a, "policy.account.json"), *v = rec_get(account, "version"), *was = rec_get(kept, "version"), *out = v_rec(a), *clamped = v_rec(a);
  if (!v || v->k != V_NUM) { *why = "The account's policy has no version"; return NULL; }
  if (was && was->k == V_NUM && v->n < was->n) { *why = fmt(a, "The account's policy %g is older than the one kept here, %g", v->n, was->n); return NULL; }
  rec_set(a, out, "device", v_cstr(a, device));
  for (size_t i = 0; i < account->r.n; i++) if (strcmp(account->r.key[i], "device")) rec_set(a, out, account->r.key[i], account->r.v[i]);
  if (!keep_json("policy.account.json", out)) { *why = "This computer couldn't save policy.account.json"; return NULL; }
  val *now = rec_get(policy(a), "grants"), *mine = rec_get(rec_get(account, "grants"), device);
  for (size_t i = 0; mine && mine->k == V_REC && i < mine->r.n; i++)
    for (size_t k = 0; mine->r.v[i]->k == V_LIST && k < mine->r.v[i]->l.n; k++) {
      val *g = mine->r.v[i]->l.v[k], *list = rec_get(clamped, mine->r.key[i]);
      if (g->k != V_TEXT || has(rec_get(now, mine->r.key[i]), g->t.s)) continue;
      if (!list) rec_set(a, clamped, mine->r.key[i], list = v_list(a));
      list_push(a, list, g);
    }
  say("The account's policy %g is in force here", v->n);
  return clamped;
}

// 9. The seed of this device's key called name, in home/keys, made and sealed first when missing and make is set.
static int seed_of(const char *name, int make, unsigned char seed[32]) {
  buf path = { 0 }, got = { 0 };
  buf_fmt(&path, "%s/keys", set.home);
  int ok = named(name);
  if (ok) {
    if (make) pl_folder(path.s);
    buf_fmt(&path, "/%s", name);
    ok = pl_unseal(path.s, &got) && got.n == 32;
  }
  if (ok) memcpy(seed, got.s, 32);
  else if (named(name) && make) {
    pl_random(seed, 32);
    ok = pl_seal(path.s, seed, 32);
  }
  if (got.s) memset(got.s, 0, got.n);
  buf_free(&path), buf_free(&got);
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
// the new catalog. 1 when an access rule written in the policy in force lets in public or an ip, so the port opens to
// other computers. A lingo's own default rule never opens it: it is reached through what connects it, like Onsay.
int core_look(void) {
  arena *a = arena_new();
  unsigned long long now;
  val *access = rec_get(policy(a), "access");
  int outside = 0;
  for (size_t i = 0; i < access->l.n; i++) {
    const char *who = text_or(access->l.v[i], "who", "");
    outside |= !rec_get(access->l.v[i], "default") && (!strcmp(who, "public") || !strncmp(who, "ip ", 3));
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
    header_add(a, out, h->r.key[i], strlen(h->r.key[i]), v ? v->t.s : b.s ? b.s : "", v ? v->t.len : b.n);
    buf_free(&b);
  }
  return out;
}

// Part of a query with its %XX and + undone, kept as it came when that isn't text.
static val *unescape(arena *a, const char *s, size_t n) {
  buf b = { 0 };
  for (size_t i = 0; i < n; i++) {
    char x = i + 2 < n ? s[i + 1] : 0, y = i + 2 < n ? s[i + 2] : 0;
    int hi = s[i] == '%' ? hex(x >= 'a' ? x - 32 : x) : -1, lo = hex(y >= 'a' ? y - 32 : y);
    if (hi >= 0 && lo >= 0) { buf_chr(&b, (char)(hi * 16 + lo)); i += 2; }
    else buf_chr(&b, s[i] == '+' ? ' ' : s[i]);
  }
  val *v = utf8_ok(b.s ? b.s : "", b.n) ? v_text(a, b.s ? b.s : "", b.n) : v_text(a, s, n);
  buf_free(&b);
  return v;
}

// A function's listing, if the lingo has one by that id, like memo.recall of memo or window.list of windows.
static val *listed(const char *lingo, const char *id) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), lingo), "phrases");
  for (size_t i = 0; own && i < own->l.n; i++) if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  return NULL;
}

void core_serve(const char *text, size_t n, const char *given, buf *out) {
  arena *a = arena_new();
  const char *problem = NULL, *lingo = NULL, *why = NULL;
  val *m = json_parse(a, text, n, NULL), *query = v_rec(a), *head = v_rec(a), *got = NOTHING, *values = v_list(a), *f = NULL, *pol = policy(a);
  if (!m || m->k != V_REC) m = v_rec(a);
  val *headers = header_names(a, rec_get(m, "headers")), *body = rec_get(m, "body");
  const char *p = text_or(m, "path", ""), *method = text_or(m, "method", "GET"), *ip = given ? NULL : text_or(m, "ip", ""), *refused = NULL;
  caller c = { "this computer", ip, NULL, 0, 0, 0 };
  int status = 200, json = 1;   // json: the body is written as JSON
  size_t at = strcspn(p, "?"), best = 0;
  buf id = { 0 }, error = { 0 }, call = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  if (!body || body->k != V_TEXT) body = v_cstr(a, "");
  serving++;

  /* The caller. A request to the port from 127.0.0.1 or ::1 is this computer's when it names this computer as its host
     and no page elsewhere sent it, since a page in a browser here can send requests too. One handed in is me, the
     account's own, or by its address. Anyone else who signed it is its signer, and a signature that doesn't check out
     is a 401. */
  if (given && !strcmp(given, "me")) c.me = 1, c.name = "me";
  else if (given) c.ip = !strncmp(given, "ip ", 3) ? given + 3 : "";
  else if (!strcmp(ip, "::1") || !strncmp(ip, "127.", 4)) {
    const char *host = text_or(headers, "host", ""), *origin = text_or(headers, "origin", NULL), *site = text_or(headers, "sec-fetch-site", "none");
    size_t end = host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":");
    c.here = (!end || (((end == 9 && (!strncmp(host, "127.0.0.1", 9) || !strncmp(host, "localhost", 9))) || (end == 5 && !strncmp(host, "[::1]", 5))) &&
                     (!host[end] || host[end] == ':'))) &&
           !(origin && (strncmp(origin, "http://", 7) || strcmp(origin + 7, host))) && (!strcmp(site, "none") || !strcmp(site, "same-origin"));
  }
  if (!c.here && !c.me) c.name = fmt(a, "ip %s", c.ip);
  if (!c.here && !c.me && rec_get(headers, "signature-input")) refused = signer(a, headers, method, p, body, &c);
  for (const char *q = p + at + (p[at] == '?'); *q;) {
    size_t len = strcspn(q, "&"), eq = strcspn(q, "=&");
    if (len) rec_set(a, query, unescape(a, q, eq)->t.s, unescape(a, q + eq + (eq < len), eq < len ? len - eq - 1 : 0));
    q += len + (q[len] == '&');
  }
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *route = route_of(a, e);
    size_t r = strlen(route);
    if (!rec_get(e, "error") && truthy(rec_get(e, "running")) && r > best && at >= r && !strncmp(p, route, r) && (at == r || p[r] == '/')) lingo = text_or(e, "name", ""), best = r;
  }

  if (p[0] != '/') status = 400, buf_str(&error, "A path starts with /, like /memo/recall");
  else if (truthy(rec_get(m, "base64") ? rec_get(m, "base64") : NOTHING) || !utf8_ok(body->t.s, body->t.len))
    status = 400, buf_str(&error, "Its body isn't text, and lingos take text, like JSON");
  else if (refused) status = 401, buf_str(&error, refused);
  else if (!strncmp(p, "/.lingo/", 8) && c.here) {
    // Control, for an app on this computer only: never one handed in, since nothing from elsewhere may change what runs here.
    val *given = strcmp(method, "GET") && body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : query;
    if (problem || !given || given->k != V_REC) status = 400, buf_str(&error, "Its values are a JSON record by name, like {\"lingo\": \"memo\"}");
    else if (!(got = core_control(a, v_text(a, p + 8, at - 8)->t.s, given, &why))) status = 400, buf_str(&error, why), got = NOTHING;
  } else if (at == 1) {
    // Every lingo for this computer, running or not, and a broken one with why. Anyone else, each they may reach.
    val *list = v_list(a);
    for (size_t i = 0; i < found->l.n; i++) {
      val *e = found->l.v[i], *one = v_rec(a);
      const char *name = text_or(e, "name", "");
      int runs = truthy(rec_get(e, "running"));
      if (!c.here && (rec_get(e, "error") || !runs || !let_in(a, pol, &c, name, NULL, &why))) continue;
      rec_set(a, one, "name", v_cstr(a, name));
      if (!rec_get(e, "error")) rec_set(a, one, "route", v_cstr(a, route_of(a, e)));
      rec_set(a, one, "running", VBOOL(runs));
      if (rec_get(e, "error")) rec_set(a, one, "error", rec_get(e, "error"));
      list_push(a, list, one);
    }
    rec_set(a, got = v_rec(a), "lingos", list);
  } else if (!lingo) status = 404, buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
  else {
    // /name alone is name.main, /name/function name.function, and a native's /name/group/function group.function.
    const char *rest = p + best, *name = rest + (at > best), *was_caller = job.caller, *was_lingo = job.lingo, *was_fn = job.fn, *was_path = job.path, *was_id = job.id;
    val *was_needs = job.needs, *was_policy = job.policy, *was_answers = job.answers;
    size_t left = at - best, nl = left ? left - 1 : 0;
    int in = c.here || let_in(a, pol, &c, lingo, v_text(a, rest, left)->t.s, &why);
    job.caller = c.name, job.lingo = lingo, job.path = v_text(a, p, at)->t.s, job.policy = pol, job.answers = v_list(a), job.id = id.s;
    if (!c.here) decided(NULL, in, why);
    buf_fmt(&call, "%s.serve", lingo);
    if (!in) status = 404, buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
    else if (listed(lingo, call.s)) {
      val *request = v_rec(a), *parsed = body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : NOTHING;
      rec_set(a, request, "method", v_cstr(a, method));
      rec_set(a, request, "path", left ? v_text(a, rest, left) : v_cstr(a, "/"));
      rec_set(a, request, "query", query);
      rec_set(a, request, "headers", headers);
      rec_set(a, request, "body", body);
      rec_set(a, request, "json", problem ? NOTHING : parsed);
      list_push(a, values, request);
    } else {
      call.n = 0;
      if (!nl) buf_fmt(&call, "%s.main", lingo);
      else if (memchr(name, '/', nl)) buf_add(&call, name, nl);
      else buf_fmt(&call, "%s.%.*s", lingo, (int)nl, name);
      for (size_t i = 0; i < call.n; i++) if (call.s[i] == '/') call.s[i] = '.';
      val *given = strcmp(method, "GET") ? body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : v_rec(a) : query;
      f = listed(lingo, call.s);
      val *params = f ? rec_get(f, "params") : NULL;
      if (!f) status = 404, buf_fmt(&error, "%s has no function %s", lingo, call.s);
      else if (problem || (given->k != V_REC && given->k != V_LIST)) status = 400, buf_str(&error, "Its values are a JSON record by name, like {\"text\": \"hello\"}");
      else if (given->k == V_LIST) values = given;
      else {
        // In the function's order, nothing for one left out, so its default applies.
        for (size_t i = 0; i < params->l.n; i++) {
          val *v = rec_get(given, text_or(params->l.v[i], "name", ""));
          list_push(a, values, v ? v : NOTHING);
        }
        for (size_t k = 0; k < given->r.n && !error.n; k++) {
          int known = 0;
          for (size_t i = 0; i < params->l.n && !known; i++) known = !strcmp(given->r.key[k], text_or(params->l.v[i], "name", ""));
          if (!known) status = 400, buf_fmt(&error, "%s doesn't take a value called %s", call.s, given->r.key[k]);
        }
      }
    }
    if (!error.n) {
      // The request is the running job: a stop with its id ends it, and each lingo(...) it asks says which function asks.
      char was_running[sizeof running];
      const char *code = NULL, *message = NULL;
      double started = pl_now();
      job.fn = call.s;
      job.needs = said_needs(a, manifest_of(lingo), call.s);
      pl_enter(gate);
      int was_stopping = stopping;
      memcpy(was_running, running, sizeof running);
      snprintf(running, sizeof running, "%s", id.s);
      stopping = 0;
      pl_leave(gate);
      got = core_call(a, call.s, values, &stopping, &code, &message);
      pl_enter(gate);
      memcpy(running, was_running, sizeof running);
      stopping = was_stopping;
      pl_leave(gate);
      say("%s answered in %.0f ms", call.s, (pl_now() - started) * 1000);
      if (!got) status = !strcmp(code, "unknown") ? 404 : !strcmp(code, "denied") ? 403 : 500, buf_str(&error, message), got = NOTHING;
      // What serve gives: a record with a number status is the response, and anything else the body of a 200.
      else if (!f) {
        val *s = got->k == V_REC ? rec_get(got, "status") : NULL;
        if (!s || s->k != V_NUM) json = got->k != V_TEXT;
        else if (s->n < 100 || s->n > 599 || s->n != (int)s->n) status = 500, buf_str(&error, "serve gave a status that isn't one, like 200 or 404");
        else {
          status = (int)s->n, head = header_names(a, rec_get(got, "headers"));
          got = rec_get(got, "body") ? rec_get(got, "body") : NOTHING;
          json = got->k != V_TEXT && got->k != V_NOTHING;
        }
      }
    }
    job.caller = was_caller, job.lingo = was_lingo, job.fn = was_fn, job.path = was_path, job.needs = was_needs, job.policy = was_policy, job.answers = was_answers, job.id = was_id;
  }

  serving--;
  if (error.n) rec_set(a, got = v_rec(a), "error", v_text(a, error.s, error.n)), json = 1;
  buf b = { 0 };
  if (json) buf_json(&b, got); else buf_text(&b, got);
  if (!rec_get(head, "content-type")) rec_set(a, head, "content-type", v_cstr(a, json ? "application/json" : "text/plain; charset=utf-8"));
  buf_fmt(out, "{\"id\":%s,\"status\":%d,\"headers\":", id.s, status);
  buf_json(out, head);
  buf_str(out, ",\"body\":");
  buf_json_str(out, b.s ? b.s : "", b.n);
  buf_str(out, ",\"base64\":false}");
  buf_free(&b), buf_free(&id), buf_free(&error), buf_free(&call);
  arena_free(a);
}

////////// 11. BEGINNING, AND PACKING //////////

int core_begin(int serves) {
  live = serves;
  gate = pl_gate_new();
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
  buf zip = { 0 }, files[2] = { { 0 }, { 0 } }, path = { 0 };
  buf_str(&path, folder);
  while (path.n > 1 && (path.s[path.n - 1] == '/' || path.s[path.n - 1] == '\\')) path.s[--path.n] = 0;
  base = path.s + path.n;
  while (base > path.s && base[-1] != '/' && base[-1] != '\\') base--;
  val *e = load(path.s) ? entry_of(base) : NULL;
  int ok = e && rec_get(e, "fresh") && !rec_get(e, "error");
  if (!e || !rec_get(e, "fresh")) say("%s isn't a folder of a lingo's files: its program, like %s.ls, and its openapi.json", folder, base);
  else if (!ok) say("%s can't be packed: %s", base, text_or(e, "error", ""));
  if (ok) {
    const char *names[2] = { "openapi.json", text_or(e, "program", "") };
    buf_json(&files[0], rec_get(e, "manifest"));
    ok = pl_read(fmt(a, "%s/%s", path.s, names[1]), &files[1]);
    zip_make(&zip, 2, names, files);
    if (to) pl_folder(to);
    const char *into = to ? fmt(a, "%s/%s.lingo", to, base) : fmt(a, "%s.lingo", path.s);
    ok = ok && pl_write(into, zip.s, zip.n);
    if (ok) say("Packed %s into %s", base, into);
    else say("%s couldn't be written", into);
  }
  buf_free(&zip), buf_free(&files[0]), buf_free(&files[1]), buf_free(&path);
  arena_free(a);
  return !ok;
}
