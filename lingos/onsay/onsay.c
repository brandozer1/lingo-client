// onsay.c - the onsay lingo, built into the lingo client on every computer (build.ps1), which makes it Onsay's: its
// one connection to Onsay, signing in and this device's key, what Onsay signs checked against its key built in, the
// account's policy kept, decisions sent for the activity feed, every request this computer makes sent in chunks, and
// /onsay, where prompts come in and the agent loop works through each: Onsay decides every step, and the calls run
// here. Everything it sends Onsay is kept in home/sent.log for the person to read. See docs/security.md in the onsay repo, The onsay lingo and the wire.
//
// 1. Onsay          where it is, its public key, checking what it signed, and sent.log
// 2. Connection     hello, each message from Onsay, the heartbeat and reconnecting, on threads of its own
// 3. Hooks          lingo(...) and use(...), a signer's key, decisions, and starting and stopping
// 4. Policy         fetched, checked and handed to the core
// 5. /onsay         prompts, with the prompt key, each worked through as the agent loop, and sessions: the whole log,
//                   what a prompt is doing as it runs, and the person's answers to what it asks
// 6. Screens       what lingos draw on, screen.open, show and close, each kept with its to-do, and streamed to
//                   anything watching /onsay/screens
// 7. Signing in     sign_in, sign_out and status, from an app through /.lingo/call or from onsay-core <settings> call,
//                   the account's devices, and where its trial of Onsay's AI stands
// 8. onsay_lingo()  adds its phrases and the base calls, as the lingo client starts
#include "core.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

////////// 1. ONSAY //////////

// Where Onsay is, built in, never a setting: its API, its resolve endpoint and its public key, in core.h (ONSAY, RESOLVE
// and ONSAY_KEY). Every statement, request and policy Onsay signs is checked against that
// key, never against a key only told over the wire. Its live connection is its API's address with ws for http, like
// wss://api.onsay.dev/live.
#define BEAT 20   // seconds between pings
#define SILENT 60   // seconds without a word before the link is taken as dead
#define FRESH 300   // seconds a signature, or a key looked up, holds
#define LOG_MOST (4 << 20)   // bytes sent.log may grow to before it is cut to its newest half
#define STEPS 100   // the newest steps of a prompt kept while it runs
#define TURNS 20   // the newest turns of a session sent with a prompt, as chat messages
#define MOST 8   // the most calls one prompt makes before it stops
#define CHUNK 100   // calls waiting before they are sent; the rest go with the next heartbeat

#define say core_say

// value as Onsay signs it: JSON with every record's keys sorted and no spaces.
static void canonical(buf *b, val *x) {
  if (x->k == V_LIST) {
    buf_chr(b, '[');
    for (size_t i = 0; i < x->l.n; i++) {
      if (i) buf_chr(b, ',');
      canonical(b, x->l.v[i]);
    }
    buf_chr(b, ']');
  } else if (x->k == V_REC) {
    size_t order[256];
    size_t n = x->r.n < 256 ? x->r.n : 256;
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
  } else
    buf_json(b, x);
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
  const char *s = signed_ && signed_->k == V_TEXT ? signed_->t.s : "";
  const char *colon = strchr(s, ':');
  buf text = { 0 };
  buf raw = { 0 };
  canonical(&text, value);
  int ok = colon && key_bytes(key ? key : ONSAY_KEY, k) && unbase64(colon + 1, strlen(colon + 1), &raw) &&
      raw.n == 64 && ed25519_check(k, text.s, text.n, (unsigned char *)raw.s);
  buf_free(&text);
  buf_free(&raw);
  return ok;
}

// Seconds since 1970 of an ISO time, like 2026-10-28T14:00:00.000Z, or 0.
static double seconds_of(const char *iso) {
  int y, mo, d, h, mi;
  double s;
  if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  y -= mo <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return (double)(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468) * 86400 + h * 3600 + mi * 60 + s;
}

/* Each message sent to Onsay, a line of home/sent.log, {at, message}, every token in it masked, so the person can read
   what went. Pings are left out. Cut to its newest half when it passes LOG_MOST. */
static pl_gate *logging;
static void sent(const char *m, size_t n) {
  static long size = -1;
  buf line = { 0 };
  buf path = { 0 };
  buf all = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  buf_fmt(&line, "{\"at\":\"%s\",\"message\":", at);
  for (size_t i = 0; i < n; i++)
    if (n - i > 9 && !memcmp(m + i, "\"token\":\"", 9)) {
      buf_str(&line, "\"token\":\"(hidden)\"");
      for (i += 9; i < n && m[i] != '"'; i++)
        if (m[i] == '\\') i++;
    } else
      buf_chr(&line, m[i]);
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
  buf_free(&line);
  buf_free(&path);
  buf_free(&all);
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
  val *headers = v_rec(a);
  val *token = lingo_setting(a, "onsay", "token");
  val *said = NULL;
  rec_set(a, headers, "content-type", v_cstr(a, "application/json"));
  if (token && token->k == V_TEXT && token->t.len) {
    buf bearer = { 0 };
    buf_fmt(&bearer, "Bearer %s", token->t.s);
    rec_set(a, headers, "authorization", v_text(a, bearer.s, bearer.n));
    buf_free(&bearer);
  }
  buf_fmt(&url, "%s%s", ONSAY, path);
  val *got = pl_http(a, method, url.s, headers, body ? body : "", body ? strlen(body) : 0, 30, error);
  val *text = rec_get(got, "text");
  buf_free(&url);
  if (got) {
    *status = rec_get(got, "status")->n;
    said = json_parse(a, text->t.s, text->t.len, NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  }
  return said;
}

////////// 2. CONNECTION //////////

/* A prompt being worked through (5): its session, its steps, a line of {text, depth} each, its own and those Onsay says
   as it decides, the questions it waits on the person for, as JSON, and their answers once they come. held: it waits
   for a device that is off, named in waiting, or for Onsay itself when waiting is empty. */
typedef struct turn {
  struct turn *next;
  const char *session, *prompt;
  buf steps, asks, answers;
  volatile int answered;
  int held;
  char waiting[64];
} turn;
// One message sent and its answer waited for: a lingo(...) or a decision, a decision's with its turn.
typedef struct wait {
  struct wait *next;
  long id;
  volatile int done;
  buf reply;
  turn *turn;
} wait;

static pl_gate *lock;   // keeps everything below, which the connection, the heartbeat and the job thread share
static pl_link *link_;   // the open link, after its hello, or NULL
static int on, ready, ponged;   // on: the lingo runs. ready: Onsay took the hello, and its statement checked out
static int holding = 1;   // a prompt may be held for a device (serve), so the heartbeat asks /resume once a minute
static double heard;   // when Onsay last said anything
static char handle[64], device[64], account[64];   // this computer's handle, its name and its account id, as ready said
static unsigned char mine[32];   // this device's public key
static buf catalog, pending, calls_;   // the catalog now, and decisions and calls not sent yet, a line each
static wait *waits;
static turn *turns_;
// The screens lingos draw on, section 6.
#define MOST_SCREENS 32
static struct {
  long id;
  char session[72], title[128];
  buf drawn;   // its instructions since its last clear, as JSON joined by commas
  char at[128];   // a far one's: the computer that draws it, like desk.brandon, and its number there
  long far;
} screens[MOST_SCREENS];
static struct {
  char at[128];
  long id;
  int heard;
} watching[8];   // the streams of far screens this computer watches through Onsay, one a computer, and whether each
                 // has said its first message, which has every screen it keeps
typedef struct streamed {
  struct streamed *next;
  char id[80], path[256];
} streamed;
static streamed *streams;   // streams Onsay asked of this computer for another of the account, each frame passed on
static long screens_made;
static struct {
  char id[32], session[72];
} calling[16];   // each request called hands in now, its id as JSON, and its turn's session
static void screens_json(buf *b, int k);   // one screen, or every screen, as the stream says them
static int screen_at(double id);
static void screen_gone(int k, buf *said);
static void lines_sent(buf *b);   // each line sent down the link
static void screen_push(buf *said);
static void far_heard(val *id, const char *text, int closed);   // a message of a far computer's stream, or its end
static void far_opened(const char *session, val *shown, buf *gone, buf *opened, buf *watch);
static void far_check(buf *unwatch);   // streams with no screen left to watch, ended
static long ids;

// One step of a turn, {text, depth}, a line of its steps, its newest STEPS kept. Inside the lock.
static void step(turn *t, const char *text, double depth) {
  size_t lines = 0;
  buf_str(&t->steps, "{\"text\":");
  buf_json_str(&t->steps, text, strlen(text));
  buf_str(&t->steps, ",\"depth\":");
  buf_num(&t->steps, depth);
  buf_str(&t->steps, "}\n");
  for (size_t i = 0; i < t->steps.n; i++) lines += t->steps.s[i] == '\n';
  if (lines > STEPS) {
    size_t cut = strcspn(t->steps.s, "\n") + 1;
    memmove(t->steps.s, t->steps.s + cut, t->steps.n - cut);
    t->steps.n -= cut;
  }
}

static int send_(const char *text, size_t n) {
  pl_enter(lock);
  int ok = link_ && tell(link_, text, n);
  pl_leave(lock);
  return ok;
}

/* Lines not sent yet, from q, as messages {type, field: [...]} of up to 1000 each, once Onsay is ready: decisions as
   log entries, calls as calls. Inside the lock. */
static void flush(buf *q, const char *type, const char *field) {
  while (ready && link_ && q->n) {
    buf m = { 0 };
    size_t at = 0;
    buf_fmt(&m, "{\"type\":\"%s\",\"%s\":[", type, field);
    for (int k = 0; at < q->n && k < 1000; k++) {
      size_t end = strcspn(q->s + at, "\n");
      if (k) buf_chr(&m, ',');
      buf_add(&m, q->s + at, end);
      at += end + 1;
    }
    buf_str(&m, "]}");
    int ok = tell(link_, m.s, m.n);
    buf_free(&m);
    if (!ok) return;
    memmove(q->s, q->s + at, q->n - at);
    q->n -= at;
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

// One message of a stream this computer serves another, passed on down the link as a frame of its request.
static void relay(void *arg, const char *text, size_t n) {
  streamed *s = arg;
  buf m = { 0 };
  buf_fmt(&m, "{\"type\":\"frame\",\"id\":%s,\"text\":", s->id);
  buf_json_str(&m, text, n);
  buf_chr(&m, '}');
  send_(m.s, m.n);
  buf_free(&m);
}

/* A stream Onsay asked for answered like any request, its answer the first message, and on a 200 kept open, each
   message after a frame (relay), until Onsay says stop or the link drops. */
static void opened(void *arg, const char *response, size_t n) {
  streamed *s = arg;
  arena *a = arena_new();
  val *status = rec_get(json_parse(a, response, n, NULL), "status");
  answered(NULL, response, n);
  if (status && status->k == V_NUM && status->n == 200) {
    pl_enter(lock);
    s->next = streams;
    streams = s;
    pl_leave(lock);
    lingo_watch(s->path, relay, s);
  } else
    free(s);
  arena_free(a);
}

// The streams this computer serves others that match id, as JSON, or every one when id is NULL, ended.
static void unstreamed(const char *id) {
  streamed *gone = NULL;
  pl_enter(lock);
  for (streamed **at = &streams; *at;)
    if (!id || !strcmp((*at)->id, id)) {
      streamed *s = *at;
      *at = s->next;
      s->next = gone;
      gone = s;
    } else
      at = &(*at)->next;
  pl_leave(lock);
  while (gone) {
    streamed *s = gone;
    gone = s->next;
    lingo_unwatch(s);
    free(s);
  }
}

// A request this lingo hands itself, /onsay/resume and the prompts it runs again: whoever asked follows them in the list.
static void ignored(void *arg, const char *response, size_t n) {}

static void policy_pull(void);

/* A request from Onsay. as: "me" is the account's own call, and as: "onsay" Onsay's own search or agent, both signed over
   {as, at, device, id, method, path, sha256}, sha256 its body's: handed in as that caller when it checks out, made within
   5 minutes for this device, else a 401. Any other is one the proxy passed on, its caller the first address of its
   x-forwarded-for, or its signer, which the core checks. */
static void request(arena *a, val *m, const char *text, size_t n) {
  val *body = rec_get(m, "body");
  val *b64 = rec_get(m, "base64");
  val *signed_ = v_rec(a);
  const char *as = text_or(m, "as", NULL);
  const char *forwarded = text_or(rec_get(m, "headers"), "x-forwarded-for", "");
  if (!as) {
    buf caller = { 0 };
    buf_fmt(&caller, "ip %.*s", (int)strcspn(forwarded, ", "), forwarded);
    lingo_hand(text, n, caller.s, answered, NULL);
    buf_free(&caller);
    return;
  }
  buf bytes = { 0 };
  buf sum64 = { 0 };
  buf out = { 0 };
  unsigned char sum[32];
  if (b64 && truthy(b64))
    unbase64(body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0, &bytes);
  else
    buf_add(&bytes, body && body->k == V_TEXT ? body->t.s : "", body && body->k == V_TEXT ? body->t.len : 0);
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
  double at = seconds_of(text_or(m, "at", NULL));
  double now = pl_now();
  int stream = rec_get(m, "stream") && truthy(rec_get(m, "stream"));
  if ((!strcmp(as, "me") || !strcmp(as, "onsay")) && at > now - FRESH && at < now + FRESH &&
      signed_by(signed_, rec_get(m, "signed"), NULL) && stream) {
    streamed *s = calloc(1, sizeof *s);
    buf j = { 0 };
    const char *p = text_or(m, "path", "/");
    buf_json(&j, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    snprintf(s->id, sizeof s->id, "%s", j.s);
    snprintf(s->path, sizeof s->path, "%.*s", (int)strcspn(p, "?"), p);
    buf_free(&j);
    lingo_hand(text, n, as, opened, s);
  } else if ((!strcmp(as, "me") || !strcmp(as, "onsay")) && at > now - FRESH && at < now + FRESH &&
      signed_by(signed_, rec_get(m, "signed"), NULL))
    lingo_hand(text, n, as, answered, NULL);
  else {
    buf_str(&out, "{\"type\":\"response\",\"id\":");
    buf_json(&out, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    buf_str(&out,
        ",\"status\":401,\"headers\":{\"content-type\":\"application/json\"},\"body\":\"{\\\"error\\\":\\\"Onsay's signature on this "
        "request doesn't check out, so it didn't run\\\"}\",\"base64\":false}");
    send_(out.s, out.n);
    say("A request said to be from Onsay didn't check out, so it didn't run");
  }
  buf_free(&bytes);
  buf_free(&sum64);
  buf_free(&out);
}

// Each message from Onsay, on the connection's thread, so a stop or an answer lands while a request runs.
static void heard_(pl_link *l, const char *text, size_t n) {
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL);
  val *id = rec_get(m, "id");
  const char *type = text_or(m, "type", "");
  if (!strcmp(type, "ready")) {
    val *statement = v_rec(a);
    buf key = { 0 };
    buf_str(&key, "ed25519:");
    base64(&key, mine, 32);
    rec_set(a, statement, "added", rec_get(m, "added") ? rec_get(m, "added") : NOTHING);
    rec_set(a, statement, "device", rec_get(m, "device") ? rec_get(m, "device") : NOTHING);
    rec_set(a, statement, "handle", rec_get(m, "handle") ? rec_get(m, "handle") : NOTHING);
    rec_set(a, statement, "account", rec_get(m, "account") ? rec_get(m, "account") : NOTHING);
    rec_set(a, statement, "key", v_text(a, key.s, key.n));
    buf_free(&key);
    if (!signed_by(statement, rec_get(m, "statement"), NULL)) {
      say("Onsay's statement on this computer's key doesn't check out, so it isn't trusted. Trying again");
      pl_close(l);
    } else {
      pl_enter(lock);
      snprintf(handle, sizeof handle, "%s", text_or(m, "handle", ""));
      snprintf(device, sizeof device, "%s", text_or(m, "device", ""));
      snprintf(account, sizeof account, "%s", text_or(m, "account", ""));
      ready = 1;
      flush(&pending, "log", "entries");
      flush(&calls_, "calls", "calls");
      pl_leave(lock);
      say("Signed in. This computer is %s.%s", device, handle);
      // Always, the default too: the rules in force here are only ever the account's, as Onsay signed them.
      policy_pull();
    }
  } else if (!strcmp(type, "request") && ready)
    request(a, m, text, n);
  else if (!strcmp(type, "stop")) {
    buf j = { 0 };
    buf_json(&j, id ? id : NOTHING);
    unstreamed(j.s);
    lingo_stop(j.s);
    buf_free(&j);
  } else if ((!strcmp(type, "lingo") || !strcmp(type, "decision")) && id && id->k == V_NUM) {
    pl_enter(lock);
    for (wait *w = waits; w; w = w->next)
      if ((double)w->id == id->n) {
        buf_add(&w->reply, text, n);
        lingo_done(&w->done);
      }
    pl_leave(lock);
  } else if (!strcmp(type, "step") && id && id->k == V_NUM) {
    val *depth = rec_get(m, "depth");
    val *shown = rec_get(m, "screen");
    buf gone = { 0 };
    buf made = { 0 };
    buf watch = { 0 };
    pl_enter(lock);
    for (wait *w = waits; w; w = w->next)
      if ((double)w->id == id->n && w->turn) {
        step(w->turn, text_or(m, "text", ""), depth && depth->k == V_NUM ? depth->n : 0);
        if (shown && shown->k == V_REC) far_opened(w->turn->session, shown, &gone, &made, &watch);
      }
    pl_leave(lock);
    screen_push(&gone);
    screen_push(&made);
    if (watch.n) send_(watch.s, watch.n);
    buf_free(&watch);
  } else if (!strcmp(type, "frame") && id && id->k == V_NUM) {
    val *closed = rec_get(m, "closed");
    far_heard(id, text_or(m, "text", ""), closed && truthy(closed));
  } else if (!strcmp(type, "policy"))
    policy_pull();
  else if (!strcmp(type, "pong")) {
    pl_enter(lock);
    ponged = 1;
    pl_leave(lock);
  } else if (strcmp(type, "lingo") && strcmp(type, "decision"))
    say("Onsay sent a message this lingo client doesn't know: %s", *type ? type : "not JSON");
  arena_free(a);
}

// Connects to Onsay while the lingo runs and has a token, and again after a drop, waiting longer each time it fails.
static void connection(void *arg) {
  for (double wait_ = 1;; pl_sleep(wait_), wait_ = wait_ * 2 > 30 ? 30 : wait_ * 2) {
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    buf error = { 0 };
    buf hello = { 0 };
    if (!on || !token || token->k != V_TEXT || !token->t.len) {
      arena_free(a);
      wait_ = 1;
      continue;
    }
    if (!key_of("device", 1, mine)) {
      say("This computer couldn't make or keep its key, so it can't sign in");
      arena_free(a);
      continue;
    }
    buf live = { 0 };
    buf_fmt(&live, "ws%s/live", &ONSAY[strlen("http")]);   // https://x is wss://x/live, http://x ws://x/live
    pl_link *l = pl_connect(live.s, NULL, &error);
    buf_free(&live);
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
      link_ = l;
      heard = pl_now();
      ponged = ready = 0;
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
      link_ = NULL;
      ready = 0;
      for (wait *w = waits; w; w = w->next) lingo_done(&w->done);
      pl_leave(lock);
      pl_free(l);
      unstreamed(NULL);
      for (int i = 0; i < 8; i++) {
        pl_enter(lock);
        long gone = watching[i].id;
        pl_leave(lock);
        if (gone) far_heard(v_num(a, (double)gone), "", 1);
      }
      say("The connection to Onsay closed");
    }
    buf_free(&hello);
    arena_free(a);
  }
}

// Pings every BEAT seconds, drops a link silent for SILENT, and one whose token is gone, signed out elsewhere.
static void heartbeat(void *arg) {
  for (long beats = 1;; beats++) {
    pl_sleep(BEAT);
    arena *a = arena_new();
    val *token = lingo_setting(a, "onsay", "token");
    pl_enter(lock);
    if (link_ && ((ponged && pl_now() - heard > SILENT) || !token || token->k != V_TEXT || !token->t.len))
      pl_close(link_);
    else if (link_) {
      pl_send(link_, "{\"type\":\"ping\"}", 15);
      flush(&calls_, "calls", "calls");
    }
    int resume = ready && holding && beats % 3 == 0;
    pl_leave(lock);
    // Prompts held for a device that is off, run again once it is back (serve, /resume).
    if (resume && token && token->k == V_TEXT) {
      val *key_ = lingo_setting(a, "onsay", "prompt_key");
      buf request = { 0 };
      buf_str(&request, "{\"id\":\"onsay resume\",\"method\":\"POST\",\"path\":\"/onsay/resume\",\"headers\":{\"authorization\":");
      buf_str(&request, "\"Bearer ");
      if (key_ && key_->k == V_TEXT) buf_add(&request, key_->t.s, key_->t.len);
      buf_str(&request, "\"},\"body\":\"\",\"base64\":false}");
      lingo_hand(request.s, request.n, "me", ignored, NULL);
      buf_free(&request);
    }
    arena_free(a);
  }
}

////////// 3. HOOKS //////////

/* A message to Onsay, {"type": type, "id": a new number, ...rest}, rest a JSON record, and its answer by that id waited
   for, answering requests meanwhile. 1 with the whole answer in reply, 0 when Onsay isn't ready, the link closed, stop
   was set or seconds passed. t is a decision's turn, else NULL: it gets the steps Onsay says, and a decision stopped here
   is stopped at Onsay too. */
static int exchange(
    const char *type, const char *rest, size_t n, volatile int *stop, double seconds, buf *reply, turn *t) {
  wait w = { .turn = t };
  buf m = { 0 };
  pl_enter(lock);
  int going = ready && link_;
  if (going) {
    w.id = ++ids;
    w.next = waits;
    waits = &w;
    buf_fmt(&m, "{\"type\":\"%s\",\"id\":%ld,", type, w.id);
    buf_add(&m, rest + 1, n - 1);
    going = tell(link_, m.s, m.n);
  }
  pl_leave(lock);
  if (going) lingo_wait(&w.done, stop, seconds);
  pl_enter(lock);
  for (wait **at = &waits; *at; at = &(*at)->next)
    if (*at == &w) {
      *at = w.next;
      break;
    }
  pl_leave(lock);
  int got = w.reply.n > 0;
  if (got)
    buf_add(reply, w.reply.s, w.reply.n);
  else if (t && *stop) {
    m.n = 0;
    buf_fmt(&m, "{\"type\":\"stop\",\"id\":%ld}", w.id);
    send_(m.s, m.n);
  }
  buf_free(&m);
  buf_free(&w.reply);
  return got;
}

static int ask(const char *values, size_t n, volatile int *stop, buf *answer) {
  return exchange("lingo", values, n, stop, 1e12, answer, NULL);
}

/* A line a request's run says for the person, sent to Onsay as a step of that request when Onsay sent it, so whoever
   asked follows along from any computer, and Onsay knows it is still working. This computer's own requests, from its
   port or its prompts, "port <n>" and "onsay <n>", are shown here instead. */
static int said(const char *for_, const char *text) {
  if (!strncmp(for_, "\"port ", 6) || !strncmp(for_, "\"onsay ", 7)) return 0;
  buf m = { 0 };
  buf_fmt(&m, "{\"type\":\"step\",\"id\":%s,\"text\":", for_);
  buf_json_str(&m, text, strlen(text));
  buf_chr(&m, '}');
  int ok = send_(m.s, m.n);
  buf_free(&m);
  return ok;
}

/* A signer's key, by its keyid, a device and its handle like laptop.brandon: from Onsay's /resolve, its statement checked,
   and kept for 5 minutes. mine when it is a device of this computer's own account. */
static struct {
  char keyid[128], key[64], handle[64], account[64];
  double until;
} known[32];

static val *key(arena *a, const char *keyid) {
  const char *dot = strchr(keyid, '.');
  val *out = NULL;
  int k = 0;
  if (!dot || strchr(dot + 1, '.') || strlen(keyid) >= sizeof known[0].keyid) return NULL;
  pl_enter(lock);
  while (k < 32 && !(!strcmp(known[k].keyid, keyid) && known[k].until > pl_now())) k++;
  pl_leave(lock);
  if (k == 32) {
    buf path = { 0 };
    buf error = { 0 };
    double status = 0;
    buf_fmt(&path, RESOLVE "%s", keyid);
    val *said = api(a, "GET", path.s, NULL, &status, &error);
    val *devices = rec_get(said, "devices");
    for (size_t i = 0; status == 200 && devices && devices->k == V_LIST && i < devices->l.n && k == 32; i++) {
      val *d = devices->l.v[i];
      val *statement = v_rec(a);
      if (strncmp(text_or(d, "name", ""), keyid, (size_t)(dot - keyid)) || text_or(d, "name", "")[dot - keyid])
        continue;
      rec_set(a, statement, "added", rec_get(d, "added") ? rec_get(d, "added") : NOTHING);
      rec_set(a, statement, "device", v_text(a, keyid, (size_t)(dot - keyid)));
      rec_set(a, statement, "handle", v_cstr(a, dot + 1));
      rec_set(a, statement, "account", rec_get(d, "account") ? rec_get(d, "account") : NOTHING);
      rec_set(a, statement, "key", rec_get(d, "key") ? rec_get(d, "key") : NOTHING);
      if (strlen(text_or(d, "key", "")) >= sizeof known[0].key || !signed_by(statement, rec_get(d, "statement"), NULL))
        continue;
      pl_enter(lock);
      for (k = 0; k < 31 && known[k].until > pl_now(); k++) {}
      snprintf(known[k].keyid, sizeof known[k].keyid, "%s", keyid);
      snprintf(known[k].key, sizeof known[k].key, "%s", text_or(d, "key", ""));
      snprintf(known[k].handle, sizeof known[k].handle, "%s", dot + 1);
      snprintf(known[k].account, sizeof known[k].account, "%s", text_or(d, "account", ""));
      known[k].until = pl_now() + FRESH;
      pl_leave(lock);
    }
    buf_free(&path);
    buf_free(&error);
  }
  if (k < 32) {
    out = v_rec(a);
    pl_enter(lock);
    rec_set(a, out, "key", v_cstr(a, known[k].key));
    rec_set(a, out, "handle", v_cstr(a, known[k].handle));
    rec_set(a, out, "account", v_cstr(a, known[k].account));
    rec_set(a, out, "mine", VBOOL(*account && !strcmp(known[k].account, account)));
    pl_leave(lock);
  }
  return out;
}

static void decided(const char *line) {
  pl_enter(lock);
  buf_str(&pending, line);
  buf_chr(&pending, '\n');
  flush(&pending, "log", "entries");
  pl_leave(lock);
}

/* A request this computer made, kept to send Onsay: sent once CHUNK wait, else with the next heartbeat. Offline, the
   newest half is kept once they pass LOG_MOST bytes. */
static void kept(const char *line) {
  size_t lines = 0;
  pl_enter(lock);
  buf_str(&calls_, line);
  buf_chr(&calls_, '\n');
  if (calls_.n > LOG_MOST) {
    size_t cut = calls_.n / 2;
    cut += strcspn(calls_.s + cut, "\n") + 1;
    if (cut > calls_.n) cut = calls_.n;
    memmove(calls_.s, calls_.s + cut, calls_.n - cut);
    calls_.n -= cut;
  }
  for (size_t i = 0; i < calls_.n; i++) lines += calls_.s[i] == '\n';
  if (lines >= CHUNK) flush(&calls_, "calls", "calls");
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
  if (link_ && !on)
    pl_close(link_);
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
    if (!lingo_set("onsay", "prompt_key", v_text(a, made.s, made.n)))
      say("Its prompt key couldn't be kept, so /onsay answers no one");
    buf_free(&made);
  }
  arena_free(a);
  pl_thread(connection, NULL);
  pl_thread(heartbeat, NULL);
}

////////// 4. POLICY //////////

/* The account's policy, from GET /policy: {version, policy, signed}, signed over {policy, version}. Checked, then handed to
   the core, which keeps it unless its version is older than the one kept, and what it applied sent back: the version and
   clamped, which is empty since the ceiling is checked at each phrase. Anything that doesn't check out is refused, and
   said. */
static void policy_pull(void) {
  arena *a = arena_new();
  buf error = { 0 };
  buf m = { 0 };
  double status = 0;
  val *said = api(a, "GET", "/policy", NULL, &status, &error);
  val *both = v_rec(a);
  val *version = rec_get(said, "version");
  val *policy = rec_get(said, "policy");
  const char *why = NULL;
  rec_set(a, both, "policy", policy ? policy : NOTHING);
  rec_set(a, both, "version", version ? version : NOTHING);
  pl_enter(lock);
  const char *name = v_cstr(a, device)->t.s;
  pl_leave(lock);
  if (!said || status != 200)
    say("The account's policy couldn't be fetched: %s", error.n ? error.s : text_or(said, "error", "Onsay said no"));
  else if (!policy || policy->k != V_REC || !version || version->k != V_NUM ||
      !signed_by(both, rec_get(said, "signed"), NULL))
    say("The account's policy from Onsay doesn't check out, so it is refused");
  else {
    rec_set(a, policy, "version", version);
    val *clamped = lingo_policy(a, policy, name, &why);
    if (!clamped)
      say("%s, so it is refused", why);
    else {
      buf_fmt(&m, "{\"type\":\"applied\",\"version\":");
      buf_json(&m, version);
      buf_str(&m, ",\"clamped\":");
      buf_json(&m, clamped);
      buf_chr(&m, '}');
      send_(m.s, m.n);
    }
  }
  buf_free(&error);
  buf_free(&m);
  arena_free(a);
}

/* Sets a rule's identity: who is public, me, my devices or onsay; acct_... is the account; anything else is a handle,
   handle#tag, or a name Onsay resolves, and Onsay keeps the account it names. */
static void identity(arena *a, val *rule, const char *who) {
  if (!strcmp(who, "public") || !strcmp(who, "me") || !strcmp(who, "my devices") || !strcmp(who, "onsay"))
    rec_set(a, rule, "who", v_cstr(a, who));
  else if (!strncmp(who, "acct_", 5))
    rec_set(a, rule, "account", v_cstr(a, who));
  else
    rec_set(a, rule, "handle", v_cstr(a, who));
}

// Whether a rule names who, by its built-in word, its account id, or its handle.
static int names_who(val *rule, const char *who) {
  return !strcmp(text_or(rule, "who", ""), who) || !strcmp(text_or(rule, "account", ""), who) ||
      !strcmp(text_or(rule, "handle", ""), who);
}

/* The account's policy from Onsay, checked and kept to what share and unshare change. Its rules are the ones to save. */
static val *policy_now(run *r, const char *what, buf *error) {
  double status = 0;
  val *said = api(r->a, "GET", "/policy", NULL, &status, error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to %s", what);
  if (status != 200 || !rec_get(said, "policy") || rec_get(said, "policy")->k != V_REC)
    fail(r, "phrase", "Onsay couldn't read the policy: %s", text_or(said, "error", "it said no"));
  return rec_get(said, "policy");
}

// Saves the policy at Onsay and keeps it here.
static val *policy_save(run *r, val *policy) {
  buf body = { 0 };
  buf error = { 0 };
  double status = 0;
  buf_str(&body, "{\"policy\":");
  buf_json(&body, policy);
  buf_chr(&body, '}');
  val *saved = api(r->a, "PUT", "/policy", body.s, &status, &error);
  buf_free(&body);
  buf_free(&error);
  if (!saved) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 200) fail(r, "phrase", "Onsay didn't keep the policy: %s", text_or(saved, "error", "it said no"));
  policy_pull();
  return saved;
}

/* share(values: record) -> the policy: shares with who, at routes, full paths like /photos/share, or every route when
   none are named, on devices when they are. who is public, me, my devices, onsay, a handle like james, handle#tag, or
   acct_.... One more allow rule is added to the account's policy, and every computer is told. */
static val *share(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *policy;
  val *rule;
  val *rules;
  val *old;
  const char *who = values->k == V_REC ? text_or(values, "who", "") : "";
  val *routes = values->k == V_REC ? rec_get(values, "routes") : NULL;
  val *devices = values->k == V_REC ? rec_get(values, "devices") : NULL;
  if (!*who)
    fail(r, "phrase", "Say who to share with: a handle like james, handle#tag, acct_..., public, me, my devices or onsay");
  policy = policy_now(r, "share", &error);
  buf_free(&error);
  rule = v_rec(a);
  rec_set(a, rule, "allow", VBOOL(1));
  identity(a, rule, who);
  if (routes && routes->k == V_LIST) {
    for (size_t i = 0; i < routes->l.n; i++)
      if (routes->l.v[i]->k != V_TEXT || routes->l.v[i]->t.s[0] != '/')
        fail(r, "phrase", "A route is a full path, like /photos/share, each starting with /");
    rec_set(a, rule, "routes", routes);
  }
  if (devices && devices->k == V_LIST) rec_set(a, rule, "devices", devices);
  rules = v_list(a);
  old = rec_get(policy, "rules");
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++) list_push(a, rules, old->l.v[i]);
  list_push(a, rules, rule);
  rec_set(a, policy, "default", v_cstr(a, "closed"));
  rec_set(a, policy, "rules", rules);
  return policy_save(r, policy);
}

/* unshare(values: record) -> the policy: stops sharing with who, taking out every rule that names them, whether by
   public, me, my devices, onsay, an account id, or a handle. */
static val *unshare(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *policy;
  val *kept;
  val *old;
  const char *who = values->k == V_REC ? text_or(values, "who", "") : "";
  if (!*who) fail(r, "phrase", "Say who to stop sharing with: a handle like james, acct_..., public, me, my devices or onsay");
  policy = policy_now(r, "share", &error);
  buf_free(&error);
  kept = v_list(a);
  old = rec_get(policy, "rules");
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++)
    if (!names_who(old->l.v[i], who)) list_push(a, kept, old->l.v[i]);
  if (!old || old->k != V_LIST || kept->l.n == old->l.n)
    fail(r, "phrase", "Nothing is shared with %s", who);
  rec_set(a, policy, "default", v_cstr(a, "closed"));
  rec_set(a, policy, "rules", kept);
  return policy_save(r, policy);
}

////////// 5. /ONSAY //////////

// A core phrase of the store, Onsay's own storage here, called as it is: serve already needs store.
static val *store(run *r, const char *what, val *key, val *value) {
  val *args[2] = { key, value ? value : NOTHING };
  return phrase_find(what)->fn(r, args);
}

/* A session's newest TURNS turns, then prompt, as the whole log in the chat messages OpenAI and OpenRouter take: each
   turn's prompt the user's, {role, content}, then the messages its answer added, an agent's tool calls and their
   results among them, or with none its text, or its questions a line each, the assistant's. */
static val *messages(arena *a, val *turns, val *prompt) {
  val *out = v_list(a);
  for (size_t i = turns->l.n > TURNS ? turns->l.n - TURNS : 0; i <= turns->l.n; i++) {
    val *t = i < turns->l.n ? turns->l.v[i] : NULL;
    val *asked = rec_get(t, "questions");
    val *added = rec_get(t, "messages");
    buf lines = { 0 };
    for (size_t k = 0; asked && asked->k == V_LIST && k < asked->l.n; k++)
      buf_fmt(&lines, "%s%s", k ? "\n" : "",
          asked->l.v[k]->k == V_TEXT ? asked->l.v[k]->t.s : text_or(asked->l.v[k], "question", ""));
    for (int assistant = 0; assistant < (t ? 2 : 1); assistant++) {
      if (assistant && added && added->k == V_LIST) {
        for (size_t k = 0; k < added->l.n; k++) list_push(a, out, added->l.v[k]);
        continue;
      }
      val *m = v_rec(a);
      val *said = prompt;
      if (t && !assistant) said = rec_get(t, "prompt");
      if (t && assistant) said = asked ? v_text(a, lines.s ? lines.s : "", lines.n) : rec_get(t, "text");
      rec_set(a, m, "role", v_cstr(a, assistant ? "assistant" : "user"));
      rec_set(a, m, "content", said ? said : v_cstr(a, ""));
      list_push(a, out, m);
    }
    buf_free(&lines);
  }
  return out;
}

static val *answer(run *r, int status, const char *error) {
  val *out = v_rec(r->a);
  val *body = v_rec(r->a);
  rec_set(r->a, body, "error", v_cstr(r->a, error));
  rec_set(r->a, out, "status", v_num(r->a, status));
  rec_set(r->a, out, "body", body);
  return out;
}

// A turn's steps, kept a line each, as a JSON list, or [] for none.
static void stepped(buf *out, buf *lines) {
  buf_chr(out, '[');
  for (size_t i = 0; lines && i < lines->n; i++)
    if (lines->s[i] != '\n' || i + 1 < lines->n) buf_chr(out, lines->s[i] == '\n' ? ',' : lines->s[i]);
  buf_chr(out, ']');
}

/* The person's answers to questions, each text or {question, choices}, which an app shows from /live and sends in at
   /answers, waited for 2 minutes: their list, or NULL when none came or the prompt was stopped. */
static val *asked(run *r, turn *t, val *questions) {
  val *got = NULL;
  pl_enter(lock);
  t->asks.n = t->answers.n = 0;
  t->answered = 0;
  buf_json(&t->asks, questions);
  pl_leave(lock);
  lingo_wait(&t->answered, r->stop, 120);
  pl_enter(lock);
  if (t->answered) got = json_parse(r->a, t->answers.s, t->answers.n, NULL);
  t->asks.n = 0;
  pl_leave(lock);
  return got && got->k == V_LIST ? got : NULL;
}

// A call handed in to run on this computer, and its answer, the core's. left: nobody waits for it any more.
typedef struct {
  buf response;
  volatile int done, left;
} handed;

static void handed_back(void *arg, const char *response, size_t n) {
  handed *h = arg;
  if (h->left) {
    buf_free(&h->response);
    free(h);
    return;
  }
  buf_add(&h->response, response, n);
  lingo_done(&h->done);
}

/* A call of a turn, as me, the account's own, so the person's own rules apply: a lingo on this computer handed in as a
   request, POST to its path with inputs as its body; anything else through use(...), the way Onsay reaches it, other
   computers' own rules applying there, the steps it says joining the turn t. One on this computer is kept as a call
   (lingo_called), ask the prompt it was for; Onsay keeps the rest. Its value, or NULL with why in why. */
static val *called(run *r, turn *t, const char *address, val *inputs, const char *ask, buf *why) {
  static long count;
  arena *a = r->a;
  buf here = { 0 };
  buf body = { 0 };
  buf request = { 0 };
  buf reply = { 0 };
  val *value = NULL;
  pl_enter(lock);
  buf_fmt(&here, "%s.%s/", device, handle);
  pl_leave(lock);
  buf_json(&body, inputs);
  if (here.n > 3 && !strncmp(address, here.s, here.n)) {
    handed *h = calloc(1, sizeof *h);
    char id[32];
    snprintf(id, sizeof id, "\"onsay %ld\"", ++count);
    buf_fmt(&request, "{\"id\":%s,\"method\":\"POST\",\"path\":", id);
    buf_json_str(&request, address + here.n - 1, strlen(address + here.n - 1));
    buf_str(&request, ",\"headers\":{\"content-type\":\"application/json\"},\"body\":");
    buf_json_str(&request, body.s, body.n);
    buf_str(&request, ",\"base64\":false}");
    val *headers = v_rec(a);
    rec_set(a, headers, "content-type", v_cstr(a, "application/json"));
    double began = pl_now();
    int slot = 0;
    pl_enter(lock);
    while (slot < 15 && calling[slot].id[0]) slot++;
    snprintf(calling[slot].id, sizeof calling[slot].id, "%s", id);
    snprintf(calling[slot].session, sizeof calling[slot].session, "%s", t ? t->session : "");
    pl_leave(lock);
    lingo_hand(request.s, request.n, "me", handed_back, h);
    lingo_wait(&h->done, r->stop, 600);
    pl_enter(lock);
    calling[slot].id[0] = 0;
    pl_leave(lock);
    if (!h->done) {
      h->left = 1;
      lingo_stop(id);
      buf_str(why, *r->stop ? "Stopped by the person" : "It took more than 10 minutes, so it was stopped");
      lingo_called("lingo", address, ask, "POST", address, headers, body.s, body.n, NULL, why->s, *r->stop, (pl_now() - began) * 1000);
    } else {
      val *got = json_parse(a, h->response.s, h->response.n, NULL);
      val *status = rec_get(got, "status");
      val *text = rec_get(got, "body");
      val *said = text && text->k == V_TEXT ? json_parse(a, text->t.s, text->t.len, NULL) : NULL;
      if (status && status->k == V_NUM && status->n >= 200 && status->n < 300) {
        value = NOTHING;
        if (text) value = text;
        if (said) value = said;
      } else
        buf_str(why,
            text_or(said, "error", text && text->k == V_TEXT && text->t.len ? text->t.s : "It answered with an error"));
      val *answer = v_rec(a);
      rec_set(a, answer, "status", status ? status : v_num(a, 0));
      rec_set(a, answer, "headers", rec_get(got, "headers") ? rec_get(got, "headers") : v_rec(a));
      if (text) rec_set(a, answer, "text", text);
      lingo_called("lingo", address, ask, "POST", address, headers, body.s, body.n, answer, why->n ? why->s : NULL, 0, (pl_now() - began) * 1000);
      buf_free(&h->response);
      free(h);
    }
  } else {
    buf_str(&request, "{\"address\":");
    buf_json_str(&request, address, strlen(address));
    buf_str(&request, ",\"values\":");
    buf_add(&request, body.s, body.n);
    buf_chr(&request, '}');
    val *got = exchange("lingo", request.s, request.n, r->stop, 600, &reply, t)
        ? json_parse(a, reply.s, reply.n, NULL)
        : NULL;
    if (got && !rec_get(got, "error"))
      value = rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
    else if (got)
      buf_str(why, text_or(got, "error", ""));
    else
      buf_str(why, *r->stop ? "Stopped by the person" : "This computer isn't connected to Onsay now");
    // A device that is off, or Onsay out of reach from here, holds the turn until it is back (serve).
    pl_enter(lock);
    if (got && !strcmp(text_or(got, "code", ""), "offline")) {
      t->held = 1;
      snprintf(t->waiting, sizeof t->waiting, "%.*s", (int)strcspn(address, "./"), address);
    } else if (!got && !*r->stop && !ready)
      t->held = 1;
    pl_leave(lock);
  }
  buf_free(&here);
  buf_free(&body);
  buf_free(&request);
  buf_free(&reply);
  return value;
}

/* Every request to /onsay, each with its prompt key as Authorization: Bearer <key>.
     POST /onsay {prompt, session, from, mode}   the prompt worked through with the session so far, answered {session,
                                       text, steps}, {session, questions, steps} or {session, waiting, steps}, steps
                                       every step it took. With no session, a new one, following up from, a session
                                       whose whole log, and its own from's, goes before its own. mode: run, the
                                       default, or list, where every pick is the person's
     GET /onsay/sessions               [{session, prompt, from, status, step, waiting, text, questions}], the newest
                                       first, each named by its first prompt: status working, with its newest step,
                                       asking, waiting, with the device it waits for, or done, and its last answer
     POST /onsay/resume                runs again each prompt held for a device that is back, as the heartbeat asks
     GET /onsay/sessions/<id>          {session, turns}: each {prompt, text} or {prompt, questions}, and messages, the
                                       chat messages it added to the log, its calls and what they gave, and its reply
     GET /onsay/sessions/<id>/live     {running, steps, questions}: while a prompt of it runs, its newest steps, each
                                       {text, depth}, and the questions it waits on the person for, or nothing
     POST /onsay/sessions/<id>/answers {answers}   the person's answers to those questions, in order
     GET /onsay/screens                {screens}: every screen open, each {screen, session, title, draw, fresh}, and as a
                                       WebSocket, each change after (section 6). Another computer of the account
                                       watches it as me, through Onsay, with no prompt key
     DELETE /onsay/screens/<screen>    the person closed it
   A prompt is the agent loop, Onsay's lingo function in the AI's seat: the whole log goes to Onsay, which decides the next
   step, a call with its inputs or the reply; the call runs here (called), and it and what it gave join the log, until
   the decision is the reply, at most MOST calls. In list mode each call is first the person's pick of the closest
   ways, or None of these. A call whose answer asks questions
   ends the prompt with them. A call to a device that is off, or Onsay out of reach, holds the prompt: kept as
   onsay/waiting/<id>, {prompt, mode, device, messages}, until /resume runs it again, going on from what it did. A
   session is kept in the store as onsay/sessions/<id>, {turns, from}, its id the time it began and a few random
   letters, or one the app made the same way, so it can follow the prompt as it runs. */
static val *serve(run *r, val **v) {
  arena *a = r->a;
  val *q = v[0];
  val *key_ = lingo_setting(a, "onsay", "prompt_key");
  val *json = rec_get(q, "json");
  val *out = v_rec(a);
  const char *method = text_or(q, "method", "");
  const char *path = text_or(q, "path", "/");
  const char *auth = text_or(rec_get(q, "headers"), "authorization", "");
  int watched = !strcmp(method, "GET") && !strcmp(path, "/screens") && !strcmp(text_or(q, "caller", ""), "me");
  if (!watched && (!key_ || key_->k != V_TEXT || !key_->t.len || strncmp(auth, "Bearer ", 7) || strcmp(auth + 7, key_->t.s)))
    return answer(r, 401, "That isn't this computer's prompt key. Send it as Authorization: Bearer <prompt key>");
  if (!strcmp(method, "GET") && !strcmp(path, "/screens")) {
    buf said = { 0 };
    pl_enter(lock);
    screens_json(&said, -1);
    pl_leave(lock);
    val *got = json_parse(a, said.s, said.n, NULL);
    buf_free(&said);
    return got;
  }
  if (!strcmp(method, "DELETE") && !strncmp(path, "/screens/", 9)) {
    buf said = { 0 };
    pl_enter(lock);
    buf unwatch = { 0 };
    int k = screen_at(atof(path + 9));
    if (k >= 0) screen_gone(k, &said);
    far_check(&unwatch);
    pl_leave(lock);
    screen_push(&said);
    lines_sent(&unwatch);
    return out;
  }
  if (!strcmp(method, "GET") && !strcmp(path, "/sessions")) {
    val *keys = store(r, "store.keys", v_cstr(a, "onsay/sessions/"), NULL);
    val *held = store(r, "store.keys", v_cstr(a, "onsay/waiting/"), NULL);
    val *list = v_list(a);
    for (size_t i = keys->l.n; i-- > 0;) {
      const char *session = keys->l.v[i]->t.s + 15;
      const char *status = "done";
      val *one = v_rec(a);
      val *kept = store(r, "store.get", keys->l.v[i], NULL);
      val *wait_ = NULL;
      val *turns, *last;
      kept = kept->k == V_TEXT ? json_parse(a, kept->t.s, kept->t.len, NULL) : NULL;
      turns = rec_get(kept, "turns");
      last = turns && turns->k == V_LIST && turns->l.n ? turns->l.v[turns->l.n - 1] : NULL;
      for (size_t k = 0; k < held->l.n; k++)
        if (!strcmp(held->l.v[k]->t.s + 14, session)) {
          wait_ = store(r, "store.get", held->l.v[k], NULL);
          wait_ = wait_->k == V_TEXT ? json_parse(a, wait_->t.s, wait_->t.len, NULL) : NULL;
        }
      val *first = last ? rec_get(turns->l.v[0], "prompt") : rec_get(wait_, "prompt");
      rec_set(a, one, "session", v_cstr(a, session));
      rec_set(a, one, "prompt", first ? first : NOTHING);
      if (rec_get(kept, "from")) rec_set(a, one, "from", rec_get(kept, "from"));
      if (wait_) {
        status = "waiting";
        rec_set(a, one, "waiting", rec_get(wait_, "device") ? rec_get(wait_, "device") : v_cstr(a, ""));
      } else if (rec_get(last, "questions"))
        status = "asking";
      buf now = { 0 };
      pl_enter(lock);
      turn *t = turns_;
      while (t && strcmp(t->session, session)) t = t->next;
      if (t) {
        status = t->asks.n ? "asking" : "working";
        if (!last) rec_set(a, one, "prompt", v_cstr(a, t->prompt));
        stepped(&now, &t->steps);
      }
      pl_leave(lock);
      val *steps = now.n ? json_parse(a, now.s, now.n, NULL) : NULL;
      if (steps && steps->k == V_LIST && steps->l.n) rec_set(a, one, "step", rec_get(steps->l.v[steps->l.n - 1], "text"));
      buf_free(&now);
      rec_set(a, one, "status", v_cstr(a, status));
      const char *said = rec_get(last, "questions") ? "questions" : "text";
      if (rec_get(last, said)) rec_set(a, one, said, rec_get(last, said));
      list_push(a, list, one);
    }
    return list;
  }
  if (!strcmp(method, "POST") && !strcmp(path, "/resume")) {
    static long resumes;
    val *held = store(r, "store.keys", v_cstr(a, "onsay/waiting/"), NULL);
    val *said = NULL;
    val *computers;
    double status = 0;
    double ran = 0;
    if (held->l.n) {
      buf error = { 0 };
      said = api(a, "GET", "/lingos", NULL, &status, &error);
      buf_free(&error);
    }
    computers = rec_get(said, "computers");
    for (size_t i = 0; said && i < held->l.n; i++) {
      val *kept = store(r, "store.get", held->l.v[i], NULL);
      kept = kept->k == V_TEXT ? json_parse(a, kept->t.s, kept->t.len, NULL) : NULL;
      const char *device = text_or(kept, "device", "");
      int back = !*device;
      for (size_t k = 0; computers && computers->k == V_LIST && k < computers->l.n; k++)
        if (!strcmp(text_or(computers->l.v[k], "name", ""), device) && truthy(rec_get(computers->l.v[k], "online")))
          back = 1;
      if (!kept || !back) continue;
      val *body = v_rec(a);
      buf request = { 0 };
      buf json_ = { 0 };
      rec_set(a, body, "session", v_cstr(a, held->l.v[i]->t.s + 14));
      rec_set(a, body, "prompt", rec_get(kept, "prompt") ? rec_get(kept, "prompt") : NOTHING);
      rec_set(a, body, "mode", v_cstr(a, text_or(kept, "mode", "run")));
      buf_json(&json_, body);
      buf_fmt(&request, "{\"id\":\"onsay r%ld\",\"method\":\"POST\",\"path\":\"/onsay\",\"headers\":{\"content-type\":\"application/json\",\"authorization\":", ++resumes);
      buf_json_str(&request, auth, strlen(auth));
      buf_str(&request, "},\"body\":");
      buf_json_str(&request, json_.s, json_.n);
      buf_str(&request, ",\"base64\":false}");
      lingo_hand(request.s, request.n, "me", ignored, NULL);
      buf_free(&request);
      buf_free(&json_);
      ran++;
    }
    pl_enter(lock);
    holding = ran < held->l.n;
    pl_leave(lock);
    rec_set(a, out, "resumed", v_num(a, ran));
    return out;
  }
  const char *sub = !strncmp(path, "/sessions/", 10) ? strchr(path + 10, '/') : NULL;
  if (sub) {
    const char *session = v_text(a, path + 10, (size_t)(sub - path - 10))->t.s;
    int live = !strcmp(method, "GET") && !strcmp(sub, "/live");
    int answers = !strcmp(method, "POST") && !strcmp(sub, "/answers");
    int waiting = 0;
    val *given = rec_get(json, "answers");
    if (!live && !answers)
      return answer(
          r, 404, "A session takes GET /onsay/sessions/<session>/live and POST /onsay/sessions/<session>/answers");
    if (answers && (!given || given->k != V_LIST))
      return answer(r, 400, "Send the answers as {\"answers\": [\"...\"]}, one for each question, in order");
    buf said = { 0 };
    pl_enter(lock);
    turn *t = turns_;
    while (t && strcmp(t->session, session)) t = t->next;
    if (live) {
      buf_str(&said, "{\"running\":");
      buf_str(&said, t ? "true" : "false");
      buf_str(&said, ",\"steps\":");
      stepped(&said, t ? &t->steps : NULL);
      buf_str(&said, ",\"questions\":");
      if (t && t->asks.n)
        buf_add(&said, t->asks.s, t->asks.n);
      else
        buf_str(&said, "null");
      buf_chr(&said, '}');
    } else if ((waiting = t && t->asks.n)) {
      t->asks.n = t->answers.n = 0;
      buf_json(&t->answers, given);
      lingo_done(&t->answered);
    }
    pl_leave(lock);
    val *got;
    if (live)
      got = json_parse(a, said.s, said.n, NULL);
    else if (waiting)
      got = v_rec(a);
    else
      got = answer(r, 404, "Nothing in that session is waiting for an answer");
    buf_free(&said);
    return got;
  }
  const char *id = NULL;
  if (!strcmp(method, "GET") && !strncmp(path, "/sessions/", 10)) id = path + 10;
  if (!strcmp(method, "POST") && !strcmp(path, "/")) id = text_or(json, "session", "");
  if (!id)
    return answer(r, 404,
        "/onsay takes POST /onsay {prompt, session, from, mode}, GET /onsay/sessions, POST /onsay/resume, GET /onsay/sessions/<session>, and its /live and /answers");
  if (strlen(id) > 64 || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(id))
    return answer(r, 400, "A session is lower case letters, digits and -");
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
  val *where = v_text(a, whole.s, whole.n);
  val *kept;
  val *turns;
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
  val *prompt = rec_get(json, "prompt");
  val *from = kept ? rec_get(kept, "from") : rec_get(json, "from");
  if (from && from->k != V_TEXT && !truthy(from)) from = NULL;
  val *log;
  val *added = v_list(a);
  val *done = v_rec(a);
  val *record = v_rec(a);
  val *asks = NULL;
  val *before = v_list(a);
  val *held;
  if (!prompt || prompt->k != V_TEXT || !prompt->t.len)
    return answer(r, 400, "Send the prompt as {\"prompt\": \"...\"}");
  if (from && (from->k != V_TEXT || !from->t.len || from->t.len > 64 ||
                  strspn(from->t.s, "abcdefghijklmnopqrstuvwxyz0123456789-") != from->t.len))
    return answer(r, 400, "from is a session: lower case letters, digits and -");
  pl_enter(lock);
  turn *running_ = turns_;
  while (running_ && strcmp(running_->session, id)) running_ = running_->next;
  pl_leave(lock);
  if (running_) return answer(r, 409, "That one is still working. Wait for it, or stop it first");
  // The same prompt again, as one held for a device runs again, goes on in place of the turn a stop left of it.
  val *cut = turns->l.n ? turns->l.v[turns->l.n - 1] : NULL;
  val *stopped = rec_get(cut, "stopped");
  if (stopped && truthy(stopped) && v_same(rec_get(cut, "prompt"), prompt)) turns->l.n--;
  rec_set(a, record, "turns", turns);
  if (from) rec_set(a, record, "from", from);
  // Kept with its prompt from the start, so a stop or a crash that ends the lingo client partway never loses what was
  // asked: until the answer takes its place, the turn says it stopped.
  val *meanwhile = v_rec(a);
  val *so_far = v_list(a);
  val *unfinished = v_rec(a);
  for (size_t i = 0; i < turns->l.n; i++) list_push(a, so_far, turns->l.v[i]);
  rec_set(a, unfinished, "prompt", prompt);
  rec_set(a, unfinished, "text", v_cstr(a, "Stopped before it finished."));
  rec_set(a, unfinished, "stopped", VBOOL(1));
  list_push(a, so_far, unfinished);
  rec_set(a, meanwhile, "turns", so_far);
  if (from) rec_set(a, meanwhile, "from", from);
  buf_json(&whole, meanwhile);
  store(r, "store.put", where, v_text(a, whole.s, whole.n));
  buf_free(&whole);
  // A prompt held for a device goes on from what it did, unless the person asked something else since.
  buf_fmt(&whole, "onsay/waiting/%s", id);
  val *waiting_at = v_text(a, whole.s, whole.n);
  buf_free(&whole);
  held = store(r, "store.get", waiting_at, NULL);
  if (held->k == V_TEXT) {
    store(r, "store.delete", waiting_at, NULL);
    held = json_parse(a, held->t.s, held->t.len, NULL);
    if (!v_same(rec_get(held, "prompt"), prompt)) held = NULL;
  } else
    held = NULL;
  // A follow up carries every session it follows, oldest first, as if they were one.
  for (int hops = 0; from && from->k == V_TEXT && hops < 20; hops++) {
    buf_fmt(&whole, "onsay/sessions/%s", from->t.s);
    val *up = store(r, "store.get", v_text(a, whole.s, whole.n), NULL);
    buf_free(&whole);
    up = up->k == V_TEXT ? json_parse(a, up->t.s, up->t.len, NULL) : NULL;
    val *its = rec_get(up, "turns");
    val *joined = v_list(a);
    for (size_t i = 0; its && its->k == V_LIST && i < its->l.n; i++) list_push(a, joined, its->l.v[i]);
    for (size_t i = 0; i < before->l.n; i++) list_push(a, joined, before->l.v[i]);
    before = joined;
    from = rec_get(up, "from");
  }
  for (size_t i = 0; i < turns->l.n; i++) list_push(a, before, turns->l.v[i]);
  const char *reply = NULL;
  const char *unanswered = "Nobody answered in 2 minutes, so I stopped. Ask again when you're ready.";
  int choosing = !strcmp(text_or(json, "mode", ""), "list");
  int reached = 1;
  buf m = { 0 };
  turn t = { .session = id, .prompt = prompt->t.s };
  pl_enter(lock);
  t.next = turns_;
  turns_ = &t;
  pl_leave(lock);
  log = messages(a, before, prompt);
  val *did = rec_get(held, "messages");
  for (size_t i = 0; did && did->k == V_LIST && i < did->l.n; i++) {
    list_push(a, added, did->l.v[i]);
    list_push(a, log, did->l.v[i]);
  }
  for (int k = 1; !reply && !asks && !*r->stop; k++) {
    buf why = { 0 };
    buf said = { 0 };
    buf called_ = { 0 };
    val *d, *inputs, *value, *choices, *given;
    if (k > MOST) {
      reply = "That needed more steps than I take at once, so I stopped here. Ask again to go on.";
      break;
    }
    buf_str(&m, "{\"messages\":");
    buf_json(&m, log);
    buf_str(&m, choosing ? ",\"mode\":\"list\"}" : "}");
    d = exchange("decide", m.s, m.n, r->stop, 150, &said, &t) ? json_parse(a, said.s, said.n, NULL) : NULL;
    m.n = said.n = 0;
    if (*r->stop) {
      buf_free(&said);
      break;
    }
    if (!d || d->k != V_REC) {
      reached = 0;
      buf_free(&said);
      break;
    }
    if (rec_get(d, "reply")) {
      reply = text_or(d, "reply", "");
      buf_free(&said);
      break;
    }
    // Let me choose: the closest ways as one card, and the one the person picks runs.
    choices = rec_get(d, "choices");
    if (choices && choices->k == V_LIST && choices->l.n) {
      val *card = v_rec(a);
      val *ways = v_list(a);
      val *questions = v_list(a);
      val *picked;
      size_t n = 0;
      for (size_t i = 0; i < choices->l.n; i++, m.n = 0) {
        buf_fmt(&m, "%s, %s", text_or(choices->l.v[i], "does", ""), text_or(choices->l.v[i], "where", ""));
        list_push(a, ways, v_text(a, m.s, m.n));
      }
      list_push(a, ways, v_cstr(a, "None of these"));
      rec_set(a, card, "question", v_cstr(a, "Which should I do?"));
      rec_set(a, card, "choices", ways);
      rec_set(a, card, "other", VBOOL(0));
      rec_set(a, card, "skip", VBOOL(0));
      list_push(a, questions, card);
      pl_enter(lock);
      step(&t, "Waiting for you to choose", 0);
      pl_leave(lock);
      picked = asked(r, &t, questions);
      while (picked && picked->l.n && n < choices->l.n && !v_same(ways->l.v[n], picked->l.v[0])) n++;
      if (*r->stop || !picked || n == choices->l.n) {
        reply = "Okay, I didn't do any of those. Say it another way and I'll look again.";
        if (!picked) reply = unanswered;
        if (*r->stop) reply = NULL;
        buf_free(&said);
        break;
      }
      d = choices->l.v[n];
    }
    inputs = rec_get(d, "inputs");
    if (!inputs || (inputs->k != V_REC && inputs->k != V_LIST)) inputs = v_rec(a);
    const char *address = text_or(d, "call", "");
    value = rec_get(d, "value");
    pl_enter(lock);
    buf_fmt(&m, "Picked: %s", text_or(d, "does", address));
    step(&t, m.s, 0);
    m.n = 0;
    if (!value) {
      buf_fmt(&m, "Running it %s", text_or(d, "where", ""));
      step(&t, m.s, 0);
      m.n = 0;
    }
    pl_leave(lock);
    if (!value) value = called(r, &t, address, inputs, prompt->t.s, &why);
    if (t.held) {
      buf_free(&why);
      buf_free(&said);
      buf_free(&called_);
      break;
    }
    // The call and what it gave join the log, its values but the log itself, which a call that reads it was given.
    given = inputs;
    if (inputs->k == V_REC) {
      given = v_rec(a);
      for (size_t i = 0; i < inputs->r.n; i++)
        if (strcmp(inputs->r.key[i], "messages")) rec_set(a, given, inputs->r.key[i], inputs->r.v[i]);
    }
    if (!value)
      buf_fmt(&said, "It failed: %s", why.s ? why.s : "");
    else if (value->k == V_TEXT)
      buf_add(&said, value->t.s, value->t.len);
    else if (rec_get(value, "text") && rec_get(value, "text")->k == V_TEXT)
      buf_text(&said, rec_get(value, "text"));
    else
      buf_json(&said, value);
    // What an agent did joins the log in short, so whatever goes on next goes on from there.
    if (value && rec_get(value, "work") && rec_get(value, "work")->k == V_TEXT)
      buf_fmt(&said, "\n\n(What it did, newest first, to go on from and never to repeat to the person:\n%s)", rec_get(value, "work")->t.s);
    if (value && rec_get(value, "questions") && rec_get(value, "questions")->k == V_LIST)
      asks = rec_get(value, "questions");
    buf_str(&called_, "{\"address\":");
    buf_json_str(&called_, address, strlen(address));
    buf_str(&called_, ",\"values\":");
    buf_json(&called_, given);
    buf_chr(&called_, '}');
    buf_fmt(&m,
        "[{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"call_%zu_%d\",\"type\":\"function\",\"function\":{\"name\":\"use\",\"arguments\":",
        turns->l.n + 1, k);
    buf_json_str(&m, called_.s, called_.n);
    buf_fmt(&m, "}}]},{\"role\":\"tool\",\"tool_call_id\":\"call_%zu_%d\",\"content\":", turns->l.n + 1, k);
    buf_json_str(&m, said.s ? said.s : "", said.n);
    buf_str(&m, "}]");
    val *both = json_parse(a, m.s, m.n, NULL);
    for (size_t i = 0; i < 2; i++) {
      list_push(a, added, both->l.v[i]);
      list_push(a, log, both->l.v[i]);
    }
    m.n = 0;
    buf_free(&why);
    buf_free(&said);
    buf_free(&called_);
  }
  pl_enter(lock);
  for (turn **at = &turns_; *at; at = &(*at)->next)
    if (*at == &t) {
      *at = t.next;
      break;
    }
  stepped(&m, &t.steps);
  pl_leave(lock);
  rec_set(a, out, "steps", json_parse(a, m.s, m.n, NULL));
  buf_free(&t.steps);
  buf_free(&t.asks);
  buf_free(&t.answers);
  m.n = 0;
  pl_enter(lock);
  if (!reached && !*r->stop && !ready) t.held = 1;
  pl_leave(lock);
  if (t.held) {
    val *wait_ = v_rec(a);
    rec_set(a, wait_, "prompt", prompt);
    rec_set(a, wait_, "mode", v_cstr(a, choosing ? "list" : "run"));
    rec_set(a, wait_, "device", v_cstr(a, t.waiting));
    rec_set(a, wait_, "messages", added);
    buf_json(&m, wait_);
    store(r, "store.put", waiting_at, v_text(a, m.s, m.n));
    m.n = 0;
    buf_json(&m, record);   // waiting, not stopped
    store(r, "store.put", where, v_text(a, m.s, m.n));
    pl_enter(lock);
    holding = 1;
    pl_leave(lock);
    buf_free(&m);
    rec_set(a, out, "waiting", v_cstr(a, t.waiting));
    return out;
  }
  if (!reached && !added->l.n) {
    buf_free(&m);
    return answer(r, 503, "Onsay didn't answer, so this couldn't run. Try again in a moment");
  }
  if (!reached) reply = "Onsay stopped answering partway through, so I stopped there. Ask again in a moment.";
  if (*r->stop) reply = "Okay, I stopped.";
  // Its reply, or its questions a line each, is Onsay's last word in the log.
  for (size_t i = 0; asks && i < asks->l.n; i++)
    buf_fmt(&m, "%s%s", i ? "\n" : "",
        asks->l.v[i]->k == V_TEXT ? asks->l.v[i]->t.s : text_or(asks->l.v[i], "question", ""));
  val *last = v_rec(a);
  rec_set(a, last, "role", v_cstr(a, "assistant"));
  rec_set(a, last, "content", asks ? v_text(a, m.s ? m.s : "", m.n) : v_cstr(a, reply));
  list_push(a, added, last);
  rec_set(a, done, "prompt", prompt);
  rec_set(a, done, asks ? "questions" : "text", asks ? asks : v_cstr(a, reply));
  rec_set(a, done, "messages", added);
  list_push(a, turns, done);
  m.n = 0;
  buf_json(&m, record);
  store(r, "store.put", where, v_text(a, m.s, m.n));
  buf_free(&m);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  rec_set(a, out, asks ? "questions" : "text", rec_get(done, asks ? "questions" : "text"));
  return out;
}

////////// 6. SCREENS //////////

/* The screens lingos draw on, the newest MOST_SCREENS: each with the session of the to-do whose work opened it, found
   from the request it runs in (calling), its title, and what was drawn on it since its last clear, instructions of
   Canvas 2D as JSON (DRAWS). Anything watching /onsay/screens on the port as a WebSocket is given every screen,
   {screens}, each {screen, session, title, draw, fresh: true}, then each batch as it is drawn, {screens: [one]}, fresh
   when it starts with a clear, and a closed one as {screen, closed: true} (lingo_push). Each device draws it itself. */
#define MOST_DRAWN (16 << 20)   // the most kept for a screen since its last clear, as JSON

// Each instruction is a record with one of these keys: its values a list, or a value for the ones that set how to draw.
static const char *DRAWS = " clear save restore translate scale rotate beginPath closePath moveTo lineTo bezierCurveTo "
                           "quadraticCurveTo arc rect roundRect fill stroke clip fillRect strokeRect clearRect fillText "
                           "strokeText drawImage fillStyle strokeStyle lineWidth font textAlign textBaseline globalAlpha ";

// One screen as the stream says it: what is drawn, items JSON joined by commas, fresh when it starts over. Under lock.
static void screen_json(buf *b, int k, const char *items, size_t n, int fresh) {
  buf_fmt(b, "{\"screen\":%ld,\"session\":", screens[k].id);
  buf_json_str(b, screens[k].session, strlen(screens[k].session));
  buf_str(b, ",\"title\":");
  buf_json_str(b, screens[k].title, strlen(screens[k].title));
  buf_str(b, ",\"draw\":[");
  buf_add(b, items, n);
  buf_fmt(b, "],\"fresh\":%s}", fresh ? "true" : "false");
}

// One screen whole, or every screen when k is -1, as the stream begins. Under lock.
static void screens_json(buf *b, int k) {
  buf_str(b, "{\"screens\":[");
  for (int i = k < 0 ? 0 : k, first = 1; i < (k < 0 ? MOST_SCREENS : k + 1); i++) {
    if (!screens[i].id) continue;
    if (!first) buf_chr(b, ',');
    first = 0;
    screen_json(b, i, screens[i].drawn.s ? screens[i].drawn.s : "", screens[i].drawn.n, 1);
  }
  buf_str(b, "]}");
}

/* A batch drawn on a screen, items JSON joined by commas: in place of what was there when fresh, else after it, and a
   line telling everything watching, into said. 0 when it would keep more than MOST_DRAWN. Under lock. */
static int screen_took(int k, const char *items, size_t n, int fresh, buf *said) {
  if ((fresh ? 0 : screens[k].drawn.n) + n > MOST_DRAWN) return 0;
  if (fresh) screens[k].drawn.n = 0;
  if (screens[k].drawn.n && n) buf_chr(&screens[k].drawn, ',');
  buf_add(&screens[k].drawn, items, n);
  buf_str(said, "{\"screens\":[");
  screen_json(said, k, items, n, fresh);
  buf_str(said, "]}\n");
  return 1;
}

// A screen taken away, by its place, told to everything watching. Under lock; pushed once it is let go, into said.
static void screen_gone(int k, buf *said) {
  buf_fmt(said, "{\"screens\":[{\"screen\":%ld,\"closed\":true}]}\n", screens[k].id);
  buf_free(&screens[k].drawn);
  memset(&screens[k], 0, sizeof screens[k]);
}

// The place of an open screen, by its number, or -1. Under lock.
static int screen_at(double id) {
  for (int k = 0; k < MOST_SCREENS; k++)
    if (screens[k].id && screens[k].id == (long)id) return k;
  return -1;
}

// Each line of said pushed to everything watching /onsay/screens. Never under lock: a push may go down the link.
static void screen_push(buf *said) {
  for (size_t at = 0, end; at < said->n; at = end + 1) {
    for (end = at; end < said->n && said->s[end] != '\n'; end++) {}
    if (end > at) lingo_push("/onsay/screens", said->s + at, end - at);
  }
  buf_free(said);
}

// Each line of b sent down the link to Onsay. Never under lock.
static void lines_sent(buf *b) {
  for (size_t at = 0, end; at < b->n; at = end + 1) {
    for (end = at; end < b->n && b->s[end] != '\n'; end++) {}
    if (end > at) send_(b->s + at, end - at);
  }
  buf_free(b);
}

/* A list of instructions as JSON joined by commas, from its last clear, which makes it fresh, into items. Fails saying
   which isn't one: a record with one key of DRAWS, and a drawImage only of a picture inside it, a data: address. */
static int drawing(run *r, val *list, buf *items) {
  size_t from = 0;
  if (list->k != V_LIST) fail(r, "phrase", "Draw a list of instructions, like [{\"fillRect\": [0, 0, 10, 10]}]");
  for (size_t i = 0; i < list->l.n; i++) {
    val *x = list->l.v[i];
    char key[40];
    snprintf(key, sizeof key, " %s ", x->k == V_REC && x->r.n == 1 ? x->r.key[0] : "");
    if (x->k != V_REC || x->r.n != 1 || strlen(key) < 3 || !strstr(DRAWS, key))
      fail(r, "phrase", "Instruction %zu isn't one: each is a record with one key, like {\"fillRect\": [0, 0, 10, 10]}, the "
           "key one of%s", i + 1, DRAWS);
    val *src = x->r.v[0]->k == V_LIST && x->r.v[0]->l.n ? x->r.v[0]->l.v[0] : NULL;
    if (!strcmp(key, " drawImage ") && (!src || src->k != V_TEXT || strncmp(src->t.s, "data:image/", 11)))
      fail(r, "phrase", "drawImage takes a picture inside it, a data:image/ address, never one on the web");
    if (!strcmp(key, " clear ")) from = i;
  }
  for (size_t i = from; i < list->l.n; i++) {
    if (i > from) buf_chr(items, ',');
    buf_json(items, list->l.v[i]);
  }
  return list->l.n && from < list->l.n && list->l.v[from]->k == V_REC && !strcmp(list->l.v[from]->r.key[0], "clear");
}

// A batch drawn on the screen numbered id, for screen.draw and screen.show. Fails when it isn't drawn here.
static void drawn_on(run *r, double id, buf *items, int fresh) {
  buf said = { 0 };
  char far[128] = "";
  pl_enter(lock);
  int k = screen_at(id);
  int took = k >= 0 && !screens[k].at[0] && screen_took(k, items->s ? items->s : "", items->n, fresh, &said);
  if (k >= 0) snprintf(far, sizeof far, "%s", screens[k].at);
  pl_leave(lock);
  buf_free(items);
  if (k < 0) fail(r, "phrase", "There's no screen %.0f open: it was closed, or never opened", id);
  if (*far) fail(r, "phrase", "Screen %.0f is drawn on %s, so it can only be drawn on there", id, far);
  if (!took) fail(r, "phrase", "That is more than 16 MB drawn since the screen's last clear. Start again with a clear");
  screen_push(&said);
}

/* screen.open(title): the session is the one of the turn whose call this request is, none for anything else. One of
   that title open for the same session is given back, so a lingo called again and again in one to-do draws on one.
   Opened for a request another computer asked for, through Onsay, it tells that computer, as a step with the screen,
   so it shows it as its own (far_opened). */
static val *screen_open(run *r, val **a) {
  const char *job = lingo_job();
  const char *title = text_of(r, a[0])->t.s;
  char session[72] = "";
  buf said = { 0 };
  int k = 0;
  pl_enter(lock);
  for (int i = 0; job && i < 16; i++)
    if (!strcmp(calling[i].id, job)) snprintf(session, sizeof session, "%s", calling[i].session);
  for (int i = 0; i < MOST_SCREENS; i++)
    if (screens[i].id && !strcmp(screens[i].session, session) && !strncmp(screens[i].title, title, sizeof screens[i].title - 1)) {
      long id = screens[i].id;
      pl_leave(lock);
      return v_num(r->a, (double)id);
    }
  for (int i = 0; i < MOST_SCREENS; i++)
    if (!screens[i].id || (screens[k].id && screens[i].id < screens[k].id)) k = i;
  if (screens[k].id) screen_gone(k, &said);
  screens[k].id = ++screens_made;
  snprintf(screens[k].title, sizeof screens[k].title, "%s", title);
  snprintf(screens[k].session, sizeof screens[k].session, "%s", session);
  long id = screens[k].id;
  buf told = { 0 };
  if (job && strncmp(job, "\"port ", 6) && strncmp(job, "\"onsay ", 7)) {
    buf_fmt(&told, "{\"type\":\"step\",\"id\":%s,\"text\":", job);
    buf_str(&told, "\"Showing it on a screen\",\"screen\":{\"at\":");
    char here[160];
    snprintf(here, sizeof here, "%s.%s", device, handle);
    buf_json_str(&told, here, strlen(here));
    buf_fmt(&told, ",\"screen\":%ld,\"title\":", id);
    buf_json_str(&told, title, strlen(title));
    buf_str(&told, "}}");
  }
  screens_json(&said, k);
  pl_leave(lock);
  screen_push(&said);
  if (told.n) send_(told.s, told.n);
  buf_free(&told);
  return v_num(r->a, (double)id);
}

// screen.draw(screen, instructions)
static val *screen_draw(run *r, val **a) {
  buf items = { 0 };
  int fresh = drawing(r, a[1], &items);
  drawn_on(r, a[0]->n, &items, fresh);
  return NOTHING;
}

/* screen.show(screen, picture): a clear the picture's size and the picture drawn whole, its size read from its own
   first bytes, a PNG's header or a JPEG's frame. A data: address is taken as it is. */
static val *screen_show(run *r, val **a) {
  const char *p = text_of(r, a[1])->t.s;
  const char *comma = strchr(p, ',');
  buf bytes = { 0 };
  buf items = { 0 };
  long w = 0;
  long h = 0;
  if (!strncmp(p, "data:", 5) && comma) p = comma + 1;
  int png = !strncmp(p, "iVBORw0KGgo", 11);
  if (strlen(p) > (12 << 20)) fail(r, "phrase", "That picture is more than 12 MB as base64, too big for a screen");
  if ((png || !strncmp(p, "/9j/", 4)) && unbase64(p, strlen(p), &bytes)) {
    const unsigned char *b = (const unsigned char *)bytes.s;
    if (png && bytes.n > 24) {
      w = (long)b[16] << 24 | (long)b[17] << 16 | (long)b[18] << 8 | b[19];
      h = (long)b[20] << 24 | (long)b[21] << 16 | (long)b[22] << 8 | b[23];
    }
    for (size_t i = 2; !png && i + 9 < bytes.n && b[i] == 0xFF; i += 2 + (size_t)(b[i + 2] << 8 | b[i + 3]))
      if (b[i + 1] >= 0xC0 && b[i + 1] <= 0xCF && b[i + 1] != 0xC4 && b[i + 1] != 0xC8 && b[i + 1] != 0xCC) {
        h = b[i + 5] << 8 | b[i + 6];
        w = b[i + 7] << 8 | b[i + 8];
        break;
      }
  }
  buf_free(&bytes);
  if (w <= 0 || h <= 0) fail(r, "phrase", "A screen shows a PNG or JPEG picture, as base64");
  buf_fmt(&items, "{\"clear\":[%ld,%ld]},{\"drawImage\":[\"data:image/%s;base64,", w, h, png ? "png" : "jpeg");
  buf_str(&items, p);
  buf_fmt(&items, "\",0,0,%ld,%ld]}", w, h);
  drawn_on(r, a[0]->n, &items, 1);
  return NOTHING;
}

// screen.close(screen), and the person closing one (serve).
static val *screen_close(run *r, val **a) {
  buf said = { 0 };
  pl_enter(lock);
  int k = screen_at(a[0]->n);
  if (k >= 0) screen_gone(k, &said);
  pl_leave(lock);
  screen_push(&said);
  return NOTHING;
}

/* A far screen: one another computer drew for a to-do of this one, which said so in a step. It is kept here like any
   other, with where it is drawn, and this computer watches that computer's /onsay/screens through Onsay, one stream a
   computer, to draw each batch as it comes. Under lock: what changes goes into gone and opened, to push, and the
   stream to open into watch, to send. */
static void far_opened(const char *session, val *shown, buf *gone, buf *opened, buf *watch) {
  const char *at = text_or(shown, "at", "");
  val *far = rec_get(shown, "screen");
  int k = 0;
  int w = 0;
  if (!*at || strlen(at) >= sizeof watching[0].at || !far || far->k != V_NUM) return;
  // Opened on this computer for one of its own to-dos, through Onsay, as a use(...) does: the screen here is that
  // to-do's, never a copy of it watched back through Onsay.
  char here[160];
  snprintf(here, sizeof here, "%s.%s", device, handle);
  if (!strcmp(at, here)) {
    int mine = screen_at(far->n);
    if (mine < 0 || screens[mine].session[0]) return;
    snprintf(screens[mine].session, sizeof screens[mine].session, "%s", session);
    screens_json(opened, mine);
    buf_chr(opened, '\n');
    return;
  }
  for (int i = 0; i < MOST_SCREENS; i++)
    if (screens[i].id && !strcmp(screens[i].at, at) && screens[i].far == (long)far->n) return;
  for (int i = 0; i < MOST_SCREENS; i++)
    if (!screens[i].id || (screens[k].id && screens[i].id < screens[k].id)) k = i;
  if (screens[k].id) screen_gone(k, gone);
  screens[k].id = ++screens_made;
  screens[k].far = (long)far->n;
  snprintf(screens[k].session, sizeof screens[k].session, "%s", session);
  snprintf(screens[k].title, sizeof screens[k].title, "%s", text_or(shown, "title", ""));
  snprintf(screens[k].at, sizeof screens[k].at, "%s", at);
  screens_json(opened, k);
  buf_chr(opened, '\n');
  while (w < 8 && strcmp(watching[w].at, at)) w++;
  if (w < 8) return;
  for (w = 0; w < 8 && watching[w].id; w++) {}
  if (w == 8) return;
  snprintf(watching[w].at, sizeof watching[w].at, "%s", at);
  watching[w].id = ++ids;
  watching[w].heard = 0;
  buf_fmt(watch, "{\"type\":\"watch\",\"id\":%ld,\"to\":", watching[w].id);
  buf_json_str(watch, at, strlen(at));
  buf_str(watch, ",\"path\":\"/onsay/screens\"}");
}

/* A message of a far computer's stream: each screen it names that is shown here takes its title and what it drew, or
   goes when it closed. Its first message has every screen it keeps, so one shown here and not in it closed meanwhile.
   A stream that ended takes every screen it drew with it. */
static void far_heard(val *id, const char *text, int closed) {
  arena *a = arena_new();
  val *list = closed ? NULL : rec_get(json_parse(a, text, strlen(text), NULL), "screens");
  buf said = { 0 };
  buf unwatch = { 0 };
  int w = 0;
  pl_enter(lock);
  while (w < 8 && !(watching[w].id && watching[w].id == (long)id->n)) w++;
  for (int i = 0; w < 8 && i < MOST_SCREENS; i++) {
    if (!screens[i].id || strcmp(screens[i].at, watching[w].at)) continue;
    val *mine = NULL;
    for (size_t j = 0; list && list->k == V_LIST && j < list->l.n && !mine; j++) {
      val *n = rec_get(list->l.v[j], "screen");
      if (n && n->k == V_NUM && (long)n->n == screens[i].far) mine = list->l.v[j];
    }
    val *draw = rec_get(mine, "draw");
    buf items = { 0 };
    if (draw && draw->k == V_LIST) {
      buf_json(&items, draw);
      items.n = items.n >= 2 ? items.n - 1 : 0;   // without its brackets
    }
    if (closed || (!mine && !watching[w].heard) || (mine && rec_get(mine, "closed")))
      screen_gone(i, &said);
    else if (mine) {
      snprintf(screens[i].title, sizeof screens[i].title, "%s", text_or(mine, "title", screens[i].title));
      if (!screen_took(i, items.n ? items.s + 1 : "", items.n ? items.n - 1 : 0, truthy(rec_get(mine, "fresh") ? rec_get(mine, "fresh") : NOTHING), &said))
        screen_gone(i, &said);
    }
    buf_free(&items);
  }
  if (w < 8) watching[w].heard = 1;
  if (w < 8 && closed) memset(&watching[w], 0, sizeof watching[w]);
  far_check(&unwatch);
  pl_leave(lock);
  screen_push(&said);
  lines_sent(&unwatch);
  arena_free(a);
}

// Under lock: each stream with no screen of its computer left here, ended, its message to Onsay a line of unwatch.
static void far_check(buf *unwatch) {
  for (int w = 0; w < 8; w++) {
    int left = 0;
    for (int i = 0; watching[w].id && i < MOST_SCREENS && !left; i++)
      left = screens[i].id && !strcmp(screens[i].at, watching[w].at);
    if (!watching[w].id || left) continue;
    buf_fmt(unwatch, "{\"type\":\"unwatch\",\"id\":%ld}\n", watching[w].id);
    memset(&watching[w], 0, sizeof watching[w]);
  }
}

////////// 7. SIGNING IN //////////

static int attempt,
    signing;   // the sign in now, by its number, so a newer one or signing out ends it; signing: it waits
static char failed[256];   // why the last sign in in the background didn't finish, for status

/* Asks Onsay every two seconds whether the person pressed Connect, for as long as the code lasts, 10 minutes, then keeps
   the token as this lingo's secret setting and makes this device's key. The account it signed in as, or NULL with why.
   stop is the command line's. In the background it is NULL, and a newer sign in, or signing out, ends it instead. */
static val *connect_(arena *a, const char *body, volatile int *stop, int mine, buf *why) {
  volatile int never = 0;
  unsigned char k[32];
  double status = 0;
  for (double until = pl_now() + 600; pl_now() < until;) {
    if (stop)
      lingo_wait(&never, stop, 2);
    else
      pl_sleep(2);
    pl_enter(lock);
    int ended = stop ? *stop : mine != attempt;
    pl_leave(lock);
    if (ended) {
      buf_str(why, stop ? "Stopped by the person" : "Signing in again, or signing out, ended this sign in");
      return NULL;
    }
    buf error = { 0 };
    val *poll = api(a, "POST", "/connect/poll", body, &status, &error);
    buf_free(&error);
    if (!poll || !strcmp(text_or(poll, "status", ""), "waiting"))
      continue;   // a moment without the web is waited out too
    if (strcmp(text_or(poll, "status", ""), "done") || !*text_or(poll, "token", ""))
      buf_fmt(why, "Signing in didn't finish: %s", text_or(poll, "error", "Onsay sent no token"));
    else if (!lingo_set("onsay", "token", rec_get(poll, "token")))
      buf_str(why, "Signed in, but this computer couldn't keep its token");
    else if (!key_of("device", 1, k))
      buf_str(why, "Signed in, but this computer couldn't make or keep its key");
    else
      return rec_get(poll, "account") ? rec_get(poll, "account") : v_rec(a);
    return NULL;
  }
  buf_str(why, "The code ran out before Connect was pressed. Sign in again");
  return NULL;
}

typedef struct {
  int mine;
  char body[];
} waiting;

// A sign in finished in the background, while the lingo client runs on. status says when it is done.
static void background(void *arg) {
  waiting *w = arg;
  arena *a = arena_new();
  buf why = { 0 };
  int done = connect_(a, w->body, NULL, w->mine, &why) != NULL;
  if (!done) say("%s", why.s);
  pl_enter(lock);
  if (w->mine == attempt) {
    signing = 0;
    snprintf(failed, sizeof failed, "%s", done ? "" : why.s);
  }
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
  buf body = { 0 };
  buf error = { 0 };
  buf shown = { 0 };
  double status = 0;
  unsigned char key_[32], random[16];
  val *install = store(r, "store.get", v_cstr(a, "onsay/install"), NULL);
  val *out = v_rec(a);
  if (install->k != V_TEXT) {
    buf made = { 0 };
    pl_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++) buf_fmt(&made, "%02x", random[i]);
    install = v_text(a, made.s, made.n);
    store(r, "store.put", v_cstr(a, "onsay/install"), install);
    buf_free(&made);
  }
  const char *name = "Server";
  if (getenv("HOSTNAME")) name = getenv("HOSTNAME");
  if (getenv("COMPUTERNAME")) name = getenv("COMPUTERNAME");
  if (v[0]->k == V_TEXT) name = v[0]->t.s;
  buf_str(&body, "{\"name\":");
  buf_json_str(&body, name, strlen(name));
  buf_str(&body, ",\"install\":");
  buf_json(&body, install);
  buf_chr(&body, '}');
  val *start = api(a, "POST", "/connect", body.s, &status, &error);
  val *account;
  buf_free(&error);
  if (!start) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 201) fail(r, "phrase", "Onsay didn't start signing in: %s", text_or(start, "error", "it said no"));
  say("Open %s on any device, sign in, and press Connect. Your code is %s", text_or(start, "url", ""),
      text_or(start, "code", ""));
  body.n = 0;
  buf_str(&body, "{\"poll\":");
  buf_json_str(&body, text_or(start, "poll", ""), strlen(text_or(start, "poll", "")));
  buf_chr(&body, '}');
  if (on || lingo_serves()) {
    waiting *w = malloc(sizeof *w + body.n + 1);
    memcpy(w->body, body.s, body.n + 1);
    pl_enter(lock);
    w->mine = ++attempt;
    signing = 1;
    *failed = 0;
    pl_leave(lock);
    pl_thread(background, w);
    rec_set(a, out, "url", rec_get(start, "url") ? rec_get(start, "url") : NOTHING);
    rec_set(a, out, "code", rec_get(start, "code") ? rec_get(start, "code") : NOTHING);
    buf_free(&body);
    buf_free(&error);
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
  buf_free(&body);
  buf_free(&error);
  buf_free(&shown);
  return out;
}

// Forgets the token and this device's key, so signing in again makes a new one, and drops the connection.
static val *sign_out(run *r, val **v) {
  if (!lingo_set("onsay", "token", NOTHING)) fail(r, "phrase", "This computer couldn't forget its token");
  key_forget("device");
  pl_enter(lock);
  attempt++;
  signing = 0;
  *handle = *device = *account = 0;
  if (link_) pl_close(link_);
  pl_leave(lock);
  return NOTHING;
}

/* {signed_in, signing_in, failed, running, connected, handle, device, account, key}: failed is why the last sign in
   didn't finish, or nothing, and running whether this lingo runs. signing_in, failed, connected, handle, device and
   account only in a lingo client that serves. */
static val *status(run *r, val **v) {
  arena *a = r->a;
  val *out = v_rec(a);
  val *token = lingo_setting(a, "onsay", "token");
  unsigned char key_[32];
  buf shown = { 0 };
  if (key_of("device", 0, key_)) {
    buf_str(&shown, "ed25519:");
    base64(&shown, key_, 32);
  }
  rec_set(a, out, "signed_in", VBOOL(token && token->k == V_TEXT && token->t.len));
  pl_enter(lock);
  rec_set(a, out, "signing_in", VBOOL(signing));
  rec_set(a, out, "failed", *failed ? v_cstr(a, failed) : NOTHING);
  rec_set(a, out, "running", VBOOL(on));
  rec_set(a, out, "connected", VBOOL(ready));
  rec_set(a, out, "handle", *handle ? v_cstr(a, handle) : NOTHING);
  rec_set(a, out, "device", *device ? v_cstr(a, device) : NOTHING);
  rec_set(a, out, "account", *account ? v_cstr(a, account) : NOTHING);
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
  val *said = api(a, "GET", "/lingos", NULL, &status, &error);
  val *computers = rec_get(said, "computers");
  val *out = v_list(a);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to see your devices");
  if (status != 200 || !computers || computers->k != V_LIST)
    fail(r, "phrase", "Onsay couldn't list your devices: %s", text_or(said, "error", "it said no"));
  for (size_t i = 0; i < computers->l.n; i++) {
    val *c = computers->l.v[i];
    val *one = v_rec(a);
    rec_set(a, one, "name", rec_get(c, "name") ? rec_get(c, "name") : NOTHING);
    rec_set(a, one, "online", VBOOL(truthy(rec_get(c, "online"))));
    rec_set(a, one, "seen", rec_get(c, "seen") ? rec_get(c, "seen") : NOTHING);
    list_push(a, out, one);
  }
  return out;
}

// Onsay's AI for the account, as Onsay says: {plan, ends, over}, so an app can offer the subscription once the trial ends.
static val *ai(run *r, val **v) {
  buf error = { 0 };
  double status = 0;
  val *said = api(r->a, "GET", "/ai", NULL, &status, &error);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to use Onsay's AI");
  if (status != 200) fail(r, "phrase", "Onsay couldn't say: %s", text_or(said, "error", "it said no"));
  return said;
}

////////// 8. ITS PHRASES //////////

void base_calls(void);   // the calls every computer has, built for its platform: windows.c or linux.c

// Called once as the lingo client starts (core.c, BUILTINS).
void onsay_lingo(void) {
  static const lingo_hooks hooks = { running, decided, ask, key, said, kept };
  lock = pl_gate_new();
  logging = pl_gate_new();
  lingo_hook(&hooks);
  phrase_add("onsay", "store",
      "serve(request: record): /onsay, where prompts come in with this computer's prompt key, each answered by Onsay, "
      "and the sessions they make",
      serve);
  phrase_add("onsay", "net, store",
      "sign_in(name: text=nothing) -> {url,code}, or {handle,key} on the command line: signs this computer in to "
      "Onsay from any device's browser, at the address given with the code; status says when it is done",
      sign_in);
  phrase_add("onsay", "none", "sign_out(): this computer forgets its sign in and its key", sign_out);
  phrase_add("onsay", "net",
      "devices() -> [{name: text, online: bool, seen?: text}]: the account's computers, whether each is online, and when each was last seen",
      devices);
  phrase_add("onsay", "none",
      "status() -> {signed_in: bool, signing_in?: bool, failed?: text, running: bool, connected?: bool, handle?: text, device?: text, key?: text}: whether this computer is signed in to "
      "Onsay, and as what",
      status);
  phrase_add("onsay", "net",
      "ai() -> {plan: text, ends?: text, over: bool}: Onsay's AI for this account: its plan, trial or paid, when its free "
      "trial ends, and whether it is over",
      ai);
  phrase_add("onsay", "net",
      "share(values: record) -> {version, policy, signed}: shares with who at routes, full paths like [\"/photos/share\"] "
      "or every route when none, on devices when named. who is a handle, handle#tag, acct_..., public, me, my devices or "
      "onsay. Every computer is told",
      share);
  phrase_add("onsay", "net",
      "unshare(values: record) -> {version, policy, signed}: stops sharing with who, taking out every rule that names them",
      unshare);
  phrase_add("screen", "none",
      "open(title: text) -> whole: a new screen, a canvas the person can watch beside the to-do this work is for, like "
      "a browser tab working in the background. Onsay decides where it shows. It stays until closed. One of the same title "
      "already open for that to-do is given back",
      screen_open);
  phrase_add("screen", "none",
      "draw(screen: whole, instructions: list): draws on a screen with Canvas 2D instructions, each a record with one key, "
      "like {\"fillStyle\": \"#5865F2\"}, {\"fillRect\": [0, 0, 240, 12]} or {\"fillText\": [\"3 of 5\", 0, 30]}, after what "
      "it drew before. {\"clear\": [width, height]} starts it over at that size. The keys: clear, save, restore, translate, "
      "scale, rotate, beginPath, closePath, moveTo, lineTo, bezierCurveTo, quadraticCurveTo, arc, rect, roundRect, fill, "
      "stroke, clip, fillRect, strokeRect, clearRect, fillText, strokeText, drawImage with a data:image/ address, and "
      "fillStyle, strokeStyle, lineWidth, font, textAlign, textBaseline and globalAlpha, as in a web page's canvas",
      screen_draw);
  phrase_add("screen", "none",
      "show(screen: whole, picture: text): puts a picture on a screen, a PNG or JPEG as base64, replacing what it showed",
      screen_show);
  phrase_add("screen", "none", "close(screen: whole): takes a screen away", screen_close);
  base_calls();
}
