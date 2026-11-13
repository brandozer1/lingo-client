// floor.c - the floor, the lingo client: one function, lingo script in and its value out, as a computer's hardware takes
// instructions. It takes a call, checks who signed it and whether the policy lets them, runs the script the call carries,
// logs it, and tries to send its log to Onsay. It is the compiler and the lingo search in one: lingo(...), and a script
// that doesn't compile, are asked of Onsay's index, and the ways it finds are called from here. Nothing else: everything
// else is a lingo, called like any other. Built by
// build.ps1 beside lingo.c, which it replaces. See ../../onsay/docs/lingo-function.md, The floor.
//
// 1. Settings   its settings file: home, port, name, owner, keys, steps and ms, and the lingos built in
// 2. Callers    a signer's key, the owner, and the policy
// 3. Offers     the table of lingos offered here: what each does and where it answers, made from its own code
// 4. Calls      each call a script makes: checked, then answered here or sent where it goes, signed, and the lingo
//               search, lingo(...), which calls from here the ways Onsay's index finds
// 5. Log        every request, a line of home/requests.log, sent to Onsay sealed whenever a connection works, its
//               answer bringing the calls that came for this computer through Onsay
// 6. Running    a call's script compiled and run, one at a time, within its limits
// 7. Port       calls coming in, as web requests or through Onsay, each taken the same way
// 8. Main       the command line

#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lingo.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#define FLOOR_VERSION "0.1.0"

/* Onsay, built in: where its resolve endpoint and its log box are, and its public key, which what it signs is checked
   against and the log is sealed to. The test build asks test/floor.mjs, which signs with the test key. */
#ifdef TEST
#define ONSAY "http://127.0.0.1:8788"
#define ONSAY_KEY "ed25519:6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw="
#else
#define ONSAY "https://api.onsay.dev"
#define ONSAY_KEY "ed25519:4DnM32U1FzXfVh3ahkdILlt7yNwfM84Ofj7Aohoe1QE="
#endif
#define FRESH 300   // seconds a signer's key looked up holds
#ifdef TEST
#define SEND_EVERY 2   // seconds between tries at sending the log, unless Onsay's answer says otherwise
#else
#define SEND_EVERY 20
#endif
#define SEND_MOST 3600   // the longest it waits after a try that didn't go, doubling the wait each time
#define HOLD_MOST 60   // seconds Onsay may keep a post, waiting for a call for this computer, before it answers
#define LOG_MOST (4 << 20)   // bytes the log, or what waits to be sent, may grow to before it is cut to its newest half
#define HEAD (64 << 10)   // the most a call's first line and headers may be
#define BODY_ROOM (4 << 20)   // and its body
#define QUIET 10   // seconds a connection may say nothing before it is closed

static val *searched(run *r, const char *id, val *values, const char *code, const char *message);
// A call that failed here is its own failure, never sent on to be searched for.
static int searchable(run *r, const char *id) {
  (void)r;
  (void)id;
  return 0;
}

#include "language.c"
#include "checks.c"

// The standard functions, from ../lingos/standard (standard.h, made by build.ps1), compiled with every call.
typedef struct {
  const char *name;
  const char *code;
} standard;
#include "standard.h"

////////// 1. SETTINGS //////////

/* floor.json, or the file named on the command line:
     home    the folder it keeps its key, policy.json and requests.log in, floor-data when left out
     port    where calls come in, 6969 when left out, on every address this computer has
     name    this computer's name, like laptop.brandon, what it signs as; left out, it signs as its key itself
     owner   whose calls reach everything: a key, ed25519:<base64>, or a handle, every one of its computers
     keys    signers known by key, {"laptop.james": "ed25519:..."}, before Onsay is asked
     steps, ms   the most one call may take */
static struct {
  val *all;
  const char *home, *root, *name, *owner;
  int port;
  long long steps;
  double ms;
} set;

static int settings_read(const char *path) {
  static arena *kept;
  buf text = { 0 };
  buf root = { 0 };
  if (!pl_read(path, &text)) {
    fprintf(stderr, "Can't read the settings file %s\n", path);
    return 0;
  }
  kept = arena_new();
  set.all = json_parse(kept, text.s, text.n, NULL);
  buf_free(&text);
  if (!set.all || set.all->k != V_REC) {
    fprintf(stderr, "%s isn't a JSON record, like {\"port\": 6969, \"owner\": \"brandon\"}\n", path);
    return 0;
  }
  val *port = rec_get(set.all, "port");
  val *steps = rec_get(set.all, "steps");
  val *ms = rec_get(set.all, "ms");
  set.home = text_or(set.all, "home", "floor-data");
  set.name = text_or(set.all, "name", "");
  set.owner = text_or(set.all, "owner", "");
  set.port = port && port->k == V_NUM ? (int)port->n : 6969;
  set.steps = steps && steps->k == V_NUM ? (long long)steps->n : 10000000;
  set.ms = ms && ms->k == V_NUM ? ms->n : 30000;
  pl_folder(set.home);
  pl_whole(set.home, &root);
  set.root = v_text(kept, root.s ? root.s : set.home, root.n)->t.s;
  buf_free(&root);
  return 1;
}

/* The lingos built into this floor, each a function that adds its calls, named as it is built, like
   -DLINGOS="X(base_calls) X(web_calls)" for a distribution: the floor itself carries none. */
#ifdef LINGOS
#define X(fn) void fn(void);
LINGOS
#undef X
static struct {
  const char *name;
  void (*add)(void);
} built_in[] = {
#define X(fn) { #fn, fn },
  LINGOS
#undef X
};
#define BUILT_IN (sizeof built_in / sizeof *built_in)
#else
static struct {
  const char *name;
  void (*add)(void);
} built_in[1];
#define BUILT_IN 0
#endif

static void logged(arena *a, const char *way, const char *who, const char *route, int allowed, const char *why,
    const char *error, double ms);

const char *lingo_home(void) { return set.root; }

// A line on stderr, which whatever started the floor keeps, like compute.log's.
void core_say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

const char *lingo_onsay(void) { return ONSAY; }
int lingo_vouched(val *value, val *signed_) { return signed_by(value, signed_, NULL); }
void onsay_name(buf *out) { buf_str(out, set.name); }
const char *native_name(size_t lingo) { return lingo && lingo <= BUILT_IN ? built_in[lingo - 1].name : "core"; }

/* What a lingo built in may ask of the floor it runs on, as lingo.h says: its settings, kept in home/settings/<lingo>.json
   for now, a wait, and the person, whom a bare floor has no app to ask. */
val *lingo_setting(arena *a, const char *lingo, const char *key) {
  buf text = { 0 };
  val *s = named(lingo) && pl_read(arena_fmt(a, "%s/settings/%s.json", set.root, lingo), &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  buf_free(&text);
  return s && s->k == V_REC ? rec_get(s, key) : NULL;
}

int lingo_set(const char *lingo, const char *key, val *value) {
  arena *a = arena_new();
  buf text = { 0 };
  val *s = NULL;
  const char *path = arena_fmt(a, "%s/settings/%s.json", set.root, lingo);
  if (named(lingo) && pl_read(path, &text)) s = json_parse(a, text.s, text.n, NULL);
  if (!s || s->k != V_REC) s = v_rec(a);
  rec_set(a, s, key, value);
  text.n = 0;
  buf_json(&text, s);
  int ok = named(lingo) && pl_folder(arena_fmt(a, "%s/settings", set.root)) && pl_write(path, text.s, text.n);
  buf_free(&text);
  arena_free(a);
  return ok;
}

void lingo_wait(volatile int *done, volatile int *stop, double seconds) {
  double until = pl_now() + seconds;
  while (!*done && !(stop && *stop) && pl_now() < until) pl_sleep(0.01);
}

val *lingo_ask(run *r, val *question, double seconds) {
  (void)r;
  (void)question;
  (void)seconds;
  return NULL;
}

int lingo_said(const char *text) {
  (void)text;
  return 0;
}

// A request a lingo built in made, a line of the log, way out.
void lingo_called(const char *kind, const char *address, const char *ask, const char *method, const char *url, val *headers,
    const char *body, size_t n, val *got, const char *error, int stopped, double ms) {
  arena *a = arena_new();
  (void)kind, (void)address, (void)ask, (void)method, (void)headers, (void)body, (void)n, (void)got, (void)stopped;
  logged(a, "out", set.name, url, !error, NULL, error, ms);
  arena_free(a);
}

////////// 2. CALLERS //////////

static pl_gate *keys_gate;
static struct {
  char keyid[96], key[64], handle[64];
  double until;
} known[32];   // signers' keys looked up, each kept FRESH seconds

// Whether a signer is the owner: its key is the owner's, or its handle is.
static int owner_of(const char *key, const char *handle) {
  return *set.owner && ((key && !strcmp(key, set.owner)) || (handle && !strcmp(handle, set.owner)));
}

/* A signer's key, {key, handle, mine}: a key that names itself, ed25519:<base64>; one in keys; this computer's own name;
   else one of a handle's computers as Onsay's resolve endpoint lists it, its statement signed by Onsay. NULL when none. */
static val *keyed(arena *a, const char *keyid) {
  const char *dot = strchr(keyid, '.');
  const char *given = text_or(rec_get(set.all, "keys"), keyid, NULL);
  val *out = v_rec(a);
  if (!strncmp(keyid, "ed25519:", 8) || given) {
    const char *key = given ? given : keyid;
    rec_set(a, out, "key", v_cstr(a, key));
    rec_set(a, out, "handle", v_cstr(a, dot && given ? dot + 1 : ""));
    rec_set(a, out, "mine", VBOOL(owner_of(key, dot && given ? dot + 1 : NULL)));
    return out;
  }
  if (!dot || strchr(dot + 1, '.') || strlen(keyid) >= sizeof known[0].keyid) return NULL;
  int k = 0;
  pl_enter(keys_gate);
  while (k < 32 && !(!strcmp(known[k].keyid, keyid) && known[k].until > pl_now())) k++;
  pl_leave(keys_gate);
  if (k == 32) {
    buf error = { 0 };
    val *got = pl_http(a, "GET", arena_fmt(a, ONSAY "/resolve/%s", keyid), v_rec(a), NULL, 0, 10, &error);
    val *said = got ? json_parse(a, text_or(got, "text", ""), strlen(text_or(got, "text", "")), NULL) : NULL;
    val *devices = rec_get(said, "devices");
    buf_free(&error);
    for (size_t i = 0; devices && devices->k == V_LIST && i < devices->l.n && k == 32; i++) {
      val *d = devices->l.v[i];
      val *statement = v_rec(a);
      if (strncmp(text_or(d, "name", ""), keyid, (size_t)(dot - keyid)) || text_or(d, "name", "")[dot - keyid]) continue;
      rec_set(a, statement, "added", rec_get(d, "added") ? rec_get(d, "added") : NOTHING);
      rec_set(a, statement, "device", v_text(a, keyid, (size_t)(dot - keyid)));
      rec_set(a, statement, "handle", v_cstr(a, dot + 1));
      rec_set(a, statement, "account", rec_get(d, "account") ? rec_get(d, "account") : NOTHING);
      rec_set(a, statement, "key", rec_get(d, "key") ? rec_get(d, "key") : NOTHING);
      if (strlen(text_or(d, "key", "")) >= sizeof known[0].key || !signed_by(statement, rec_get(d, "statement"), NULL))
        continue;
      pl_enter(keys_gate);
      for (k = 0; k < 31 && known[k].until > pl_now(); k++) {}
      snprintf(known[k].keyid, sizeof known[k].keyid, "%s", keyid);
      snprintf(known[k].key, sizeof known[k].key, "%s", text_or(d, "key", ""));
      snprintf(known[k].handle, sizeof known[k].handle, "%s", dot + 1);
      known[k].until = pl_now() + FRESH;
      pl_leave(keys_gate);
    }
  }
  if (k == 32) return NULL;
  pl_enter(keys_gate);
  rec_set(a, out, "key", v_cstr(a, known[k].key));
  rec_set(a, out, "handle", v_cstr(a, known[k].handle));
  rec_set(a, out, "mine", VBOOL(owner_of(known[k].key, known[k].handle)));
  pl_leave(keys_gate);
  return out;
}

// The policy in force, read as each call comes, so a change by hand or by a lingo holds at once: home/policy.json's
// grants, {grants: [...]}. Everything is closed until one opens it.
static val *policy(arena *a) {
  buf text = { 0 };
  val *out = v_rec(a);
  val *file = pl_read(arena_fmt(a, "%s/policy.json", set.root), &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  val *grants = rec_get(file, "grants");
  buf_free(&text);
  rec_set(a, out, "own", grants && grants->k == V_LIST ? grants : v_list(a));
  rec_set(a, out, "grants", v_list(a));
  return out;
}

////////// 3. OFFERS //////////

static pl_gate *offer_gate;

/* Ready code: the way the lingo search found for a request, {address, inputs} as JSON, kept by the request as it came, so
   the same request goes straight there next time, the way DNS keeps an answer. One is dropped when it fails or after
   READY_FOR, and every one when what is offered here changes. */
#define READY 256
#define READY_FOR 86400
static struct { char *key, *way; double at; } ready[READY];
static size_t ready_next;
static pl_gate *ready_gate;

// The way kept for a request, in a's memory, or NULL.
static const char *ready_find(arena *a, const char *key) {
  const char *way = NULL;
  pl_enter(ready_gate);
  for (size_t i = 0; i < READY && !way; i++)
    if (ready[i].key && !strcmp(ready[i].key, key) && pl_now() - ready[i].at < READY_FOR) way = v_cstr(a, ready[i].way)->t.s;
  pl_leave(ready_gate);
  return way;
}

// A request's way kept, or with no way dropped, or with no request every one dropped.
static void ready_keep(const char *key, const char *way) {
  pl_enter(ready_gate);
  size_t slot = ready_next;
  for (size_t i = 0; i < READY; i++) {
    if (!ready[i].key || (key && strcmp(ready[i].key, key))) continue;
    free(ready[i].key);
    free(ready[i].way);
    ready[i].key = ready[i].way = NULL;
    slot = i;
  }
  if (key && way) {
    free(ready[slot].key);
    free(ready[slot].way);
    ready[slot].key = strdup(key);
    ready[slot].way = strdup(way);
    ready[slot].at = pl_now();
    if (slot == ready_next) ready_next = (ready_next + 1) % READY;
  }
  pl_leave(ready_gate);
}
static void offered_sent(void);
static val *natives(arena *a);

// The table, home/offered.json: {name: {address or code, about, settings, openapi}}, settings the names of those its code
// reads, read as each call comes, so a change holds at once.
static val *offers(arena *a) {
  buf text = { 0 };
  val *t = pl_read(arena_fmt(a, "%s/offered.json", set.root), &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  buf_free(&text);
  return t && t->k == V_REC ? t : v_rec(a);
}

/* A lingo's OpenAPI document, made from its listing as its code says it: each function a POST at /<name>/<function>,
   its words the summary and description, its values' types and words the request's schema, what it gives the answer's,
   and x-lingo-internal on a helper, one whose name starts with _. */
static val *openapi_of(arena *a, const char *name, val *l) {
  val *doc = v_rec(a);
  val *info = v_rec(a);
  val *paths = v_rec(a);
  val *x = v_rec(a);
  val *phrases = rec_get(l, "phrases");
  rec_set(a, info, "title", v_cstr(a, name));
  rec_set(a, info, "version", v_cstr(a, "1.0.0"));
  if (rec_get(l, "about")) rec_set(a, info, "description", rec_get(l, "about"));
  for (size_t i = 0; phrases && i < phrases->l.n; i++) {
    val *p = phrases->l.v[i];
    const char *id = text_or(p, "id", "");
    const char *does = text_or(p, "does", "");
    const char *stop = strstr(does, ". ");
    val *op = v_rec(a);
    val *item = v_rec(a);
    val *body = v_rec(a);
    val *content = v_rec(a);
    val *json = v_rec(a);
    val *ok = v_rec(a);
    val *responses = v_rec(a);
    if (*does) rec_set(a, op, "summary", v_text(a, does, stop ? (size_t)(stop - does + 1) : strlen(does)));
    if (*does) rec_set(a, op, "description", v_cstr(a, does));
    rec_set(a, json, "schema", rec_get(p, "inputSchema") ? rec_get(p, "inputSchema") : v_rec(a));
    rec_set(a, content, "application/json", json);
    rec_set(a, body, "content", content);
    rec_set(a, op, "requestBody", body);
    rec_set(a, ok, "description", v_cstr(a, "The function's value, as JSON"));
    if (rec_get(p, "gives")) {
      val *answer = v_rec(a);
      val *got = v_rec(a);
      rec_set(a, got, "schema", type_schema(a, text_or(p, "gives", "any")));
      rec_set(a, answer, "application/json", got);
      rec_set(a, ok, "content", answer);
    }
    rec_set(a, responses, "200", ok);
    rec_set(a, op, "responses", responses);
    rec_set(a, op, "x-lingo-function", v_cstr(a, id));
    if (truthy(rec_get(p, "internal") ? rec_get(p, "internal") : NOTHING)) rec_set(a, op, "x-lingo-internal", VBOOL(1));
    rec_set(a, item, "post", op);
    rec_set(a, paths, arena_fmt(a, "/%s/%s", name, strchr(id, '.') ? strchr(id, '.') + 1 : id), item);
  }
  rec_set(a, x, "name", v_cstr(a, name));
  rec_set(a, x, "route", v_cstr(a, arena_fmt(a, "/%s", name)));
  rec_set(a, doc, "openapi", v_cstr(a, "3.1.0"));
  rec_set(a, doc, "info", info);
  rec_set(a, doc, "paths", paths);
  rec_set(a, doc, "x-lingo", x);
  return doc;
}

/* 3. lingo.offer(name, code, address): a lingo offering itself here, its code, Lingo or only its functions' signatures,
      read by the compiler into what it does, and where it answers, an address its calls are sent to. With no address,
      its code is kept in the table and runs right here, stacked with each script that names it. Its functions, each its
      name and what it does, but its helpers. Offered again, it replaces what was; with no code, it is taken away. A
      name that is one of this computer's own groups, like files, can't be offered. */
static val *lingo_offer(run *r, val **a) {
  arena *ra = r->a;
  const char *name = a[0]->t.s;
  const char *code = a[1]->k == V_TEXT ? a[1]->t.s : "";
  val *out = v_list(ra);
  if (!named(name)) fail(r, "type", "A lingo's name is lower case letters, digits, _ and -, like weather");
  const char *prefix = arena_fmt(ra, "%s.", name);
  for (size_t k = 0; k <= BUILT_IN; k++) {
    val *ps = native_phrases(ra, k);
    for (size_t i = 0; i < ps->l.n; i++)
      if (!strncmp(text_or(ps->l.v[i], "id", ""), prefix, strlen(prefix)))
        fail(r, "denied", "%s is one of this computer's own, so it can't be offered", name);
  }
  pl_enter(offer_gate);
  val *table = offers(ra);
  val *kept = v_rec(ra);
  for (size_t i = 0; i < table->r.n; i++)
    if (strcmp(table->r.key[i], name)) rec_set(ra, kept, table->r.key[i], table->r.v[i]);
  if (*code) {
    const char *why = NULL;
    val *stacked = v_list(ra);
    val *e = v_rec(ra);
    rec_set(ra, e, "file", v_cstr(ra, arena_fmt(ra, "%s.ls", name)));
    rec_set(ra, e, "code", v_cstr(ra, code));
    list_push(ra, stacked, e);
    val *inst = lingo_load(ra, natives(ra), stacked, &why);
    val *l = inst ? rec_get(rec_get(inst, "lingos"), name) : NULL;
    if (!l) {
      pl_leave(offer_gate);
      fail(r, "phrase", "%s", why ? why : "Its code doesn't say what it does");
    }
    val *entry = v_rec(ra);
    if (a[2]->k == V_TEXT && *a[2]->t.s)
      rec_set(ra, entry, "address", a[2]);
    else
      rec_set(ra, entry, "code", v_cstr(ra, code));
    if (rec_get(l, "about")) rec_set(ra, entry, "about", rec_get(l, "about"));
    if (rec_get(l, "settings")) rec_set(ra, entry, "settings", rec_get(l, "settings"));
    rec_set(ra, entry, "openapi", openapi_of(ra, name, l));
    rec_set(ra, kept, name, entry);
    val *phrases = rec_get(l, "phrases");
    for (size_t i = 0; i < phrases->l.n; i++) {
      val *p = phrases->l.v[i];
      if (rec_get(p, "internal")) continue;
      val *one = v_rec(ra);
      rec_set(ra, one, "name", v_cstr(ra, strchr(text_or(p, "id", ""), '.') + 1));
      rec_set(ra, one, "does", rec_get(p, "does"));
      list_push(ra, out, one);
    }
  }
  buf text = { 0 };
  buf_json(&text, kept);
  int ok = pl_write(arena_fmt(ra, "%s/offered.json", set.root), text.s, text.n);
  buf_free(&text);
  pl_leave(offer_gate);
  ready_keep(NULL, NULL);
  if (!ok) fail(r, "phrase", "This computer couldn't keep what was offered");
  offered_sent();
  return out;
}

/* 3. lingo.list(): every lingo here, {name, about, functions: [{name, does}]}: this computer's own groups first, like
      input and window, each call's does its values and what it does, then every lingo offered, from its OpenAPI, its
      helpers left out. std and lingo, part of the language, aren't listed. */
static val *lingo_list(run *r, val **a) {
  val *table = offers(r->a);
  val *out = v_list(r->a);
  val *groups = v_rec(r->a);
  (void)a;
  for (size_t k = 0; k <= BUILT_IN; k++) {
    val *ps = native_phrases(r->a, k);
    for (size_t i = 0; i < ps->l.n; i++) {
      const char *id = text_or(ps->l.v[i], "id", "");
      const char *dot = strchr(id, '.');
      if (!dot) continue;
      const char *group = arena_fmt(r->a, "%.*s", (int)(dot - id), id);
      if (!strcmp(group, "std") || !strcmp(group, "lingo")) continue;
      val *one = rec_get(groups, group);
      if (!one) {
        one = v_rec(r->a);
        rec_set(r->a, one, "name", v_cstr(r->a, group));
        rec_set(r->a, one, "about", v_cstr(r->a, "Onsay's own calls on this computer"));
        rec_set(r->a, one, "functions", v_list(r->a));
        rec_set(r->a, groups, group, one);
        list_push(r->a, out, one);
      }
      buf line = { 0 };
      val *params = rec_get(ps->l.v[i], "params");
      buf_chr(&line, '(');
      for (size_t p = 0; params && p < params->l.n; p++) buf_fmt(&line, "%s%s", p ? ", " : "", text_or(params->l.v[p], "name", ""));
      buf_fmt(&line, ") %s", text_or(ps->l.v[i], "does", ""));
      val *f = v_rec(r->a);
      rec_set(r->a, f, "name", v_cstr(r->a, dot + 1));
      rec_set(r->a, f, "does", v_text(r->a, line.s, line.n));
      buf_free(&line);
      list_push(r->a, rec_get(one, "functions"), f);
    }
  }
  for (size_t i = 0; i < table->r.n; i++) {
    val *one = v_rec(r->a);
    val *functions = v_list(r->a);
    val *paths = rec_get(rec_get(table->r.v[i], "openapi"), "paths");
    for (size_t k = 0; paths && paths->k == V_REC && k < paths->r.n; k++) {
      val *op = rec_get(paths->r.v[k], "post");
      const char *slash = strrchr(paths->r.key[k], '/');
      if (!op || rec_get(op, "x-lingo-internal")) continue;
      val *f = v_rec(r->a);
      rec_set(r->a, f, "name", v_cstr(r->a, slash ? slash + 1 : paths->r.key[k]));
      rec_set(r->a, f, "does", v_cstr(r->a, text_or(op, "description", "")));
      list_push(r->a, functions, f);
    }
    rec_set(r->a, one, "name", v_cstr(r->a, table->r.key[i]));
    rec_set(r->a, one, "about", v_cstr(r->a, text_or(table->r.v[i], "about", "")));
    rec_set(r->a, one, "functions", functions);
    list_push(r->a, out, one);
  }
  return out;
}

// 3. lingo.code(name): the code of a lingo kept here, or nothing for one that answers elsewhere or isn't offered.
static val *lingo_code(run *r, val **a) {
  val *code = rec_get(rec_get(offers(r->a), a[0]->t.s), "code");
  return code ? code : NOTHING;
}

// Where an offered lingo answers, by its name, or NULL.
static const char *offered_at(arena *a, const char *name) {
  return text_or(rec_get(offers(a), name), "address", NULL);
}

////////// 4. CALLS //////////

static _Thread_local caller now;       // who the call running on this thread is from
static _Thread_local val *now_policy;  // and the policy it is judged by
static _Thread_local const char *now_run;   // the Onsay-Run it came with, passed on with every call it makes, so whoever
                                            // started it, like the app, knows which of its runs a call is part of

// 4. Each call of the computer's a script makes but pure computing, a request to its address, like /files/read, judged
//    by the policy for whoever sent the script. At the command line, the owner's.
void may(run *r, const char *id) {
  if (!now.name) return;
  const char *route = arena_fmt(r->a, "/%s", id);
  const char *why = NULL;
  for (char *x = (char *)route; *x; x++)
    if (*x == '.') *x = '/';
  int in = let_in(r->a, now_policy, &now, route, 0, &why);
  logged(r->a, "call", now.name, route, in, why, NULL, 0);
  if (!in) fail(r, "denied", "%s can't make %s here: no grant lets them", now.name, id);
}

/* One call sent where it goes, signed by this computer: values as JSON in a POST, and its answer, the JSON its body is.
   A record with error, or a status of 400 or more, is the call's failure. */
static val *sent(run *r, const char *url, val *values, int asked) {
  arena *a = r->a;
  val *headers = v_rec(a);
  buf body = { 0 };
  buf error = { 0 };
  double began = pl_now();
  buf_json(&body, values);
  rec_set(a, headers, "Content-Type", v_cstr(a, "application/json"));
  if (now_run) rec_set(a, headers, "Onsay-Run", v_cstr(a, now_run));
  sign_request(a, headers, "POST", url, body.s, body.n);
  val *got = pl_http(a, "POST", url, headers, body.s, body.n, set.ms / 1000, &error);
  val *status = rec_get(got, "status");
  const char *text = text_or(got, "text", "");
  val *answer = got ? json_parse(a, text, strlen(text), NULL) : NULL;
  const char *why = !got ? arena_fmt(a, "%s", error.s ? error.s : "it couldn't be reached")
      : rec_get(answer, "error") ? text_or(answer, "error", "")
      : status && status->n >= 400 ? arena_fmt(a, "it answered %g", status->n) : NULL;
  logged(a, "out", set.name, url, !why, NULL, why, (pl_now() - began) * 1000);
  buf_free(&body);
  buf_free(&error);
  if (why && !got) fail(r, "phrase", "%s", why);
  if (why && asked) fail(r, text_or(answer, "code", "phrase"), "%s", why);   // the search's own words, for the person
  if (why) fail(r, text_or(answer, "code", "phrase"), "%s: %s", url, why);
  if (asked) return rec_get(answer, "value") ? rec_get(answer, "value") : NOTHING;
  return answer ? answer : v_cstr(a, text);
}

// A call nothing here answers, like weather.now: sent to where the lingo offered as its group answers, its values in order,
// else the error it always was. A phrase that failed here stays its own failure.
static val *searched(run *r, const char *id, val *values, const char *code, const char *message) {
  const char *dot = strchr(id, '.');
  const char *group = dot ? arena_fmt(r->a, "%.*s", (int)(dot - id), id) : id;
  const char *where = offered_at(r->a, group);
  if (strcmp(code, "unknown") || !where || !dot) fail(r, code, "%s", message);
  may(r, id);
  return sent(r, arena_fmt(r->a, "%s/%s", where, dot + 1), values, 0);
}

// What Onsay's lingo search is asked, {ask, values, gives, mode} or {address, values}, answered {value} or {error, code}.
static val *asked(run *r, val *question) {
  return sent(r, ONSAY "/search/ask", question, 1);
}

static val *run_script(arena *a, const char *script, val *values, const caller *who, const char **code, const char **message);
static const char *called_as(arena *a, const char *path, val *given, val *values);

/* A call of an address from here: a web address sent to as it is, a lingo offered here sent where it answers, a lingo
   kept here or one of this computer's own calls, like input/move_in, run here, as whoever runs this, the call judged as
   theirs, and any other, like a lingo on another computer, reached through Onsay. */
static val *used(run *r, const char *address, val *values) {
  size_t n = strcspn(address, "/");
  const char *name = arena_fmt(r->a, "%.*s", (int)n, address);
  const char *where = offered_at(r->a, name);
  const char *own = address[n] == '/' ? arena_fmt(r->a, "%s.%.*s", name, (int)strcspn(address + n + 1, "/?"), address + n + 1) : "";
  if (!strncmp(address, "http://", 7) || !strncmp(address, "https://", 8)) return sent(r, address, values, 0);
  if (where) return sent(r, arena_fmt(r->a, "%s%s", where, address + n), values, 0);
  if (address[n] == '/' && (rec_get(rec_get(offers(r->a), name), "code") || phrase_find(own))) {
    val *in = v_list(r->a);
    const char *script = called_as(r->a, address, values && (values->k == V_REC || values->k == V_LIST) ? values : v_rec(r->a), in);
    const char *id = script + strlen("return ");
    if (!phrase_find(own)) may(r, arena_fmt(r->a, "%.*s", (int)strcspn(id, "("), id));   // an own call judges itself
    caller who = now;
    val *policy_was = now_policy;
    const char *code = NULL;
    const char *message = NULL;
    val *got = run_script(r->a, script, in, &who, &code, &message);
    now = who;
    now_policy = policy_was;
    if (!got) fail(r, code, "%s", message ? message : "it failed");
    return got;
  }
  val *q = v_rec(r->a);
  rec_set(r->a, q, "address", v_cstr(r->a, address));
  rec_set(r->a, q, "values", values);
  return asked(r, q);
}

/* lingo.run(ask, values, gives), what lingo(...) is: the lingo search, built in. Onsay's index gives the ways that answer
   it, best first, nothing run there, and each is called from here until one answers, at most WAYS_MOST. A way that needs
   an input nobody gave is passed over. Its answer, or the last way's failure, or unknown when none was found. Each way
   tried is a line of the log, so Onsay learns which work. */
#define WAYS_MOST 3
static _Thread_local int ways_tried;   // ways the last lingo(...) on this thread called, for a request the search was given
static _Thread_local const char *way_found;   // the way that answered it, {address, inputs} as JSON, kept as ready code
static val *lingo_run(run *r, val **a) {
  val *q = v_rec(r->a);
  rec_set(r->a, q, "ask", a[0]);
  rec_set(r->a, q, "values", a[1]);
  rec_set(r->a, q, "gives", a[2]);
  rec_set(r->a, q, "mode", v_cstr(r->a, "list"));
  ways_tried = 0;
  way_found = NULL;
  val *ways = asked(r, q);
  volatile int tried = 0;
  for (volatile size_t i = 0; ways->k == V_LIST && i < ways->l.n && tried < WAYS_MOST; i++) {
    val *way = ways->l.v[i];
    val *needs = rec_get(way, "needs");
    val *inputs = rec_get(way, "inputs");
    if (needs && needs->k == V_LIST && needs->l.n) continue;
    tried++;
    ways_tried = tried;
    trap_t t;
    void *volatile prev = r->trap;
    volatile int depth = r->depth;
    r->trap = &t;
    if (!setjmp(t.j)) {
      val *got = used(r, text_or(way, "address", ""), inputs ? inputs : v_rec(r->a));
      r->trap = prev;
      buf found = { 0 };
      buf_json(&found, way);
      way_found = v_text(r->a, found.s, found.n)->t.s;
      buf_free(&found);
      return got;
    }
    r->trap = prev;
    r->depth = depth;
    if (!strcmp(r->code, "limit") || !strcmp(r->code, "stopped")) rethrow(r);
  }
  if (tried) rethrow(r);
  fail(r, "unknown", "Nothing found that does that yet");
}

// lingo.ways(ask, values).
static val *lingo_ways(run *r, val **a) {
  val *q = v_rec(r->a);
  rec_set(r->a, q, "ask", a[0]);
  rec_set(r->a, q, "values", a[1]);
  rec_set(r->a, q, "mode", v_cstr(r->a, "list"));
  return asked(r, q);
}

// lingo.use(address, values)
static val *lingo_use(run *r, val **a) {
  return used(r, a[0]->t.s, a[1]);
}

// lingo.setting(key, reference): the floor keeps no settings, so it is asked of the lingo offered as settings.
static val *setting_(run *r, val **a) {
  val *values = v_list(r->a);
  list_push(r->a, values, a[0]);
  list_push(r->a, values, a[1]);
  return searched(r, "settings.get", values, "unknown", "Nothing keeps settings here: no lingo called settings is offered");
}

////////// 5. LOG //////////

static pl_gate *log_gate;
static buf waiting;   // lines not yet sent to Onsay
static long log_size = -1;
static _Thread_local const char *now_program;   // the program on this computer the call on this thread came from

/* A line of home/requests.log, and kept to send: {at, way, ms, caller, program, route, allowed, why, error}. way is in, a
   call that came in; call, a call a script made; out, one sent from here. program is the file of the program on this
   computer that made a call coming in, and on each call its script made, when the port can tell, so the person can be
   asked about that program by name. */
static void logged(arena *a, const char *way, const char *who, const char *route, int allowed, const char *why,
    const char *error, double ms) {
  val *line = v_rec(a);
  buf text = { 0 };
  buf all = { 0 };
  char at[32];
  time_t t = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  rec_set(a, line, "at", v_cstr(a, at));
  rec_set(a, line, "way", v_cstr(a, way));
  if (ms) rec_set(a, line, "ms", v_num(a, ms));
  rec_set(a, line, "caller", v_cstr(a, who && *who ? who : "this computer"));
  if (now_program && strcmp(way, "out")) rec_set(a, line, "program", v_cstr(a, now_program));
  rec_set(a, line, "route", v_cstr(a, route));
  rec_set(a, line, "allowed", VBOOL(allowed));
  if (why) rec_set(a, line, "why", v_cstr(a, why));
  if (error) rec_set(a, line, "error", v_cstr(a, error));
  buf_json(&text, line);
  buf_chr(&text, '\n');
  const char *path = arena_fmt(a, "%s/requests.log", set.root);
  pl_enter(log_gate);
  if (log_size < 0 || log_size > LOG_MOST) {
    log_size = pl_read(path, &all) ? (long)all.n : 0;
    const char *half = log_size > LOG_MOST ? memchr(all.s + log_size / 2, '\n', log_size - log_size / 2) : NULL;
    if (half && pl_write(path, half + 1, all.s + log_size - half - 1)) log_size = all.s + log_size - half - 1;
  }
  if (pl_append(path, text.s, text.n)) log_size += (long)text.n;
  buf_add(&waiting, text.s, text.n);
  if (waiting.n > LOG_MOST) {
    const char *half = memchr(waiting.s + waiting.n / 2, '\n', waiting.n - waiting.n / 2);
    size_t cut = half ? (size_t)(half + 1 - waiting.s) : waiting.n;
    memmove(waiting.s, waiting.s + cut, waiting.n - cut);
    waiting.n -= cut;
  }
  pl_leave(log_gate);
  buf_free(&text);
  buf_free(&all);
}

/* 5. Lines of the log, sealed so only Onsay can read them and signed by this computer, POSTed to Onsay's log box:
      {key, name, hold, eph, nonce, box, sig}. The seal is X25519 with Onsay's key turned to it and a fresh key of this
      one's, the shared secret hashed with both into a ChaCha20-Poly1305 key (RFC 8439); sig is this computer's Ed25519
      signature over eph, nonce and box. With hold, Onsay may keep it until a call for this computer comes, up to
      HOLD_MOST seconds, so calls reach a computer nothing can reach directly, over plain web requests every system has.
      Onsay's answer, a record, or NULL when it didn't go. */
static val *posted(arena *a, const char *lines, size_t n, int hold) {
  unsigned char onsay[32], theirs[32], eph[32], mine[32], shared[32], key[32], nonce[12], sig[64], me[32];
  buf raw = { 0 };
  if (!key_bytes(ONSAY_KEY, onsay) || !key_of("device", 1, me)) return NULL;
  crypto_eddsa_to_x25519(theirs, onsay);
  pl_random(eph, 32);
  crypto_x25519_public_key(mine, eph);
  crypto_x25519(shared, eph, theirs);
  buf_add(&raw, (char *)shared, 32);
  buf_add(&raw, (char *)mine, 32);
  buf_add(&raw, (char *)theirs, 32);
  sha256(raw.s, raw.n, key);
  pl_random(nonce, sizeof nonce);
  unsigned char *box = malloc(n + 16);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, nonce);
  crypto_aead_write(&ctx, box, box + n, NULL, 0, (const unsigned char *)lines, n);
  raw.n = 0;
  buf_add(&raw, (char *)mine, 32);
  buf_add(&raw, (char *)nonce, sizeof nonce);
  buf_add(&raw, (char *)box, n + 16);
  key_sign("device", raw.s, raw.n, sig);
  buf body = { 0 };
  buf part = { 0 };
  base64(&part, me, 32);
  buf_fmt(&body, "{\"key\":\"ed25519:%s\",\"name\":", part.s);
  buf_json_str(&body, set.name, strlen(set.name));
  buf_fmt(&body, ",\"hold\":%s", hold ? "true" : "false");
  part.n = 0;
  base64(&part, mine, 32);
  buf_fmt(&body, ",\"eph\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, nonce, sizeof nonce);
  buf_fmt(&body, ",\"nonce\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, box, n + 16);
  buf_fmt(&body, ",\"box\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, sig, 64);
  buf_fmt(&body, ",\"sig\":\"%s\"}", part.s);
  val *headers = v_rec(a);
  buf error = { 0 };
  rec_set(a, headers, "Content-Type", v_cstr(a, "application/json"));
  val *got = pl_http(a, "POST", ONSAY "/logs", headers, body.s, body.n, hold ? HOLD_MOST + 10 : 10, &error);
  val *status = rec_get(got, "status");
  val *said = NULL;
  if (status && status->k == V_NUM && status->n == 200) {
    const char *text = text_or(got, "text", "");
    said = json_parse(a, text, strlen(text), NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  } else {
    // A post that didn't go is a line of the log, so why this computer looks offline to Onsay can be read here.
    const char *answered = got ? text_or(got, "text", "") : "";
    const char *why = !got ? (error.s ? error.s : "it couldn't be reached")
        : arena_fmt(a, "it answered %g: %.200s", status ? status->n : 0, answered);
    logged(a, "out", set.name, ONSAY "/logs", 0, NULL, why, 0);
  }
  crypto_wipe(eph, 32);
  crypto_wipe(shared, 32);
  crypto_wipe(key, 32);
  free(box);
  buf_free(&raw);
  buf_free(&body);
  buf_free(&part);
  buf_free(&error);
  return said;
}

static void relayed(void *arg);

/* 5. What waits, posted and held for calls. How many seconds to wait before the next post: what Onsay's answer says in
      next, and when it doesn't go, twice the last wait, at least SEND_EVERY and up to SEND_MOST. Each call in Onsay's
      answer is taken on its own thread, and offer asks for the table again, which Onsay lost. */
static double send_log(double last) {
  arena *a = arena_new();
  buf lines = { 0 };
  pl_enter(log_gate);
  buf_add(&lines, waiting.s ? waiting.s : "", waiting.n);
  pl_leave(log_gate);
  val *said = posted(a, lines.s ? lines.s : "", lines.n, 1);
  double wait = last < SEND_EVERY ? SEND_EVERY : last * 2;
  if (wait > SEND_MOST) wait = SEND_MOST;
  if (said) {
    val *next = rec_get(said, "next");
    val *calls = rec_get(said, "calls");
    val *offer = rec_get(said, "offer");
    wait = next && next->k == V_NUM && next->n >= 0 ? next->n : SEND_EVERY;
    pl_enter(log_gate);
    size_t gone = lines.n <= waiting.n ? lines.n : waiting.n;
    memmove(waiting.s, waiting.s + gone, waiting.n - gone);
    waiting.n -= gone;
    pl_leave(log_gate);
    if (offer && truthy(offer)) offered_sent();
    for (size_t i = 0; calls && calls->k == V_LIST && i < calls->l.n; i++) {
      buf one = { 0 };
      buf_json(&one, calls->l.v[i]);
      pl_thread(relayed, one.s);
    }
  }
  buf_free(&lines);
  arena_free(a);
  return wait;
}

/* 5. The table as Onsay indexes it, a line of what waits to be sent: {way: offered, lingos: [{name, openapi}]}, never
      where they answer. As it starts, and each time it changes. */
static void offered_sent(void) {
  arena *a = arena_new();
  val *table = offers(a);
  val *line = v_rec(a);
  val *lingos = v_list(a);
  buf text = { 0 };
  for (size_t i = 0; i < table->r.n; i++) {
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, table->r.key[i]));
    rec_set(a, one, "openapi", rec_get(table->r.v[i], "openapi") ? rec_get(table->r.v[i], "openapi") : v_rec(a));
    list_push(a, lingos, one);
  }
  rec_set(a, line, "way", v_cstr(a, "offered"));
  rec_set(a, line, "lingos", lingos);
  buf_json(&text, line);
  buf_chr(&text, '\n');
  pl_enter(log_gate);
  buf_add(&waiting, text.s, text.n);
  pl_leave(log_gate);
  buf_free(&text);
  arena_free(a);
}

static void sender(void *arg) {
  double wait = SEND_EVERY;
  (void)arg;
  for (;;) {
    pl_sleep(wait);
    wait = send_log(wait);
  }
}

////////// 6. RUNNING //////////

// This computer's own calls, as the compiler reads them, made in a's memory, since compiling writes into them.
static val *natives(arena *a) {
  val *out = v_list(a);
  val *core = v_rec(a);
  rec_set(a, core, "name", v_cstr(a, "core"));
  rec_set(a, core, "phrases", native_phrases(a, 0));
  list_push(a, out, core);
  for (size_t i = 0; i < BUILT_IN; i++) {
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, built_in[i].name));
    rec_set(a, one, "phrases", native_phrases(a, i + 1));
    list_push(a, out, one);
  }
  return out;
}

// Whether text names the lingo name, as name.function or name/function, not inside a longer name.
static int names_lingo(const char *text, const char *name) {
  size_t n = strlen(name);
  for (const char *p = strstr(text, name); p; p = strstr(p + 1, name)) {
    int alone = p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-');
    if (alone && (p[n] == '.' || p[n] == '/')) return 1;
  }
  return 0;
}

/* A script run for who: compiled as the body of fn main(a0, a1, ...) in call.ls, against the standard functions, the
   lingos kept here that it names, and those they name in turn, and this computer's own calls, and run with values as
   a0, a1 and on, within the limits. Its value, or NULL with code and message, compile when it isn't Lingo. Each runs on
   its own thread, side by side. */
static val *run_script(arena *a, const char *script, val *values, const caller *who, const char **code, const char **message) {
  buf code_text = { 0 };
  val *stacked = v_list(a);
  val *input = v_rec(a);
  volatile int stop = 0;
  buf_str(&code_text, "fn main(");
  for (size_t i = 0; i < values->l.n; i++) {
    buf_fmt(&code_text, "%sa%zu", i ? ", " : "", i);
    rec_set(a, input, arena_fmt(a, "a%zu", i), values->l.v[i]);
  }
  buf_fmt(&code_text, ") {%s\n}\n", unfenced(a, script, strlen(script)));
  for (size_t i = 0; i < sizeof STANDARD / sizeof *STANDARD; i++) {
    val *e = v_rec(a);
    rec_set(a, e, "file", v_cstr(a, arena_fmt(a, "%s.ls", STANDARD[i].name)));
    rec_set(a, e, "code", v_cstr(a, STANDARD[i].code));
    list_push(a, stacked, e);
  }
  val *e = v_rec(a);
  rec_set(a, e, "file", v_cstr(a, "call.ls"));
  rec_set(a, e, "code", v_text(a, code_text.s, code_text.n));
  list_push(a, stacked, e);
  buf_free(&code_text);
  // Each lingo kept here that something stacked names, until none more is named.
  val *table = offers(a);
  size_t standards = sizeof STANDARD / sizeof *STANDARD;
  for (int more = 1; more;) {
    more = 0;
    for (size_t i = 0; i < table->r.n && !more; i++) {
      const char *kept = text_or(table->r.v[i], "code", NULL);
      const char *file = arena_fmt(a, "%s.ls", table->r.key[i]);
      int there = 0;
      int named_here = 0;
      for (size_t k = standards; kept && k < stacked->l.n; k++) {
        there |= !strcmp(text_or(stacked->l.v[k], "file", ""), file);
        named_here |= names_lingo(text_or(stacked->l.v[k], "code", ""), table->r.key[i]);
      }
      if (!kept || there || !named_here) continue;
      val *one = v_rec(a);
      rec_set(a, one, "file", v_cstr(a, file));
      rec_set(a, one, "code", v_cstr(a, kept));
      list_push(a, stacked, one);
      more = 1;
    }
  }
  val *inst = lingo_load(a, natives(a), stacked, message);
  val *program = inst ? lingo_call(a, inst, "call.main") : NULL;
  if (!program) {
    *code = "compile";
    return NULL;
  }
  now = *who;
  now_policy = policy(a);
  val *got = run_program(a, program, input, (limits){ set.steps, set.ms, &stop }, code, message);
  now.name = NULL;
  return got;
}

/* /group/name with its values, a list in order or a record by name, as the script return group.name(a0, a1, ...), the
   values in order put in values: one of this computer's own calls by its names, an offered function's in the order its
   schema lists them, else the record as its one value. */
static const char *called_as(arena *a, const char *path, val *given, val *values) {
  buf call = { 0 };
  for (const char *p = path + (*path == '/'); *p && *p != '?'; p++) buf_chr(&call, *p == '/' ? '.' : *p);
  phrase *ph = call.s ? phrase_find(call.s) : NULL;
  if (given->k == V_LIST)
    for (size_t i = 0; i < given->l.n; i++) list_push(a, values, given->l.v[i]);
  for (int i = 0; given->k == V_REC && ph && i < ph->count; i++) {
    val *v = rec_get(given, ph->params[i]);
    list_push(a, values, v ? v : NOTHING);
  }
  const char *dot = call.s ? strchr(call.s, '.') : NULL;
  val *offer = dot && !ph ? rec_get(offers(a), arena_fmt(a, "%.*s", (int)(dot - call.s), call.s)) : NULL;
  const char *where[] = { "openapi", "paths", NULL, "post", "requestBody", "content", "application/json", "schema", "properties" };
  where[2] = dot ? arena_fmt(a, "/%.*s/%s", (int)(dot - call.s), call.s, dot + 1) : "";
  val *props = offer;
  for (size_t k = 0; props && k < sizeof where / sizeof *where; k++) props = rec_get(props, where[k]);
  for (size_t i = 0; given->k == V_REC && props && props->k == V_REC && i < props->r.n; i++) {
    val *v = rec_get(given, props->r.key[i]);
    list_push(a, values, v ? v : NOTHING);
  }
  if (given->k == V_REC && !ph && !(props && props->k == V_REC)) list_push(a, values, given);
  buf b = { 0 };
  buf_fmt(&b, "return %s(", call.s ? call.s : "");
  for (size_t i = 0; i < values->l.n; i++) buf_fmt(&b, "%sa%zu", i ? ", " : "", i);
  buf_chr(&b, ')');
  const char *script = v_text(a, b.s, b.n)->t.s;
  buf_free(&b);
  buf_free(&call);
  return script;
}

/* The one input: whatever came, a path like /test/data with its values, or text, Lingo, a prompt or anything else, as
   {script, values} when it carries values by name, like a chat's messages. Like DNS, in order:
     1. code ready to send: a path to something open here, a lingo kept or offered or one of this computer's own groups,
        is called; else a way the search found for this same request before is used again
     2. text the compiler can run is run
     3. anything else, or anything that failed to compile on the way, goes to the lingo search as it came, which tries
        the next best way when one fails, and the way that answered is kept as ready code
   Each is judged as the caller's, the search too, so a caller who may not search gets what it was: the compile error,
   or nothing at that path. When the search finds nothing, its words say so, for the person. Its status and body, JSON. */
#define SEARCHED "return lingo.run(a0, a1)"
static int answer(arena *a, const char *path, const char *body, size_t n, const caller *who, buf *out) {
  const char *code = "unknown";
  const char *message = NULL;
  const char *text = NULL;     // the request as it came: what ready code is kept by and the search is asked
  const char *script = NULL;   // what the compiler runs
  val *values = v_list(a);
  val *given = v_rec(a);       // its values by name, for the search
  val *got = NULL;
  int root = !strcmp(path, "/");
  val *sent = n ? json_parse(a, body, n, NULL) : NULL;
  if (root && sent && sent->k == V_REC && rec_get(sent, "script") && rec_get(sent, "script")->k == V_TEXT) {
    text = script = rec_get(sent, "script")->t.s;
    if (rec_get(sent, "values") && rec_get(sent, "values")->k == V_REC) given = rec_get(sent, "values");
  } else if (root) {
    text = script = v_text(a, body, n)->t.s;
  } else {
    text = path;
    if (!sent || (sent->k != V_LIST && sent->k != V_REC)) {
      sent = v_list(a);
      if (n) list_push(a, sent, v_text(a, body, n));
    }
    if (sent->k == V_REC) given = sent;
    else if (sent->l.n) rec_set(a, given, "values", sent);
    size_t len = strcspn(path + 1, "/?");
    const char *group = arena_fmt(a, "%.*s.", (int)len, path + 1);
    int open = rec_get(offers(a), arena_fmt(a, "%.*s", (int)len, path + 1)) != NULL;
    for (size_t k = 0; k <= BUILT_IN && !open; k++) {
      val *ps = native_phrases(a, k);
      for (size_t i = 0; i < ps->l.n && !open; i++) open = !strncmp(text_or(ps->l.v[i], "id", ""), group, len + 1);
    }
    if (open) script = called_as(a, path, sent, values);
  }

  // 1. A path to something open here.
  if (!root && script) {
    got = run_script(a, script, values, who, &code, &message);
    if (got || strcmp(code, "compile")) goto said;
  }
  // 1. A way found for this same request before, its inputs given anew where this request gives them.
  const char *kept = ready_find(a, text);
  val *way = kept ? json_parse(a, kept, strlen(kept), NULL) : NULL;
  if (way && way->k == V_REC) {
    val *inputs = rec_get(way, "inputs") && rec_get(way, "inputs")->k == V_REC ? rec_get(way, "inputs") : v_rec(a);
    for (size_t i = 0; i < given->r.n; i++) rec_set(a, inputs, given->r.key[i], given->r.v[i]);
    val *call = v_list(a);
    list_push(a, call, rec_get(way, "address") ? rec_get(way, "address") : v_cstr(a, ""));
    list_push(a, call, inputs);
    got = run_script(a, "return lingo.use(a0, a1)", call, who, &code, &message);
    if (got || !strcmp(code, "denied")) goto said;
    ready_keep(text, NULL);
  }
  // 2. Text the compiler can run.
  if (root) {
    got = run_script(a, script, values, who, &code, &message);
    if (got || strcmp(code, "compile")) goto said;
  }
  // 3. The lingo search, given the request as it came.
  {
    const char *was = message;
    const char *was_code = code;
    val *asking = v_list(a);
    list_push(a, asking, v_cstr(a, text));
    list_push(a, asking, given);
    ways_tried = 0;
    way_found = NULL;
    got = run_script(a, SEARCHED, asking, who, &code, &message);
    // Kept only when each value given goes into the way by its own name, so using it again with new ones is the same
    // as searching again; a value the search read something from, like a city out of the messages, isn't.
    val *found = got && way_found ? rec_get(json_parse(a, way_found, strlen(way_found), NULL), "inputs") : NULL;
    int same = found && found->k == V_REC;
    for (size_t i = 0; same && i < given->r.n; i++) same = rec_get(found, given->r.key[i]) != NULL;
    if (same) ready_keep(text, way_found);
    // What it was stands, unless the search tried a way or looked and said in words why nothing does it.
    int searched = ways_tried || !strcmp(code, "nothing");
    if (!got && !searched) {
      code = was_code;
      message = was ? was : arena_fmt(a, "There's nothing at %s", path);
    }
  }

said:
  if (got) {
    buf_json(out, got);
    return 200;
  }
  buf_str(out, "{\"error\":");
  buf_json_str(out, message ? message : "", message ? strlen(message) : 0);
  buf_str(out, ",\"code\":");
  buf_json_str(out, code, strlen(code));
  buf_chr(out, '}');
  if (!strcmp(code, "denied")) return 403;
  if (!strcmp(code, "unknown") || !strcmp(code, "nothing")) return 404;
  return !strcmp(code, "compile") ? 400 : 500;
}

////////// 7. PORT //////////

typedef struct {
  pl_conn *c;
  buf from;
} visit;

static void respond(pl_conn *c, int status, const char *body, size_t n) {
  buf out = { 0 };
  buf_fmt(&out, "HTTP/1.1 %d \r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, n);
  buf_add(&out, body, n);
  pl_put(c, out.s, out.n);
  buf_free(&out);
}

/* A call, however it came: its method, path, headers by lower case name and body, and where it came from, like
   ip 127.0.0.1. Its caller is whoever signed it (RFC 9421), else anyone from there, with the badge it carries in
   Onsay-Badge; where it came from never lets anyone in. GET / says what this is and its key, so an owner can add it to
   their record. Logged. Its status, and its body in out. */
static int called(arena *a, const char *method, const char *where, val *headers, const char *body, size_t n,
    const char *from, buf *out) {
  double began = pl_now();
  const char *bad = NULL;
  const char *why = NULL;
  int status = 401;
  caller who = { NULL, 0, 0, NULL };
  who.name = from;
  if (rec_get(headers, "signature-input")) bad = signer(a, headers, method, where, v_text(a, body, n), &who);
  if (!bad && rec_get(headers, "onsay-badge")) who.badge = badge_of(a, text_or(headers, "onsay-badge", ""), &who.begun, &bad);
  if (bad) {
    buf_str(out, "{\"error\":");
    buf_json_str(out, bad, strlen(bad));
    buf_chr(out, '}');
  } else if (!strcmp(method, "GET") && !strcmp(where, "/")) {
    unsigned char key[32];
    buf part = { 0 };
    status = 200;
    if (key_of("device", 1, key)) base64(&part, key, 32);
    buf_fmt(out, "{\"floor\":\"" FLOOR_VERSION "\",\"name\":");
    buf_json_str(out, set.name, strlen(set.name));
    buf_fmt(out, ",\"key\":\"ed25519:%s\"}", part.s ? part.s : "");
    buf_free(&part);
  } else {
    now_run = text_or(headers, "onsay-run", NULL);
    status = answer(a, where, body, n, &who, out);
    now_run = NULL;
    why = status == 403 ? "no grant lets them in" : NULL;
  }
  logged(a, "in", who.name, where, status < 400, why, status >= 400 ? out->s : NULL, (pl_now() - began) * 1000);
  return status;
}

// One web request, on its own thread: its first line, headers and body of Content-Length, read whole, and called from its ip.
static void visitor(void *arg) {
  visit v = *(visit *)arg;
  free(arg);
  arena *a = arena_new();
  buf in = { 0 };
  buf out = { 0 };
  const char *end = NULL;
  const char *bad = NULL;
  int status = 400;
  int got = 1;
  size_t size = 0;
  val *headers = v_rec(a);
  while (got && !(end = in.s ? strstr(in.s, "\r\n\r\n") : NULL) && in.n < HEAD) got = pl_get(v.c, &in, QUIET);
  size_t method = end ? strspn(in.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") : 0;
  size_t path = method && in.s[method] == ' ' && in.s[method + 1] == '/' ? strcspn(in.s + method + 1, " \r\n") : 0;
  if (!end || !path || !utf8_ok(in.s, (size_t)(end - in.s)))
    bad = "That isn't a web request. Its first line is like POST / HTTP/1.1";
  for (const char *s = bad ? end : strstr(in.s, "\r\n") + 2; s && s < end; s = strstr(s, "\r\n") + 2) {
    const char *eol = strstr(s, "\r\n");
    const char *colon = memchr(s, ':', (size_t)(eol - s));
    const char *value = colon;
    if (!colon || colon == s) {
      bad = "A header isn't a name, a colon and its value";
      break;
    }
    while (++value < eol && (*value == ' ' || *value == '\t')) {}
    header_add(a, headers, s, (size_t)(colon - s), value, (size_t)(eol - value));
  }
  const char *length = text_or(headers, "content-length", "0");
  if (!bad && (strspn(length, "0123456789") != strlen(length) || strlen(length) > 9 || (size = strtoul(length, NULL, 10)) > BODY_ROOM)) {
    status = 413;
    bad = "Its body isn't sent with a Content-Length of at most 4 MB";
  }
  size_t from = end ? (size_t)(end - in.s) + 4 : 0;
  while (!bad && in.n - from < size && (got = pl_get(v.c, &in, QUIET))) {}
  if (!bad && in.n - from < size) bad = "Its body was shorter than its Content-Length";
  else if (!bad && !utf8_ok(in.s + from, size)) bad = "Its body isn't text: lingo script, or JSON";

  const char *ip = arena_fmt(a, "ip %s", v.from.s ? v.from.s : "");
  buf program = { 0 };
  if (pl_program(v.c, &program)) now_program = v_text(a, program.s, program.n)->t.s;
  buf_free(&program);
  if (bad) {
    buf_str(&out, "{\"error\":");
    buf_json_str(&out, bad, strlen(bad));
    buf_chr(&out, '}');
    logged(a, "in", ip, "/", 0, NULL, out.s, 0);
  } else {
    const char *where = v_text(a, in.s + method + 1, path)->t.s;
    status = called(a, v_text(a, in.s, method)->t.s, where, headers, in.s + from, size, ip, &out);
  }
  now_program = NULL;
  respond(v.c, status, out.s ? out.s : "", out.n);
  pl_drop(v.c);
  buf_free(&in);
  buf_free(&out);
  buf_free(&v.from);
  arena_free(a);
}

/* A call that came for this computer through Onsay, {id, method, path, headers, body}, on its own thread: called like one
   from the port, and its answer sent straight back, a line {way: answer, id, status, body}, kept with what waits to be
   sent when that doesn't go. */
static void relayed(void *arg) {
  char *text = arg;
  arena *a = arena_new();
  val *call = json_parse(a, text, strlen(text), NULL);
  val *given = rec_get(call, "headers");
  val *base64ed = rec_get(call, "base64");
  val *headers = v_rec(a);
  val *line = v_rec(a);
  const char *body = text_or(call, "body", "");
  buf out = { 0 };
  buf said = { 0 };
  int status = 400;
  free(text);
  for (size_t i = 0; given && given->k == V_REC && i < given->r.n; i++) {
    val *v = given->r.v[i];
    if (v->k == V_TEXT) header_add(a, headers, given->r.key[i], strlen(given->r.key[i]), v->t.s, v->t.len);
  }
  if (base64ed && truthy(base64ed))
    buf_str(&out, "{\"error\":\"Its body isn't text: lingo script, or JSON\"}");
  else
    status = called(a, text_or(call, "method", "POST"), text_or(call, "path", "/"), headers, body, strlen(body), "through Onsay", &out);
  rec_set(a, line, "way", v_cstr(a, "answer"));
  rec_set(a, line, "id", rec_get(call, "id") ? rec_get(call, "id") : NOTHING);
  rec_set(a, line, "status", v_num(a, status));
  rec_set(a, line, "body", v_text(a, out.s ? out.s : "", out.n));
  buf_json(&said, line);
  buf_chr(&said, '\n');
  if (!posted(a, said.s, said.n, 0)) {
    pl_enter(log_gate);
    buf_add(&waiting, said.s, said.n);
    pl_leave(log_gate);
  }
  buf_free(&out);
  buf_free(&said);
  arena_free(a);
}

static void listener(void *arg) {
  pl_conn *p = arg;
  for (;;) {
    visit *v = calloc(1, sizeof *v);
    if ((v->c = pl_next(p, &v->from, 1)))
      pl_thread(visitor, v);
    else {
      buf_free(&v->from);
      free(v);
    }
  }
}

////////// 8. MAIN //////////

/*   floor settings.json                 runs it: calls come in on its port, and its log goes to Onsay
     floor settings.json key             its own key, ed25519:<base64>, made first when missing, for an owner's record
     floor settings.json run <script>    a script run here as the owner, its value printed as JSON */
int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "floor.json";
  const char *verb = argc > 2 ? argv[2] : NULL;
  if (!settings_read(path)) return 1;
  keys_gate = pl_gate_new();
  nonce_gate = pl_gate_new();
  log_gate = pl_gate_new();
  offer_gate = pl_gate_new();
  ready_gate = pl_gate_new();
  native_begin(0);
  std_phrases();
  phrase_add("lingo", "net", "run(ask: text, values: record=nothing, gives: text=\"\")", lingo_run);
  phrase_add("lingo", "net",
      "ways(ask: text, values: record=nothing) -> [{does: text, where: text, address: url, inputs: record}]: the best ways to do it, best first, nothing run",
      lingo_ways);
  phrase_add("lingo", "net", "use(address: url, values: record=nothing)", lingo_use);
  phrase_add("lingo", "net", "setting(key: text, reference: text)", setting_);
  phrase_add("lingo", "lingos",
      "offer(name: text, code: text, address: url=nothing) -> [{name: text, does: text}]: a lingo offering itself here, its "
      "code or its functions' signatures, and where its calls are sent, or with no address its code kept to run here, "
      "giving back its functions; with no code, taken away",
      lingo_offer);
  phrase_add("lingo", "none", "list() -> [{name: text, about: text, functions: [{name: text, does: text}]}]: every lingo offered here", lingo_list);
  phrase_add("lingo", "none", "code(name: text) -> text?: the code of a lingo kept here, nothing for one that answers elsewhere", lingo_code);
  for (size_t i = 0; i < BUILT_IN; i++) {
    native_begin(i + 1);
    built_in[i].add();
  }

  if (verb && !strcmp(verb, "key")) {
    unsigned char key[32];
    buf part = { 0 };
    if (!key_of("device", 1, key)) {
      fprintf(stderr, "This computer couldn't make or keep its key\n");
      return 1;
    }
    base64(&part, key, 32);
    printf("{\"key\":\"ed25519:%s\"}\n", part.s);
    buf_free(&part);
    return 0;
  }
  if (verb && !strcmp(verb, "run")) {
    arena *a = arena_new();
    buf out = { 0 };
    caller me = { "this computer", 1, 1, NULL };
    int status = answer(a, "/", argc > 3 ? argv[3] : "", argc > 3 ? strlen(argv[3]) : 0, &me, &out);
    printf("%.*s\n", (int)out.n, out.s ? out.s : "null");
    return status == 200 ? 0 : 1;
  }
  if (verb) {
    fprintf(stderr, "floor %s %s isn't something it does. It does key and run\n", path, verb);
    return 1;
  }
  buf error = { 0 };
  pl_conn *p = pl_bind("tcp", set.port, 1, &error);
  if (!p) {
    fprintf(stderr, "%s\n", error.s);
    return 1;
  }
  fprintf(stderr, "The floor %s takes calls on port %d\n", FLOOR_VERSION, set.port);
  offered_sent();
  pl_thread(sender, NULL);
  listener(p);
  return 0;
}

#include "hardware.c"
