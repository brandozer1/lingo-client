// web.c - the web lingo: http.send, one web request to any address, through the computer's own web stack (pl_http), so
// proxies and certificates work as for any program there. A lingo built into a floor that wants the web (LINGOS in
// ../../core/build.ps1), never a part of the floor itself. Each request goes in the floor's log (lingo_called).
//
// 1. send(r, a)       http.send(method, url, headers, body, timeout)
// 2. web_calls()      its calls, added as the floor starts
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

////////// 2. WEB_CALLS //////////

// 2.
void web_calls(void) {
  phrase_add("http", "net",
      "send(method: text, url: url, headers: record=nothing, body: text=nothing, timeout: number=30) -> {status: whole, headers: record, text: text}: one request, "
      "headers a record, body text, redirects followed, at most 4 MB back, with bytes in place of text for a file",
      send);
}
