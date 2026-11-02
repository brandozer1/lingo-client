// calls_shared.c - the base calls that are the same plain C on every computer, included by windows.c and linux.c, which
// each supply the functions declared below.
//
// 1. helpers    str, put, put_num, num_or, nap, trim_end, mark_length
// 2. files      read, write, add_to, edit, list, find, search, copy, move, make_folder
// 3. commands   output, and where a started command keeps it
// 4. chat       ask, say, step
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

// What windows, controls and files are, written out where a call gives one (calls_every.h and calls_device.h).
#define WINDOW "{id: whole, app: text, title: text, x: number, y: number, w: number, h: number, focused: bool, minimized: bool}"
#define CONTROL "{id: any, role: text, name: text, value: any, x: number, y: number, w: number, h: number, enabled: bool, focused: bool, can: [text]}"
#define FILE_ "{name: text, path: path, size: number, folder: bool, changed: text}"

#define SEARCH_MOST 5000000   // the biggest file files.search reads, in bytes
#define SEARCH_LINE 300   // the most of a line it gives back, in bytes
#define SEARCH_SNIFF 8000   // how far into a file it looks for a 0 byte, which says the file isn't text

// What each platform supplies. Its SLASH, "/" or "\\", is defined before this file is included.
static val *path_of(run *r, val *p, const char *need);   // a path as the person means it, whole and checked
static void file_failed(run *r, const char *what, const char *path);   // fails, saying why this computer refused
static val *listing(run *r, val **a, int deep);   // a folder's entries, with those inside it when deep
static void copy_or_move(run *r, val **a, int move);
static const char *temp_folder(run *r);   // ends in its SLASH
static void show(buf *line);   // one line of JSON on stdout for an app, then freed

////////// 1. HELPERS //////////

static const char *str(run *r, val *x) { return text_of(r, x)->t.s; }

static void put(run *r, val *rec, const char *key, val *v) { rec_set(r->a, rec, key, v); }

static void put_num(run *r, val *rec, const char *key, double n) { rec_set(r->a, rec, key, v_num(r->a, n)); }

static double num_or(val *x, double fallback) {
  if (x->k == V_NUM) return x->n;
  return fallback;
}

// A wait that a stop ends at once.
static void nap(run *r, double seconds) {
  double until = pl_now() + seconds;
  while (pl_now() < until) {
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    pl_sleep(0.02);
  }
}

// Takes a command's last new lines and spaces off its output.
static void trim_end(buf *out) {
  while (out->n && strchr("\n\r ", out->s[out->n - 1])) {
    out->n--;
    out->s[out->n] = 0;
  }
}

// How many bytes of a byte order mark start the text, which isn't part of it.
static size_t mark_length(const buf *b) {
  if (b->n >= 3 && !memcmp(b->s, "\xEF\xBB\xBF", 3)) return 3;
  return 0;
}

////////// 2. FILES //////////

static val *fs_read(run *r, val **a) {
  val *p = path_of(r, a[0], "files.read");
  buf b = { 0 };
  if (!pl_read(p->t.s, &b)) file_failed(r, "read", p->t.s);
  size_t skip = mark_length(&b);
  val *out = v_text(r->a, b.s ? b.s + skip : "", b.n - skip);
  buf_free(&b);
  return out;
}

// The folders above a path, made.
static void parent_folder(run *r, const char *path) {
  const char *slash = strrchr(path, SLASH[0]);
  if (slash && slash > path) pl_folder(v_text(r->a, path, (size_t)(slash - path))->t.s);
}

static val *fs_write(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  buf b = { 0 };
  if (a[1]->k == V_TEXT)
    buf_add(&b, a[1]->t.s, a[1]->t.len);
  else
    buf_json(&b, a[1]);
  parent_folder(r, p->t.s);
  int ok = pl_write(p->t.s, b.s ? b.s : "", b.n);
  buf_free(&b);
  if (!ok) file_failed(r, "write", p->t.s);
  return NOTHING;
}

static val *fs_append(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  const char *text = str(r, a[1]);
  parent_folder(r, p->t.s);
  if (!pl_append(p->t.s, text, strlen(text))) file_failed(r, "add to", p->t.s);
  return NOTHING;
}

static val *fs_folder(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  if (!pl_folder(p->t.s)) fail(r, "phrase", "can't make the folder %s", p->t.s);
  return NOTHING;
}

// A name against a pattern: * is any run of characters and ? any one, ignoring case.
static int glob(const char *pattern, const char *name) {
  if (!*pattern) return !*name;
  if (*pattern == '*') return glob(pattern + 1, name) || (*name && glob(pattern, name + 1));
  if (!*name) return 0;
  if (*pattern == '?') {
    name++;
    while ((*name & 0xC0) == 0x80) name++;   // the rest of a UTF-8 character
    return glob(pattern + 1, name);
  }
  if (tolower((unsigned char)*pattern) != tolower((unsigned char)*name)) return 0;
  return glob(pattern + 1, name + 1);
}

static val *fs_list(run *r, val **a) { return listing(r, a, 0); }
static val *fs_find(run *r, val **a) { return listing(r, a, 1); }

static val *fs_copy(run *r, val **a) {
  copy_or_move(r, a, 0);
  return NOTHING;
}

static val *fs_move(run *r, val **a) {
  copy_or_move(r, a, 1);
  return NOTHING;
}

// Whether the line of len bytes holds the words, of n bytes, in any case.
static int line_has(const char *line, size_t len, const char *words, size_t n) {
  for (size_t at = 0; at + n <= len; at++) {
    size_t same = 0;
    while (same < n && tolower((unsigned char)line[at + same]) == tolower((unsigned char)words[same])) same++;
    if (same == n) return 1;
  }
  return 0;
}

// Every line of a file's text holding the words goes on out, until it holds most.
static void search_text(run *r, const char *path, const buf *text, const char *words, double most, val *out) {
  size_t n = strlen(words);
  int number = 0;
  const char *line = text->s;
  const char *end = text->s + text->n;
  while (line < end && out->l.n < most) {
    const char *stop = memchr(line, '\n', (size_t)(end - line));
    size_t len = stop ? (size_t)(stop - line) : (size_t)(end - line);
    number++;
    if (line_has(line, len, words, n)) {
      size_t shown = len;
      if (shown && line[shown - 1] == '\r') shown--;
      if (shown > SEARCH_LINE) shown = SEARCH_LINE;
      val *hit = v_rec(r->a);
      put(r, hit, "path", v_cstr(r->a, path));
      put_num(r, hit, "line", number);
      put(r, hit, "text", v_text(r->a, line, shown));
      list_push(r->a, out, hit);
    }
    line += len + 1;
  }
}

// files.search: every line holding the words, in any case, in the text files of a folder and the folders inside it,
// leaving out .git and node_modules, files over SEARCH_MOST bytes and files that aren't text.
static val *fs_search(run *r, val **a) {
  const char *words = str(r, a[1]);
  if (!*words) fail(r, "phrase", "files.search needs the words to look for");
  double most = num_of(r, a[3], "most");
  val *where[2] = { a[0], a[2] };
  val *found = listing(r, where, 1);
  val *out = v_list(r->a);
  for (size_t i = 0; i < found->l.n && out->l.n < most && !*r->stop; i++) {
    val *f = found->l.v[i];
    const char *path = text_or(f, "path", "");
    if (truthy(rec_get(f, "folder")) || rec_get(f, "size")->n > SEARCH_MOST) continue;
    if (strstr(path, SLASH ".git" SLASH) || strstr(path, SLASH "node_modules" SLASH)) continue;
    buf text = { 0 };
    if (pl_read(path, &text) && text.n && !memchr(text.s, 0, text.n < SEARCH_SNIFF ? text.n : SEARCH_SNIFF)) {
      search_text(r, path, &text, words, most, out);
    }
    buf_free(&text);
  }
  return out;
}

// Text with each lone new line written \r\n, as many Windows files end their lines.
static const char *with_crlf(run *r, const char *text) {
  buf b = { 0 };
  for (const char *c = text; *c; c++) {
    if (*c == '\n' && (c == text || c[-1] != '\r')) buf_chr(&b, '\r');
    buf_chr(&b, *c);
  }
  const char *out = v_text(r->a, b.s ? b.s : "", b.n)->t.s;
  buf_free(&b);
  return out;
}

// How many times what is in the text.
static int times_in(const char *text, const char *what) {
  int count = 0;
  for (const char *at = strstr(text, what); at; at = strstr(at + strlen(what), what)) count++;
  return count;
}

// files.edit: exact words in a file replaced by others. Words that are there more than once are refused unless all is
// set, so the wrong place is never changed. Words of several lines also match a file that ends its lines \r\n.
static val *fs_edit(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  const char *find = str(r, a[1]);
  const char *with = str(r, a[2]);
  if (!*find) fail(r, "phrase", "files.edit needs the words to replace");
  buf b = { 0 };
  if (!pl_read(p->t.s, &b)) file_failed(r, "read", p->t.s);
  const char *text = v_text(r->a, b.s ? b.s : "", b.n)->t.s;
  buf_free(&b);
  if (!times_in(text, find) && strchr(find, '\n')) {
    find = with_crlf(r, find);
    with = with_crlf(r, with);
  }
  int count = times_in(text, find);
  if (!count) fail(r, "phrase", "those words aren't in %s", p->t.s);
  if (count > 1 && !truthy(a[3])) {
    fail(
        r, "phrase", "those words are in %s %d times: give more of what is around them, or all as true", p->t.s, count);
  }
  buf out = { 0 };
  const char *from = text;
  for (const char *at = strstr(from, find); at; at = strstr(from, find)) {
    buf_add(&out, from, (size_t)(at - from));
    buf_str(&out, with);
    from = at + strlen(find);
  }
  buf_str(&out, from);
  int ok = pl_write(p->t.s, out.s ? out.s : "", out.n);
  buf_free(&out);
  if (!ok) file_failed(r, "write", p->t.s);
  val *rec = v_rec(r->a);
  put_num(r, rec, "replaced", count);
  return rec;
}

////////// 3. COMMANDS //////////

// Where a started command keeps what commands.output reads: <temp>onsay-<id>.<what>.
static const char *kept_at(run *r, const char *id, const char *what) {
  buf b = { 0 };
  buf_fmt(&b, "%sonsay-%s.%s", temp_folder(r), id, what);
  const char *out = v_text(r->a, b.s, b.n)->t.s;
  buf_free(&b);
  return out;
}

// A new started command's id, a big random number as text.
static const char *started_id(run *r) {
  unsigned long long number = 0;
  pl_random(&number, sizeof number);
  buf id = { 0 };
  buf_fmt(&id, "%llu", number);
  const char *out = v_text(r->a, id.s, id.n)->t.s;
  buf_free(&id);
  return out;
}

// What a started command has written so far, and its exit code once it ended.
static val *shell_output(run *r, val **a) {
  const char *id = str(r, a[0]);
  if (!*id || strspn(id, "0123456789") != strlen(id)) fail(r, "phrase", "that isn't the id of a started command");
  buf log = { 0 };
  buf code = { 0 };
  buf pid = { 0 };
  if (!pl_read(kept_at(r, id, "pid"), &pid)) {
    // It may only just have been started, before its shell has written anything.
    nap(r, 1);
    if (!pl_read(kept_at(r, id, "pid"), &pid)) fail(r, "phrase", "no started command has that id");
  }
  // Whether it ended first, so all it wrote is read once it has.
  int ended = pl_read(kept_at(r, id, "code"), &code);
  pl_read(kept_at(r, id, "log"), &log);
  size_t skip = mark_length(&log);
  val *rec = v_rec(r->a);
  put(r, rec, "running", VBOOL(!ended));
  put(r, rec, "code", ended ? v_num(r->a, atoi(code.s + mark_length(&code))) : NOTHING);
  put_num(r, rec, "pid", atoi(pid.s + mark_length(&pid)));
  put(r, rec, "out", v_text(r->a, log.n ? log.s + skip : "", log.n - skip));
  buf_free(&log);
  buf_free(&code);
  buf_free(&pid);
  return rec;
}

////////// 4. CHAT //////////

// Questions in the person's Onsay chat here, each text, answered by typing, or {question, choices}, answered by picking
// one, waited for until an app answers (lingo_ask), which shows them as one card. A text alone is one question. Their
// answers in order, or nothing when none came in seconds.
static val *chat_ask(run *r, val **a) {
  val *questions = a[0];
  if (questions->k == V_TEXT) {
    questions = v_list(r->a);
    list_push(r->a, questions, a[0]);
  }
  if (questions->k != V_LIST || !questions->l.n)
    fail(r, "type", "chat.ask takes questions, each a text or {question, choices}");
  val *ask = v_rec(r->a);
  put(r, ask, "questions", questions);
  val *got = lingo_ask(r, ask, num_or(a[1], 120));
  if (!got) fail(r, "phrase", "No Onsay app is open on this computer to ask the person");
  return got;
}

// A line in the chat, {"show":"say"} or {"show":"step"}, with its text, for an app to draw. A request Onsay sent this
// computer sends it on instead, as a step, to the person who asked, wherever they are (lingo_said).
static val *chat_line(run *r, const char *kind, val *text) {
  const char *t = str(r, text);
  if (lingo_said(t)) return NOTHING;
  buf b = { 0 };
  buf_fmt(&b, "{\"show\":\"%s\",\"text\":", kind);
  buf_json_str(&b, t, strlen(t));
  buf_chr(&b, '}');
  show(&b);
  return NOTHING;
}

static val *chat_say(run *r, val **a) { return chat_line(r, "say", a[0]); }
static val *chat_step(run *r, val **a) { return chat_line(r, "step", a[0]); }
