// host.c - the lingo client as a program of its own, on a device whose platform.h has threads and a port.
//
//   onsay-core settings.json                 runs it: its port, and its lingos, any of which may connect it to more,
//                                            like the onsay lingo to Onsay
//   onsay-core settings.json <verb> ...      one thing done, whether or not it runs, its answer printed as JSON:
//     pack <folder> [<to>]                   a folder of a lingo's files made into its .lingo file
//     start <lingo>, stop <lingo>            a lingo running or stopped
//     ceiling <needs...>                     the most this computer ever allows
//     policy                                 the policy in force
//     settings <lingo>                       its settings, each with its value now
//     set <lingo> <key> <value>              one changed. value is JSON, or text as it is when it isn't JSON
//     manifest <lingo>                       its whole OpenAPI manifest, or why it can't load
//     call <lingo.function> [values...]      a function of a lingo, or a phrase, run here, like onsay.sign_in. Each
//                                            value is JSON, or text as it is when it isn't JSON
//
// Threads: main (runs queued requests one at a time, in order, and looks at the lingos folder), listener (takes each
// connection to the port), and one for each web request (reads it, queues it, and writes its answer, or keeps it open
// as a stream). A native lingo
// hands in requests from its own threads (lingo_hand). A request that waits, on lingo(...) or the person's answer, waits
// on main, answering the requests that come meanwhile (lingo_wait), and so does one in a command, a web request or a
// pause. A crash under a phrase ends only its request, with an error (pl_faults in run.c).
#include "core.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD (64 << 10)   // the most a web request's first line and headers may be
#define ROOM (4 << 20)   // and its body

// One request for main: its text, its caller (NULL for the port, its ip in it), and who gets its answer.
typedef struct message {
  struct message *next;
  char *text, *caller;
  size_t n;
  void (*answered)(void *arg, const char *response, size_t n);
  void *arg;
} message;

static pl_gate *gate;
static pl_gate *watch;   // keeps watchers, and each frame whole on its connection
static message *first, *last;
static long requests;   // web requests taken from the port, for their ids
static int port, outside;   // the port, and whether it is open to other computers, as core_look says
static char outermost[128];   // the id, as JSON, of the request main answers now, with every other waiting inside it

////////// JOBS //////////

// One request answered, on main. On Android each is written to the log as it starts and ends, but the app's polls, so
// one that never ends shows there, since nobody there sees the program's own window.
static void handle(message *m) {
  buf out = { 0 };
#ifdef __ANDROID__
  arena *a = arena_new();
  val *request = json_parse(a, m->text, m->n, NULL);
  const char *path = request ? text_or(request, "path", "") : "";
  const char *method = request ? text_or(request, "method", "") : "";
  int logged = strncmp(path, "/.lingo/asks", 12) && !strstr(path, "/live");
  double began = pl_now();
  if (logged) core_say("answering %s %s", method, path);
#endif
  core_serve(m->text, m->n, m->caller, &out);
#ifdef __ANDROID__
  if (logged) core_say("answered %s %s in %.1f seconds", method, path, pl_now() - began);
  arena_free(a);
#endif
  m->answered(m->arg, out.s, out.n);
  buf_free(&out);
  free(m->text);
  free(m->caller);
  free(m);
}

static void queue(
    char *text, size_t n, const char *caller, void (*answered)(void *arg, const char *response, size_t n), void *arg) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, caller ? strdup(caller) : NULL, n, answered, arg };
  pl_enter(gate);
  if (last)
    last->next = m;
  else
    first = m;
  last = m;
  pl_wake(gate);
  pl_leave(gate);
}

// The next request, waiting up to seconds for one, or NULL.
static message *next(double seconds, volatile int *done, volatile int *stop) {
  pl_enter(gate);
  for (double until = pl_now() + seconds; !first && !(done && *done) && !(stop && *stop) && pl_now() < until;)
    pl_wait(gate, until - pl_now() < 1 ? until - pl_now() : 1);
  message *m = done && *done ? NULL : first;
  if (m) {
    first = m->next;
    if (!first) last = NULL;
  }
  pl_leave(gate);
  return m;
}

// lingo.h: a request from elsewhere, copied, in the queue.
void lingo_hand(const char *request, size_t n, const char *caller,
    void (*answered)(void *arg, const char *response, size_t n), void *arg) {
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
      double began = pl_now();
      int wide = core_look();
      if (pl_now() - began > 2) core_say("looking at the lingos folder took %.1f seconds", pl_now() - began);
      pl_enter(gate);
      outside = wide;
      pl_leave(gate);
      look = pl_now() + CORE_LOOK;
    }
    if (!m) continue;
    arena *a = arena_new();
    val *request = json_parse(a, m->text, m->n, NULL);
    val *id = request ? rec_get(request, "id") : NULL;
    buf said = { 0 };
    if (id) buf_json(&said, id);
    pl_enter(gate);
    snprintf(outermost, sizeof outermost, "%s", said.s ? said.s : "");
    pl_leave(gate);
    buf_free(&said);
    arena_free(a);
    handle(m);
    pl_enter(gate);
    outermost[0] = 0;
    pl_leave(gate);
  }
}

#ifdef __ANDROID__
// Stops everything running, from any thread. On Android the core lives inside the app, so its Stop can't start the
// program again the way Desktop's does.
void host_stop(void) {
  char id[sizeof outermost];
  pl_enter(gate);
  snprintf(id, sizeof id, "%s", outermost);
  pl_leave(gate);
  if (id[0]) lingo_stop(id);
}
#endif

////////// STREAMS //////////

/* A web request asking to be a WebSocket, a GET with Upgrade: websocket, is answered like any other first, so every
   check applies, and on a 200 it stays open: its body is the first message, then each the lingo at its path pushes
   (lingo_push), until either side closes it. What the other side sends is not read, but its close. */
typedef struct watcher {
  struct watcher *next;
  pl_socket *c;
  char path[256];
} watcher;
static watcher *watchers;

// One frame, unmasked, as a server sends it: text, opcode 1, or a ping, 9. Under watch.
static int frame(pl_socket *c, int opcode, const char *text, size_t n) {
  unsigned char head[10];
  size_t k = 2;
  head[0] = (unsigned char)(0x80 | opcode);
  if (n < 126)
    head[1] = (unsigned char)n;
  else if (n < 65536) {
    head[1] = 126;
    head[2] = (unsigned char)(n >> 8);
    head[3] = (unsigned char)n;
    k = 4;
  } else {
    head[1] = 127;
    for (int i = 0; i < 8; i++) head[2 + i] = (unsigned char)((unsigned long long)n >> (56 - 8 * i));
    k = 10;
  }
  return pl_give(c, (const char *)head, k) && (!n || pl_give(c, text, n));
}

// lingo.h
void lingo_push(const char *path, const char *text, size_t n) {
  pl_enter(watch);
  for (watcher *w = watchers; w; w = w->next)
    if (!strcmp(w->path, path)) frame(w->c, 1, text, n);
  pl_leave(watch);
}

// On the visitor's thread, until it closes: a quiet 10 seconds is asked about with a ping, and nothing coming back at
// once means it closed.
static void stream(pl_socket *c, const char *path, size_t n, const char *key, const char *first, size_t size) {
  buf joined = { 0 };
  buf accept = { 0 };
  buf head = { 0 };
  unsigned char digest[20];
  char chunk[4096];
  watcher w = { NULL, c, "" };
  size_t len = 0;
  while (len < n && path[len] != '?') len++;
  snprintf(w.path, sizeof w.path, "%.*s", (int)len, path);
  buf_fmt(&joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
  sha1((const unsigned char *)joined.s, joined.n, digest);
  base64(&accept, digest, sizeof digest);
  buf_fmt(&head,
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n\r\n", accept.s);
  pl_enter(watch);
  int open = pl_give(c, head.s, head.n) && frame(c, 1, first, size);
  if (open) {
    w.next = watchers;
    watchers = &w;
  }
  pl_leave(watch);
  for (int got; open;) {
    double began = pl_now();
    if ((got = pl_take(c, chunk, sizeof chunk)) > 0) {
      open = (chunk[0] & 0x0f) != 8;
      continue;
    }
    pl_enter(watch);
    open = pl_now() - began > 1 && frame(c, 9, "", 0);
    pl_leave(watch);
  }
  pl_enter(watch);
  for (watcher **at = &watchers; *at; at = &(*at)->next)
    if (*at == &w) {
      *at = w.next;
      break;
    }
  pl_leave(watch);
  buf_free(&joined);
  buf_free(&accept);
  buf_free(&head);
}

////////// THE PORT //////////

// An answer written to a web request's connection: its status, its headers by lower case name, and its body.
static void respond(pl_socket *c, int status, val *h, const char *body, size_t n) {
  buf out = { 0 };
  buf_fmt(&out, "HTTP/1.1 %d \r\n", status);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    const char *name = h->r.key[i];
    const char *value = h->r.v[i]->k == V_TEXT ? h->r.v[i]->t.s : "";
    // Its length and closing are written below, and a line break would start a header of its own.
    if (!*name || strpbrk(name, "\r\n: ") || strpbrk(value, "\r\n") || !strcmp(name, "content-length") ||
        !strcmp(name, "connection") || !strcmp(name, "transfer-encoding"))
      continue;
    buf_fmt(&out, "%s: %s\r\n", name, value);
  }
  buf_fmt(&out, "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
  buf_add(&out, body, n);
  pl_give(c, out.s, out.n);
  buf_free(&out);
}

typedef struct {
  buf response;
  int done;
} answer;   // a web request's answer, which its thread waits for

static void answered(void *arg, const char *response, size_t n) {
  answer *to = arg;
  buf_add(&to->response, response, n);
  lingo_done(&to->done);
}

typedef struct {
  pl_socket *c;
  buf from;
} visit;

/* One web request to the port, on its own thread: its first line, headers and a body of Content-Length, read whole,
   answered on main in its turn (core_serve), and closed. One that isn't a web request is answered here with why, and
   one that went quiet before its headers ended is closed. */
static void visitor(void *arg) {
  visit v = *(visit *)arg;
  free(arg);
  arena *a = arena_new();
  buf in = { 0 };
  char chunk[16384];
  const char *end = NULL;
  const char *bad = NULL;
  int status = 400;
  int got = 1;
  size_t method = 0;
  size_t path = 0;
  size_t size = 0;
  size_t from = 0;
  val *headers = v_rec(a);
  while (got && !(end = in.s ? strstr(in.s, "\r\n\r\n") : NULL) && in.n < HEAD)
    if ((got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
  if (!end && in.n >= HEAD) {
    status = 431;
    bad = "Its headers are more than 64 KB";
  } else if (end) {
    method = strspn(in.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (method && method < 16 && in.s[method] == ' ' && in.s[method + 1] == '/') {
      path = strcspn(in.s + method + 1, " \r\n");
    }
    const char *version = in.s + method + 1 + path;
    if (!path || strncmp(version, " HTTP/1.", 8) || (version[8] != '0' && version[8] != '1') ||
        strncmp(version + 9, "\r\n", 2) || !utf8_ok(in.s, (size_t)(end - in.s)))
      bad = "That isn't a web request. Its first line is like GET /memo HTTP/1.1";
    for (const char *s = strstr(in.s, "\r\n") + 2; !bad && s < end; s = strstr(s, "\r\n") + 2) {
      const char *eol = strstr(s, "\r\n");
      const char *colon = memchr(s, ':', (size_t)(eol - s));
      const char *value = colon;
      const char *stop = eol;
      if (!colon || colon == s || memchr(s, ' ', (size_t)(colon - s))) {
        bad = "A header isn't a name, a colon and its value";
        break;
      }
      while (++value < eol && (*value == ' ' || *value == '\t')) {}
      while (stop > value && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
      header_add(a, headers, s, (size_t)(colon - s), value, (size_t)(stop - value));
    }
    const char *length = text_or(headers, "content-length", "0");
    if (!bad && rec_get(headers, "transfer-encoding")) {
      status = 411;
      bad = "Send its body with a Content-Length, not in chunks";
    } else if (!bad && (!*length || strspn(length, "0123456789") != strlen(length)))
      bad = "Its Content-Length isn't a number";
    else if (!bad && (strlen(length) > 9 || (size = strtoul(length, NULL, 10)) > ROOM)) {
      status = 413;
      bad = "Its body is more than 4 MB";
    }
    from = (size_t)(end - in.s) + 4;
    if (!bad && !strcmp(text_or(headers, "expect", ""), "100-continue") && in.n - from < size)
      pl_give(v.c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (!bad && in.n - from < size && (got = pl_take(v.c, chunk, sizeof chunk))) buf_add(&in, chunk, (size_t)got);
    if (!bad && in.n - from < size)
      bad = "Its body was shorter than its Content-Length";
    else if (!bad && !utf8_ok(in.s + from, size))
      bad = "Its body isn't text, and lingos take text, like JSON";
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
    const char *ip = v.from.s ? v.from.s : "";
#ifdef __ANDROID__
    // Every app on a phone can reach its 127.0.0.1, so only Onsay's own, sending the key it gave the core, is this phone.
    const char *key = getenv("ONSAY_APP_KEY");
    if (!key || !*key || strcmp(text_or(headers, "x-onsay-app", ""), key)) ip = "another app";
#endif
    pl_enter(gate);
    buf_fmt(&msg, "{\"id\":\"port %ld\",\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"ip\":");
    buf_json_str(&msg, ip, strlen(ip));
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_chr(&msg, '}');
    queue(msg.s, msg.n, NULL, answered, &to);   // the queue owns the text now
    pl_enter(gate);
    while (!to.done) pl_wait(gate, 1);
    pl_leave(gate);
    val *r = json_parse(a, to.response.s ? to.response.s : "", to.response.n, NULL);
    val *status_ = rec_get(r, "status");
    val *body = rec_get(r, "body");
    const char *key = text_or(headers, "sec-websocket-key", "");
    int ok = status_ && status_->k == V_NUM && status_->n == 200;
    if (ok && *key && method == 3 && !strncmp(in.s, "GET", 3) && strstr(text_or(headers, "upgrade", ""), "ebsocket"))
      stream(v.c, in.s + method + 1, path, key, body ? body->t.s : "", body ? body->t.len : 0);
    else
      respond(v.c, status_ && status_->k == V_NUM ? (int)status_->n : 500, rec_get(r, "headers"), body ? body->t.s : "",
        body ? body->t.len : 0);
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
  if (p)
    core_say("Answering web requests on port %d, %s", port,
        everywhere ? "from other computers too" : "from this computer only");
  else if (say)
    core_say("%s, so it answers no web requests", error.s);
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
      if (!p) {
        pl_sleep(1);
        continue;
      }
    }
    visit *v = calloc(1, sizeof *v);
    if ((v->c = pl_accept(p, &v->from)))
      pl_thread(visitor, v);
    else {
      buf_free(&v->from);
      free(v);
    }
  }
}

////////// MAIN //////////

// What each verb takes, by name, in order. The last takes the rest of the words, joined by spaces, or for ceiling and
// call as a list, and set's value and call's values are read as JSON when they are JSON.
static const char *TAKES[][4] = { { "start", "lingo" }, { "stop", "lingo" }, { "ceiling", "needs" },
  { "policy" }, { "settings", "lingo" }, { "set", "lingo", "key", "value" }, { "manifest", "lingo" },
  { "call", "fn", "values" } };

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "onsay.json";
  const char *verb = argc > 2 ? argv[2] : NULL;
  size_t k = 0;
  size_t count = sizeof TAKES / sizeof *TAKES;
  while (verb && k < count && strcmp(TAKES[k][0], verb)) k++;
  if (verb && k == count && strcmp(verb, "pack")) {
    core_say(
        "onsay-core %s %s isn't something it does. It does pack, start, stop, ceiling, policy, settings, set, manifest and call",
        path, verb);
    return 1;
  }
  buf text = { 0 };
  if (!pl_read(path, &text)) {
    core_say("Can't read the settings file %s", path);
    return 1;
  }
  const core_settings *s = core_read(text.s, text.n);
  buf_free(&text);
  if (!s) return 1;
  gate = pl_gate_new();
  watch = pl_gate_new();
  if (!core_begin(!verb)) return 1;
  if (verb && !strcmp(verb, "pack")) {
    if (argc < 4) {
      core_say("Say which folder, like pack lingos/weather");
      return 1;
    }
    return core_pack(argv[3], argc > 4 ? argv[4] : NULL);
  }
  if (verb) {
    arena *a = arena_new();
    val *values = v_rec(a);
    val *done;
    const char *why = NULL;
    int names = 1;
    while (names < 4 && TAKES[k][names]) names++;
    for (int i = 1; i < names && 2 + i < argc; i++) {
      buf rest = { 0 };
      const char *take = TAKES[k][i];
      val *list = v_list(a);
      val *v;
      val *parsed;
      for (int j = 2 + i; j < (i == names - 1 ? argc : 3 + i); j++) {
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
    if (!(done = core_control(a, verb, values, &why))) {
      core_say("%s", why);
      return 1;
    }
    buf shown = { 0 };
    buf_json(&shown, done);
    buf_chr(&shown, '\n');
    fwrite(shown.s, 1, shown.n, stdout);
    return 0;
  }
  port = s->port;
  outside = core_look();
  pl_thread(listener, listen_(outside, 1));
  jobs();
  return 0;
}
