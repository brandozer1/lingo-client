// platform_win.c - platform.h for Windows, on WinHTTP, so proxies and certificates work as for any Windows program.
//
// 1. Text      UTF-8 to and from UTF-16, shared with Windows lingos
// 2. Time and threads
// 3. Links     connect, send, receive, close and free
// 4. Files and device, and loading a native lingo
// 5. Web       http.send, one request through WinHTTP
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

////////// 1. TEXT //////////

wchar_t *win_wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = malloc((size_t)(n ? n : 1) * sizeof *w);
  if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
  return w;
}

void win_narrow(buf *out, const wchar_t *w, int len) {
  if (len < 0) len = (int)wcslen(w);
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
  char *s = malloc((size_t)n + 1);
  WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
  buf_add(out, s, (size_t)n);
  free(s);
}

////////// 2. TIME AND THREADS //////////

double pl_now(void) {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (double)(t - 116444736000000000ULL) / 1e7;
}

void pl_sleep(double seconds) { Sleep(seconds > 0 ? (DWORD)(seconds * 1000) : 0); }

int pl_stopped(void) { return 0; }   // host.c stops a job directly, from its receiver thread

typedef struct { void (*fn)(void *); void *arg; } start;

static DWORD WINAPI begin(LPVOID p) {
  start s = *(start *)p;
  free(p);
  s.fn(s.arg);
  return 0;
}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  HANDLE h = CreateThread(NULL, 0, begin, s, 0, NULL);
  if (h) CloseHandle(h);
}

struct pl_gate { CRITICAL_SECTION lock; CONDITION_VARIABLE wake; };

pl_gate *pl_gate_new(void) {
  pl_gate *g = malloc(sizeof *g);
  InitializeCriticalSection(&g->lock);
  InitializeConditionVariable(&g->wake);
  return g;
}
void pl_enter(pl_gate *g) { EnterCriticalSection(&g->lock); }
void pl_leave(pl_gate *g) { LeaveCriticalSection(&g->lock); }
void pl_wait(pl_gate *g, double seconds) { SleepConditionVariableCS(&g->wake, &g->lock, (DWORD)(seconds * 1000)); }
void pl_wake(pl_gate *g) { WakeAllConditionVariable(&g->wake); }

////////// 3. LINKS //////////

struct pl_link {
  HINTERNET session, connection, socket;
  CRITICAL_SECTION sending;   // WinHTTP allows one send at a time
  volatile LONG closed;
};

static void why(buf *error, const char *what, DWORD code) {
  const char *plain = code == ERROR_WINHTTP_CANNOT_CONNECT ? "nothing answered at that address"
                    : code == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "that server name wasn't found"
                    : code == ERROR_WINHTTP_TIMEOUT ? "it took too long"
                    : code == ERROR_WINHTTP_SECURE_FAILURE ? "its certificate wasn't trusted"
                    : code == ERROR_WINHTTP_UNRECOGNIZED_SCHEME || code == ERROR_WINHTTP_INVALID_URL ? "that isn't a whole web address"
                    : "Windows error";
  buf_fmt(error, "%s: %s (%lu)", what, plain, (unsigned long)code);
}

// The address cracked, and a session and connection to its host. 0 with the step that failed and why.
static DWORD open_url(const wchar_t *w, URL_COMPONENTS *parts, HINTERNET *session, HINTERNET *connection, const char **step) {
  memset(parts, 0, sizeof *parts);
  parts->dwStructSize = sizeof *parts;
  parts->dwHostNameLength = parts->dwUrlPathLength = parts->dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(w, 0, 0, parts)) return GetLastError();
  wchar_t host[256] = { 0 };
  wcsncpy(host, parts->lpszHostName, parts->dwHostNameLength < 255 ? parts->dwHostNameLength : 255);
  *step = "Connecting";
  *session = WinHttpOpen(L"onsay-core", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!*session) return GetLastError();
  WinHttpSetTimeouts(*session, 10000, 10000, 30000, 30000);
  *connection = WinHttpConnect(*session, host, parts->nPort, 0);
  return *connection ? 0 : GetLastError();
}

pl_link *pl_connect(const char *url, buf *error) {
  // WinHTTP reads the address as http or https, then upgrades the connection to a WebSocket.
  buf http = { 0 };
  if (!strncmp(url, "wss://", 6)) { buf_str(&http, "https://"); buf_str(&http, url + 6); }
  else if (!strncmp(url, "ws://", 5)) { buf_str(&http, "http://"); buf_str(&http, url + 5); }
  else buf_str(&http, url);
  wchar_t *w = win_wide(http.s);
  buf_free(&http);

  pl_link *l = calloc(1, sizeof *l);
  InitializeCriticalSection(&l->sending);
  URL_COMPONENTS parts;
  HINTERNET request = NULL;
  const char *step = "The server address";
  DWORD failed = open_url(w, &parts, &l->session, &l->connection, &step);
  if (failed) goto done;
  // The path runs to the end of the address, so its query, like ?room=1, stays on it.
  request = WinHttpOpenRequest(l->connection, L"GET", parts.dwUrlPathLength ? parts.lpszUrlPath : L"/", NULL,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
      !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, NULL)) { failed = GetLastError(); goto done; }

  DWORD status = 0, size = sizeof status;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 101) { buf_fmt(error, "The server answered %lu instead of opening a connection", (unsigned long)status); goto done; }
  l->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
  if (!l->socket) { failed = GetLastError(); step = "Opening the connection"; goto done; }
  DWORD forever = 0;
  WinHttpSetOption(l->socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof forever);   // the heartbeat watches it instead

done:
  if (failed) why(error, step, failed);
  if (request) WinHttpCloseHandle(request);
  free(w);
  if (!l->socket) { pl_free(l); return NULL; }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) {
  if (l->closed) return 0;
  EnterCriticalSection(&l->sending);
  DWORD e = l->closed ? 1 : WinHttpWebSocketSend(l->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text, (DWORD)n);
  LeaveCriticalSection(&l->sending);
  return e == 0;
}

int pl_receive(pl_link *l, buf *out) {
  char chunk[16384];
  for (;;) {
    if (l->closed) return 0;
    DWORD got = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    if (WinHttpWebSocketReceive(l->socket, chunk, sizeof chunk, &got, &type)) return 0;
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return 0;
    buf_add(out, chunk, got);
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return 1;
  }
}

// Closing the socket's handle ends a receive waiting on it at once, which is how the heartbeat drops a dead link.
void pl_close(pl_link *l) {
  if (InterlockedExchange(&l->closed, 1)) return;
  if (l->socket) WinHttpCloseHandle(l->socket);
}

void pl_free(pl_link *l) {
  pl_close(l);
  if (l->connection) WinHttpCloseHandle(l->connection);
  if (l->session) WinHttpCloseHandle(l->session);
  DeleteCriticalSection(&l->sending);
  free(l);
}

////////// 4. FILES AND DEVICE //////////

int pl_read(const char *path, buf *out) {
  wchar_t *w = win_wide(path);
  FILE *f = _wfopen(w, L"rb");
  free(w);
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
  wchar_t *wt = win_wide(temp.s), *w = win_wide(path);
  buf_free(&temp);
  FILE *f = _wfopen(wt, L"wb");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  ok = ok && MoveFileExW(wt, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(wt);
  free(wt);
  free(w);
  return ok;
}

int pl_delete(const char *path) {
  wchar_t *w = win_wide(path);
  int ok = DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND;
  free(w);
  return ok;
}

int pl_folder(const char *path) {
  wchar_t *w = win_wide(path);
  for (wchar_t *p = w; *p; p++) {
    if ((*p == L'/' || *p == L'\\') && p > w && p[-1] != L':') {
      wchar_t c = *p;
      *p = 0;
      CreateDirectoryW(w, NULL);
      *p = c;
    }
  }
  int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
  free(w);
  return ok;
}

void pl_list(const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
  buf pattern = { 0 };
  buf_fmt(&pattern, "%s/*", folder);
  wchar_t *w = win_wide(pattern.s);
  buf_free(&pattern);
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileW(w, &found);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
    buf name = { 0 };
    win_narrow(&name, found.cFileName, -1);
    unsigned long long t = ((unsigned long long)found.ftLastWriteTime.dwHighDateTime << 32) | found.ftLastWriteTime.dwLowDateTime;
    double size = (double)(((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow);
    each(name.s, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, size, (double)(t - 116444736000000000ULL) / 1e7, arg);
    buf_free(&name);
  } while (FindNextFileW(h, &found));
  FindClose(h);
}

// One text field of the context, left out when empty.
static void context_field(buf *out, const char *name, const wchar_t *w) {
  if (!w[0]) return;
  buf t = { 0 };
  win_narrow(&t, w, -1);
  buf_fmt(out, ",\"%s\":", name);
  buf_json_str(out, t.s, t.n);
  buf_free(&t);
}

// The app in front is its program's file name, like chrome, as window.list names it.
void pl_context(buf *out) {
  SYSTEMTIME t;
  TIME_ZONE_INFORMATION z;
  GetLocalTime(&t);
  DWORD kind = GetTimeZoneInformation(&z);
  long offset = -(z.Bias + (kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : z.StandardBias)), at = labs(offset);
  buf_fmt(out, "{\"os\":\"windows\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
    offset < 0 ? '-' : '+', at / 60, at % 60);
  context_field(out, "zone", kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightName : z.StandardName);
  wchar_t language[LOCALE_NAME_MAX_LENGTH] = L"", app[MAX_PATH] = L"", title[512] = L"";
  GetUserDefaultLocaleName(language, LOCALE_NAME_MAX_LENGTH);
  context_field(out, "language", language);
  HWND h = GetForegroundWindow();
  DWORD pid = 0, n = MAX_PATH;
  if (h) GetWindowThreadProcessId(h, &pid);
  HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
  if (p && QueryFullProcessImageNameW(p, 0, app, &n)) {
    wchar_t *slash = wcsrchr(app, L'\\'), *dot;
    if (slash) memmove(app, slash + 1, (wcslen(slash + 1) + 1) * sizeof *app);
    if ((dot = wcsrchr(app, L'.'))) *dot = 0;
  }
  if (p) CloseHandle(p);
  if (h) GetWindowTextW(h, title, 512);
  context_field(out, "app", app);
  context_field(out, "window", title);
  buf_chr(out, '}');
}

// The Windows app runs the core as its own program, so its lingos are native libraries and it has no host lingos.
int pl_hosts(void) { return 0; }
int pl_call(const char *id, const char *values, size_t n, buf *out) { (void)id, (void)values, (void)n, (void)out; return 0; }

// Its own folder is searched first for anything it needs in turn. Never freed: its phrases are in the table from now on.
lingo_entry pl_library(const char *path, buf *error) {
  wchar_t *w = win_wide(path);
  HMODULE m = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  free(w);
  if (!m) {
    wchar_t why[256] = L"";
    DWORD code = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, why, 256, NULL);
    for (size_t n = wcslen(why); n && (why[n - 1] == L'\r' || why[n - 1] == L'\n' || why[n - 1] == L'.'); n--) why[n - 1] = 0;
    buf_fmt(error, "Windows can't load it (%lu): ", code);
    win_narrow(error, why, -1);
    return NULL;
  }
  lingo_entry entry = (lingo_entry)(void (*)(void))GetProcAddress(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

////////// 5. WEB //////////

#define ROOM (4u << 20)   // the most an answer may be

// WinHTTP looks at its own timeouts only every 4 seconds or so, so a 1 second one ends after 4. A timer closes the
// request when its time is up instead, which ends whatever call it is waiting in. closed: whoever closes it first.
typedef struct { HINTERNET q; volatile LONG closed; } watch;

static void CALLBACK too_long(PVOID arg, BOOLEAN fired) {
  watch *w = arg;
  (void)fired;
  if (!InterlockedExchange(&w->closed, 1)) WinHttpCloseHandle(w->q);
}

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

int pl_http(const char *values, size_t n, buf *out) {
  arena *a = arena_new();
  const char *problem = NULL;
  val *v = json_parse(a, values, n, &problem);
  if (problem || v->k != V_LIST || v->l.n != 5) { arena_free(a); buf_str(out, "{\"error\":\"its values weren't the five it takes\"}"); return 1; }
  val **x = v->l.v, *headers = x[2];
  buf method = { 0 }, url = { 0 }, body = { 0 }, head = { 0 }, error = { 0 }, text = { 0 }, raw = { 0 };
  buf_text(&method, x[0]), buf_text(&url, x[1]), buf_text(&body, x[3]);
  buf_chr(&url, 0), buf_chr(&method, 0);   // so neither is ever empty of bytes
  for (size_t i = 0; method.s[i]; i++) if (method.s[i] >= 'a' && method.s[i] <= 'z') method.s[i] -= 32;
  if (strchr(url.s, '#')) *strchr(url.s, '#') = 0;   // the part after # is the page's own, never sent
  DWORD ms = x[4]->k == V_NUM && x[4]->n > 0 && x[4]->n < 3600 ? (DWORD)(x[4]->n * 1000) : 30000, status = 0, size = sizeof status, failed = 0;
  if (!*method.s || strspn(method.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(method.s)) buf_str(&error, "the method is a word, like GET or POST");
  for (size_t i = 0; headers->k == V_REC && i < headers->r.n && !error.n; i++) {
    buf one = { 0 };
    buf_text(&one, headers->r.v[i]);
    if (strpbrk(headers->r.key[i], "\r\n:") || strpbrk(one.s ? one.s : "", "\r\n")) buf_fmt(&error, "the header %s has a line break or a colon in it", headers->r.key[i]);
    else buf_fmt(&head, "%s: %s\r\n", headers->r.key[i], one.s ? one.s : "");
    buf_free(&one);
  }
  wchar_t *w = win_wide(url.s), *mw = win_wide(method.s), *hw = head.n ? win_wide(head.s) : NULL;
  URL_COMPONENTS parts;
  HINTERNET s = NULL, c = NULL, q = NULL;
  HANDLE timer = NULL;
  watch late = { NULL, 0 };
  const char *step = "";
  if (!error.n && !(failed = open_url(w, &parts, &s, &c, &step))) {
    q = late.q = WinHttpOpenRequest(c, mw, parts.dwUrlPathLength ? parts.lpszUrlPath : L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (q) CreateTimerQueueTimer(&timer, NULL, too_long, &late, ms, 0, WT_EXECUTEONLYONCE);
    if (!q || !WinHttpSetTimeouts(q, (int)ms, (int)ms, (int)ms, (int)ms) || !WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms) ||
        !WinHttpSendRequest(q, hw ? hw : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L, body.s, (DWORD)body.n, (DWORD)body.n, 0) || !WinHttpReceiveResponse(q, NULL)) failed = GetLastError();
  }
  if (q && !failed) {
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t *lines = calloc(size / sizeof *lines + 1, sizeof *lines);
    if (WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, lines, &size, WINHTTP_NO_HEADER_INDEX)) win_narrow(&raw, lines, -1);
    free(lines);
    char chunk[16384];
    for (DWORD got = 1; got && text.n <= ROOM;) {
      if (!WinHttpReadData(q, chunk, sizeof chunk, &got)) { failed = GetLastError(); break; }
      buf_add(&text, chunk, got);
    }
  }
  if (timer) DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);   // waits for it, if it is closing the request now
  if (late.closed) failed = ERROR_WINHTTP_TIMEOUT;
  else if (q) WinHttpCloseHandle(q);
  if (failed) {
    buf where = { 0 };
    buf_fmt(&where, "%s couldn't be reached", url.s);
    why(&error, where.s, failed);
    buf_free(&where);
  } else if (!error.n && text.n > ROOM) buf_fmt(&error, "%s answered with more than 4 MB", url.s);
  else if (!error.n && !utf8_ok(text.s ? text.s : "", text.n)) buf_fmt(&error, "%s answered with something that isn't text, like a picture or a file", url.s);
  if (error.n) { buf_str(out, "{\"error\":"); buf_json_str(out, error.s, error.n); }
  else {
    buf_fmt(out, "{\"value\":{\"status\":%lu,\"headers\":", (unsigned long)status);
    buf_json(out, header_record(a, raw.s ? raw.s : ""));
    buf_str(out, ",\"text\":");
    buf_json_str(out, text.s ? text.s : "", text.n);
    buf_chr(out, '}');
  }
  buf_chr(out, '}');
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  free(w), free(mw), free(hw);
  buf_free(&method), buf_free(&url), buf_free(&body), buf_free(&head), buf_free(&error), buf_free(&text), buf_free(&raw);
  arena_free(a);
  return 1;
}

