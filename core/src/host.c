// host.c - the core as a program of its own, for a device whose platform.h has threads and a WebSocket, like Windows.
//
//   onsay-core settings.json [--manifest]     --manifest prints what it tells Onsay it can do, and stops
//   onsay-core settings.json --sent           every message sent to Onsay printed on stdout too, {"sent": the message}
//                                             a line, so the program around the core can keep a log the person reads
//   onsay-core settings.json --sign-in        signs this device in from any other device's browser, for a server with
//                                             none, and writes its token into the settings
//
// Three threads: main (connects, then runs queued jobs one at a time, or looks at the lingos folder), receiver (hands
// every message to the core, answers pings, queues jobs) and heartbeat (pings every BEAT seconds, drops a silent link).
// A job that reaches lingo(...), use(...) or setting(...) asks Onsay and waits on main for the answer, running the calls
// Onsay sends meanwhile (ask_).
#include "core.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Built in, never a setting, so a copied core only ever talks to Onsay. The test build talks to test/server.mjs.
#ifdef TEST
#define SERVER "ws://127.0.0.1:8787/live"
#define API "http://127.0.0.1:8787"
#else
#define SERVER "wss://api.onsay.dev/live"
#define API "https://api.onsay.dev"
#endif
#define BEAT 20       // seconds between pings
#define SILENT 60     // seconds without a word before the link is taken as dead

typedef struct message { struct message *next; char *text; size_t n; int closed; } message;   // closed: the link closed

static pl_gate *gate;
static message *first, *last;
static pl_link *open_link;        // the open link, or NULL
static double heard;              // when the server last said anything
static int ponged;                // whether this server answers pings, so silence means something
static long asks;                 // lingo(...) requests sent, for their ids

static int showing;   // --sent: every message sent to Onsay printed on stdout too, {"sent":...} a line, for the app's log

static int send_(const char *text, size_t n) {
  pl_enter(gate);
  int sent = open_link && pl_send(open_link, text, n);
  pl_leave(gate);
  if (showing && sent) { fwrite("{\"sent\":", 1, 8, stdout); fwrite(text, 1, n, stdout); fwrite("}\n", 1, 2, stdout); fflush(stdout); }
  return sent;
}

/* lingo.run, lingo.use or lingo.setting on main, the job's thread: {"type":"lingo","id","ask","values","gives"}, for
   use(...) {"type":"lingo","id","address","values"}, or {"type":"lingo","id","key","reference"}, goes to Onsay, and the queue is read until its answer, {"type":"lingo","id",
   "value"} or with "error" and "code", which is the phrase's answer as it is. A call that comes meanwhile runs here,
   inside the job, since Onsay may need this device to answer. Everything else waits, in order, at the front of the queue.
   Gives 0 when the link closed or the job was stopped first. */
static int ask_(const char *values, size_t n, volatile int *stop, buf *answer) {
  arena *a = arena_new();
  const char *problem = NULL;
  long id = ++asks;
  buf out = { 0 };
  buf_fmt(&out, "{\"type\":\"lingo\",\"id\":%ld,", id);
  buf_add(&out, values + 1, n - 1);   // the values by name, after the id
  int got = 0, going = send_(out.s, out.n);
  buf_free(&out);
  message *held = NULL, *held_last = NULL;
  while (going && !got) {
    pl_enter(gate);
    while (!first && !*stop) pl_wait(gate, 1);
    message *m = *stop ? NULL : first;
    if (m) { first = m->next; if (!first) last = NULL; m->next = NULL; }
    pl_leave(gate);
    if (!m) break;
    val *said = m->closed ? NULL : json_parse(a, m->text, m->n, &problem);
    const char *type = said && !problem && said->k == V_REC && rec_get(said, "type") && rec_get(said, "type")->k == V_TEXT ? rec_get(said, "type")->t.s : "";
    val *of = said && *type ? rec_get(said, "id") : NULL;
    if (!strcmp(type, "lingo") && of && of->k == V_NUM && of->n == (double)id) { buf_add(answer, m->text, m->n); got = 1; }
    else if (!strcmp(type, "call")) core_do(m->text, m->n);
    else {
      going = !m->closed;
      if (held_last) held_last->next = m; else held = m;
      held_last = m;
      continue;
    }
    free(m->text);
    free(m);
  }
  pl_enter(gate);
  if (held) { held_last->next = first; first = held; if (!last) last = held_last; }
  pl_leave(gate);
  arena_free(a);
  return got;
}

static void queue(char *text, size_t n, int closed) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, n, closed };
  pl_enter(gate);
  if (last) last->next = m; else first = m;
  last = m;
  pl_wake(gate);
  pl_leave(gate);
}

// On its own thread, so stops and pings land while a job runs.
static void receiver(void *arg) {
  pl_link *l = arg;
  for (;;) {
    buf in = { 0 };
    if (!pl_receive(l, &in)) { buf_free(&in); break; }
    int kind = core_heard(in.s, in.n);
    pl_enter(gate);
    heard = pl_now();
    if (kind == CORE_PONG) ponged = 1;
    if (kind == CORE_DONE) pl_wake(gate);   // a stop, which may end a job's wait for lingo(...)
    pl_leave(gate);
    if (kind == CORE_PING) pl_send(l, "{\"type\":\"pong\"}", 15);
    if (kind == CORE_JOB) queue(in.s, in.n, 0);   // the queue owns the text now
    else buf_free(&in);
  }
  queue(NULL, 0, 1);
}

static void heartbeat(void *arg) {
  (void)arg;
  for (;;) {
    pl_sleep(BEAT);
    pl_enter(gate);
    if (open_link) {
      if (ponged && pl_now() - heard > SILENT) { core_say("The server has been silent for %d seconds, so reconnecting", SILENT); pl_close(open_link); }
      else pl_send(open_link, "{\"type\":\"ping\"}", 15);
    }
    pl_leave(gate);
  }
}

static void serve(void) {
  double wait = 1;
  for (;;) {
    buf error = { 0 };
    core_say("Connecting to %s", SERVER);
    pl_link *l = pl_connect(SERVER, &error);
    if (!l) {
      core_say("%s. Trying again in %.0f seconds", error.s ? error.s : "It couldn't connect", wait);
      buf_free(&error);
      pl_sleep(wait);
      wait = wait * 2 > 30 ? 30 : wait * 2;
      continue;
    }
    pl_enter(gate);
    open_link = l;
    heard = pl_now();
    ponged = 0;
    pl_leave(gate);
    core_open();
    pl_thread(receiver, l);

    for (;;) {
      pl_enter(gate);
      if (!first) pl_wait(gate, CORE_LOOK);
      message *m = first;
      if (m) { first = m->next; if (!first) last = NULL; }
      pl_leave(gate);
      if (!m) { core_look(); continue; }
      if (m->closed) { free(m); break; }
      if (core_do(m->text, m->n)) wait = 1;   // signed in, so the next drop starts its waiting over
      free(m->text);
      free(m);
    }
    pl_enter(gate);
    open_link = NULL;
    pl_leave(gate);
    pl_free(l);
    core_say("The connection closed. Trying again in %.0f seconds", wait);
    pl_sleep(wait);
    wait = wait * 2 > 30 ? 30 : wait * 2;
  }
}

// A record's text, or "" when it has none by that name.
static const char *field(val *r, const char *name) {
  val *v = r && r->k == V_REC ? rec_get(r, name) : NULL;
  return v && v->k == V_TEXT ? v->t.s : "";
}

// body POSTed to Onsay at path. Its answer as a record, and its status, or NULL with why in error when it wasn't reached.
static val *post(arena *a, const char *path, const char *body, double *status, buf *error) {
  buf values = { 0 }, out = { 0 };
  buf_fmt(&values, "[\"POST\",\"%s%s\",{\"content-type\":\"application/json\"},", API, path);
  buf_json_str(&values, body, strlen(body));
  buf_str(&values, ",30]");
  pl_http(values.s, values.n, &out);
  val *answer = json_parse(a, out.s ? out.s : "", out.n, NULL), *v = answer && answer->k == V_REC ? rec_get(answer, "value") : NULL;
  val *said = NULL, *text = v && v->k == V_REC ? rec_get(v, "text") : NULL;
  if (!v || !text) buf_str(error, *field(answer, "error") ? field(answer, "error") : "Onsay couldn't be reached");
  else {
    *status = rec_get(v, "status")->n;
    said = json_parse(a, text->t.s, text->t.len, NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  }
  buf_free(&values);
  buf_free(&out);
  return said;
}

// What the Windows app does in Link.Connect: the person approves this device on onsay.dev from any device, while this
// asks every two seconds, for as long as the code lasts (10 minutes). install is this device's own id, kept in the
// settings, so signing in again gets it its own instance back.
static int sign_in(const char *path) {
  arena *a = arena_new();
  buf text = { 0 }, body = { 0 }, error = { 0 };
  val *s = pl_read(path, &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  if (!s || s->k != V_REC) { core_say("Can't read the settings file %s, which must be a JSON record", path); return 1; }
  if (!*field(s, "install")) {
    char id[33] = { 0 };
    srand((unsigned)(pl_now() * 1e6) ^ (unsigned)(size_t)&id);
    for (int i = 0; i < 32; i++) id[i] = "0123456789abcdef"[rand() % 16];
    rec_set(a, s, "install", v_cstr(a, id));
  }
  const char *name = *field(s, "name") ? field(s, "name") : getenv("HOSTNAME") ? getenv("HOSTNAME") : getenv("COMPUTERNAME") ? getenv("COMPUTERNAME") : "Server";
  double status = 0;
  buf_str(&body, "{\"name\":");
  buf_json_str(&body, name, strlen(name));
  buf_str(&body, ",\"install\":");
  buf_json_str(&body, field(s, "install"), strlen(field(s, "install")));
  buf_chr(&body, '}');
  val *start = post(a, "/connect", body.s, &status, &error);
  if (start && status != 201) buf_fmt(&error, "%s", *field(start, "error") ? field(start, "error") : "Onsay didn't start the sign in");
  if (error.n) { core_say("Signing in didn't start: %s", error.s); return 1; }
  core_say("Open %s on any device, sign in, and press Connect. Your code is %s", field(start, "url"), field(start, "code"));

  body.n = 0;
  buf_str(&body, "{\"poll\":");
  buf_json_str(&body, field(start, "poll"), strlen(field(start, "poll")));
  buf_chr(&body, '}');
  val *done = NULL;
  for (double until = pl_now() + 600; !done && pl_now() < until;) {
    pl_sleep(2);
    error.n = 0;
    val *poll = post(a, "/connect/poll", body.s, &status, &error);
    if (!poll || !strcmp(field(poll, "status"), "waiting")) continue;   // a moment without the web is waited out too
    if (strcmp(field(poll, "status"), "done") || !*field(poll, "token")) {
      core_say("Signing in didn't finish: %s", *field(poll, "error") ? field(poll, "error") : "Onsay sent no token");
      return 1;
    }
    done = poll;
  }
  if (!done) { core_say("The code ran out before Connect was pressed. Run onsay-core %s --sign-in again", path); return 1; }
  rec_set(a, s, "token", rec_get(done, "token"));
  text.n = 0;
  buf_json(&text, s);
  if (!pl_write(path, text.s, text.n)) { core_say("Signed in, but the settings file %s couldn't be written", path); return 1; }
  val *account = rec_get(done, "account");
  core_say("Signed in as %s (%s). The token is in %s", field(account, "handle"), field(account, "email"), path);
  buf_free(&text), buf_free(&body), buf_free(&error);
  arena_free(a);
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "onsay.json";
  for (int i = 2; i < argc; i++) showing |= !strcmp(argv[i], "--sent");
  if (argc > 2 && !strcmp(argv[2], "--sign-in")) return sign_in(path);
  gate = pl_gate_new();
  buf settings = { 0 };
  if (!pl_read(path, &settings)) { core_say("Can't read the settings file %s", path); return 1; }
  int began = core_begin(settings.s, settings.n, send_, ask_);
  buf_free(&settings);
  if (!began) return 1;
  if (argc > 2 && !strcmp(argv[2], "--manifest")) {
    buf out = { 0 };
    core_manifests(&out);
    fwrite(out.s, 1, out.n, stdout);
    return 0;
  }
  pl_thread(heartbeat, NULL);
  serve();
  return 0;
}
