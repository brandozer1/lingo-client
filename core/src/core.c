// core.c - the lingo client's engine: its settings, the lingos folder it watches and loads, and every request it answers.
//
// 1. Settings        the settings file, read once
// 2. Floor           the compute, store and http phrases every core has, and lingo(...) and use(...), asked of Onsay
// 3. Lingos          Lingo lingos, a file or a folder each, and native ones, a folder each: loading them and the catalog
// 4. Messages        what a message from Onsay is, and stopping the running request
// 5. Calls           one function of a lingo, run
// 6. Lingo settings  setting(...), settings.get and settings.set, kept on this computer, never on Onsay
// 7. Connection      hello with the catalog, and looking at the lingos folder again
// 8. Requests        web requests, from this computer's port or Onsay's connection, each answered by the lingo at its route
// 9. Beginning
// It never starts a thread or touches the connection (see core.h), but core_heard may run on another thread, so the
// running request's stop is kept behind one gate.
#include "core.h"
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define say core_say

static core_send sender;     // the host's way to Onsay
static core_ask asker;       // and to Onsay, for lingo(...) and use(...)
static char calling[128];    // the function the running request is of, like agent.ask, told with each lingo(...) it asks

////////// 1. SETTINGS //////////

static core_settings set;

// Lower case letters, digits, _ and -, as a lingo's name and a settings file's are.
static int named(const char *s) { return *s && strlen(s) <= 64 && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(s); }

// 1. Kept in their own arena for as long as the core runs.
const core_settings *core_read(const char *text, size_t n) {
  arena *kept = arena_new();
  const char *problem = NULL;
  val *s = json_parse(kept, text, n, &problem), *allow, *port;
  if (problem || s->k != V_REC) { say("The settings aren't a JSON record: %s", problem ? problem : "they're another kind"); return NULL; }
  set.all = s;
  set.token = *text_or(s, "token", "") ? text_or(s, "token", "") : NULL;
  set.home = text_or(s, "home", "onsay-data");
  set.lingos = text_or(s, "lingos", NULL);
  if (!set.lingos) {
    char *folder = arena_get(kept, strlen(set.home) + 8);
    sprintf(folder, "%s/lingos", set.home);
    set.lingos = folder;
  }
  port = rec_get(s, "port");
  set.port = port && port->k == V_NUM ? (int)port->n : 6969;
  set.network = !strcmp(text_or(s, "listen", ""), "network");
  set.inbound = !strcmp(text_or(s, "inbound", ""), "onsay");
  set.steps = rec_get(s, "steps") && rec_get(s, "steps")->k == V_NUM ? (long long)rec_get(s, "steps")->n : 5000000;
  set.ms = rec_get(s, "ms") && rec_get(s, "ms")->k == V_NUM ? rec_get(s, "ms")->n : 600000;
  allow = rec_get(s, "allow");
  set.allow = arena_get(kept, ((allow && allow->k == V_LIST ? allow->l.n : 0) + 1) * sizeof *set.allow);
  for (size_t i = 0; allow && allow->k == V_LIST && i < allow->l.n; i++) if (allow->l.v[i]->k == V_TEXT) set.allow[set.allowed++] = allow->l.v[i]->t.s;
  return &set;
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

// http.send: one web request, made by the computer itself (pl_http), its method a word and each header one whole line.
static val *send_(run *r, val **a) {
  buf method = { 0 }, url = { 0 }, body = { 0 }, why = { 0 };
  val *headers = v_rec(r->a), *got = NULL;
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
  if (!why.n) got = pl_http(r->a, method.s, url.s, headers, body.s ? body.s : "", body.n, seconds, &why);
  val *problem = v_text(r->a, why.s ? why.s : "", why.n);
  buf_free(&method), buf_free(&url), buf_free(&body), buf_free(&why);
  if (!got) fail(r, "phrase", "%s", problem->t.s);
  return got;
}

// lingo(...) and use(...) as a program reaches them: asked of Onsay through the host, which waits for the answer.
static val *asked(run *r, buf *in) {
  buf out = { 0 };
  int answered = asker(in->s, in->n, r->stop, &out);
  buf_free(in);
  val *got = answered ? json_parse(r->a, out.s ? out.s : "", out.n, NULL) : NULL;
  buf_free(&out);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got || got->k != V_REC) fail(r, "phrase", "This lingo client isn't connected to Onsay");
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
  if (*calling) { buf_str(&in, ",\"asking\":"); buf_json_str(&in, calling, strlen(calling)); }
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

static val *setting_(run *r, val **a), *settings_get(run *r, val **a), *settings_set(run *r, val **a);   // 6.

// lingo.run, lingo.use and lingo.setting are lingo(...), use(...) and setting(...), the language's own, in no listing.
static void floor_phrases(void) {
  phrase_add("compute", "none", "now() -> number: seconds since 1970", time_);
  phrase_add("compute", "none", "wait(seconds): at most 60", wait_);
  phrase_add("compute", "none", "log(x): a line in the lingo client's log", log_);
  phrase_add("store", "none", "get(key) -> text?: Onsay's own storage on this computer", get);
  phrase_add("store", "none", "put(key, value): kept as text", put);
  phrase_add("store", "none", "keys(prefix=\"\") -> [text]: sorted", keys);
  phrase_add("store", "none", "delete(key)", delete_);
  phrase_add("http", "net", "send(method, url, headers=nothing, body=nothing, timeout=30) -> {status,headers,text}: one request, "
             "headers a record, body text, redirects followed, at most 4 MB back", send_);
  phrase_add("settings", "none", "get(lingo) -> [record]: every setting of a lingo, as its lingo.json and its code say, each with "
             "its value now", settings_get);
  phrase_add("settings", "none", "set(lingo, key, value): kept wherever that lingo keeps its settings", settings_set);
  phrase_add("lingo", "none", "run(ask, values=nothing, gives=\"\")", lingo_run);
  phrase_add("lingo", "none", "use(address, values=nothing)", lingo_use);
  phrase_add("lingo", "none", "setting(key, reference)", setting_);
}

////////// 3. LINGOS //////////

// Core is the only lingo built in. A lingo written in Lingo is one file in the lingos folder, like memo.lingo, or a folder
// with its lingo.json beside it, like agent/agent.lingo, its listing made from its code. A native lingo, a library like
// windows.dll, is a folder with a lingo.json. One that can't load is set aside with its reason, and the rest still load.
// Onsay is told what each one shares, never its code (7. CONNECTION).
static arena *library;
static val *device;                  // the compiler's instance of every lingo that loaded
static val *catalog;                 // every lingo, whether it works, and what it shares
static val *folders;                 // each lingo as read: {file, code}, {folder, manifest} or both, with error if it failed
static unsigned long long looked;    // a fingerprint of the lingos folder when it was last read
#define FNV 14695981039346656037ULL

// Every native lingo by its number, core first, with the folder it came from. A library is loaded once and kept, since
// its phrases are in the table from then on: a changed one is used after a restart.
static struct { const char *folder, *name; } natives[64] = { { NULL, "core" } };
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

typedef struct { arena *a; val *names; unsigned long long *print; } found;

// Each folder, and each .lingo file, fingerprinted as it is listed.
static void collect(const char *name, int folder, double size, double changed, void *arg) {
  found *f = arg;
  size_t len = strlen(name);
  if (name[0] == '.' || (!folder && (len <= 6 || strcmp(name + len - 6, ".lingo")))) return;
  list_push(f->a, f->names, v_cstr(f->a, name));
  print_file(name, folder, size, changed, f->print);
}

// What is in the lingos folder now, and a fingerprint of every file of it: names, sizes and times, so any change shows.
static val *look(arena *a, unsigned long long *print) {
  *print = FNV;
  found f = { a, v_list(a), print };
  pl_list(set.lingos, collect, &f);
  for (size_t i = 0; i < f.names->l.n; i++) {
    buf path = { 0 };
    buf_fmt(&path, "%s/%s", set.lingos, f.names->l.v[i]->t.s);
    pl_list(path.s, print_file, print);   // nothing for a file
    buf_free(&path);
  }
  return f.names;
}

// Sets one aside, with why.
static void set_aside(arena *a, val *e, const char *why) {
  rec_set(a, e, "error", v_cstr(a, why));
  say("Lingo %s can't be used: %s", text_or(e, "folder", text_or(e, "file", "")), why);
}

// One name in the lingos folder: a .lingo file as {file, code}, a folder as {folder, manifest}, and the folder of a lingo
// written in Lingo, its code beside its lingo.json as <name>.lingo, as both. NULL for a folder with no lingo.json, which
// isn't a lingo. One whose lingo.json says its settings wrongly is set aside.
static val *read_lingo(arena *a, const char *name) {
  buf path = { 0 }, text = { 0 };
  size_t len = strlen(name);
  int file = len > 6 && !strcmp(name + len - 6, ".lingo");
  if (file) buf_fmt(&path, "%s/%s", set.lingos, name); else buf_fmt(&path, "%s/%s/lingo.json", set.lingos, name);
  val *e = NULL;
  if (pl_read(path.s, &text)) {
    e = v_rec(a);
    rec_set(a, e, file ? "file" : "folder", v_cstr(a, name));
    const char *problem = NULL;
    val *m = file ? NULL : json_parse(a, text.s, text.n, &problem);
    if (file) rec_set(a, e, "code", v_text(a, text.s, text.n));
    else if (problem || m->k != V_REC) set_aside(a, e, "its lingo.json isn't a JSON record");
    else {
      rec_set(a, e, "manifest", m);
      path.n = text.n = 0;
      buf_fmt(&path, "%s/%s/%s.lingo", set.lingos, name, name);
      if (strcmp(text_or(m, "runs", ""), "native") && pl_read(path.s, &text)) {
        rec_set(a, e, "file", v_text(a, path.s + strlen(set.lingos) + len + 2, len + 6));
        rec_set(a, e, "code", v_text(a, text.s, text.n));
      }
      val *settings = rec_get(m, "settings"), *keep = rec_get(m, "keep");
      int listed = !settings || settings->k == V_LIST;
      for (size_t i = 0; listed && settings && i < settings->l.n; i++) listed = settings->l.v[i]->k == V_TEXT || text_or(settings->l.v[i], "name", NULL);
      if (!listed) set_aside(a, e, "its settings are a list, each a name or a record with its name, like {\"name\": \"key\", \"secret\": true}");
      else if (keep && (!text_or(keep, "read", NULL) || !text_or(keep, "write", NULL)))
        set_aside(a, e, "keep names its own functions that read and write its settings, like {\"read\": \"read\", \"write\": \"write\"}");
      else if (!rec_get(e, "file") && strcmp(text_or(m, "runs", ""), "native"))
        set_aside(a, e, "its code goes beside its lingo.json, named for its folder, like agent/agent.lingo, or its runs is native, with its library");
    }
  }
  buf_free(&path);
  buf_free(&text);
  return e;
}

// The native lingo already called name, or -1.
static int native_called(const char *name) {
  for (size_t i = 0; i < native_total; i++) if (!strcmp(natives[i].name, name)) return (int)i;
  return -1;
}

// A lingo's lingo.json, a native's from the folder it loaded from, or NULL when it has none.
static val *manifest_of(const char *lingo) {
  int k = native_called(lingo);
  const char *folder = k > 0 ? natives[k].folder : lingo;
  for (size_t i = 0; i < folders->l.n; i++)
    if (!strcmp(text_or(folders->l.v[i], "folder", ""), folder) && !rec_get(folders->l.v[i], "error")) return rec_get(folders->l.v[i], "manifest");
  return NULL;
}

/* A native lingo's lingo.json: its name, which is its folder's, and its library, a file in its folder, like windows.dll.
   Every native folder gets "native", its number: one already loaded is found by its folder, and a new one loads. What
   can't load is set aside. */
static void load_natives(arena *a, val *found) {
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i], *m = rec_get(e, "manifest");
    const char *folder = text_or(e, "folder", ""), *name = text_or(m, "name", ""), *main_ = text_or(m, "library", "");
    size_t k = 1;
    while (k < native_total && strcmp(natives[k].folder, folder)) k++;
    if (k < native_total) { rec_set(a, e, "native", v_num(a, (double)k)); continue; }
    if (!named(name) || strcmp(name, folder)) { set_aside(a, e, "its name is its folder's, in lower case letters, digits, _ and -, like windows"); continue; }
    if (!*main_ || strpbrk(main_, "/\\")) { set_aside(a, e, "library names its library, in its folder, like windows.dll"); continue; }
    if (native_called(name) >= 0) { set_aside(a, e, "another lingo here is already called that"); continue; }
    if (native_total == sizeof natives / sizeof *natives) { set_aside(a, e, "this core can't take more native lingos"); continue; }
    buf path = { 0 }, why = { 0 };
    buf_fmt(&path, "%s/%s/%s", set.lingos, folder, main_);
    lingo_entry entry = pl_library(path.s, &why);
    path.n = 0;
    if (!entry) { buf_fmt(&path, "%s: %s", main_, why.s); set_aside(a, e, path.s); }
    buf_free(&path), buf_free(&why);
    if (!entry) continue;
    // Kept for as long as the core runs, like the library itself.
    natives[native_total].folder = strdup(folder), natives[native_total].name = strdup(name);
    native_begin(native_total);
    entry();
    rec_set(a, e, "native", v_num(a, (double)native_total++));
    say("Lingo %s is loaded from %s", name, main_);
  }
}

static val *declared(arena *a, const char *lingo, val **keep);   // 6.

// Reads everything again: every folder, the natives, the graph and the catalog. Built beside what there was, which stays
// in use when even core's own lingos can't load now. 1 when it loaded.
static int lingos(void) {
  arena *was = library;
  val *was_device = device, *was_catalog = catalog, *was_folders = folders;
  arena *a = library = arena_new();
  val *native_found = v_list(a), *stacked = v_list(a), *offered = v_list(a), *names = look(a, &looked);
  folders = v_list(a);
  for (size_t i = 0; i < names->l.n; i++) {
    val *e = read_lingo(a, names->l.v[i]->t.s);
    if (e) list_push(a, folders, e);
    if (e && !rec_get(e, "error")) list_push(a, rec_get(e, "file") ? stacked : native_found, e);
  }
  // core, then every native whose folder is here now. One whose folder is gone stays loaded, but isn't offered.
  load_natives(a, native_found);
  for (size_t i = 0; i < native_total; i++) {
    int here = i == 0;
    for (size_t k = 0; k < native_found->l.n && !here; k++) { val *n = rec_get(native_found->l.v[k], "native"); here = n && n->n == (double)i; }
    if (!here) continue;
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, natives[i].name));
    rec_set(a, one, "phrases", native_phrases(a, i));
    list_push(a, offered, one);
  }
  // Loaded together. While one is at fault, it is set aside with its reason, and the rest load again without it.
  for (;;) {
    const char *why = NULL;
    device = lingo_load(a, offered, stacked, &why);
    if (device) break;
    size_t k = 0;
    for (; k < stacked->l.n; k++) {
      const char *f = text_or(stacked->l.v[k], "file", "");
      size_t n = strlen(f);
      if (!strncmp(why, f, n) && (why[n] == ':' || why[n] == ' ')) break;
    }
    if (k == stacked->l.n) {
      say("The core's own lingos can't load: %s", why);
      arena_free(a);
      library = was, device = was_device, catalog = was_catalog, folders = was_folders;
      return 0;
    }
    set_aside(a, stacked->l.v[k], why);
    memmove(&stacked->l.v[k], &stacked->l.v[k + 1], (stacked->l.n - k - 1) * sizeof *stacked->l.v);
    stacked->l.n--;
  }

  // Every lingo, what it is, the names of its settings and every function it shares, never its code.
  catalog = v_list(a);
  val *all = rec_get(device, "lingos");
  for (size_t i = 0; i < all->r.n; i++) {
    if (!strcmp(all->r.key[i], "core")) continue;   // every computer's floor, never listed
    val *l = all->r.v[i], *one = v_rec(a), *mine = declared(a, all->r.key[i], NULL), *settings = v_list(a), *phrases = v_list(a);
    val *about = rec_get(l, "native") ? rec_get(manifest_of(all->r.key[i]), "about") : rec_get(l, "about"), *own = rec_get(l, "phrases");
    for (size_t k = 0; k < mine->l.n; k++) list_push(a, settings, rec_get(mine->l.v[k], "name"));
    for (size_t k = 0; k < own->l.n; k++) {
      val *p = own->l.v[k], *shared = v_rec(a);
      rec_set(a, shared, "id", rec_get(p, "id"));
      rec_set(a, shared, "does", rec_get(p, "does"));
      rec_set(a, shared, "inputSchema", rec_get(p, "inputSchema"));
      rec_set(a, shared, "lingos", rec_get(p, "lingos") ? rec_get(p, "lingos") : v_num(a, 0));
      list_push(a, phrases, shared);
    }
    rec_set(a, one, "name", rec_get(l, "name"));
    rec_set(a, one, "status", v_cstr(a, "ok"));
    if (about && about->k == V_TEXT) rec_set(a, one, "about", about);
    rec_set(a, one, "settings", settings);
    rec_set(a, one, "phrases", phrases);
    if (rec_get(l, "native")) rec_set(a, one, "native", VBOOL(1));
    list_push(a, catalog, one);
  }
  for (size_t i = 0; i < folders->l.n; i++) {
    val *e = folders->l.v[i], *one = v_rec(a), *f = rec_get(e, "file");
    if (!rec_get(e, "error")) continue;
    rec_set(a, one, "name", rec_get(e, "folder") ? rec_get(e, "folder") : v_text(a, f->t.s, f->t.len - 6));
    rec_set(a, one, "status", v_cstr(a, "broken"));
    rec_set(a, one, "error", rec_get(e, "error"));
    list_push(a, catalog, one);
  }
  say("%zu lingos are ready", all->r.n - 1);
  arena_free(was);
  return 1;
}

////////// 4. MESSAGES //////////

static pl_gate *gate;             // keeps the two below, which core_heard may set from another thread while a request runs
static volatile int stopping;     // the running request's stop flag
static char running[128];         // the running request's id, as JSON, so a stop can name it

void core_say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "onsay: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// 4. A stop is handled here, at once, even while a request runs on another thread.
int core_heard(const char *text, size_t n) {
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL);
  const char *type = text_or(m, "type", "");
  int kind = !strcmp(type, "pong") ? CORE_PONG : !strcmp(type, "stop") ? CORE_DONE : CORE_JOB;
  if (kind == CORE_DONE) {
    buf id = { 0 };
    buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    pl_enter(gate);
    if (!strcmp(id.s, running)) stopping = 1;
    pl_leave(gate);
    buf_free(&id);
  }
  arena_free(a);
  return kind;
}

////////// 5. CALLS //////////

// 5. fn, a function of a lingo here, linked from its code and run with its values in order, a value left out taking its
// default. Its value, or NULL with the error's code and message.
val *core_call(arena *a, const char *fn, val *values, volatile int *stop, const char **code, const char **message) {
  val *program = lingo_call(a, device, fn), *input = v_rec(a);
  char name[24];
  if (!program) { *code = "unknown", *message = v_cstr(a, fn)->t.s; return NULL; }
  for (size_t i = 0; i < values->l.n; i++) {
    snprintf(name, sizeof name, "a%zu", i);
    rec_set(a, input, name, values->l.v[i]);
  }
  return run_program(a, program, input, (limits){ set.steps, set.ms, stop, (const char *const *)set.allow, set.allowed }, code, message);
}

////////// 6. LINGO SETTINGS //////////

// A lingo's settings are kept on this computer, never on Onsay: in home/settings/<lingo>.json, a record by name, or by
// the lingo's own functions when its lingo.json says keep: {read, write}, so its maker decides where they live.
// setting(...) is lingo.setting(key, reference), its value the one kept, else its lingo.json's default, else nothing.

static const char *keeping;   // the lingo whose own read or write runs now, so its setting(...) reads this computer's file

/* One lingo's settings, as a form shows them: each its lingo.json lists, a name alone or a record of its name, title,
   about, default, required and secret, then each other one its code reads, by name. keep, when asked for, is where its
   lingo.json says they are kept, {read, write}, its own functions, or NULL for this computer's file of them, as it is
   inside those functions too. */
static val *declared(arena *a, const char *lingo, val **keep) {
  val *out = v_list(a), *m = manifest_of(lingo), *own = rec_get(rec_get(device, "lingos"), lingo);
  val *listed = m ? rec_get(m, "settings") : NULL, *read = own ? rec_get(own, "settings") : NULL;
  for (size_t i = 0; listed && i < listed->l.n; i++) {
    val *s = listed->l.v[i], *one = v_rec(a);
    if (s->k == V_TEXT) rec_set(a, one, "name", s);
    else for (size_t k = 0; k < s->r.n; k++) rec_set(a, one, s->r.key[k], s->r.v[k]);
    list_push(a, out, one);
  }
  for (size_t i = 0; read && i < read->l.n; i++) {
    int has = 0;
    for (size_t k = 0; k < out->l.n && !has; k++) has = !strcmp(text_or(out->l.v[k], "name", ""), read->l.v[i]->t.s);
    if (!has) { val *one = v_rec(a); rec_set(a, one, "name", read->l.v[i]); list_push(a, out, one); }
  }
  if (keep) *keep = m && !(keeping && !strcmp(keeping, lingo)) ? rec_get(m, "keep") : NULL;
  return out;
}

// The lingo a setting is of, checked, since it names a file, and the setting's name when there is one.
static const char *whose(run *r, val *lingo, val *key) {
  if (lingo->k != V_TEXT || !named(lingo->t.s)) fail(r, "phrase", "a lingo's settings are named by the lingo, like agent");
  if (key && (key->k != V_TEXT || !key->t.len)) fail(r, "phrase", "a setting is named by its name, like key");
  return lingo->t.s;
}

// The settings this computer keeps for a lingo, a record by name, and the file they are in.
static val *stored(run *r, const char *lingo, buf *path) {
  buf text = { 0 };
  buf_fmt(path, "%s/settings/%s.json", set.home, lingo);
  val *s = pl_read(path->s, &text) ? json_parse(r->a, text.s, text.n, NULL) : NULL;
  buf_free(&text);
  return s && s->k == V_REC ? s : v_rec(r->a);
}

// A lingo's own function that keeps its settings, read(key) or write(key, value). Its value.
static val *kept(run *r, const char *lingo, const char *fn, val *values) {
  const char *was = keeping, *code = NULL, *message = NULL;
  buf id = { 0 };
  buf_fmt(&id, "%s.%s", lingo, fn);
  val *name = v_text(r->a, id.s, id.n);
  buf_free(&id);
  keeping = lingo;
  val *got = core_call(r->a, name->t.s, values, r->stop, &code, &message);
  keeping = was;
  if (!got) fail(r, code, "%s, which keeps %s's settings: %s", name->t.s, lingo, !strcmp(code, "unknown") ? "it has no function by that name" : message);
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
  } else {
    buf path = { 0 };
    v = rec_get(stored(r, lingo, &path), a[0]->t.s);
    buf_free(&path);
  }
  for (size_t i = 0; (!v || v->k == V_NOTHING) && i < all->l.n; i++)
    if (!strcmp(text_or(all->l.v[i], "name", ""), a[0]->t.s)) v = rec_get(all->l.v[i], "default");
  return v ? v : NOTHING;
}

// settings.get(lingo), for a form of them: each its lingo.json lists, with what it says, then each other one its code
// reads, each with its value now.
static val *settings_get(run *r, val **a) {
  val *all = declared(r->a, whose(r, a[0], NULL), NULL), *asked[2] = { NULL, a[0] };
  for (size_t i = 0; i < all->l.n; i++) {
    asked[0] = rec_get(all->l.v[i], "name");
    rec_set(r->a, all->l.v[i], "value", setting_(r, asked));
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
  buf path = { 0 }, text = { 0 }, folder = { 0 };
  val *s = stored(r, lingo, &path);
  rec_set(r->a, s, a[1]->t.s, a[2]);
  buf_json(&text, s);
  buf_fmt(&folder, "%s/settings", set.home);
  int ok = pl_folder(folder.s) && pl_write(path.s, text.s, text.n);
  buf_free(&path);
  buf_free(&text);
  buf_free(&folder);
  if (!ok) fail(r, "phrase", "this computer couldn't save the setting");
  return NOTHING;
}

////////// 7. CONNECTION //////////

static int reply(const char *text, size_t n) { return sender && sender(text, n); }

// A message that ends with the catalog, sent.
static void send_catalog(buf *out) {
  buf_str(out, ",\"catalog\":");
  buf_json(out, catalog);
  buf_chr(out, '}');
  reply(out->s, out->n);
  buf_free(out);
}

// When anything in the lingos folder changed: everything read again, and the new catalog sent.
void core_look(void) {
  arena *a = arena_new();
  unsigned long long now;
  look(a, &now);
  arena_free(a);
  if (now == looked || !lingos()) return;
  buf out = { 0 };
  buf_str(&out, "{\"type\":\"catalog\"");
  send_catalog(&out);
}

// Hello, with the catalog, since a new connection may be a server that never had it.
void core_open(void) {
  buf out = { 0 };
  buf_str(&out, "{\"type\":\"hello\",\"token\":");
  buf_json_str(&out, set.token, strlen(set.token));
  buf_str(&out, ",\"client\":\"" CORE_CLIENT "\",\"version\":\"" CORE_VERSION "\"");
  send_catalog(&out);
}

////////// 8. REQUESTS //////////

// A web request reaches the lingo at the longest route its path starts with: the route its lingo.json names, like
// "/onsay", else /<name>. From anywhere but this computer, only a lingo whose lingo.json says "open": true is there,
// unless Onsay sends it for this computer's own account (owner). A lingo with serve(request) gets every request to its
// route, and any other is called as /<name>/<function>, or a native's /<name>/<group>/<function>, its values a JSON
// record by name or a list in order, or the query for a GET. GET / lists the routes the caller may reach.

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
  val *l = rec_get(rec_get(device, "lingos"), lingo), *own = l ? rec_get(l, "phrases") : NULL;
  for (size_t i = 0; own && i < own->l.n; i++) if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  return NULL;
}

void core_serve(const char *text, size_t n, int from, buf *out) {
  arena *a = arena_new();
  const char *problem = NULL, *lingo = NULL, *fn = NULL;
  val *m = json_parse(a, text, n, NULL), *query = v_rec(a), *routes = v_list(a), *head = v_rec(a), *got = NOTHING, *values = v_list(a), *f = NULL;
  if (!m || m->k != V_REC) m = v_rec(a);
  val *headers = header_names(a, rec_get(m, "headers")), *body = rec_get(m, "body");
  const char *p = text_or(m, "path", ""), *method = text_or(m, "method", "GET");
  int status = 200, json = 1, owner = from == CORE_ONSAY && rec_get(m, "owner") && truthy(rec_get(m, "owner"));   // json: the body is written as JSON
  size_t at = strcspn(p, "?"), best = 0;
  buf id = { 0 }, error = { 0 }, call = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  if (!body || body->k != V_TEXT) body = v_cstr(a, "");

  // A page in a browser on this computer can send requests here too: one counts as from this computer only when it
  // names this computer as its host and no page elsewhere sent it. Onsay's own, for this computer's account, does too.
  if (from == CORE_HERE) {
    const char *host = text_or(headers, "host", ""), *origin = text_or(headers, "origin", NULL), *site = text_or(headers, "sec-fetch-site", "none");
    size_t end = host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":");
    int here = !end || (((end == 9 && (!strncmp(host, "127.0.0.1", 9) || !strncmp(host, "localhost", 9))) || (end == 5 && !strncmp(host, "[::1]", 5))) &&
                        (!host[end] || host[end] == ':'));
    if (!here || (origin && (strncmp(origin, "http://", 7) || strcmp(origin + 7, host))) || (strcmp(site, "none") && strcmp(site, "same-origin"))) from = CORE_NETWORK;
  }
  for (const char *q = p + at + (p[at] == '?'); *q;) {
    size_t len = strcspn(q, "&"), eq = strcspn(q, "=&");
    if (len) rec_set(a, query, unescape(a, q, eq)->t.s, unescape(a, q + eq + (eq < len), eq < len ? len - eq - 1 : 0));
    q += len + (q[len] == '&');
  }
  for (size_t i = 0; i < catalog->l.n; i++) {
    val *e = catalog->l.v[i], *mf, *open;
    const char *name = text_or(e, "name", ""), *own;
    if (strcmp(text_or(e, "status", ""), "ok")) continue;
    mf = manifest_of(name), open = mf ? rec_get(mf, "open") : NULL, own = text_or(mf, "route", "");
    if (from != CORE_HERE && !owner && !(open && open->k == V_BOOL && open->b)) continue;
    buf r = { 0 };
    if (own[0] == '/' && own[1] && !strchr(own, '?')) buf_str(&r, own); else buf_fmt(&r, "/%s", name);
    while (r.n > 1 && r.s[r.n - 1] == '/') r.s[--r.n] = 0;
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, name));
    rec_set(a, one, "route", v_text(a, r.s, r.n));
    rec_set(a, one, "open", VBOOL(open && open->k == V_BOOL && open->b));
    list_push(a, routes, one);
    if (r.n > best && at >= r.n && !memcmp(p, r.s, r.n) && (at == r.n || p[r.n] == '/')) lingo = name, best = r.n;
    buf_free(&r);
  }

  if (from == CORE_ONSAY && !owner && !set.inbound) status = 403, buf_str(&error, "This lingo client doesn't take requests through Onsay");
  else if (p[0] != '/') status = 400, buf_str(&error, "A path starts with /, like /memo/recall");
  else if (truthy(rec_get(m, "base64") ? rec_get(m, "base64") : NOTHING) || !utf8_ok(body->t.s, body->t.len))
    status = 400, buf_str(&error, "Its body isn't text, and lingos take text, like JSON");
  else if (at == 1) rec_set(a, got = v_rec(a), "lingos", routes);
  else if (!lingo) status = 404, buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
  else {
    // /name alone is name.main, /name/function name.function, and a native's /name/group/function group.function.
    const char *rest = p + best, *name = rest + (at > best);   // rest: what follows the route, from its /
    size_t left = at - best, nl = left ? left - 1 : 0;
    buf_fmt(&call, "%s.serve", lingo);
    if (listed(lingo, call.s)) {
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
      char was_running[sizeof running], was_calling[sizeof calling];
      const char *code = NULL, *message = NULL;
      double started = pl_now();
      fn = call.s;
      pl_enter(gate);
      int was_stopping = stopping;
      memcpy(was_running, running, sizeof running);
      snprintf(running, sizeof running, "%s", id.s);
      stopping = 0;
      pl_leave(gate);
      memcpy(was_calling, calling, sizeof calling);
      snprintf(calling, sizeof calling, "%s", fn);
      got = core_call(a, fn, values, &stopping, &code, &message);
      memcpy(calling, was_calling, sizeof calling);
      pl_enter(gate);
      memcpy(running, was_running, sizeof running);
      stopping = was_stopping;
      pl_leave(gate);
      say("%s answered in %.0f ms", fn, (pl_now() - started) * 1000);
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
  }

  if (error.n) rec_set(a, got = v_rec(a), "error", v_text(a, error.s, error.n)), json = 1;
  buf b = { 0 };
  if (json) buf_json(&b, got); else buf_text(&b, got);
  if (!rec_get(head, "content-type")) rec_set(a, head, "content-type", v_cstr(a, json ? "application/json" : "text/plain; charset=utf-8"));
  buf_fmt(out, "{\"type\":\"response\",\"id\":%s,\"status\":%d,\"headers\":", id.s, status);
  buf_json(out, head);
  buf_str(out, ",\"body\":");
  buf_json_str(out, b.s ? b.s : "", b.n);
  buf_str(out, ",\"base64\":false}");
  buf_free(&b), buf_free(&id), buf_free(&error), buf_free(&call);
  arena_free(a);
}

// 8. One message core_heard called a job: hello taken, or a request. Gives 1 when it was ready, the sign hello was taken.
int core_do(const char *text, size_t n) {
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL);
  const char *type = text_or(m, "type", "");
  int ready = !strcmp(type, "ready");
  if (ready) say("Signed in. This computer is %s", text_or(m, "device", "?"));
  else if (!strcmp(type, "request")) {
    buf out = { 0 };
    core_serve(text, n, CORE_ONSAY, &out);
    reply(out.s, out.n);
    buf_free(&out);
  } else if (strcmp(type, "lingo")) say("Onsay sent a message this lingo client doesn't know: %s", *type ? type : "not JSON");   // lingo: an answer that came after a stop
  arena_free(a);
  return ready;
}

////////// 9. BEGINNING //////////

int core_begin(core_send send, core_ask ask) {
  sender = send;
  asker = ask;
  gate = pl_gate_new();
  native_begin(0);
  std_phrases();
  floor_phrases();
  return lingos();
}
