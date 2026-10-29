// onsay.c - the onsay lingo, a native library the core loads (onsay.dll, onsay.so), which makes a lingo client Onsay's: its
// one connection to Onsay, signing in and this device's key, what Onsay signs checked against its key built in, the
// account's policy kept, decisions sent for the activity feed, and /onsay, where prompts come in. Everything it sends
// Onsay is kept in home/sent.log for the person to read. See docs/security.md in the onsay repo, The onsay lingo and the
// wire.
//
// 1. Onsay          where it is, its public key, checking what it signed, and sent.log
// 2. Connection     hello, each message from Onsay, the heartbeat and reconnecting, on threads of its own
// 3. Hooks          lingo(...) and use(...), a signer's key, decisions, and starting and stopping
// 4. Policy         fetched, checked and handed to the core
// 5. /onsay         prompts, with the prompt key, and sessions: what a prompt is doing as it runs, and the person's
//                   answers to what it asks
// 6. Signing in     sign_in, sign_out and status, from an app through /.lingo/call or from onsay-core <settings> call,
//                   and the account's devices
// 7. lingo_phrases
#include "core.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

////////// 1. ONSAY //////////

// Where Onsay is, built in, never a setting: its live connection here, and its API and resolve endpoint in core.h (ONSAY
// and RESOLVE). And its public key, as /.well-known/onsay-key gives it: every statement, request and policy Onsay signs
// is checked against it, never against a key only told over the wire. The release key is set here when Onsay's is
// fixed; until then a release trusts nothing Onsay signs. The test build talks to core/test/server.mjs, which signs with
// the test key.
#ifdef TEST
#define LIVE "ws://127.0.0.1:8787/live"
#define ONSAY_KEY "ed25519:6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw="
#else
#define LIVE "wss://api.onsay.dev/live"
#define ONSAY_KEY "ed25519:JAyqe6gpH0e2k51yjygP66uAo92x0ebK8q3w+y/B4Tw="
#endif
#define BEAT 20       // seconds between pings
#define SILENT 60     // seconds without a word before the link is taken as dead
#define FRESH 300     // seconds a signature, or a key looked up, holds
#define LOG_MOST (4 << 20)   // bytes sent.log may grow to before it is cut to its newest half
#define STEPS 100            // the newest steps of a prompt kept while it runs
#define TURNS 20             // the newest turns of a session sent with a prompt, as chat messages

#define say(...) (fprintf(stderr, "onsay: " __VA_ARGS__), fputc('\n', stderr))

// value as Onsay signs it: JSON with every record's keys sorted and no spaces.
static void canonical(buf *b, val *x) {
  if (x->k == V_LIST) {
    buf_chr(b, '[');
    for (size_t i = 0; i < x->l.n; i++) { if (i) buf_chr(b, ','); canonical(b, x->l.v[i]); }
    buf_chr(b, ']');
  } else if (x->k == V_REC) {
    size_t order[256], n = x->r.n < 256 ? x->r.n : 256;
    for (size_t i = 0; i < n; i++) {
      size_t j = i;
      for (; j > 0 && strcmp(x->r.key[order[j - 1]], x->r.key[i]) > 0; j--) order[j] = order[j - 1];
      order[j] = i;
    }
    buf_chr(b, '{');
    for (size_t i = 0; i < n; i++) {
      if (i) buf_chr(b, ',');
      buf_json_str(b, x->r.key[order[i]], strlen(x->r.key[order[i]]));
      buf_chr(b, ':');
      canonical(b, x->r.v[order[i]]);
    }
    buf_chr(b, '}');
  } else buf_json(b, x);
}

// A key, ed25519:<base64>, as its 32 bytes. 0 when it isn't one.
static int key_bytes(const char *text, unsigned char key[32]) {
  buf raw = { 0 };
  int ok = text && !strncmp(text, "ed25519:", 8) && unbase64(text + 8, strlen(text + 8), &raw) && raw.n == 32;
  if (ok) memcpy(key, raw.s, 32);
  buf_free(&raw);
  return ok;
}

// Whether signed, <anything>:<base64>, is a signature over value as Onsay signs it, by key, Onsay's when NULL.
static int signed_by(val *value, val *signed_, const char *key) {
  unsigned char k[32];
  const char *s = signed_ && signed_->k == V_TEXT ? signed_->t.s : "", *colon = strchr(s, ':');
  buf text = { 0 }, raw = { 0 };
  canonical(&text, value);
  int ok = colon && key_bytes(key ? key : ONSAY_KEY, k) && unbase64(colon + 1, strlen(colon + 1), &raw) && raw.n == 64 &&
           ed25519_check(k, text.s, text.n, (unsigned char *)raw.s);
  buf_free(&text), buf_free(&raw);
  return ok;
}

// Seconds since 1970 of an ISO time, like 2026-10-28T14:00:00.000Z, or 0.
static double seconds_of(const char *iso) {
  int y, mo, d, h, mi;
  double s;
  if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  y -= mo <= 2;
  long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return (double)(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468) * 86400 + h * 3600 + mi * 60 + s;
}

/* Each message sent to Onsay, a line of home/sent.log, {at, message}, every token in it masked, so the person can read
   what went. Pings are left out. Cut to its newest half when it passes LOG_MOST. */
static pl_gate *logging;
static void sent(const char *m, size_t n) {
  static long size = -1;
  buf line = { 0 }, path = { 0 }, all = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  buf_fmt(&line, "{\"at\":\"%s\",\"message\":", at);
  for (size_t i = 0; i < n; i++)
    if (n - i > 9 && !memcmp(m + i, "\"token\":\"", 9)) {
      buf_str(&line, "\"token\":\"(hidden)\"");
      for (i += 9; i < n && m[i] != '"'; i++) if (m[i] == '\\') i++;
    } else buf_chr(&line, m[i]);
  buf_str(&line, "}\n");
  buf_fmt(&path, "%s/sent.log", lingo_home());
  pl_enter(logging);
  if (size < 0 || size > LOG_MOST) {
    size = pl_read(path.s, &all) ? (long)all.n : 0;
    const char *half = size > LOG_MOST ? memchr(all.s + size / 2, '\n', size - size / 2) : NULL;
    if (half && pl_write(path.s, half + 1, all.s + size - half - 1)) size = all.s + size - half - 1;
  }
  if (pl_append(path.s, line.s, line.n)) size += (long)line.n;
  pl_leave(logging);
  buf_free(&line), buf_free(&path), buf_free(&all);
}

static int tell(pl_link *l, const char *m, size_t n) {
  int ok = pl_send(l, m, n);
  if (ok) sent(m, n);
  return ok;
}

// One request to Onsay's API: its answer as a record, and its status, or NULL with why in error when it wasn't reached.
static val *api(arena *a, const char *method, const char *path, const char *body, double *status, buf *error) {
  buf url = { 0 };
  buf_fmt(&url, "{\"type\":\"api\",\"method\":\"%s\",\"path\":", method);
  buf_json_str(&url, path, strlen(path));
  buf_fmt(&url, ",\"body\":%s}", body ? body : "null");
  sent(url.s, url.n);
  url.n = 0;
  val *headers = v_rec(a), *token = lingo_setting(a, "onsay", "token"), *said = NULL;
  rec_set(a, headers, "content-type", v_cstr(a, "application/json"));
  if (token && token->k == V_TEXT && token->t.len) {
    buf bearer = { 0 };
    buf_fmt(&bearer, "Bearer %s", token->t.s);
    rec_set(a, headers, "authorization", v_text(a, bearer.s, bearer.n));
    buf_free(&bearer);
  }
  buf_fmt(&url, "%s%s", ONSAY, path);
  val *got = pl_http(a, method, url.s, headers, body ? body : "", body ? strlen(body) : 0, 30, error), *text = rec_get(got, "text");
  buf_free(&url);
  if (got) {
    *status = rec_get(got, "status")->n;
    said = json_parse(a, text->t.s, text->t.len, NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  }
  return said;
}

////////// 2. CONNECTION //////////

/* One message sent and its answer waited for: a lingo(...) or a prompt. A prompt's also has its session, the steps Onsay
   says it takes as it runs, a line of {text, depth} each, and asks, the questions it waits on the person for, as JSON. */
typedef struct wait { struct wait *next; long id; volatile int done; buf reply; const char *session; buf steps, asks; } wait;

static pl_gate *lock;                     // keeps everything below, which the connection, the heartbeat and the job thread share
static pl_link *link_;                    // the open link, after its hello, or NULL
static int on, ready, ponged;             // on: the lingo runs. ready: Onsay took the hello, and its statement checked out
static double heard;                      // when Onsay last said anything
static char handle[64], device[64];       // this computer's handle and its name on it, as ready said
static unsigned char mine[32];            // this device's public key
static buf catalog, pending;              // the catalog now, and decisions not sent yet, a line each
static wait *waits;
static long ids;

static int send_(const char *text, size_t n) {
  pl_enter(lock);
  int ok = link_ && tell(link_, text, n);
  pl_leave(lock);
  return ok;
}

// Decisions not sent yet, as log messages of up to 1000 each, once Onsay is ready. Inside the lock.
static void flush(void) {
  while (ready && link_ && pending.n) {
    buf m = { 0 };
    size_t at = 0;
    buf_str(&m, "{\"type\":\"log\",\"entries\":[");
    for (int k = 0; at < pending.n && k < 1000; k++) {
      size_t end = strcspn(pending.s + at, "\n");
      if (k) buf_chr(&m, ',');
      buf_add(&m, pending.s + at, end);
      at += end + 1;
    }
    buf_str(&m, "]}");
    int ok = tell(link_, m.s, m.n);
    buf_free(&m);
    if (!ok) return;
    memmove(pending.s, pending.s + at, pending.n - at);
    pending.n -= at;
  }
}

// A response sent back down the link, from the core's answer {id, status, headers, body, base64}.
static void answered(void *arg, const char *response, size_t n) {
  buf m = { 0 };
  buf_str(&m, "{\"type\":\"response\",");
  buf_add(&m, response + 1, n - 1);
  send_(m.s, m.n);
  buf_free(&m);
}

static void policy_pull(void);

/* A request from Onsay. as: "me" is the account's own call, which Onsay signs over {as, at, device, id, method, path,
   sha256}, sha256 its body's: handed in as me when it checks out, made within 5 minutes for this device, else a 401.
   Any other is one the proxy passed on, its caller the first address of its x-forwarded-for, or its signer, which the
   core checks. */
static void request(arena *a, val *m, const char *text, size_t n) {
  val *body = rec_get(m, "body"), *b64 = rec_get(m, "base64"), *signed_ = v_rec(a);
  const char *as = text_or(m, "as", NULL), *forwarded = text_or(rec_get(m, "headers"), "x-forwarded-for", "");
  if (!as) {
    buf caller = { 0 };
    buf_fmt(&caller, "ip %.*s", (int)strcspn(forwarded, ", "), forwarded);
    lingo_hand(text, n, caller.s, answered, NULL);
    buf_free(&caller);
    return;
  }
  buf bytes = { 0 }, sum64 = { 0 }, out = { 0 };
  unsigned char sum[32];
  if (b64 && truthy(b64)) unbase64(body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0, &bytes);
  else buf_add(&bytes, body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0);
  sha256(bytes.s, bytes.n, sum);
  base64(&sum64, sum, 32);
  rec_set(a, signed_, "as", v_cstr(a, as));
  rec_set(a, signed_, "at", rec_get(m, "at") ? rec_get(m, "at") : NOTHING);
  pl_enter(lock);
  rec_set(a, signed_, "device", v_cstr(a, device));
  pl_leave(lock);
  rec_set(a, signed_, "id", rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  rec_set(a, signed_, "method", rec_get(m, "method") ? rec_get(m, "method") : NOTHING);
  rec_set(a, signed_, "path", rec_get(m, "path") ? rec_get(m, "path") : NOTHING);
  rec_set(a, signed_, "sha256", v_text(a, sum64.s, sum64.n));
  double at = seconds_of(text_or(m, "at", NULL)), now = pl_now();
  if (!strcmp(as, "me") && at > now - FRESH && at < now + FRESH && signed_by(signed_, rec_get(m, "signed"), NULL)) lingo_hand(text, n, "me", answered, NULL);
  else {
    buf_str(&out, "{\"type\":\"response\",\"id\":");
    buf_json(&out, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    buf_str(&out, ",\"status\":401,\"headers\":{\"content-type\":\"application/json\"},\"body\":\"{\\\"error\\\":\\\"Onsay's signature on this "
                  "request doesn't check out, so it didn't run\\\"}\",\"base64\":false}");
    send_(out.s, out.n);
    say("A request said to be the account's own didn't check out, so it didn't run");
  }
  buf_free(&bytes), buf_free(&sum64), buf_free(&out);
}

// Each message from Onsay, on the connection's thread, so a stop or an answer lands while a request runs.
static void heard_(pl_link *l, const char *text, size_t n) {
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL), *id = rec_get(m, "id");
  const char *type = text_or(m, "type", "");
  if (!strcmp(type, "ready")) {
    val *statement = v_rec(a);
    buf key = { 0 };
    buf_str(&key, "ed25519:");
    base64(&key, mine, 32);
    rec_set(a, statement, "added", rec_get(m, "added") ? rec_get(m, "added") : NOTHING);
    rec_set(a, statement, "device", rec_get(m, "device") ? rec_get(m, "device") : NOTHING);
    rec_set(a, statement, "handle", rec_get(m, "handle") ? rec_get(m, "handle") : NOTHING);
    rec_set(a, statement, "key", v_text(a, key.s, key.n));
    buf_free(&key);
    if (!signed_by(statement, rec_get(m, "statement"), NULL)) {
      say("Onsay's statement on this computer's key doesn't check out, so it isn't trusted. Trying again");
      pl_close(l);
    } else {
      pl_enter(lock);
      snprintf(handle, sizeof handle, "%s", text_or(m, "handle", ""));
      snprintf(device, sizeof device, "%s", text_or(m, "device", ""));
      ready = 1;
      flush();
      pl_leave(lock);
      say("Signed in. This computer is %s.%s", device, handle);
      val *version = rec_get(m, "policy");
      if (version && version->k == V_NUM && version->n > 0) policy_pull();
    }
  } else if (!strcmp(type, "request") && ready) request(a, m, text, n);
  else if (!strcmp(type, "stop")) {
    buf j = { 0 };
    buf_json(&j, id ? id : NOTHING);
    lingo_stop(j.s);
    buf_free(&j);
  } else if ((!strcmp(type, "lingo") || !strcmp(type, "answer")) && id && id->k == V_NUM) {
    pl_enter(lock);
    for (wait *w = waits; w; w = w->next)
      if ((double)w->id == id->n) { buf_add(&w->reply, text, n); lingo_done(&w->done); }
    pl_leave(lock);
  } else if ((!strcmp(type, "step") || !strcmp(type, "choose")) && id && id->k == V_NUM) {
    val *step = v_rec(a);
    rec_set(a, step, "text", rec_get(m, "text") ? rec_get(m, "text") : NOTHING);
    rec_set(a, step, "depth", rec_get(m, "depth") ? rec_get(m, "depth") : v_num(a, 0));
    pl_enter(lock);
    for (wait *w = waits; w; w = w->next) {
      if ((double)w->id != id->n || !w->session) continue;
      if (*type == 'c') { w->asks.n = 0; buf_json(&w->asks, rec_get(m, "questions") ? rec_get(m, "questions") : NOTHING); continue; }
      buf_json(&w->steps, step);
      buf_chr(&w->steps, '\n');
      size_t lines = 0;
      for (size_t i = 0; i < w->steps.n; i++) lines += w->steps.s[i] == '\n';
      if (lines > STEPS) {
        size_t cut = strcspn(w->steps.s, "\n") + 1;
        memmove(w->steps.s, w->steps.s + cut, w->steps.n - cut);
        w->steps.n -= cut;
      }
    }
    pl_leave(lock);
  } else if (!strcmp(type, "policy")) policy_pull();
  else if (!strcmp(type, "pong")) {
    pl_enter(lock);
    ponged = 1;
    pl_leave(lock);
  } else if (strcmp(type, "lingo") && strcmp(type, "answer")) say("Onsay sent a message this lingo client doesn't know: %s", *type ? type : "not JSON");
  arena_free(a);
}

// Connects to Onsay while the lingo runs and has a token, and again after a drop, waiting longer each time it fails.
static void connection(void *arg) {
  for (double wait_ = 1;; pl_sleep(wait_), wait_ = wait_ * 2 > 30 ? 30 : wait_ * 2) {
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    buf error = { 0 }, hello = { 0 };
    if (!on || !token || token->k != V_TEXT || !token->t.len) { arena_free(a); wait_ = 1; continue; }
    if (!key_of("device", 1, mine)) { say("This computer couldn't make or keep its key, so it can't sign in"); arena_free(a); continue; }
    pl_link *l = pl_connect(LIVE, &error);
    if (!l) say("%s. Trying again in %.0f seconds", error.s ? error.s : "It couldn't connect to Onsay", wait_);
    buf_free(&error);
    if (l) {
      buf_str(&hello, "{\"type\":\"hello\",\"token\":");
      buf_json_str(&hello, token->t.s, token->t.len);
      buf_str(&hello, ",\"client\":\"lingo-client\",\"version\":\"" CORE_VERSION "\",\"key\":\"ed25519:");
      base64(&hello, mine, 32);
      buf_str(&hello, "\",\"catalog\":");
      pl_enter(lock);
      buf_add(&hello, catalog.s ? catalog.s : "[]", catalog.s ? catalog.n : 2);
      buf_chr(&hello, '}');
      link_ = l, heard = pl_now(), ponged = ready = 0;
      tell(l, hello.s, hello.n);
      pl_leave(lock);
      buf in = { 0 };
      while (pl_receive(l, &in)) {
        pl_enter(lock);
        heard = pl_now();
        pl_leave(lock);
        heard_(l, in.s, in.n);
        in.n = 0;
      }
      buf_free(&in);
      pl_enter(lock);
      if (ready) wait_ = 0.5;   // so the next drop starts its waiting over
      link_ = NULL, ready = 0;
      for (wait *w = waits; w; w = w->next) lingo_done(&w->done);
      pl_leave(lock);
      pl_free(l);
      say("The connection to Onsay closed");
    }
    buf_free(&hello);
    arena_free(a);
  }
}

// Pings every BEAT seconds, drops a link silent for SILENT, and one whose token is gone, signed out elsewhere.
static void heartbeat(void *arg) {
  for (;;) {
    pl_sleep(BEAT);
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    pl_enter(lock);
    if (link_ && ((ponged && pl_now() - heard > SILENT) || !token || token->k != V_TEXT || !token->t.len)) pl_close(link_);
    else if (link_) pl_send(link_, "{\"type\":\"ping\"}", 15);
    pl_leave(lock);
    arena_free(a);
  }
}

////////// 3. HOOKS //////////

// A prompt's steps, kept a line each, as a JSON list, or [] for none.
static void stepped(buf *out, buf *lines) {
  buf_chr(out, '[');
  for (size_t i = 0; lines && i < lines->n; i++) if (lines->s[i] != '\n' || i + 1 < lines->n) buf_chr(out, lines->s[i] == '\n' ? ',' : lines->s[i]);
  buf_chr(out, ']');
}

/* A message to Onsay, {"type": type, "id": a new number, ...rest}, rest a JSON record, and its answer by that id waited
   for, answering requests meanwhile. 1 with the whole answer in reply, 0 when Onsay isn't ready, the link closed, stop
   was set or seconds passed. session is a prompt's, else NULL: its answer gets its steps, and a prompt stopped here is
   stopped at Onsay too. */
static int exchange(const char *type, const char *rest, size_t n, volatile int *stop, double seconds, buf *reply, const char *session) {
  wait w = { .session = session };
  buf m = { 0 };
  pl_enter(lock);
  int going = ready && link_;
  if (going) {
    w.id = ++ids, w.next = waits, waits = &w;
    buf_fmt(&m, "{\"type\":\"%s\",\"id\":%ld,", type, w.id);
    buf_add(&m, rest + 1, n - 1);
    going = tell(link_, m.s, m.n);
  }
  pl_leave(lock);
  if (going) lingo_wait(&w.done, stop, seconds);
  pl_enter(lock);
  for (wait **at = &waits; *at; at = &(*at)->next) if (*at == &w) { *at = w.next; break; }
  pl_leave(lock);
  int got = w.reply.n > 0;
  if (got) buf_add(reply, w.reply.s, w.reply.n);
  // A prompt's answer carries every step it took, for an app that couldn't follow all of it as it ran.
  if (got && session && reply->s[reply->n - 1] == '}') {
    reply->n--;
    buf_str(reply, ",\"steps\":");
    stepped(reply, &w.steps);
    buf_chr(reply, '}');
  } else if (!got && session && *stop) {
    m.n = 0;
    buf_fmt(&m, "{\"type\":\"stop\",\"id\":%ld}", w.id);
    send_(m.s, m.n);
  }
  buf_free(&m), buf_free(&w.reply), buf_free(&w.steps), buf_free(&w.asks);
  return got;
}

static int ask(const char *values, size_t n, volatile int *stop, buf *answer) { return exchange("lingo", values, n, stop, 1e12, answer, NULL); }

/* A signer's key, by its keyid, a device and its handle like laptop.brandon: from Onsay's /resolve, its statement checked,
   and kept for 5 minutes. mine when the handle is this computer's own. */
static struct { char keyid[128], key[64], handle[64]; double until; } known[32];

static val *key(arena *a, const char *keyid) {
  const char *dot = strchr(keyid, '.');
  val *out = NULL;
  int k = 0;
  if (!dot || strchr(dot + 1, '.') || strlen(keyid) >= sizeof known[0].keyid) return NULL;
  pl_enter(lock);
  while (k < 32 && !(!strcmp(known[k].keyid, keyid) && known[k].until > pl_now())) k++;
  pl_leave(lock);
  if (k == 32) {
    buf path = { 0 }, error = { 0 };
    double status = 0;
    buf_fmt(&path, RESOLVE "%s", keyid);
    val *said = api(a, "GET", path.s, NULL, &status, &error), *devices = rec_get(said, "devices");
    for (size_t i = 0; status == 200 && devices && devices->k == V_LIST && i < devices->l.n && k == 32; i++) {
      val *d = devices->l.v[i], *statement = v_rec(a);
      if (strncmp(text_or(d, "name", ""), keyid, (size_t)(dot - keyid)) || text_or(d, "name", "")[dot - keyid]) continue;
      rec_set(a, statement, "added", rec_get(d, "added") ? rec_get(d, "added") : NOTHING);
      rec_set(a, statement, "device", v_text(a, keyid, (size_t)(dot - keyid)));
      rec_set(a, statement, "handle", v_cstr(a, dot + 1));
      rec_set(a, statement, "key", rec_get(d, "key") ? rec_get(d, "key") : NOTHING);
      if (strlen(text_or(d, "key", "")) >= sizeof known[0].key || !signed_by(statement, rec_get(d, "statement"), NULL)) continue;
      pl_enter(lock);
      for (k = 0; k < 31 && known[k].until > pl_now(); k++) {}
      snprintf(known[k].keyid, sizeof known[k].keyid, "%s", keyid);
      snprintf(known[k].key, sizeof known[k].key, "%s", text_or(d, "key", ""));
      snprintf(known[k].handle, sizeof known[k].handle, "%s", dot + 1);
      known[k].until = pl_now() + FRESH;
      pl_leave(lock);
    }
    buf_free(&path), buf_free(&error);
  }
  if (k < 32) {
    out = v_rec(a);
    pl_enter(lock);
    rec_set(a, out, "key", v_cstr(a, known[k].key));
    rec_set(a, out, "handle", v_cstr(a, known[k].handle));
    rec_set(a, out, "mine", VBOOL(*handle && !strcmp(known[k].handle, handle)));
    pl_leave(lock);
  }
  return out;
}

static void decided(const char *line) {
  pl_enter(lock);
  buf_str(&pending, line);
  buf_chr(&pending, '\n');
  flush();
  pl_leave(lock);
}

// Started, the first time: a prompt key made when there is none, and the connection's threads. Each time, the catalog
// sent on, as the core gives it. Stopped, the link closes.
static void running(const char *now) {
  static int started;
  pl_enter(lock);
  on = now != NULL;
  catalog.n = 0;
  if (now) buf_str(&catalog, now);
  if (link_ && !on) pl_close(link_);
  else if (link_) {
    buf m = { 0 };
    buf_str(&m, "{\"type\":\"catalog\",\"catalog\":");
    buf_str(&m, now);
    buf_chr(&m, '}');
    tell(link_, m.s, m.n);
    buf_free(&m);
  }
  pl_leave(lock);
  if (!on || started) return;
  started = 1;
  arena *a = arena_new();
  val *key_ = lingo_setting(a, "onsay", "prompt_key");
  if (!key_ || key_->k != V_TEXT || !key_->t.len) {
    unsigned char random[24];
    buf made = { 0 };
    pl_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++) buf_fmt(&made, "%02x", random[i]);
    if (!lingo_set("onsay", "prompt_key", v_text(a, made.s, made.n))) say("Its prompt key couldn't be kept, so /onsay answers no one");
    buf_free(&made);
  }
  arena_free(a);
  pl_thread(connection, NULL);
  pl_thread(heartbeat, NULL);
}

////////// 4. POLICY //////////

/* The account's policy, from GET /policy: {version, policy, signed}, signed over {policy, version}. Checked, then handed to
   the core, which keeps it unless its version is older than the one kept, and what the ceiling clamped sent back as
   applied. Anything that doesn't check out is refused, and said. */
static void policy_pull(void) {
  arena *a = arena_new();
  buf error = { 0 }, m = { 0 };
  double status = 0;
  val *said = api(a, "GET", "/policy", NULL, &status, &error), *both = v_rec(a), *version = rec_get(said, "version"), *policy = rec_get(said, "policy");
  const char *why = NULL;
  rec_set(a, both, "policy", policy ? policy : NOTHING);
  rec_set(a, both, "version", version ? version : NOTHING);
  pl_enter(lock);
  const char *name = v_cstr(a, device)->t.s;
  pl_leave(lock);
  if (!said || status != 200) say("The account's policy couldn't be fetched: %s", error.n ? error.s : text_or(said, "error", "Onsay said no"));
  else if (!policy || policy->k != V_REC || !version || version->k != V_NUM || !signed_by(both, rec_get(said, "signed"), NULL))
    say("The account's policy from Onsay doesn't check out, so it is refused");
  else {
    rec_set(a, policy, "version", version);
    val *clamped = lingo_policy(a, policy, name, &why);
    if (!clamped) say("%s, so it is refused", why);
    else {
      buf_fmt(&m, "{\"type\":\"applied\",\"version\":");
      buf_json(&m, version);
      buf_str(&m, ",\"clamped\":");
      buf_json(&m, clamped);
      buf_chr(&m, '}');
      send_(m.s, m.n);
    }
  }
  buf_free(&error), buf_free(&m);
  arena_free(a);
}

////////// 5. /ONSAY //////////

// A core phrase of the store, Onsay's own storage here, called as it is: serve already needs store.
static val *store(run *r, const char *what, val *key, val *value) {
  val *args[2] = { key, value ? value : NOTHING };
  return phrase_find(what)->fn(r, args);
}

/* A session's newest TURNS turns, then prompt, as chat messages the way OpenAI and OpenRouter take them, {role,
   content}: each turn's prompt the user's, and its text, or its questions a line each, the assistant's. */
static val *messages(arena *a, val *turns, val *prompt) {
  val *out = v_list(a);
  for (size_t i = turns->l.n > TURNS ? turns->l.n - TURNS : 0; i <= turns->l.n; i++) {
    val *t = i < turns->l.n ? turns->l.v[i] : NULL, *asked = rec_get(t, "questions");
    buf lines = { 0 };
    for (size_t k = 0; asked && asked->k == V_LIST && k < asked->l.n; k++)
      buf_fmt(&lines, "%s%s", k ? "\n" : "", asked->l.v[k]->k == V_TEXT ? asked->l.v[k]->t.s : text_or(asked->l.v[k], "question", ""));
    for (int assistant = 0; assistant < (t ? 2 : 1); assistant++) {
      val *m = v_rec(a), *said = !t ? prompt : !assistant ? rec_get(t, "prompt") : asked ? v_text(a, lines.s ? lines.s : "", lines.n) : rec_get(t, "text");
      rec_set(a, m, "role", v_cstr(a, assistant ? "assistant" : "user"));
      rec_set(a, m, "content", said ? said : v_cstr(a, ""));
      list_push(a, out, m);
    }
    buf_free(&lines);
  }
  return out;
}

static val *answer(run *r, int status, const char *error) {
  val *out = v_rec(r->a), *body = v_rec(r->a);
  rec_set(r->a, body, "error", v_cstr(r->a, error));
  rec_set(r->a, out, "status", v_num(r->a, status));
  rec_set(r->a, out, "body", body);
  return out;
}

/* Every request to /onsay, each with its prompt key as Authorization: Bearer <key>.
     POST /onsay {prompt, session, mode}   the prompt sent to Onsay with the session so far as chat messages, answered
                                       {session, text, steps} or {session, questions, steps}, steps every step it took.
                                       With no session, a new one. mode: run, the default, or decision, where every pick
                                       is the person's
     GET /onsay/sessions               [{session, prompt}], the newest first, each named by its first prompt
     GET /onsay/sessions/<id>          {session, turns}: each {prompt, text} or {prompt, questions}
     GET /onsay/sessions/<id>/live     {running, steps, questions}: while a prompt of it runs, its newest steps, each
                                       {text, depth}, and the questions it waits on the person for, or nothing
     POST /onsay/sessions/<id>/answers {answers}   the person's answers to those questions, in order, sent on to Onsay
   A session is kept in the store as onsay/sessions/<id>, its id the time it began and a few random letters, or one the
   app made the same way, so it can follow the prompt as it runs. */
static val *serve(run *r, val **v) {
  arena *a = r->a;
  val *q = v[0], *key_ = lingo_setting(a, "onsay", "prompt_key"), *json = rec_get(q, "json"), *out = v_rec(a);
  const char *method = text_or(q, "method", ""), *path = text_or(q, "path", "/"), *auth = text_or(rec_get(q, "headers"), "authorization", "");
  if (!key_ || key_->k != V_TEXT || !key_->t.len || strncmp(auth, "Bearer ", 7) || strcmp(auth + 7, key_->t.s))
    return answer(r, 401, "That isn't this computer's prompt key. Send it as Authorization: Bearer <prompt key>");
  if (!strcmp(method, "GET") && !strcmp(path, "/sessions")) {
    val *keys = store(r, "store.keys", v_cstr(a, "onsay/sessions/"), NULL), *list = v_list(a);
    for (size_t i = keys->l.n; i-- > 0;) {
      val *one = v_rec(a), *kept = store(r, "store.get", v_text(a, keys->l.v[i]->t.s, keys->l.v[i]->t.len), NULL), *turns;
      kept = kept->k == V_TEXT ? json_parse(a, kept->t.s, kept->t.len, NULL) : NULL;
      turns = rec_get(kept, "turns");
      rec_set(a, one, "session", v_cstr(a, keys->l.v[i]->t.s + 15));
      rec_set(a, one, "prompt", turns && turns->k == V_LIST && turns->l.n ? rec_get(turns->l.v[0], "prompt") : NOTHING);
      list_push(a, list, one);
    }
    return list;
  }
  const char *sub = !strncmp(path, "/sessions/", 10) ? strchr(path + 10, '/') : NULL;
  if (sub) {
    const char *session = v_text(a, path + 10, (size_t)(sub - path - 10))->t.s;
    int live = !strcmp(method, "GET") && !strcmp(sub, "/live"), answers = !strcmp(method, "POST") && !strcmp(sub, "/answers");
    val *given = rec_get(json, "answers");
    if (!live && !answers) return answer(r, 404, "A session takes GET /onsay/sessions/<session>/live and POST /onsay/sessions/<session>/answers");
    if (answers && (!given || given->k != V_LIST)) return answer(r, 400, "Send the answers as {\"answers\": [\"...\"]}, one for each question, in order");
    buf said = { 0 };
    long asked = 0;
    pl_enter(lock);
    wait *w = waits;
    while (w && !(w->session && !strcmp(w->session, session))) w = w->next;
    if (live) {
      buf_str(&said, "{\"running\":");
      buf_str(&said, w ? "true" : "false");
      buf_str(&said, ",\"steps\":");
      stepped(&said, w ? &w->steps : NULL);
      buf_str(&said, ",\"questions\":");
      if (w && w->asks.n) buf_add(&said, w->asks.s, w->asks.n); else buf_str(&said, "null");
      buf_chr(&said, '}');
    } else if (w && w->asks.n) asked = w->id, w->asks.n = 0;
    pl_leave(lock);
    if (live) {
      val *got = json_parse(a, said.s, said.n, NULL);
      buf_free(&said);
      return got;
    }
    if (!asked) return answer(r, 404, "Nothing in that session is waiting for an answer");
    buf_fmt(&said, "{\"type\":\"chosen\",\"id\":%ld,\"answers\":", asked);
    buf_json(&said, given);
    buf_chr(&said, '}');
    send_(said.s, said.n);
    buf_free(&said);
    return v_rec(a);
  }
  const char *id = !strcmp(method, "GET") && !strncmp(path, "/sessions/", 10) ? path + 10 : !strcmp(method, "POST") && !strcmp(path, "/") ? text_or(json, "session", "") : NULL;
  if (!id) return answer(r, 404, "/onsay takes POST /onsay {prompt, session, mode}, GET /onsay/sessions, GET /onsay/sessions/<session>, and its /live and /answers");
  if (strlen(id) > 64 || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(id)) return answer(r, 400, "A session is lower case letters, digits and -");
  if (!*id) {
    unsigned char random[3];
    pl_random(random, 3);
    buf made = { 0 };
    buf_fmt(&made, "%012llx%02x%02x%02x", (unsigned long long)(pl_now() * 1000), random[0], random[1], random[2]);
    id = v_text(a, made.s, made.n)->t.s;
    buf_free(&made);
  }
  buf whole = { 0 };
  buf_fmt(&whole, "onsay/sessions/%s", id);
  val *where = v_text(a, whole.s, whole.n), *kept, *turns;
  buf_free(&whole);
  kept = store(r, "store.get", where, NULL);
  kept = kept->k == V_TEXT ? json_parse(a, kept->t.s, kept->t.len, NULL) : NULL;
  turns = rec_get(kept, "turns");
  if (!turns || turns->k != V_LIST) turns = v_list(a);
  rec_set(a, out, "session", v_cstr(a, id));
  if (!strcmp(method, "GET")) {
    if (!kept) return answer(r, 404, "There's no session by that name here");
    rec_set(a, out, "turns", turns);
    return out;
  }
  val *prompt = rec_get(json, "prompt"), *rest = v_rec(a), *got, *turn = v_rec(a), *record = v_rec(a);
  if (!prompt || prompt->k != V_TEXT || !prompt->t.len) return answer(r, 400, "Send the prompt as {\"prompt\": \"...\"}");
  buf m = { 0 }, reply = { 0 };
  rec_set(a, rest, "prompt", prompt);
  rec_set(a, rest, "session", v_cstr(a, id));
  rec_set(a, rest, "messages", messages(a, turns, prompt));
  if (!strcmp(text_or(json, "mode", ""), "decision")) rec_set(a, rest, "mode", v_cstr(a, "decision"));
  buf_json(&m, rest);
  int answered_ = exchange("prompt", m.s, m.n, r->stop, 300, &reply, id);
  got = answered_ ? json_parse(a, reply.s, reply.n, NULL) : NULL;
  buf_free(&m), buf_free(&reply);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got) return answer(r, 503, "This computer isn't connected to Onsay now, so it can't answer. Try again in a moment");
  const char *part = rec_get(got, "questions") ? "questions" : "text";
  rec_set(a, turn, "prompt", prompt);
  rec_set(a, turn, part, rec_get(got, part) ? rec_get(got, part) : NOTHING);
  list_push(a, turns, turn);
  rec_set(a, record, "turns", turns);
  buf_json(&m, record);
  store(r, "store.put", where, v_text(a, m.s, m.n));
  buf_free(&m);
  rec_set(a, out, part, rec_get(turn, part));
  rec_set(a, out, "steps", rec_get(got, "steps") ? rec_get(got, "steps") : v_list(a));
  return out;
}

////////// 6. SIGNING IN //////////

static int attempt, signing;   // the sign in now, by its number, so a newer one or signing out ends it; signing: it waits
static char failed[256];       // why the last sign in in the background didn't finish, for status

/* Asks Onsay every two seconds whether the person pressed Connect, for as long as the code lasts, 10 minutes, then keeps
   the token as this lingo's secret setting and makes this device's key. The account it signed in as, or NULL with why.
   stop is the command line's. In the background it is NULL, and a newer sign in, or signing out, ends it instead. */
static val *connect_(arena *a, const char *body, volatile int *stop, int mine, buf *why) {
  volatile int never = 0;
  unsigned char k[32];
  double status = 0;
  for (double until = pl_now() + 600; pl_now() < until;) {
    if (stop) lingo_wait(&never, stop, 2);
    else pl_sleep(2);
    pl_enter(lock);
    int ended = stop ? *stop : mine != attempt;
    pl_leave(lock);
    if (ended) { buf_str(why, stop ? "Stopped by the person" : "Signing in again, or signing out, ended this sign in"); return NULL; }
    buf error = { 0 };
    val *poll = api(a, "POST", "/connect/poll", body, &status, &error);
    buf_free(&error);
    if (!poll || !strcmp(text_or(poll, "status", ""), "waiting")) continue;   // a moment without the web is waited out too
    if (strcmp(text_or(poll, "status", ""), "done") || !*text_or(poll, "token", "")) buf_fmt(why, "Signing in didn't finish: %s", text_or(poll, "error", "Onsay sent no token"));
    else if (!lingo_set("onsay", "token", rec_get(poll, "token"))) buf_str(why, "Signed in, but this computer couldn't keep its token");
    else if (!key_of("device", 1, k)) buf_str(why, "Signed in, but this computer couldn't make or keep its key");
    else return rec_get(poll, "account") ? rec_get(poll, "account") : v_rec(a);
    return NULL;
  }
  buf_str(why, "The code ran out before Connect was pressed. Sign in again");
  return NULL;
}

typedef struct { int mine; char body[]; } waiting;

// A sign in finished in the background, while the lingo client runs on. status says when it is done.
static void background(void *arg) {
  waiting *w = arg;
  arena *a = arena_new();
  buf why = { 0 };
  int done = connect_(a, w->body, NULL, w->mine, &why) != NULL;
  if (!done) say("%s", why.s);
  pl_enter(lock);
  if (w->mine == attempt) signing = 0, snprintf(failed, sizeof failed, "%s", done ? "" : why.s);
  pl_leave(lock);
  buf_free(&why);
  arena_free(a);
  free(w);
}

/* Signs this computer in from any device's browser: Onsay gives an address and a code to open there, and the person
   signs in and presses Connect. In a lingo client that serves, as an app calls it, the address and code come back at once
   and the rest waits in the background, which status follows, even with this lingo stopped, so a request never waits on it. On the command line it says them on the log and waits,
   giving the handle and this device's key. Its install id, kept in the store, gets it its own instance back when it signs
   in again. name is what Onsay calls it, its host name when left out. */
static val *sign_in(run *r, val **v) {
  arena *a = r->a;
  buf body = { 0 }, error = { 0 }, shown = { 0 };
  double status = 0;
  unsigned char key_[32], random[16];
  val *install = store(r, "store.get", v_cstr(a, "onsay/install"), NULL), *out = v_rec(a);
  if (install->k != V_TEXT) {
    buf made = { 0 };
    pl_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++) buf_fmt(&made, "%02x", random[i]);
    install = v_text(a, made.s, made.n);
    store(r, "store.put", v_cstr(a, "onsay/install"), install);
    buf_free(&made);
  }
  const char *name = v[0]->k == V_TEXT ? v[0]->t.s : getenv("COMPUTERNAME") ? getenv("COMPUTERNAME") : getenv("HOSTNAME") ? getenv("HOSTNAME") : "Server";
  buf_str(&body, "{\"name\":");
  buf_json_str(&body, name, strlen(name));
  buf_str(&body, ",\"install\":");
  buf_json(&body, install);
  buf_chr(&body, '}');
  val *start = api(a, "POST", "/connect", body.s, &status, &error), *account;
  buf_free(&error);
  if (!start) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 201) fail(r, "phrase", "Onsay didn't start signing in: %s", text_or(start, "error", "it said no"));
  say("Open %s on any device, sign in, and press Connect. Your code is %s", text_or(start, "url", ""), text_or(start, "code", ""));
  body.n = 0;
  buf_str(&body, "{\"poll\":");
  buf_json_str(&body, text_or(start, "poll", ""), strlen(text_or(start, "poll", "")));
  buf_chr(&body, '}');
  if (on || lingo_serves()) {
    waiting *w = malloc(sizeof *w + body.n + 1);
    memcpy(w->body, body.s, body.n + 1);
    pl_enter(lock);
    w->mine = ++attempt, signing = 1, *failed = 0;
    pl_leave(lock);
    pl_thread(background, w);
    rec_set(a, out, "url", rec_get(start, "url") ? rec_get(start, "url") : NOTHING);
    rec_set(a, out, "code", rec_get(start, "code") ? rec_get(start, "code") : NOTHING);
    buf_free(&body), buf_free(&error);
    return out;
  }
  error.n = 0;
  account = connect_(a, body.s, r->stop, 0, &error);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!account) fail(r, "phrase", "%s", error.s);
  key_of("device", 0, key_);
  buf_str(&shown, "ed25519:");
  base64(&shown, key_, 32);
  rec_set(a, out, "handle", rec_get(account, "handle") ? rec_get(account, "handle") : NOTHING);
  rec_set(a, out, "key", v_text(a, shown.s, shown.n));
  buf_free(&body), buf_free(&error), buf_free(&shown);
  return out;
}

// Forgets the token and this device's key, so signing in again makes a new one, and drops the connection.
static val *sign_out(run *r, val **v) {
  if (!lingo_set("onsay", "token", NOTHING)) fail(r, "phrase", "This computer couldn't forget its token");
  key_forget("device");
  pl_enter(lock);
  attempt++, signing = 0, *handle = *device = 0;
  if (link_) pl_close(link_);
  pl_leave(lock);
  return NOTHING;
}

/* {signed_in, signing_in, failed, running, connected, handle, device, key}: failed is why the last sign in didn't finish,
   or nothing, and running whether this lingo runs. signing_in, failed, connected, handle and device only in a lingo
   client that serves. */
static val *status(run *r, val **v) {
  arena *a = r->a;
  val *out = v_rec(a), *token = lingo_setting(a, "onsay", "token");
  unsigned char key_[32];
  buf shown = { 0 };
  if (key_of("device", 0, key_)) { buf_str(&shown, "ed25519:"); base64(&shown, key_, 32); }
  rec_set(a, out, "signed_in", VBOOL(token && token->k == V_TEXT && token->t.len));
  pl_enter(lock);
  rec_set(a, out, "signing_in", VBOOL(signing));
  rec_set(a, out, "failed", *failed ? v_cstr(a, failed) : NOTHING);
  rec_set(a, out, "running", VBOOL(on));
  rec_set(a, out, "connected", VBOOL(ready));
  rec_set(a, out, "handle", *handle ? v_cstr(a, handle) : NOTHING);
  rec_set(a, out, "device", *device ? v_cstr(a, device) : NOTHING);
  pl_leave(lock);
  rec_set(a, out, "key", shown.n ? v_text(a, shown.s, shown.n) : NOTHING);
  buf_free(&shown);
  return out;
}

// The account's computers as Onsay knows them, the oldest first: [{name, online, seen}], seen when each last said anything.
static val *devices(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  double status = 0;
  val *said = api(a, "GET", "/lingos", NULL, &status, &error), *computers = rec_get(said, "computers"), *out = v_list(a);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to see your devices");
  if (status != 200 || !computers || computers->k != V_LIST) fail(r, "phrase", "Onsay couldn't list your devices: %s", text_or(said, "error", "it said no"));
  for (size_t i = 0; i < computers->l.n; i++) {
    val *c = computers->l.v[i], *one = v_rec(a);
    rec_set(a, one, "name", rec_get(c, "name") ? rec_get(c, "name") : NOTHING);
    rec_set(a, one, "online", VBOOL(truthy(rec_get(c, "online"))));
    rec_set(a, one, "seen", rec_get(c, "seen") ? rec_get(c, "seen") : NOTHING);
    list_push(a, out, one);
  }
  return out;
}

////////// 7. LINGO_PHRASES //////////

LINGO_ENTRY void lingo_phrases(void) {
  static const lingo_hooks hooks = { "public", running, decided, ask, key };
  lock = pl_gate_new(), logging = pl_gate_new();
  lingo_hook(&hooks);
  phrase_add("onsay", "store", "serve(request): /onsay, where prompts come in with this computer's prompt key, each answered by Onsay, "
             "and the sessions they make", serve);
  phrase_add("onsay", "net, store", "sign_in(name=nothing) -> {url,code}, or {handle,key} on the command line: signs this computer in to "
             "Onsay from any device's browser, at the address given with the code; status says when it is done", sign_in);
  phrase_add("onsay", "none", "sign_out(): this computer forgets its sign in and its key", sign_out);
  phrase_add("onsay", "net", "devices() -> [{name,online,seen}]: the account's computers, whether each is online, and when each was last seen", devices);
  phrase_add("onsay", "none", "status() -> {signed_in,signing_in,failed,running,connected,handle,device,key}: whether this computer is signed in to "
             "Onsay, and as what", status);
}
