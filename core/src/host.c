// host.c - the lingo client as a program of its own, on a device whose platform.h has threads and a WebSocket.
//
//   onsay-core settings.json                 runs it: its port, and its connection to Onsay when it has a token
//   onsay-core settings.json --sent          every message sent to Onsay printed on stdout too, {"sent": the message} a
//                                            line, so the program around it can keep a log the person reads
//   onsay-core settings.json <verb> ...      one thing done, whether or not it runs, its answer printed as JSON:
//     pack <folder> [<to>]                   a folder of a lingo's files made into its .lingo file
//     start <lingo>, stop <lingo>            a lingo running or stopped
//     share <lingo> <who> [--route <r>]      who may reach it: public, ip 10.0.0.0/8, my devices or a handle
//     grant <lingo> <need> [<path>]          what it may do, like run, or files.read and a folder
//     ceiling <needs...>                     the most this computer ever allows
//     policy                                 the policy in force
//     settings <lingo>                       its settings, each with its value now
//     set <lingo> <key> <value>              one changed. value is JSON, or text as it is when it isn't JSON
//     sign-in                                signs this computer in to Onsay from any other device's browser, for a
//                                            server with none, and writes its token into the settings
//
// Threads: main (runs queued jobs one at a time, in order, and looks at the lingos folder), connection (connects, again
// after a drop, hands every message to the core, queues jobs), heartbeat (pings every BEAT seconds, drops a silent
// link), listener (takes each connection to the port), and one for each web request (reads it, queues it as a job, and
// writes its answer). With no token there is no connection or heartbeat: it runs bare, answering its port only. A job
// that reaches lingo(...) or use(...), or waits for the person's answer, waits on main, answering the requests that come
// meanwhile (meanwhile).
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
static long asks, requests;       // lingo(...) asks sent and web requests taken, for their ids

static int port, outside;   // the port, and whether it is open to other computers, as core_look says
static int showing;   // --sent: every message sent to Onsay printed on stdout too, {"sent":...} a line, for the app's log

////////// JOBS //////////

static int send_(const char *text, size_t n) {
  pl_enter(gate);
  int sent = open_link && pl_send(open_link, text, n);
  pl_leave(gate);
  if (showing && sent) { fwrite("{\"sent\":", 1, 8, stdout); fwrite(text, 1, n, stdout); fwrite("}\n", 1, 2, stdout); fflush(stdout); }
  return sent;
}

// One job, on main: hello when a link opens, a web request from the port answered to its thread, or what Onsay said.
// 1 when it was Onsay taking the hello.
static int handle(message *m) {
  if (m->kind == OPENED) core_open();
  else if (m->to) {
    core_serve(m->text, m->n, m->from, &m->to->response);
    pl_enter(gate);
    m->to->done = 1;
    pl_wake(gate);
    pl_leave(gate);
  } else if (m->kind == SAID) return core_do(m->text, m->n);
  return 0;
}

/* On main, while a job waits: for Onsay's answer to its lingo(...) or use(...) number id, {"type":"lingo","id","value"}
   or with "error" and "code", put in answer as it is, or with id 0 until done is set, by a request answered meanwhile,
   or seconds pass. A request that comes meanwhile, from the port or from Onsay, is answered here, inside the job, since
   what answers may be on this computer. Everything else waits, in order, at the front of the queue. Waiting for Onsay
   ends when the link closes. Gives 1 when Onsay's answer came. */
static int meanwhile(long id, buf *answer, volatile int *done, volatile int *stop, double seconds) {
  arena *a = arena_new();
  int got = 0, going = 1;
  double until = pl_now() + seconds;
  message *held = NULL, *held_last = NULL;
  while (going && !got && !(done && *done) && pl_now() < until) {
    pl_enter(gate);
    while (!first && !*stop && pl_now() < until) pl_wait(gate, until - pl_now() < 1 ? until - pl_now() : 1);
    message *m = *stop ? NULL : first;
    if (m) { first = m->next; if (!first) last = NULL; m->next = NULL; }
    pl_leave(gate);
    if (!m) break;
    val *said = m->kind == SAID && !m->to ? json_parse(a, m->text, m->n, NULL) : NULL, *of = said && said->k == V_REC ? rec_get(said, "id") : NULL;
    const char *type = text_or(said, "type", "");
    if (id && !strcmp(type, "lingo") && of && of->k == V_NUM && of->n == (double)id) { buf_add(answer, m->text, m->n); got = 1; }
    else if (m->to || !strcmp(type, "request")) handle(m);
    else {
      going = !id || m->kind != CLOSED;
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

// lingo.run or lingo.use: {"type":"lingo","id","ask","values","gives","context","asking"}, or for use(...)
// {"type":"lingo","id","address","values"}, sent to Onsay, then its answer waited for. 0 when the link closed or the job
// was stopped first.
static int ask_(const char *values, size_t n, volatile int *stop, buf *answer) {
  long id = ++asks;
  buf out = { 0 };
  buf_fmt(&out, "{\"type\":\"lingo\",\"id\":%ld,", id);
  buf_add(&out, values + 1, n - 1);   // the values by name, after the id
  int going = send_(out.s, out.n);
  buf_free(&out);
  return going && meanwhile(id, answer, NULL, stop, 1e12);
}

// The person's answer, waited for.
static void wait_(volatile int *done, volatile int *stop, double seconds) { meanwhile(0, NULL, done, stop, seconds); }

static void queue(char *text, size_t n, int kind, int from, answer *to) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, n, kind, from, to };
  pl_enter(gate);
  if (last) last->next = m; else first = m;
  last = m;
  pl_wake(gate);
  pl_leave(gate);
}

// main: every job in turn, and a look at the lingos folder every CORE_LOOK seconds, however busy.
static void jobs(void) {
  for (double next = pl_now() + CORE_LOOK;;) {
    pl_enter(gate);
    if (!first) pl_wait(gate, CORE_LOOK);
    message *m = first;
    if (m) { first = m->next; if (!first) last = NULL; }
    pl_leave(gate);
    if (pl_now() >= next) {
      int wide = core_look();
      pl_enter(gate);
      outside = wide;
      pl_leave(gate);
      next = pl_now() + CORE_LOOK;
    }
    if (!m) continue;
    if (handle(m)) {
      pl_enter(gate);
      signed_in = 1;
      pl_leave(gate);
    }
    free(m->text);
    free(m);
  }
}

////////// THE CONNECTION //////////

// Connects to Onsay, and again after a drop, waiting longer each time it fails. Every message heard goes to the core
// here, on this thread, so stops land while a job runs.
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
      if (ponged && pl_now() - heard > SILENT) { core_say("Onsay has been silent for %d seconds, so reconnecting", SILENT); pl_close(open_link); }
      else pl_send(open_link, "{\"type\":\"ping\"}", 15);
    }
    pl_leave(gate);
  }
}

////////// THE PORT //////////

// An answer written to a web request's connection: its status, its headers by lower case name, and its body.
static void respond(pl_socket *c, int status, val *h, const char *body, size_t n) {
  buf out = { 0 };
  buf_fmt(&out, "HTTP/1.1 %d \r\n", status);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    const char *name = h->r.key[i], *value = h->r.v[i]->k == V_TEXT ? h->r.v[i]->t.s : "";
    // Its length and closing are written below, and a line break would start a header of its own.
    if (!*name || strpbrk(name, "\r\n: ") || strpbrk(value, "\r\n") || !strcmp(name, "content-length") || !strcmp(name, "connection") ||
        !strcmp(name, "transfer-encoding")) continue;
    buf_fmt(&out, "%s: %s\r\n", name, value);
  }
  buf_fmt(&out, "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
  buf_add(&out, body, n);
  pl_give(c, out.s, out.n);
  buf_free(&out);
}

typedef struct { pl_socket *c; buf from; } visit;

/* One web request to the port, on its own thread: its first line, headers and a body of Content-Length, read whole,
   answered on main in its turn (core_serve), and closed. One that isn't a web request is answered here with why, and
   one that went quiet before its headers ended is closed. */
static void visitor(void *arg) {
  visit v = *(visit *)arg;
  free(arg);
  arena *a = arena_new();
  buf in = { 0 };
  char chunk[16384];
  const char *end = NULL, *bad = NULL;
  int status = 400, got = 1;
  size_t method = 0, path = 0, size = 0, from = 0;
  val *headers = v_rec(a);
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
      header_add(a, headers, s, (size_t)(colon - s), value, (size_t)(stop - value));
    }
    const char *length = text_or(headers, "content-length", "0");
    if (!bad && rec_get(headers, "transfer-encoding")) status = 411, bad = "Send its body with a Content-Length, not in chunks";
    else if (!bad && (!*length || strspn(length, "0123456789") != strlen(length))) bad = "Its Content-Length isn't a number";
    else if (!bad && (strlen(length) > 9 || (size = strtoul(length, NULL, 10)) > ROOM)) status = 413, bad = "Its body is more than 4 MB";
    from = (size_t)(end - in.s) + 4;
    if (!bad && !strcmp(text_or(headers, "expect", ""), "100-continue") && in.n - from < size) pl_give(v.c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (!bad && in.n - from < size && (got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
    if (!bad && in.n - from < size) bad = "Its body was shorter than its Content-Length";
    else if (!bad && !utf8_ok(in.s + from, size)) bad = "Its body isn't text, and lingos take text, like JSON";
  }

  if (bad) {
    buf body = { 0 };
    val *h = v_rec(a);
    rec_set(a, h, "content-type", v_cstr(a, "application/json"));
    buf_str(&body, "{\"error\":");
    buf_json_str(&body, bad, strlen(bad));
    buf_chr(&body, '}');
    respond(v.c, status, h, body.s, body.n);
    buf_free(&body);
  } else if (end) {
    answer to = { { 0 }, 0 };
    buf msg = { 0 };
    pl_enter(gate);
    buf_fmt(&msg, "{\"type\":\"request\",\"id\":\"port %ld\",\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"ip\":");
    buf_json_str(&msg, v.from.s ? v.from.s : "", v.from.n);
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_chr(&msg, '}');
    queue(msg.s, msg.n, SAID, CORE_PORT, &to);   // the queue owns the text now
    pl_enter(gate);
    while (!to.done) pl_wait(gate, 1);
    pl_leave(gate);
    val *r = json_parse(a, to.response.s ? to.response.s : "", to.response.n, NULL), *status_ = rec_get(r, "status"), *body = rec_get(r, "body");
    respond(v.c, status_ && status_->k == V_NUM ? (int)status_->n : 500, rec_get(r, "headers"), body ? body->t.s : "", body ? body->t.len : 0);
    buf_free(&to.response);
  }
  pl_hang_up(v.c);
  buf_free(&in);
  buf_free(&v.from);
  arena_free(a);
}

// The port, on every address this computer has when the policy lets anyone else in, else on this computer alone, so
// nothing asks to be let through its firewall until something is shared. NULL when it can't be, said when say is set.
static pl_socket *listen_(int everywhere, int say) {
  buf error = { 0 };
  pl_socket *p = pl_listen(port, everywhere, &error);
  if (p) core_say("Answering web requests on port %d, %s", port, everywhere ? "from other computers too" : "from this computer only");
  else if (say) core_say("%s, so it answers no web requests", error.s);
  buf_free(&error);
  return p;
}

// Takes each connection to the port, and reads it on a thread of its own. When the policy changes who it lets in, the
// port is opened again to fit, within a second of main's look, and tried again each second while it can't be.
static void listener(void *arg) {
  pl_socket *p = arg;
  int wide = outside;
  for (;;) {
    pl_enter(gate);
    int want = outside;
    pl_leave(gate);
    if (want != wide || !p) {
      if (p) pl_hang_up(p);
      p = listen_(want, want != wide);
      wide = want;
      if (!p) { pl_sleep(1); continue; }
    }
    visit *v = calloc(1, sizeof *v);
    if ((v->c = pl_accept(p, &v->from))) pl_thread(visitor, v);
    else { buf_free(&v->from); free(v); }
  }
}

////////// SIGNING IN //////////

// body POSTed to Onsay at path. Its answer as a record, and its status, or NULL with why in error when it wasn't reached.
static val *post(arena *a, const char *path, const char *body, double *status, buf *error) {
  buf url = { 0 };
  val *headers = v_rec(a), *said = NULL;
  rec_set(a, headers, "content-type", v_cstr(a, "application/json"));
  buf_fmt(&url, "%s%s", API, path);
  val *got = pl_http(a, "POST", url.s, headers, body, strlen(body), 30, error), *text = got ? rec_get(got, "text") : NULL;
  buf_free(&url);
  if (got) {
    *status = rec_get(got, "status")->n;
    said = json_parse(a, text->t.s, text->t.len, NULL);
    if (!said || said->k != V_REC) said = v_rec(a);
  }
  return said;
}

// What the Windows app does in Link.Connect: the person approves this computer on onsay.dev from any device, while this
// asks every two seconds, for as long as the code lasts (10 minutes). install is this computer's own id, kept in the
// settings, so signing in again gets it its own instance back.
static int sign_in(const char *path, const core_settings *s) {
  arena *a = arena_new();
  buf text = { 0 }, body = { 0 }, error = { 0 };
  val *all = s->all;
  if (!*text_or(all, "install", "")) {
    char id[33] = { 0 };
    srand((unsigned)(pl_now() * 1e6) ^ (unsigned)(size_t)&id);
    for (int i = 0; i < 32; i++) id[i] = "0123456789abcdef"[rand() % 16];
    rec_set(a, all, "install", v_cstr(a, id));
  }
  const char *name = *text_or(all, "name", "") ? text_or(all, "name", "") : getenv("HOSTNAME") ? getenv("HOSTNAME") : getenv("COMPUTERNAME") ? getenv("COMPUTERNAME") : "Server";
  double status = 0;
  buf_str(&body, "{\"name\":");
  buf_json_str(&body, name, strlen(name));
  buf_str(&body, ",\"install\":");
  buf_json_str(&body, text_or(all, "install", ""), strlen(text_or(all, "install", "")));
  buf_chr(&body, '}');
  val *start = post(a, "/connect", body.s, &status, &error);
  if (start && status != 201) buf_fmt(&error, "%s", text_or(start, "error", "Onsay didn't start the sign in"));
  if (error.n) { core_say("Signing in didn't start: %s", error.s); return 1; }
  core_say("Open %s on any device, sign in, and press Connect. Your code is %s", text_or(start, "url", ""), text_or(start, "code", ""));

  body.n = 0;
  buf_str(&body, "{\"poll\":");
  buf_json_str(&body, text_or(start, "poll", ""), strlen(text_or(start, "poll", "")));
  buf_chr(&body, '}');
  val *done = NULL;
  for (double until = pl_now() + 600; !done && pl_now() < until;) {
    pl_sleep(2);
    error.n = 0;
    val *poll = post(a, "/connect/poll", body.s, &status, &error);
    if (!poll || !strcmp(text_or(poll, "status", ""), "waiting")) continue;   // a moment without the web is waited out too
    if (strcmp(text_or(poll, "status", ""), "done") || !*text_or(poll, "token", "")) {
      core_say("Signing in didn't finish: %s", text_or(poll, "error", "Onsay sent no token"));
      return 1;
    }
    done = poll;
  }
  if (!done) { core_say("The code ran out before Connect was pressed. Run onsay-core %s sign-in again", path); return 1; }
  rec_set(a, all, "token", rec_get(done, "token"));
  buf_json(&text, all);
  if (!pl_write(path, text.s, text.n)) { core_say("Signed in, but the settings file %s couldn't be written", path); return 1; }
  val *account = rec_get(done, "account");
  core_say("Signed in as %s (%s). The token is in %s", text_or(account, "handle", ""), text_or(account, "email", ""), path);
  buf_free(&text), buf_free(&body), buf_free(&error);
  arena_free(a);
  return 0;
}

////////// MAIN //////////

// What each verb takes, by name, in order. The last takes the rest of the words, joined by spaces, or for ceiling as a
// list, and set's value is read as JSON when it is JSON.
static const char *TAKES[][4] = { { "start", "lingo" }, { "stop", "lingo" }, { "share", "lingo", "who" }, { "grant", "lingo", "need" }, { "ceiling", "needs" },
                                  { "policy" }, { "settings", "lingo" }, { "set", "lingo", "key", "value" } };

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "onsay.json", *verb = argc > 2 && strcmp(argv[2], "--sent") ? argv[2] : NULL;
  size_t k = 0, count = sizeof TAKES / sizeof *TAKES;
  while (verb && k < count && strcmp(TAKES[k][0], verb)) k++;
  if (verb && k == count && strcmp(verb, "pack") && strcmp(verb, "sign-in")) {
    core_say("onsay-core %s %s isn't something it does. It does pack, start, stop, share, grant, ceiling, policy, settings, set and sign-in", path, verb);
    return 1;
  }
  showing = argc > 2 && !verb;
  buf text = { 0 };
  if (!pl_read(path, &text)) { core_say("Can't read the settings file %s", path); return 1; }
  const core_settings *s = core_read(text.s, text.n);
  buf_free(&text);
  if (!s) return 1;
  if (verb && !strcmp(verb, "sign-in")) return sign_in(path, s);
  gate = pl_gate_new();
  if (!core_begin(send_, ask_, wait_)) return 1;
  if (verb && !strcmp(verb, "pack")) {
    if (argc < 4) { core_say("Say which folder, like pack lingos/weather"); return 1; }
    return core_pack(argv[3], argc > 4 ? argv[4] : NULL);
  }
  if (verb) {
    arena *a = arena_new();
    val *values = v_rec(a), *done;
    const char *why = NULL;
    int names = 1, last = argc;
    while (names < 4 && TAKES[k][names]) names++;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--route") && i + 1 < argc) { rec_set(a, values, "route", v_cstr(a, argv[i + 1])); last = i; break; }
    for (int i = 1; i < names && 2 + i < last; i++) {
      buf rest = { 0 };
      val *list = v_list(a), *v, *parsed;
      for (int j = 2 + i; j < (i == names - 1 ? last : 3 + i); j++) { if (rest.n) buf_chr(&rest, ' '); buf_str(&rest, argv[j]); list_push(a, list, v_cstr(a, argv[j])); }
      v = !strcmp(TAKES[k][i], "needs") ? list : v_text(a, rest.s, rest.n);
      if (!strcmp(TAKES[k][i], "value") && (parsed = json_parse(a, rest.s, rest.n, NULL))) v = parsed;
      rec_set(a, values, TAKES[k][i], v);
      buf_free(&rest);
    }
    if (!(done = core_control(a, verb, values, &why))) { core_say("%s", why); return 1; }
    buf shown = { 0 };
    buf_json(&shown, done);
    buf_chr(&shown, '\n');
    fwrite(shown.s, 1, shown.n, stdout);
    return 0;
  }
  // The port, and the connection to Onsay when there is a token.
  port = s->port, outside = core_look();
  pl_socket *listening = listen_(outside, 1);
  if (listening) pl_thread(listener, listening);
  if (s->token) { pl_thread(connection, NULL); pl_thread(heartbeat, NULL); }
  else if (!listening) return 1;
  else core_say("The settings have no token, so it runs bare, with no connection to Onsay");
  jobs();
  return 0;
}
