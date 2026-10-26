// platform_linux.c - platform.h for Linux, headless servers first, with the web through libcurl loaded as it starts.
//
// 1. Time and threads   clock_gettime, pthreads, and the gate as a mutex and a condition
// 2. Curl               libcurl found at run time, so the core needs no curl to build and says plainly when it's missing
// 3. Links              a WebSocket written here on a connection curl opens (TCP, TLS and proxies), RFC 6455
// 4. Files and device, and loading a native lingo
// 5. Web                http.send, one request through curl
// libcurl's own WebSocket API is left alone, since many distributions build curl without it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "platform.h"

////////// 1. TIME AND THREADS //////////

double pl_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void pl_sleep(double seconds) {
  if (seconds <= 0) return;
  struct timespec t = { (time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9) };
  while (nanosleep(&t, &t) && errno == EINTR) {}
}

int pl_stopped(void) { return 0; }   // host.c stops a job directly, from its receiver thread

typedef struct { void (*fn)(void *); void *arg; } start;

static void *begin(void *p) {
  start s = *(start *)p;
  free(p);
  s.fn(s.arg);
  return NULL;
}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  pthread_t t;
  if (pthread_create(&t, NULL, begin, s)) free(s);
  else pthread_detach(t);
}

// Recursive, like the Windows gate, and timed on the monotonic clock, so a change of the time of day never ends a wait.
struct pl_gate { pthread_mutex_t lock; pthread_cond_t wake; };

pl_gate *pl_gate_new(void) {
  pl_gate *g = malloc(sizeof *g);
  pthread_mutexattr_t m;
  pthread_mutexattr_init(&m);
  pthread_mutexattr_settype(&m, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g->lock, &m);
  pthread_mutexattr_destroy(&m);
  pthread_condattr_t c;
  pthread_condattr_init(&c);
  pthread_condattr_setclock(&c, CLOCK_MONOTONIC);
  pthread_cond_init(&g->wake, &c);
  pthread_condattr_destroy(&c);
  return g;
}
void pl_enter(pl_gate *g) { pthread_mutex_lock(&g->lock); }
void pl_leave(pl_gate *g) { pthread_mutex_unlock(&g->lock); }
void pl_wake(pl_gate *g) { pthread_cond_broadcast(&g->wake); }

void pl_wait(pl_gate *g, double seconds) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  long long ns = (long long)t.tv_nsec + (long long)(seconds * 1e9);
  t.tv_sec += (time_t)(ns / 1000000000);
  t.tv_nsec = (long)(ns % 1000000000);
  pthread_cond_timedwait(&g->wake, &g->lock, &t);
}

////////// 2. CURL //////////

// The few parts of curl/curl.h the core uses, with their numbers, which curl never changes.
typedef void CURL;
struct curl_slist;
enum {
  CURLE_OK = 0, CURLE_UNSUPPORTED_PROTOCOL = 1, CURLE_URL_MALFORMAT = 3, CURLE_COULDNT_RESOLVE_HOST = 6,
  CURLE_COULDNT_CONNECT = 7, CURLE_WRITE_ERROR = 23, CURLE_OPERATION_TIMEDOUT = 28, CURLE_SSL_CONNECT_ERROR = 35,
  CURLE_TOO_MANY_REDIRECTS = 47, CURLE_PEER_FAILED_VERIFICATION = 60, CURLE_SSL_CACERT = 77, CURLE_AGAIN = 81,
  CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
  CURLOPT_HTTPHEADER = 10023, CURLOPT_HEADERDATA = 10029, CURLOPT_CUSTOMREQUEST = 10036, CURLOPT_WRITEFUNCTION = 20011,
  CURLOPT_HEADERFUNCTION = 20079, CURLOPT_NOBODY = 44, CURLOPT_FOLLOWLOCATION = 52, CURLOPT_POSTFIELDSIZE = 60,
  CURLOPT_MAXREDIRS = 68, CURLOPT_NOSIGNAL = 99, CURLOPT_CONNECT_ONLY = 141, CURLOPT_TIMEOUT_MS = 155,
  CURLOPT_CONNECTTIMEOUT_MS = 156,
  CURLINFO_RESPONSE_CODE = 0x200002, CURLINFO_ACTIVESOCKET = 0x500000 + 44,
  CURL_GLOBAL_ALL = 3,
};

static struct {
  int ok;
  int (*global_init)(long flags);
  CURL *(*init)(void);
  void (*cleanup)(CURL *c);
  int (*setopt)(CURL *c, int option, ...);
  int (*getinfo)(CURL *c, int info, ...);
  int (*perform)(CURL *c);
  int (*send)(CURL *c, const void *data, size_t n, size_t *sent);
  int (*recv)(CURL *c, void *data, size_t n, size_t *got);
  const char *(*strerror)(int code);
  struct curl_slist *(*slist_append)(struct curl_slist *list, const char *line);
  void (*slist_free_all)(struct curl_slist *list);
} curl;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

// Once, before any thread uses it. A write to a closed connection is an error here, never a signal that ends the core.
static void curl_load(void) {
  void *m = dlopen("libcurl.so.4", RTLD_NOW);
  if (!m) return;
  *(void **)&curl.global_init = dlsym(m, "curl_global_init");
  *(void **)&curl.init = dlsym(m, "curl_easy_init");
  *(void **)&curl.cleanup = dlsym(m, "curl_easy_cleanup");
  *(void **)&curl.setopt = dlsym(m, "curl_easy_setopt");
  *(void **)&curl.getinfo = dlsym(m, "curl_easy_getinfo");
  *(void **)&curl.perform = dlsym(m, "curl_easy_perform");
  *(void **)&curl.send = dlsym(m, "curl_easy_send");
  *(void **)&curl.recv = dlsym(m, "curl_easy_recv");
  *(void **)&curl.strerror = dlsym(m, "curl_easy_strerror");
  *(void **)&curl.slist_append = dlsym(m, "curl_slist_append");
  *(void **)&curl.slist_free_all = dlsym(m, "curl_slist_free_all");
  curl.ok = curl.global_init && curl.init && curl.cleanup && curl.setopt && curl.getinfo && curl.perform && curl.send &&
            curl.recv && curl.strerror && curl.slist_append && curl.slist_free_all && !curl.global_init(CURL_GLOBAL_ALL);
  signal(SIGPIPE, SIG_IGN);
}

#define NO_CURL "This computer has no libcurl, which the core needs to reach the web. Install it, like apt install libcurl4 (libcurl4t64 on newer Ubuntu) or dnf install libcurl"

static void why(buf *error, const char *what, int code) {
  const char *plain = code == CURLE_COULDNT_CONNECT ? "nothing answered at that address"
                    : code == CURLE_COULDNT_RESOLVE_HOST ? "that server name wasn't found"
                    : code == CURLE_OPERATION_TIMEDOUT ? "it took too long"
                    : code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT || code == CURLE_SSL_CONNECT_ERROR ? "its certificate wasn't trusted"
                    : code == CURLE_URL_MALFORMAT || code == CURLE_UNSUPPORTED_PROTOCOL ? "that isn't a whole web address"
                    : code == CURLE_TOO_MANY_REDIRECTS ? "it sent it on too many times"
                    : curl.strerror(code);
  buf_fmt(error, "%s: %s (curl %d)", what, plain, code);
}

////////// 3. LINKS //////////

struct pl_link {
  CURL *c;
  int socket;
  pthread_mutex_t io;        // around every curl_easy_send and curl_easy_recv, never held while waiting
  pthread_mutex_t sending;   // one frame at a time, whole
  volatile int closed;
  double until;              // while it opens, when to give up waiting
  buf held;                  // what was read and isn't a whole frame yet
};

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(buf *out, const unsigned char *s, size_t n) {
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
    buf_chr(out, ALPHABET[v >> 18 & 63]);
    buf_chr(out, ALPHABET[v >> 12 & 63]);
    buf_chr(out, i + 1 < n ? ALPHABET[v >> 6 & 63] : '=');
    buf_chr(out, i + 2 < n ? ALPHABET[v & 63] : '=');
  }
}

// SHA-1, only for the server's Sec-WebSocket-Accept, which is base64 of SHA-1 of the key and the protocol's own GUID.
static void sha1(const unsigned char *s, size_t n, unsigned char digest[20]) {
  unsigned h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  size_t total = (n + 9 + 63) / 64 * 64;
  unsigned char *m = calloc(total, 1);
  memcpy(m, s, n);
  m[n] = 0x80;
  for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)((unsigned long long)n * 8 >> (8 * i));
  for (size_t at = 0; at < total; at += 64) {
    unsigned w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (unsigned)m[at + 4 * i] << 24 | (unsigned)m[at + 4 * i + 1] << 16 | (unsigned)m[at + 4 * i + 2] << 8 | m[at + 4 * i + 3];
    for (int i = 16; i < 80; i++) { unsigned x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = x << 1 | x >> 31; }
    for (int i = 0; i < 80; i++) {
      unsigned f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
      unsigned k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
      unsigned t = (a << 5 | a >> 27) + f + e + k + w[i];
      e = d, d = c, c = b << 30 | b >> 2, b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  free(m);
  for (int i = 0; i < 20; i++) digest[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

// Waits for the socket to be ready, a second at a time so a close is seen. 0 once the link is closed, or out of time.
static int ready(pl_link *l, short what) {
  struct pollfd p = { l->socket, what, 0 };
  while (!l->closed && !(l->until && pl_now() > l->until)) if (poll(&p, 1, 1000) != 0) return !l->closed;
  return 0;
}

// More of what the server sent, added to held. Everything curl has buffered is read before waiting on the socket.
static int read_more(pl_link *l) {
  char chunk[16384];
  for (;;) {
    if (l->closed) return 0;
    size_t got = 0;
    pthread_mutex_lock(&l->io);
    int e = curl.recv(l->c, chunk, sizeof chunk, &got);
    pthread_mutex_unlock(&l->io);
    if (e == CURLE_OK) { buf_add(&l->held, chunk, got); return got > 0; }
    if (e != CURLE_AGAIN || !ready(l, POLLIN)) return 0;
  }
}

static int write_all(pl_link *l, const char *s, size_t n) {
  while (n) {
    if (l->closed) return 0;
    size_t sent = 0;
    pthread_mutex_lock(&l->io);
    int e = curl.send(l->c, s, n, &sent);
    pthread_mutex_unlock(&l->io);
    if (e == CURLE_OK) s += sent, n -= sent;
    else if (e != CURLE_AGAIN || !ready(l, POLLOUT)) return 0;
  }
  return 1;
}

// One frame from this side, which is always masked.
static int send_frame(pl_link *l, int op, const char *s, size_t n) {
  unsigned char mask[4];
  if (getrandom(mask, 4, 0) != 4) return 0;
  buf f = { 0 };
  buf_chr(&f, (char)(0x80 | op));
  if (n < 126) buf_chr(&f, (char)(0x80 | n));
  else if (n < 65536) { buf_chr(&f, (char)(0x80 | 126)); buf_chr(&f, (char)(n >> 8)); buf_chr(&f, (char)n); }
  else { buf_chr(&f, (char)(0x80 | 127)); for (int i = 7; i >= 0; i--) buf_chr(&f, (char)((unsigned long long)n >> (8 * i))); }
  buf_add(&f, (const char *)mask, 4);
  for (size_t i = 0; i < n; i++) buf_chr(&f, (char)(s[i] ^ mask[i & 3]));
  pthread_mutex_lock(&l->sending);
  int ok = write_all(l, f.s, f.n);
  pthread_mutex_unlock(&l->sending);
  buf_free(&f);
  return ok;
}

pl_link *pl_connect(const char *url, buf *error) {
  pthread_once(&curl_once, curl_load);
  if (!curl.ok) { buf_str(error, NO_CURL); return NULL; }
  int secure = !strncmp(url, "wss://", 6);
  if (!secure && strncmp(url, "ws://", 5)) { buf_str(error, "The server address: that isn't a whole web address"); return NULL; }
  // curl opens the connection to the address as http or https, and the request to upgrade it is written here.
  unsigned char random[16], digest[20];
  if ((size_t)getrandom(random, sizeof random, 0) != sizeof random) { buf_str(error, "This computer gave no random numbers"); return NULL; }
  const char *host = url + (secure ? 6 : 5), *path = strchr(host, '/');
  if (!path) path = host + strlen(host);
  buf http = { 0 }, ask = { 0 }, key = { 0 }, expect = { 0 };
  buf_fmt(&http, "%s://%.*s", secure ? "https" : "http", (int)(path - host), host);
  base64(&key, random, sizeof random);
  buf_fmt(&expect, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key.s);
  sha1((const unsigned char *)expect.s, expect.n, digest);
  expect.n = 0;
  base64(&expect, digest, sizeof digest);
  buf_fmt(&ask, "GET %s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\nUser-Agent: onsay-core\r\n\r\n", *path ? path : "/", (int)(path - host), host, key.s);

  pl_link *l = calloc(1, sizeof *l);
  pthread_mutex_init(&l->io, NULL);
  pthread_mutex_init(&l->sending, NULL);
  l->socket = -1;
  l->c = curl.init();
  long yes = 1, ms = 10000;
  if (!l->c) buf_str(error, "Connecting: curl couldn't start");
  else {
    curl.setopt(l->c, CURLOPT_URL, http.s);
    curl.setopt(l->c, CURLOPT_CONNECT_ONLY, yes);
    curl.setopt(l->c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(l->c, CURLOPT_CONNECTTIMEOUT_MS, ms);
    int e = curl.perform(l->c);
    if (e) why(error, "Connecting", e);
    else if (curl.getinfo(l->c, CURLINFO_ACTIVESOCKET, &l->socket) || l->socket < 0) buf_str(error, "Connecting: curl gave no connection");
  }

  // Its answer is read up to the blank line after its headers, within 10 seconds. Anything after it is the first frames.
  char *end = NULL;
  l->until = pl_now() + 10;
  if (!error->n && !write_all(l, ask.s, ask.n)) buf_str(error, "Opening the connection: the request to open it couldn't be sent");
  while (!error->n && !(end = l->held.s ? strstr(l->held.s, "\r\n\r\n") : NULL)) {
    if (!read_more(l)) buf_str(error, pl_now() > l->until ? "Opening the connection: it took too long" : "Opening the connection: the server closed it");
    buf_chr(&l->held, 0), l->held.n--;   // a 0 after it, for strstr, never counted
  }
  l->until = 0;
  if (!error->n) {
    int status = 0;
    sscanf(l->held.s, "HTTP/%*s %d", &status);
    size_t head = (size_t)(end - l->held.s) + 4;
    int accepted = 0;
    for (char *line = strstr(l->held.s, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n"))
      if (!strncasecmp(line + 2, "sec-websocket-accept:", 21)) {
        char *v = line + 23;
        while (*v == ' ') v++;
        accepted = !strncmp(v, expect.s, expect.n) && (v[expect.n] == '\r' || v[expect.n] == ' ');
      }
    if (status != 101) buf_fmt(error, "The server answered %d instead of opening a connection", status);
    else if (!accepted) buf_str(error, "The server's answer to opening the connection wasn't right");
    memmove(l->held.s, l->held.s + head, l->held.n - head);
    l->held.n -= head;
  }
  buf_free(&http), buf_free(&ask), buf_free(&key), buf_free(&expect);
  if (error->n) { pl_free(l); return NULL; }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) { return !l->closed && send_frame(l, 1, text, n); }

// Frames until a whole message: pieces of one joined, a ping answered, a close the end.
int pl_receive(pl_link *l, buf *out) {
  for (;;) {
    // Its length is in 7 bits, or in the 2 or 8 bytes after them, then a mask, which a server shouldn't send.
    unsigned char *h = (unsigned char *)l->held.s;
    size_t n = l->held.n >= 2 ? h[1] & 127 : 0, size = n == 126 ? 2 : n == 127 ? 8 : 0;
    size_t at = 2 + size + (l->held.n >= 2 && h[1] & 0x80 ? 4 : 0);
    if (l->held.n < at) { if (!read_more(l)) return 0; continue; }
    if (size) { n = 0; for (size_t i = 0; i < size; i++) n = n << 8 | h[2 + i]; }
    if (n > (1u << 30)) return 0;   // a message of more than a GB is no message from Onsay
    if (l->held.n < at + n) { if (!read_more(l)) return 0; continue; }
    int op = h[0] & 15, fin = h[0] & 0x80;
    char *body = l->held.s + at;
    if (h[1] & 0x80) for (size_t i = 0; i < n; i++) body[i] ^= (char)h[at - 4 + (i & 3)];
    if (op == 8) { l->closed = 1; return 0; }
    if (op == 9 && !send_frame(l, 10, body, n)) return 0;
    if (op <= 2) buf_add(out, body, n);
    memmove(l->held.s, l->held.s + at + n, l->held.n - at - n);
    l->held.n -= at + n;
    if (op <= 2 && fin) { if (!out->s) buf_add(out, "", 0); return 1; }
  }
}

// Shutting the socket ends a receive waiting on it at once, which is how the heartbeat drops a dead link.
void pl_close(pl_link *l) {
  if (__atomic_exchange_n(&l->closed, 1, __ATOMIC_SEQ_CST)) return;
  if (l->socket >= 0) shutdown(l->socket, SHUT_RDWR);
}

void pl_free(pl_link *l) {
  pl_close(l);
  if (l->c) curl.cleanup(l->c);
  pthread_mutex_destroy(&l->io);
  pthread_mutex_destroy(&l->sending);
  buf_free(&l->held);
  free(l);
}

////////// 4. FILES AND DEVICE //////////

int pl_read(const char *path, buf *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  char chunk[16384];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(out, chunk, n);
  fclose(f);
  if (!out->s) buf_add(out, "", 0);
  return 1;
}

int pl_write(const char *path, const char *data, size_t n) {
  buf temp = { 0 };
  buf_fmt(&temp, "%s.new", path);
  FILE *f = fopen(temp.s, "wb");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fflush(f) && !fsync(fileno(f)) && !fclose(f) && ok;
  ok = ok && !rename(temp.s, path);
  if (!ok) remove(temp.s);
  buf_free(&temp);
  return ok;
}

int pl_delete(const char *path) { return !remove(path) || errno == ENOENT; }

int pl_folder(const char *path) {
  char *p = strdup(path);
  for (char *c = p + 1; *c; c++) {
    if (*c != '/') continue;
    *c = 0;
    mkdir(p, 0777);
    *c = '/';
  }
  int ok = !mkdir(p, 0777) || errno == EEXIST;
  free(p);
  return ok;
}

void pl_list(const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
  DIR *d = opendir(folder);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    buf path = { 0 };
    buf_fmt(&path, "%s/%s", folder, e->d_name);
    struct stat s;
    if (!stat(path.s, &s))
      each(e->d_name, S_ISDIR(s.st_mode), (double)s.st_size, (double)s.st_mtim.tv_sec + (double)s.st_mtim.tv_nsec / 1e9, arg);
    buf_free(&path);
  }
  closedir(d);
}

// A server has no app or window in front. The zone is TZ, or the system's own: /etc/timezone, or where /etc/localtime
// points, like /usr/share/zoneinfo/America/New_York. The language is LANG's, like en_US.UTF-8 as en-US.
void pl_context(buf *out) {
  time_t now = time(NULL);
  struct tm t;
  localtime_r(&now, &t);
  long offset = t.tm_gmtoff / 60, at = labs(offset);
  buf_fmt(out, "{\"os\":\"linux\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
    t.tm_hour, t.tm_min, t.tm_sec, offset < 0 ? '-' : '+', at / 60, at % 60);
  char zone[256] = "", link[256] = "", *z = getenv("TZ");
  FILE *f = NULL;
  ssize_t n = 0;
  if (z && *z) snprintf(zone, sizeof zone, "%s", z + (*z == ':'));
  else if ((f = fopen("/etc/timezone", "r"))) {
    if (!fgets(zone, sizeof zone, f)) zone[0] = 0;
    fclose(f);
  }
  if (!zone[0] && (n = readlink("/etc/localtime", link, sizeof link - 1)) > 0) {
    link[n] = 0;
    if ((z = strstr(link, "zoneinfo/"))) snprintf(zone, sizeof zone, "%s", z + 9);
  }
  zone[strcspn(zone, "\r\n")] = 0;
  if (zone[0]) { buf_str(out, ",\"zone\":"); buf_json_str(out, zone, strlen(zone)); }
  char language[64] = "", *lang = getenv("LANG");
  if (lang && *lang && strcmp(lang, "C") && strncmp(lang, "C.", 2) && strcmp(lang, "POSIX")) snprintf(language, sizeof language, "%s", lang);
  language[strcspn(language, ".@")] = 0;
  for (char *c = language; *c; c++) if (*c == '_') *c = '-';
  if (language[0]) { buf_str(out, ",\"language\":"); buf_json_str(out, language, strlen(language)); }
  buf_chr(out, '}');
}

// The core runs as its own program here too, so its lingos are native libraries and it has no host lingos.
int pl_hosts(void) { return 0; }
int pl_call(const char *id, const char *values, size_t n, buf *out) { (void)id, (void)values, (void)n, (void)out; return 0; }

// Never closed: its phrases are in the table from now on. The core is built with -rdynamic, so a library finds its words.
lingo_entry pl_library(const char *path, buf *error) {
  void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!m) { buf_fmt(error, "Linux can't load it: %s", dlerror()); return NULL; }
  lingo_entry entry;
  *(void **)&entry = dlsym(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

////////// 5. WEB //////////

#define ROOM (4u << 20)   // the most an answer may be

// The answer's headers, "Name: value" lines, as a record by lower case name. A name said twice has both, joined by ", ".
static val *header_record(arena *a, const char *s) {
  val *out = v_rec(a);
  for (const char *end; *s; s = *end ? end + 2 : end) {
    if (!(end = strstr(s, "\r\n"))) end = s + strlen(s);
    const char *colon = memchr(s, ':', (size_t)(end - s)), *v = colon;
    if (!colon || colon == s) continue;
    while (++v < end && *v == ' ') {}
    buf name = { 0 }, both = { 0 };
    for (const char *c = s; c < colon; c++) buf_chr(&name, (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c));
    val *had = rec_get(out, name.s);
    if (had) { buf_add(&both, had->t.s, had->t.len); buf_str(&both, ", "); }
    buf_add(&both, v, (size_t)(end - v));
    rec_set(a, out, name.s, v_text(a, both.s, both.n));
    buf_free(&name);
    buf_free(&both);
  }
  return out;
}

// What curl hands over as it reads. The headers start again at each answer's status line, so only the last one's stay.
static size_t got_text(char *s, size_t size, size_t n, void *to) {
  buf *text = to;
  buf_add(text, s, size * n);
  return text->n > ROOM ? 0 : size * n;
}

static size_t got_header(char *s, size_t size, size_t n, void *to) {
  buf *raw = to;
  if (size * n >= 5 && !strncmp(s, "HTTP/", 5)) raw->n = 0;
  else buf_add(raw, s, size * n);
  return size * n;
}

int pl_http(const char *values, size_t n, buf *out) {
  pthread_once(&curl_once, curl_load);
  arena *a = arena_new();
  const char *problem = NULL;
  val *v = json_parse(a, values, n, &problem);
  if (problem || v->k != V_LIST || v->l.n != 5) { arena_free(a); buf_str(out, "{\"error\":\"its values weren't the five it takes\"}"); return 1; }
  val **x = v->l.v, *headers = x[2];
  buf method = { 0 }, url = { 0 }, body = { 0 }, error = { 0 }, text = { 0 }, raw = { 0 };
  buf_text(&method, x[0]), buf_text(&url, x[1]), buf_text(&body, x[3]);
  buf_chr(&url, 0), buf_chr(&method, 0);   // so neither is ever empty of bytes
  for (size_t i = 0; method.s[i]; i++) if (method.s[i] >= 'a' && method.s[i] <= 'z') method.s[i] -= 32;
  if (strchr(url.s, '#')) *strchr(url.s, '#') = 0;   // the part after # is the page's own, never sent
  long ms = x[4]->k == V_NUM && x[4]->n > 0 && x[4]->n < 3600 ? (long)(x[4]->n * 1000) : 30000, status = 0, yes = 1, most = 10;
  // Expect: left empty, so curl never waits a second for a go ahead before sending a body.
  struct curl_slist *head = curl.ok ? curl.slist_append(NULL, "Expect:") : NULL;
  if (!curl.ok) buf_str(&error, NO_CURL);
  else if (!*method.s || strspn(method.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(method.s)) buf_str(&error, "the method is a word, like GET or POST");
  for (size_t i = 0; headers->k == V_REC && i < headers->r.n && !error.n; i++) {
    buf one = { 0 }, line = { 0 };
    buf_text(&one, headers->r.v[i]);
    if (strpbrk(headers->r.key[i], "\r\n:") || strpbrk(one.s ? one.s : "", "\r\n")) buf_fmt(&error, "the header %s has a line break or a colon in it", headers->r.key[i]);
    else { buf_fmt(&line, "%s: %s", headers->r.key[i], one.s ? one.s : ""); head = curl.slist_append(head, line.s); }
    buf_free(&one);
    buf_free(&line);
  }
  CURL *c = error.n ? NULL : curl.init();
  if (!error.n && !c) buf_str(&error, "curl couldn't start");
  if (c) {
    curl.setopt(c, CURLOPT_URL, url.s);
    curl.setopt(c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(c, CURLOPT_FOLLOWLOCATION, yes);
    curl.setopt(c, CURLOPT_MAXREDIRS, most);
    curl.setopt(c, CURLOPT_TIMEOUT_MS, ms);
    curl.setopt(c, CURLOPT_USERAGENT, "onsay-core");
    curl.setopt(c, CURLOPT_HTTPHEADER, head);
    curl.setopt(c, CURLOPT_WRITEFUNCTION, got_text);
    curl.setopt(c, CURLOPT_WRITEDATA, &text);
    curl.setopt(c, CURLOPT_HEADERFUNCTION, got_header);
    curl.setopt(c, CURLOPT_HEADERDATA, &raw);
    // GET without a body is curl's own. HEAD reads no body. Anything else sends its body, even an empty one.
    if (!strcmp(method.s, "HEAD")) curl.setopt(c, CURLOPT_NOBODY, yes);
    else if (strcmp(method.s, "GET") || body.n) {
      curl.setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.n);
      curl.setopt(c, CURLOPT_POSTFIELDS, body.s ? body.s : "");
      if (strcmp(method.s, "POST")) curl.setopt(c, CURLOPT_CUSTOMREQUEST, method.s);
    }
    int e = curl.perform(c);
    curl.getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (text.n > ROOM) buf_fmt(&error, "%s answered with more than 4 MB", url.s);
    else if (e) {
      buf where = { 0 };
      buf_fmt(&where, "%s couldn't be reached", url.s);
      why(&error, where.s, e);
      buf_free(&where);
    } else if (!utf8_ok(text.s ? text.s : "", text.n)) buf_fmt(&error, "%s answered with something that isn't text, like a picture or a file", url.s);
    curl.cleanup(c);
  }
  if (error.n) { buf_str(out, "{\"error\":"); buf_json_str(out, error.s, error.n); }
  else {
    buf_fmt(out, "{\"value\":{\"status\":%ld,\"headers\":", status);
    buf_chr(&raw, 0);
    buf_json(out, header_record(a, raw.s));
    buf_str(out, ",\"text\":");
    buf_json_str(out, text.s ? text.s : "", text.n);
    buf_chr(out, '}');
  }
  buf_chr(out, '}');
  if (head) curl.slist_free_all(head);
  buf_free(&method), buf_free(&url), buf_free(&body), buf_free(&error), buf_free(&text), buf_free(&raw);
  arena_free(a);
  return 1;
}
