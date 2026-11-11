// hardware.c - the hardware calls every lingo client built here stands on, pl_* in lingo.h: time and threads, links,
// files, the device, the web and connections, on Windows and on Linux and Android. Included whole by floor.c and lingo.c.
//
// 11. Windows    on Windows
// 12. Linux      on Linux and Android

//////////////////// 11. WINDOWS ////////////////////

#ifdef _WIN32

// Platform.h for Windows, on WinHTTP, so proxies and certificates work as for any Windows program.
//
// 1. Text      UTF-8 to and from UTF-16, shared with windows.c
// 2. Time and threads
// 3. Links     connect, send, receive, close and free
// 4. Files and device, and loading a native lingo
// 5. Web       http.send, one request through WinHTTP
// 6. Connections  tcp and udp, out and in, through Winsock: the port's web requests too
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// The precise clock where Windows has it, 8 and later, else the ordinary one, so it starts on Windows 7 too.
double pl_now(void) {
  typedef VOID(WINAPI * clock_fn)(LPFILETIME);
  static clock_fn precise;
  static int looked;
  FILETIME ft;
  if (!looked) {
    precise = (clock_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime");
    looked = 1;
  }
  if (precise)
    precise(&ft);
  else
    GetSystemTimeAsFileTime(&ft);
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (double)(t - 116444736000000000ULL) / 1e7;
}

void pl_sleep(double seconds) { Sleep(seconds > 0 ? (DWORD)(seconds * 1000) : 0); }

typedef struct {
  void (*fn)(void *);
  void *arg;
} start;

// Each thread keeps back stack to handle its own stack running out (pl_faults).
static DWORD WINAPI begin(LPVOID p) {
  start s = *(start *)p;
  ULONG room = 64 << 10;
  free(p);
  SetThreadStackGuarantee(&room);
  s.fn(s.arg);
  return 0;
}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  HANDLE h = CreateThread(NULL, 0, begin, s, 0, NULL);
  if (h) CloseHandle(h);
}

static void *(*fault_active)(void);
static void (*fault_recover)(const char *what);
static _Thread_local const char *fault_what;
static _Thread_local int fault_overflow;

// Where a crash continues, as though the instruction that crashed had called it, so the jump out of it unwinds as usual.
__attribute__((force_align_arg_pointer)) static void fault_landing(void) { fault_recover(fault_what); }

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
  const char *what = NULL;
  switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: what = "a bad memory address"; break;
    case EXCEPTION_IN_PAGE_ERROR: what = "memory it couldn't read"; break;
    case EXCEPTION_STACK_OVERFLOW: what = "the stack running out"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: what = "a bad instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "dividing by zero"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: what = "misaligned memory"; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!fault_active || !fault_active()) return EXCEPTION_CONTINUE_SEARCH;
  fault_what = what;
  fault_overflow = e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
  CONTEXT *c = e->ContextRecord;
#ifdef _WIN64
  c->Rsp -= 8;
  *(DWORD64 *)c->Rsp = c->Rip;
  c->Rip = (DWORD64)fault_landing;
#else
  c->Esp -= 4;
  *(DWORD *)c->Esp = c->Eip;
  c->Eip = (DWORD)fault_landing;
#endif
  return EXCEPTION_CONTINUE_EXECUTION;
}

void pl_faults(void *(*active)(void), void (*recover)(const char *what)) {
  ULONG room = 64 << 10;   // what the stack keeps back to handle running out
  fault_active = active;
  fault_recover = recover;
  SetThreadStackGuarantee(&room);
  SetUnhandledExceptionFilter(on_fault);
}

void pl_fault_over(void) {
  if (fault_overflow) _resetstkoflw();
  fault_overflow = 0;
}

struct pl_gate {
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
};

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
  const char *plain = "Windows error";
  if (code == ERROR_WINHTTP_CANNOT_CONNECT) plain = "nothing answered at that address";
  if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) plain = "that server name wasn't found";
  if (code == ERROR_WINHTTP_TIMEOUT) plain = "it took too long";
  if (code == ERROR_WINHTTP_SECURE_FAILURE) plain = "its certificate wasn't trusted";
  if (code == ERROR_WINHTTP_UNRECOGNIZED_SCHEME || code == ERROR_WINHTTP_INVALID_URL) {
    plain = "that isn't a whole web address";
  }
  buf_fmt(error, "%s: %s (%lu)", what, plain, (unsigned long)code);
}

// The address cracked, and a session and connection to its host. 0 with the step that failed and why.
static DWORD open_url(
    const wchar_t *w, URL_COMPONENTS *parts, HINTERNET *session, HINTERNET *connection, const char **step) {
  memset(parts, 0, sizeof *parts);
  parts->dwStructSize = sizeof *parts;
  parts->dwHostNameLength = parts->dwUrlPathLength = parts->dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(w, 0, 0, parts)) return GetLastError();
  wchar_t host[256] = { 0 };
  wcsncpy(host, parts->lpszHostName, parts->dwHostNameLength < 255 ? parts->dwHostNameLength : 255);
  *step = "Connecting";
  // The proxy as Windows finds it, from 8.1 on; before that, Windows 7's, the one its settings name.
  *session = WinHttpOpen(
      L"onsay-core", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!*session)
    *session = WinHttpOpen(
        L"onsay-core", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!*session) return GetLastError();
  // TLS 1.2 and 1.3 where Windows has them, since Windows 7 leaves 1.2 off unless asked; refused when it has neither.
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000;   // 0x2000: TLS 1.3, in newer headers
  if (!WinHttpSetOption(*session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(*session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
  }
  WinHttpSetTimeouts(*session, 10000, 10000, 30000, 30000);
  *connection = WinHttpConnect(*session, host, parts->nPort, 0);
  return *connection ? 0 : GetLastError();
}

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  // WinHTTP reads the address as http or https, then upgrades the connection to a WebSocket.
  buf http = { 0 };
  if (!strncmp(url, "wss://", 6)) {
    buf_str(&http, "https://");
    buf_str(&http, url + 6);
  } else if (!strncmp(url, "ws://", 5)) {
    buf_str(&http, "http://");
    buf_str(&http, url + 5);
  } else
    buf_str(&http, url);
  wchar_t *w = win_wide(http.s);
  wchar_t *extra = headers && *headers ? win_wide(headers) : NULL;
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
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
      !WinHttpSendRequest(request, extra ? extra : WINHTTP_NO_ADDITIONAL_HEADERS, extra ? (DWORD)-1L : 0,
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, NULL)) {
    failed = GetLastError();
    goto done;
  }

  DWORD status = 0;
  DWORD size = sizeof status;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
      &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 101) {
    buf_fmt(error, "The server answered %lu instead of opening a connection", (unsigned long)status);
    goto done;
  }
  l->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
  if (!l->socket) {
    failed = GetLastError();
    step = "Opening the connection";
    goto done;
  }
  DWORD forever = 0;
  WinHttpSetOption(
      l->socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof forever);   // the heartbeat watches it instead

done:
  if (failed) why(error, step, failed);
  if (request) WinHttpCloseHandle(request);
  free(w);
  free(extra);
  if (!l->socket) {
    pl_free(l);
    return NULL;
  }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) {
  if (l->closed) return 0;
  EnterCriticalSection(&l->sending);
  DWORD e = l->closed
      ? 1
      : WinHttpWebSocketSend(l->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text, (DWORD)n);
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
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
      return 1;
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
  wchar_t *wt = win_wide(temp.s);
  wchar_t *w = win_wide(path);
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

int pl_append(const char *path, const char *data, size_t n) {
  wchar_t *w = win_wide(path);
  FILE *f = _wfopen(w, L"ab");
  free(w);
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  return ok;
}

int pl_delete(const char *path) {
  wchar_t *w = win_wide(path);
  int ok = DeleteFileW(w) || RemoveDirectoryW(w) || GetLastError() == ERROR_FILE_NOT_FOUND;
  free(w);
  return ok;
}

void pl_whole(const char *path, buf *out) {
  wchar_t *w = win_wide(path);
  wchar_t full[MAX_PATH * 4];
  if (GetFullPathNameW(w, sizeof full / sizeof *full, full, NULL))
    win_narrow(out, full, -1);
  else
    buf_str(out, path);
  free(w);
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

void pl_list(
    const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
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
    unsigned long long t = ((unsigned long long)found.ftLastWriteTime.dwHighDateTime << 32) |
        found.ftLastWriteTime.dwLowDateTime;
    double size = (double)(((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow);
    each(name.s, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, size,
        (double)(t - 116444736000000000ULL) / 1e7, arg);
    buf_free(&name);
  } while (FindNextFileW(h, &found));
  FindClose(h);
}

// Sealed by DPAPI to this Windows user: the file holds only what Windows can open again, for that user on this computer.
int pl_seal(const char *path, const void *data, size_t n) {
  DATA_BLOB in = { (DWORD)n, (BYTE *)data };
  DATA_BLOB out = { 0 };
  if (!CryptProtectData(&in, L"lingo client key", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
  int ok = pl_write(path, (const char *)out.pbData, out.cbData);
  LocalFree(out.pbData);
  return ok;
}

int pl_unseal(const char *path, buf *out) {
  buf sealed = { 0 };
  DATA_BLOB in = { 0 };
  DATA_BLOB open = { 0 };
  int ok = pl_read(path, &sealed);
  in.cbData = (DWORD)sealed.n;
  in.pbData = (BYTE *)sealed.s;
  ok = ok && CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &open);
  if (ok) {
    buf_add(out, (const char *)open.pbData, open.cbData);
    SecureZeroMemory(open.pbData, open.cbData);
    LocalFree(open.pbData);
  }
  buf_free(&sealed);
  return ok;
}

void pl_random(void *to, size_t n) { BCryptGenRandom(NULL, to, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

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
  long offset = -(z.Bias + (kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : z.StandardBias));
  long at = labs(offset);
  buf_fmt(out, "{\"os\":\"windows\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.wYear, t.wMonth, t.wDay,
      t.wHour, t.wMinute, t.wSecond, offset < 0 ? '-' : '+', at / 60, at % 60);
  context_field(out, "zone", kind == TIME_ZONE_ID_DAYLIGHT ? z.DaylightName : z.StandardName);
  wchar_t language[LOCALE_NAME_MAX_LENGTH] = L"";
  wchar_t app[MAX_PATH] = L"";
  wchar_t title[512] = L"";
  GetUserDefaultLocaleName(language, LOCALE_NAME_MAX_LENGTH);
  context_field(out, "language", language);
  HWND h = GetForegroundWindow();
  DWORD pid = 0;
  DWORD n = MAX_PATH;
  if (h) GetWindowThreadProcessId(h, &pid);
  HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
  if (p && QueryFullProcessImageNameW(p, 0, app, &n)) {
    wchar_t *slash = wcsrchr(app, L'\\');
    wchar_t *dot;
    if (slash) memmove(app, slash + 1, (wcslen(slash + 1) + 1) * sizeof *app);
    if ((dot = wcsrchr(app, L'.'))) *dot = 0;
  }
  if (p) CloseHandle(p);
  if (h) GetWindowTextW(h, title, 512);
  context_field(out, "app", app);
  context_field(out, "window", title);
  buf_chr(out, '}');
}

// Its own folder is searched first for anything it needs in turn. Never freed: its phrases are in the table from now on.
void *pl_symbol(const char *library, const char *function) {
  HMODULE m = library[0] ? LoadLibraryA(library) : GetModuleHandleA(NULL);
  return m ? (void *)(void (*)(void))GetProcAddress(m, function) : NULL;
}

lingo_entry pl_library(const char *path, buf *error) {
  wchar_t *w = win_wide(path);
  HMODULE m = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  free(w);
  if (!m) {
    wchar_t why[256] = L"";
    DWORD code = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, why, 256, NULL);
    for (size_t n = wcslen(why); n && (why[n - 1] == L'\r' || why[n - 1] == L'\n' || why[n - 1] == L'.'); n--)
      why[n - 1] = 0;
    buf_fmt(error, "Windows can't load it (%lu): ", code);
    win_narrow(error, why, -1);
    return NULL;
  }
  lingo_entry entry = (lingo_entry)(void (*)(void))GetProcAddress(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

////////// 5. WEB //////////

#define ANSWER_ROOM (4u << 20)   // the most an answer may be

// WinHTTP looks at its own timeouts only every 4 seconds or so, so a 1 second one ends after 4. A timer closes the
// request when its time is up instead, which ends whatever call it is waiting in. closed: whoever closes it first.
typedef struct {
  HINTERNET q;
  volatile LONG closed;
} win_watch;

static void CALLBACK too_long(PVOID arg, BOOLEAN fired) {
  win_watch *w = arg;
  (void)fired;
  if (!InterlockedExchange(&w->closed, 1)) WinHttpCloseHandle(w->q);
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  buf head = { 0 };
  buf text = { 0 };
  buf raw = { 0 };
  for (size_t i = 0; i < headers->r.n; i++) buf_fmt(&head, "%s: %s\r\n", headers->r.key[i], headers->r.v[i]->t.s);
  DWORD ms = (DWORD)(seconds * 1000);
  DWORD status = 0;
  DWORD size = sizeof status;
  DWORD failed = 0;
  wchar_t *w = win_wide(url);
  wchar_t *mw = win_wide(method);
  wchar_t *hw = head.n ? win_wide(head.s) : NULL;
  URL_COMPONENTS parts;
  HINTERNET s = NULL;
  HINTERNET c = NULL;
  HINTERNET q = NULL;
  HANDLE timer = NULL;
  win_watch late = { NULL, 0 };
  const char *step = "";
  if (!(failed = open_url(w, &parts, &s, &c, &step))) {
    q = late.q = WinHttpOpenRequest(c, mw, parts.dwUrlPathLength ? parts.lpszUrlPath : L"/", NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (q) CreateTimerQueueTimer(&timer, NULL, too_long, &late, ms, 0, WT_EXECUTEONLYONCE);
    if (!q || !WinHttpSetTimeouts(q, (int)ms, (int)ms, (int)ms, (int)ms) ||
        !WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms) ||
        !WinHttpSendRequest(
            q, hw ? hw : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L, (LPVOID)body, (DWORD)n, (DWORD)n, 0) ||
        !WinHttpReceiveResponse(q, NULL))
      failed = GetLastError();
  }
  if (q && !failed) {
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
        &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(
        q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t *lines = calloc(size / sizeof *lines + 1, sizeof *lines);
    if (WinHttpQueryHeaders(
            q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, lines, &size, WINHTTP_NO_HEADER_INDEX))
      win_narrow(&raw, lines, -1);
    free(lines);
    char chunk[16384];
    for (DWORD got = 1; got && text.n <= ANSWER_ROOM;) {
      if (!WinHttpReadData(q, chunk, sizeof chunk, &got)) {
        failed = GetLastError();
        break;
      }
      buf_add(&text, chunk, got);
    }
  }
  if (timer)
    DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);   // waits for it, if it is closing the request now
  if (late.closed)
    failed = ERROR_WINHTTP_TIMEOUT;
  else if (q)
    WinHttpCloseHandle(q);
  val *out = NULL;
  if (failed) {
    buf where = { 0 };
    buf_fmt(&where, "%s couldn't be reached", url);
    why(error, where.s, failed);
    buf_free(&where);
  } else if (text.n > ANSWER_ROOM)
    buf_fmt(error, "%s answered with more than 4 MB", url);
  else {
    int words = utf8_ok(text.s ? text.s : "", text.n);
    out = v_rec(a);
    rec_set(a, out, "status", v_num(a, status));
    rec_set(a, out, "headers", header_lines(a, raw.s ? raw.s : ""));
    if (words) rec_set(a, out, "text", v_text(a, text.s ? text.s : "", text.n));
    else rec_set(a, out, "bytes", v_bytes(a, text.s, text.n));
  }
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  free(w);
  free(mw);
  free(hw);
  buf_free(&head);
  buf_free(&text);
  buf_free(&raw);
  return out;
}

////////// 6. CONNECTIONS //////////

struct pl_conn {
  SOCKET s;
  int udp;
};

// A connection kept: nothing it sends may wait more than 10 seconds to be taken.
static pl_conn *conn(SOCKET s, int udp) {
  pl_conn *c = malloc(sizeof *c);
  DWORD ms = 10000;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&ms, sizeof ms);
  c->s = s;
  c->udp = udp;
  return c;
}

// Every address the name has is tried in turn, IPv6 and IPv4, until one answers.
pl_conn *pl_dial(const char *kind, const char *host, int port, buf *error) {
  struct addrinfo hints = { 0 };
  struct addrinfo *found = NULL;
  char service[16];
  int udp = !strcmp(kind, "udp");
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  snprintf(service, sizeof service, "%d", port);
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  if (getaddrinfo(host, service, &hints, &found)) {
    buf_fmt(error, "%s couldn't be found", host);
    return NULL;
  }
  SOCKET s = INVALID_SOCKET;
  for (struct addrinfo *at = found; at && s == INVALID_SOCKET; at = at->ai_next) {
    s = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (s != INVALID_SOCKET && connect(s, at->ai_addr, (int)at->ai_addrlen)) {
      closesocket(s);
      s = INVALID_SOCKET;
    }
  }
  freeaddrinfo(found);
  if (s == INVALID_SOCKET) {
    buf_fmt(error, "%s didn't answer on port %d", host, port);
    return NULL;
  }
  return conn(s, udp);
}

// Everywhere: IPv6 and IPv4 on one socket, or IPv4 alone on a computer without IPv6. Else 127.0.0.1 alone, which
// Windows' firewall never asks about.
pl_conn *pl_bind(const char *kind, int port, int everywhere, buf *error) {
  WSADATA w;
  struct sockaddr_in four = { 0 };
  struct sockaddr_in6 six = { 0 };   // its address all zeros: every one, IPv4 too once V6ONLY is off
  DWORD no = 0;
  DWORD yes = 1;
  if (strcmp(kind, "tcp")) {
    buf_fmt(error, "only tcp connections can come in so far");
    return NULL;
  }
  four.sin_family = AF_INET;
  four.sin_port = htons((u_short)port);
  four.sin_addr.s_addr = htonl(everywhere ? INADDR_ANY : INADDR_LOOPBACK);
  six.sin6_family = AF_INET6;
  six.sin6_port = htons((u_short)port);
  SOCKET s = WSAStartup(MAKEWORD(2, 2), &w) || !everywhere ? INVALID_SOCKET
                                                           : socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  int v6 = s != INVALID_SOCKET;
  if (!v6) s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s != INVALID_SOCKET) {
    setsockopt(
        s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof yes);   // so no other program can listen on it too
    if (v6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&no, sizeof no);
  }
  if (s == INVALID_SOCKET ||
      bind(s, v6 ? (struct sockaddr *)&six : (struct sockaddr *)&four, v6 ? sizeof six : sizeof four) ||
      listen(s, SOMAXCONN)) {
    int code = WSAGetLastError();
    buf_fmt(error, "Port %d couldn't be opened: %s (%d)", port,
        code == WSAEADDRINUSE || code == WSAEACCES ? "another program is using it" : "Windows error", code);
    if (s != INVALID_SOCKET) closesocket(s);
    return NULL;
  }
  return conn(s, 0);
}

// An IPv4 address as IPv6 writes it, ::ffff:1.2.3.4, is written 1.2.3.4.
pl_conn *pl_next(pl_conn *listener, buf *from, double seconds) {
  struct sockaddr_storage at;
  struct timeval wait = { (long)seconds, (long)((seconds - (long)seconds) * 1e6) };
  fd_set ready;
  int n = sizeof at;
  FD_ZERO(&ready);
  FD_SET(listener->s, &ready);
  if (select(0, &ready, NULL, NULL, &wait) != 1) return NULL;
  SOCKET c = accept(listener->s, (struct sockaddr *)&at, &n);
  if (c == INVALID_SOCKET) return NULL;
  char text[64] = "";
  inet_ntop(at.ss_family,
      at.ss_family == AF_INET ? (void *)&((struct sockaddr_in *)&at)->sin_addr
                              : (void *)&((struct sockaddr_in6 *)&at)->sin6_addr,
      text, sizeof text);
  buf_str(from, strncmp(text, "::ffff:", 7) || !strchr(text, '.') ? text : text + 7);
  return conn(c, 0);
}

int pl_put(pl_conn *c, const char *data, size_t n) {
  if (c->udp) return send(c->s, data, (int)n, 0) == (int)n;
  for (int sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, (int)n, 0)) <= 0) return 0;
  return 1;
}

int pl_get(pl_conn *c, buf *out, double seconds) {
  char chunk[65536];
  struct timeval wait = { (long)seconds, (long)((seconds - (long)seconds) * 1e6) };
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(c->s, &ready);
  if (seconds > 0 && select(0, &ready, NULL, NULL, &wait) != 1) return 0;
  int got = recv(c->s, chunk, sizeof chunk, 0);
  if (got > 0) buf_add(out, chunk, (size_t)got);
  return got > 0;
}

void pl_shut(pl_conn *c) { shutdown(c->s, SD_BOTH); }

// What was sent goes before it closes.
void pl_drop(pl_conn *c) {
  shutdown(c->s, SD_SEND);
  closesocket(c->s);
  free(c);
}

#endif

//////////////////// 12. LINUX ////////////////////

#ifndef _WIN32

// Platform.h for Linux, headless servers first, with the web through libcurl loaded as it starts.
//
// 1. Time and threads   clock_gettime, pthreads, and the gate as a mutex and a condition
// 2. Curl               libcurl found at run time, so the core needs no curl to build and says plainly when it's missing
// 3. Links              a WebSocket written here on a connection curl opens (TCP, TLS and proxies), RFC 6455
// 4. Files and device, and loading a native lingo
// 5. Web                http.send, one request through curl
// 6. Connections        tcp and udp, out and in, on plain sockets: the port's web requests too
// 7. Android            the same file built into the Android app, where the app does links, the web and what is
//                       around (pl_device), in place of 2, 3, 5 and the context
// libcurl's own WebSocket API is left alone, since many distributions build curl without it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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

typedef struct {
  void (*fn)(void *);
  void *arg;
} start;

// Each thread has stack of its own aside, so its stack running out can be handled too (pl_faults).
static void *begin(void *p) {
  start s = *(start *)p;
  stack_t aside = { .ss_sp = malloc(64 << 10), .ss_size = 64 << 10 };
  stack_t off = { .ss_flags = SS_DISABLE };
  free(p);
  sigaltstack(&aside, NULL);
  s.fn(s.arg);
  sigaltstack(&off, NULL);
  free(aside.ss_sp);
  return NULL;
}

static void *(*fault_active)(void);
static void (*fault_recover)(const char *what);

static void on_fault(int number) {
  const char *what = number == SIGFPE ? "dividing by zero" : number == SIGILL ? "a bad instruction" : "a bad memory address";
  if (!fault_active || !fault_active()) {
#ifdef __ANDROID__
    pl_crashed(what);
#endif
    signal(number, SIG_DFL);
    return;   // the same crash again, now ending the program
  }
  fault_recover(what);
}

void pl_faults(void *(*active)(void), void (*recover)(const char *what)) {
  stack_t aside = { .ss_sp = malloc(64 << 10), .ss_size = 64 << 10 };   // so the stack running out can be handled too
  struct sigaction on = { .sa_handler = on_fault, .sa_flags = SA_ONSTACK | SA_NODEFER };
  fault_active = active;
  fault_recover = recover;
  sigaltstack(&aside, NULL);
  sigaction(SIGSEGV, &on, NULL);
  sigaction(SIGBUS, &on, NULL);
  sigaction(SIGILL, &on, NULL);
  sigaction(SIGFPE, &on, NULL);
}

void pl_fault_over(void) {}

void pl_thread(void (*fn)(void *), void *arg) {
  start *s = malloc(sizeof *s);
  *s = (start){ fn, arg };
  pthread_t t;
  if (pthread_create(&t, NULL, begin, s))
    free(s);
  else
    pthread_detach(t);
}

// Recursive, like the Windows gate, and timed on the monotonic clock, so a change of the time of day never ends a wait.
struct pl_gate {
  pthread_mutex_t lock;
  pthread_cond_t wake;
};

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

#ifndef __ANDROID__
////////// 2. CURL //////////

// The few parts of curl/curl.h the core uses, with their numbers, which curl never changes.
typedef void CURL;
struct curl_slist;
enum {
  CURLE_OK = 0,
  CURLE_UNSUPPORTED_PROTOCOL = 1,
  CURLE_URL_MALFORMAT = 3,
  CURLE_COULDNT_RESOLVE_HOST = 6,
  CURLE_COULDNT_CONNECT = 7,
  CURLE_WRITE_ERROR = 23,
  CURLE_OPERATION_TIMEDOUT = 28,
  CURLE_SSL_CONNECT_ERROR = 35,
  CURLE_TOO_MANY_REDIRECTS = 47,
  CURLE_PEER_FAILED_VERIFICATION = 60,
  CURLE_SSL_CACERT = 77,
  CURLE_AGAIN = 81,
  CURLOPT_WRITEDATA = 10001,
  CURLOPT_URL = 10002,
  CURLOPT_POSTFIELDS = 10015,
  CURLOPT_USERAGENT = 10018,
  CURLOPT_HTTPHEADER = 10023,
  CURLOPT_HEADERDATA = 10029,
  CURLOPT_CUSTOMREQUEST = 10036,
  CURLOPT_WRITEFUNCTION = 20011,
  CURLOPT_HEADERFUNCTION = 20079,
  CURLOPT_NOBODY = 44,
  CURLOPT_FOLLOWLOCATION = 52,
  CURLOPT_POSTFIELDSIZE = 60,
  CURLOPT_MAXREDIRS = 68,
  CURLOPT_NOSIGNAL = 99,
  CURLOPT_CONNECT_ONLY = 141,
  CURLOPT_TIMEOUT_MS = 155,
  CURLOPT_CONNECTTIMEOUT_MS = 156,
  CURLINFO_RESPONSE_CODE = 0x200002,
  CURLINFO_ACTIVESOCKET = 0x500000 + 44,
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

#define NO_CURL \
  "This computer has no libcurl, which the core needs to reach the web. Install it, like apt install libcurl4 (libcurl4t64 on newer Ubuntu) or dnf install libcurl"

static void why(buf *error, const char *what, int code) {
  const char *plain = curl.strerror(code);
  if (code == CURLE_COULDNT_CONNECT) plain = "nothing answered at that address";
  if (code == CURLE_COULDNT_RESOLVE_HOST) plain = "that server name wasn't found";
  if (code == CURLE_OPERATION_TIMEDOUT) plain = "it took too long";
  if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT || code == CURLE_SSL_CONNECT_ERROR) {
    plain = "its certificate wasn't trusted";
  }
  if (code == CURLE_URL_MALFORMAT || code == CURLE_UNSUPPORTED_PROTOCOL) plain = "that isn't a whole web address";
  if (code == CURLE_TOO_MANY_REDIRECTS) plain = "it sent it on too many times";
  buf_fmt(error, "%s: %s (curl %d)", what, plain, code);
}

////////// 3. LINKS //////////

struct pl_link {
  CURL *c;
  int socket;
  pthread_mutex_t io;   // around every curl_easy_send and curl_easy_recv, never held while waiting
  pthread_mutex_t sending;   // one frame at a time, whole
  volatile int closed;
  double until;   // while it opens, when to give up waiting
  buf held;   // what was read and isn't a whole frame yet
};

// Waits for the socket to be ready, a second at a time so a close is seen. 0 once the link is closed, or out of time.
static int link_ready(pl_link *l, short what) {
  struct pollfd p = { l->socket, what, 0 };
  while (!l->closed && !(l->until && pl_now() > l->until))
    if (poll(&p, 1, 1000) != 0) return !l->closed;
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
    if (e == CURLE_OK) {
      buf_add(&l->held, chunk, got);
      return got > 0;
    }
    if (e != CURLE_AGAIN || !link_ready(l, POLLIN)) return 0;
  }
}

static int write_all(pl_link *l, const char *s, size_t n) {
  while (n) {
    if (l->closed) return 0;
    size_t sent = 0;
    pthread_mutex_lock(&l->io);
    int e = curl.send(l->c, s, n, &sent);
    pthread_mutex_unlock(&l->io);
    if (e == CURLE_OK) {
      s += sent;
      n -= sent;
    } else if (e != CURLE_AGAIN || !link_ready(l, POLLOUT))
      return 0;
  }
  return 1;
}

// One frame from this side, which is always masked.
static int send_frame(pl_link *l, int op, const char *s, size_t n) {
  unsigned char mask[4];
  if (getrandom(mask, 4, 0) != 4) return 0;
  buf f = { 0 };
  buf_chr(&f, (char)(0x80 | op));
  if (n < 126)
    buf_chr(&f, (char)(0x80 | n));
  else if (n < 65536) {
    buf_chr(&f, (char)(0x80 | 126));
    buf_chr(&f, (char)(n >> 8));
    buf_chr(&f, (char)n);
  } else {
    buf_chr(&f, (char)(0x80 | 127));
    for (int i = 7; i >= 0; i--) buf_chr(&f, (char)((unsigned long long)n >> (8 * i)));
  }
  buf_add(&f, (const char *)mask, 4);
  for (size_t i = 0; i < n; i++) buf_chr(&f, (char)(s[i] ^ mask[i & 3]));
  pthread_mutex_lock(&l->sending);
  int ok = write_all(l, f.s, f.n);
  pthread_mutex_unlock(&l->sending);
  buf_free(&f);
  return ok;
}

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  pthread_once(&curl_once, curl_load);
  if (!curl.ok) {
    buf_str(error, NO_CURL);
    return NULL;
  }
  int secure = !strncmp(url, "wss://", 6);
  if (!secure && strncmp(url, "ws://", 5)) {
    buf_str(error, "The server address: that isn't a whole web address");
    return NULL;
  }
  // curl opens the connection to the address as http or https, and the request to upgrade it is written here.
  unsigned char random[16], digest[20];
  if ((size_t)getrandom(random, sizeof random, 0) != sizeof random) {
    buf_str(error, "This computer gave no random numbers");
    return NULL;
  }
  const char *host = url + (secure ? 6 : 5);
  const char *path = strchr(host, '/');
  if (!path) path = host + strlen(host);
  buf http = { 0 };
  buf ask = { 0 };
  buf key = { 0 };
  buf expect = { 0 };
  buf_fmt(&http, "%s://%.*s", secure ? "https" : "http", (int)(path - host), host);
  base64(&key, random, sizeof random);
  buf_fmt(&expect, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key.s);
  sha1((const unsigned char *)expect.s, expect.n, digest);
  expect.n = 0;
  base64(&expect, digest, sizeof digest);
  buf_fmt(&ask,
      "GET %s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\nUser-Agent: onsay-core\r\n%s\r\n",
      *path ? path : "/", (int)(path - host), host, key.s, headers ? headers : "");

  pl_link *l = calloc(1, sizeof *l);
  pthread_mutex_init(&l->io, NULL);
  pthread_mutex_init(&l->sending, NULL);
  l->socket = -1;
  l->c = curl.init();
  long yes = 1;
  long ms = 10000;
  if (!l->c)
    buf_str(error, "Connecting: curl couldn't start");
  else {
    curl.setopt(l->c, CURLOPT_URL, http.s);
    curl.setopt(l->c, CURLOPT_CONNECT_ONLY, yes);
    curl.setopt(l->c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(l->c, CURLOPT_CONNECTTIMEOUT_MS, ms);
    int e = curl.perform(l->c);
    if (e)
      why(error, "Connecting", e);
    else if (curl.getinfo(l->c, CURLINFO_ACTIVESOCKET, &l->socket) || l->socket < 0)
      buf_str(error, "Connecting: curl gave no connection");
  }

  // Its answer is read up to the blank line after its headers, within 10 seconds. Anything after it is the first frames.
  char *end = NULL;
  l->until = pl_now() + 10;
  if (!error->n && !write_all(l, ask.s, ask.n))
    buf_str(error, "Opening the connection: the request to open it couldn't be sent");
  while (!error->n && !(end = l->held.s ? strstr(l->held.s, "\r\n\r\n") : NULL)) {
    if (!read_more(l))
      buf_str(error,
          pl_now() > l->until ? "Opening the connection: it took too long"
                              : "Opening the connection: the server closed it");
    buf_chr(&l->held, 0);
    l->held.n--;   // a 0 after it, for strstr, never counted
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
    if (status != 101)
      buf_fmt(error, "The server answered %d instead of opening a connection", status);
    else if (!accepted)
      buf_str(error, "The server's answer to opening the connection wasn't right");
    memmove(l->held.s, l->held.s + head, l->held.n - head);
    l->held.n -= head;
  }
  buf_free(&http);
  buf_free(&ask);
  buf_free(&key);
  buf_free(&expect);
  if (error->n) {
    pl_free(l);
    return NULL;
  }
  return l;
}

int pl_send(pl_link *l, const char *text, size_t n) { return !l->closed && send_frame(l, 1, text, n); }

// Frames until a whole message: pieces of one joined, a ping answered, a close the end.
int pl_receive(pl_link *l, buf *out) {
  for (;;) {
    // Its length is in 7 bits, or in the 2 or 8 bytes after them, then a mask, which a server shouldn't send.
    unsigned char *h = (unsigned char *)l->held.s;
    size_t n = l->held.n >= 2 ? h[1] & 127 : 0;
    size_t size = 0;
    if (n == 126) size = 2;
    if (n == 127) size = 8;
    size_t at = 2 + size + (l->held.n >= 2 && h[1] & 0x80 ? 4 : 0);
    if (l->held.n < at) {
      if (!read_more(l)) return 0;
      continue;
    }
    if (size) {
      n = 0;
      for (size_t i = 0; i < size; i++) n = n << 8 | h[2 + i];
    }
    if (n > (1u << 30)) return 0;   // a message of more than a GB is no message at all
    if (l->held.n < at + n) {
      if (!read_more(l)) return 0;
      continue;
    }
    int op = h[0] & 15;
    int fin = h[0] & 0x80;
    char *body = l->held.s + at;
    if (h[1] & 0x80)
      for (size_t i = 0; i < n; i++) body[i] ^= (char)h[at - 4 + (i & 3)];
    if (op == 8) {
      l->closed = 1;
      return 0;
    }
    if (op == 9 && !send_frame(l, 10, body, n)) return 0;
    if (op <= 2) buf_add(out, body, n);
    memmove(l->held.s, l->held.s + at + n, l->held.n - at - n);
    l->held.n -= at + n;
    if (op <= 2 && fin) {
      if (!out->s) buf_add(out, "", 0);
      return 1;
    }
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
#endif

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

int pl_append(const char *path, const char *data, size_t n) {
  FILE *f = fopen(path, "ab");
  int ok = f && fwrite(data, 1, n, f) == n;
  if (f) ok = !fclose(f) && ok;
  return ok;
}

int pl_delete(const char *path) { return !remove(path) || errno == ENOENT; }

// A file only its user can read or write, made so from the start.
int pl_seal(const char *path, const void *data, size_t n) {
  buf temp = { 0 };
  buf_fmt(&temp, "%s.new", path);
  remove(temp.s);
  int fd = open(temp.s, O_WRONLY | O_CREAT | O_EXCL, 0600);
  int ok = fd >= 0 && write(fd, data, n) == (ssize_t)n;
  if (fd >= 0) ok = !fsync(fd) && !close(fd) && ok;
  ok = ok && !rename(temp.s, path);
  if (!ok) remove(temp.s);
  buf_free(&temp);
  return ok;
}

int pl_unseal(const char *path, buf *out) { return pl_read(path, out); }

void pl_random(void *to, size_t n) {
  for (size_t got = 0; got < n;) {
    ssize_t k = getrandom((char *)to + got, n - got, 0);
    if (k > 0) got += (size_t)k;
  }
}

void pl_whole(const char *path, buf *out) {
  char *full = realpath(path, NULL);
  buf_str(out, full ? full : path);
  free(full);
}

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

void pl_list(
    const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg) {
  DIR *d = opendir(folder);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    buf path = { 0 };
    buf_fmt(&path, "%s/%s", folder, e->d_name);
    struct stat s;
    if (!stat(path.s, &s))
      each(e->d_name, S_ISDIR(s.st_mode), (double)s.st_size, (double)s.st_mtim.tv_sec + (double)s.st_mtim.tv_nsec / 1e9,
          arg);
    buf_free(&path);
  }
  closedir(d);
}

#ifndef __ANDROID__
// A server has no app or window in front. The zone is TZ, or the system's own: /etc/timezone, or where /etc/localtime
// points, like /usr/share/zoneinfo/America/New_York. The language is LANG's, like en_US.UTF-8 as en-US.
void pl_context(buf *out) {
  time_t now = time(NULL);
  struct tm t;
  localtime_r(&now, &t);
  long offset = t.tm_gmtoff / 60;
  long at = labs(offset);
  buf_fmt(out, "{\"os\":\"linux\",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld\"", t.tm_year + 1900,
      t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, offset < 0 ? '-' : '+', at / 60, at % 60);
  char zone[256] = "";
  char link[256] = "";
  char *z = getenv("TZ");
  FILE *f = NULL;
  ssize_t n = 0;
  if (z && *z)
    snprintf(zone, sizeof zone, "%s", z + (*z == ':'));
  else if ((f = fopen("/etc/timezone", "r"))) {
    if (!fgets(zone, sizeof zone, f)) zone[0] = 0;
    fclose(f);
  }
  if (!zone[0] && (n = readlink("/etc/localtime", link, sizeof link - 1)) > 0) {
    link[n] = 0;
    if ((z = strstr(link, "zoneinfo/"))) snprintf(zone, sizeof zone, "%s", z + 9);
  }
  zone[strcspn(zone, "\r\n")] = 0;
  if (zone[0]) {
    buf_str(out, ",\"zone\":");
    buf_json_str(out, zone, strlen(zone));
  }
  char language[64] = "";
  char *lang = getenv("LANG");
  if (lang && *lang && strcmp(lang, "C") && strncmp(lang, "C.", 2) && strcmp(lang, "POSIX"))
    snprintf(language, sizeof language, "%s", lang);
  language[strcspn(language, ".@")] = 0;
  for (char *c = language; *c; c++)
    if (*c == '_') *c = '-';
  if (language[0]) {
    buf_str(out, ",\"language\":");
    buf_json_str(out, language, strlen(language));
  }
  buf_chr(out, '}');
}
#endif

// Never closed: its phrases are in the table from now on. The core is built with -rdynamic, so a library finds its words.
void *pl_symbol(const char *library, const char *function) {
  void *m = dlopen(library[0] ? library : NULL, RTLD_NOW);
  return m ? dlsym(m, function) : NULL;
}

lingo_entry pl_library(const char *path, buf *error) {
  void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!m) {
    buf_fmt(error, "Linux can't load it: %s", dlerror());
    return NULL;
  }
  lingo_entry entry;
  *(void **)&entry = dlsym(m, "lingo_phrases");
  if (!entry) buf_str(error, "it has no lingo_phrases");
  return entry;
}

#ifndef __ANDROID__
////////// 5. WEB //////////

#define ANSWER_ROOM (4u << 20)   // the most an answer may be

// What curl hands over as it reads. The headers start again at each answer's status line, so only the last one's stay.
static size_t got_text(char *s, size_t size, size_t n, void *to) {
  buf *text = to;
  buf_add(text, s, size * n);
  return text->n > ANSWER_ROOM ? 0 : size * n;
}

static size_t got_header(char *s, size_t size, size_t n, void *to) {
  buf *raw = to;
  if (size * n >= 5 && !strncmp(s, "HTTP/", 5))
    raw->n = 0;
  else
    buf_add(raw, s, size * n);
  return size * n;
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  pthread_once(&curl_once, curl_load);
  buf text = { 0 };
  buf raw = { 0 };
  long status = 0;
  long yes = 1;
  long most = 10;
  val *out = NULL;
  if (!curl.ok) {
    buf_str(error, NO_CURL);
    return NULL;
  }
  // Expect: left empty, so curl never waits a second for a go ahead before sending a body.
  struct curl_slist *head = curl.slist_append(NULL, "Expect:");
  for (size_t i = 0; i < headers->r.n; i++) {
    buf line = { 0 };
    buf_fmt(&line, "%s: %s", headers->r.key[i], headers->r.v[i]->t.s);
    head = curl.slist_append(head, line.s);
    buf_free(&line);
  }
  CURL *c = curl.init();
  if (!c)
    buf_str(error, "curl couldn't start");
  else {
    curl.setopt(c, CURLOPT_URL, url);
    curl.setopt(c, CURLOPT_NOSIGNAL, yes);
    curl.setopt(c, CURLOPT_FOLLOWLOCATION, yes);
    curl.setopt(c, CURLOPT_MAXREDIRS, most);
    curl.setopt(c, CURLOPT_TIMEOUT_MS, (long)(seconds * 1000));
    curl.setopt(c, CURLOPT_USERAGENT, "onsay-core");
    curl.setopt(c, CURLOPT_HTTPHEADER, head);
    curl.setopt(c, CURLOPT_WRITEFUNCTION, got_text);
    curl.setopt(c, CURLOPT_WRITEDATA, &text);
    curl.setopt(c, CURLOPT_HEADERFUNCTION, got_header);
    curl.setopt(c, CURLOPT_HEADERDATA, &raw);
    // GET without a body is curl's own. HEAD reads no body. Anything else sends its body, even an empty one.
    if (!strcmp(method, "HEAD"))
      curl.setopt(c, CURLOPT_NOBODY, yes);
    else if (strcmp(method, "GET") || n) {
      curl.setopt(c, CURLOPT_POSTFIELDSIZE, (long)n);
      curl.setopt(c, CURLOPT_POSTFIELDS, body);
      if (strcmp(method, "POST")) curl.setopt(c, CURLOPT_CUSTOMREQUEST, method);
    }
    int e = curl.perform(c);
    curl.getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (text.n > ANSWER_ROOM)
      buf_fmt(error, "%s answered with more than 4 MB", url);
    else if (e) {
      buf where = { 0 };
      buf_fmt(&where, "%s couldn't be reached", url);
      why(error, where.s, e);
      buf_free(&where);
    } else {
      int words = utf8_ok(text.s ? text.s : "", text.n);
      out = v_rec(a);
      rec_set(a, out, "status", v_num(a, (double)status));
      rec_set(a, out, "headers", header_lines(a, raw.s ? raw.s : ""));
      if (words) rec_set(a, out, "text", v_text(a, text.s ? text.s : "", text.n));
      else rec_set(a, out, "bytes", v_bytes(a, text.s, text.n));
    }
    curl.cleanup(c);
  }
  curl.slist_free_all(head);
  buf_free(&text);
  buf_free(&raw);
  return out;
}
#endif

////////// 6. CONNECTIONS //////////

struct pl_conn {
  int s;
  int udp;
};

// A connection kept: nothing it sends may wait more than 10 seconds to be taken.
static pl_conn *conn(int s, int udp) {
  pl_conn *c = malloc(sizeof *c);
  struct timeval quiet = { 10, 0 };
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &quiet, sizeof quiet);
  c->s = s;
  c->udp = udp;
  return c;
}

// Every address the name has is tried in turn, IPv6 and IPv4, until one answers.
pl_conn *pl_dial(const char *kind, const char *host, int port, buf *error) {
  struct addrinfo hints = { 0 };
  struct addrinfo *found = NULL;
  char service[16];
  int udp = !strcmp(kind, "udp");
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  snprintf(service, sizeof service, "%d", port);
  if (getaddrinfo(host, service, &hints, &found)) {
    buf_fmt(error, "%s couldn't be found", host);
    return NULL;
  }
  int s = -1;
  for (struct addrinfo *at = found; at && s == -1; at = at->ai_next) {
    s = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (s != -1 && connect(s, at->ai_addr, (socklen_t)at->ai_addrlen)) {
      close(s);
      s = -1;
    }
  }
  freeaddrinfo(found);
  if (s == -1) {
    buf_fmt(error, "%s didn't answer on port %d", host, port);
    return NULL;
  }
  return conn(s, udp);
}

// Everywhere: IPv6 and IPv4 on one socket, or IPv4 alone on a server without IPv6. Else 127.0.0.1 alone.
pl_conn *pl_bind(const char *kind, int port, int everywhere, buf *error) {
  struct sockaddr_in four = { 0 };
  struct sockaddr_in6 six = { 0 };   // its address all zeros: every one, IPv4 too once V6ONLY is off
  int no = 0;
  int yes = 1;
  if (strcmp(kind, "tcp")) {
    buf_fmt(error, "only tcp connections can come in so far");
    return NULL;
  }
  int s = everywhere ? socket(AF_INET6, SOCK_STREAM, 0) : -1;
  int v6 = s >= 0;
  four.sin_family = AF_INET;
  four.sin_port = htons((unsigned short)port);
  four.sin_addr.s_addr = htonl(everywhere ? INADDR_ANY : INADDR_LOOPBACK);
  six.sin6_family = AF_INET6;
  six.sin6_port = htons((unsigned short)port);
  if (!v6) s = socket(AF_INET, SOCK_STREAM, 0);
  if (s >= 0) {
    setsockopt(
        s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);   // so a restart doesn't wait for the last one's connections
    if (v6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
  }
  if (s < 0 || bind(s, v6 ? (struct sockaddr *)&six : (struct sockaddr *)&four, v6 ? sizeof six : sizeof four) ||
      listen(s, SOMAXCONN)) {
    buf_fmt(error, "Port %d couldn't be opened: %s", port,
        errno == EADDRINUSE ? "another program is using it" : strerror(errno));
    if (s >= 0) close(s);
    return NULL;
  }
  return conn(s, 0);
}

// An IPv4 address as IPv6 writes it, ::ffff:1.2.3.4, is written 1.2.3.4.
pl_conn *pl_next(pl_conn *listener, buf *from, double seconds) {
  struct sockaddr_storage at;
  socklen_t n = sizeof at;
  struct pollfd ready = { listener->s, POLLIN, 0 };
  if (poll(&ready, 1, (int)(seconds * 1000)) != 1) return NULL;
  int c = accept(listener->s, (struct sockaddr *)&at, &n);
  if (c < 0) return NULL;
  char text[64] = "";
  inet_ntop(at.ss_family,
      at.ss_family == AF_INET ? (void *)&((struct sockaddr_in *)&at)->sin_addr
                              : (void *)&((struct sockaddr_in6 *)&at)->sin6_addr,
      text, sizeof text);
  buf_str(from, strncmp(text, "::ffff:", 7) || !strchr(text, '.') ? text : text + 7);
  return conn(c, 0);
}

// MSG_NOSIGNAL: a connection the other side closed is a 0, never a signal that ends the program.
int pl_put(pl_conn *c, const char *data, size_t n) {
  if (c->udp) return send(c->s, data, n, MSG_NOSIGNAL) == (ssize_t)n;
  for (ssize_t sent; n; data += sent, n -= (size_t)sent)
    if ((sent = send(c->s, data, n, MSG_NOSIGNAL)) <= 0) return 0;
  return 1;
}

int pl_get(pl_conn *c, buf *out, double seconds) {
  char chunk[65536];
  struct pollfd ready = { c->s, POLLIN, 0 };
  if (seconds > 0 && poll(&ready, 1, (int)(seconds * 1000)) != 1) return 0;
  ssize_t got = recv(c->s, chunk, sizeof chunk, 0);
  if (got > 0) buf_add(out, chunk, (size_t)got);
  return got > 0;
}

void pl_shut(pl_conn *c) { shutdown(c->s, SHUT_RDWR); }

// What was sent goes before it closes.
void pl_drop(pl_conn *c) {
  shutdown(c->s, SHUT_WR);
  close(c->s);
  free(c);
}

////////// 7. ANDROID //////////

#ifdef __ANDROID__
// An app on Android has no curl, and Android's own does the web, so the app around the core reaches it, holds the links
// and knows what is around (pl_device in lingo.h).

// The app's answer to name, asked with the record in ask, read into a, or NULL with why in error.
static val *ask_app(arena *a, const char *name, buf *ask, buf *error) {
  buf out = { 0 };
  val *got = NULL;
  int ok = pl_device(name, ask->s ? ask->s : "{}", &out);
  if (ok)
    got = json_parse(a, out.s ? out.s : "null", out.s ? out.n : 4, NULL);
  else
    buf_add(error, out.s ? out.s : "", out.n);
  if (ok && !got) buf_fmt(error, "the phone's answer to %s couldn't be read", name);
  buf_free(&out);
  return got;
}

struct pl_link {
  double id;   // the app's number for it
};

pl_link *pl_connect(const char *url, const char *headers, buf *error) {
  arena *a = arena_new();
  buf ask = { 0 };
  buf_str(&ask, "{\"url\":");
  buf_json_str(&ask, url, strlen(url));
  if (headers) {
    buf_str(&ask, ",\"headers\":");
    buf_json(&ask, header_lines(a, headers));
  }
  buf_chr(&ask, '}');
  val *got = ask_app(a, "link.open", &ask, error);
  val *id = got ? rec_get(got, "id") : NULL;
  pl_link *l = NULL;
  if (id && id->k == V_NUM) {
    l = malloc(sizeof *l);
    l->id = id->n;
  } else if (!error->n) {
    buf_str(error, "the phone couldn't open the link");
  }
  buf_free(&ask);
  arena_free(a);
  return l;
}

// One of a link's calls after it opened: each names the link, and a send gives its text too.
static val *linux_link_call(arena *a, pl_link *l, const char *name, const char *text, size_t n) {
  buf ask = { 0 };
  buf error = { 0 };
  buf_fmt(&ask, "{\"id\":%.0f", l->id);
  if (text) {
    buf_str(&ask, ",\"text\":");
    buf_json_str(&ask, text, n);
  }
  buf_chr(&ask, '}');
  val *got = ask_app(a, name, &ask, &error);
  buf_free(&ask);
  buf_free(&error);
  return got;
}

int pl_send(pl_link *l, const char *text, size_t n) {
  arena *a = arena_new();
  int sent = linux_link_call(a, l, "link.send", text, n) != NULL;
  arena_free(a);
  return sent;
}

// The app waits for the next message, and gives {} with no text once the link is closed.
int pl_receive(pl_link *l, buf *out) {
  arena *a = arena_new();
  val *got = linux_link_call(a, l, "link.receive", NULL, 0);
  val *text = got ? rec_get(got, "text") : NULL;
  int open = text && text->k == V_TEXT;
  if (open) buf_add(out, text->t.s, text->t.len);
  arena_free(a);
  return open;
}

void pl_close(pl_link *l) {
  arena *a = arena_new();
  linux_link_call(a, l, "link.close", NULL, 0);
  arena_free(a);
}

void pl_free(pl_link *l) {
  arena *a = arena_new();
  linux_link_call(a, l, "link.free", NULL, 0);
  arena_free(a);
  free(l);
}

// The app knows the time, the zone, the language and the app in front.
void pl_context(buf *out) {
  size_t start = out->n;
  if (pl_device("context", "{}", out)) return;
  out->n = start;
  buf_str(out, "{\"os\":\"android\"}");
}

val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds,
    buf *error) {
  buf ask = { 0 };
  buf_str(&ask, "{\"method\":");
  buf_json_str(&ask, method, strlen(method));
  buf_str(&ask, ",\"url\":");
  buf_json_str(&ask, url, strlen(url));
  buf_str(&ask, ",\"headers\":");
  buf_json(&ask, headers);
  buf_str(&ask, ",\"body\":");
  buf_json_str(&ask, body ? body : "", body ? n : 0);
  buf_fmt(&ask, ",\"seconds\":%g}", seconds);
  val *got = ask_app(a, "http", &ask, error);
  buf_free(&ask);
  return got;
}
#endif

#endif
