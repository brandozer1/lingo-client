// core.c - the Lingo core's engine: hello, jobs and results, and the lingos folder it watches, loads and lists for Onsay.
//
// 1. Settings      onsay.json
// 2. Floor         the compute, store, http and settings phrases every core has
// 3. Lingos        Lingo lingos, a file or a folder each, and native and host ones, a folder each: the catalog and loading
// 4. Messages      what a message is, and stopping the running job
// 5. Jobs          run and call, to a result, lingo(...) asked of Onsay as a program runs
// 6. Lingo settings  setting(...), settings.get and settings.set, kept on this device, never on Onsay
// 7. Connection    hello with the catalog, and looking at the lingos folder again
// 8. Requests      web requests, from this computer's port or Onsay's connection, each answered by the lingo at its route
// 9. Beginning
// It never starts a thread or touches the connection (see core.h), but core_heard may run on another thread, so the
// running job's stop is kept behind one gate.
#include "core.h"
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define say core_say

static core_send sender;     // the host's way to Onsay
static core_ask asker;       // and to Onsay, for lingo.run and lingo.use, or NULL when pl_call answers them
static char calling[128];    // the function the running call is of, like agent.ask, told with each lingo(...) it asks

////////// 1. SETTINGS //////////

static struct {
  const char *token, *name, *home, *lingos;
  const char **allow;
  int allowed, inbound;   // inbound: requests that come down the connection to Onsay are taken too
  long long steps;
  double ms;
} set;

// A record's text or number by key, or the fallback when r isn't a record or the part isn't that kind.
static const char *text_or(val *r, const char *key, const char *fallback) {
  val *v = r && r->k == V_REC ? rec_get(r, key) : NULL;
  return v && v->k == V_TEXT ? v->t.s : fallback;
}

static double num_or(val *r, const char *key, double fallback) {
  val *v = r && r->k == V_REC ? rec_get(r, key) : NULL;
  return v && v->k == V_NUM ? v->n : fallback;
}

// Kept in their own arena for as long as the core runs.
static int settings(const char *text, size_t n) {
  static arena *kept;
  kept = arena_new();
  const char *problem = NULL;
  val *s = json_parse(kept, text ? text : "", text ? n : 0, &problem);
  if (problem || s->k != V_REC) { say("The settings aren't a JSON record: %s", problem ? problem : "they're another kind"); return 0; }
  set.token = text_or(s, "token", NULL);
  set.name = text_or(s, "name", "");
  set.home = text_or(s, "home", "onsay-data");
  set.lingos = text_or(s, "lingos", NULL);
  if (!set.lingos) {
    char *folder = arena_get(kept, strlen(set.home) + 8);
    sprintf(folder, "%s/lingos", set.home);
    set.lingos = folder;
  }
  set.steps = (long long)num_or(s, "steps", 5000000);
  set.ms = num_or(s, "ms", 600000);
  set.inbound = !strcmp(text_or(s, "inbound", ""), "onsay");
  val *allow = rec_get(s, "allow");
  set.allow = arena_get(kept, ((allow && allow->k == V_LIST ? allow->l.n : 0) + 1) * sizeof *set.allow);
  if (allow && allow->k == V_LIST)
    for (size_t i = 0; i < allow->l.n; i++) if (allow->l.v[i]->k == V_TEXT) set.allow[set.allowed++] = allow->l.v[i]->t.s;
  return 1;
}

////////// 2. FLOOR //////////

// compute: the clock, waiting, and a line for the person.

static val *time_(run *r, val **a) { (void)a; return v_num(r->a, pl_now()); }

static val *wait_(run *r, val **a) {
  double s = num_of(r, a[0], NULL), until = pl_now() + (s > 60 ? 60 : s);
  // Short sleeps, so a stop still lands quickly.
  while (pl_now() < until) {
    if (*r->stop || pl_stopped()) fail(r, "stopped", "Stopped by the person");
    double left = until - pl_now();
    pl_sleep(left < 0.05 ? left : 0.05);
  }
  return NOTHING;
}

static val *note_(run *r, val **a) { note(r, a[0]); return NOTHING; }

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
  if (!ok) fail(r, "phrase", "this device couldn't save it");
  return NOTHING;
}

static val *delete_(run *r, val **a) {
  buf path = { 0 };
  file_of(r, a[0], &path);
  int ok = pl_delete(path.s);
  buf_free(&path);
  if (!ok) fail(r, "phrase", "this device couldn't delete it");
  return NOTHING;
}

typedef struct { run *r; val *prefix, *out; } listing;

static void each_key(const char *name, int folder, double size, double changed, void *arg) {
  listing *l = arg;
  (void)size, (void)changed;
  if (folder) return;
  if (name[0] != 'k') return;
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
  // Sorted, so every device lists the same keys in the same order.
  val **v = l.out->l.v;
  for (size_t i = 1; i < l.out->l.n; i++)
    for (size_t j = i; j > 0 && strcmp(v[j - 1]->t.s, v[j]->t.s) > 0; j--) { val *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  return l.out;
}

static val *hosted(run *r, val **a), *settings_get(run *r, val **a), *settings_set(run *r, val **a);   // 3. and 6.

// http is one web request, made by the device itself where it can (pl_http), or by its host.
static void floor_phrases(void) {
  group_add("compute", "none");
  phrase_add("compute", "none", "now() -> number: seconds since 1970", time_);
  phrase_add("compute", "none", "wait(seconds): at most 60", wait_);
  phrase_add("compute", "none", "log(x): a line shown to the person", note_);
  group_add("store", "none");
  phrase_add("store", "none", "get(key) -> text?: Onsay's own storage on this device", get);
  phrase_add("store", "none", "put(key, value): kept as text", put);
  phrase_add("store", "none", "keys(prefix=\"\") -> [text]: sorted", keys);
  phrase_add("store", "none", "delete(key)", delete_);
  group_add("http", "net");
  phrase_add("http", "net", "send(method, url, headers=nothing, body=nothing, timeout=30) -> {status,headers,text}: one request, "
             "headers a record, body text, redirects followed, at most 4 MB back", hosted);
  group_add("settings", "none");
  phrase_add("settings", "none", "get(lingo) -> [record]: every setting of a lingo, as its lingo.json and its code say, each with "
             "its value now", settings_get);
  phrase_add("settings", "none", "set(lingo, key, value): kept wherever that lingo keeps its settings", settings_set);
}

////////// 3. LINGOS //////////

// Core is the only lingo built in. A lingo written in Lingo is one file in the lingos folder, like memo.lingo, or a folder
// with its lingo.json beside it, like agent/agent.lingo, its listing made from its code. A native lingo (a library like
// windows.dll) or a host one (phrases the program around the core runs) is a folder with a lingo.json. One that can't
// load is set aside with its reason, and the rest still load. Onsay is told what each one shares, never its code (7.
// CONNECTION).
static arena *library;
static val *device;                  // the compiler's instance of every lingo that loaded
static val *catalog;                 // Onsay's table of contents: every lingo, whether it works, and what it shares
static val *folders;                 // each lingo as read: {file, code}, {folder, manifest} or both, with error if it failed
static val *offered;                 // the manifest of every native lingo offered now, core first
static unsigned long long looked;    // a fingerprint of the lingos folder when it was last read
#define FNV 14695981039346656037ULL

// Every native lingo by its number, with the folder it came from. A library is loaded once and kept, since its phrases
// are in the table from then on: a changed one is used after a restart.
static struct { const char *folder, *name, *version; } natives[64] = { { NULL, "core", "0.1" } };
#define CORE_ABOUT "Every computer's own floor, what Lingo can't write for itself: reaching inside values, converting them, the clock, waiting, a line for the person, Onsay's own storage, and web requests."

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

typedef struct { arena *a; val *names; unsigned long long *print; } named;

// Each folder, and each .lingo file, fingerprinted as it is listed.
static void collect(const char *name, int folder, double size, double changed, void *arg) {
  named *n = arg;
  size_t len = strlen(name);
  if (name[0] == '.' || (!folder && (len <= 6 || strcmp(name + len - 6, ".lingo")))) return;
  list_push(n->a, n->names, v_cstr(n->a, name));
  print_file(name, folder, size, changed, n->print);
}

// What is in the lingos folder now, and a fingerprint of every file of it: names, sizes and times, so any change shows.
static val *look(arena *a, unsigned long long *print) {
  *print = FNV;
  named n = { a, v_list(a), print };
  pl_list(set.lingos, collect, &n);
  for (size_t i = 0; i < n.names->l.n; i++) {
    buf path = { 0 };
    buf_fmt(&path, "%s/%s", set.lingos, n.names->l.v[i]->t.s);
    pl_list(path.s, print_file, print);   // nothing for a file
    buf_free(&path);
  }
  return n.names;
}

// Sets one aside, with why.
static void set_aside(arena *a, val *e, const char *why) {
  buf b = { 0 };
  buf_fmt(&b, "%s: %s", rec_get(e, "folder")->t.s, why);
  rec_set(a, e, "error", v_text(a, b.s, b.n));
  say("Lingo %s can't be used: %s", rec_get(e, "folder")->t.s, b.s);
  buf_free(&b);
}

// One name in the lingos folder: a .lingo file as {file, code}, a folder as {folder, manifest}, and the folder of a lingo
// written in Lingo, its lingo.json's runs neither native nor host and its code beside it as <name>.lingo, as both. NULL
// for a folder with no lingo.json, which isn't a lingo. One whose lingo.json lists its settings wrongly is set aside.
static val *read_lingo(arena *a, const char *name) {
  buf path = { 0 }, text = { 0 };
  size_t len = strlen(name);
  int file = len > 6 && !strcmp(name + len - 6, ".lingo");
  if (file) buf_fmt(&path, "%s/%s", set.lingos, name); else buf_fmt(&path, "%s/%s/lingo.json", set.lingos, name);
  int found = pl_read(path.s, &text);
  val *e = NULL;
  if (found) {
    e = v_rec(a);
    rec_set(a, e, file ? "file" : "folder", v_cstr(a, name));
    const char *problem = NULL;
    val *m = file ? NULL : json_parse(a, text.s, text.n, &problem);
    if (file) rec_set(a, e, "code", v_text(a, text.s, text.n));
    else if (!problem && m->k == V_REC) {
      rec_set(a, e, "manifest", m);
      const char *runs = text_or(m, "runs", "");
      path.n = text.n = 0;
      buf_fmt(&path, "%s/%s/%s.lingo", set.lingos, name, name);
      if (strcmp(runs, "native") && strcmp(runs, "host") && pl_read(path.s, &text)) {
        rec_set(a, e, "file", v_cstr(a, path.s + strlen(set.lingos) + len + 2));
        rec_set(a, e, "code", v_text(a, text.s, text.n));
      }
      val *settings = rec_get(m, "settings"), *keep = rec_get(m, "keep");
      int listed = !settings || settings->k == V_LIST;
      for (size_t i = 0; listed && settings && i < settings->l.n; i++) listed = settings->l.v[i]->k == V_TEXT || text_or(settings->l.v[i], "name", NULL);
      if (!listed) set_aside(a, e, "its settings are a list, each a name or a record with its name, like {\"name\": \"key\", \"secret\": true}");
      else if (keep && (!text_or(keep, "read", NULL) || !text_or(keep, "write", NULL)))
        set_aside(a, e, "keep names its own functions that read and write its settings, like {\"read\": \"read\", \"write\": \"write\"}");
    }
  }
  buf_free(&path);
  buf_free(&text);
  return e;
}

// The native lingo already called name, or -1.
static int native_called(const char *name) {
  for (size_t i = 0; i < native_count(); i++) if (!strcmp(natives[i].name, name)) return (int)i;
  return -1;
}

// A lingo's lingo.json, a native's from the folder it loaded from, or NULL when it has none.
static val *manifest_of(const char *lingo) {
  int k = native_called(lingo);
  const char *folder = k > 0 && natives[k].folder ? natives[k].folder : lingo;
  for (size_t i = 0; i < folders->l.n; i++)
    if (!strcmp(text_or(folders->l.v[i], "folder", ""), folder) && !rec_get(folders->l.v[i], "error")) return rec_get(folders->l.v[i], "manifest");
  return NULL;
}

static const char *keeping;   // the lingo whose own read or write runs now, so its setting(...) reads this device's file

/* One lingo's settings, as a form shows them: each its lingo.json lists, a name alone or a record of its name, title,
   about, default, required and secret, then each other one its code reads, by name. keep, when asked for, is where its
   lingo.json says they are kept, {read, write}, its own functions, or NULL for this device's file of them, as it is
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

// A host lingo's lingo.json lists its groups, shapes and functions (see README). This one function serves every host
// phrase, however many: it hands the running phrase's values to the host through pl_call, and gives back its answer.
// lingo.run and lingo.use, lingo(...) and use(...) as the program reaches them, go to the host's ask instead where it has one, like host.c's, their values by name, lingo.run's with its context last
// (pl_context), what was around it. http.send goes to pl_http, the same way.
static val *hosted(run *r, val **a) {
  const char *id = phrase_running();
  phrase *p = phrase_find(id);
  buf in = { 0 }, out = { 0 };
  int asking = asker && !strncmp(id, "lingo.", 6), web = !strcmp(id, "http.send");
  buf_chr(&in, asking ? '{' : '[');
  for (int i = 0; p && i < p->count; i++) {
    if (i) buf_chr(&in, ',');
    if (asking) { buf_json_str(&in, p->params[i], strlen(p->params[i])); buf_chr(&in, ':'); }
    buf_json(&in, a[i] ? a[i] : NOTHING);
  }
  if (!strcmp(id, "lingo.run")) {
    buf_str(&in, asking ? ",\"context\":" : ",");
    pl_context(&in);   // what was around it, never a value
    // The function asking.
    if (*calling || !asking) { buf_str(&in, asking ? ",\"asking\":" : ","); buf_json_str(&in, calling, strlen(calling)); }
  }
  buf_chr(&in, asking ? '}' : ']');
  int asked = asking ? asker(in.s, in.n, r->stop, &out) : web ? pl_http(in.s, in.n, &out) : pl_call(id, in.s, in.n, &out);
  buf_free(&in);
  if (*r->stop || pl_stopped()) { buf_free(&out); fail(r, "stopped", "Stopped by the person"); }
  const char *problem = NULL;
  val *got = asked ? json_parse(r->a, out.s ? out.s : "", out.n, &problem) : NULL;
  buf_free(&out);
  if (got && (problem || got->k != V_REC)) got = NULL;
  if (!got) fail(r, "phrase", asking ? "This lingo client isn't connected to Onsay" : "this device's host didn't answer");
  val *error = rec_get(got, "error"), *code = rec_get(got, "code"), *value = rec_get(got, "value");
  if (error && error->k == V_TEXT) fail(r, code && code->k == V_TEXT ? code->t.s : "phrase", "%s", error->t.s);
  return value ? value : NOTHING;
}

static void needs_text(buf *b, val *needs) { for (size_t i = 0; i < needs->l.n; i++) buf_fmt(b, "%s%s", i ? ", " : "", needs->l.v[i]->t.s); }

// Its shapes, groups and functions, read by lingo_read, into the core's own table as listing lines, kept for as long as
// the core runs.
static void host_phrases(val *l) {
  val *groups = rec_get(l, "groups"), *phrases = rec_get(l, "phrases"), *shapes = rec_get(l, "shapes");
  for (size_t i = 0; i < shapes->r.n; i++) shape_add(strdup(shapes->r.key[i]), strdup(shapes->r.v[i]->t.s));
  for (size_t i = 0; i < groups->l.n; i++) {
    buf needs = { 0 }, line = { 0 };
    needs_text(&needs, rec_get(groups->l.v[i], "needs"));
    group_add(strdup(rec_get(groups->l.v[i], "name")->t.s), strdup(needs.s));
    for (size_t k = 0; k < phrases->l.n; k++) {
      val *p = phrases->l.v[k];
      if (strcmp(rec_get(p, "group")->t.s, rec_get(groups->l.v[i], "name")->t.s)) continue;
      buf mine = { 0 };
      needs_text(&mine, rec_get(p, "needs"));
      line.n = 0;
      lingo_line(&line, p, needs.s);
      phrase_add(strdup(rec_get(p, "group")->t.s), strdup(mine.s), strdup(line.s), hosted);
      buf_free(&mine);
    }
    buf_free(&needs);
    buf_free(&line);
  }
}

// Every native or host folder gets "native", its number: one already loaded is found by its folder, and a new one loads,
// built on core. What can't load is set aside, a lingo.json of a lingo written in Lingo too.
static void load_natives(arena *a, val *found) {
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *folder = rec_get(e, "folder")->t.s, *why = NULL;
    size_t k = 1;
    while (k < native_count() && !(natives[k].folder && !strcmp(natives[k].folder, folder))) k++;
    if (k < native_count()) { rec_set(a, e, "native", v_num(a, (double)k)); continue; }
    val *l = lingo_read(a, rec_get(e, "manifest"), folder, &why);
    if (!l) { set_aside(a, e, why); continue; }
    rec_set(a, e, "lingo", l);
    int host = !strcmp(text_or(l, "runs", ""), "host");
    const char *name = rec_get(l, "name")->t.s, *version = rec_get(l, "version")->t.s, *main_ = text_or(l, "main", NULL);
    if (native_called(name) >= 0) { set_aside(a, e, "another lingo here is already called that"); continue; }
    if (native_count() == sizeof natives / sizeof *natives) { set_aside(a, e, "this core can't take more native lingos"); continue; }
    if (host && !pl_hosts()) { set_aside(a, e, "its phrases are run by the program around the core, and this device's has none"); continue; }
    lingo_entry entry = NULL;
    if (!host) {
      buf path = { 0 }, why = { 0 };
      buf_fmt(&path, "%s/%s/%s", set.lingos, folder, main_);
      entry = pl_library(path.s, &why);
      if (!entry) {
        path.n = 0;
        buf_fmt(&path, "%s: %s", main_, why.s);
        set_aside(a, e, path.s);
      }
      buf_free(&path);
      buf_free(&why);
      if (!entry) continue;
    }
    // Kept for as long as the core runs, like the library itself.
    size_t at = native_count();
    natives[at].folder = strdup(folder), natives[at].name = strdup(name), natives[at].version = strdup(version);
    native_begin(natives[at].name, natives[at].version, "core 0.1");
    if (host) host_phrases(l); else entry();
    rec_set(a, e, "native", v_num(a, (double)at));
    if (host) say("Lingo %s %s is loaded, run by this device's host", name, version);
    else say("Lingo %s %s is loaded from %s", name, version, main_);
  }
}

// The functions a lingo shares, as its catalog lists them: each one's id, head, what it gives when it says, what it
// does, its inputSchema, and lingos, how many lingo(...) calls it makes, none for a native.
static val *shared(arena *a, val *phrases) {
  val *out = v_list(a);
  static const char *KEPT[] = { "id", "head", "gives", "does", "inputSchema", NULL };
  for (size_t i = 0; phrases && i < phrases->l.n; i++) {
    val *p = phrases->l.v[i], *one = v_rec(a);
    for (int k = 0; KEPT[k]; k++) if (rec_get(p, KEPT[k])) rec_set(a, one, KEPT[k], rec_get(p, KEPT[k]));
    rec_set(a, one, "lingos", rec_get(p, "lingos") ? rec_get(p, "lingos") : v_num(a, 0));
    list_push(a, out, one);
  }
  return out;
}

// Reads everything again: every folder, the natives, the graph and the catalog. Built beside what there was, which stays
// in use when even core's own lingos can't load now. 1 when it loaded.
static int lingos(void) {
  arena *was = library;
  val *was_device = device, *was_catalog = catalog, *was_folders = folders, *was_offered = offered;
  arena *a = library = arena_new();
  offered = v_list(a);
  val *native_found = v_list(a), *stacked = v_list(a), *names = look(a, &looked);
  folders = v_list(a);
  for (size_t i = 0; i < names->l.n; i++) {
    val *e = read_lingo(a, names->l.v[i]->t.s);
    if (e) list_push(a, folders, e);
    if (e && !rec_get(e, "error")) list_push(a, rec_get(e, "file") ? stacked : native_found, e);
  }
  // core, then every native whose folder is here now. One whose folder is gone stays loaded, but isn't offered.
  load_natives(a, native_found);
  for (size_t i = 0; i < native_count(); i++) {
    int here = i == 0;
    for (size_t k = 0; k < native_found->l.n && !here; k++) { val *n = rec_get(native_found->l.v[k], "native"); here = n && n->n == (double)i; }
    if (!here) continue;
    buf b = { 0 };
    phrase_manifest(&b, i);
    list_push(a, offered, v_text(a, b.s, b.n));
    buf_free(&b);
  }
  // Loaded together. While one is at fault, it is set aside with its reason, and the rest load again without it.
  for (;;) {
    const char *why = NULL;
    device = lingo_load(a, offered, stacked, &why);
    if (device) break;
    size_t k = 0;
    for (; k < stacked->l.n; k++) {
      const char *f = rec_get(stacked->l.v[k], "file")->t.s;
      size_t n = strlen(f);
      if (!strncmp(why, f, n) && (why[n] == ':' || why[n] == ' ')) break;
    }
    if (k == stacked->l.n) {
      say("The core's own lingos can't load: %s", why);
      arena_free(a);
      library = was, device = was_device, catalog = was_catalog, folders = was_folders, offered = was_offered;
      return 0;
    }
    rec_set(a, stacked->l.v[k], "error", v_cstr(a, why));
    say("Lingo %s can't be used: %s", rec_get(stacked->l.v[k], "file")->t.s, why);
    memmove(&stacked->l.v[k], &stacked->l.v[k + 1], (stacked->l.n - k - 1) * sizeof *stacked->l.v);
    stacked->l.n--;
  }

  // The table of contents: each lingo, what it is, the names of its settings and every function it shares, for
  // Onsay. A native one has its version and folder, and a Lingo one its file.
  catalog = v_list(a);
  val *all = rec_get(device, "lingos");
  for (size_t i = 0; i < all->r.n; i++) {
    val *l = all->r.v[i], *one = v_rec(a);
    int core = !strcmp(all->r.key[i], "core");
    rec_set(a, one, "name", rec_get(l, "name"));
    if (rec_get(l, "version")) rec_set(a, one, "version", rec_get(l, "version"));
    rec_set(a, one, "status", v_cstr(a, "ok"));
    if (rec_get(l, "file")) rec_set(a, one, "file", rec_get(l, "file"));
    for (size_t k = 0; k < folders->l.n; k++) {
      val *e = folders->l.v[k], *n = rec_get(e, "native");
      if (n && natives[(size_t)n->n].folder && !strcmp(natives[(size_t)n->n].name, all->r.key[i])) {
        rec_set(a, one, "folder", rec_get(e, "folder"));
        if (rec_get(rec_get(e, "manifest"), "about")) rec_set(a, one, "about", rec_get(rec_get(e, "manifest"), "about"));   // a native loaded before isn't read again
      }
    }
    if (rec_get(l, "about")) rec_set(a, one, "about", rec_get(l, "about"));
    if (core) rec_set(a, one, "about", v_cstr(a, CORE_ABOUT));
    val *mine = declared(a, all->r.key[i], NULL), *names = v_list(a);
    for (size_t k = 0; k < mine->l.n; k++) list_push(a, names, rec_get(mine->l.v[k], "name"));
    if (rec_get(l, "settings") || names->l.n) rec_set(a, one, "settings", names);
    if (!core) rec_set(a, one, "phrases", shared(a, rec_get(l, "phrases")));
    if (rec_get(l, "native")) rec_set(a, one, "native", VBOOL(1));
    else say("Lingo %s is ready", rec_get(l, "name")->t.s);
    list_push(a, catalog, one);
  }
  for (size_t i = 0; i < folders->l.n; i++) {
    val *e = folders->l.v[i], *m = rec_get(e, "manifest"), *f = rec_get(e, "file");
    if (!rec_get(e, "error")) continue;
    val *one = v_rec(a), *n = m && m->k == V_REC ? rec_get(m, "name") : NULL;
    rec_set(a, one, "name", f ? v_text(a, f->t.s, f->t.len - 6) : n && n->k == V_TEXT ? n : rec_get(e, "folder"));
    rec_set(a, one, f ? "file" : "folder", f ? f : rec_get(e, "folder"));
    rec_set(a, one, "status", v_cstr(a, "broken"));
    rec_set(a, one, "error", rec_get(e, "error"));
    list_push(a, catalog, one);
  }
  arena_free(was);
  return 1;
}

////////// 4. MESSAGES //////////

static pl_gate *gate;             // keeps the two below, which core_heard may set from another thread while a job runs
static volatile int stopping;     // the running job's stop flag
static char running[128];         // the running job's id, as JSON, so a stop can name it

void core_say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "onsay: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// A stop is handled here, at once, even while a job runs on another thread. What isn't JSON is a job, so core_do says so.
int core_heard(const char *text, size_t n) {
  arena *a = arena_new();
  const char *problem = NULL;
  val *m = json_parse(a, text ? text : "", text ? n : 0, &problem);
  const char *type = problem ? "" : text_or(m, "type", "");
  int kind = CORE_JOB;
  if (!strcmp(type, "ping")) kind = CORE_PING;
  else if (!strcmp(type, "pong")) kind = CORE_PONG;
  else if (!strcmp(type, "stop")) {
    buf id = { 0 };
    buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    pl_enter(gate);
    if (!strcmp(id.s, running)) stopping = 1;
    pl_leave(gate);
    buf_free(&id);
    kind = CORE_DONE;
  }
  arena_free(a);
  return kind;
}

////////// 5. JOBS //////////

static int reply(const char *text, size_t n) { return sender && sender(text, n); }

// A result for what it can't run, so Onsay never waits on a job that will never finish. line 0 is no line.
static void refuse(buf *out, const char *id, const char *code, const char *why, int line) {
  buf_fmt(out, "{\"type\":\"result\",\"id\":%s,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":", id, code);
  buf_json_str(out, why, strlen(why));
  if (line) buf_fmt(out, ",\"at\":\"\",\"line\":%d},\"log\":[]}", line);
  else buf_str(out, ",\"at\":\"\",\"line\":null},\"log\":[]}");
}

// The lingos a job names, or every one this device has.
static val *names_of(arena *a, val *m) {
  val *names = rec_get(m, "names");
  if (names && names->k == V_LIST) return names;
  val *all = v_list(a), *lingos = rec_get(device, "lingos");
  for (size_t i = 0; i < lingos->r.n; i++) list_push(a, all, v_cstr(a, lingos->r.key[i]));
  return all;
}

// A phrase written in Lingo, like memo.recall, as a one-line program on its lingo: tool phrase(v) {return memo.recall(v[0])}.
// NULL when it isn't one of a folder lingo's here, or isn't a plain id, so nothing but a phrase is ever compiled from it.
static val *written(arena *a, const char *phrase, val *values) {
  const char *dot = strrchr(phrase, '.');
  if (!dot || phrase_find(phrase) || strspn(phrase, "abcdefghijklmnopqrstuvwxyz0123456789_.") != strlen(phrase)) return NULL;
  val *l = rec_get(rec_get(device, "lingos"), v_text(a, phrase, (size_t)(dot - phrase))->t.s);
  if (!l || rec_get(l, "native")) return NULL;
  buf b = { 0 };
  buf_fmt(&b, "tool phrase(v) {return %s(", phrase);
  for (size_t i = 0; values && values->k == V_LIST && i < values->l.n; i++) buf_fmt(&b, "%sv[%zu]", i ? ", " : "", i);
  buf_str(&b, ")}");
  val *code = v_text(a, b.s, b.n);
  buf_free(&b);
  return code;
}

/* A lingo's function by where it is in the lingos folder, as an address's path names it: test.lingo/find is test.find,
   test.lingo alone test.main, and a native's by its group, windows.lingo/window/list window.list. Whatever comes before
   /lingos/ is the computer, so a whole address, like desktop.brandon/lingos/show.lingo/print, reads the same. NULL when
   it names no .lingo file. */
static const char *phrase_at(arena *a, const char *path) {
  const char *p = strstr(path, "/lingos/");
  p = p ? p + 8 : !strncmp(path, "lingos/", 7) ? path + 7 : path;
  size_t n = strcspn(p, "/?#");
  if (n <= 6 || strncmp(p + n - 6, ".lingo", 6)) return NULL;
  const char *fn = p[n] == '/' && p[n + 1] && p[n + 1] != '?' && p[n + 1] != '#' ? p + n + 1 : "main";
  size_t fl = strcspn(fn, "?#");
  buf b = { 0 };
  if (memchr(fn, '/', fl)) buf_add(&b, fn, fl); else buf_fmt(&b, "%.*s.%.*s", (int)(n - 6), p, (int)fl, fn);
  for (size_t i = 0; i < b.n; i++) if (b.s[i] == '/') b.s[i] = '.';
  char *out = arena_get(a, b.n + 1);
  memcpy(out, b.s, b.n);
  buf_free(&b);
  return out;
}

// A function's listing, if the lingo has one by that id, like memo.recall of memo or window.list of windows.
static val *listed(const char *lingo, const char *id) {
  val *l = rec_get(rec_get(device, "lingos"), lingo), *own = l ? rec_get(l, "phrases") : NULL;
  for (size_t i = 0; own && i < own->l.n; i++) if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  return NULL;
}

/* A phrase's values in its order: a list as it is, a record by each value's name, nothing for one left out, so its
   default applies. NULL with why when a record names a value it doesn't take. One the device hasn't got keeps its
   values, for run_phrase to say so. */
static val *in_order(arena *a, const char *id, val *values, const char **why) {
  if (!values || values->k != V_REC) return values && values->k == V_LIST ? values : v_list(a);
  phrase *p = phrase_find(id);
  const char *dot = strrchr(id, '.');
  val *own = !p && dot ? listed(v_text(a, id, (size_t)(dot - id))->t.s, id) : NULL, *params = own ? rec_get(own, "params") : NULL;
  if (!p && !params) return v_list(a);
  size_t count = p ? (size_t)p->count : params->l.n;
  val *out = v_list(a);
  for (size_t i = 0; i < count; i++) {
    const char *name = p ? p->params[i] : text_or(params->l.v[i], "name", "");
    val *v = rec_get(values, name);
    list_push(a, out, v ? v : NOTHING);
  }
  for (size_t k = 0; k < values->r.n; k++) {
    int known = 0;
    for (size_t i = 0; i < count && !known; i++) known = !strcmp(values->r.key[k], p ? p->params[i] : text_or(params->l.v[i], "name", ""));
    if (!known) {
      buf b = { 0 };
      buf_fmt(&b, "%s doesn't take a value called %s", id, values->r.key[k]);
      *why = arena_get(a, b.n + 1);
      memcpy((char *)*why, b.s, b.n);
      buf_free(&b);
      return NULL;
    }
  }
  return out;
}

// run: a tree from Onsay, or Lingo code compiled here. call: one phrase the device has, by its id or by its file and
// function in the lingos folder, a phrase written in Lingo run as a program of its own, so its limits, stop and errors
// are a program's. Its result goes in out. A call may come while a job waits on lingo(...), and runs inside it: that
// job's id, stop and function come back after.
static void work(arena *a, val *m, const char *type, const char *id, buf *out) {
  if (strlen(id) >= sizeof running) { refuse(out, "null", "type", "That job's id is too long", 0); return; }
  char was_running[sizeof running], was[sizeof calling];
  pl_enter(gate);
  int was_stopping = stopping;
  memcpy(was_running, running, sizeof running);
  snprintf(running, sizeof running, "%s", id);
  stopping = 0;
  pl_leave(gate);
  memcpy(was, calling, sizeof calling);
  double started = pl_now();
  const char *why = NULL;
  int call = !strcmp(type, "call"), line = 0;
  const char *lingo = text_or(m, "lingo", NULL), *phrase = lingo ? phrase_at(a, lingo) : text_or(m, "phrase", "");
  val *values = call && phrase ? in_order(a, phrase, rec_get(m, "values"), &why) : NULL, *program = call ? NULL : rec_get(m, "program"), *input = rec_get(m, "input");
  val *code = !call ? rec_get(m, "code") : values ? written(a, phrase, values) : NULL, *names = call ? v_list(a) : NULL;
  if (call && phrase) snprintf(calling, sizeof calling, "%s", phrase);
  if (call && code) {
    list_push(a, names, v_text(a, phrase, (size_t)(strrchr(phrase, '.') - phrase)));
    rec_set(a, input = v_rec(a), "v", values->k == V_LIST ? values : v_list(a));
  }
  if (code && code->k == V_TEXT) program = lingo_prepare(a, device, code->t.s, code->t.len, names ? names : names_of(a, m), &line, &why);
  if (!phrase) refuse(out, id, "unknown", "A lingo is called by its file and function, like test.lingo/find", 0);
  else if (call && !values) refuse(out, id, "type", why, 0);
  else if (code && code->k == V_TEXT && !program) refuse(out, id, "compile", why, line);
  else if (call && !code) run_phrase(a, id, phrase, values, &stopping, set.allow, set.allowed, out);
  else {
    // Onsay may ask for less than the settings allow, never more.
    val *limits = rec_get(m, "limits");
    long long steps = set.steps;
    double ms = set.ms;
    if (limits && limits->k == V_REC) {
      double s = num_or(limits, "steps", (double)steps), t = num_or(limits, "ms", ms);
      if (s > 0 && s < steps) steps = (long long)s;
      if (t > 0 && t < ms) ms = t;
    }
    run_program(a, id, program ? program : NOTHING, input && input->k == V_REC ? input : v_rec(a), steps, ms, &stopping,
                set.allow, set.allowed, out);
  }
  memcpy(calling, was, sizeof calling);
  pl_enter(gate);
  memcpy(running, was_running, sizeof running);
  stopping = was_stopping;
  pl_leave(gate);
  say("%s %s done in %.0f ms", type, id, (pl_now() - started) * 1000);
}

// One message core_heard called a job. Gives 1 when it was ready, the sign that hello was accepted.
int core_do(const char *text, size_t n) {
  arena *a = arena_new();
  const char *problem = NULL;
  val *m = json_parse(a, text ? text : "", text ? n : 0, &problem);
  if (problem || m->k != V_REC) { say("The server sent something that isn't JSON: %s", problem ? problem : "not a record"); arena_free(a); return 0; }
  const char *type = text_or(m, "type", "");
  int ready = !strcmp(type, "ready");
  buf id = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);

  if (ready) say("Signed in. This device is %s", text_or(m, "device", "?"));
  else if (!strcmp(type, "error")) say("The server says: %s", text_or(m, "error", "an error"));
  else if (!strcmp(type, "run") || !strcmp(type, "call") || !strcmp(type, "request")) {
    buf out = { 0 };
    if (!strcmp(type, "request")) core_serve(text, n, CORE_ONSAY, &out); else work(a, m, type, id.s, &out);
    reply(out.s, out.n);
    buf_free(&out);
  } else if (strcmp(type, "lingo")) say("The server sent a message this core doesn't know: %s", type);   // lingo: an answer after a stop
  buf_free(&id);
  arena_free(a);
  return ready;
}

////////// 6. LINGO SETTINGS //////////

// A lingo's settings are kept on this device, never on Onsay: in home/settings/<lingo>.json, a record by name, or by the
// lingo's own functions when its lingo.json says keep: {read, write}, so its maker decides where they live. setting(...)
// is lingo.setting(key, reference), its value the one kept, else its lingo.json's default, else nothing.

// The lingo a setting is of, checked, since it names a file, and the setting's name when there is one.
static const char *whose(run *r, val *lingo, val *key) {
  if (lingo->k != V_TEXT || !lingo->t.len || lingo->t.len > 64 || strspn(lingo->t.s, "abcdefghijklmnopqrstuvwxyz0123456789_-") != lingo->t.len)
    fail(r, "phrase", "a lingo's settings are named by the lingo, like openrouter");
  if (key && (key->k != V_TEXT || !key->t.len)) fail(r, "phrase", "a setting is named by its name, like key");
  return lingo->t.s;
}

// The settings this device keeps for a lingo, a record by name, and the file they are in.
static val *stored(run *r, const char *lingo, buf *path) {
  buf text = { 0 };
  buf_fmt(path, "%s/settings/%s.json", set.home, lingo);
  val *s = pl_read(path->s, &text) ? json_parse(r->a, text.s, text.n, NULL) : NULL;
  buf_free(&text);
  return s && s->k == V_REC ? s : v_rec(r->a);
}

// A lingo's own function that keeps its settings, read(key) or write(key, value), run as a program of its own. Its value.
static val *kept(run *r, const char *lingo, const char *fn, val *values) {
  buf id = { 0 }, out = { 0 };
  buf_fmt(&id, "%s.%s", lingo, fn);
  val *code = written(r->a, id.s, values), *names = v_list(r->a), *input = v_rec(r->a), *program = NULL;
  const char *why = "it has no function by that name", *was = keeping;
  int line = 0;
  list_push(r->a, names, v_cstr(r->a, lingo));
  rec_set(r->a, input, "v", values);
  if (code) program = lingo_prepare(r->a, device, code->t.s, code->t.len, names, &line, &why);
  if (program) {
    keeping = lingo;
    run_program(r->a, "null", program, input, set.steps, set.ms, r->stop, set.allow, set.allowed, &out);
    keeping = was;
  }
  val *got = program ? json_parse(r->a, out.s, out.n, NULL) : NULL, *error = got ? rec_get(got, "error") : NULL;
  val *fn_ = v_text(r->a, id.s, id.n);
  buf_free(&id);
  buf_free(&out);
  if (!program || error) fail(r, error ? text_or(error, "code", "phrase") : "phrase", "%s, which keeps %s's settings: %s", fn_->t.s, lingo, error ? text_or(error, "message", "") : why);
  return rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
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
  if (!ok) fail(r, "phrase", "this device couldn't save the setting");
  return NOTHING;
}

////////// 7. CONNECTION //////////

// Ends a message with the native lingos' manifests, core first, and the catalog of every lingo, then sends it.
static void send_catalog(buf *out) {
  buf_str(out, "\"manifests\":");
  buf_json(out, offered);
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
  buf_str(&out, "{\"type\":\"catalog\",");
  send_catalog(&out);
}

// Hello, with the catalog, since a new connection may be a server that never had it.
void core_open(void) {
  buf out = { 0 };
  buf_str(&out, "{\"type\":\"hello\",\"token\":");
  buf_json_str(&out, set.token, strlen(set.token));
  buf_str(&out, ",\"client\":\"" CORE_CLIENT "\",\"version\":\"" CORE_VERSION "\",\"level\":\"run\",\"links\":[\"onsay\"],\"name\":");
  buf_json_str(&out, set.name, strlen(set.name));
  buf_str(&out, ",\"allow\":[\"none\"");
  for (int i = 0; i < set.allowed; i++) if (strcmp(set.allow[i], "none")) { buf_chr(&out, ','); buf_json_str(&out, set.allow[i], strlen(set.allow[i])); }
  buf_str(&out, "],");
  send_catalog(&out);
}

////////// 8. REQUESTS //////////

// A web request reaches the lingo at the longest route its path starts with: the route its lingo.json names, like
// "/onsay", else /<name>. From anywhere but this computer, only a lingo whose lingo.json says "open": true is there. A
// lingo with serve(request) gets every request to its route, and any other is called as /<name>/<function>, its values a
// JSON record by name, or the query for a GET. GET / lists the routes the caller may reach.

// Headers as a record by lower case name, each value text.
static val *header_names(arena *a, val *h) {
  val *out = v_rec(a);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    buf name = { 0 }, value = { 0 };
    for (const char *c = h->r.key[i]; *c; c++) buf_chr(&name, (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c));
    buf_text(&value, h->r.v[i]);
    rec_set(a, out, name.s ? name.s : "", v_text(a, value.s ? value.s : "", value.n));
    buf_free(&name), buf_free(&value);
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

void core_serve(const char *text, size_t n, int from, buf *out) {
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  arena *a = arena_new();
  const char *problem = NULL, *lingo = NULL;
  val *m = json_parse(a, text ? text : "", text ? n : 0, NULL), *query = v_rec(a), *routes = v_list(a), *head = v_rec(a), *got = NOTHING;
  if (!m || m->k != V_REC) m = v_rec(a);
  val *headers = header_names(a, rec_get(m, "headers")), *raw = rec_get(m, "body"), *base64 = rec_get(m, "base64");
  const char *p = text_or(m, "path", ""), *method = text_or(m, "method", "GET");
  size_t at = strcspn(p, "?"), best = 0;
  int status = 200, json = 1;   // json: the body is written as JSON, else as text
  buf id = { 0 }, body = { 0 }, error = { 0 }, call = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  int coded = base64 && truthy(base64);
  if (raw && raw->k == V_TEXT && !coded) buf_add(&body, raw->t.s, raw->t.len);
  for (size_t i = 0, bits = 0, v = 0; coded && raw && raw->k == V_TEXT && i < raw->t.len && raw->t.s[i] != '='; i++) {
    const char *digit = raw->t.s[i] ? strchr(B64, raw->t.s[i]) : NULL;
    if (!digit) { buf_str(&error, "Its body isn't base64"); break; }
    v = v << 6 | (size_t)(digit - B64);
    bits += 6;
    if (bits >= 8) { bits -= 8; buf_chr(&body, (char)(v >> bits & 255)); }
  }

  // A page in a browser on this computer can send requests here too: one counts as from this computer only when it
  // names this computer as its host and no page elsewhere sent it.
  if (from == CORE_HERE) {
    const char *host = text_or(headers, "host", ""), *origin = text_or(headers, "origin", NULL), *site = text_or(headers, "sec-fetch-site", "none");
    size_t end = host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":");
    int named = !end || (((end == 9 && (!strncmp(host, "127.0.0.1", 9) || !strncmp(host, "localhost", 9))) || (end == 5 && !strncmp(host, "[::1]", 5))) &&
                         (!host[end] || host[end] == ':'));
    if (!named || (origin && (strncmp(origin, "http://", 7) || strcmp(origin + 7, host))) || (strcmp(site, "none") && strcmp(site, "same-origin"))) from = CORE_NETWORK;
  }
  for (const char *q = p + at + (p[at] == '?'); *q;) {
    size_t len = strcspn(q, "&"), eq = strcspn(q, "=&");
    if (len) rec_set(a, query, unescape(a, q, eq)->t.s, unescape(a, q + eq + (eq < len), eq < len ? len - eq - 1 : 0));
    q += len + (q[len] == '&');
  }
  for (size_t i = 0; i < catalog->l.n; i++) {
    val *e = catalog->l.v[i], *mf, *open;
    const char *name = text_or(e, "name", ""), *own;
    if (strcmp(text_or(e, "status", ""), "ok") || !strcmp(name, "core")) continue;
    mf = manifest_of(name), open = mf ? rec_get(mf, "open") : NULL, own = text_or(mf, "route", "");
    if (from != CORE_HERE && !(open && open->k == V_BOOL && open->b)) continue;
    buf r = { 0 };
    if (own[0] == '/' && own[1] && !strchr(own, '?')) buf_str(&r, own); else buf_fmt(&r, "/%s", name);
    while (r.n > 1 && r.s[r.n - 1] == '/') r.s[--r.n] = 0;
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, name));
    rec_set(a, one, "route", v_text(a, r.s, r.n));
    list_push(a, routes, one);
    if (r.n > best && at >= r.n && !memcmp(p, r.s, r.n) && (at == r.n || p[r.n] == '/')) lingo = name, best = r.n;
    buf_free(&r);
  }

  if (error.n) status = 400;
  else if (from == CORE_ONSAY && !set.inbound) status = 403, buf_str(&error, "This lingo client doesn't take requests through Onsay");
  else if (p[0] != '/') status = 400, buf_str(&error, "A path starts with /, like /memo/recall");
  else if (!utf8_ok(body.s ? body.s : "", body.n)) status = 400, buf_str(&error, "Its body isn't text, and lingos take text, like JSON");
  else if (at == 1) rec_set(a, got = v_rec(a), "lingos", routes);
  else if (!lingo) status = 404, buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
  else {
    const char *rest = p + best, *why = NULL, *fn = NULL;
    size_t left = at - best;
    val *job = v_rec(a), *values = v_list(a);
    buf_fmt(&call, "%s.serve", lingo);
    int serving = listed(lingo, call.s) != NULL;
    if (serving) {
      val *request = v_rec(a), *parsed = body.n ? json_parse(a, body.s, body.n, &problem) : NOTHING;
      rec_set(a, request, "method", v_cstr(a, method));
      rec_set(a, request, "path", left ? v_text(a, rest, left) : v_cstr(a, "/"));
      rec_set(a, request, "query", query);
      rec_set(a, request, "headers", headers);
      rec_set(a, request, "body", v_text(a, body.s ? body.s : "", body.n));
      rec_set(a, request, "json", problem ? NOTHING : parsed);
      list_push(a, values, request);
    } else {
      call.n = 0;
      buf_fmt(&call, "%s.lingo%.*s", lingo, (int)left, rest);
      fn = phrase_at(a, call.s);
      values = strcmp(method, "GET") && body.n ? json_parse(a, body.s, body.n, &problem) : strcmp(method, "GET") ? v_rec(a) : query;
      if (!listed(lingo, fn)) status = 404, buf_fmt(&error, "%s has no function %s", lingo, fn);
      else if (problem || (values->k != V_REC && values->k != V_LIST) || !in_order(a, fn, values, &why))
        status = 400, buf_str(&error, why ? why : "Its values are a JSON record by name, like {\"text\": \"hello\"}");
    }
    if (!error.n) {
      buf result = { 0 };
      rec_set(a, job, "phrase", v_cstr(a, serving ? call.s : fn));
      rec_set(a, job, "values", values);
      work(a, job, "call", id.s, &result);
      val *r = json_parse(a, result.s, result.n, NULL), *e = rec_get(r, "error");
      buf_free(&result);
      const char *code = text_or(e, "code", "");
      if (e) status = !strcmp(code, "unknown") ? 404 : !strcmp(code, "denied") ? 403 : 500, buf_str(&error, text_or(e, "message", "It failed"));
      else got = rec_get(r, "value") ? rec_get(r, "value") : NOTHING;
      // What serve gives: a record with a number status is the response, and anything else the body of a 200.
      if (serving && !error.n) {
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
  buf_free(&b), buf_free(&id), buf_free(&body), buf_free(&error), buf_free(&call);
  arena_free(a);
}

////////// 9. BEGINNING //////////

// lingo.run, lingo.use and lingo.setting are in no listing: they are lingo(...), use(...) and setting(...), part of the
// language, so every core has them, unless a host lingo of its own gave them. lingo.setting is answered here (6.).
int core_begin(const char *text, size_t n, core_send send, core_ask ask) {
  if (!settings(text, n)) return 0;
  sender = send;
  asker = ask;
  gate = pl_gate_new();
  native_begin("core", "0.1", NULL);
  std_phrases();
  floor_phrases();
  if (!lingos()) return 0;
  if (!phrase_find("lingo.run")) phrase_add("lingo", "none", "run(ask, values=nothing, gives=\"\")", hosted);
  if (!phrase_find("lingo.use")) phrase_add("lingo", "none", "use(address, values=nothing)", hosted);
  if (!phrase_find("lingo.setting")) phrase_add("lingo", "none", "setting(key, reference)", setting_);
  return 1;
}

void core_manifests(buf *out) {
  for (size_t i = 0; i < offered->l.n; i++) {
    if (i) buf_str(out, "---\n");
    buf_add(out, offered->l.v[i]->t.s, offered->l.v[i]->t.len);
  }
}
