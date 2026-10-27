// host.c - the core as a program of its own, for a device whose platform.h has threads and a WebSocket, like Windows.
//
//   onsay-core settings.json [--manifest]     --manifest prints what it tells Onsay it can do, and stops
//   onsay-core settings.json --sent           every message sent to Onsay printed on stdout too, {"sent": the message}
//                                             a line, so the program around the core can keep a log the person reads
//   onsay-core settings.json --sign-in        signs this device in from any other device's browser, for a server with
//                                             none, and writes its token into the settings
//   onsay-core settings.json --settings <lingo>
//                                             prints every setting of a lingo as JSON, as settings.get gives them, for
//                                             the app's form of them
//   onsay-core settings.json --set <lingo> <key> <value>
//                                             changes one wherever that lingo keeps it, as settings.set does. value is
//                                             JSON, or text as it is when it isn't JSON
//
// Threads: main (runs queued jobs one at a time, in order, or looks at the lingos folder), connection (connects, again
// after a drop, hands every message to the core, answers pings, queues jobs), heartbeat (pings every BEAT seconds, drops
// a silent link), listener (takes each connection to the port), and one for each web request (reads it, queues it as a
// job, and writes its answer). With no token there is no connection or heartbeat: it runs bare, answering its port only.
// A job that reaches lingo(...) or use(...) asks Onsay and waits on main for the answer, running the calls Onsay sends
// meanwhile (ask_).
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
#define HEAD (64 << 10)   // the most a web request's first line and headers may be
#define ROOM (4 << 20)    // and its body

typedef struct { buf response; int done; } answer;   // a web request's answer, which its thread waits for
// One job for main: what Onsay said, the link opening or closing, or a web request from the port, with where it came
// from and where its answer goes.
typedef struct message { struct message *next; char *text; size_t n; int kind, from; answer *to; } message;
enum { SAID, OPENED, CLOSED };

static pl_gate *gate;
static message *first, *last;
static pl_link *open_link;        // the open link, or NULL
static double heard;              // when the server last said anything
static int ponged;                // whether this server answers pings, so silence means something
static int signed_in;             // whether Onsay took this link's hello
static long asks, requests;       // lingo(...) requests sent and web requests taken, for their ids

static int showing;   // --sent: every message sent to Onsay printed on stdout too, {"sent":...} a line, for the app's log

static int send_(const char *text, size_t n) {
  pl_enter(gate);
  int sent = open_link && pl_send(open_link, text, n);
  pl_leave(gate);
  if (showing && sent) { fwrite("{\"sent\":", 1, 8, stdout); fwrite(text, 1, n, stdout); fwrite("}\n", 1, 2, stdout); fflush(stdout); }
  return sent;
}

/* lingo.run or lingo.use on main, the job's thread: {"type":"lingo","id","ask","values","gives"}, or for use(...)
   {"type":"lingo","id","address","values"}, goes to Onsay, and the queue is read until its answer, {"type":"lingo","id",
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
    val *said = m->text && !m->to ? json_parse(a, m->text, m->n, &problem) : NULL;
    const char *type = said && !problem && said->k == V_REC && rec_get(said, "type") && rec_get(said, "type")->k == V_TEXT ? rec_get(said, "type")->t.s : "";
    val *of = said && *type ? rec_get(said, "id") : NULL;
    if (!strcmp(type, "lingo") && of && of->k == V_NUM && of->n == (double)id) { buf_add(answer, m->text, m->n); got = 1; }
    else if (!strcmp(type, "call")) core_do(m->text, m->n);
    else {
      going = m->kind != CLOSED;
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

static void queue(char *text, size_t n, int kind, int from, answer *to) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, n, kind, from, to };
  pl_enter(gate);
  if (last) last->next = m; else first = m;
  last = m;
  pl_wake(gate);
  pl_leave(gate);
}

// Connects to Onsay, and again after a drop, waiting longer each time it fails. Every message heard goes to the core
// here, on this thread, so stops and pings land while a job runs.
static void connection(void *arg) {
  (void)arg;
  double wait = 1;
  for (;;) {
    buf error = { 0 };
    core_say("Connecting to %s", SERVER);
    pl_link *l = pl_connect(SERVER, &error);
    if (!l) core_say("%s. Trying again in %.0f seconds", error.s ? error.s : "It couldn't connect", wait);
    buf_free(&error);
    if (l) {
      pl_enter(gate);
      open_link = l;
      heard = pl_now();
      ponged = signed_in = 0;
      pl_leave(gate);
      queue(NULL, 0, OPENED, 0, NULL);
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
        if (kind == CORE_JOB) queue(in.s, in.n, SAID, 0, NULL);   // the queue owns the text now
        else buf_free(&in);
      }
      queue(NULL, 0, CLOSED, 0, NULL);
      pl_enter(gate);
      open_link = NULL;
      if (signed_in) wait = 1;   // so the next drop starts its waiting over
      pl_leave(gate);
      pl_free(l);
      core_say("The connection closed. Trying again in %.0f seconds", wait);
    }
    pl_sleep(wait);
    wait = wait * 2 > 30 ? 30 : wait * 2;
  }
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

// main: every job in turn, hello when a link opens, and a look at the lingos folder while there is nothing to do.
static void jobs(void) {
  for (;;) {
    pl_enter(gate);
    if (!first) pl_wait(gate, CORE_LOOK);
    message *m = first;
    if (m) { first = m->next; if (!first) last = NULL; }
    pl_leave(gate);
    if (!m) { core_look(); continue; }
    if (m->kind == OPENED) core_open();
    else if (m->to) {
      core_serve(m->text, m->n, m->from, &m->to->response);
      pl_enter(gate);
      m->to->done = 1;
      pl_wake(gate);
      pl_leave(gate);
    } else if (m->kind == SAID && core_do(m->text, m->n)) {
      pl_enter(gate);
      signed_in = 1;
      pl_leave(gate);
    }
    free(m->text);
    free(m);
  }
}

// An answer, {status, headers, body} as core_serve gives one, written to a web request's connection.
static void respond(pl_socket *c, val *r) {
  val *h = rec_get(r, "headers"), *status = rec_get(r, "status");
  buf out = { 0 }, body = { 0 };
  buf_text(&body, rec_get(r, "body") ? rec_get(r, "body") : NOTHING);
  buf_fmt(&out, "HTTP/1.1 %d \r\n", status && status->k == V_NUM ? (int)status->n : 500);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    const char *name = h->r.key[i], *value = h->r.v[i]->k == V_TEXT ? h->r.v[i]->t.s : "";
    // Its length and closing are written below, and a line break would start a header of its own.
    if (!*name || strpbrk(name, "\r\n: ") || strpbrk(value, "\r\n") || !strcmp(name, "content-length") || !strcmp(name, "connection") ||
        !strcmp(name, "transfer-encoding")) continue;
    buf_fmt(&out, "%s: %s\r\n", name, value);
  }
  buf_fmt(&out, "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.n);
  buf_add(&out, body.s ? body.s : "", body.n);
  pl_give(c, out.s, out.n);
  buf_free(&out), buf_free(&body);
}

typedef struct { pl_socket *c; int here; } visit;

/* One web request to the port, on its own thread: its first line, headers and a body of Content-Length, read whole,
   answered on main in its turn (core_serve), and closed. One that isn't a web request is answered here with why, and
   one that went quiet before its headers ended is closed. */
static void visitor(void *arg) {
  visit v = *(visit *)arg;
  free(arg);
  arena *a = arena_new();
  buf in = { 0 }, msg = { 0 };
  char chunk[16384];
  const char *end = NULL, *bad = NULL;
  int status = 400, got = 1;
  size_t method = 0, path = 0, size = 0, from = 0;
  val *headers = v_rec(a), *r = NULL;
  while (got && !(end = in.s ? strstr(in.s, "\r\n\r\n") : NULL) && in.n < HEAD) if ((got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
  if (!end && in.n >= HEAD) status = 431, bad = "Its headers are more than 64 KB";
  else if (end) {
    method = strspn(in.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    path = method && method < 16 && in.s[method] == ' ' && in.s[method + 1] == '/' ? strcspn(in.s + method + 1, " \r\n") : 0;
    const char *version = in.s + method + 1 + path;
    if (!path || strncmp(version, " HTTP/1.", 8) || (version[8] != '0' && version[8] != '1') || strncmp(version + 9, "\r\n", 2) || !utf8_ok(in.s, (size_t)(end - in.s)))
      bad = "That isn't a web request. Its first line is like GET /memo HTTP/1.1";
    for (const char *s = strstr(in.s, "\r\n") + 2; !bad && s < end; s = strstr(s, "\r\n") + 2) {
      const char *eol = strstr(s, "\r\n"), *colon = memchr(s, ':', (size_t)(eol - s)), *value = colon, *stop = eol;
      if (!colon || colon == s || memchr(s, ' ', (size_t)(colon - s))) { bad = "A header isn't a name, a colon and its value"; break; }
      while (++value < eol && (*value == ' ' || *value == '\t')) {}
      while (stop > value && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
      // A name said twice has both values, joined by ", ".
      buf name = { 0 }, both = { 0 };
      for (const char *x = s; x < colon; x++) buf_chr(&name, (char)(*x >= 'A' && *x <= 'Z' ? *x + 32 : *x));
      val *had = rec_get(headers, name.s);
      if (had) { buf_add(&both, had->t.s, had->t.len); buf_str(&both, ", "); }
      buf_add(&both, value, (size_t)(stop - value));
      rec_set(a, headers, name.s, v_text(a, both.s ? both.s : "", both.n));
      buf_free(&name), buf_free(&both);
    }
    val *length = rec_get(headers, "content-length"), *expect = rec_get(headers, "expect");
    const char *n = length ? length->t.s : "0";
    if (!bad && rec_get(headers, "transfer-encoding")) status = 411, bad = "Send its body with a Content-Length, not in chunks";
    else if (!bad && (!*n || strspn(n, "0123456789") != strlen(n))) bad = "Its Content-Length isn't a number";
    else if (!bad && (strlen(n) > 9 || (size = strtoul(n, NULL, 10)) > ROOM)) status = 413, bad = "Its body is more than 4 MB";
    from = (size_t)(end - in.s) + 4;
    if (!bad && expect && !strcmp(expect->t.s, "100-continue") && in.n - from < size) pl_give(v.c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (!bad && in.n - from < size && (got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
    if (!bad && in.n - from < size) bad = "Its body was shorter than its Content-Length";
    else if (!bad && !utf8_ok(in.s + from, size)) bad = "Its body isn't text, and lingos take text, like JSON";
  }

  if (bad) {
    buf error = { 0 };
    buf_str(&error, "{\"error\":");
    buf_json_str(&error, bad, strlen(bad));
    buf_chr(&error, '}');
    buf_fmt(&msg, "{\"status\":%d,\"headers\":{\"content-type\":\"application/json\"},\"body\":", status);
    buf_json_str(&msg, error.s, error.n);
    buf_chr(&msg, '}');
    r = json_parse(a, msg.s, msg.n, NULL);
    buf_free(&msg), buf_free(&error);
  } else if (end) {
    answer to = { { 0 }, 0 };
    pl_enter(gate);
    buf_fmt(&msg, "{\"type\":\"request\",\"id\":%ld,\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_str(&msg, ",\"base64\":false}");
    queue(msg.s, msg.n, SAID, v.here ? CORE_HERE : CORE_NETWORK, &to);   // the queue owns the text now
    pl_enter(gate);
    while (!to.done) pl_wait(gate, 1);
    pl_leave(gate);
    r = json_parse(a, to.response.s ? to.response.s : "", to.response.n, NULL);
    buf_free(&to.response);
  }
  if (r) respond(v.c, r);
  pl_hang_up(v.c);
  buf_free(&in);
  arena_free(a);
}

// Takes each connection to the port, and reads it on a thread of its own.
static void listener(void *arg) {
  for (;;) {
    visit *v = malloc(sizeof *v);
    if ((v->c = pl_accept(arg, &v->here))) pl_thread(visitor, v);
    else { free(v); pl_sleep(0.1); }
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
  val *s = began ? json_parse(arena_new(), settings.s, settings.n, NULL) : NULL, *port = s ? rec_get(s, "port") : NULL;
  buf_free(&settings);
  if (!began) return 1;
  if (argc > 2 && !strcmp(argv[2], "--manifest")) {
    buf out = { 0 };
    core_manifests(&out);
    fwrite(out.s, 1, out.n, stdout);
    return 0;
  }
  int setting = argc > 2 && !strcmp(argv[2], "--set");
  if (setting || (argc > 2 && !strcmp(argv[2], "--settings"))) {
    if (argc < (setting ? 6 : 4)) { core_say("Say which lingo, like --settings agent, or --set agent model claude-opus-5-5"); return 1; }
    static const char *none[] = { "none" };
    volatile int stop = 0;
    arena *a = arena_new();
    const char *problem = NULL;
    val *values = v_list(a), *value = setting ? json_parse(a, argv[5], strlen(argv[5]), &problem) : NULL;
    for (int i = 3; i < (setting ? 5 : 4); i++) list_push(a, values, v_cstr(a, argv[i]));
    if (setting) list_push(a, values, problem ? v_cstr(a, argv[5]) : value);
    buf out = { 0 }, shown = { 0 };
    run_phrase(a, "null", setting ? "settings.set" : "settings.get", values, &stop, none, 1, &out);
    val *done = json_parse(a, out.s, out.n, NULL), *error = rec_get(done, "error");
    if (error) { core_say("%s", field(error, "message")); return 1; }
    if (!setting) { buf_json(&shown, rec_get(done, "value")); buf_chr(&shown, '\n'); fwrite(shown.s, 1, shown.n, stdout); }
    return 0;
  }
  // The port, here (this computer only) unless listen says network, and the connection to Onsay when there is a token.
  buf error = { 0 };
  int number = port && port->k == V_NUM ? (int)port->n : 6969;
  pl_socket *listening = pl_listen(number, !strcmp(field(s, "listen"), "network"), &error);
  if (listening) { core_say("Answering web requests on port %d", number); pl_thread(listener, listening); }
  else core_say("%s, so it answers no web requests", error.s);
  if (*field(s, "token")) { pl_thread(connection, NULL); pl_thread(heartbeat, NULL); }
  else if (!listening) return 1;
  else core_say("The settings have no token, so it runs on its own, with no connection to Onsay");
  jobs();
  return 0;
}
