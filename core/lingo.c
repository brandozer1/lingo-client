// lingo.c - the lingo client before the floor (floor.c replaces it): its routes and the policies guarding them, the floor
// calls, its link to Onsay with the lingo search and the log of every request. The language, the checks and the hardware
// calls are included from language.c, checks.c and hardware.c, which the floor shares. Lingos attach through lingo.h.
// Built by build.ps1 with zig, beside the code it takes as it came (vendor/).
//
// 1-6.           Lingo itself: language.c
//                Who is calling and whether they may: checks.c
// 7. Zips        a .lingo file read and made
// 8. Engine      settings, the floor calls, lingos loaded, policies, calls, routes and every request answered
// 9. Onsay       the link to Onsay, signing in, keys, policy sync, the log sent, prompts and screens
// 10. Host       the program: its threads, its port, the command line and main
// 11-12.         The hardware calls on Windows, and on Linux and Android: hardware.c

#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lingo.h"
#include "monocypher-ed25519.h"
#include "puff.h"
#ifndef __ANDROID__
#include <ffi.h>
#endif

#define CORE_VERSION "0.24.6"
#define CORE_LOOK 5   // seconds between looks at the lingos folder

/* Where Onsay is, built in: its API, which the onsay lingo talks to and whose resolve endpoint the lingo client asks where
   an Onsay name goes (../../docs/lingo.md, section 15), and its public key, which everything Onsay signs is checked
   against (Onsay, section 9). The test build asks test/server.mjs, which signs with the test key. */
#ifdef TEST
#define ONSAY "http://127.0.0.1:8787"
#define ONSAY_KEY "ed25519:6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw="
#else
#define ONSAY "https://api.onsay.dev"
#define ONSAY_KEY "ed25519:4DnM32U1FzXfVh3ahkdILlt7yNwfM84Ofj7Aohoe1QE="
#endif
#define RESOLVE "/resolve/"   // on ONSAY, then the name

typedef struct {
  val *all;                           // the whole file, as read
  const char *home, *root, *lingos;   // root: home as a whole path
  int port;
  long long steps;
  double ms;
} core_settings;

const core_settings *core_read(const char *text, size_t n);
int core_begin(int serving);
int core_look(void);
void core_serve(const char *request, size_t n, const char *caller, buf *out);
void core_stop(const char *id);
val *core_control(arena *a, const char *verb, val *values, const char **why);
int core_pack(const char *folder, const char *to);

int zip_file(const char *zip, size_t n, const char *name, buf *out);
void zip_make(buf *out, int count, const char *const *names, const buf *files);

#include "language.c"
#include "checks.c"

//////////////////// 7. ZIPS ////////////////////

// A .lingo file is a zip: one file read out of one, stored or deflated (puff.c), and one made, its files stored.
//
// 1. zip_file(zip, n, name, out)        -> 1 with the file called name in out, 0 when it isn't there or isn't whole
// 2. zip_make(out, count, names, files) -> a zip of those files, each stored as it is

#define ZIP_MOST (64u << 20)   // the most one file in a zip may be, unpacked

static uint32_t crc(const unsigned char *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0 - (c & 1)));
  }
  return ~c;
}

static uint32_t num(const unsigned char *p, int bytes) {
  uint32_t v = 0;
  for (int i = bytes - 1; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

static void zip_put(buf *b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++) buf_chr(b, (char)(v >> (8 * i)));
}

////////// 1. READING //////////

// Its end record names where the list of its files is, and each in the list where the file is.
int zip_file(const char *zip, size_t n, const char *name, buf *out) {
  const unsigned char *z = (const unsigned char *)zip;
  const unsigned char *end = NULL;
  for (size_t i = n >= 22 ? n - 22 : 0; n >= 22 && !end; i--) {
    if (num(z + i, 4) == 0x06054b50) end = z + i;
    if (!i || n - i > 22 + 65535) break;
  }
  if (!end) return 0;
  size_t at = num(end + 16, 4);
  size_t count = num(end + 10, 2);
  size_t len = strlen(name);
  for (size_t k = 0; k < count && at + 46 <= n && num(z + at, 4) == 0x02014b50; k++) {
    const unsigned char *e = z + at;
    size_t nl = num(e + 28, 2);
    size_t method = num(e + 10, 2);
    size_t packed = num(e + 20, 4);
    size_t size = num(e + 24, 4);
    size_t local = num(e + 42, 4);
    at += 46 + nl + num(e + 30, 2) + num(e + 32, 2);
    if (at > n || nl != len || memcmp(e + 46, name, len)) continue;
    if (local + 30 > n || num(z + local, 4) != 0x04034b50 || size > ZIP_MOST) return 0;
    size_t from = local + 30 + num(z + local + 26, 2) + num(z + local + 28, 2);
    if (from > n || packed > n - from || (method != 0 && method != 8)) return 0;
    unsigned char *data = malloc(size + 1);
    unsigned long got = size;
    unsigned long used = packed;
    int ok;
    if (method == 0) {
      ok = packed == size;
      if (ok) memcpy(data, z + from, size);
    } else {
      ok = !puff(data, &got, z + from, &used) && got == size;
    }
    ok = ok && crc(data, size) == num(e + 16, 4);
    if (ok) buf_add(out, (const char *)data, size);
    if (ok && !out->s) buf_add(out, "", 0);
    free(data);
    return ok;
  }
  return 0;
}

////////// 2. MAKING //////////

// Every time is 1980-01-01, the first a zip can say, so the same files always make the same zip.
void zip_make(buf *out, int count, const char *const *names, const buf *files) {
  buf list = { 0 };
  for (int i = 0; i < count; i++) {
    const unsigned char *data = (const unsigned char *)(files[i].s ? files[i].s : "");
    uint32_t sum = crc(data, files[i].n);
    uint32_t size = (uint32_t)files[i].n;
    uint32_t at = (uint32_t)out->n;
    uint32_t nl = (uint32_t)strlen(names[i]);
    zip_put(out, 0x04034b50, 4);
    zip_put(out, 20, 2);
    zip_put(out, 0, 2);
    zip_put(out, 0, 2);
    zip_put(out, 0, 2);
    zip_put(out, 33, 2);
    zip_put(out, sum, 4);
    zip_put(out, size, 4);
    zip_put(out, size, 4);
    zip_put(out, nl, 2);
    zip_put(out, 0, 2);
    buf_add(out, names[i], nl);
    buf_add(out, (const char *)data, size);
    zip_put(&list, 0x02014b50, 4);
    zip_put(&list, 20, 2);
    zip_put(&list, 20, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 33, 2);
    zip_put(&list, sum, 4);
    zip_put(&list, size, 4);
    zip_put(&list, size, 4);
    zip_put(&list, nl, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 2);
    zip_put(&list, 0, 4);
    zip_put(&list, at, 4);
    buf_add(&list, names[i], nl);
  }
  uint32_t at = (uint32_t)out->n;
  buf_add(out, list.s ? list.s : "", list.n);
  zip_put(out, 0x06054b50, 4);
  zip_put(out, 0, 2);
  zip_put(out, 0, 2);
  zip_put(out, (uint32_t)count, 2);
  zip_put(out, (uint32_t)count, 2);
  zip_put(out, (uint32_t)list.n, 4);
  zip_put(out, at, 4);
  zip_put(out, 0, 2);
  buf_free(&list);
}

//////////////////// 8. ENGINE ////////////////////

// The lingo client's engine: its settings, the lingos it loads, who may call what, and every request it answers.
//
// 1. Settings        the settings file, read once, and JSON files in its home
// 2. Floor           the compute, store, http, socket, connection and settings phrases every core has, and lingo(...) and use(...),
//                    asked of the native lingo that answers them, or of the settings' ask
// 3. Lingos          .lingo files and folders: read, loaded, their OpenAPI manifests, running or stopped, the catalog,
//                    and the hooks of native lingos that run
// 4. Policy          policy.json and the account's: callers, signatures, grants and badges, asks and the log
// 5. Stops           stopping a running request, and those running inside its wait
// 6. Calls           one function of a lingo, run
// 7. Lingo settings  setting(...), settings.get and settings.set, kept on this computer, never on Onsay
// 8. Control         start, stop, the person's answers, settings, manifests, the policy and call, at /.lingo/ and on the
//                    command line
// 9. Hooks           what native lingos are offered back: the account's policy kept, settings, this device's keys, its
//                    home, and looking at the lingos folder again
// 10. Requests       web requests, from this computer's port or handed in by a native lingo, each answered by the lingo
//                    at its route when the caller may reach it
// 11. Beginning, and packing a lingo
// It starts no thread and holds no connection but each http.send's, each socket's and each connection's, and core_stop may run
// on another thread, so those and the running request's stop are kept behind one gate.
#include "tlds.h"   // TLDS, the web's top level domains as one text, " aaa aarp ... ", made by build.ps1 from tlds.txt

#define say core_say

/* The request running now, for the checks at each phrase: its caller as the log names them, the lingo and function it
   reached, like agent and agent.ask, its path, the policy in force, and what the person answered this time,
   [{what, yes}], and its id as JSON, which each lingo(...) and use(...) it asks carries as for, and the badge it came
   with, its grants in order. lingo is NULL outside a request, at the command line. */
static struct {
  const char *caller, *lingo, *fn, *path, *id;
  val *policy, *answers, *badge;
} job;

/* What each native lingo, by its number, filled with lingo_hook as it loaded, and whether it runs in a lingo client that
   serves, when they are used (lingo.h, section 7). */
static lingo_hooks hooks[64];
static int on[64], live;   // live: it serves requests, so its running native lingos are told
static size_t loading;   // the native lingo loading now

// Whether a list holds this text.
static int has(val *list, const char *s) {
  for (size_t i = 0; list && list->k == V_LIST && i < list->l.n; i++)
    if (list->l.v[i]->k == V_TEXT && !strcmp(list->l.v[i]->t.s, s)) return 1;
  return 0;
}

////////// 1. SETTINGS //////////

static core_settings set;


// 1. Kept in their own arena for as long as the core runs.
const core_settings *core_read(const char *text, size_t n) {
  arena *core_kept = arena_new();
  const char *problem = NULL;
  val *s = json_parse(core_kept, text, n, &problem);
  val *port;
  val *steps;
  val *ms;
  if (problem || s->k != V_REC) {
    say("The settings aren't a JSON record: %s", problem ? problem : "they're another kind");
    return NULL;
  }
  buf root = { 0 };
  set.all = s;
  set.home = text_or(s, "home", "onsay-data");
  set.lingos = text_or(s, "lingos", NULL) ? text_or(s, "lingos", NULL) : arena_fmt(core_kept, "%s/lingos", set.home);
  pl_folder(set.home);
  pl_whole(set.home, &root);
  set.root = v_text(core_kept, root.s, root.n)->t.s;
  buf_free(&root);
  port = rec_get(s, "port");
  steps = rec_get(s, "steps");
  ms = rec_get(s, "ms");
  set.port = port && port->k == V_NUM ? (int)port->n : 6969;
  set.steps = steps && steps->k == V_NUM ? (long long)steps->n : 5000000;
  set.ms = ms && ms->k == V_NUM ? ms->n : 600000;
  return &set;
}

// A JSON file in home, like policy.json or settings/agent.json, or NULL when it isn't there or isn't JSON.
static val *home_json(arena *a, const char *name) {
  buf path = { 0 };
  buf text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  val *v = pl_read(path.s, &text) ? json_parse(a, text.s, text.n, NULL) : NULL;
  buf_free(&path);
  buf_free(&text);
  return v;
}

// Keeps one, its folder made when missing. 1 when it was kept.
static int keep_json(const char *name, val *v) {
  buf path = { 0 };
  buf text = { 0 };
  buf_fmt(&path, "%s/%s", set.home, name);
  buf_json(&text, v);
  char *slash = strrchr(path.s, '/');
  *slash = 0;
  pl_folder(path.s);
  *slash = '/';
  int ok = pl_write(path.s, text.s, text.n);
  buf_free(&path);
  buf_free(&text);
  return ok;
}

////////// 2. FLOOR //////////

// compute: the clock, waiting, and a line in the lingo client's log.

// store: Onsay's own storage, one file per key in home/store. A key's name is written so any key is a safe file name on
// any system: only a to z, 0 to 9, _ and - stay, everything else becomes %XX, so case and dots can never clash.

// Each lingo's keys start with its name and a slash, like memo/today, so no lingo reads or changes another's. The person,
// at the command line or through /onsay, reaches any.
static void core_mine(run *r, val *key) {
  size_t n = r->lingo ? strlen(r->lingo) : 0;
  if (n && (key->t.len <= n || memcmp(key->t.s, r->lingo, n) || key->t.s[n] != '/'))
    fail(r, "phrase", "%s's keys start with %s/, like %s/notes", r->lingo, r->lingo, r->lingo);
}

static void file_of(run *r, val *key, buf *path) {
  if (key->k != V_TEXT || !key->t.len) fail(r, "phrase", "a key is text, like notes/today");
  if (key->t.len > 100) fail(r, "phrase", "a key is at most 100 characters");
  core_mine(r, key);
  buf_fmt(path, "%s/store/k", set.home);
  for (size_t i = 0; i < key->t.len; i++) {
    unsigned char c = (unsigned char)key->t.s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
      buf_chr(path, (char)c);
    else
      buf_fmt(path, "%%%02X", c);
  }
}

static int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static val *get(run *r, val **a) {
  buf path = { 0 };
  buf text = { 0 };
  file_of(r, a[0], &path);
  int found = pl_read(path.s, &text);
  val *out = found ? v_text(r->a, text.s, text.n) : NOTHING;
  buf_free(&path);
  buf_free(&text);
  return out;
}

static val *core_put(run *r, val **a) {
  buf path = { 0 };
  buf text = { 0 };
  buf folder = { 0 };
  file_of(r, a[0], &path);
  buf_text(&text, a[1]);
  buf_fmt(&folder, "%s/store", set.home);
  int ok = pl_folder(folder.s) && pl_write(path.s, text.s ? text.s : "", text.n);
  buf_free(&path);
  buf_free(&text);
  buf_free(&folder);
  if (!ok) fail(r, "phrase", "this computer couldn't save it");
  return NOTHING;
}

static val *delete_(run *r, val **a) {
  buf path = { 0 };
  file_of(r, a[0], &path);
  int ok = pl_delete(path.s);
  buf_free(&path);
  if (!ok) fail(r, "phrase", "this computer couldn't delete it");
  return NOTHING;
}

typedef struct {
  run *r;
  val *prefix, *out;
} key_listing;

static void each_key(const char *name, int folder, double size, double changed, void *arg) {
  key_listing *l = arg;
  (void)size;
  (void)changed;
  if (folder || name[0] != 'k') return;
  buf key = { 0 };
  for (const char *p = name + 1; *p; p++) {
    int hi, lo;
    if (*p == '%' && (hi = hex(p[1])) >= 0 && (lo = hex(p[2])) >= 0) {
      buf_chr(&key, (char)(hi * 16 + lo));
      p += 2;
    } else if (*p == '%') {
      buf_free(&key);
      return;
    }   // not one of ours
    else
      buf_chr(&key, *p);
  }
  if (key.n >= l->prefix->t.len && !memcmp(key.s, l->prefix->t.s, l->prefix->t.len))
    list_push(l->r->a, l->out, v_text(l->r->a, key.s, key.n));
  buf_free(&key);
}

static val *keys(run *r, val **a) {
  val *prefix = a[0]->k == V_TEXT ? a[0] : v_cstr(r->a, "");
  if (r->lingo && !prefix->t.len) prefix = v_cstr(r->a, arena_fmt(r->a, "%s/", r->lingo));   // every key of its own
  core_mine(r, prefix);
  key_listing l = { r, prefix, v_list(r->a) };
  buf folder = { 0 };
  buf_fmt(&folder, "%s/store", set.home);
  pl_list(folder.s, each_key, &l);
  buf_free(&folder);
  // Sorted, so every computer lists the same keys in the same order.
  val **v = l.out->l.v;
  for (size_t i = 1; i < l.out->l.n; i++)
    for (size_t j = i; j > 0 && strcmp(v[j - 1]->t.s, v[j]->t.s) > 0; j--) {
      val *t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return l.out;
}

/* A web address as a lingo wrote it, made the one to send, by the one rule every Onsay repo keeps (onsay's
   server/lib/names.js web): a host is on the web when it is an IP, localhost, or a name with a dot ending in one of the
   web's top level domains, like example.com, and is itself, http:// or https:// left off when it likes, http for an IP.
   Any other, like laptop.brandon, is an Onsay name: Onsay's resolve endpoint (ONSAY and RESOLVE, at the top) gives its web
   address, and what followed the name goes on after it. ws, for a WebSocket: ws:// and wss:// read as http:// and
   https://, and the address given is ws:// or wss://, but never a computer's. NULL, with why, when it has none. */
static const char *reached(arena *a, const char *url, int ws, buf *why) {
  size_t s = strspn(url, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
  size_t n;
  size_t dots = 0;
  size_t digits = 1;
  const char *rest = s && !strncmp(url + s, "://", 3) ? url + s + 3 : url;
  char host[256], word[80];
  if ((n = strcspn(rest, "/?#")) >= sizeof host || !n) return url;
  for (size_t i = 0; i <= n; i++) host[i] = i < n ? (char)tolower((unsigned char)rest[i]) : 0;
  char *port = strrchr(host, ':');
  char *last = strrchr(host, '.');
  if (port && host[0] != '[' && strspn(port + 1, "0123456789") == strlen(port + 1)) *port = 0;
  for (const char *c = host; *c; c++)
    if (*c == '.')
      dots++;
    else if (!isdigit((unsigned char)*c))
      digits = 0;
  int ip = host[0] == '[' || !strcmp(host, "localhost") || (digits && dots == 3);
  snprintf(word, sizeof word, " %s ", last ? last + 1 : "");
  if (ip || (last && strlen(last) < 70 && strstr(TLDS, word)))
    return rest != url ? url : arena_fmt(a, "%s%s://%s", ws ? "ws" : "http", ip ? "" : "s", url);
  buf at = { 0 };
  buf_fmt(&at, "%s%s%s", ONSAY, RESOLVE, host);
  val *got = pl_http(a, "GET", at.s, v_rec(a), "", 0, 30, why);
  val *text = rec_get(got, "text");
  val *said = text && text->k == V_TEXT ? json_parse(a, text->t.s, text->t.len, NULL) : NULL;
  const char *address = said && said->k == V_REC ? text_or(said, "address", NULL) : NULL;
  buf_free(&at);
  if (!address && got) buf_fmt(why, "%s", said && said->k == V_REC ? text_or(said, "error", "") : "");
  if (!address && got && !why->n) buf_fmt(why, "%s has nowhere to send a request yet", host);
  if (!address) return NULL;
  if (!ws || strncmp(address, "http", 4)) return arena_fmt(a, "%s%s", address, rest + n);
  // Onsay's proxy address for a computer, or a route to one, ends with its name (onsay's server/lib/proxy.js address).
  size_t length = strlen(address);
  size_t name = strlen(host);
  if (length > name && address[length - name - 1] == '/' && !strcmp(address + length - name, host)) {
    buf_fmt(why, "%s is on a computer, which Onsay reaches one request at a time, so a WebSocket can't reach it yet", host);
    return NULL;
  }
  return arena_fmt(a, "ws%s%s", address + 4, rest + n);   // http://x is ws://x, https://x wss://x
}

static pl_gate *core_gate, *log_gate;

#define CALL_BODY 4096   // the most bytes of a body a call keeps (lingo_called)
#define LOG_MOST (4 << 20)   // bytes a log in home may grow to before it is cut to its newest half

// A line added to a log in home, requests.log or sent.log, cut to its newest half once it passes LOG_MOST. size is the
// log's size as last known, -1 before it is read.
static void log_add(const char *name, long *size, const char *line, size_t n) {
  buf path = { 0 };
  buf all = { 0 };
  buf_fmt(&path, "%s/%s", set.root, name);
  pl_enter(log_gate);
  if (*size < 0 || *size > LOG_MOST) {
    *size = pl_read(path.s, &all) ? (long)all.n : 0;
    const char *half = *size > LOG_MOST ? memchr(all.s + *size / 2, '\n', *size - *size / 2) : NULL;
    if (half && pl_write(path.s, half + 1, all.s + *size - half - 1)) *size = all.s + *size - half - 1;
  }
  if (pl_append(path.s, line, n)) *size += (long)n;
  pl_leave(log_gate);
  buf_free(&path);
  buf_free(&all);
}

/* A line of home/requests.log, the one log of every request this lingo client answers or makes, for the person to read:
   way is in, out, or need, a need of a lingo's decided. */
static void requested(val *line) {
  static long size = -1;
  buf text = { 0 };
  buf_json(&text, line);
  buf_chr(&text, '\n');
  log_add("requests.log", &size, text.s, text.n);
  buf_free(&text);
}

// A header that carries a key, or a query value's name that says it is one: never kept in a call.
static int secret(const char *name, int query) {
  static const char *headers[] = { "authorization", "cookie", "set-cookie", "proxy-authorization", "x-api-key", "api-key" };
  static const char *words[] = { "key", "token", "secret", "password", "sig", "auth" };
  char low[128];
  size_t n = 0;
  for (; name[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
  low[n] = 0;
  if (!query) {
    for (size_t i = 0; i < sizeof headers / sizeof *headers; i++)
      if (!strcmp(low, headers[i])) return 1;
    return 0;
  }
  for (size_t i = 0; i < sizeof words / sizeof *words; i++)
    if (strstr(low, words[i])) return 1;
  return 0;
}

// Headers as a call keeps them, every secret one (redacted).
static val *kept_headers(arena *a, val *headers) {
  val *out = v_rec(a);
  for (size_t i = 0; headers && headers->k == V_REC && i < headers->r.n; i++)
    rec_set(a, out, headers->r.key[i], secret(headers->r.key[i], 0) ? v_cstr(a, "(redacted)") : headers->r.v[i]);
  return out;
}

// lingo.h: one request made, a line of requests.log, and as JSON to each running native lingo that keeps calls.
void lingo_called(const char *kind, const char *address, const char *ask, const char *method, const char *url, val *headers,
    const char *body, size_t n, val *got, const char *error, int stopped, double ms) {
  arena *a = arena_new();
  val *line = v_rec(a);
  val *request = v_rec(a);
  val *response = v_rec(a);
  buf safe = { 0 };
  buf text = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  const char *query = strchr(url, '?');
  // The address with each secret query value taken out.
  buf_add(&safe, url, query ? (size_t)(query - url) : strlen(url));
  for (const char *p = query; p && *p;) {
    size_t len = strcspn(p + 1, "&");
    size_t name = strcspn(p + 1, "=&");
    char key[128];
    snprintf(key, sizeof key, "%.*s", (int)(name < sizeof key ? name : sizeof key - 1), p + 1);
    buf_chr(&safe, *p);
    if (name < len && secret(key, 1))
      buf_fmt(&safe, "%s=(redacted)", key);
    else
      buf_add(&safe, p + 1, len);
    p = p[1 + len] ? p + 1 + len : NULL;
  }
  buf_chr(&safe, 0);
  const char *host = strstr(safe.s, "://");
  const char *path = host ? strchr(host + 3, '/') : safe.s;
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  rec_set(a, request, "method", v_cstr(a, method));
  rec_set(a, request, "url", v_cstr(a, safe.s));
  rec_set(a, request, "path", v_cstr(a, path && *path ? path : "/"));
  rec_set(a, request, "headers", kept_headers(a, headers));
  rec_set(a, request, "body", v_text(a, body ? body : "", body ? (n < CALL_BODY ? n : CALL_BODY) : 0));
  val *said = got ? rec_get(got, "text") : NULL;   // text, or for a socket a record of what went each way, kept whole
  val *status = got ? rec_get(got, "status") : NULL;
  if (said && said->k == V_TEXT && said->t.len > CALL_BODY) said = v_text(a, said->t.s, CALL_BODY);
  rec_set(a, response, "status", status && status->k == V_NUM ? status : v_num(a, 0));
  rec_set(a, response, "headers", kept_headers(a, got ? rec_get(got, "headers") : NULL));
  rec_set(a, response, "body", said ? said : NOTHING);
  rec_set(a, line, "at", v_cstr(a, at));
  rec_set(a, line, "way", v_cstr(a, "out"));
  rec_set(a, line, "ms", v_num(a, ms));
  rec_set(a, line, "kind", v_cstr(a, kind));
  rec_set(a, line, "address", v_cstr(a, address ? address : safe.s));
  rec_set(a, line, "ask", v_cstr(a, ask ? ask : ""));
  rec_set(a, line, "request", request);
  rec_set(a, line, "response", response);
  rec_set(a, line, "error", v_cstr(a, error ? error : ""));
  rec_set(a, line, "stopped", VBOOL(stopped));
  buf_json(&text, line);
  for (size_t i = 1; i < 64; i++)
    if (on[i] && hooks[i].called) hooks[i].called(text.s);
  requested(line);
  buf_free(&safe);
  buf_free(&text);
  arena_free(a);
}

// One http.send on a thread of its own, with its own memory, so other requests are answered while it waits.
typedef struct {
  arena *a;
  const char *method, *url, *body;
  size_t n;
  val *headers, *got;
  double seconds;
  buf why;
  int done, finished, left;
} trip;

static void trip_free(trip *t) {
  buf_free(&t->why);
  arena_free(t->a);
  free(t);
}

static void travel(void *arg) {
  trip *t = arg;
  const char *to = reached(t->a, t->url, 0, &t->why);
  if (to) t->got = pl_http(t->a, t->method, to, t->headers, t->body, t->n, t->seconds, &t->why);
  pl_enter(core_gate);
  t->finished = 1;
  int left = t->left;
  pl_leave(core_gate);
  if (left)
    trip_free(t);
  else
    lingo_done(&t->done);
}

// http.send: one web request, made by the computer itself (pl_http), its method a word and each header one whole line.
static val *core_send(run *r, val **a) {
  buf method = { 0 };
  buf url = { 0 };
  buf body = { 0 };
  buf why = { 0 };
  trip *t = calloc(1, sizeof *t);
  val *got = NULL;
  t->a = arena_new();
  t->headers = v_rec(t->a);
  buf_text(&method, a[0]);
  buf_text(&url, a[1]);
  buf_text(&body, a[3]);
  buf_chr(&method, 0);
  buf_chr(&url, 0);   // so neither is ever empty of bytes
  for (size_t i = 0; method.s[i]; i++)
    if (method.s[i] >= 'a' && method.s[i] <= 'z') method.s[i] -= 32;
  if (strchr(url.s, '#')) *strchr(url.s, '#') = 0;   // the part after # is the page's own, never sent
  if (!*method.s || strspn(method.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(method.s))
    buf_str(&why, "the method is a word, like GET or POST");
  for (size_t i = 0; a[2]->k == V_REC && i < a[2]->r.n && !why.n; i++) {
    val *value = text_of(r, a[2]->r.v[i]);
    if (strpbrk(a[2]->r.key[i], "\r\n:") || strpbrk(value->t.s, "\r\n"))
      buf_fmt(&why, "the header %s has a line break or a colon in it", a[2]->r.key[i]);
    else
      rec_set(t->a, t->headers, a[2]->r.key[i], v_cstr(t->a, value->t.s));
  }
  double seconds = a[4]->k == V_NUM && a[4]->n > 0 && a[4]->n < 3600 ? a[4]->n : 30;
  val *problem = v_text(r->a, why.s ? why.s : "", why.n);
  if (!why.n) {
    t->method = v_cstr(t->a, method.s)->t.s;
    t->url = v_cstr(t->a, url.s)->t.s;
    t->body = v_text(t->a, body.s ? body.s : "", body.n)->t.s;
    t->n = body.n;
    t->seconds = seconds;
  }
  int asked_ = !why.n;
  buf_free(&method);
  buf_free(&url);
  buf_free(&body);
  buf_free(&why);
  if (!asked_) {
    trip_free(t);
    fail(r, "phrase", "%s", problem->t.s);
  }
  double began = pl_now();
  pl_thread(travel, t);
  while (!t->done && !*r->stop) lingo_wait(&t->done, r->stop, 1);
  // Kept as a call once: stopped here while the trip still runs, with no answer, else below with its answer.
  int stopped = !t->done;
  if (stopped) lingo_called("web", NULL, NULL, t->method, t->url, t->headers, t->body, t->n, NULL, "Stopped by the person", 1, (pl_now() - began) * 1000);
  pl_enter(core_gate);
  int gone = !t->finished;   // stopped first: it frees itself when it ends
  t->left = gone;
  pl_leave(core_gate);
  if (gone) fail(r, "stopped", "Stopped by the person");
  val *status = t->got ? rec_get(t->got, "status") : NULL;
  const char *error = t->got ? NULL : t->why.s ? t->why.s : "No answer";
  if (status && status->k == V_NUM && (status->n < 200 || status->n > 399)) error = "It answered with an error";
  if (!stopped) lingo_called("web", NULL, NULL, t->method, t->url, t->headers, t->body, t->n, t->got, error, 0, (pl_now() - began) * 1000);
  // Written out and read back, since a copy would still point at text in the trip's memory, freed below.
  buf whole = { 0 };
  if (t->got) buf_json(&whole, t->got);
  got = t->got ? json_parse(r->a, whole.s, whole.n, NULL) : NULL;
  buf_free(&whole);
  problem = v_text(r->a, t->why.s ? t->why.s : "", t->why.n);
  trip_free(t);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got) fail(r, "phrase", "%s", problem->t.s);
  return got;
}

/* socket: WebSockets a run opens, each opened and read on a thread of its own (talk), every message that comes kept in
   waiting until socket.receive takes it. Every socket a run opened is closed as that run ends (core_call). talk frees a
   socket its run let go of once talk ends, else the run frees it, the way a trip is freed. Each is kept as a call of
   kind socket as its run lets go of it, and one that couldn't open at once (let_go). */
typedef struct sock {
  struct sock *next;
  long id;
  char *url, *headers;   // url as the lingo wrote it, headers whole lines, or NULL
  pl_link *l;            // set once, as it opens, and freed with it
  buf waiting;           // each message come and not yet taken: its length, a size_t, then its text
  buf why;               // why it didn't open
  volatile int news;     // set as it opens or ends and as each message comes, for lingo_wait
  int ended, finished, left;   // ended: closed, or never opened. left: let go by its run
  double began, over;          // when it was asked for, and when it ended
  double sent, sent_bytes, came, came_bytes;   // messages each way, and their size
} sock;

static sock *socks;   // the open ones, newest first, so those a run opened are on top as it ends
static long socks_made;

static void sock_free(sock *s) {
  if (s->l) pl_free(s->l);
  buf_free(&s->waiting);
  buf_free(&s->why);
  free(s->url);
  free(s->headers);
  free(s);
}

static void talk(void *arg) {
  sock *s = arg;
  buf in = { 0 };
  arena *a = arena_new();
  const char *to = reached(a, s->url, 1, &s->why);
  pl_link *l = to ? pl_connect(to, s->headers, &s->why) : NULL;
  arena_free(a);
  pl_enter(core_gate);
  s->l = l;
  if (l && s->left) pl_close(l);
  pl_leave(core_gate);
  lingo_done(&s->news);
  while (l && pl_receive(l, &in)) {
    pl_enter(core_gate);
    buf_add(&s->waiting, (const char *)&in.n, sizeof in.n);
    buf_add(&s->waiting, in.s ? in.s : "", in.n);
    s->came++;
    s->came_bytes += (double)in.n;
    pl_leave(core_gate);
    lingo_done(&s->news);
    in.n = 0;
  }
  buf_free(&in);
  pl_enter(core_gate);
  s->ended = 1;
  s->over = pl_now();
  pl_leave(core_gate);
  lingo_done(&s->news);
  pl_enter(core_gate);
  s->finished = 1;
  int left = s->left;
  pl_leave(core_gate);
  if (left) sock_free(s);
}

/* Kept as a call, closed and taken off the open ones, freed now when talk has ended, else by talk as it ends. closed: by
   the lingo, or as the run ended, unless the other side closed it first. stopped: the person stopped it opening. */
static void let_go(sock *s, const char *closed, int stopped) {
  sock **at = &socks;
  arena *a = arena_new();
  val *got = NULL;
  while (*at != s) at = &(*at)->next;
  *at = s->next;
  pl_enter(core_gate);
  int opened = s->l != NULL;
  if (opened && s->ended) closed = "by the other side";
  double ms = ((opened && s->ended ? s->over : pl_now()) - s->began) * 1000;
  if (opened) {
    val *sent = v_rec(a);
    val *received = v_rec(a);
    val *body = v_rec(a);
    rec_set(a, sent, "messages", v_num(a, s->sent));
    rec_set(a, sent, "bytes", v_num(a, s->sent_bytes));
    rec_set(a, received, "messages", v_num(a, s->came));
    rec_set(a, received, "bytes", v_num(a, s->came_bytes));
    rec_set(a, body, "sent", sent);
    rec_set(a, body, "received", received);
    rec_set(a, body, "closed", v_cstr(a, closed));
    got = v_rec(a);
    rec_set(a, got, "status", v_num(a, 101));
    rec_set(a, got, "text", body);
  }
  const char *why = "Stopped by the person";
  if (!stopped) why = s->why.n ? v_cstr(a, s->why.s)->t.s : "It couldn't open";
  pl_leave(core_gate);
  lingo_called("socket", NULL, NULL, "GET", s->url, header_lines(a, s->headers ? s->headers : ""), NULL, 0, got,
      opened ? NULL : why, stopped, ms);
  arena_free(a);
  pl_enter(core_gate);
  s->left = 1;
  if (s->l) pl_close(s->l);
  int finished = s->finished;
  pl_leave(core_gate);
  if (finished) sock_free(s);
}

static sock *sock_of(run *r, val *id) {
  sock *s = socks;
  while (s && !(id->k == V_NUM && s->id == id->n)) s = s->next;
  if (!s) fail(r, "phrase", "no socket %s is open in this run", text_of(r, id)->t.s);
  return s;
}

// socket.open: its address ws://, wss:// or with none, an Onsay name too (reached), and each header one whole line, as
// for http.send.
static val *socket_open(run *r, val **a) {
  val *url = text_of(r, a[0]);
  buf lines = { 0 };
  size_t scheme = strspn(url->t.s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
  if (!strncmp(url->t.s + scheme, "://", 3) && strncmp(url->t.s, "ws://", 5) && strncmp(url->t.s, "wss://", 6))
    fail(r, "phrase", "a WebSocket's address starts with ws:// or wss://, or leaves it off");
  for (size_t i = 0; a[1]->k == V_REC && i < a[1]->r.n; i++) {
    val *value = text_of(r, a[1]->r.v[i]);
    if (strpbrk(a[1]->r.key[i], "\r\n:") || strpbrk(value->t.s, "\r\n")) {
      buf_free(&lines);
      fail(r, "phrase", "the header %s has a line break or a colon in it", a[1]->r.key[i]);
    }
    buf_fmt(&lines, "%s: %s\r\n", a[1]->r.key[i], value->t.s);
  }
  sock *s = calloc(1, sizeof *s);
  s->id = ++socks_made;
  s->url = strdup(url->t.s);
  s->headers = lines.s;
  s->began = pl_now();
  s->next = socks;
  socks = s;
  pl_thread(talk, s);
  while (!s->news && !*r->stop) lingo_wait(&s->news, r->stop, 1);
  pl_enter(core_gate);
  int open = s->l != NULL;
  pl_leave(core_gate);
  if (open) return v_num(r->a, (double)s->id);
  val *why = v_text(r->a, s->why.s ? s->why.s : "", s->why.n);
  let_go(s, "as the run ended", *r->stop);   // closed so, should it open just as it is stopped
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  fail(r, "phrase", "%s", why->t.s);
}

static val *socket_send(run *r, val **a) {
  sock *s = sock_of(r, a[0]);
  val *text = text_of(r, a[1]);
  if (!pl_send(s->l, text->t.s, text->t.len)) fail(r, "phrase", "the socket was closed");
  s->sent++;
  s->sent_bytes += (double)text->t.len;
  return NOTHING;
}

// socket.receive: the oldest message waiting, else the next to come within seconds, else nothing.
static val *socket_receive(run *r, val **a) {
  sock *s = sock_of(r, a[0]);
  double seconds = a[1]->k == V_NUM && a[1]->n >= 0 && a[1]->n < 3600 ? a[1]->n : 30;
  double until = pl_now() + seconds;
  for (;;) {
    val *got = NULL;
    pl_enter(core_gate);
    if (s->waiting.n) {
      size_t n;
      memcpy(&n, s->waiting.s, sizeof n);
      got = v_text(r->a, s->waiting.s + sizeof n, n);
      s->waiting.n -= sizeof n + n;
      memmove(s->waiting.s, s->waiting.s + sizeof n + n, s->waiting.n);
    }
    int ended = s->ended;
    if (!got && !ended) s->news = 0;
    pl_leave(core_gate);
    if (got) return got;
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    if (ended) fail(r, "phrase", "the socket was closed");
    if (pl_now() >= until) return NOTHING;
    lingo_wait(&s->news, r->stop, until - pl_now());
  }
}

static val *socket_close(run *r, val **a) {
  let_go(sock_of(r, a[0]), "by the lingo", 0);
  return NOTHING;
}

/* connection: the floor under every protocol, bytes over a connection a run opens, tcp or udp to a host and port, or a
   listener taking tcp connections coming in to this computer. Its address says its kind, like tcp://example.com:80, and
   the kinds a port opens itself are PL_KINDS. Each is opened and read on a thread of its own (pull), what comes kept in
   waiting until connection.receive or connection.accept takes it, and the reading pauses while a lot is waiting. Every
   connection a run opened is closed as that run ends (core_call). pull frees one its run let go of once pull ends, else
   the run frees it, the way a socket is freed. */
typedef struct cn {
  struct cn *next;
  long id;
  char kind[8], *host;
  int port, listening;
  pl_conn *c;              // set once, as it opens, and freed with it
  buf waiting;             // bytes come and not yet taken, or for a listener each connection come in, a pl_conn *
  buf why;                 // why it didn't open
  volatile int news;       // set as it opens or ends and as anything comes, for lingo_wait
  int ended, finished, left;
} cn;

#define CN_HELD (4 << 20)   // the most kept waiting before reading pauses
static cn *cns;
static long cns_made;

static void cn_free(cn *k) {
  if (k->listening)
    for (size_t i = 0; i + sizeof(pl_conn *) <= k->waiting.n; i += sizeof(pl_conn *)) {
      pl_conn *c;
      memcpy(&c, k->waiting.s + i, sizeof c);
      pl_drop(c);
    }
  if (k->c) pl_drop(k->c);
  buf_free(&k->waiting);
  buf_free(&k->why);
  free(k->host);
  free(k);
}

static int cn_left(cn *k) {
  pl_enter(core_gate);
  int left = k->left;
  pl_leave(core_gate);
  return left;
}

static void pull(void *arg) {
  cn *k = arg;
  buf in = { 0 };
  buf from = { 0 };
  pl_conn *c = k->c;
  if (!c) c = k->listening ? pl_bind(k->kind, k->port, 0, &k->why) : pl_dial(k->kind, k->host, k->port, &k->why);
  pl_enter(core_gate);
  k->c = c;
  if (c && k->left) pl_shut(c);
  pl_leave(core_gate);
  lingo_done(&k->news);
  while (c && !cn_left(k)) {
    pl_conn *came = NULL;
    if (k->listening && !(came = pl_next(c, &from, 1))) continue;
    if (!k->listening && !pl_get(c, &in, 0)) break;
    pl_enter(core_gate);
    if (came) buf_add(&k->waiting, (const char *)&came, sizeof came);
    if (!came) buf_add(&k->waiting, in.s, in.n);
    int full = k->waiting.n > CN_HELD;
    pl_leave(core_gate);
    lingo_done(&k->news);
    in.n = 0;
    from.n = 0;
    while (full && !cn_left(k)) {
      pl_sleep(0.01);
      pl_enter(core_gate);
      full = k->waiting.n > CN_HELD;
      pl_leave(core_gate);
    }
  }
  buf_free(&in);
  buf_free(&from);
  pl_enter(core_gate);
  k->ended = 1;
  pl_leave(core_gate);
  lingo_done(&k->news);
  pl_enter(core_gate);
  k->finished = 1;
  int left = k->left;
  pl_leave(core_gate);
  if (left) cn_free(k);
}

static void cn_let_go(cn *k) {
  cn **at = &cns;
  while (*at != k) at = &(*at)->next;
  *at = k->next;
  pl_enter(core_gate);
  k->left = 1;
  if (k->c) pl_shut(k->c);
  int finished = k->finished;
  pl_leave(core_gate);
  if (finished) cn_free(k);
}

static cn *cn_of(run *r, val *id, int listening) {
  cn *k = cns;
  while (k && !(id->k == V_NUM && k->id == id->n)) k = k->next;
  if (!k) fail(r, "phrase", "no connection %s is open in this run", text_of(r, id)->t.s);
  if (k->listening != listening)
    fail(r, "phrase", listening ? "that's a connection: send to it and receive from it" : "that's a listener: accept from it");
  return k;
}

// One started and its thread waited for until it opens or can't. c is one already open, come in to a listener.
static long cn_start(run *r, const char *kind, const char *host, int port, int listening, pl_conn *c) {
  cn *k = calloc(1, sizeof *k);
  k->id = ++cns_made;
  snprintf(k->kind, sizeof k->kind, "%s", kind);
  k->host = strdup(host);
  k->port = port;
  k->listening = listening;
  k->c = c;
  k->next = cns;
  cns = k;
  pl_thread(pull, k);
  while (!k->news && !*r->stop) lingo_wait(&k->news, r->stop, 1);
  pl_enter(core_gate);
  int open = k->c != NULL;
  pl_leave(core_gate);
  if (open) return k->id;
  val *why = v_text(r->a, k->why.s ? k->why.s : "", k->why.n);
  cn_let_go(k);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  fail(r, "phrase", "%s", why->t.s);
  return 0;
}

// kind://host:port into its parts, a host in brackets for IPv6, like tcp://[::1]:80. An address with no kind is an Onsay
// address, a lingo on a computer, which isn't reached this way yet.
static void cn_address(run *r, val *address, char kind[8], buf *host, int *port) {
  const char *s = text_of(r, address)->t.s;
  const char *rest = strstr(s, "://");
  if (!rest) fail(r, "phrase", "a connection to a lingo on a computer, like %s, isn't built yet: give a kind, like tcp://%s", s, s);
  if (rest - s >= 8 || rest == s) fail(r, "phrase", "%s has no kind of connection this computer knows", s);
  memcpy(kind, s, (size_t)(rest - s));
  kind[rest - s] = 0;
  char words[16];
  snprintf(words, sizeof words, " %s ", kind);
  if (!strstr(" " PL_KINDS " ", words)) fail(r, "phrase", "this computer can't open %s connections", kind);
  rest += 3;
  const char *colon = rest[0] == '[' ? strstr(rest, "]:") : strrchr(rest, ':');
  if (rest[0] == '[' && colon) colon++;
  char *end = NULL;
  long n = colon ? strtol(colon + 1, &end, 10) : 0;
  if (!colon || !end || *end || n < 1 || n > 65535)
    fail(r, "phrase", "%s needs a port, like %s://example.com:80", s, kind);
  if (rest[0] == '[')
    buf_add(host, rest + 1, (size_t)(colon - rest - 2));
  else
    buf_add(host, rest, (size_t)(colon - rest));
  buf_chr(host, 0);
  *port = (int)n;
}

static val *connection_open(run *r, val **a) {
  char kind[8];
  buf host = { 0 };
  int port;
  cn_address(r, a[0], kind, &host, &port);
  val *where = v_cstr(r->a, host.s);
  buf_free(&host);
  return v_num(r->a, (double)cn_start(r, kind, where->t.s, port, 0, NULL));
}

// Only on this computer: what comes from elsewhere comes through the port, where the policies stand.
static val *connection_listen(run *r, val **a) {
  char kind[8];
  buf host = { 0 };
  int port;
  cn_address(r, a[0], kind, &host, &port);
  int here = !host.s[0] || !strcmp(host.s, "127.0.0.1") || !strcmp(host.s, "localhost");
  buf_free(&host);
  if (!here) fail(r, "phrase", "a lingo listens on this computer alone, like %s://127.0.0.1:%d", kind, port);
  return v_num(r->a, (double)cn_start(r, kind, "127.0.0.1", port, 1, NULL));
}

static val *connection_send(run *r, val **a) {
  cn *k = cn_of(r, a[0], 0);
  val *data = bytes_of(r, a[1]);
  if (!pl_put(k->c, data->t.s, data->t.len)) fail(r, "phrase", "the connection was closed");
  return NOTHING;
}

// Everything that came and wasn't taken, else what comes within seconds, else nothing. For a listener, the next
// connection come in, as a connection of this run.
static val *taken(run *r, cn *k, double seconds) {
  double until = pl_now() + (seconds >= 0 && seconds < 3600 ? seconds : 30);
  for (;;) {
    val *got = NULL;
    pl_conn *came = NULL;
    pl_enter(core_gate);
    if (k->listening && k->waiting.n >= sizeof came) {
      memcpy(&came, k->waiting.s, sizeof came);
      k->waiting.n -= sizeof came;
      memmove(k->waiting.s, k->waiting.s + sizeof came, k->waiting.n);
    } else if (!k->listening && k->waiting.n) {
      got = v_bytes(r->a, k->waiting.s, k->waiting.n);
      k->waiting.n = 0;
    }
    int ended = k->ended;
    if (!got && !came && !ended) k->news = 0;
    pl_leave(core_gate);
    if (came) return v_num(r->a, (double)cn_start(r, k->kind, "127.0.0.1", k->port, 0, came));
    if (got) return got;
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    if (ended) fail(r, "phrase", "the connection was closed");
    if (pl_now() >= until) return NOTHING;
    lingo_wait(&k->news, r->stop, until - pl_now());
  }
}

static val *connection_receive(run *r, val **a) { return taken(r, cn_of(r, a[0], 0), a[1]->k == V_NUM ? a[1]->n : 30); }

static val *connection_accept(run *r, val **a) { return taken(r, cn_of(r, a[0], 1), a[1]->k == V_NUM ? a[1]->n : 30); }

static val *connection_close(run *r, val **a) {
  cn *k = cns;
  while (k && !(a[0]->k == V_NUM && k->id == a[0]->n)) k = k->next;
  if (!k) fail(r, "phrase", "no connection %s is open in this run", text_of(r, a[0])->t.s);
  cn_let_go(k);
  return NOTHING;
}

/* lingo(...) and use(...) as a program reaches them, in, with for, the request it runs for: asked of the running native
   lingo that answers them, which waits, else POSTed to the settings' ask, a web address that answers the same way. */
static val *core_asked(run *r, buf *in) {
  buf out = { 0 };
  buf why = { 0 };
  size_t k = 1;
  const char *at = text_or(set.all, "ask", NULL);
  val *got = NULL;
  val *answer;
  val *text;
  if (job.id) {
    in->n--;
    buf_str(in, ",\"for\":");
    buf_str(in, job.id);
    buf_chr(in, '}');
  }
  while (k < 64 && !(on[k] && hooks[k].ask)) k++;
  if (k < 64 && hooks[k].ask(in->s, in->n, r->stop, &out))
    got = json_parse(r->a, out.s ? out.s : "", out.n, NULL);
  else if (at) {
    val *headers = v_rec(r->a);
    rec_set(r->a, headers, "content-type", v_cstr(r->a, "application/json"));
    if ((at = reached(r->a, at, 0, &why)) &&
        (answer = pl_http(r->a, "POST", at, headers, in->s, in->n, set.ms / 1000, &why)) &&
        (text = rec_get(answer, "text")) && text->k == V_TEXT)
      got = json_parse(r->a, text->t.s, text->t.len, NULL);
  }
  const char *problem = why.n ? v_text(r->a, why.s, why.n)->t.s : "This lingo client isn't connected to Onsay";
  buf_free(in);
  buf_free(&out);
  buf_free(&why);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  if (!got || got->k != V_REC) fail(r, "phrase", "%s", problem);
  const char *error = text_or(got, "error", NULL);
  if (error) fail(r, text_or(got, "code", "phrase"), "%s", error);
  return rec_get(got, "value") ? rec_get(got, "value") : NOTHING;
}

// lingo.h: a line said by the running request, sent on by the running native lingo that sends lines on, if any.
int lingo_said(const char *text) {
  size_t k = 1;
  while (k < 64 && !(on[k] && hooks[k].said)) k++;
  return k < 64 && job.id && hooks[k].said(job.id, text);
}

const char *lingo_job(void) { return job.id; }

/* A request of the lingo function, with what was around it (pl_context) and the function asking, never the answer
   itself. gives is what lingo.run says it wants, NULL for lingo.ways, which asks for the ways, nothing run. */
static val *lingo_asked(run *r, val *ask, val *values, val *gives) {
  buf in = { 0 };
  buf_str(&in, "{\"ask\":");
  buf_json(&in, ask);
  buf_str(&in, ",\"values\":");
  buf_json(&in, values);
  if (gives) {
    buf_str(&in, ",\"gives\":");
    buf_json(&in, gives);
  } else
    buf_str(&in, ",\"mode\":\"list\"");
  buf_str(&in, ",\"context\":");
  pl_context(&in);
  if (job.fn) {
    buf_str(&in, ",\"asking\":");
    buf_json_str(&in, job.fn, strlen(job.fn));
  }
  buf_chr(&in, '}');
  return core_asked(r, &in);
}

/* Whether a call that is missing or fails may ask the lingo search: never inside a try, which wants the error, never in a
   request Onsay itself sent, so a search never asks itself back, and never for lingo(...) and use(...) themselves. */
static int searchable(run *r, const char *id) {
  return !r->catching && strcmp(job.caller ? job.caller : "", "onsay") && strncmp(id, "lingo.", 6);
}

/* A call this computer hasn't got (code unknown), or one whose phrase failed (code phrase), with its values: asked of the
   lingo search, which finds a way and gives back its answer, else the error it always was, message. Offline, or with the
   onsay lingo's setting search_calls false, it is that error at once. */
static val *searched(run *r, const char *id, val *values, const char *code, const char *message) {
  val *off = lingo_setting(r->a, "onsay", "search_calls");
  int no = off && (off->k == V_BOOL ? !truthy(off) : off->k == V_TEXT && !strcmp(off->t.s, "false"));
  if (no || !searchable(r, id) || !onsay_ready()) fail(r, code, "%s", message);
  val *with = v_rec(r->a);
  rec_set(r->a, with, "values", values);
  const char *ask = strcmp(code, "unknown") ? arena_fmt(r->a, "%s, which failed here: %s", id, message) : id;
  trap_t t;
  void *volatile prev = r->trap;
  volatile int depth = r->depth;
  const char *volatile lingo = r->lingo;
  r->trap = &t;
  if (!setjmp(t.j)) {
    val *out = lingo_asked(r, v_cstr(r->a, ask), with, v_cstr(r->a, ""));
    r->trap = prev;
    return out;
  }
  r->trap = prev;
  r->depth = depth;
  r->lingo = lingo;
  if (!strcmp(r->code, "stopped") || !strcmp(r->code, "limit") || !strcmp(r->code, "denied")) rethrow(r);
  fail(r, code, "%s", message);
  return NOTHING;
}

// lingo.run(ask, values, gives).
static val *lingo_run(run *r, val **a) { return lingo_asked(r, a[0], a[1], a[2]); }

// lingo.ways(ask, values).
static val *lingo_ways(run *r, val **a) { return lingo_asked(r, a[0], a[1], NULL); }

// lingo.use(address, values).
static val *lingo_use(run *r, val **a) {
  buf in = { 0 };
  buf_str(&in, "{\"address\":");
  buf_json(&in, a[0]);
  buf_str(&in, ",\"values\":");
  buf_json(&in, a[1]);
  buf_chr(&in, '}');
  return core_asked(r, &in);
}

static val *setting_(run *r, val **a), *settings_get(run *r, val **a), *settings_set(run *r, val **a);   // 7.
static val *lingo_list(run *r, val **a), *lingo_code(run *r, val **a), *lingo_save(run *r, val **a);   // 8.

#ifndef __ANDROID__
#if defined(__aarch64__) && !defined(_WIN32)
// libffi's arm64 closures want this from a part of it not in vendor/. native.call never makes a closure.
void *ffi_data_to_code_pointer(void *data) { return data; }
#endif

// A layout name to its libffi type. text and cstring are a pointer; a number is sent and read as that width.
static ffi_type *native_type(const char *name) {
  if (!strcmp(name, "none")) return &ffi_type_void;
  if (!strcmp(name, "i8")) return &ffi_type_sint8;
  if (!strcmp(name, "u8")) return &ffi_type_uint8;
  if (!strcmp(name, "i16")) return &ffi_type_sint16;
  if (!strcmp(name, "u16")) return &ffi_type_uint16;
  if (!strcmp(name, "i32")) return &ffi_type_sint32;
  if (!strcmp(name, "u32")) return &ffi_type_uint32;
  if (!strcmp(name, "i64")) return &ffi_type_sint64;
  if (!strcmp(name, "u64")) return &ffi_type_uint64;
  if (!strcmp(name, "f32")) return &ffi_type_float;
  if (!strcmp(name, "f64")) return &ffi_type_double;
  if (!strcmp(name, "pointer") || !strcmp(name, "text") || !strcmp(name, "cstring")) return &ffi_type_pointer;
  return NULL;
}

// One argument's room, big enough for any scalar or a pointer, and its address is what libffi is given.
typedef union {
  int8_t i8;
  uint8_t u8;
  int16_t i16;
  uint16_t u16;
  int32_t i32;
  uint32_t u32;
  int64_t i64;
  uint64_t u64;
  float f32;
  double f64;
  void *ptr;
} native_slot;

// A number put into a slot as the layout's width, or a text's bytes as a pointer.
static void native_put(run *r, native_slot *s, const char *t, val *v) {
  if (!strcmp(t, "text") || !strcmp(t, "cstring")) {
    s->ptr = (void *)text_of(r, v)->t.s;
    return;
  }
  if (!strcmp(t, "pointer")) {
    s->ptr = (void *)(uintptr_t)num_of(r, v, "a pointer");
    return;
  }
  double n = num_of(r, v, "an argument");
  if (!strcmp(t, "i8"))
    s->i8 = (int8_t)n;
  else if (!strcmp(t, "u8"))
    s->u8 = (uint8_t)n;
  else if (!strcmp(t, "i16"))
    s->i16 = (int16_t)n;
  else if (!strcmp(t, "u16"))
    s->u16 = (uint16_t)n;
  else if (!strcmp(t, "i32"))
    s->i32 = (int32_t)n;
  else if (!strcmp(t, "u32"))
    s->u32 = (uint32_t)n;
  else if (!strcmp(t, "i64"))
    s->i64 = (int64_t)n;
  else if (!strcmp(t, "u64"))
    s->u64 = (uint64_t)n;
  else if (!strcmp(t, "f32"))
    s->f32 = (float)n;
  else if (!strcmp(t, "f64"))
    s->f64 = n;
}

/* native.call(library, function, returns, args): the floor's one call into the system, any function of it or of a
   library by name and the shape of what it takes and returns, arranged by libffi (vendor/libffi). Not on Android yet:
   its 32-bit chips' parts of libffi aren't in vendor/. */
static val *native_call(run *r, val **a) {
  const char *library = text_of(r, a[0])->t.s;
  const char *function = text_of(r, a[1])->t.s;
  const char *ret = text_of(r, a[2])->t.s;
  val *args = a[3] && a[3]->k == V_LIST ? a[3] : NULL;
  size_t n = args ? args->l.n : 0;
  ffi_type *rtype = native_type(ret);
  if (!rtype) fail(r, "phrase", "native.call doesn't know the return type %s", ret);
  void *sym = pl_symbol(library, function);
  if (!sym) fail(r, "phrase", "%s was not found%s%s", function, library[0] ? " in " : "", library);
  ffi_type **atypes = arena_get(r->a, (n ? n : 1) * sizeof *atypes);
  void **avalues = arena_get(r->a, (n ? n : 1) * sizeof *avalues);
  native_slot *slots = arena_get(r->a, (n ? n : 1) * sizeof *slots);
  for (size_t i = 0; i < n; i++) {
    val *arg = args->l.v[i];
    if (arg->k != V_REC) fail(r, "phrase", "each argument is a record like {type: \"i32\", value: 5}");
    const char *t = text_of(r, rec_get(arg, "type") ? rec_get(arg, "type") : NOTHING)->t.s;
    atypes[i] = native_type(t);
    if (!atypes[i] || atypes[i] == &ffi_type_void)
      fail(r, "phrase", "native.call doesn't know the argument type %s", t);
    native_put(r, &slots[i], t, rec_get(arg, "value") ? rec_get(arg, "value") : NOTHING);
    avalues[i] = &slots[i];
  }
  ffi_cif cif;
  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, atypes) != FFI_OK)
    fail(r, "phrase", "native.call couldn't arrange the call to %s", function);
  union {
    ffi_arg i;
    float f32;
    double f64;
    void *ptr;
  } out;
  ffi_call(&cif, FFI_FN(sym), &out, avalues);
  if (!strcmp(ret, "none")) return NOTHING;
  if (!strcmp(ret, "f32")) return v_num(r->a, out.f32);
  if (!strcmp(ret, "f64")) return v_num(r->a, out.f64);
  if (!strcmp(ret, "cstring")) return out.ptr ? v_cstr(r->a, (const char *)out.ptr) : NOTHING;
  if (!strcmp(ret, "pointer")) return v_num(r->a, (double)(uintptr_t)out.ptr);
  if (!strcmp(ret, "i8")) return v_num(r->a, (int8_t)out.i);
  if (!strcmp(ret, "i16")) return v_num(r->a, (int16_t)out.i);
  if (!strcmp(ret, "i32")) return v_num(r->a, (int32_t)out.i);
  if (!strcmp(ret, "i64")) return v_num(r->a, (double)(int64_t)out.i);
  if (!strcmp(ret, "u64")) return v_num(r->a, (double)(uint64_t)out.i);
  return v_num(r->a, (double)(uint64_t)(uint32_t)out.i);   // u8, u16, u32
}
#endif

// badge.read(): who called the request running, and the data of every grant of the badge it came with, a later grant's
// written over an earlier's.
static val *badge_read(run *r, val **v) {
  val *out = v_rec(r->a);
  val *data = v_rec(r->a);
  (void)v;
  for (size_t i = 0; job.badge && i < job.badge->l.n; i++) {
    val *d = rec_get(job.badge->l.v[i], "data");
    for (size_t k = 0; d && d->k == V_REC && k < d->r.n; k++) rec_set(r->a, data, d->r.key[k], d->r.v[k]);
  }
  rec_set(r->a, out, "caller", v_cstr(r->a, job.caller ? job.caller : "this computer"));
  rec_set(r->a, out, "data", data);
  return out;
}

// lingo.run, lingo.use and lingo.setting are lingo(...), use(...) and setting(...), the language's own, in no listing.
static void floor_phrases(void) {
  phrase_add("badge", "none",
      "read() -> {caller: text, data: record}: who called the request running, and the data of the badge it came with, "
      "a later grant's written over an earlier's",
      badge_read);
  phrase_add("store", "store", "get(key: text) -> text?: Onsay's own storage on this computer", get);
  phrase_add("store", "store", "put(key: text, value: any): kept as text", core_put);
  phrase_add("store", "store", "keys(prefix: text=\"\") -> [text]: sorted", keys);
  phrase_add("store", "store", "delete(key: text)", delete_);
  phrase_add("http", "net",
      "send(method: text, url: url, headers: record=nothing, body: text=nothing, timeout: number=30) -> {status: whole, headers: record, text: text}: one request, "
      "headers a record, body text, redirects followed, at most 4 MB back",
      core_send);
  phrase_add("socket", "net",
      "open(url: url, headers: record=nothing) -> whole: a WebSocket to a ws:// or wss:// address or an Onsay name, its "
      "number for the other socket phrases. Closed when the run ends, if not before",
      socket_open);
  phrase_add("socket", "net", "send(socket: whole, text: text): one message", socket_send);
  phrase_add("socket", "net",
      "receive(socket: whole, seconds: number=30) -> text?: the next message, or nothing when none came in seconds",
      socket_receive);
  phrase_add("socket", "net", "close(socket: whole)", socket_close);
  phrase_add("connection", "net",
      "open(address: text) -> whole: bytes over a connection, its kind first, like tcp://example.com:80 or udp://10.0.0.5:53, "
      "its number for the other connection phrases. Closed when the run ends, if not before",
      connection_open);
  phrase_add("connection", "net", "send(connection: whole, data: bytes): all of it, text as its UTF-8 bytes, for udp one packet",
      connection_send);
  phrase_add("connection", "net",
      "receive(connection: whole, seconds: number=30) -> bytes?: everything come and not yet taken, else what comes within "
      "seconds, else nothing",
      connection_receive);
  phrase_add("connection", "net", "close(connection: whole)", connection_close);
#ifndef __ANDROID__
  phrase_add("native", "run",
      "call(library: text, function: text, returns: text, args: list=nothing) -> any: calls any function of this "
      "computer's system or a library by name. library is its file, like \"kernel32.dll\" or \"libc.so.6\", or \"\" for "
      "this program. returns and each argument's type is one of i8 u8 i16 u16 i32 u32 i64 u64 (whole numbers), f32 f64 "
      "(decimals), pointer, text, cstring, or none. args is a list of {type, value}",
      native_call);
#endif
  phrase_add("connection", "net",
      "listen(address: text) -> whole: connections coming in on this computer, like tcp://127.0.0.1:7000, its number for "
      "connection.accept",
      connection_listen);
  phrase_add("connection", "net",
      "accept(listener: whole, seconds: number=30) -> whole?: the next connection come in, else nothing within seconds",
      connection_accept);
  phrase_add("settings", "none",
      "get(lingo: text) -> [record]: every setting of a lingo, as its openapi.json and its code say, each "
      "with its value now",
      settings_get);
  phrase_add("settings", "none", "set(lingo: text, key: text, value: any): kept wherever that lingo keeps its settings", settings_set);
  phrase_add("lingo", "none", "run(ask: text, values: record=nothing, gives: text=\"\")", lingo_run);
  phrase_add("lingo", "none",
      "ways(ask: text, values: record=nothing) -> [{does: text, where: text, address: url, inputs: record, needs?: [text]}]: the best ways to do it, best first, nothing run",
      lingo_ways);
  phrase_add("lingo", "none", "use(address: url, values: record=nothing)", lingo_use);
  phrase_add("lingo", "none", "setting(key: text, reference: text)", setting_);
  phrase_add("lingo", "none",
      "list() -> [{name: text, about: text, functions: [{name: text, does: text}]}]: every lingo running on this computer "
      "but those built into it, what it is and its functions",
      lingo_list);
  phrase_add("lingo", "none",
      "code(name: text) -> {code: text, openapi: record}: a lingo of this computer as its maker writes it, to change it: its "
      "code, and its openapi.json's info and x-lingo, such as its settings",
      lingo_code);
  phrase_add("lingo", "lingos",
      "save(name: text, code: text, openapi: record=nothing) -> [{name: text, does: text}]: a lingo written or changed and "
      "running on this computer at once, as a folder of its code and, when given, its openapi.json: its functions, or why "
      "it can't run, like its code's first mistake",
      lingo_save);
}

////////// 3. LINGOS //////////

/* Core, and the lingos built into this program for its platform (BUILTINS), are always there and always running. Every
   other is a .lingo file in the lingos folder, a zip of its openapi.json and its program, or the same files in a folder
   of its name, as a lingo is written: its program is <name>.ls, in Lingo, or a native library, <name>.dll or <name>.so.
   One that can't load is set aside with its reason, and the rest still load. A lingo from the folder is running, its
   routes answering and in the catalog, or stopped, as home/running.json says. */
static arena *library;
static int serving;   // how many requests are being answered, one inside another's wait
static arena *retired[16];   // what lingos were read into while a request ran, freed once none runs
static int retiring;
static val *device;   // the compiler's instance of every lingo that loaded
static val *found;   // every lingo read: {name, from, manifest, program, file and code or library,
    // native, running, fresh, error}. fresh: its paths are made from its code
static val *catalog;   // every running lingo, with its manifest or why it can't load
static unsigned long long looked;   // a fingerprint of the lingos folder and running.json when they were last read
#define FNV 14695981039346656037ULL

/* The lingos built into this program, each from ../lingos/<name>: its name, its openapi.json as text, and the function
   in its C that adds its phrases. builtins.h is made by build.ps1:
   onsay alone today, on every platform. Each is never read from the lingos folder, always runs, and can't be stopped. */
typedef struct {
  const char *name;
  const char *manifest;
  lingo_entry entry;
} builtin;
#include "builtins.h"
#define BUILTIN "built in"   // what a built-in lingo's from and program say

// A call of the onsay lingo no request ever reaches: what would sign this computer out, reach a setting's value by its
// lingo's reference, or what a caller can as well do itself.
static int private_call(const char *id) {
  return !strcmp(id, "onsay.sign_in") || !strcmp(id, "onsay.sign_out") || !strcmp(id, "lingo.setting") ||
      !strcmp(id, "lingo.use");
}

/* The standard functions every lingo can call with nothing to install, like text.split, list.sort and request.get, from
   ../lingos/standard: each group's name and its Lingo code (standard.h, made by build.ps1). They compile with every lingo
   and run as part of the lingo calling them, under its permissions, and are no app: no route, no catalog entry, no switch. */
typedef struct {
  const char *name;
  const char *code;
} standard;
#include "standard.h"

// Whether name is a group of standard functions.
static int is_standard(const char *name) {
  for (size_t i = 0; i < sizeof STANDARD / sizeof *STANDARD; i++) {
    if (!strcmp(STANDARD[i].name, name)) return 1;
  }
  return 0;
}

// The built-in lingo of that name, or NULL.
static const builtin *builtin_of(const char *name) {
  for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
    if (!strcmp(BUILTINS[i].name, name)) return &BUILTINS[i];
  }
  return NULL;
}

// Every native lingo by its number, core first, by its name. A library is loaded once and kept, since its phrases are in
// the table from then on: a changed one is used after a restart.
static const char *natives[64] = { "core" };
static size_t native_total = 1;

const char *native_name(size_t lingo) { return lingo < native_total ? natives[lingo] : "core"; }

static unsigned long long fnv(unsigned long long h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void print_file(const char *name, int folder, double size, double changed, void *arg) {
  unsigned long long *h = arg;
  *h = fnv(*h, name, strlen(name));
  *h = fnv(*h, &folder, sizeof folder);
  *h = fnv(*h, &size, sizeof size);
  *h = fnv(*h, &changed, sizeof changed);
}

typedef struct {
  arena *a;
  val *names;
  unsigned long long *print;
} seen;

// Each folder, and each .lingo file, fingerprinted as it is listed.
static void collect(const char *name, int folder, double size, double changed, void *arg) {
  seen *f = arg;
  size_t len = strlen(name);
  if (name[0] == '.' || (!folder && (len <= 6 || strcmp(name + len - 6, ".lingo")))) return;
  list_push(f->a, f->names, v_cstr(f->a, name));
  print_file(name, folder, size, changed, f->print);
}

// What is in the lingos folder now, and a fingerprint of every file of it, names, sizes and times, and of running.json,
// so any change shows, a lingo started or stopped from the command line too.
static val *look(arena *a, unsigned long long *print) {
  buf path = { 0 };
  buf running = { 0 };
  *print = FNV;
  seen f = { a, v_list(a), print };
  pl_list(set.lingos, collect, &f);
  for (size_t i = 0; i < f.names->l.n; i++) {
    path.n = 0;
    buf_fmt(&path, "%s/%s", set.lingos, f.names->l.v[i]->t.s);
    pl_list(path.s, print_file, print);   // nothing for a file
  }
  path.n = 0;
  buf_fmt(&path, "%s/running.json", set.home);
  if (pl_read(path.s, &running)) *print = fnv(*print, running.s, running.n);
  buf_free(&path);
  buf_free(&running);
  return f.names;
}

// Sets one aside, with why.
static void set_aside(arena *a, val *e, const char *why) {
  rec_set(a, e, "error", v_cstr(a, why));
  say("Lingo %s can't be used: %s", text_or(e, "from", ""), why);
}

// One file of a lingo, from its .lingo file's zip, or from its folder when zip is NULL.
static int lingo_piece(const char *path, buf *zip, const char *file, buf *out) {
  buf at = { 0 };
  buf_fmt(&at, "%s/%s", path, file);
  int got = zip ? zip_file(zip->s, zip->n, file, out) : pl_read(at.s, out);
  buf_free(&at);
  return got;
}

static int native_called(const char *name) {
  for (size_t i = 0; i < native_total; i++)
    if (!strcmp(natives[i], name)) return (int)i;
  return -1;
}

/* 3. One lingo, from its .lingo file or a folder of its files, at path: {name, from, manifest, program, and file and code
   or library}, with error when it can't be used. NULL for a folder with neither an openapi.json nor a program, which
   isn't a lingo. Its program is the one its openapi.json's x-lingo names, else whichever of <name>.ls, <name>.dll and
   <name>.so is there. A library in a .lingo file is put in home/libraries to be loaded, unless it is loaded already. */
static val *read_lingo(arena *a, const char *path) {
  const char *base = path + strlen(path);
  const char *problem = NULL;
  while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
  size_t len = strlen(base);
  int zipped = len > 6 && !strcmp(base + len - 6, ".lingo");
  val *e = v_rec(a);
  val *m = NULL;
  val *x;
  val *settings;
  val *keep;
  buf zip = { 0 };
  buf text = { 0 };
  buf program = { 0 };
  buf code = { 0 };
  rec_set(a, e, "name", v_text(a, base, zipped ? len - 6 : len));
  rec_set(a, e, "from", v_cstr(a, base));
  const char *name = text_or(e, "name", "");
  const char *own = text_or(e, "from", "");
  const char *route;
  const char *wanted;
  const char *programs[] = { arena_fmt(a, "%s.ls", name), arena_fmt(a, "%s.dll", name), arena_fmt(a, "%s.so", name) };
  if (zipped) pl_read(path, &zip);
  int manifest = lingo_piece(path, zipped ? &zip : NULL, "openapi.json", &text);
  int k = 0;
  m = manifest ? json_parse(a, text.s, text.n, &problem) : v_rec(a);
  x = rec_get(m, "x-lingo");
  settings = rec_get(x, "settings");
  keep = rec_get(x, "keep");
  route = text_or(x, "route", NULL);
  wanted = text_or(x, "program", NULL);
  while (k < 3 && !((!wanted || !strcmp(wanted, programs[k])) && lingo_piece(path, zipped ? &zip : NULL, programs[k], &code)))
    k++;
  if (k < 3) buf_str(&program, programs[k]);
  rec_set(a, e, "manifest", m ? m : v_rec(a));

  int listed = !settings || settings->k == V_LIST;
  for (size_t i = 0; listed && settings && i < settings->l.n; i++)
    listed = settings->l.v[i]->k == V_TEXT || text_or(settings->l.v[i], "name", NULL);
  if (!zipped && !manifest && !program.n)
    e = NULL;
  else if (zipped && !manifest)
    set_aside(a, e, "it isn't a .lingo file: a zip of its openapi.json and its program, made with pack");
  else if (problem || m->k != V_REC)
    set_aside(a, e, "its openapi.json isn't a JSON record");
  else if (!named(name))
    set_aside(a, e, "its name is lower case letters, digits, _ and -, like notes, and never lingo");
  else if (text_or(x, "name", NULL) && strcmp(text_or(x, "name", ""), name))
    set_aside(a, e, arena_fmt(a, "its openapi.json calls it %s, and its file %s", text_or(x, "name", ""), own));
  else if (wanted && !program.n)
    set_aside(a, e,
        arena_fmt(a, "its openapi.json's program, %s, isn't beside it: it is %s.ls, or a library like %s.dll", wanted,
            name, name));
  else if (!program.n)
    set_aside(a, e,
        arena_fmt(
            a, "its program goes beside its openapi.json, named for it: %s.ls, or a library like %s.dll", name, name));
  else if (!listed)
    set_aside(a, e,
        "its settings are a list, each a name or a record with its name, like {\"name\": \"key\", \"secret\": true}");
  else if (keep && (!text_or(keep, "read", NULL) || !text_or(keep, "write", NULL)))
    set_aside(a, e,
        "keep names its own functions that read and write its settings, like {\"read\": \"read\", \"write\": \"write\"}");
  else if (route &&
      (route[0] != '/' || !route[1] || strpbrk(route, "?# ") || route[strlen(route) - 1] == '/' ||
          !strncmp(route, "/.lingo", 7)))
    set_aside(a, e, "its route is a path of its own, like /onsay");
  else {
    rec_set(a, e, "program", v_text(a, program.s, program.n));
    if (k == 0) {
      rec_set(a, e, "file", v_text(a, program.s, program.n));
      rec_set(a, e, "code", v_text(a, code.s ? code.s : "", code.n));
    } else if (!zipped)
      rec_set(a, e, "library", v_cstr(a, arena_fmt(a, "%s/%s", path, program.s)));
    else {
      // Written only when it changed, so a library another lingo client here has loaded is left as it is.
      buf at = { 0 };
      buf now = { 0 };
      buf_fmt(&at, "%s/libraries/%s", set.home, name);
      pl_folder(at.s);
      buf_fmt(&at, "/%s", program.s);
      int same = pl_read(at.s, &now) && now.n == code.n && !memcmp(now.s, code.s, code.n);
      if (native_called(name) < 0 && !same && !pl_write(at.s, code.s, code.n))
        set_aside(a, e, arena_fmt(a, "its library couldn't be put in %s", at.s));
      else
        rec_set(a, e, "library", v_text(a, at.s, at.n));
      buf_free(&at);
      buf_free(&now);
    }
  }
  buf_free(&zip);
  buf_free(&text);
  buf_free(&program);
  buf_free(&code);
  return e;
}

// A lingo read, by its name, or NULL.
static val *entry_of(const char *name) {
  for (size_t i = 0; found && i < found->l.n; i++)
    if (!strcmp(text_or(found->l.v[i], "name", ""), name)) return found->l.v[i];
  return NULL;
}

static val *manifest_of(const char *lingo) { return rec_get(entry_of(lingo), "manifest"); }

// Where a lingo answers: the route its manifest's x-lingo names, like /onsay, else /<name>.
static const char *route_of(arena *a, val *e) {
  const char *own = text_or(rec_get(rec_get(e, "manifest"), "x-lingo"), "route", NULL);
  return own ? own : arena_fmt(a, "/%s", text_or(e, "name", ""));
}

// The functions a lingo shares at its routes: serve alone when it has serve(request), else every one.
static val *shared(arena *a, const char *name) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), name), "phrases");
  val *out = v_list(a);
  const char *serve = arena_fmt(a, "%s.serve", name);
  // The onsay lingo holds the base calls, its own and the core's, every one at /onsay beside its door for prompts.
  if (!strcmp(name, "onsay")) {
    val *lists[2] = { own, rec_get(rec_get(rec_get(device, "lingos"), "core"), "phrases") };
    for (int k = 0; k < 2; k++)
      for (size_t i = 0; lists[k] && i < lists[k]->l.n; i++)
        if (!private_call(text_or(lists[k]->l.v[i], "id", ""))) list_push(a, out, lists[k]->l.v[i]);
    return out;
  }
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), serve)) {
      list_push(a, out, own->l.v[i]);
      return out;
    }
  for (size_t i = 0; own && i < own->l.n; i++) list_push(a, out, own->l.v[i]);
  return out;
}


/* 3. A lingo's whole OpenAPI manifest, as pack writes it: what its openapi.json says, its name, program and route in
   x-lingo, and each function it shares as a path under its route, its words, its values as JSON Schema, x-lingo-function,
   x-lingo-needs and x-lingo-calls, and x-lingo-internal on a helper, a function whose name starts with _, which Onsay
   lists nowhere. /name alone is its main, and a lingo with serve(request) shares serve
   alone, at its route. */
static val *described(arena *a, val *e) {
  const char *name = text_or(e, "name", "");
  const char *route = route_of(a, e);
  val *given = rec_get(e, "manifest");
  val *l = rec_get(rec_get(device, "lingos"), name);
  val *fns = shared(a, name);
  val *out = v_rec(a);
  val *info = v_rec(a);
  val *x = v_rec(a);
  val *paths = v_rec(a);
  val *was;
  for (size_t i = 0; (was = rec_get(given, "info")) && was->k == V_REC && i < was->r.n; i++)
    rec_set(a, info, was->r.key[i], was->r.v[i]);
  for (size_t i = 0; (was = rec_get(given, "x-lingo")) && was->k == V_REC && i < was->r.n; i++)
    rec_set(a, x, was->r.key[i], was->r.v[i]);
  if (!rec_get(info, "title")) rec_set(a, info, "title", v_cstr(a, name));
  if (!rec_get(info, "version")) rec_set(a, info, "version", v_cstr(a, "1.0.0"));
  if (!rec_get(info, "description") && rec_get(l, "about")) rec_set(a, info, "description", rec_get(l, "about"));
  rec_set(a, x, "name", v_cstr(a, name));
  rec_set(a, x, "program", rec_get(e, "program"));
  rec_set(a, x, "route", v_cstr(a, route));
  rec_set(a, out, "openapi", v_cstr(a, "3.1.0"));
  rec_set(a, out, "info", info);
  for (size_t i = 0; given && i < given->r.n; i++)
    if (strcmp(given->r.key[i], "openapi") && strcmp(given->r.key[i], "info") && strcmp(given->r.key[i], "paths") &&
        strcmp(given->r.key[i], "x-lingo"))
      rec_set(a, out, given->r.key[i], given->r.v[i]);
  for (size_t i = 0; i < fns->l.n; i++) {
    val *f = fns->l.v[i];
    val *op = v_rec(a);
    val *item = v_rec(a);
    val *says = rec_get(f, "does");
    val *schema = rec_get(f, "inputSchema");
    val *takes = rec_get(schema, "properties");
    const char *id = text_or(f, "id", "");
    const char *local = !strncmp(id, name, strlen(name)) && id[strlen(name)] == '.' ? id + strlen(name) + 1 : id;
    const char *does = text_or(f, "does", "");
    const char *words = does;
    // A lingo written in Lingo has no types in its code, so its openapi.json may say them, by function and value, its
    // answer as "->". A native's are in its signatures.
    val *own = rec_get(rec_get(rec_get(given, "x-lingo"), "types"), local);
    for (size_t k = 0; own && own->k == V_REC && takes && k < takes->r.n; k++) {
      val *type = rec_get(own, takes->r.key[k]);
      val *fallback = rec_get(takes->r.v[k], "default");
      if (!type || type->k != V_TEXT) continue;
      val *made = type_schema(a, type->t.s);
      if (fallback) rec_set(a, made, "default", fallback);
      takes->r.v[k] = made;
    }
    const char *gives = text_or(f, "gives", NULL);
    if (!gives && own && own->k == V_REC) gives = text_or(own, "->", NULL);
    int serve = !strcmp(local, "serve");
    buf path = { 0 };
    buf_str(&path, route);
    if (!serve && strcmp(local, "main")) {
      buf_chr(&path, '/');
      for (const char *c = local; *c; c++) buf_chr(&path, *c == '.' ? '/' : *c);
    }
    if (*words)
      rec_set(a, op, "summary",
          v_text(a, words, strstr(words, ". ") ? (size_t)(strstr(words, ". ") - words + 1) : strlen(words)));
    if (*does) rec_set(a, op, "description", says);
    if (!serve && takes && takes->k == V_REC && takes->r.n) {
      val *body = v_rec(a);
      val *content = v_rec(a);
      val *json = v_rec(a);
      rec_set(a, json, "schema", schema);
      rec_set(a, content, "application/json", json);
      rec_set(a, body, "content", content);
      rec_set(a, op, "requestBody", body);
    }
    val *responses = v_rec(a);
    val *ok = v_rec(a);
    rec_set(a, ok, "description",
        v_cstr(a,
            serve ? "What serve gives, for any method and any path under its route" : "The function's value, as JSON"));
    if (gives) {
      val *answer = v_rec(a);
      val *json = v_rec(a);
      rec_set(a, json, "schema", type_schema(a, gives));
      rec_set(a, answer, "application/json", json);
      rec_set(a, ok, "content", answer);
    }
    rec_set(a, responses, "200", ok);
    rec_set(a, op, "responses", responses);
    rec_set(a, op, "x-lingo-function", v_cstr(a, id));
    rec_set(a, op, "x-lingo-needs", rec_get(f, "needs") ? rec_get(f, "needs") : v_list(a));
    rec_set(a, op, "x-lingo-calls", rec_get(f, "lingos") ? rec_get(f, "lingos") : v_num(a, 0));
    if (truthy(rec_get(f, "internal") ? rec_get(f, "internal") : NOTHING) || (serve && !strcmp(name, "onsay")))
      rec_set(a, op, "x-lingo-internal", VBOOL(1));
    rec_set(a, item, serve ? "get" : "post", op);
    rec_set(a, paths, v_text(a, path.s, path.n)->t.s, item);
    buf_free(&path);
  }
  rec_set(a, out, "paths", paths);
  rec_set(a, out, "x-lingo", x);
  return out;
}

static val *declared(arena *a, const char *lingo, val **keep);   // 7.

/* Its manifest's x-lingo says missing, the names of its required settings with no value on this computer, never a value,
   so Onsay knows it can't run here yet. Settings a lingo keeps itself can't be seen without running it, so they count as
   set. */
static void say_missing(arena *a, val *e) {
  const char *name = text_or(e, "name", "");
  val *keep = NULL;
  val *all = declared(a, name, &keep);
  val *missing = v_list(a);
  if (keep) return;
  for (size_t i = 0; i < all->l.n; i++) {
    val *s = all->l.v[i];
    val *wanted = rec_get(s, "required");
    val *now = lingo_setting(a, name, text_or(s, "name", ""));
    if (!wanted || !truthy(wanted) || rec_get(s, "default")) continue;
    if (!now || now->k == V_NOTHING || (now->k == V_TEXT && !now->t.len)) list_push(a, missing, rec_get(s, "name"));
  }
  if (missing->l.n) rec_set(a, rec_get(rec_get(e, "manifest"), "x-lingo"), "missing", missing);
}

// Reads everything again: every lingo, the natives, the graph, the manifests and the catalog, with extra, a folder being
// packed, in place of any lingo of its name. Built beside what there was, which stays in use when even core's own lingos
// can't load now. 1 when it loaded.
static int load(const char *extra) {
  arena *was = library;
  val *was_device = device;
  val *was_catalog = catalog;
  val *was_found = found;
  arena *a = library = arena_new();
  val *names = look(a, &looked);
  val *stacked = v_list(a);
  val *offered = v_list(a);
  val *running = home_json(a, "running.json");
  val *more = NULL;
  found = v_list(a);

  // The built-in lingos first, so one in the folder with the same name is set aside.
  for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
    val *e = v_rec(a);
    rec_set(a, e, "name", v_cstr(a, BUILTINS[i].name));
    rec_set(a, e, "from", v_cstr(a, BUILTIN));
    rec_set(a, e, "program", v_cstr(a, BUILTIN));
    rec_set(a, e, "manifest", json_parse(a, BUILTINS[i].manifest, strlen(BUILTINS[i].manifest), NULL));
    rec_set(a, e, "running", VBOOL(1));
    list_push(a, found, e);
  }
  if (extra) {
    buf path = { 0 };
    buf_str(&path, extra);
    while (path.n > 1 && (path.s[path.n - 1] == '/' || path.s[path.n - 1] == '\\')) path.s[--path.n] = 0;
    if ((more = read_lingo(a, path.s))) rec_set(a, more, "fresh", VBOOL(1));
    buf_free(&path);
  }
  for (size_t i = 0; i <= names->l.n; i++) {
    val *e = more;
    if (i < names->l.n) {
      buf path = { 0 };
      buf_fmt(&path, "%s/%s", set.lingos, names->l.v[i]->t.s);
      e = read_lingo(a, path.s);
      buf_free(&path);
      if (e && more && !strcmp(text_or(e, "name", ""), text_or(more, "name", ""))) continue;
    }
    if (!e) continue;
    if (!rec_get(e, "error") && (builtin_of(text_or(e, "name", "")) || is_standard(text_or(e, "name", ""))))
      set_aside(a, e, "something built into this program is already called that");
    else if (!rec_get(e, "error") && entry_of(text_or(e, "name", "")))
      set_aside(a, e, "another lingo here is already called that");
    rec_set(a, e, "running", VBOOL(has(running, text_or(e, "name", ""))));
    list_push(a, found, e);
  }

  // core, then every native here, each library loaded the first time it is seen. One whose lingo is gone stays loaded,
  // but isn't offered.
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *name = text_or(e, "name", "");
    const char *path = text_or(e, "library", NULL);
    const builtin *built = strcmp(text_or(e, "from", ""), BUILTIN) ? NULL : builtin_of(name);
    if (rec_get(e, "error") || (!path && !built)) continue;
    int k = native_called(name);
    if (k < 0 && native_total == sizeof natives / sizeof *natives) {
      set_aside(a, e, "this core can't take more native lingos");
      continue;
    }
    if (k < 0) {
      lingo_entry entry = NULL;
      if (built)
        entry = built->entry;
      else {
        buf why = { 0 };
        entry = pl_library(path, &why);
        if (!entry) set_aside(a, e, arena_fmt(a, "%s: %s", text_or(e, "program", ""), why.s));
        buf_free(&why);
      }
      if (!entry) continue;
      natives[native_total] = strdup(name);   // kept for as long as the core runs, like the library itself
      native_begin(loading = native_total);
      entry();
      say("Lingo %s is loaded from %s", name, text_or(e, "program", ""));
      k = (int)native_total++;
    }
    rec_set(a, e, "native", v_num(a, k));
  }
  for (size_t i = 0; i < native_total; i++) {
    val *one = v_rec(a);
    val *e = i ? entry_of(natives[i]) : NULL;
    if (i && (!e || rec_get(e, "error") || !rec_get(e, "native"))) continue;
    rec_set(a, one, "name", v_cstr(a, natives[i]));
    rec_set(a, one, "phrases", native_phrases(a, i));
    list_push(a, offered, one);
  }
  for (size_t i = 0; i < sizeof STANDARD / sizeof *STANDARD; i++) {
    val *e = v_rec(a);
    rec_set(a, e, "name", v_cstr(a, STANDARD[i].name));
    rec_set(a, e, "file", v_cstr(a, arena_fmt(a, "%s.ls", STANDARD[i].name)));
    rec_set(a, e, "code", v_cstr(a, STANDARD[i].code));
    list_push(a, stacked, e);
  }
  for (size_t i = 0; i < found->l.n; i++)
    if (!rec_get(found->l.v[i], "error") && rec_get(found->l.v[i], "code")) list_push(a, stacked, found->l.v[i]);

  // Loaded together. While one is at fault, it is set aside with its reason, and the rest load again without it.
  for (;;) {
    const char *why = NULL;
    device = lingo_load(a, offered, stacked, &why);
    if (!why) break;
    size_t k = 0;
    for (; k < stacked->l.n; k++) {
      const char *f = text_or(stacked->l.v[k], "file", "");
      size_t n = strlen(f);
      if (!strncmp(why, f, n) && (why[n] == ':' || why[n] == ' ')) break;
    }
    if (k == stacked->l.n) {
      say("The core's own lingos can't load: %s", why);
      arena_free(a);
      library = was;
      device = was_device;
      catalog = was_catalog;
      found = was_found;
      return 0;
    }
    set_aside(a, stacked->l.v[k], why);
    memmove(&stacked->l.v[k], &stacked->l.v[k + 1], (stacked->l.n - k - 1) * sizeof *stacked->l.v);
    stacked->l.n--;
  }

  // Each lingo's whole manifest: its own when it says its paths, else made from what loaded. Then the catalog: every
  // running lingo by its name, with its manifest, or why it can't load. Never its code.
  val *all = rec_get(device, "lingos");
  catalog = v_list(a);
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    val *one = v_rec(a);
    val *error = rec_get(e, "error");
    if (!error && (!rec_get(rec_get(e, "manifest"), "paths") || rec_get(e, "fresh")))
      rec_set(a, e, "manifest", described(a, e));
    if (!truthy(rec_get(e, "running"))) continue;
    if (!error) say_missing(a, e);
    rec_set(a, one, "name", rec_get(e, "name"));
    rec_set(a, one, error ? "error" : "openapi", error ? error : rec_get(e, "manifest"));
    list_push(a, catalog, one);
  }
  say("%zu lingos are ready", all->r.n - 1);
  // A request running now may be inside a lingo read before, so what that was read into is kept till none runs.
  if (serving && retiring < (int)(sizeof retired / sizeof *retired))
    retired[retiring++] = was;
  else if (!serving)
    arena_free(was);

  // Each native lingo that runs, in a lingo client that serves, told the catalog, and each that stopped told so.
  buf now = { 0 };
  buf_json(&now, catalog);
  for (size_t i = 1; i < native_total; i++) {
    val *e = entry_of(natives[i]);
    int runs = live && e && !rec_get(e, "error") && truthy(rec_get(e, "running"));
    if (hooks[i].running && (runs || on[i])) hooks[i].running(runs ? now.s : NULL);
    on[i] = runs;
  }
  buf_free(&now);
  return 1;
}

// 3.
void lingo_hook(const lingo_hooks *h) { hooks[loading] = *h; }

////////// 4. POLICY //////////

/* Who may reach what: the policy alone decides. A request's caller is the owner, this computer's own, which reaches
   everything, or whoever its proof says, a signer or Onsay, or anyone at all, or a lingo running here, by its route like
   /weather, as each call of this computer's it makes is a request too: they reach what a grant in the policy, or the
   badge they came with, lets them, and anyone else gets a 404, as if it weren't there
   (../../onsay/docs/lingo-function.md, Badges and grants, and Built for low level). Every request goes in
   home/requests.log. */
#define ATTACHED 10   // seconds an app counts as open after it last asked for the questions waiting

// The questions waiting for the person's answer, each {id, lingo, caller, questions}, a native lingo's for the chat
// (lingo_ask), in the run's arena that waits.
static struct {
  val *ask;
  arena *a;
  volatile int done;
} asks[16];
static int waiting;   // how many wait, the last asked last
static long questions;   // their ids
static double polled;   // when an app last asked for them

#define OWN_GRANTS "[{\"to\":\"/\",\"allow\":[\"/\"]}]"   // this computer's own grants until it has a policy.json

/* 4. The policy in force, read as each request comes, so a change from the command line or by hand holds at once:
   {grants, own, device, local, account}. grants are the account's, home/policy.account.json, as Onsay keeps them; own
   are this computer's, the grants of home/policy.json, kept only here, which with none let its own lingos reach
   everything. local and account are the two files as they are. */
static val *policy(arena *a) {
  val *local = home_json(a, "policy.json");
  val *account = home_json(a, "policy.account.json");
  val *out = v_rec(a);
  val *own;
  if (!local || local->k != V_REC) local = v_rec(a);
  if (account && account->k != V_REC) account = NULL;
  own = rec_get(local, "grants");
  if (!own || own->k != V_LIST) own = json_parse(a, OWN_GRANTS, strlen(OWN_GRANTS), NULL);
  rec_set(a, out, "grants", account && rec_get(account, "grants") ? rec_get(account, "grants") : v_list(a));
  rec_set(a, out, "own", own);
  rec_set(a, out, "device", v_cstr(a, account ? text_or(account, "device", "") : ""));
  rec_set(a, out, "local", local);
  rec_set(a, out, "account", account ? account : NOTHING);
  return out;
}








// The key the running native lingo that looks keys up gives for keyid, {key, handle, account, mine}, or NULL.
static val *keyed(arena *a, const char *keyid) {
  for (size_t k = 1; k < 64; k++)
    if (on[k] && hooks[k].key) return hooks[k].key(a, keyid);
  return NULL;
}





/* One decision, {at, caller, lingo, route, need, allowed, why}. need is nothing for whether anyone but the owner may
   reach what they asked, sent as JSON to each running native lingo that wants it, its request's line in requests.log
   saying it too. need is the call, like files.read, for one of this computer's calls a lingo running here made, the
   caller that lingo by its route: a line of requests.log each, and sent on when refused. */
static void core_decided(const char *who, const char *route, const char *need, int allowed, const char *why) {
  arena *a = arena_new();
  val *line = v_rec(a);
  buf text = { 0 };
  char at[32];
  time_t now = (time_t)pl_now();
  strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  rec_set(a, line, "at", v_cstr(a, at));
  if (need) rec_set(a, line, "way", v_cstr(a, "in"));
  rec_set(a, line, "caller", v_cstr(a, who ? who : "this computer"));
  rec_set(a, line, "lingo", job.lingo ? v_cstr(a, job.lingo) : NOTHING);
  rec_set(a, line, "route", route ? v_cstr(a, route) : NOTHING);
  rec_set(a, line, "need", need ? v_cstr(a, need) : NOTHING);
  rec_set(a, line, "allowed", VBOOL(allowed));
  rec_set(a, line, "why", v_cstr(a, why));
  buf_json(&text, line);
  for (size_t i = 1; i < 64 && (!need || !allowed); i++)
    if (on[i] && hooks[i].decided) hooks[i].decided(text.s);
  if (need) requested(line);
  buf_free(&text);
  arena_free(a);
}

/* 4. Whether the lingo running may make id, a call of this computer's like files.read, a request to its address,
   /onsay/files/read, its caller the lingo by its route, like /weather, judged by the policy as anyone is: this
   computer's own grants let its own lingos reach everything until they say otherwise. At the command line, outside any
   request, it is the owner's. A refusal is a denied failure. */
void may(run *r, const char *id) {
  if (!job.lingo) return;
  val *p = job.policy ? job.policy : policy(r->a);
  const char *route = arena_fmt(r->a, "/onsay/%s", id);
  const char *why = NULL;
  caller c = { route_of(r->a, entry_of(job.lingo)), 0, 0, NULL };
  for (char *x = (char *)route + 7; *x; x++)
    if (*x == '.') *x = '/';
  int in = let_in(r->a, p, &c, route, 0, &why);
  core_decided(c.name, route, id, in, why);
  if (!in) fail(r, "denied", "%s can't use %s here: no grant lets it", job.lingo, id);
}

/* 4. A question for the person, with its id, the lingo asking and its caller, and waited for in asks until an app
      answers it, stop is set or seconds pass, while other requests are answered. Its answer, nothing when none came, or
      NULL when no app is open to ask. */
val *lingo_ask(run *r, val *question, double seconds) {
  if (pl_now() - polled > ATTACHED || waiting == 16) return NULL;
  int slot = waiting++;
  val *ask = v_rec(r->a);
  rec_set(r->a, ask, "id", v_num(r->a, (double)++questions));
  rec_set(r->a, ask, "lingo", v_cstr(r->a, job.lingo ? job.lingo : ""));
  for (size_t i = 0; i < question->r.n; i++) rec_set(r->a, ask, question->r.key[i], question->r.v[i]);
  rec_set(r->a, ask, "caller", v_cstr(r->a, job.caller ? job.caller : "this computer"));
  asks[slot].ask = ask;
  asks[slot].a = r->a;
  asks[slot].done = 0;
  lingo_wait(&asks[slot].done, r->stop, seconds);
  waiting--;
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  val *answer = rec_get(ask, "answer");
  return answer ? answer : NOTHING;
}

////////// 5. STOPS //////////

// Each request running, the newest on top, one run inside another's wait: its id, as JSON, so a stop can name it, and
// its stop flag. gate keeps them, since core_stop may set one from another thread while a request runs.
typedef struct frame {
  struct frame *up;
  char id[128];
  volatile int stop;
} frame;
static frame *top;

void core_say(const char *fmt_, ...) {
  va_list ap;
  va_start(ap, fmt_);
  fprintf(stderr, "onsay: ");
  vfprintf(stderr, fmt_, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// 5. At once, even while the request runs on another thread, with every request running inside its wait.
void core_stop(const char *id) {
  pl_enter(core_gate);
  frame *hit = top;
  while (hit && strcmp(hit->id, id)) hit = hit->up;
  for (frame *f = top; hit && f != hit->up; f = f->up) f->stop = 1;
  pl_leave(core_gate);
}

////////// 6. CALLS //////////

// 6. fn, a function of a lingo here, linked from its code and run with its values in order, a value left out taking its
// default. Its value, or NULL with the error's code and message.
static val *core_call(
    arena *a, const char *fn, val *values, volatile int *stop, const char **code, const char **message) {
  val *program = lingo_call(a, device, fn);
  val *input = v_rec(a);
  char name[24];
  if (!program) {
    *code = "unknown";
    *message = v_cstr(a, fn)->t.s;
    return NULL;
  }
  for (size_t i = 0; i < values->l.n; i++) {
    snprintf(name, sizeof name, "a%zu", i);
    rec_set(a, input, name, values->l.v[i]);
  }
  long before = socks_made;
  long opened = cns_made;
  val *got = run_program(a, program, input, (limits){ set.steps, set.ms, stop }, code, message);
  while (socks && socks->id > before) let_go(socks, "as the run ended", 0);
  while (cns && cns->id > opened) cn_let_go(cns);
  return got;
}

////////// 7. LINGO SETTINGS //////////

// A lingo's settings are kept on this computer, never on Onsay: in home/settings/<lingo>.json, a record by name, or by
// the lingo's own functions when its manifest's x-lingo says keep: {read, write}, so its maker decides where they live.
// setting(...) is lingo.setting(key, reference), its value the one kept, else its manifest's default, else nothing.

static const char
    *keeping;   // the lingo whose own read or write runs now, so its setting(...) reads this computer's file

/* One lingo's settings, as a form shows them: each its manifest's x-lingo lists, a name alone or a record of its name,
   title, about, default, required and secret, then each other one its code reads, by name. keep, when asked for, is
   where its x-lingo says they are kept, {read, write}, its own functions, or NULL for this computer's file of them, as it
   is inside those functions too. */
static val *declared(arena *a, const char *lingo, val **keep) {
  val *out = v_list(a);
  val *x = rec_get(manifest_of(lingo), "x-lingo");
  val *own = rec_get(rec_get(device, "lingos"), lingo);
  val *listed = rec_get(x, "settings");
  val *read = rec_get(own, "settings");
  for (size_t i = 0; listed && listed->k == V_LIST && i < listed->l.n; i++) {
    val *s = listed->l.v[i];
    val *one = v_rec(a);
    if (s->k == V_TEXT)
      rec_set(a, one, "name", s);
    else
      for (size_t k = 0; k < s->r.n; k++) rec_set(a, one, s->r.key[k], s->r.v[k]);
    list_push(a, out, one);
  }
  for (size_t i = 0; read && i < read->l.n; i++) {
    int had = 0;
    for (size_t k = 0; k < out->l.n && !had; k++) had = !strcmp(text_or(out->l.v[k], "name", ""), read->l.v[i]->t.s);
    if (!had) {
      val *one = v_rec(a);
      rec_set(a, one, "name", read->l.v[i]);
      list_push(a, out, one);
    }
  }
  if (keep) *keep = !(keeping && !strcmp(keeping, lingo)) ? rec_get(x, "keep") : NULL;
  return out;
}

// The lingo a setting is of, checked, since it names a file, and the setting's name when there is one.
static const char *whose(run *r, val *lingo, val *key) {
  if (lingo->k != V_TEXT || !named(lingo->t.s))
    fail(r, "phrase", "a lingo's settings are named by the lingo, like agent");
  if (key && (key->k != V_TEXT || !key->t.len)) fail(r, "phrase", "a setting is named by its name, like key");
  return lingo->t.s;
}

// 7. The settings this computer keeps for a lingo, in home/settings/<lingo>.json, a record by name.
val *lingo_setting(arena *a, const char *lingo, const char *key) {
  val *s = named(lingo) ? home_json(a, arena_fmt(a, "settings/%s.json", lingo)) : NULL;
  return s && s->k == V_REC ? rec_get(s, key) : NULL;
}

// 7.
int lingo_set(const char *lingo, const char *key, val *value) {
  arena *a = arena_new();
  val *s = named(lingo) ? home_json(a, arena_fmt(a, "settings/%s.json", lingo)) : NULL;
  if (!s || s->k != V_REC) s = v_rec(a);
  rec_set(a, s, key, value);
  int ok = named(lingo) && keep_json(arena_fmt(a, "settings/%s.json", lingo), s);
  arena_free(a);
  looked = 0;   // read everything again, so the catalog says what is still missing
  return ok;
}

// A lingo's own function that keeps its settings, read(key) or write(key, value), run as that lingo's own. Its value.
static val *core_kept(run *r, const char *lingo, const char *fn, val *values) {
  const char *was = keeping;
  const char *code = NULL;
  const char *message = NULL;
  const char *was_lingo = job.lingo;
  const char *was_fn = job.fn;
  const char *id = arena_fmt(r->a, "%s.%s", lingo, fn);
  keeping = lingo;
  if (job.lingo) {
    job.lingo = lingo;
    job.fn = id;
  }
  val *got = core_call(r->a, id, values, r->stop, &code, &message);
  keeping = was;
  job.lingo = was_lingo;
  job.fn = was_fn;
  if (!got)
    fail(r, code, "%s, which keeps %s's settings: %s", id, lingo,
        !strcmp(code, "unknown") ? "it has no function by that name" : message);
  return got;
}

// A setting's value, kept wherever its lingo keeps it, else its manifest's default, else nothing. all: its settings, as
// declared gives them.
static val *setting_value(run *r, const char *lingo, val *key, val *all, val *keep) {
  val *v;
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, key);
    v = core_kept(r, lingo, text_or(keep, "read", ""), values);
  } else
    v = lingo_setting(r->a, lingo, key->t.s);
  for (size_t i = 0; (!v || v->k == V_NOTHING) && i < all->l.n; i++)
    if (!strcmp(text_or(all->l.v[i], "name", ""), key->t.s)) v = rec_get(all->l.v[i], "default");
  return v ? v : NOTHING;
}

// Whether the lingo running is another than lingo. The person, at the command line or through /onsay, is none.
static int other(run *r, const char *lingo) { return r->lingo && strcmp(r->lingo, lingo); }

// lingo.setting(key, reference). Another lingo's setting is read too, so one token serves many, but never one its
// manifest calls secret.
static val *setting_(run *r, val **a) {
  const char *lingo = whose(r, a[1], a[0]);
  val *keep = NULL;
  val *all = declared(r->a, lingo, &keep);
  for (size_t i = 0; other(r, lingo) && i < all->l.n; i++) {
    val *secret = rec_get(all->l.v[i], "secret");
    if (!strcmp(text_or(all->l.v[i], "name", ""), a[0]->t.s) && secret && truthy(secret))
      fail(r, "phrase", "%s is a secret of %s's, and only %s reads it", a[0]->t.s, lingo, lingo);
  }
  return setting_value(r, lingo, a[0], all, keep);
}

// settings.get(lingo), for a form of them: each its manifest lists, with what it says, then each other one its code
// reads, each with its value now.
static val *settings_get(run *r, val **a) {
  const char *lingo = whose(r, a[0], NULL);
  val *keep = NULL;
  val *all = declared(r->a, lingo, &keep);
  for (size_t i = 0; i < all->l.n; i++) {
    val *secret = rec_get(all->l.v[i], "secret");
    val *now = setting_value(r, lingo, rec_get(all->l.v[i], "name"), all, keep);
    // A secret's value goes only to its own lingo and to the person's form here, never by request, where a model may
    // read it, nor to another lingo: they get whether it is set.
    int hidden = other(r, lingo) || (job.path && job.lingo && !strcmp(job.lingo, "onsay"));
    if (hidden && secret && truthy(secret) && now->k != V_NOTHING) now = v_cstr(r->a, "set");
    rec_set(r->a, all->l.v[i], "value", now);
  }
  return all;
}

// settings.set(lingo, key, value), wherever that lingo keeps them. Only the lingo itself and the person change them.
static val *settings_set(run *r, val **a) {
  const char *lingo = whose(r, a[0], a[1]);
  if (other(r, lingo)) fail(r, "phrase", "only %s, or the person, changes %s's settings", lingo, lingo);
  val *keep = NULL;
  declared(r->a, lingo, &keep);
  if (keep) {
    val *values = v_list(r->a);
    list_push(r->a, values, a[1]);
    list_push(r->a, values, a[2]);
    core_kept(r, lingo, text_or(keep, "write", ""), values);
    return NOTHING;
  }
  if (!lingo_set(lingo, a[1]->t.s, a[2])) fail(r, "phrase", "this computer couldn't save the setting");
  return NOTHING;
}

// A lingo's functions from its manifest, each its name and what it does, but its helpers for its own use.
static val *functions_of(arena *a, val *manifest) {
  val *paths = rec_get(manifest, "paths");
  val *out = v_list(a);
  for (size_t i = 0; paths && paths->k == V_REC && i < paths->r.n; i++) {
    val *op = rec_get(paths->r.v[i], "post");
    val *helper = rec_get(op, "x-lingo-internal");
    val *one;
    if (!op || (helper && truthy(helper))) continue;
    one = v_rec(a);
    rec_set(a, one, "name", v_cstr(a, strrchr(paths->r.key[i], '/') + 1));
    rec_set(a, one, "does", v_cstr(a, text_or(op, "summary", "")));
    list_push(a, out, one);
  }
  return out;
}

// lingo.list(): from the catalog, each running lingo that loaded, but those built in, whose calls every lingo has.
static val *lingo_list(run *r, val **a) {
  val *out = v_list(r->a);
  (void)a;
  for (size_t i = 0; catalog && i < catalog->l.n; i++) {
    val *c = catalog->l.v[i];
    val *m = rec_get(c, "openapi");
    val *one;
    if (!m || builtin_of(text_or(c, "name", ""))) continue;
    one = v_rec(r->a);
    rec_set(r->a, one, "name", v_cstr(r->a, text_or(c, "name", "")));
    rec_set(r->a, one, "about", v_cstr(r->a, text_or(rec_get(m, "info"), "description", "")));
    rec_set(r->a, one, "functions", functions_of(r->a, m));
    list_push(r->a, out, one);
  }
  return out;
}

// lingo.code(name): its code, and of its manifest what its maker writes, info and x-lingo, but what this computer adds.
static val *lingo_code(run *r, val **a) {
  const char *name = a[0]->t.s;
  val *e = entry_of(name);
  val *out = v_rec(r->a);
  val *made = v_rec(r->a);
  val *x = v_rec(r->a);
  val *m;
  val *own;
  if (!e || builtin_of(name)) fail(r, "unknown", "There's no lingo called %s here", name);
  if (!rec_get(e, "code")) fail(r, "phrase", "%s isn't written in Lingo, so it has no code to read", name);
  m = rec_get(e, "manifest");
  own = rec_get(m, "x-lingo");
  for (size_t i = 0; own && own->k == V_REC && i < own->r.n; i++)
    if (strcmp(own->r.key[i], "missing")) rec_set(r->a, x, own->r.key[i], v_copy(r->a, own->r.v[i]));
  if (rec_get(m, "info")) rec_set(r->a, made, "info", v_copy(r->a, rec_get(m, "info")));
  rec_set(r->a, made, "x-lingo", x);
  rec_set(r->a, out, "code", v_copy(r->a, rec_get(e, "code")));
  rec_set(r->a, out, "openapi", made);
  return out;
}

/* lingo.save(name, code, openapi): written as a folder in the lingos folder, in place of a .lingo file of its name, put
   among those that run, and every lingo read again at once, so it answers and Onsay is told it while the request that
   saved it still runs. Its functions, each its name and what it does, or why it can't run, in its own words. */
static val *lingo_save(run *r, val **a) {
  arena *ra = r->a;
  const char *name = a[0]->t.s;
  const char *folder = arena_fmt(ra, "%s/%s", set.lingos, name);
  val *running = home_json(ra, "running.json");
  val *e;
  if (!named(name)) fail(r, "type", "A lingo's name is lower case letters, digits, _ and -, like chess-site, and never lingo");
  if (builtin_of(name) || is_standard(name)) fail(r, "denied", "%s is built into this program, so it can't be changed", name);
  if (retiring == (int)(sizeof retired / sizeof *retired))
    fail(r, "phrase", "That is as many lingos as can be saved in one go. Save it again once this is done");
  pl_folder(folder);
  if (!pl_write(arena_fmt(ra, "%s/%s.ls", folder, name), a[1]->t.s, a[1]->t.len))
    fail(r, "phrase", "this computer couldn't write %s", folder);
  if (a[2]->k != V_NOTHING) {
    buf json = { 0 };
    buf_json(&json, a[2]);
    int ok = pl_write(arena_fmt(ra, "%s/openapi.json", folder), json.s, json.n);
    buf_free(&json);
    if (!ok) fail(r, "phrase", "this computer couldn't write %s/openapi.json", folder);
  }
  pl_delete(arena_fmt(ra, "%s/%s.lingo", set.lingos, name));
  if (!running || running->k != V_LIST) running = v_list(ra);
  if (!has(running, name)) list_push(ra, running, v_cstr(ra, name));
  if (!keep_json("running.json", running)) fail(r, "phrase", "This computer couldn't save which lingos run");
  load(NULL);
  e = entry_of(name);
  if (!e) fail(r, "phrase", "%s was saved in %s, but it wasn't read back", name, folder);
  if (rec_get(e, "error")) fail(r, "type", "%s was saved, but it can't run: %s", name, text_or(e, "error", ""));
  return functions_of(ra, rec_get(e, "manifest"));
}

////////// 8. CONTROL //////////

/* 8. One thing an app or the person does, from /.lingo/<verb> or the command line, its values by name. Its answer, or
   NULL with why in words.
     start, stop {lingo}             a lingo running or stopped, kept in home/running.json
     asks                            the questions waiting for the person, and the app asking counts as open to ask
     answer {id, answer}             the list of answers to questions
     settings {lingo}                its settings, each with what it is and its value now
     set {lingo, key, value}         one of them changed
     manifest {lingo}                its whole OpenAPI manifest, as the catalog has it, or why it can't load
     policy                          the policy in force, and the files it comes from
     call {fn, values}               a function of any lingo here, or a phrase, run as this computer, its values in order,
                                     like onsay.sign_in, its value
     badge {to, allow, except, pass, until, data, badge}
                                     a badge signed by this computer, for public, everything and a day unless it says,
                                     stapled on to badge when given, as base64 to send. From the command line, its values
                                     are one JSON record */
val *core_control(arena *a, const char *verb, val *v, const char **why) {
  const char *lingo = text_or(v, "lingo", "");
  const char *code = NULL;
  const char *message = NULL;
  val *out = NOTHING;
  val *values = v_list(a);
  volatile int stop = 0;
  *why = NULL;
  if (!strcmp(verb, "start") || !strcmp(verb, "stop")) {
    val *was = home_json(a, "running.json");
    val *now = v_list(a);
    if (!entry_of(lingo)) {
      *why = arena_fmt(a, "There's no lingo called %s here", lingo);
      return NULL;
    }
    if (builtin_of(lingo)) {
      *why = arena_fmt(a, "%s is built into this program, so it always runs", lingo);
      return NULL;
    }
    for (size_t i = 0; was && was->k == V_LIST && i < was->l.n; i++)
      if (was->l.v[i]->k == V_TEXT && strcmp(was->l.v[i]->t.s, lingo)) list_push(a, now, was->l.v[i]);
    if (!strcmp(verb, "start")) list_push(a, now, v_cstr(a, lingo));
    if (!keep_json("running.json", now)) {
      *why = "This computer couldn't save which lingos run";
      return NULL;
    }
    rec_set(a, out = v_rec(a), "lingo", v_cstr(a, lingo));
    rec_set(a, out, "running", VBOOL(!strcmp(verb, "start")));
    if (serving <= 1) core_look();   // at once, unless a request waits inside, which the next look waits for
  } else if (!strcmp(verb, "asks")) {
    polled = pl_now();
    out = v_list(a);
    for (int i = 0; i < waiting; i++) list_push(a, out, v_copy(a, asks[i].ask));
  } else if (!strcmp(verb, "answer")) {
    val *id = rec_get(v, "id");
    val *answer = rec_get(v, "answer");
    int i = 0;
    while (i < waiting && !(id && id->k == V_NUM && rec_get(asks[i].ask, "id")->n == id->n)) i++;
    if (i == waiting) {
      *why = "That question isn't waiting any more";
      return NULL;
    }
    if (!answer || answer->k != V_LIST) {
      *why = "The answer is a list, one for each question, in order";
      return NULL;
    }
    // Read again into the arena of the run that waits, since this request's own, which holds its texts, ends first.
    buf core_kept = { 0 };
    buf_json(&core_kept, answer);
    rec_set(asks[i].a, asks[i].ask, "answer", json_parse(asks[i].a, core_kept.s, core_kept.n, NULL));
    buf_free(&core_kept);
    asks[i].done = 1;
  } else if (!strcmp(verb, "settings") || !strcmp(verb, "set")) {
    list_push(a, values, v_cstr(a, lingo));
    if (!strcmp(verb, "set")) {
      list_push(a, values, rec_get(v, "key") ? rec_get(v, "key") : NOTHING);
      list_push(a, values, rec_get(v, "value") ? rec_get(v, "value") : NOTHING);
    }
    out = core_call(a, !strcmp(verb, "set") ? "settings.set" : "settings.get", values, &stop, &code, &message);
    if (!out) {
      *why = message;
      return NULL;
    }
  } else if (!strcmp(verb, "manifest")) {
    val *e = entry_of(lingo);
    if (!e) {
      *why = arena_fmt(a, "There's no lingo called %s here", lingo);
      return NULL;
    }
    if (rec_get(e, "error")) {
      *why = text_or(e, "error", "");
      return NULL;
    }
    out = rec_get(e, "manifest");
  } else if (!strcmp(verb, "policy"))
    out = policy(a);
  else if (!strcmp(verb, "call")) {
    const char *fn = text_or(v, "fn", "");
    val *given = rec_get(v, "values");
    if (!(out = core_call(a, fn, given && given->k == V_LIST ? given : values, &stop, &code, &message))) {
      *why = !strcmp(code, "unknown")
          ? arena_fmt(a, "There's no function or phrase called %s here, like memo.recall", fn)
          : message;
      return NULL;
    }
  } else if (!strcmp(verb, "badge")) {
    val *given = rec_get(v, "value");
    if (!(out = badge_make(a, given && given->k == V_REC ? given : v, why))) return NULL;
  } else {
    *why = arena_fmt(a,
        "There's nothing called %s here. It does start, stop, asks, answer, settings, set, manifest, policy, call and badge",
        verb);
    return NULL;
  }
  return out;
}

////////// 9. HOOKS //////////

/* 9. The account's policy as this computer keeps it, home/policy.account.json: device first, then the policy as it came.
   A version older than the one kept is refused. It returns an empty record, kept for callers that report what was
   clamped: there is nothing to clamp any more. */
val *lingo_policy(arena *a, val *account, const char *device, const char **why) {
  val *core_kept = home_json(a, "policy.account.json");
  val *v = rec_get(account, "version");
  val *was = rec_get(core_kept, "version");
  val *out = v_rec(a);
  if (!v || v->k != V_NUM) {
    *why = "The account's policy has no version";
    return NULL;
  }
  if (was && was->k == V_NUM && v->n < was->n) {
    *why = arena_fmt(a, "The account's policy %g is older than the one kept here, %g", v->n, was->n);
    return NULL;
  }
  rec_set(a, out, "device", v_cstr(a, device));
  for (size_t i = 0; i < account->r.n; i++)
    if (strcmp(account->r.key[i], "device")) rec_set(a, out, account->r.key[i], account->r.v[i]);
  if (!keep_json("policy.account.json", out)) {
    *why = "This computer couldn't save policy.account.json";
    return NULL;
  }
  say("The account's policy %g is in force here", v->n);
  return v_rec(a);
}





// 9.
const char *lingo_home(void) { return set.root; }
int lingo_serves(void) { return live; }

// When anything in the lingos folder or running.json changed: everything read again, and each running native lingo told
// the new catalog. 1 when a grant in force is for public, so the port opens to other computers. One for a person never
// opens it: that call arrives over Onsay's connection, signed by the person's device.
int core_look(void) {
  arena *a = arena_new();
  unsigned long long now;
  val *p = policy(a);
  int outside = 0;
  for (int k = 0; k < 2; k++) {   // this computer's own grants, then the account's
    val *grants = rec_get(p, k ? "grants" : "own");
    for (size_t i = 0; grants && grants->k == V_LIST && i < grants->l.n; i++)
      outside |= !strcmp(text_or(grants->l.v[i], "to", ""), "public");
  }
  look(a, &now);
  arena_free(a);
  if (now != looked) load(NULL);
  return outside;
}

////////// 10. REQUESTS //////////

// A web request reaches the running lingo at the longest route its path starts with: the route its manifest names, like
// "/onsay", else /<name>, when the caller may reach it. A lingo with serve(request) gets every request to its route, and
// any other is called as /<name>/<function>, or a native's /<name>/<group>/<function>, its values a JSON record by name
// or a list in order, or the query for a GET. GET / lists the lingos the caller may reach, and /.lingo/ is for apps on
// this computer (8. CONTROL).


// Part of a query with its %XX and + undone, kept as it came when that isn't text.
static val *unescape(arena *a, const char *s, size_t n) {
  buf b = { 0 };
  for (size_t i = 0; i < n; i++) {
    char x = i + 2 < n ? s[i + 1] : 0;
    char y = i + 2 < n ? s[i + 2] : 0;
    int hi = s[i] == '%' ? hex(x >= 'a' ? x - 32 : x) : -1;
    int lo = hex(y >= 'a' ? y - 32 : y);
    if (hi >= 0 && lo >= 0) {
      buf_chr(&b, (char)(hi * 16 + lo));
      i += 2;
    } else
      buf_chr(&b, s[i] == '+' ? ' ' : s[i]);
  }
  val *v = utf8_ok(b.s ? b.s : "", b.n) ? v_text(a, b.s ? b.s : "", b.n) : v_text(a, s, n);
  buf_free(&b);
  return v;
}

// A function's listing, if the lingo has one by that id, like memo.recall of memo or files.read of onsay.
static val *listed(const char *lingo, const char *id) {
  val *own = rec_get(rec_get(rec_get(device, "lingos"), lingo), "phrases");
  int base = !strcmp(lingo, "onsay");
  if (base && private_call(id)) return NULL;
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  if (!base) return NULL;
  own = rec_get(rec_get(rec_get(device, "lingos"), "core"), "phrases");
  for (size_t i = 0; own && i < own->l.n; i++)
    if (!strcmp(text_or(own->l.v[i], "id", ""), id)) return own->l.v[i];
  return NULL;
}

// Whether a request carries this computer's prompt key, the onsay lingo's setting, as Authorization: Bearer <key>.
static int prompt_keyed(arena *a, val *headers) {
  val *key = lingo_setting(a, "onsay", "prompt_key");
  const char *sent = text_or(headers, "authorization", "");
  return key && key->k == V_TEXT && key->t.len && !strncmp(sent, "Bearer ", 7) && !strcmp(sent + 7, key->t.s);
}

void core_serve(const char *text, size_t n, const char *given, buf *out) {
  arena *a = arena_new();
  const char *problem = NULL;
  const char *lingo = NULL;
  const char *why = NULL;
  val *m = json_parse(a, text, n, NULL);
  val *query = v_rec(a);
  val *head = v_rec(a);
  val *got = NOTHING;
  val *values = v_list(a);
  val *f = NULL;
  val *pol = policy(a);
  if (!m || m->k != V_REC) m = v_rec(a);
  val *headers = header_names(a, rec_get(m, "headers"));
  val *body = rec_get(m, "body");
  const char *p = text_or(m, "path", "");
  const char *method = text_or(m, "method", "GET");
  const char *ip = given ? NULL : text_or(m, "ip", "");
  const char *refused = NULL;
  caller c = { NULL, 0, 0, NULL };
  int full;   // whether the caller reaches the control routes, and so everything here: the owner, or its badge
  int allowed = 1;   // allowed and let, for its line in requests.log: whether it was let in, and what let it
  const char *let = NULL;
  double began = pl_now();
  int status = 200;
  int json = 1;   // json: the body is written as JSON
  size_t at = strcspn(p, "?");
  size_t best = 0;
  buf id = { 0 };
  buf error = { 0 };
  buf call = { 0 };
  buf_json(&id, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
  if (!body || body->k != V_TEXT) body = v_cstr(a, "");
  serving++;

  /* The caller, by the proof it came with: one handed in is the account's own, me, Onsay's, or anyone's, by its address,
     like the first of a proxy's x-forwarded-for, and one to the port anyone's, unless it is signed. A badge, in
     Onsay-Badge or a handed-in request's badge, is checked and kept with it. A signature or a badge that doesn't check
     out is a 401. An address never lets anyone in, and the control routes are only for the port. */
  if (given && (!strcmp(given, "me") || !strcmp(given, "onsay")))
    c.name = given;
  else
    c.name = arena_fmt(a, "ip %s", given ? (strncmp(given, "ip ", 3) ? "" : given + 3) : ip);
  c.owner = given && !strcmp(given, "me");
  if (!c.owner && strcmp(c.name, "onsay") && rec_get(headers, "signature-input"))
    refused = signer(a, headers, method, p, body, &c);
  const char *badge = text_or(m, "badge", text_or(headers, "onsay-badge", NULL));
  if (!refused && badge) c.badge = badge_of(a, badge, &c.begun, &refused);
  full = !given && !refused && let_in(a, pol, &c, "/.lingo/", 0, &why);
  if (full) let = why;
  if (refused) allowed = 0;
  for (const char *q = p + at + (p[at] == '?'); *q;) {
    size_t len = strcspn(q, "&");
    size_t eq = strcspn(q, "=&");
    if (len) rec_set(a, query, unescape(a, q, eq)->t.s, unescape(a, q + eq + (eq < len), eq < len ? len - eq - 1 : 0));
    q += len + (q[len] == '&');
  }
  for (size_t i = 0; i < found->l.n; i++) {
    val *e = found->l.v[i];
    const char *route = route_of(a, e);
    size_t r = strlen(route);
    if (!rec_get(e, "error") && truthy(rec_get(e, "running")) && r > best && at >= r && !strncmp(p, route, r) &&
        (at == r || p[r] == '/')) {
      lingo = text_or(e, "name", "");
      best = r;
    }
  }

  if (p[0] != '/') {
    status = 400;
    buf_str(&error, "A path starts with /, like /memo/recall");
  } else if (truthy(rec_get(m, "base64") ? rec_get(m, "base64") : NOTHING) || !utf8_ok(body->t.s, body->t.len)) {
    status = 400;
    buf_str(&error, "Its body isn't text, and lingos take text, like JSON");
  } else if (refused) {
    status = 401;
    buf_str(&error, refused);
  } else if (!strncmp(p, "/.lingo/", 8) && full) {
    // Control, for an app on this computer only: never one handed in, since nothing from elsewhere may change what runs here.
    val *given = strcmp(method, "GET") && body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : query;
    if (problem || !given || given->k != V_REC) {
      status = 400;
      buf_str(&error, "Its values are a JSON record by name, like {\"lingo\": \"memo\"}");
    } else if (!(got = core_control(a, v_text(a, p + 8, at - 8)->t.s, given, &why))) {
      status = 400;
      buf_str(&error, why);
      got = NOTHING;
    }
  } else if (at == 1) {
    // Every lingo for this computer, running or not, and a broken one with why. Anyone else, each they may reach.
    val *list = v_list(a);
    for (size_t i = 0; i < found->l.n; i++) {
      val *e = found->l.v[i];
      val *one = v_rec(a);
      const char *name = text_or(e, "name", "");
      int runs = truthy(rec_get(e, "running"));
      if (!full && (rec_get(e, "error") || !runs || !let_in(a, pol, &c, route_of(a, e), 1, &why))) continue;
      rec_set(a, one, "name", v_cstr(a, name));
      if (!rec_get(e, "error")) rec_set(a, one, "route", v_cstr(a, route_of(a, e)));
      rec_set(a, one, "running", VBOOL(runs));
      if (rec_get(e, "error")) rec_set(a, one, "error", rec_get(e, "error"));
      list_push(a, list, one);
    }
    rec_set(a, got = v_rec(a), "lingos", list);
  } else if (!lingo) {
    status = 404;
    buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
  } else {
    // /name alone is name.main, /name/function name.function, and a native's /name/group/function group.function.
    const char *rest = p + best;
    const char *name = rest + (at > best);
    const char *was_caller = job.caller;
    const char *was_lingo = job.lingo;
    const char *was_fn = job.fn;
    const char *was_path = job.path;
    const char *was_id = job.id;
    val *was_policy = job.policy;
    val *was_answers = job.answers;
    val *was_badge = job.badge;
    size_t left = at - best;
    size_t nl = left ? left - 1 : 0;
    int in = full || let_in(a, pol, &c, v_text(a, p, at)->t.s, 0, &why);
    // /onsay is open, for its door checks its own key, but a base call is only for this account's own devices or that key.
    int base = 0;
    if (!strcmp(lingo, "onsay") && nl) {
      buf id = { 0 };
      if (memchr(name, '/', nl))
        buf_add(&id, name, nl);
      else
        buf_fmt(&id, "onsay.%.*s", (int)nl, name);
      for (size_t i = 0; i < id.n; i++)
        if (id.s[i] == '/') id.s[i] = '.';
      base = id.s && listed(lingo, id.s) && strcmp(id.s, "onsay.serve");
      buf_free(&id);
    }
    if (base && in && !full && !c.owner && !prompt_keyed(a, headers)) {
      in = 0;
      why = "a base call needs this account's own device or the prompt key";
    }
    job.caller = full ? "this computer" : c.name;
    job.badge = c.badge;
    job.lingo = lingo;
    job.path = v_text(a, p, at)->t.s;
    job.policy = pol;
    job.answers = v_list(a);
    job.id = id.s;
    allowed = in;
    if (!full) let = why;
    if (!full) core_decided(c.name, job.path, NULL, in, why);
    buf_fmt(&call, "%s.serve", lingo);
    if (!in) {
      status = 404;
      buf_fmt(&error, "There's nothing at %.*s", (int)at, p);
    } else if (!base && listed(lingo, call.s)) {
      val *request = v_rec(a);
      val *parsed = body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : NOTHING;
      rec_set(a, request, "method", v_cstr(a, method));
      rec_set(a, request, "path", left ? v_text(a, rest, left) : v_cstr(a, "/"));
      rec_set(a, request, "query", query);
      rec_set(a, request, "headers", headers);
      rec_set(a, request, "body", body);
      rec_set(a, request, "json", problem ? NOTHING : parsed);
      rec_set(a, request, "caller", v_cstr(a, full ? "this computer" : c.name));
      list_push(a, values, request);
    } else {
      call.n = 0;
      if (!nl)
        buf_fmt(&call, "%s.main", lingo);
      else if (memchr(name, '/', nl))
        buf_add(&call, name, nl);
      else
        buf_fmt(&call, "%s.%.*s", lingo, (int)nl, name);
      for (size_t i = 0; i < call.n; i++)
        if (call.s[i] == '/') call.s[i] = '.';
      val *given = query;
      if (strcmp(method, "GET")) given = body->t.len ? json_parse(a, body->t.s, body->t.len, &problem) : v_rec(a);
      f = listed(lingo, call.s);
      val *params = f ? rec_get(f, "params") : NULL;
      if (!f) {
        status = 404;
        buf_fmt(&error, "%s has no function %s", lingo, call.s);
      } else if (problem || (given->k != V_REC && given->k != V_LIST)) {
        status = 400;
        buf_str(&error, "Its values are a JSON record by name, like {\"text\": \"hello\"}");
      } else if (given->k == V_LIST)
        values = given;
      else {
        // In the function's order, nothing for one left out, so its default applies.
        for (size_t i = 0; i < params->l.n; i++) {
          val *v = rec_get(given, text_or(params->l.v[i], "name", ""));
          list_push(a, values, v ? v : NOTHING);
        }
        for (size_t k = 0; k < given->r.n && !error.n; k++) {
          int known = 0;
          for (size_t i = 0; i < params->l.n && !known; i++)
            known = !strcmp(given->r.key[k], text_or(params->l.v[i], "name", ""));
          if (!known) {
            status = 400;
            buf_fmt(&error, "%s doesn't take a value called %s", call.s, given->r.key[k]);
          }
        }
      }
    }
    if (!error.n) {
      // The request is the running job: a stop with its id ends it, and each lingo(...) it asks says which function asks.
      const char *code = NULL;
      const char *message = NULL;
      double started = pl_now();
      job.fn = call.s;
      frame me = { top, "", 0 };
      snprintf(me.id, sizeof me.id, "%s", id.s);
      pl_enter(core_gate);
      top = &me;
      pl_leave(core_gate);
      got = core_call(a, call.s, values, &me.stop, &code, &message);
      pl_enter(core_gate);
      top = me.up;
      pl_leave(core_gate);
      say("%s answered in %.0f ms", call.s, (pl_now() - started) * 1000);
      if (!got) {
        status = !strcmp(code, "unknown") ? 404 : !strcmp(code, "denied") ? 403 : 500;
        buf_str(&error, message);
        got = NOTHING;
      }
      // What serve gives: a record with a number status is the response, and anything else the body of a 200.
      else if (!f) {
        val *s = got->k == V_REC ? rec_get(got, "status") : NULL;
        if (!s || s->k != V_NUM)
          json = got->k != V_TEXT;
        else if (s->n < 100 || s->n > 599 || s->n != (int)s->n) {
          status = 500;
          buf_str(&error, "serve gave a status that isn't one, like 200 or 404");
        } else {
          status = (int)s->n;
          head = header_names(a, rec_get(got, "headers"));
          got = rec_get(got, "body") ? rec_get(got, "body") : NOTHING;
          json = got->k != V_TEXT && got->k != V_NOTHING;
        }
      }
    }
    job.caller = was_caller;
    job.lingo = was_lingo;
    job.fn = was_fn;
    job.path = was_path;
    job.policy = was_policy;
    job.answers = was_answers;
    job.badge = was_badge;
    job.id = was_id;
  }

  // Its line in requests.log: who asked, with whose badge, for what, and how it was answered. Written before what a
  // reload retired is freed, since the lingo's name is in it.
  val *line = v_rec(a);
  char stamp[32];
  time_t now = (time_t)pl_now();
  strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  rec_set(a, line, "at", v_cstr(a, stamp));
  rec_set(a, line, "way", v_cstr(a, "in"));
  rec_set(a, line, "ms", v_num(a, (pl_now() - began) * 1000));
  rec_set(a, line, "caller", v_cstr(a, full ? "this computer" : c.name));
  if (c.badge) rec_set(a, line, "badge", v_cstr(a, text_or(c.badge->l.v[0], "by", "")));
  rec_set(a, line, "lingo", lingo ? v_cstr(a, lingo) : NOTHING);
  rec_set(a, line, "route", v_text(a, p, at));
  rec_set(a, line, "method", v_cstr(a, method));
  rec_set(a, line, "status", v_num(a, status));
  rec_set(a, line, "allowed", VBOOL(allowed));
  rec_set(a, line, "why", let ? v_cstr(a, let) : NOTHING);
  if (error.n) rec_set(a, line, "error", v_text(a, error.s, error.n));
  requested(line);

  if (!--serving)
    while (retiring) arena_free(retired[--retiring]);
  if (error.n) {
    rec_set(a, got = v_rec(a), "error", v_text(a, error.s, error.n));
    json = 1;
  }

  buf b = { 0 };
  if (json)
    buf_json(&b, got);
  else
    buf_text(&b, got);
  if (!rec_get(head, "content-type"))
    rec_set(a, head, "content-type", v_cstr(a, json ? "application/json" : "text/plain; charset=utf-8"));
  buf_fmt(out, "{\"id\":%s,\"status\":%d,\"headers\":", id.s, status);
  buf_json(out, head);
  buf_str(out, ",\"body\":");
  buf_json_str(out, b.s ? b.s : "", b.n);
  buf_str(out, ",\"base64\":false}");
  buf_free(&b);
  buf_free(&id);
  buf_free(&error);
  buf_free(&call);
  arena_free(a);
}

////////// 11. BEGINNING, AND PACKING //////////

int core_begin(int serves) {
  live = serves;
  core_gate = pl_gate_new();
  log_gate = pl_gate_new();
  native_begin(0);
  std_phrases();
  floor_phrases();
  return load(NULL);
}

/* 11. A folder of a lingo's files, its program and the openapi.json its maker wrote, made into its .lingo file: a zip of
   its program and its whole openapi.json, its paths made from its code, or a native library's phrases. It is loaded
   with every lingo here first, so what it reaches in them counts. The file goes in to, or beside the folder. */
int core_pack(const char *folder, const char *to) {
  arena *a = arena_new();
  const char *base = folder + strlen(folder);
  buf zip = { 0 };
  buf files[2] = { { 0 }, { 0 } };
  buf path = { 0 };
  buf_str(&path, folder);
  while (path.n > 1 && (path.s[path.n - 1] == '/' || path.s[path.n - 1] == '\\')) path.s[--path.n] = 0;
  base = path.s + path.n;
  while (base > path.s && base[-1] != '/' && base[-1] != '\\') base--;
  val *e = load(path.s) ? entry_of(base) : NULL;
  int ok = e && rec_get(e, "fresh") && !rec_get(e, "error");
  if (!e || !rec_get(e, "fresh"))
    say("%s isn't a folder of a lingo's files: its program, like %s.ls, and its openapi.json", folder, base);
  else if (!ok)
    say("%s can't be packed: %s", base, text_or(e, "error", ""));
  if (ok) {
    const char *names[2] = { "openapi.json", text_or(e, "program", "") };
    buf_json(&files[0], rec_get(e, "manifest"));
    ok = pl_read(arena_fmt(a, "%s/%s", path.s, names[1]), &files[1]);
    zip_make(&zip, 2, names, files);
    if (to) pl_folder(to);
    const char *into = to ? arena_fmt(a, "%s/%s.lingo", to, base) : arena_fmt(a, "%s.lingo", path.s);
    ok = ok && pl_write(into, zip.s, zip.n);
    if (ok)
      say("Packed %s into %s", base, into);
    else
      say("%s couldn't be written", into);
  }
  buf_free(&zip);
  buf_free(&files[0]);
  buf_free(&files[1]);
  buf_free(&path);
  arena_free(a);
  return !ok;
}

//////////////////// 9. ONSAY ////////////////////

// The onsay lingo, built into the lingo client on every computer (build.ps1), which makes it Onsay's: its
// one connection to Onsay, signing in and this device's key, what Onsay signs checked against its key built in, the
// account's policy kept, decisions sent for the activity feed, and every request this computer makes sent in chunks.
// The link is offered to the distribution's private lingos too (lingo.h, Onsay), like the agent loop at /onsay
// (lingos/onsay/agent.c). Everything it sends Onsay is kept in home/sent.log for the person to read. See
// docs/security.md in the onsay repo, The onsay lingo and the wire.
//
// 1. Onsay          where it is, its public key, checking what it signed, and sent.log
// 2. Connection     hello, each message from Onsay, the heartbeat and reconnecting, on threads of its own
// 3. Hooks          lingo(...) and use(...), a signer's key, decisions, and starting and stopping
// 4. Policy         fetched, checked and handed to the core
// 5. Signing in     sign_in, sign_out and status, from an app through /.lingo/call or from onsay-core <settings> call,
//                   the account's devices, and where its trial of Onsay's AI stands
// 6. onsay_lingo()  adds its phrases and the base calls, as the lingo client starts

////////// 1. ONSAY //////////

// Where Onsay is, built in, never a setting: its API, its resolve endpoint and its public key, at the top of this file (ONSAY, RESOLVE
// and ONSAY_KEY). Every statement, request and policy Onsay signs is checked against that
// key, never against a key only told over the wire. Its live connection is its API's address with ws for http, like
// wss://api.onsay.dev/live.
#define BEAT 20   // seconds between pings
#define SILENT 60   // seconds without a word before the link is taken as dead
#define FRESH 300   // seconds a signature, or a key looked up, holds
#define CALLS_CHUNK 100   // calls waiting before they are sent; the rest go with the next heartbeat

#define say core_say




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
static void sent(const char *m, size_t n) {
  static long size = -1;
  buf line = { 0 };
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
  log_add("sent.log", &size, line.s, line.n);
  buf_free(&line);
}

static int tell(pl_link *l, const char *m, size_t n) {
  int ok = pl_send(l, m, n);
  if (ok) sent(m, n);
  return ok;
}

// One request to Onsay's API: its answer as a record, and its status, or NULL with why in error when it wasn't reached.
val *onsay_api(arena *a, const char *method, const char *path, const char *body, double *status, buf *error) {
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

// One message sent and its answer waited for, like a lingo(...) or a decision, and who hears what Onsay says of it
// meanwhile. hearing: how many of those are being heard now, outside the lock.
typedef struct wait {
  struct wait *next;
  long id;
  volatile int done;
  buf reply;
  onsay_heard heard;
  void *arg;
  int hearing;
} wait;

static pl_gate *lock;   // keeps everything below, which the connection, the heartbeat and the job thread share
static pl_link *link_;   // the open link, after its hello, or NULL
static int onsay_on, ready, ponged;   // on: the lingo runs. ready: Onsay took the hello, and its statement checked out
static double heard;   // when Onsay last said anything
static char handle[64], onsay_device[64], account[64];   // this computer's handle, its name and its account id, as ready said
static unsigned char onsay_mine[32];   // this device's public key
static buf onsay_catalog, pending, calls_;   // the catalog now, and decisions and calls not sent yet, a line each
static wait *waits;
static onsay_listener listening;   // the private lingo that hears what the link doesn't know (onsay_listen)
typedef struct streamed {
  struct streamed *next;
  char id[80], path[256];
} streamed;
static streamed *streams;   // streams Onsay asked of this computer for another of the account, each frame passed on
static long ids;

int onsay_send(const char *text, size_t n) {
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
static void onsay_answered(void *arg, const char *response, size_t n) {
  buf m = { 0 };
  buf_str(&m, "{\"type\":\"response\",");
  buf_add(&m, response + 1, n - 1);
  onsay_send(m.s, m.n);
  buf_free(&m);
}

// One message of a stream this computer serves another, passed on down the link as a frame of its request.
static void relay(void *arg, const char *text, size_t n) {
  streamed *s = arg;
  buf m = { 0 };
  buf_fmt(&m, "{\"type\":\"frame\",\"id\":%s,\"text\":", s->id);
  buf_json_str(&m, text, n);
  buf_chr(&m, '}');
  onsay_send(m.s, m.n);
  buf_free(&m);
}

/* A stream Onsay asked for answered like any request, its answer the first message, and on a 200 kept open, each
   message after a frame (relay), until Onsay says stop or the link drops. */
static void opened(void *arg, const char *response, size_t n) {
  streamed *s = arg;
  arena *a = arena_new();
  val *status = rec_get(json_parse(a, response, n, NULL), "status");
  onsay_answered(NULL, response, n);
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
    lingo_hand(text, n, caller.s, onsay_answered, NULL);
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
  rec_set(a, signed_, "device", v_cstr(a, onsay_device));
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
    lingo_hand(text, n, as, onsay_answered, NULL);
  else {
    buf_str(&out, "{\"type\":\"response\",\"id\":");
    buf_json(&out, rec_get(m, "id") ? rec_get(m, "id") : NOTHING);
    buf_str(&out,
        ",\"status\":401,\"headers\":{\"content-type\":\"application/json\"},\"body\":\"{\\\"error\\\":\\\"Onsay's signature on this "
        "request doesn't check out, so it didn't run\\\"}\",\"base64\":false}");
    onsay_send(out.s, out.n);
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
    base64(&key, onsay_mine, 32);
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
      snprintf(onsay_device, sizeof onsay_device, "%s", text_or(m, "device", ""));
      snprintf(account, sizeof account, "%s", text_or(m, "account", ""));
      ready = 1;
      flush(&pending, "log", "entries");
      flush(&calls_, "calls", "calls");
      pl_leave(lock);
      say("Signed in. This computer is %s.%s", onsay_device, handle);
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
    pl_enter(lock);
    wait *w = waits;
    while (w && !((double)w->id == id->n && w->heard)) w = w->next;
    if (w) w->hearing++;
    pl_leave(lock);
    if (w) {
      w->heard(w->arg, text, n);
      pl_enter(lock);
      w->hearing--;
      pl_leave(lock);
    }
  } else if (!strcmp(type, "policy"))
    policy_pull();
  else if (!strcmp(type, "pong")) {
    pl_enter(lock);
    ponged = 1;
    pl_leave(lock);
  } else if (!listening.heard || !listening.heard(text, n))
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
    if (!onsay_on || !token || token->k != V_TEXT || !token->t.len) {
      arena_free(a);
      wait_ = 1;
      continue;
    }
    if (!key_of("device", 1, onsay_mine)) {
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
      base64(&hello, onsay_mine, 32);
      buf_str(&hello, "\",\"catalog\":");
      pl_enter(lock);
      buf_add(&hello, onsay_catalog.s ? onsay_catalog.s : "[]", onsay_catalog.s ? onsay_catalog.n : 2);
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
      if (listening.heard) listening.heard(NULL, 0);
      say("The connection to Onsay closed");
    }
    buf_free(&hello);
    arena_free(a);
  }
}

// Pings every BEAT seconds, drops a link silent for SILENT, and one whose token is gone, signed out elsewhere.
static void heartbeat(void *arg) {
  for (;;) {
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
    pl_leave(lock);
    arena_free(a);
  }
}

////////// 3. HOOKS //////////

/* A message to Onsay, {"type": type, "id": a new number, ...rest}, rest a JSON record, and its answer by that id waited
   for, answering requests meanwhile. 1 with the whole answer in reply, 0 when Onsay isn't ready, the link closed, stop
   was set or seconds passed. heard, when set, gets each step Onsay says of it meanwhile, with arg, and one stopped here
   is stopped at Onsay too. */
int onsay_exchange(const char *type, const char *rest, size_t n, volatile int *stop, double seconds, buf *reply,
    onsay_heard heard, void *arg) {
  wait w = { .heard = heard, .arg = arg };
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
  while (w.hearing) {
    pl_leave(lock);
    pl_sleep(0.01);
    pl_enter(lock);
  }
  pl_leave(lock);
  int got = w.reply.n > 0;
  if (got)
    buf_add(reply, w.reply.s, w.reply.n);
  else if (heard && *stop) {
    m.n = 0;
    buf_fmt(&m, "{\"type\":\"stop\",\"id\":%ld}", w.id);
    onsay_send(m.s, m.n);
  }
  buf_free(&m);
  buf_free(&w.reply);
  return got;
}

// A lingo(...) or use(...) a program reaches, heard by whoever follows the request it runs in (onsay_listen).
static int ask(const char *values, size_t n, volatile int *stop, buf *answer) {
  void *arg = NULL;
  onsay_heard heard = listening.follows ? listening.follows(lingo_job(), &arg) : NULL;
  return onsay_exchange("lingo", values, n, stop, 1e12, answer, heard, arg);
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
  int ok = onsay_send(m.s, m.n);
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
    val *said = onsay_api(a, "GET", path.s, NULL, &status, &error);
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

static void onsay_decided(const char *line) {
  pl_enter(lock);
  buf_str(&pending, line);
  buf_chr(&pending, '\n');
  flush(&pending, "log", "entries");
  pl_leave(lock);
}

/* A request this computer made, kept to send Onsay: sent once CHUNK wait, else with the next heartbeat. Offline, the
   newest half is kept once they pass LOG_MOST bytes. */
static void onsay_kept(const char *line) {
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
  if (lines >= CALLS_CHUNK) flush(&calls_, "calls", "calls");
  pl_leave(lock);
}

// Started, the first time: the connection's threads, and the private lingo listening told. Each time, the catalog sent
// on, as the core gives it. Stopped, the link closes.
static void onsay_running(const char *now) {
  static int started;
  pl_enter(lock);
  onsay_on = now != NULL;
  onsay_catalog.n = 0;
  if (now) buf_str(&onsay_catalog, now);
  if (link_ && !onsay_on)
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
  if (!onsay_on || started) return;
  started = 1;
  if (listening.started) listening.started();
  pl_thread(connection, NULL);
  pl_thread(heartbeat, NULL);
}

long onsay_id(void) {
  pl_enter(lock);
  long id = ++ids;
  pl_leave(lock);
  return id;
}

int onsay_ready(void) {
  pl_enter(lock);
  int is = ready && link_;
  pl_leave(lock);
  return is;
}

void onsay_name(buf *out) {
  pl_enter(lock);
  if (*onsay_device && *handle) buf_fmt(out, "%s.%s", onsay_device, handle);
  pl_leave(lock);
}

void onsay_listen(const onsay_listener *l) { listening = *l; }

////////// 4. POLICY //////////

/* The account's policy, from GET /policy: {version, policy, signed}, signed over {policy, version}. Checked, then handed to
   the core, which keeps it unless its version is older than the one kept, and what it applied sent back: the version and
   clamped, which is empty. Anything that doesn't check out is refused, and
   said. */
static void policy_pull(void) {
  arena *a = arena_new();
  buf error = { 0 };
  buf m = { 0 };
  double status = 0;
  val *said = onsay_api(a, "GET", "/policy", NULL, &status, &error);
  val *both = v_rec(a);
  val *version = rec_get(said, "version");
  val *policy = rec_get(said, "policy");
  const char *why = NULL;
  rec_set(a, both, "policy", policy ? policy : NOTHING);
  rec_set(a, both, "version", version ? version : NOTHING);
  pl_enter(lock);
  const char *name = v_cstr(a, onsay_device)->t.s;
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
      onsay_send(m.s, m.n);
    }
  }
  buf_free(&error);
  buf_free(&m);
  arena_free(a);
}

/* The account's policy from Onsay, checked and kept to what share and unshare change. Its grants are the ones to save. */
static val *policy_now(run *r, const char *what, buf *error) {
  double status = 0;
  val *said = onsay_api(r->a, "GET", "/policy", NULL, &status, error);
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
  val *saved = onsay_api(r->a, "PUT", "/policy", body.s, &status, &error);
  buf_free(&body);
  buf_free(&error);
  if (!saved) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status != 200) fail(r, "phrase", "Onsay didn't keep the policy: %s", text_or(saved, "error", "it said no"));
  policy_pull();
  return saved;
}

/* share(values: record) -> the policy: one more grant in the account's policy, {to, allow, except, pass}, allow every
   route when it names none, and every computer told (../../onsay/docs/lingo-function.md, Badges and grants). */
static val *share(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *grant = v_rec(a);
  val *grants = v_list(a);
  val *everything = v_list(a);
  if (values->k != V_REC || !*text_or(values, "to", ""))
    fail(r, "phrase", "Say who to share with, to: a handle like james, a computer like laptop.james, or public");
  val *policy = policy_now(r, "share", &error);
  buf_free(&error);
  val *old = rec_get(policy, "grants");
  list_push(a, everything, v_cstr(a, "/"));
  rec_set(a, grant, "allow", everything);
  for (size_t i = 0; i < values->r.n; i++)
    if (strstr(" to allow except pass ", arena_fmt(a, " %s ", values->r.key[i]))) rec_set(a, grant, values->r.key[i], values->r.v[i]);
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++) list_push(a, grants, old->l.v[i]);
  list_push(a, grants, grant);
  rec_set(a, policy, "grants", grants);
  return policy_save(r, policy);
}

/* unshare(values: record) -> the policy: every grant for to taken out of the account's policy. */
static val *onsay_unshare(run *r, val **v) {
  arena *a = r->a;
  buf error = { 0 };
  val *values = v[0];
  val *kept = v_list(a);
  const char *to = values->k == V_REC ? text_or(values, "to", "") : "";
  if (!*to) fail(r, "phrase", "Say who to stop sharing with, to: a handle like james, a computer like laptop.james, or public");
  val *policy = policy_now(r, "share", &error);
  buf_free(&error);
  val *old = rec_get(policy, "grants");
  for (size_t i = 0; old && old->k == V_LIST && i < old->l.n; i++)
    if (strcmp(text_or(old->l.v[i], "to", ""), to)) list_push(a, kept, old->l.v[i]);
  if (!old || old->k != V_LIST || kept->l.n == old->l.n) fail(r, "phrase", "Nothing is shared with %s", to);
  rec_set(a, policy, "grants", kept);
  return policy_save(r, policy);
}

////////// 7. SIGNING IN //////////

static int attempt,
    signing;   // the sign in now, by its number, so a newer one or signing out ends it; signing: it waits
static char onsay_failed[256];   // why the last sign in in the background didn't finish, for status

/* Asks Onsay every two seconds whether the person pressed Connect, for as long as the code lasts, 10 minutes, then keeps
   the token as this lingo's secret setting and makes this device's key. The account it signed in as, or NULL with why.
   stop is the command line's. In the background it is NULL, and a newer sign in, or signing out, ends it instead. */
static val *connect_(arena *a, const char *body, volatile int *stop, int onsay_mine, buf *why) {
  volatile int never = 0;
  unsigned char k[32];
  double status = 0;
  for (double until = pl_now() + 600; pl_now() < until;) {
    if (stop)
      lingo_wait(&never, stop, 2);
    else
      pl_sleep(2);
    pl_enter(lock);
    int ended = stop ? *stop : onsay_mine != attempt;
    pl_leave(lock);
    if (ended) {
      buf_str(why, stop ? "Stopped by the person" : "Signing in again, or signing out, ended this sign in");
      return NULL;
    }
    buf error = { 0 };
    val *poll = onsay_api(a, "POST", "/connect/poll", body, &status, &error);
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
} onsay_waiting;

// A sign in finished in the background, while the lingo client runs on. status says when it is done.
static void background(void *arg) {
  onsay_waiting *w = arg;
  arena *a = arena_new();
  buf why = { 0 };
  int done = connect_(a, w->body, NULL, w->mine, &why) != NULL;
  if (!done) say("%s", why.s);
  pl_enter(lock);
  if (w->mine == attempt) {
    signing = 0;
    snprintf(onsay_failed, sizeof onsay_failed, "%s", done ? "" : why.s);
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
  val *place[2] = { v_cstr(a, "onsay/install"), NOTHING };
  val *install = phrase_find("store.get")->fn(r, place);
  val *out = v_rec(a);
  if (install->k != V_TEXT) {
    buf made = { 0 };
    pl_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; i++) buf_fmt(&made, "%02x", random[i]);
    install = v_text(a, made.s, made.n);
    place[1] = install;
    phrase_find("store.put")->fn(r, place);
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
  val *start = onsay_api(a, "POST", "/connect", body.s, &status, &error);
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
  if (onsay_on || lingo_serves()) {
    onsay_waiting *w = malloc(sizeof *w + body.n + 1);
    memcpy(w->body, body.s, body.n + 1);
    pl_enter(lock);
    w->mine = ++attempt;
    signing = 1;
    *onsay_failed = 0;
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

// Forgets the token and drops the connection. This computer's key stays: it is the computer's own, and the badges it
// made, like an app's here, keep working.
static val *sign_out(run *r, val **v) {
  if (!lingo_set("onsay", "token", NOTHING)) fail(r, "phrase", "This computer couldn't forget its token");
  pl_enter(lock);
  attempt++;
  signing = 0;
  *handle = *onsay_device = *account = 0;
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
  rec_set(a, out, "failed", *onsay_failed ? v_cstr(a, onsay_failed) : NOTHING);
  rec_set(a, out, "running", VBOOL(onsay_on));
  rec_set(a, out, "connected", VBOOL(ready));
  rec_set(a, out, "handle", *handle ? v_cstr(a, handle) : NOTHING);
  rec_set(a, out, "device", *onsay_device ? v_cstr(a, onsay_device) : NOTHING);
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
  val *said = onsay_api(a, "GET", "/lingos", NULL, &status, &error);
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
  val *said = onsay_api(r->a, "GET", "/ai", NULL, &status, &error);
  buf_free(&error);
  if (!said) fail(r, "phrase", "Onsay couldn't be reached. Check that this computer is online, then try again");
  if (status == 401) fail(r, "phrase", "Sign in to use Onsay's AI");
  if (status != 200) fail(r, "phrase", "Onsay couldn't say: %s", text_or(said, "error", "it said no"));
  return said;
}

////////// 8. ITS PHRASES //////////

void base_calls(void);   // the calls every computer has, built for its platform: windows.c or linux.c
void agent_calls(void);   // ../lingos/onsay/agent.c

// Called once as the lingo client starts (the engine, BUILTINS).
void onsay_lingo(void) {
  static const lingo_hooks hooks = { onsay_running, onsay_decided, ask, key, said, onsay_kept };
  lock = pl_gate_new();
  lingo_hook(&hooks);
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
      "share(values: record) -> {version, policy, signed}: one more grant in the account's policy, {to, allow, except, "
      "pass}: to is a handle, a computer like laptop.james, or public, allow addresses like [\"/photos\"], every route "
      "when none. Every computer is told",
      share);
  phrase_add("onsay", "net",
      "unshare(values: record) -> {version, policy, signed}: every grant for to taken out of the account's policy",
      onsay_unshare);
  base_calls();
  agent_calls();
}

//////////////////// 10. HOST ////////////////////

// The lingo client as a program of its own, on a device whose floor has threads and a port.
//
//   onsay-core settings.json                 runs it: its port, and its lingos, any of which may connect it to more,
//                                            like the onsay lingo to Onsay
//   onsay-core settings.json <verb> ...      one thing done, whether or not it runs, its answer printed as JSON:
//     pack <folder> [<to>]                   a folder of a lingo's files made into its .lingo file
//     start <lingo>, stop <lingo>            a lingo running or stopped
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
// pause. A crash under a phrase ends only its request, with an error (pl_faults, in running).

#define HEAD (64 << 10)   // the most a web request's first line and headers may be
#define BODY_ROOM (4 << 20)   // and its body
#define QUIET 10   // seconds a connection to the port may say nothing before it is closed, or a stream asked if it is there

// One request for main: its text, its caller (NULL for the port, its ip in it), and who gets its answer.
typedef struct message {
  struct message *next;
  char *text, *caller;
  size_t n;
  void (*answered)(void *arg, const char *response, size_t n);
  void *arg;
} message;

static pl_gate *host_gate;
static pl_gate *watch;   // keeps watchers, and each frame whole on its connection
static message *first, *last;
static long requests;   // web requests taken from the port, for their ids
static int port, outside;   // the port, and whether it is open to other computers, as core_look says
static char outermost[128];   // the id, as JSON, of the request main answers now, with every other waiting inside it

////////// JOBS //////////

// One request answered, on main. On Android each is written to the log as it starts and ends, but the app's polls, so
// one that never ends shows there, since nobody there sees the program's own window.
static void host_handle(message *m) {
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
    char *text, size_t n, const char *caller, void (*host_answered)(void *arg, const char *response, size_t n), void *arg) {
  message *m = malloc(sizeof *m);
  *m = (message){ NULL, text, caller ? strdup(caller) : NULL, n, host_answered, arg };
  pl_enter(host_gate);
  if (last)
    last->next = m;
  else
    first = m;
  last = m;
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// The next request, waiting up to seconds for one, or NULL.
static message *next(double seconds, volatile int *done, volatile int *stop) {
  pl_enter(host_gate);
  for (double until = pl_now() + seconds; !first && !(done && *done) && !(stop && *stop) && pl_now() < until;)
    pl_wait(host_gate, until - pl_now() < 1 ? until - pl_now() : 1);
  message *m = done && *done ? NULL : first;
  if (m) {
    first = m->next;
    if (!first) last = NULL;
  }
  pl_leave(host_gate);
  return m;
}

// lingo.h: a request from elsewhere, copied, in the queue.
void lingo_hand(const char *request, size_t n, const char *caller,
    void (*host_answered)(void *arg, const char *response, size_t n), void *arg) {
  char *text = malloc(n + 1);
  memcpy(text, request, n);
  text[n] = 0;
  queue(text, n, caller, host_answered, arg);
}

// lingo.h
void lingo_stop(const char *id) {
  core_stop(id);
  pl_enter(host_gate);
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// lingo.h: requests that come meanwhile are answered here, inside the one that waits, since what answers may be here.
void lingo_wait(volatile int *done, volatile int *stop, double seconds) {
  for (double until = pl_now() + seconds; !*done && !*stop && pl_now() < until;) {
    message *m = next(until - pl_now(), done, stop);
    if (m) host_handle(m);
  }
}

// lingo.h
void lingo_done(volatile int *done) {
  pl_enter(host_gate);
  *done = 1;
  pl_wake(host_gate);
  pl_leave(host_gate);
}

// main: every request in turn, and a look at the lingos folder every CORE_LOOK seconds, however busy.
static void jobs(void) {
  for (double look = pl_now() + CORE_LOOK;;) {
    message *m = next(CORE_LOOK, NULL, NULL);
    if (pl_now() >= look) {
      double began = pl_now();
      int wide = core_look();
      if (pl_now() - began > 2) core_say("looking at the lingos folder took %.1f seconds", pl_now() - began);
      pl_enter(host_gate);
      outside = wide;
      pl_leave(host_gate);
      look = pl_now() + CORE_LOOK;
    }
    if (!m) continue;
    arena *a = arena_new();
    val *request = json_parse(a, m->text, m->n, NULL);
    val *id = request ? rec_get(request, "id") : NULL;
    buf said = { 0 };
    if (id) buf_json(&said, id);
    pl_enter(host_gate);
    snprintf(outermost, sizeof outermost, "%s", said.s ? said.s : "");
    pl_leave(host_gate);
    buf_free(&said);
    arena_free(a);
    host_handle(m);
    pl_enter(host_gate);
    outermost[0] = 0;
    pl_leave(host_gate);
  }
}

#ifdef __ANDROID__
// Stops everything running, from any thread. On Android the core lives inside the app, so its Stop can't start the
// program again the way Desktop's does.
void host_stop(void) {
  char id[sizeof outermost];
  pl_enter(host_gate);
  snprintf(id, sizeof id, "%s", outermost);
  pl_leave(host_gate);
  if (id[0]) lingo_stop(id);
}
#endif

////////// STREAMS //////////

/* A web request asking to be a WebSocket, a GET with Upgrade: websocket, is answered like any other first, so every
   check applies, and on a 200 it stays open: its body is the first message, then each the lingo at its path pushes
   (lingo_push), until either side closes it. What the other side sends is not read, but its close. */
typedef struct watcher {
  struct watcher *next;
  pl_conn *c;   // a connection to the port, or NULL for a native lingo's (lingo_watch)
  void (*send)(void *arg, const char *text, size_t n);
  void *arg;
  char path[256];
} watcher;
static watcher *watchers;

// One frame, unmasked, as a server sends it: text, opcode 1, or a ping, 9. Under watch.
static int ws_frame(pl_conn *c, int opcode, const char *text, size_t n) {
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
  return pl_put(c, (const char *)head, k) && (!n || pl_put(c, text, n));
}

// lingo.h
void lingo_push(const char *path, const char *text, size_t n) {
  pl_enter(watch);
  for (watcher *w = watchers; w; w = w->next)
    if (strcmp(w->path, path))
      continue;
    else if (w->c)
      ws_frame(w->c, 1, text, n);
    else
      w->send(w->arg, text, n);
  pl_leave(watch);
}

// lingo.h
void lingo_watch(const char *path, void (*send)(void *arg, const char *text, size_t n), void *arg) {
  watcher *w = calloc(1, sizeof *w);
  w->send = send;
  w->arg = arg;
  snprintf(w->path, sizeof w->path, "%s", path);
  pl_enter(watch);
  w->next = watchers;
  watchers = w;
  pl_leave(watch);
}

// lingo.h
void lingo_unwatch(void *arg) {
  pl_enter(watch);
  for (watcher **at = &watchers; *at; at = &(*at)->next)
    if (!(*at)->c && (*at)->arg == arg) {
      watcher *gone = *at;
      *at = gone->next;
      free(gone);
      break;
    }
  pl_leave(watch);
}

// On the visitor's thread, until it closes: a quiet 10 seconds is asked about with a ping, and nothing coming back at
// once means it closed.
static void stream(pl_conn *c, const char *path, size_t n, const char *key, const char *first, size_t size) {
  buf joined = { 0 };
  buf accept = { 0 };
  buf head = { 0 };
  unsigned char digest[20];
  buf came = { 0 };
  watcher w = { NULL, c, NULL, NULL, "" };
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
  int open = pl_put(c, head.s, head.n) && ws_frame(c, 1, first, size);
  if (open) {
    w.next = watchers;
    watchers = &w;
  }
  pl_leave(watch);
  while (open) {
    double began = pl_now();
    came.n = 0;
    if (pl_get(c, &came, QUIET)) {
      open = (came.s[0] & 0x0f) != 8;
      continue;
    }
    pl_enter(watch);
    open = pl_now() - began > 1 && ws_frame(c, 9, "", 0);
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
  buf_free(&came);
}

////////// THE PORT //////////

// An answer written to a web request's connection: its status, its headers by lower case name, and its body.
static void respond(pl_conn *c, int status, val *h, const char *body, size_t n) {
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
  pl_put(c, out.s, out.n);
  buf_free(&out);
}

typedef struct {
  buf response;
  int done;
} host_answer;   // a web request's answer, which its thread waits for

static void host_answered(void *arg, const char *response, size_t n) {
  host_answer *to = arg;
  buf_add(&to->response, response, n);
  lingo_done(&to->done);
}

typedef struct {
  pl_conn *c;
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
  const char *end = NULL;
  const char *bad = NULL;
  int status = 400;
  int got = 1;
  size_t method = 0;
  size_t path = 0;
  size_t size = 0;
  size_t from = 0;
  val *headers = v_rec(a);
  while (got && !(end = in.s ? strstr(in.s, "\r\n\r\n") : NULL) && in.n < HEAD) got = pl_get(v.c, &in, QUIET);
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
    else if (!bad && (strlen(length) > 9 || (size = strtoul(length, NULL, 10)) > BODY_ROOM)) {
      status = 413;
      bad = "Its body is more than 4 MB";
    }
    from = (size_t)(end - in.s) + 4;
    if (!bad && !strcmp(text_or(headers, "expect", ""), "100-continue") && in.n - from < size)
      pl_put(v.c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (!bad && in.n - from < size && (got = pl_get(v.c, &in, QUIET))) {}
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
    host_answer to = { { 0 }, 0 };
    buf msg = { 0 };
    const char *ip = v.from.s ? v.from.s : "";
    pl_enter(host_gate);
    buf_fmt(&msg, "{\"id\":\"port %ld\",\"method\":\"%.*s\",\"path\":", ++requests, (int)method, in.s);
    pl_leave(host_gate);
    buf_json_str(&msg, in.s + method + 1, path);
    buf_str(&msg, ",\"ip\":");
    buf_json_str(&msg, ip, strlen(ip));
    buf_str(&msg, ",\"headers\":");
    buf_json(&msg, headers);
    buf_str(&msg, ",\"body\":");
    buf_json_str(&msg, in.s + from, size);
    buf_chr(&msg, '}');
    queue(msg.s, msg.n, NULL, host_answered, &to);   // the queue owns the text now
    pl_enter(host_gate);
    while (!to.done) pl_wait(host_gate, 1);
    pl_leave(host_gate);
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
  pl_drop(v.c);
  buf_free(&in);
  buf_free(&v.from);
  arena_free(a);
}

// The port, on every address this computer has when the policy lets anyone else in, else on this computer alone, so
// nothing asks to be let through its firewall until something is shared. NULL when it can't be, said when say is set.
static pl_conn *listen_(int everywhere, int loud) {
  buf error = { 0 };
  pl_conn *p = pl_bind("tcp", port, everywhere, &error);
  if (p)
    core_say("Answering web requests on port %d, %s", port,
        everywhere ? "from other computers too" : "from this computer only");
  else if (loud)
    core_say("%s, so it answers no web requests", error.s);
  buf_free(&error);
  return p;
}

// Takes each connection to the port, and reads it on a thread of its own. When the policy changes who it lets in, the
// port is opened again to fit, within a second of main's look, and tried again each second while it can't be.
static void listener(void *arg) {
  pl_conn *p = arg;
  int wide = outside;
  for (;;) {
    pl_enter(host_gate);
    int want = outside;
    pl_leave(host_gate);
    if (want != wide || !p) {
      if (p) pl_drop(p);
      p = listen_(want, want != wide);
      wide = want;
      if (!p) {
        pl_sleep(1);
        continue;
      }
    }
    visit *v = calloc(1, sizeof *v);
    if ((v->c = pl_next(p, &v->from, 1)))
      pl_thread(visitor, v);
    else {
      buf_free(&v->from);
      free(v);
    }
  }
}

////////// MAIN //////////

// What each verb takes, by name, in order. The last takes the rest of the words, joined by spaces, or for call
// as a list, and set's value and call's values are read as JSON when they are JSON.
static const char *TAKES[][4] = { { "start", "lingo" }, { "stop", "lingo" },
  { "policy" }, { "settings", "lingo" }, { "set", "lingo", "key", "value" }, { "manifest", "lingo" },
  { "call", "fn", "values" }, { "badge", "value" } };

int main(int argc, char **argv) {
  nonce_gate = pl_gate_new();
  const char *path = argc > 1 ? argv[1] : "onsay.json";
  const char *verb = argc > 2 ? argv[2] : NULL;
  size_t k = 0;
  size_t count = sizeof TAKES / sizeof *TAKES;
  while (verb && k < count && strcmp(TAKES[k][0], verb)) k++;
  if (verb && k == count && strcmp(verb, "pack")) {
    core_say(
        "onsay-core %s %s isn't something it does. It does pack, start, stop, policy, settings, set, manifest, call and badge",
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
  host_gate = pl_gate_new();
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
      v = !strcmp(take, "values") ? list : v_text(a, rest.s, rest.n);
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

#include "hardware.c"
