// host.c - the lingo client as a program of its own, on a device whose platform.h has threads and a port.
//
//   onsay-core settings.json                 runs it: its port, and its lingos, any of which may connect it to more,
//                                            like the onsay lingo to Onsay
//   onsay-core settings.json <verb> ...      one thing done, whether or not it runs, its answer printed as JSON:
//     pack <folder> [<to>]                   a folder of a lingo's files made into its .lingo file
//     start <lingo>, stop <lingo>            a lingo running or stopped
//     share <lingo> <who> [--route <r>]      who may reach it: public, me, my devices, a handle, or ip 10.0.0.0/8
//     grant <lingo> <need> [<path>]          what it may do, like run, or files.read and a folder
//     ceiling <needs...>                     the most this computer ever allows
//     policy                                 the policy in force
//     settings <lingo>                       its settings, each with its value now
//     set <lingo> <key> <value>              one changed. value is JSON, or text as it is when it isn't JSON
//     call <lingo.function> [values...]      a function of a lingo, or a phrase, run here, like onsay.sign_in. Each
//                                            value is JSON, or text as it is when it isn't JSON
//
// Threads: main (runs queued requests one at a time, in order, and looks at the lingos folder), listener (takes each
// connection to the port), and one for each web request (reads it, queues it, and writes its answer). A native lingo
// hands in requests from its own threads (lingo_hand). A request that waits, on lingo(...) or the person's answer, waits
// on main, answering the requests that come meanwhile (lingo_wait).
#include "core.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD (64 << 10)   // the most a web request's first line and headers may be
#define ROOM (4 << 20)    // and its body

// One request for main: its text, its caller (NULL for the port, its ip in it), and who gets its answer.
typedef struct message { struct message *next; char *text, *caller; size_t n; void (*answered)(void *arg, const char *response, size_t n); void *arg; } message;

static pl_gate *gate;
static message *first, *last;
static long requests;       // web requests taken from the port, for their ids
static int port, outside;   // the port, and whether it is open to other computers, as core_look says

////////// JOBS //////////

// One request answered, on main.
static void handle(message *m) {
  buf out = { 0 };
  core_serve(m->text, m->n, m->caller, &out);
  m->answered(m->arg, out.s, out.n);
  buf_free(&out);
  free(m->text), free(m->caller), free(m);
}

static void queue(char *text, size_t n, const char *caller, void (*answered)(void *arg, const char *response, size_t n), void *arg) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, caller ? strdup(caller) : NULL, n, answered, arg };
  pl_enter(gate);
  if (last) last->next = m; else first = m;
  last = m;
  pl_wake(gate);
  pl_leave(gate);
}

// The next request, waiting up to seconds for one, or NULL.
static message *next(double seconds, volatile int *done, volatile int *stop) {
  pl_enter(gate);
  for (double until = pl_now() + seconds; !first && !(done && *done) && !(stop && *stop) && pl_now() < until;) pl_wait(gate, until - pl_now() < 1 ? until - pl_now() : 1);
  message *m = done && *done ? NULL : first;
  if (m) { first = m->next; if (!first) last = NULL; }
  pl_leave(gate);
  return m;
}

// lingo.h: a request from elsewhere, copied, in the queue.
void lingo_hand(const char *request, size_t n, const char *caller, void (*answered)(void *arg, const char *response, size_t n), void *arg) {
  char *text = malloc(n + 1);
  memcpy(text, request, n);
  text[n] = 0;
  queue(text, n, caller, answered, arg);
}

// lingo.h
void lingo_stop(const char *id) {
  core_stop(id);
  pl_enter(gate);
  pl_wake(gate);
  pl_leave(gate);
}

// lingo.h: requests that come meanwhile are answered here, inside the one that waits, since what answers may be here.
void lingo_wait(volatile int *done, volatile int *stop, double seconds) {
  for (double until = pl_now() + seconds; !*done && !*stop && pl_now() < until;) {
    message *m = next(until - pl_now(), done, stop);
    if (m) handle(m);
  }
}

// lingo.h
void lingo_done(volatile int *done) {
  pl_enter(gate);
  *done = 1;
  pl_wake(gate);
  pl_leave(gate);
}

// main: every request in turn, and a look at the lingos folder every CORE_LOOK seconds, however busy.
static void jobs(void) {
  for (double look = pl_now() + CORE_LOOK;;) {
    message *m = next(CORE_LOOK, NULL, NULL);
    if (pl_now() >= look) {
      int wide = core_look();
      pl_enter(gate);
      outside = wide;
      pl_leave(gate);
      look = pl_now() + CORE_LOOK;
    }
    if (m) handle(m);
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

typedef struct { buf response; int done; } answer;   // a web request's answer, which its thread waits for

static void answered(void *arg, const char *response, size_t n) {
  answer *to = arg;
  buf_add(&to->response, response, n);
  lingo_done(&to->done);
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
    buf_fmt(&msg, "{\"id\":\"port %ld\",\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"ip\":");
    buf_json_str(&msg, v.from.s ? v.from.s : "", v.from.n);
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_chr(&msg, '}');
    queue(msg.s, msg.n, NULL, answered, &to);   // the queue owns the text now
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

////////// MAIN //////////

// What each verb takes, by name, in order. The last takes the rest of the words, joined by spaces, or for ceiling and
// call as a list, and set's value and call's values are read as JSON when they are JSON.
static const char *TAKES[][4] = { { "start", "lingo" }, { "stop", "lingo" }, { "share", "lingo", "who" }, { "grant", "lingo", "need" }, { "ceiling", "needs" },
                                  { "policy" }, { "settings", "lingo" }, { "set", "lingo", "key", "value" }, { "call", "fn", "values" } };

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "onsay.json", *verb = argc > 2 ? argv[2] : NULL;
  size_t k = 0, count = sizeof TAKES / sizeof *TAKES;
  while (verb && k < count && strcmp(TAKES[k][0], verb)) k++;
  if (verb && k == count && strcmp(verb, "pack")) {
    core_say("onsay-core %s %s isn't something it does. It does pack, start, stop, share, grant, ceiling, policy, settings, set and call", path, verb);
    return 1;
  }
  buf text = { 0 };
  if (!pl_read(path, &text)) { core_say("Can't read the settings file %s", path); return 1; }
  const core_settings *s = core_read(text.s, text.n);
  buf_free(&text);
  if (!s) return 1;
  gate = pl_gate_new();
  if (!core_begin(!verb)) return 1;
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
      const char *take = TAKES[k][i];
      val *list = v_list(a), *v, *parsed;
      for (int j = 2 + i; j < (i == names - 1 ? last : 3 + i); j++) {
        if (rest.n) buf_chr(&rest, ' ');
        buf_str(&rest, argv[j]);
        parsed = !strcmp(take, "values") ? json_parse(a, argv[j], strlen(argv[j]), NULL) : NULL;
        list_push(a, list, parsed ? parsed : v_cstr(a, argv[j]));
      }
      v = !strcmp(take, "needs") || !strcmp(take, "values") ? list : v_text(a, rest.s, rest.n);
      if (!strcmp(take, "value") && (parsed = json_parse(a, rest.s, rest.n, NULL))) v = parsed;
      rec_set(a, values, take, v);
      buf_free(&rest);
    }
    if (!(done = core_control(a, verb, values, &why))) { core_say("%s", why); return 1; }
    buf shown = { 0 };
    buf_json(&shown, done);
    buf_chr(&shown, '\n');
    fwrite(shown.s, 1, shown.n, stdout);
    return 0;
  }
  port = s->port, outside = core_look();
  pl_thread(listener, listen_(outside, 1));
  jobs();
  return 0;
}
