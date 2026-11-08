// agent.c - the agent loop at /onsay, its sessions, and the screens lingos draw on, for every distribution that wants
// them: added with the onsay lingo's own calls (base_calls), reaching Onsay through the lingo client's link (lingo.h,
// Onsay).
//
// 1. Turns      a prompt being worked through, its steps, the screens and far streams kept with it, and what Onsay
//               says on its way to a decision
// 2. /onsay     prompts, with the prompt key, each worked through as the agent loop, and sessions: the whole log,
//               what a prompt is doing as it runs, and the person's answers to what it asks
// 3. Screens    what lingos draw on, screen.open, show and close, each kept with its to-do, and streamed to anything
//               watching /onsay/screens
// 4. Starting   the prompt key, held prompts run again, and agent_calls(), as the lingo client starts
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lingo.h"

#define say core_say
#define STEPS 100   // the newest steps of a prompt kept while it runs
#define TURNS 20   // the newest turns of a session sent with a prompt, as chat messages
#define MOST_CALLS 8   // the most calls one prompt makes before it stops
#define MOST_SCREENS 32

////////// 1. TURNS //////////

/* A prompt being worked through (2): its session, its steps, a line of {text, depth} each, its own and those Onsay says
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

static pl_gate *lock;   // keeps everything below, which the link's thread, the resume tick and the job thread share
static int holding = 1;   // a prompt may be held for a device (serve), so the tick asks /resume once a minute
static turn *turns_;
// The screens lingos draw on, section 3.
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
static void far_opened(const char *session, val *shown, const char *here, buf *gone, buf *opened, buf *watch);
static void far_check(buf *unwatch);   // streams with no screen left to watch, ended

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

/* What Onsay says of a turn's decision, or of a lingo(...) asked for it, before its answer: a step, which may bring a
   screen another computer opened for it (far_opened). */
static void turn_heard(void *arg, const char *text, size_t n) {
  turn *t = arg;
  arena *a = arena_new();
  val *m = json_parse(a, text, n, NULL);
  val *depth = rec_get(m, "depth");
  val *shown = rec_get(m, "screen");
  buf here = { 0 };
  buf gone = { 0 };
  buf made = { 0 };
  buf watch = { 0 };
  if (strcmp(text_or(m, "type", ""), "step")) {
    arena_free(a);
    return;
  }
  onsay_name(&here);
  pl_enter(lock);
  step(t, text_or(m, "text", ""), depth && depth->k == V_NUM ? depth->n : 0);
  if (shown && shown->k == V_REC) far_opened(t->session, shown, here.s ? here.s : "", &gone, &made, &watch);
  pl_leave(lock);
  screen_push(&gone);
  screen_push(&made);
  if (watch.n) onsay_send(watch.s, watch.n);
  buf_free(&watch);
  buf_free(&here);
  arena_free(a);
}

/* The turn of the to-do a request works for: one a call of its loop handed in here (calling), or NULL. Inside the
   lock. */
static turn *turn_of(const char *job) {
  const char *session = NULL;
  for (int i = 0; job && i < 16 && !session; i++)
    if (calling[i].id[0] && !strcmp(calling[i].id, job)) session = calling[i].session;
  for (turn *t = turns_; session && t; t = t->next)
    if (!strcmp(t->session, session)) return t;
  return NULL;
}

/* Who hears what Onsay says of a lingo(...) or use(...) the request job asks: its to-do's turn, so the steps of what it
   reaches, and a screen opened for it, come back to it. NULL when it works for no turn. */
static onsay_heard follows(const char *job, void **arg) {
  pl_enter(lock);
  turn *t = turn_of(job);
  pl_leave(lock);
  *arg = t;
  return t ? turn_heard : NULL;
}

// A message of Onsay's the link doesn't know: a frame of a far computer's stream this computer watches. NULL as the link
// closes, which ends every far stream.
static int heard(const char *text, size_t n) {
  arena *a = arena_new();
  if (!text) {
    for (int i = 0; i < 8; i++) {
      pl_enter(lock);
      long gone = watching[i].id;
      pl_leave(lock);
      if (gone) far_heard(v_num(a, (double)gone), "", 1);
    }
    arena_free(a);
    return 1;
  }
  val *m = json_parse(a, text, n, NULL);
  val *id = rec_get(m, "id");
  val *closed = rec_get(m, "closed");
  int frame = !strcmp(text_or(m, "type", ""), "frame") && id && id->k == V_NUM;
  if (frame) far_heard(id, text_or(m, "text", ""), closed && truthy(closed));
  arena_free(a);
  return frame;
}

// A request this lingo hands itself, /onsay/resume and the prompts it runs again: whoever asked follows them in the list.
static void ignored(void *arg, const char *response, size_t n) {}

////////// 2. /ONSAY //////////

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
    val *onsay_asked = rec_get(t, "questions");
    val *added = rec_get(t, "messages");
    buf lines = { 0 };
    for (size_t k = 0; onsay_asked && onsay_asked->k == V_LIST && k < onsay_asked->l.n; k++)
      buf_fmt(&lines, "%s%s", k ? "\n" : "",
          onsay_asked->l.v[k]->k == V_TEXT ? onsay_asked->l.v[k]->t.s : text_or(onsay_asked->l.v[k], "question", ""));
    for (int assistant = 0; assistant < (t ? 2 : 1); assistant++) {
      if (assistant && added && added->k == V_LIST) {
        for (size_t k = 0; k < added->l.n; k++) list_push(a, out, added->l.v[k]);
        continue;
      }
      val *m = v_rec(a);
      val *said = prompt;
      if (t && !assistant) said = rec_get(t, "prompt");
      if (t && assistant) said = onsay_asked ? v_text(a, lines.s ? lines.s : "", lines.n) : rec_get(t, "text");
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
static val *onsay_asked(run *r, turn *t, val *questions) {
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
  onsay_name(&here);
  buf_chr(&here, '/');
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
    val *got = onsay_exchange("lingo", request.s, request.n, r->stop, 600, &reply, turn_heard, t)
        ? json_parse(a, reply.s, reply.n, NULL)
        : NULL;
    if (got && !rec_get(got, "error"))
      value = rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
    else if (got)
      buf_str(why, text_or(got, "error", ""));
    else
      buf_str(why, *r->stop ? "Stopped by the person" : "This computer isn't connected to Onsay now");
    // A device that is off, or Onsay out of reach from here, holds the turn until it is back (serve).
    int linked = onsay_ready();
    pl_enter(lock);
    if (got && !strcmp(text_or(got, "code", ""), "offline")) {
      t->held = 1;
      snprintf(t->waiting, sizeof t->waiting, "%.*s", (int)strcspn(address, "./"), address);
    } else if (!got && !*r->stop && !linked)
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
                                       WebSocket, each change after (section 3). Another computer of the account
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
      val *onsay_kept = store(r, "store.get", keys->l.v[i], NULL);
      val *wait_ = NULL;
      val *turns, *last;
      onsay_kept = onsay_kept->k == V_TEXT ? json_parse(a, onsay_kept->t.s, onsay_kept->t.len, NULL) : NULL;
      turns = rec_get(onsay_kept, "turns");
      last = turns && turns->k == V_LIST && turns->l.n ? turns->l.v[turns->l.n - 1] : NULL;
      for (size_t k = 0; k < held->l.n; k++)
        if (!strcmp(held->l.v[k]->t.s + 14, session)) {
          wait_ = store(r, "store.get", held->l.v[k], NULL);
          wait_ = wait_->k == V_TEXT ? json_parse(a, wait_->t.s, wait_->t.len, NULL) : NULL;
        }
      val *first = last ? rec_get(turns->l.v[0], "prompt") : rec_get(wait_, "prompt");
      rec_set(a, one, "session", v_cstr(a, session));
      rec_set(a, one, "prompt", first ? first : NOTHING);
      if (rec_get(onsay_kept, "from")) rec_set(a, one, "from", rec_get(onsay_kept, "from"));
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
      said = onsay_api(a, "GET", "/lingos", NULL, &status, &error);
      buf_free(&error);
    }
    computers = rec_get(said, "computers");
    for (size_t i = 0; said && i < held->l.n; i++) {
      val *onsay_kept = store(r, "store.get", held->l.v[i], NULL);
      onsay_kept = onsay_kept->k == V_TEXT ? json_parse(a, onsay_kept->t.s, onsay_kept->t.len, NULL) : NULL;
      const char *onsay_device = text_or(onsay_kept, "device", "");
      int back = !*onsay_device;
      for (size_t k = 0; computers && computers->k == V_LIST && k < computers->l.n; k++)
        if (!strcmp(text_or(computers->l.v[k], "name", ""), onsay_device) && truthy(rec_get(computers->l.v[k], "online")))
          back = 1;
      if (!onsay_kept || !back) continue;
      val *body = v_rec(a);
      buf request = { 0 };
      buf json_ = { 0 };
      rec_set(a, body, "session", v_cstr(a, held->l.v[i]->t.s + 14));
      rec_set(a, body, "prompt", rec_get(onsay_kept, "prompt") ? rec_get(onsay_kept, "prompt") : NOTHING);
      rec_set(a, body, "mode", v_cstr(a, text_or(onsay_kept, "mode", "run")));
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
    int onsay_waiting = 0;
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
    } else if ((onsay_waiting = t && t->asks.n)) {
      t->asks.n = t->answers.n = 0;
      buf_json(&t->answers, given);
      lingo_done(&t->answered);
    }
    pl_leave(lock);
    val *got;
    if (live)
      got = json_parse(a, said.s, said.n, NULL);
    else if (onsay_waiting)
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
  val *onsay_kept;
  val *turns;
  buf_free(&whole);
  onsay_kept = store(r, "store.get", where, NULL);
  onsay_kept = onsay_kept->k == V_TEXT ? json_parse(a, onsay_kept->t.s, onsay_kept->t.len, NULL) : NULL;
  turns = rec_get(onsay_kept, "turns");
  if (!turns || turns->k != V_LIST) turns = v_list(a);
  rec_set(a, out, "session", v_cstr(a, id));
  if (!strcmp(method, "GET")) {
    if (!onsay_kept) return answer(r, 404, "There's no session by that name here");
    rec_set(a, out, "turns", turns);
    return out;
  }
  val *prompt = rec_get(json, "prompt");
  val *from = onsay_kept ? rec_get(onsay_kept, "from") : rec_get(json, "from");
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
    if (k > MOST_CALLS) {
      reply = "That needed more steps than I take at once, so I stopped here. Ask again to go on.";
      break;
    }
    buf_str(&m, "{\"messages\":");
    buf_json(&m, log);
    buf_str(&m, choosing ? ",\"mode\":\"list\"}" : "}");
    d = onsay_exchange("decide", m.s, m.n, r->stop, 150, &said, turn_heard, &t) ? json_parse(a, said.s, said.n, NULL) : NULL;
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
      picked = onsay_asked(r, &t, questions);
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
  if (!reached && !*r->stop && !onsay_ready()) t.held = 1;
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

////////// 3. SCREENS //////////

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
    if (end > at) onsay_send(b->s + at, end - at);
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
  buf here = { 0 };
  int k = 0;
  onsay_name(&here);
  pl_enter(lock);
  turn *onsay_mine = turn_of(job);
  if (onsay_mine) snprintf(session, sizeof session, "%s", onsay_mine->session);
  for (int i = 0; i < MOST_SCREENS; i++)
    if (screens[i].id && !strcmp(screens[i].session, session) && !strncmp(screens[i].title, title, sizeof screens[i].title - 1)) {
      long id = screens[i].id;
      pl_leave(lock);
      buf_free(&here);
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
    buf_json_str(&told, here.s ? here.s : "", here.n);
    buf_fmt(&told, ",\"screen\":%ld,\"title\":", id);
    buf_json_str(&told, title, strlen(title));
    buf_str(&told, "}}");
  }
  screens_json(&said, k);
  pl_leave(lock);
  screen_push(&said);
  if (told.n) onsay_send(told.s, told.n);
  buf_free(&told);
  buf_free(&here);
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
static void far_opened(const char *session, val *shown, const char *here, buf *gone, buf *opened, buf *watch) {
  const char *at = text_or(shown, "at", "");
  val *far = rec_get(shown, "screen");
  int k = 0;
  int w = 0;
  if (!*at || strlen(at) >= sizeof watching[0].at || !far || far->k != V_NUM) return;
  // Opened on this computer for one of its own to-dos, through Onsay, as a use(...) does: the screen here is that
  // to-do's, never a copy of it watched back through Onsay.
  if (!strcmp(at, here)) {
    int onsay_mine = screen_at(far->n);
    if (onsay_mine < 0 || screens[onsay_mine].session[0]) return;
    snprintf(screens[onsay_mine].session, sizeof screens[onsay_mine].session, "%s", session);
    screens_json(opened, onsay_mine);
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
  watching[w].id = onsay_id();
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
    val *onsay_mine = NULL;
    for (size_t j = 0; list && list->k == V_LIST && j < list->l.n && !onsay_mine; j++) {
      val *n = rec_get(list->l.v[j], "screen");
      if (n && n->k == V_NUM && (long)n->n == screens[i].far) onsay_mine = list->l.v[j];
    }
    val *draw = rec_get(onsay_mine, "draw");
    buf items = { 0 };
    if (draw && draw->k == V_LIST) {
      buf_json(&items, draw);
      items.n = items.n >= 2 ? items.n - 1 : 0;   // without its brackets
    }
    if (closed || (!onsay_mine && !watching[w].heard) || (onsay_mine && rec_get(onsay_mine, "closed")))
      screen_gone(i, &said);
    else if (onsay_mine) {
      snprintf(screens[i].title, sizeof screens[i].title, "%s", text_or(onsay_mine, "title", screens[i].title));
      if (!screen_took(i, items.n ? items.s + 1 : "", items.n ? items.n - 1 : 0, truthy(rec_get(onsay_mine, "fresh") ? rec_get(onsay_mine, "fresh") : NOTHING), &said))
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

////////// 4. STARTING //////////

// Every minute, once Onsay is reached, the prompts held for a device that is off run again (serve, /resume).
static void tick(void *arg) {
  for (;;) {
    pl_sleep(60);
    arena *a = arena_new();
    pl_enter(lock);
    int resume = holding;
    pl_leave(lock);
    val *key_ = lingo_setting(a, "onsay", "prompt_key");
    if (resume && onsay_ready()) {
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

// As the onsay lingo starts in a lingo client that serves: a prompt key made when there is none, and the tick.
static void started(void) {
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
  pl_thread(tick, NULL);
}

// Called once as the lingo client starts, from base_calls.
void agent_calls(void) {
  static const onsay_listener listener = { started, heard, follows };
  lock = pl_gate_new();
  onsay_listen(&listener);
  phrase_add("onsay", "store",
      "serve(request: record): /onsay, where prompts come in with this computer's prompt key, each answered by Onsay, "
      "and the sessions they make",
      serve);
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
}
