// web.c - the web lingo: http.send, one web request to any address, and socket, WebSockets, through the computer's own
// web stack (pl_http, pl_connect), so proxies and certificates work as for any program there. A lingo built into a floor
// that wants the web (LINGOS in ../../core/build.ps1), never a part of the floor itself. Each request and each socket
// opened goes in the floor's log (lingo_called).
//
// 1. send(r, a)                            http.send(method, url, headers, body, timeout)
// 2. socket_open, _send, _receive, _close  socket.open(url, headers), send(socket, text), receive(socket, seconds) and
//                                          close(socket)
// 3. web_calls()                           its calls, added as the floor starts
#include <stdlib.h>
#include <string.h>
#include "lingo.h"

////////// 1. SEND //////////

// 1. Its answer {status, headers, text}, or the call's failure with why.
static val *send(run *r, val **a) {
  const char *method = text_of(r, a[0])->t.s;
  const char *url = text_of(r, a[1])->t.s;
  const char *body = a[3]->k == V_NOTHING ? NULL : text_of(r, a[3])->t.s;
  size_t n = body ? strlen(body) : 0;
  double seconds = a[4]->k == V_NUM ? a[4]->n : 30;
  val *headers = a[2]->k == V_REC ? a[2] : v_rec(r->a);
  buf error = { 0 };
  double began = pl_now();
  val *got = pl_http(r->a, method, url, headers, body, n, seconds, &error);
  lingo_called("web", NULL, NULL, method, url, headers, body, n, got, got ? NULL : error.s, 0, (pl_now() - began) * 1000);
  if (!got) {
    const char *why = error.s ? arena_fmt(r->a, "%s", error.s) : "it couldn't be reached";
    buf_free(&error);
    fail(r, "phrase", "%s", why);
  }
  buf_free(&error);
  return got;
}

////////// 2. SOCKETS //////////

/* WebSockets a lingo opens, each read on a thread of its own (talk), every message kept in waiting until socket.receive
   takes it. One stays open until its lingo closes it or the other side does. */
typedef struct sock {
  struct sock *next;
  long id;
  pl_link *l;
  buf waiting;           // each message come and not yet taken: its length, a size_t, then its text
  volatile int news;     // set as each message comes and as it ends, for lingo_wait
  int ended, left;       // ended: closed by the other side. left: closed by its lingo, so talk frees it as it ends
} sock;

static pl_gate *socks_gate;
static sock *socks;   // the open ones
static long socks_made;

static void sock_free(sock *s) {
  pl_free(s->l);
  buf_free(&s->waiting);
  free(s);
}

static void talk(void *arg) {
  sock *s = arg;
  buf in = { 0 };
  while (pl_receive(s->l, &in)) {
    pl_enter(socks_gate);
    buf_add(&s->waiting, (const char *)&in.n, sizeof in.n);
    buf_add(&s->waiting, in.s ? in.s : "", in.n);
    s->news = 1;
    pl_leave(socks_gate);
    in.n = 0;
  }
  buf_free(&in);
  pl_enter(socks_gate);
  s->ended = 1;
  s->news = 1;
  int left = s->left;
  pl_leave(socks_gate);
  if (left) sock_free(s);
}

// Under socks_gate.
static sock *sock_of(run *r, val *id) {
  sock *s = socks;
  while (s && !(id->k == V_NUM && s->id == (long)id->n)) s = s->next;
  if (!s) {
    pl_leave(socks_gate);
    fail(r, "phrase", "No socket %s is open", text_of(r, id)->t.s);
  }
  return s;
}

// 2. Its address ws:// or wss://, and each header one whole line.
static val *socket_open(run *r, val **a) {
  const char *url = text_of(r, a[0])->t.s;
  buf lines = { 0 };
  buf error = { 0 };
  if (strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) fail(r, "phrase", "A WebSocket's address starts with ws:// or wss://");
  for (size_t i = 0; a[1]->k == V_REC && i < a[1]->r.n; i++) {
    const char *value = text_of(r, a[1]->r.v[i])->t.s;
    if (strpbrk(a[1]->r.key[i], "\r\n:") || strpbrk(value, "\r\n")) {
      buf_free(&lines);
      fail(r, "phrase", "The header %s has a line break or a colon in it", a[1]->r.key[i]);
    }
    buf_fmt(&lines, "%s: %s\r\n", a[1]->r.key[i], value);
  }
  double began = pl_now();
  pl_link *l = pl_connect(url, lines.s, &error);
  buf_free(&lines);
  val *got = NULL;
  if (l) {
    got = v_rec(r->a);
    rec_set(r->a, got, "status", v_num(r->a, 101));
  }
  lingo_called("socket", NULL, NULL, "GET", url, v_rec(r->a), NULL, 0, got, l ? NULL : error.s, 0, (pl_now() - began) * 1000);
  if (!l) {
    const char *why = error.s ? arena_fmt(r->a, "%s", error.s) : "It couldn't open";
    buf_free(&error);
    fail(r, "phrase", "%s", why);
  }
  sock *s = calloc(1, sizeof *s);
  s->l = l;
  pl_enter(socks_gate);
  s->id = ++socks_made;
  s->next = socks;
  socks = s;
  long id = s->id;
  pl_leave(socks_gate);
  pl_thread(talk, s);
  return v_num(r->a, (double)id);
}

// 2.
static val *socket_send(run *r, val **a) {
  val *text = text_of(r, a[1]);
  pl_enter(socks_gate);
  sock *s = sock_of(r, a[0]);
  pl_link *l = s->l;
  pl_leave(socks_gate);
  if (!pl_send(l, text->t.s, text->t.len)) fail(r, "phrase", "The socket was closed");
  return NOTHING;
}

// 2. The oldest message waiting, else the next to come within seconds, else nothing.
static val *socket_receive(run *r, val **a) {
  double seconds = a[1]->k == V_NUM && a[1]->n >= 0 && a[1]->n < 3600 ? a[1]->n : 30;
  double until = pl_now() + seconds;
  for (;;) {
    val *got = NULL;
    pl_enter(socks_gate);
    sock *s = sock_of(r, a[0]);
    if (s->waiting.n) {
      size_t n;
      memcpy(&n, s->waiting.s, sizeof n);
      got = v_text(r->a, s->waiting.s + sizeof n, n);
      s->waiting.n -= sizeof n + n;
      memmove(s->waiting.s, s->waiting.s + sizeof n + n, s->waiting.n);
    }
    int ended = s->ended;
    if (!got && !ended) s->news = 0;
    volatile int *news = &s->news;
    pl_leave(socks_gate);
    if (got) return got;
    if (r->stop && *r->stop) fail(r, "stopped", "Stopped by the person");
    if (ended) fail(r, "phrase", "The socket was closed");
    if (pl_now() >= until) return NOTHING;
    lingo_wait(news, r->stop, until - pl_now());
  }
}

// 2. Taken off the open ones; freed now when the other side closed it already, else by talk as it ends.
static val *socket_close(run *r, val **a) {
  pl_enter(socks_gate);
  sock *s = sock_of(r, a[0]);
  sock **at = &socks;
  while (*at != s) at = &(*at)->next;
  *at = s->next;
  s->left = 1;
  int ended = s->ended;
  if (!ended) pl_close(s->l);
  pl_leave(socks_gate);
  if (ended) sock_free(s);
  return NOTHING;
}

////////// 3. WEB_CALLS //////////

// 3.
void web_calls(void) {
  socks_gate = pl_gate_new();
  phrase_add("http", "net",
      "send(method: text, url: url, headers: record=nothing, body: text=nothing, timeout: number=30) -> {status: whole, headers: record, text: text}: one request, "
      "headers a record, body text, redirects followed, at most 4 MB back, with bytes in place of text for a file",
      send);
  phrase_add("socket", "net",
      "open(url: url, headers: record=nothing) -> whole: a WebSocket to a ws:// or wss:// address, its number for the other "
      "socket calls. Open until closed, by the lingo or the other side. Windows 8 or later on Windows",
      socket_open);
  phrase_add("socket", "net", "send(socket: whole, text: text): one message", socket_send);
  phrase_add("socket", "net",
      "receive(socket: whole, seconds: number=30) -> text?: the next message, or nothing when none came in seconds",
      socket_receive);
  phrase_add("socket", "net", "close(socket: whole)", socket_close);
}
