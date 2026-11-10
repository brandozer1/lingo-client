// floor.c - the floor, the lingo client: one function, lingo script in and its value out, as a computer's hardware takes
// instructions. It takes a call, checks who signed it and whether the policy lets them, runs the script the call carries,
// logs it, and tries to send its log to Onsay. Nothing else: everything else is a lingo, called like any other. Built by
// build.ps1 beside lingo.c, which it replaces. See ../../onsay/docs/lingo-function.md, The floor.
//
// 1. Settings   its settings file: home, port, name, owner, keys, calls, ask, steps and ms, and the lingos built in
// 2. Callers    a signer's key, the owner, and the policy
// 3. Calls      each call a script makes: checked, then answered here or sent where it goes, signed
// 4. Log        every request, a line of home/requests.log, sent to Onsay sealed whenever a connection works
// 5. Running    a call's script compiled and run, one at a time, within its limits
// 6. Port       calls coming in, as web requests
// 7. Main       the command line

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
     calls   where calls it doesn't answer go, by group, {"files": "http://127.0.0.1:7000"}
     ask     where lingo(...), and use(...) of a name it can't reach, are asked, as JSON
     steps, ms   the most one call may take */
static struct {
  val *all;
  const char *home, *root, *name, *owner, *ask;
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
  set.ask = text_or(set.all, "ask", "");
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

////////// 3. CALLS //////////

static caller now;          // who the call running now is from
static val *now_policy;     // and the policy it is judged by

// 3. Each call of the computer's a script makes but pure computing, a request to its address, like /files/read, judged
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
  if (why) fail(r, text_or(answer, "code", "phrase"), "%s: %s", url, why);
  if (asked) return rec_get(answer, "value") ? rec_get(answer, "value") : NOTHING;
  return answer ? answer : v_cstr(a, text);
}

// A call nothing here answers, like files.read: sent to where calls lists its group, its values in order, else the error
// it always was. A phrase that failed here stays its own failure.
static val *searched(run *r, const char *id, val *values, const char *code, const char *message) {
  const char *dot = strchr(id, '.');
  const char *group = dot ? arena_fmt(r->a, "%.*s", (int)(dot - id), id) : id;
  const char *where = text_or(rec_get(set.all, "calls"), group, NULL);
  if (strcmp(code, "unknown") || !where || !dot) fail(r, code, "%s", message);
  may(r, id);
  return sent(r, arena_fmt(r->a, "%s/%s", where, dot + 1), values, 0);
}

// What lingo(...) and use(...) ask of ask: {ask, values, gives} or {address, values}, answered {value} or {error, code}.
static val *asked(run *r, val *question) {
  if (!*set.ask) fail(r, "unknown", "Nothing answers lingo(...) here: its settings name no ask");
  return sent(r, set.ask, question, 1);
}

// lingo.run(ask, values, gives).
static val *lingo_run(run *r, val **a) {
  val *q = v_rec(r->a);
  rec_set(r->a, q, "ask", a[0]);
  rec_set(r->a, q, "values", a[1]);
  rec_set(r->a, q, "gives", a[2]);
  return asked(r, q);
}

// lingo.ways(ask, values).
static val *lingo_ways(run *r, val **a) {
  val *q = v_rec(r->a);
  rec_set(r->a, q, "ask", a[0]);
  rec_set(r->a, q, "values", a[1]);
  rec_set(r->a, q, "mode", v_cstr(r->a, "list"));
  return asked(r, q);
}

// lingo.use(address, values): a web address is sent to as it is, a group calls names is sent there, and any other name is
// asked of ask.
static val *lingo_use(run *r, val **a) {
  const char *address = a[0]->t.s;
  size_t n = strcspn(address, "/");
  const char *where = text_or(rec_get(set.all, "calls"), arena_fmt(r->a, "%.*s", (int)n, address), NULL);
  if (!strncmp(address, "http://", 7) || !strncmp(address, "https://", 8)) return sent(r, address, a[1], 0);
  if (where) return sent(r, arena_fmt(r->a, "%s%s", where, address + n), a[1], 0);
  val *q = v_rec(r->a);
  rec_set(r->a, q, "address", a[0]);
  rec_set(r->a, q, "values", a[1]);
  return asked(r, q);
}

// lingo.setting(key, reference): the floor keeps no settings, so it is asked of whatever calls names for settings.
static val *setting_(run *r, val **a) {
  val *values = v_list(r->a);
  list_push(r->a, values, a[0]);
  list_push(r->a, values, a[1]);
  return searched(r, "settings.get", values, "unknown", "Nothing keeps settings here: its settings' calls name none");
}

////////// 4. LOG //////////

static pl_gate *log_gate;
static buf waiting;   // lines not yet sent to Onsay
static long log_size = -1;

// A line of home/requests.log, and kept to send: {at, way, ms, caller, route, allowed, why, error}. way is in, a call that
// came in; call, a call a script made; out, one sent from here.
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

/* 4. What waits, sealed so only Onsay can read it and signed by this computer, POSTed to Onsay's log box:
      {key, name, eph, nonce, box, sig}. The seal is X25519 with Onsay's key turned to it and a fresh key of this one's,
      the shared secret hashed with both into a ChaCha20-Poly1305 key (RFC 8439); sig is this computer's Ed25519
      signature over eph, nonce and box. Kept to try again when it doesn't go. How many seconds to wait before the next
      try: what Onsay's answer says in next, SEND_EVERY with nothing to send, and when it doesn't go, twice the last wait,
      up to SEND_MOST. */
static double send_log(double last) {
  arena *a = arena_new();
  buf lines = { 0 };
  unsigned char onsay[32], theirs[32], eph[32], mine[32], shared[32], key[32], nonce[12], sig[64], me[32];
  buf raw = { 0 };
  pl_enter(log_gate);
  buf_add(&lines, waiting.s ? waiting.s : "", waiting.n);
  pl_leave(log_gate);
  if (!lines.n || !key_bytes(ONSAY_KEY, onsay) || !key_of("device", 1, me)) {
    buf_free(&lines);
    arena_free(a);
    return SEND_EVERY;
  }
  crypto_eddsa_to_x25519(theirs, onsay);
  pl_random(eph, 32);
  crypto_x25519_public_key(mine, eph);
  crypto_x25519(shared, eph, theirs);
  buf_add(&raw, (char *)shared, 32);
  buf_add(&raw, (char *)mine, 32);
  buf_add(&raw, (char *)theirs, 32);
  sha256(raw.s, raw.n, key);
  pl_random(nonce, sizeof nonce);
  unsigned char *box = malloc(lines.n + 16);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, nonce);
  crypto_aead_write(&ctx, box, box + lines.n, NULL, 0, (unsigned char *)lines.s, lines.n);
  raw.n = 0;
  buf_add(&raw, (char *)mine, 32);
  buf_add(&raw, (char *)nonce, sizeof nonce);
  buf_add(&raw, (char *)box, lines.n + 16);
  key_sign("device", raw.s, raw.n, sig);
  buf body = { 0 };
  buf part = { 0 };
  base64(&part, me, 32);
  buf_fmt(&body, "{\"key\":\"ed25519:%s\",\"name\":", part.s);
  buf_json_str(&body, set.name, strlen(set.name));
  part.n = 0;
  base64(&part, mine, 32);
  buf_fmt(&body, ",\"eph\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, nonce, sizeof nonce);
  buf_fmt(&body, ",\"nonce\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, box, lines.n + 16);
  buf_fmt(&body, ",\"box\":\"%s\"", part.s);
  part.n = 0;
  base64(&part, sig, 64);
  buf_fmt(&body, ",\"sig\":\"%s\"}", part.s);
  val *headers = v_rec(a);
  buf error = { 0 };
  rec_set(a, headers, "Content-Type", v_cstr(a, "application/json"));
  val *got = pl_http(a, "POST", ONSAY "/logs", headers, body.s, body.n, 10, &error);
  val *status = rec_get(got, "status");
  double wait = last * 2 < SEND_MOST ? last * 2 : SEND_MOST;
  if (status && status->k == V_NUM && status->n == 200) {
    const char *text = text_or(got, "text", "");
    val *next = rec_get(json_parse(a, text, strlen(text), NULL), "next");
    wait = next && next->k == V_NUM && next->n >= 1 ? next->n : SEND_EVERY;
    pl_enter(log_gate);
    size_t gone = lines.n <= waiting.n ? lines.n : waiting.n;
    memmove(waiting.s, waiting.s + gone, waiting.n - gone);
    waiting.n -= gone;
    pl_leave(log_gate);
  }
  crypto_wipe(eph, 32);
  crypto_wipe(shared, 32);
  crypto_wipe(key, 32);
  free(box);
  buf_free(&lines);
  buf_free(&raw);
  buf_free(&body);
  buf_free(&part);
  buf_free(&error);
  arena_free(a);
  return wait;
}

static void sender(void *arg) {
  double wait = SEND_EVERY;
  (void)arg;
  for (;;) {
    pl_sleep(wait);
    wait = send_log(wait);
  }
}

////////// 5. RUNNING //////////

/* A script run for who: compiled as the body of fn main(a0, a1, ...) in call.ls, against the standard functions and this
   computer's own calls, and run with values as a0, a1 and on, within the limits. Its value, or NULL with code and
   message. One runs at a time. */
static pl_gate *run_gate;
static val *run_script(arena *a, const char *script, val *values, const caller *who, const char **code, const char **message) {
  buf code_text = { 0 };
  val *stacked = v_list(a);
  val *input = v_rec(a);
  val *offered = v_list(a);   // this computer's own calls, as the compiler reads them, made again in this call's memory,
  val *core = v_rec(a);       // since compiling writes into them
  volatile int stop = 0;
  rec_set(a, core, "name", v_cstr(a, "core"));
  rec_set(a, core, "phrases", native_phrases(a, 0));
  list_push(a, offered, core);
  for (size_t i = 0; i < BUILT_IN; i++) {
    val *one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, built_in[i].name));
    rec_set(a, one, "phrases", native_phrases(a, i + 1));
    list_push(a, offered, one);
  }
  buf_str(&code_text, "fn main(");
  for (size_t i = 0; i < values->l.n; i++) {
    buf_fmt(&code_text, "%sa%zu", i ? ", " : "", i);
    rec_set(a, input, arena_fmt(a, "a%zu", i), values->l.v[i]);
  }
  buf_fmt(&code_text, ") {\n%s\n}\n", script);
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
  val *inst = lingo_load(a, offered, stacked, message);
  val *program = inst ? lingo_call(a, inst, "call.main") : NULL;
  if (!program) {
    *code = "compile";
    return NULL;
  }
  pl_enter(run_gate);
  now = *who;
  now_policy = policy(a);
  val *got = run_program(a, program, input, (limits){ set.steps, set.ms, &stop }, code, message);
  now.name = NULL;
  pl_leave(run_gate);
  return got;
}

/* A call: a script, or /group/name with its values, a JSON list in order or a record by name for one of this computer's
   own calls, which becomes return group.name(a0, a1, ...). Its status and body, JSON. */
static int answer(arena *a, const char *path, const char *body, size_t n, const caller *who, buf *out) {
  const char *code = NULL;
  const char *message = NULL;
  const char *script = NULL;
  val *values = v_list(a);
  if (!strcmp(path, "/")) {
    script = v_text(a, body, n)->t.s;
  } else {
    const char *problem = NULL;
    val *given = n ? json_parse(a, body, n, &problem) : v_list(a);
    buf call = { 0 };
    for (const char *p = path + 1; *p && *p != '?'; p++) buf_chr(&call, *p == '/' ? '.' : *p);
    phrase *ph = call.s ? phrase_find(call.s) : NULL;
    if (problem || !given || (given->k != V_LIST && given->k != V_REC)) {
      buf_free(&call);
      buf_str(out, "{\"error\":\"Its values are a JSON list in order, or a record by name\"}");
      return 400;
    }
    if (given->k == V_LIST) values = given;
    for (int i = 0; given->k == V_REC && ph && i < ph->count; i++) {
      val *v = rec_get(given, ph->params[i]);
      list_push(a, values, v ? v : NOTHING);
    }
    if (given->k == V_REC && !ph) list_push(a, values, given);
    buf b = { 0 };
    buf_fmt(&b, "return %s(", call.s ? call.s : "");
    for (size_t i = 0; i < values->l.n; i++) buf_fmt(&b, "%sa%zu", i ? ", " : "", i);
    buf_chr(&b, ')');
    script = v_text(a, b.s, b.n)->t.s;
    buf_free(&b);
    buf_free(&call);
  }
  val *got = run_script(a, script, values, who, &code, &message);
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
  if (!strcmp(code, "unknown")) return 404;
  return !strcmp(code, "compile") ? 400 : 500;
}

////////// 6. PORT //////////

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

/* One call, on its own thread: a web request's first line, headers and body of Content-Length, read whole. Its caller is
   whoever signed it (RFC 9421), else anyone, ip and its address, with the badge it carries in Onsay-Badge; an address
   never lets anyone in. GET / says what this is and its key, so an owner can add it to their record. */
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
  double began = pl_now();
  val *headers = v_rec(a);
  caller who = { NULL, 0, 0, NULL };
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

  const char *where = bad ? "/" : v_text(a, in.s + method + 1, path)->t.s;
  const char *why = NULL;
  if (!bad) {
    val *body = v_text(a, in.s + from, size);
    who.name = arena_fmt(a, "ip %s", v.from.s ? v.from.s : "");
    if (rec_get(headers, "signature-input")) bad = signer(a, headers, v_text(a, in.s, method)->t.s, where, body, &who);
    if (!bad && rec_get(headers, "onsay-badge")) who.badge = badge_of(a, text_or(headers, "onsay-badge", ""), &who.begun, &bad);
    if (bad) status = 401;
  }
  if (bad) {
    buf_str(&out, "{\"error\":");
    buf_json_str(&out, bad, strlen(bad));
    buf_chr(&out, '}');
  } else if (!strncmp(in.s, "GET / ", 6)) {
    unsigned char key[32];
    buf part = { 0 };
    status = 200;
    if (key_of("device", 1, key)) base64(&part, key, 32);
    buf_fmt(&out, "{\"floor\":\"" FLOOR_VERSION "\",\"name\":");
    buf_json_str(&out, set.name, strlen(set.name));
    buf_fmt(&out, ",\"key\":\"ed25519:%s\"}", part.s ? part.s : "");
    buf_free(&part);
  } else {
    status = answer(a, where, in.s + from, size, &who, &out);
    why = status == 403 ? "no grant lets them in" : NULL;
  }
  logged(a, "in", who.name ? who.name : "", where, status < 400, why, status >= 400 ? out.s : NULL, (pl_now() - began) * 1000);
  respond(v.c, status, out.s ? out.s : "", out.n);
  pl_drop(v.c);
  buf_free(&in);
  buf_free(&out);
  buf_free(&v.from);
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

////////// 7. MAIN //////////

/*   floor settings.json                 runs it: calls come in on its port, and its log goes to Onsay
     floor settings.json key             its own key, ed25519:<base64>, made first when missing, for an owner's record
     floor settings.json run <script>    a script run here as the owner, its value printed as JSON */
int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "floor.json";
  const char *verb = argc > 2 ? argv[2] : NULL;
  if (!settings_read(path)) return 1;
  keys_gate = pl_gate_new();
  log_gate = pl_gate_new();
  run_gate = pl_gate_new();
  native_begin(0);
  std_phrases();
  phrase_add("lingo", "net", "run(ask: text, values: record=nothing, gives: text=\"\")", lingo_run);
  phrase_add("lingo", "net",
      "ways(ask: text, values: record=nothing) -> [{does: text, where: text, address: url, inputs: record}]: the best ways to do it, best first, nothing run",
      lingo_ways);
  phrase_add("lingo", "net", "use(address: url, values: record=nothing)", lingo_use);
  phrase_add("lingo", "net", "setting(key: text, reference: text)", setting_);
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
  pl_thread(sender, NULL);
  listener(p);
  return 0;
}

#include "hardware.c"
