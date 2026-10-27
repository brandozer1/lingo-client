// windows.c - the Windows lingo, a native DLL (windows.dll) the core loads: windows, controls, files, the clipboard,
// commands, notifications, Onsay's panel, input (the mouse and keyboard as a person uses them) and look (the screen read).
// OCR and notifications have no C API, so they go through Windows PowerShell. Positions are physical desktop pixels.
// What the app draws, Onsay's pointer, its rings and his panel, goes out as a line of JSON on stdout (show), which
// onsay-desktop's Overlay.cs reads.
//
// 1. helpers                  text, records, boxes, paths, waiting, PowerShell
// 2. window                   list, current, open, wait_for, focus, close, resize, move
// 3. controls                 find, list, read, press, fill, toggle, pick, scroll, wait_for
// 4. screen                   a picture of it, and its words by OCR
// 5. pointer and keys         Onsay's pointer (virtual or real), clicks, drags, the wheel, rings, his panel, and typing
// 6. files                    read, write, add_to, exists, list, find, copy, move, delete, make_folder
// 7. clipboard/commands/notify
// 8. input                    the mouse and keyboard the way a person uses them: words on screen, controls by name, places in a window
// 9. look                     the screen read as text, and a picture of it
// 10. lingo_phrases()         -> every phrase joined to its function, called by the core on load
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wincodec.h>
#include <uiautomation.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

////////// 1. HELPERS //////////

static const char *str(run *r, val *x) {
  if (x->k == V_TEXT) return x->t.s;
  buf b = { 0 };
  buf_text(&b, x);
  val *t = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return t->t.s;
}

static val *wtext(run *r, const wchar_t *w, int len) {
  buf b = { 0 };
  win_narrow(&b, w, len);
  val *t = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return t;
}

static void put(run *r, val *rec, const char *key, val *v) { rec_set(r->a, rec, key, v); }
static void put_num(run *r, val *rec, const char *key, double n) { rec_set(r->a, rec, key, v_num(r->a, n)); }
static val *part(val *rec, const char *key) { val *v = rec->k == V_REC ? rec_get(rec, key) : NULL; return v ? v : NOTHING; }
static double num_or(val *x, double fallback) { return x->k == V_NUM ? x->n : fallback; }

// Text as Windows lowers it, to find words whatever their case. Freed with free.
static wchar_t *lowered(run *r, val *x) {
  wchar_t *w = win_wide(str(r, x));
  CharLowerW(w);
  return w;
}

// Text without the spaces around it, as text.trim gives it.
static val *trimmed(run *r, val *x) {
  const char *s = str(r, x);
  size_t from = 0, to = strlen(s);
  while (from < to && isspace((unsigned char)s[from])) from++;
  while (to > from && isspace((unsigned char)s[to - 1])) to--;
  return v_text(r->a, s + from, to - from);
}

// A place on screen, {x,y,w,h}, as windows, controls and lines of text all have, with the text it holds when it has one.
typedef struct { double x, y, w, h; val *text; } box;

static box box_of(run *r, val *x) {
  return (box){ num_of(r, part(x, "x"), "x"), num_of(r, part(x, "y"), "y"), num_of(r, part(x, "w"), "w"), num_of(r, part(x, "h"), "h"), part(x, "text") };
}

static int mid(double at, double size) { return (int)(at + floor(size / 2)); }

// whole: {x,y,w,h,text}. Otherwise its middle, {x,y,text}, where a click goes.
static val *box_val(run *r, box b, int whole) {
  val *out = v_rec(r->a);
  put_num(r, out, "x", whole ? b.x : mid(b.x, b.w));
  put_num(r, out, "y", whole ? b.y : mid(b.y, b.h));
  if (whole) { put_num(r, out, "w", b.w); put_num(r, out, "h", b.h); }
  put(r, out, "text", b.text);
  return out;
}

// A wait that a stop ends at once.
static void nap(run *r, double seconds) {
  for (double until = pl_now() + seconds; pl_now() < until;) {
    if (*r->stop) fail(r, "stopped", "Stopped by the person");
    Sleep(20);
  }
}

// Every field of match is a case-insensitive substring of rec's: {title:"note"} finds Notepad.
static int matches(run *r, val *rec, val *match) {
  if (!match || match->k != V_REC) return 1;
  for (size_t i = 0; i < match->r.n; i++) {
    wchar_t *h = lowered(r, part(rec, match->r.key[i])), *w = lowered(r, match->r.v[i]);
    int found = wcsstr(h, w) != NULL;
    free(h);
    free(w);
    if (!found) return 0;
  }
  return 1;
}

// A path as the person means it: / or \, ~ for home, downloads/desktop/documents for those folders, otherwise from home.
static val *path_of(run *r, val *p) {
  wchar_t *w = win_wide(str(r, p)), full[MAX_PATH * 4], *base = NULL, joined[MAX_PATH * 4];
  for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
  size_t first = wcscspn(w, L"\\");
  const KNOWNFOLDERID *known = NULL;
  if (first == 1 && w[0] == L'~') known = &FOLDERID_Profile;
  else if (first == 9 && !_wcsnicmp(w, L"downloads", 9)) known = &FOLDERID_Downloads;
  else if (first == 7 && !_wcsnicmp(w, L"desktop", 7)) known = &FOLDERID_Desktop;
  else if (first == 9 && !_wcsnicmp(w, L"documents", 9)) known = &FOLDERID_Documents;
  int rooted = w[0] == L'\\' || (w[0] && w[1] == L':');
  if (known || !rooted) {
    SHGetKnownFolderPath(known ? known : &FOLDERID_Profile, 0, NULL, &base);
    _snwprintf(joined, sizeof joined / sizeof *joined, L"%ls%ls%ls", base ? base : L"", w[known ? first : 0] ? L"\\" : L"", w + (known ? first + (w[first] ? 1 : 0) : 0));
    CoTaskMemFree(base);
  } else wcsncpy(joined, w, sizeof joined / sizeof *joined - 1);
  joined[sizeof joined / sizeof *joined - 1] = 0;
  free(w);
  if (!GetFullPathNameW(joined, sizeof full / sizeof *full, full, NULL)) wcscpy(full, joined);
  return wtext(r, full, -1);
}

static void win_fail(run *r, const char *what, const char *path) {
  DWORD e = GetLastError();
  const char *why = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? "it doesn't exist" : e == ERROR_ACCESS_DENIED ? "Windows denied access"
                  : e == ERROR_SHARING_VIOLATION ? "another program has it open" : "Windows refused";
  fail(r, "phrase", "can't %s %s: %s", what, path, why);
}

// Runs a script from a temp file (no length limit). wait 0 starts it and returns; otherwise gives its exit code.
static int powershell(run *r, const char *script, int wait, double seconds, buf *out) {
  wchar_t dir[MAX_PATH], file[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  GetTempFileNameW(dir, L"ons", 0, file);
  wcscat(file, L".ps1");
  buf text = { 0 };
  buf_str(&text, "\xEF\xBB\xBF$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=[Text.Encoding]::UTF8\n");
  buf_str(&text, script);
  if (!wait) buf_str(&text, "\nRemove-Item -LiteralPath $PSCommandPath -ErrorAction SilentlyContinue");
  HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
  DWORD wrote = 0;
  if (f != INVALID_HANDLE_VALUE) { WriteFile(f, text.s, (DWORD)text.n, &wrote, NULL); CloseHandle(f); }
  buf_free(&text);
  wchar_t line[MAX_PATH + 128];
  _snwprintf(line, sizeof line / sizeof *line, L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%ls\"", file);

  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE readEnd = NULL, writeEnd = NULL;
  if (wait) { CreatePipe(&readEnd, &writeEnd, &sa, 0); SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); }
  STARTUPINFOW si = { .cb = sizeof si };
  si.dwFlags = wait ? STARTF_USESTDHANDLES : 0;
  si.hStdOutput = si.hStdError = writeEnd;
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    if (wait) { CloseHandle(readEnd); CloseHandle(writeEnd); }
    DeleteFileW(file);
    fail(r, "phrase", "Windows PowerShell wouldn't start");
  }
  CloseHandle(pi.hThread);
  if (!wait) { CloseHandle(pi.hProcess); return 0; }
  CloseHandle(writeEnd);
  double until = pl_now() + seconds;
  char chunk[8192];
  for (int done = 0;;) {
    DWORD ready = 0, got = 0;
    while (PeekNamedPipe(readEnd, NULL, 0, NULL, &ready, NULL) && ready && ReadFile(readEnd, chunk, ready < sizeof chunk ? ready : sizeof chunk, &got, NULL) && got) {
      if (out->n < 4u << 20) buf_add(out, chunk, got);
      ready = 0;
    }
    if (done) break;
    done = WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0;
    if (!done && (*r->stop || pl_now() > until)) {
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hProcess);
      CloseHandle(readEnd);
      DeleteFileW(file);
      if (*r->stop) fail(r, "stopped", "Stopped by the person");
      fail(r, "phrase", "it was still running after %.0f seconds, so it was stopped", seconds);
    }
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(readEnd);
  DeleteFileW(file);
  while (out->n && (out->s[out->n - 1] == '\n' || out->s[out->n - 1] == '\r' || out->s[out->n - 1] == ' ')) out->s[--out->n] = 0;
  return (int)code;
}

// Text as a PowerShell string that means exactly it.
static void ps_quote(buf *b, const char *s) {
  buf_chr(b, '\'');
  for (; *s; s++) { if (*s == '\'') buf_chr(b, '\''); buf_chr(b, *s); }
  buf_chr(b, '\'');
}

////////// 2. WINDOW //////////

static void title_of(HWND h, wchar_t *out, int n) { out[0] = 0; GetWindowTextW(h, out, n); }

// Top level windows front to back, the ones the taskbar would show. visit says whether to go on.
static void each_window(int (*visit)(HWND, void *), void *arg) {
  for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) continue;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) continue;
    int cloaked = 0;
    typedef HRESULT(WINAPI * attr_fn)(HWND, DWORD, PVOID, DWORD);
    static attr_fn attr;
    if (!attr) attr = (attr_fn)(void *)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute");
    if (attr && attr(h, 14, &cloaked, sizeof cloaked) == S_OK && cloaked) continue;   // 14 DWMWA_CLOAKED
    wchar_t t[8], c[16];
    title_of(h, t, 8);
    GetClassNameW(h, c, 16);
    if (!t[0] || !wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW")) continue;
    if (!visit(h, arg)) return;
  }
}

static val *window_rec(run *r, HWND h) {
  val *w = v_rec(r->a);
  RECT rc;
  GetWindowRect(h, &rc);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  wchar_t app[MAX_PATH] = L"", title[512];
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (p) {
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, app, &n)) {
      wchar_t *slash = wcsrchr(app, L'\\'), *dot;
      if (slash) memmove(app, slash + 1, (wcslen(slash + 1) + 1) * sizeof *app);
      if ((dot = wcsrchr(app, L'.'))) *dot = 0;
    }
    CloseHandle(p);
  }
  title_of(h, title, 512);
  put_num(r, w, "id", (double)(intptr_t)h);
  put(r, w, "app", wtext(r, app, -1));
  put(r, w, "title", wtext(r, title, -1));
  put_num(r, w, "x", rc.left);
  put_num(r, w, "y", rc.top);
  put_num(r, w, "w", rc.right - rc.left);
  put_num(r, w, "h", rc.bottom - rc.top);
  put(r, w, "focused", VBOOL(h == GetForegroundWindow()));
  put(r, w, "minimized", VBOOL(IsIconic(h)));
  return w;
}

static HWND hwnd_of(run *r, val *x, const char *what) {
  val *id = x->k == V_REC ? rec_get(x, "id") : x;
  if (!id || id->k != V_NUM) fail(r, "phrase", "%s must be a window, from window.list or window.wait_for", what);
  HWND h = (HWND)(intptr_t)id->n;
  if (!IsWindow(h)) fail(r, "phrase", "that window is gone. Find it again with window.list or window.wait_for");
  return h;
}

typedef struct { run *r; val *list, *match; HWND found; HWND *skip; size_t skips; } walk;

static int add_window(HWND h, void *arg) { walk *w = arg; list_push(w->r->a, w->list, window_rec(w->r, h)); return 1; }

static int match_window(HWND h, void *arg) {
  walk *w = arg;
  for (size_t i = 0; i < w->skips; i++) if (w->skip[i] == h) return 1;
  if (w->match && !matches(w->r, window_rec(w->r, h), w->match)) return 1;
  w->found = h;
  return 0;
}

static val *win_list(run *r, val **a) {
  walk w = { .r = r, .list = v_list(r->a) };
  each_window(add_window, &w);
  return w.list;
}

// Onsay's own windows (his chat and body) are never what the user means by the front window.
static int onsay_own(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  wchar_t path[MAX_PATH] = L"";
  DWORD n = MAX_PATH;
  if (p) { QueryFullProcessImageNameW(p, 0, path, &n); CloseHandle(p); }
  const wchar_t *name = wcsrchr(path, L'\\');
  return !_wcsicmp(name ? name + 1 : path, L"Onsay.exe");
}

static int first_window(HWND h, void *arg) {
  if (onsay_own(h) || IsIconic(h)) return 1;
  *(HWND *)arg = h;
  return 0;
}

// While the user talks to Onsay his chat has the focus, so the front window is the first one that isn't his.
static val *win_front(run *r, val **a) {
  HWND h = NULL;
  each_window(first_window, &h);
  return h ? window_rec(r, h) : NOTHING;
}

static int count_window(HWND h, void *arg) { walk *w = arg; w->skip[w->skips++] = h; return w->skips < 4096; }

// Opens it as a double click would, then waits up to ten seconds for a new window. Path-like targets go through path_of.
static val *win_open(run *r, val **a) {
  const char *t = str(r, a[0]);
  int path = !strstr(t, "://") && (strpbrk(t, "/\\~") || !_strnicmp(t, "downloads", 9) || !_strnicmp(t, "desktop", 7) || !_strnicmp(t, "documents", 9));
  wchar_t *target = win_wide(path ? path_of(r, a[0])->t.s : t), expanded[2048];
  ExpandEnvironmentStringsW(target, expanded, 2048);
  free(target);
  walk before = { .r = r, .skip = malloc(4096 * sizeof(HWND)) };
  each_window(count_window, &before);
  SHELLEXECUTEINFOW info = { .cbSize = sizeof info };
  info.lpVerb = L"open";
  info.lpFile = expanded;
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) { free(before.skip); fail(r, "phrase", "Windows couldn't open %s", t); }
  val *out = NOTHING;
  for (double until = pl_now() + 10; pl_now() < until && out == NOTHING;) {
    before.found = NULL;
    each_window(match_window, &before);
    if (before.found) { nap(r, 0.3); out = window_rec(r, before.found); }
    else nap(r, 0.2);
  }
  free(before.skip);
  return out;
}

static val *win_wait(run *r, val **a) {
  double seconds = a[1]->k == V_NUM ? a[1]->n : 10;
  for (double until = pl_now() + seconds;; nap(r, 0.25)) {
    walk w = { .r = r, .match = a[0] };
    each_window(match_window, &w);
    if (w.found) return window_rec(r, w.found);
    if (pl_now() > until) {
      buf m = { 0 };
      buf_json(&m, a[0]);
      fail(r, "phrase", "no window like %s appeared within %g seconds", m.s, seconds);
    }
  }
}

// Windows only lets the app the person last used take the front, so a tapped Alt counts as using this one.
static val *win_focus(run *r, val **a) {
  HWND h = hwnd_of(r, a[0], "id");
  if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
  keybd_event(VK_MENU, 0, 0, 0);
  keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
  SetForegroundWindow(h);
  return NOTHING;
}

static val *win_close(run *r, val **a) { PostMessageW(hwnd_of(r, a[0], "id"), WM_CLOSE, 0, 0); return NOTHING; }

static val *win_size(run *r, val **a) {
  const char *how = str(r, a[1]);
  ShowWindow(hwnd_of(r, a[0], "id"), !strcmp(how, "min") ? SW_MINIMIZE : !strcmp(how, "max") ? SW_MAXIMIZE : SW_RESTORE);
  return NOTHING;
}

static val *win_move(run *r, val **a) {
  HWND h = hwnd_of(r, a[0], "id");
  RECT rc;
  GetWindowRect(h, &rc);
  if (IsZoomed(h) || IsIconic(h)) ShowWindow(h, SW_RESTORE);
  int x = a[1]->k == V_NUM ? (int)a[1]->n : rc.left, y = a[2]->k == V_NUM ? (int)a[2]->n : rc.top;
  int w = a[3]->k == V_NUM ? (int)a[3]->n : rc.right - rc.left, hh = a[4]->k == V_NUM ? (int)a[4]->n : rc.bottom - rc.top;
  SetWindowPos(h, NULL, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
  return NOTHING;
}

////////// 3. CONTROLS //////////

// UI Automation: one cached fetch per window tree. Controls keep their ids between phrases so a program can find, then press.
static IUIAutomation *uia;
static IUIAutomationCacheRequest *fetch;
typedef struct { double id; IUIAutomationElement *e; } seen_control;
static seen_control *seen;
static size_t seen_count, seen_room;
static double next_control = 1;

static const PROPERTYID FETCHED[] = {
  UIA_ControlTypePropertyId, UIA_LocalizedControlTypePropertyId, UIA_NamePropertyId, UIA_BoundingRectanglePropertyId,
  UIA_IsOffscreenPropertyId, UIA_IsEnabledPropertyId, UIA_HasKeyboardFocusPropertyId, UIA_IsPasswordPropertyId,
  UIA_IsInvokePatternAvailablePropertyId, UIA_IsTogglePatternAvailablePropertyId, UIA_IsValuePatternAvailablePropertyId,
  UIA_IsSelectionItemPatternAvailablePropertyId, UIA_IsExpandCollapsePatternAvailablePropertyId, UIA_IsScrollPatternAvailablePropertyId,
  UIA_ValueValuePropertyId,
};

static const struct { int type; const char *role; } ROLES[] = {
  { UIA_ButtonControlTypeId, "button" }, { UIA_SplitButtonControlTypeId, "button" }, { UIA_EditControlTypeId, "field" },
  { UIA_DocumentControlTypeId, "document" }, { UIA_HyperlinkControlTypeId, "link" }, { UIA_CheckBoxControlTypeId, "checkbox" },
  { UIA_RadioButtonControlTypeId, "radio" }, { UIA_ComboBoxControlTypeId, "dropdown" }, { UIA_ListControlTypeId, "list" },
  { UIA_ListItemControlTypeId, "list item" }, { UIA_TreeItemControlTypeId, "tree item" }, { UIA_TabItemControlTypeId, "tab" },
  { UIA_MenuItemControlTypeId, "menu item" }, { UIA_MenuControlTypeId, "menu" }, { UIA_SliderControlTypeId, "slider" },
  { UIA_TextControlTypeId, "text" }, { UIA_ImageControlTypeId, "image" }, { UIA_DataItemControlTypeId, "row" },
  { UIA_HeaderItemControlTypeId, "header" }, { UIA_SpinnerControlTypeId, "field" }, { UIA_WindowControlTypeId, "window" },
  { UIA_PaneControlTypeId, "pane" }, { UIA_GroupControlTypeId, "group" }, { UIA_TableControlTypeId, "table" },
};

static void uia_start(run *r) {
  if (uia) return;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&uia)) || !uia)
    fail(r, "phrase", "Windows' UI Automation isn't available");
  IUIAutomation_CreateCacheRequest(uia, &fetch);
  for (size_t i = 0; i < sizeof FETCHED / sizeof *FETCHED; i++) IUIAutomationCacheRequest_AddProperty(fetch, FETCHED[i]);
  IUIAutomationCacheRequest_put_TreeScope(fetch, TreeScope_Subtree);
}

static VARIANT cached(IUIAutomationElement *e, PROPERTYID id) {
  VARIANT v;
  VariantInit(&v);
  IUIAutomationElement_GetCachedPropertyValue(e, id, &v);
  return v;
}

static int yes(IUIAutomationElement *e, PROPERTYID id) {
  VARIANT v = cached(e, id);
  int on = v.vt == VT_BOOL && v.boolVal;
  VariantClear(&v);
  return on;
}

static val *bstr_text(run *r, BSTR s) { return wtext(r, s ? s : L"", s ? (int)SysStringLen(s) : 0); }

// A control as a record, or NULL when it has no place on screen.
static val *control_rec(run *r, IUIAutomationElement *e) {
  VARIANT box = cached(e, UIA_BoundingRectanglePropertyId);
  double rc[4] = { 0 };
  if (box.vt == (VT_ARRAY | VT_R8) && box.parray) {
    double *d;
    if (SUCCEEDED(SafeArrayAccessData(box.parray, (void **)&d))) {
      if (box.parray->rgsabound[0].cElements >= 4) memcpy(rc, d, sizeof rc);
      SafeArrayUnaccessData(box.parray);
    }
  }
  VariantClear(&box);
  if (rc[2] <= 0 || rc[3] <= 0 || yes(e, UIA_IsOffscreenPropertyId)) return NULL;

  val *c = v_rec(r->a), *can = v_list(r->a);
  VARIANT type = cached(e, UIA_ControlTypePropertyId), local = cached(e, UIA_LocalizedControlTypePropertyId), name = cached(e, UIA_NamePropertyId);
  const char *role = NULL;
  for (size_t i = 0; i < sizeof ROLES / sizeof *ROLES && type.vt == VT_I4; i++) if (ROLES[i].type == type.lVal) role = ROLES[i].role;
  put(r, c, "role", role ? v_cstr(r->a, role) : bstr_text(r, local.vt == VT_BSTR ? local.bstrVal : NULL));
  val *n = bstr_text(r, name.vt == VT_BSTR ? name.bstrVal : NULL);
  size_t from = 0, to = n->t.len;
  while (from < to && (n->t.s[from] == ' ' || n->t.s[from] == '\n' || n->t.s[from] == '\r' || n->t.s[from] == '\t')) from++;
  while (to > from && (n->t.s[to - 1] == ' ' || n->t.s[to - 1] == '\n' || n->t.s[to - 1] == '\r' || n->t.s[to - 1] == '\t')) to--;
  put(r, c, "name", v_text(r->a, n->t.s + from, to - from));
  VariantClear(&type);
  VariantClear(&local);
  VariantClear(&name);
  if (yes(e, UIA_IsPasswordPropertyId)) put(r, c, "value", v_cstr(r->a, "(hidden)"));
  else if (yes(e, UIA_IsValuePatternAvailablePropertyId)) {
    VARIANT v = cached(e, UIA_ValueValuePropertyId);
    put(r, c, "value", v.vt == VT_BSTR ? bstr_text(r, v.bstrVal) : NOTHING);
    VariantClear(&v);
  } else put(r, c, "value", NOTHING);
  put_num(r, c, "x", (int)rc[0]);
  put_num(r, c, "y", (int)rc[1]);
  put_num(r, c, "w", (int)rc[2]);
  put_num(r, c, "h", (int)rc[3]);
  put(r, c, "enabled", VBOOL(yes(e, UIA_IsEnabledPropertyId)));
  put(r, c, "focused", VBOOL(yes(e, UIA_HasKeyboardFocusPropertyId)));
  if (yes(e, UIA_IsInvokePatternAvailablePropertyId)) list_push(r->a, can, v_cstr(r->a, "press"));
  if (yes(e, UIA_IsValuePatternAvailablePropertyId)) list_push(r->a, can, v_cstr(r->a, "fill"));
  if (yes(e, UIA_IsTogglePatternAvailablePropertyId)) list_push(r->a, can, v_cstr(r->a, "toggle"));
  if (yes(e, UIA_IsSelectionItemPatternAvailablePropertyId) || yes(e, UIA_IsExpandCollapsePatternAvailablePropertyId)) list_push(r->a, can, v_cstr(r->a, "choose"));
  if (yes(e, UIA_IsScrollPatternAvailablePropertyId)) list_push(r->a, can, v_cstr(r->a, "scroll"));
  put(r, c, "can", can);
  return c;
}

static void forget_controls(void) {
  for (size_t i = 0; i < seen_count; i++) IUIAutomationElement_Release(seen[i].e);
  seen_count = 0;
}

static double keep(IUIAutomationElement *e) {
  if (seen_count == seen_room) { seen_room = seen_room ? seen_room * 2 : 256; seen = realloc(seen, seen_room * sizeof *seen); }
  IUIAutomationElement_AddRef(e);
  seen[seen_count] = (seen_control){ next_control++, e };
  return seen[seen_count++].id;
}

typedef struct { run *r; val *found, *match; int max; } finding;

static void visit(finding *f, IUIAutomationElement *e) {
  if ((int)f->found->l.n >= f->max) return;
  val *c = control_rec(f->r, e);
  if (c) {
    const char *role = rec_get(c, "role")->t.s;
    int keepit = f->match ? matches(f->r, c, f->match) : rec_get(c, "name")->t.len > 0 || !strcmp(role, "field") || !strcmp(role, "checkbox") || !strcmp(role, "dropdown");
    if (keepit) { put_num(f->r, c, "id", keep(e)); list_push(f->r->a, f->found, c); }
  }
  IUIAutomationElementArray *kids = NULL;
  if (FAILED(IUIAutomationElement_GetCachedChildren(e, &kids)) || !kids) return;
  int n = 0;
  IUIAutomationElementArray_get_Length(kids, &n);
  for (int i = 0; i < n; i++) {
    IUIAutomationElement *kid = NULL;
    if (SUCCEEDED(IUIAutomationElementArray_GetElement(kids, i, &kid)) && kid) { visit(f, kid); IUIAutomationElement_Release(kid); }
  }
  IUIAutomationElementArray_Release(kids);
}

// What matches in a window: name, role and value, part of the text each. No match keeps every control worth seeing.
static val *find(run *r, HWND win, val *match, int max) {
  uia_start(r);
  if (seen_count > 20000) forget_controls();
  IUIAutomationElement *root = NULL;
  if (FAILED(IUIAutomation_ElementFromHandleBuildCache(uia, win, fetch, &root)) || !root) fail(r, "phrase", "that window can't be read");
  finding f = { r, v_list(r->a), match, max };
  visit(&f, root);
  IUIAutomationElement_Release(root);
  return f.found;
}

static IUIAutomationElement *element(run *r, val *control) {
  val *id = control->k == V_REC ? rec_get(control, "id") : control;
  if (!id || id->k != V_NUM) fail(r, "phrase", "that is not a control from controls.find");
  for (size_t i = 0; i < seen_count; i++) if (seen[i].id == id->n) return seen[i].e;
  fail(r, "phrase", "control %g is no longer known. Find it again with controls.find", id->n);
}

static void *pattern(IUIAutomationElement *e, PATTERNID id, REFIID iid) {
  void *p = NULL;
  IUIAutomationElement_GetCurrentPatternAs(e, id, iid, &p);
  return p;
}

static void click(run *r, int x, int y, int right, int times);
static void glide(run *r, int x, int y, double seconds);
static void show_press(int right, int times);
static void type_text(run *r, const char *text);
static void press_keys(run *r, const char *combo);

static val *ui_find(run *r, val **a) { return find(r, hwnd_of(r, a[0], "win"), a[1], 200); }
static val *ui_all(run *r, val **a) { return find(r, hwnd_of(r, a[0], "win"), NULL, a[1]->k == V_NUM ? (int)a[1]->n : 80); }

// The words of a page or document, through the text pattern browsers and editors give screen readers, or every control's name.
static val *ui_text(run *r, val **a) {
  HWND win = hwnd_of(r, a[0], "win");
  uia_start(r);
  IUIAutomationElement *root = NULL, *doc = NULL;
  IUIAutomationCondition *has = NULL, *shown = NULL, *both = NULL;
  VARIANT t, f;
  VariantInit(&t);
  VariantInit(&f);
  t.vt = f.vt = VT_BOOL;
  t.boolVal = VARIANT_TRUE;
  f.boolVal = VARIANT_FALSE;
  IUIAutomation_ElementFromHandle(uia, win, &root);
  IUIAutomation_CreatePropertyCondition(uia, UIA_IsTextPatternAvailablePropertyId, t, &has);
  IUIAutomation_CreatePropertyCondition(uia, UIA_IsOffscreenPropertyId, f, &shown);
  IUIAutomation_CreateAndCondition(uia, has, shown, &both);
  if (root && both) IUIAutomationElement_FindFirst(root, TreeScope_Subtree, both, &doc);
  val *out = NULL;
  IUIAutomationTextPattern *text = doc ? pattern(doc, UIA_TextPatternId, &IID_IUIAutomationTextPattern) : NULL;
  if (text) {
    IUIAutomationTextRange *range = NULL;
    BSTR words = NULL;
    if (SUCCEEDED(IUIAutomationTextPattern_get_DocumentRange(text, &range)) && range && SUCCEEDED(IUIAutomationTextRange_GetText(range, 200000, &words))) out = bstr_text(r, words);
    SysFreeString(words);
    if (range) IUIAutomationTextRange_Release(range);
    IUIAutomationTextPattern_Release(text);
  }
  if (doc) IUIAutomationElement_Release(doc);
  if (both) IUIAutomationCondition_Release(both);
  if (has) IUIAutomationCondition_Release(has);
  if (shown) IUIAutomationCondition_Release(shown);
  if (root) IUIAutomationElement_Release(root);
  if (out) return out;
  val *all = find(r, win, NULL, 2000);
  buf b = { 0 };
  for (size_t i = 0; i < all->l.n; i++) {
    val *n = rec_get(all->l.v[i], "name");
    if (!n->t.len) continue;
    if (b.n) buf_chr(&b, '\n');
    buf_add(&b, n->t.s, n->t.len);
  }
  out = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return out;
}

// The control's own action (invoke, toggle, select, expand, or focus for a text box). 0 when it has none.
static int act(IUIAutomationElement *e) {
  IUIAutomationInvokePattern *inv = pattern(e, UIA_InvokePatternId, &IID_IUIAutomationInvokePattern);
  if (inv) { IUIAutomationInvokePattern_Invoke(inv); IUIAutomationInvokePattern_Release(inv); return 1; }
  IUIAutomationTogglePattern *tog = pattern(e, UIA_TogglePatternId, &IID_IUIAutomationTogglePattern);
  if (tog) { IUIAutomationTogglePattern_Toggle(tog); IUIAutomationTogglePattern_Release(tog); return 1; }
  IUIAutomationSelectionItemPattern *sel = pattern(e, UIA_SelectionItemPatternId, &IID_IUIAutomationSelectionItemPattern);
  if (sel) { IUIAutomationSelectionItemPattern_Select(sel); IUIAutomationSelectionItemPattern_Release(sel); return 1; }
  IUIAutomationExpandCollapsePattern *exp = pattern(e, UIA_ExpandCollapsePatternId, &IID_IUIAutomationExpandCollapsePattern);
  if (exp) { IUIAutomationExpandCollapsePattern_Expand(exp); IUIAutomationExpandCollapsePattern_Release(exp); return 1; }
  CONTROLTYPEID type = 0;
  IUIAutomationElement_get_CurrentControlType(e, &type);
  if (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId || type == UIA_ComboBoxControlTypeId)
    return SUCCEEDED(IUIAutomationElement_SetFocus(e));
  return 0;
}

// The hand goes to it and presses it: its own action where it has one, a click in its middle where it doesn't.
static void press_element(run *r, IUIAutomationElement *e) {
  RECT rc;
  IUIAutomationElement_get_CurrentBoundingRectangle(e, &rc);
  int x = (rc.left + rc.right) / 2, y = (rc.top + rc.bottom) / 2;
  glide(r, x, y, 0);
  if (act(e)) show_press(0, 1);
  else click(r, x, y, 0, 1);
}

static val *ui_press(run *r, val **a) { press_element(r, element(r, a[0])); return NOTHING; }

static val *ui_fill(run *r, val **a) {
  IUIAutomationElement *e = element(r, a[0]);
  const char *text = str(r, a[1]);
  IUIAutomationValuePattern *v = pattern(e, UIA_ValuePatternId, &IID_IUIAutomationValuePattern);
  BOOL locked = TRUE;
  if (v) IUIAutomationValuePattern_get_CurrentIsReadOnly(v, &locked);
  if (v && !locked) {
    wchar_t *w = win_wide(text);
    BSTR b = SysAllocString(w);
    HRESULT done = IUIAutomationValuePattern_SetValue(v, b);
    SysFreeString(b);
    free(w);
    IUIAutomationValuePattern_Release(v);
    if (SUCCEEDED(done)) return NOTHING;
  } else if (v) IUIAutomationValuePattern_Release(v);
  IUIAutomationElement_SetFocus(e);
  press_keys(r, "ctrl+a");
  type_text(r, text);
  return NOTHING;
}

static val *ui_toggle(run *r, val **a) {
  IUIAutomationTogglePattern *t = pattern(element(r, a[0]), UIA_TogglePatternId, &IID_IUIAutomationTogglePattern);
  if (!t) fail(r, "phrase", "that control can't toggle");
  IUIAutomationTogglePattern_Toggle(t);
  IUIAutomationTogglePattern_Release(t);
  return NOTHING;
}

// A dropdown or list: opened if it opens, then the option picked by name, wherever its list appears.
static val *ui_choose(run *r, val **a) {
  IUIAutomationElement *e = element(r, a[0]), *item = NULL, *top = NULL;
  IUIAutomationExpandCollapsePattern *exp = pattern(e, UIA_ExpandCollapsePatternId, &IID_IUIAutomationExpandCollapsePattern);
  if (exp) { IUIAutomationExpandCollapsePattern_Expand(exp); nap(r, 0.3); }
  wchar_t *w = win_wide(str(r, a[1]));
  VARIANT name, kind;
  VariantInit(&name);
  VariantInit(&kind);
  name.vt = VT_BSTR;
  name.bstrVal = SysAllocString(w);
  kind.vt = VT_I4;
  kind.lVal = UIA_ListItemControlTypeId;
  free(w);
  IUIAutomationCondition *named = NULL, *listed = NULL, *both = NULL;
  IUIAutomation_CreatePropertyConditionEx(uia, UIA_NamePropertyId, name, PropertyConditionFlags_IgnoreCase, &named);
  IUIAutomationElement_FindFirst(e, TreeScope_Descendants, named, &item);
  if (!item) {
    IUIAutomation_CreatePropertyCondition(uia, UIA_ControlTypePropertyId, kind, &listed);
    IUIAutomation_CreateAndCondition(uia, named, listed, &both);
    IUIAutomation_GetRootElement(uia, &top);
    if (top && both) IUIAutomationElement_FindFirst(top, TreeScope_Descendants, both, &item);
  }
  VariantClear(&name);
  if (named) IUIAutomationCondition_Release(named);
  if (listed) IUIAutomationCondition_Release(listed);
  if (both) IUIAutomationCondition_Release(both);
  if (top) IUIAutomationElement_Release(top);
  if (!item) { if (exp) IUIAutomationExpandCollapsePattern_Release(exp); fail(r, "phrase", "there is no option called %s", str(r, a[1])); }
  IUIAutomationSelectionItemPattern *sel = pattern(item, UIA_SelectionItemPatternId, &IID_IUIAutomationSelectionItemPattern);
  if (sel) { IUIAutomationSelectionItemPattern_Select(sel); IUIAutomationSelectionItemPattern_Release(sel); }
  else press_element(r, item);
  IUIAutomationElement_Release(item);
  if (exp) {
    enum ExpandCollapseState state = ExpandCollapseState_Collapsed;
    IUIAutomationExpandCollapsePattern_get_CurrentExpandCollapseState(exp, &state);
    if (state == ExpandCollapseState_Expanded) IUIAutomationExpandCollapsePattern_Collapse(exp);
    IUIAutomationExpandCollapsePattern_Release(exp);
  }
  return NOTHING;
}

static val *ui_scroll(run *r, val **a) {
  IUIAutomationScrollPattern *s = pattern(element(r, a[0]), UIA_ScrollPatternId, &IID_IUIAutomationScrollPattern);
  if (!s) fail(r, "phrase", "that control can't scroll");
  int amount = (int)num_of(r, a[1], NULL);
  for (int i = 0; i < abs(amount) * 3; i++) IUIAutomationScrollPattern_Scroll(s, ScrollAmount_NoAmount, amount > 0 ? ScrollAmount_SmallDecrement : ScrollAmount_SmallIncrement);
  IUIAutomationScrollPattern_Release(s);
  return NOTHING;
}

static val *ui_wait(run *r, val **a) {
  HWND win = hwnd_of(r, a[0], "win");
  double seconds = a[2]->k == V_NUM ? a[2]->n : 10;
  for (double until = pl_now() + seconds;; nap(r, 0.3)) {
    val *got = find(r, win, a[1], 1);
    if (got->l.n) return got->l.v[0];
    if (pl_now() > until) {
      buf m = { 0 };
      buf_json(&m, a[1]);
      fail(r, "phrase", "no control like %s appeared within %g seconds", m.s, seconds);
    }
  }
}

////////// 4. SCREEN //////////

/* grab(): a picture of a window, or the main screen when win is nothing, saved as a PNG in ~/Onsay/shots. box is
   nothing, or {x,y,w,h} in the window's pixels: only that part is kept. Gives the file, and the part's place on screen. */
typedef struct { val *path; int x, y, w, h; } shot;

static shot grab(run *r, val *win, val *box) {
  HWND hwnd = win->k == V_NOTHING ? NULL : hwnd_of(r, win, "window");
  RECT rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
  if (hwnd) GetWindowRect(hwnd, &rc);
  int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  if (ww <= 0 || wh <= 0) fail(r, "phrase", "that window has no size to take a picture of");
  int bx = 0, by = 0, bw = ww, bh = wh;
  if (box->k == V_REC) {
    val *v[4] = { rec_get(box, "x"), rec_get(box, "y"), rec_get(box, "w"), rec_get(box, "h") };
    for (int i = 0; i < 4; i++) if (!v[i] || v[i]->k != V_NUM) fail(r, "phrase", "box must be {x,y,w,h}, in pixels inside the window");
    bx = (int)v[0]->n, by = (int)v[1]->n, bw = (int)v[2]->n, bh = (int)v[3]->n;
    if (bx < 0) bw += bx, bx = 0;
    if (by < 0) bh += by, by = 0;
    if (bx + bw > ww) bw = ww - bx;
    if (by + bh > wh) bh = wh - by;
    if (bw <= 0 || bh <= 0) fail(r, "phrase", "that box isn't inside the window");
  }

  HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, ww, wh);
  HGDIOBJ old = SelectObject(mem, bmp);
  if (!hwnd || !PrintWindow(hwnd, mem, 2)) BitBlt(mem, 0, 0, ww, wh, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);   // 2 PW_RENDERFULLCONTENT
  SelectObject(mem, old);

  wchar_t *home = NULL, folder[MAX_PATH], file[MAX_PATH];
  SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &home);
  _snwprintf(folder, MAX_PATH, L"%ls\\Onsay\\shots", home ? home : L".");
  CoTaskMemFree(home);
  SHCreateDirectoryExW(NULL, folder, NULL);
  SYSTEMTIME t;
  GetLocalTime(&t);
  _snwprintf(file, MAX_PATH, L"%ls\\%04d%02d%02d-%02d%02d%02d-%03d.png", folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

  // Window bitmap, clipped to box, encoded to file.
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  IWICImagingFactory *wic = NULL;
  IWICBitmap *pic = NULL;
  IWICBitmapClipper *clip = NULL;
  IWICStream *stream = NULL;
  IWICBitmapEncoder *enc = NULL;
  IWICBitmapFrameEncode *frame = NULL;
  IPropertyBag2 *props = NULL;
  GUID format = GUID_WICPixelFormat24bppBGR;
  WICRect part = { bx, by, bw, bh };
  HRESULT ok = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
  if (SUCCEEDED(ok)) ok = IWICImagingFactory_CreateBitmapFromHBITMAP(wic, bmp, NULL, WICBitmapIgnoreAlpha, &pic);
  if (SUCCEEDED(ok)) ok = IWICImagingFactory_CreateBitmapClipper(wic, &clip);
  if (SUCCEEDED(ok)) ok = IWICBitmapClipper_Initialize(clip, (IWICBitmapSource *)pic, &part);
  if (SUCCEEDED(ok)) ok = IWICImagingFactory_CreateStream(wic, &stream);
  if (SUCCEEDED(ok)) ok = IWICStream_InitializeFromFilename(stream, file, GENERIC_WRITE);
  if (SUCCEEDED(ok)) ok = IWICImagingFactory_CreateEncoder(wic, &GUID_ContainerFormatPng, NULL, &enc);
  if (SUCCEEDED(ok)) ok = IWICBitmapEncoder_Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(ok)) ok = IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props);
  if (SUCCEEDED(ok)) ok = IWICBitmapFrameEncode_Initialize(frame, props);
  if (SUCCEEDED(ok)) ok = IWICBitmapFrameEncode_SetSize(frame, (UINT)bw, (UINT)bh);
  if (SUCCEEDED(ok)) ok = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
  if (SUCCEEDED(ok)) ok = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)clip, NULL);
  if (SUCCEEDED(ok)) ok = IWICBitmapFrameEncode_Commit(frame);
  if (SUCCEEDED(ok)) ok = IWICBitmapEncoder_Commit(enc);
  if (props) IPropertyBag2_Release(props);
  if (frame) IWICBitmapFrameEncode_Release(frame);
  if (enc) IWICBitmapEncoder_Release(enc);
  if (stream) IWICStream_Release(stream);
  if (clip) IWICBitmapClipper_Release(clip);
  if (pic) IWICBitmap_Release(pic);
  if (wic) IWICImagingFactory_Release(wic);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  if (FAILED(ok)) fail(r, "phrase", "the picture couldn't be saved");
  return (shot){ wtext(r, file, -1), rc.left + bx, rc.top + by, bw, bh };
}

static val *look_screenshot(run *r, val **a) {
  shot s = grab(r, a[0], a[1]);
  val *out = v_rec(r->a);
  put(r, out, "path", s.path);
  put_num(r, out, "x", s.x);
  put_num(r, out, "y", s.y);
  put_num(r, out, "w", s.w);
  put_num(r, out, "h", s.h);
  return out;
}

// Windows' own OCR of a window, or the main screen, one entry per line, placed on the screen.
static val *screen_read(run *r, val *window) {
  shot taken = grab(r, window, NOTHING);
  val *file = taken.path;
  int x = taken.x, y = taken.y;
  buf s = { 0 }, out = { 0 };
  buf_str(&s, "$path=");
  ps_quote(&s, file->t.s);
  buf_fmt(&s, ";$ox=%d;$oy=%d\n", x, y);
  buf_str(&s,
    "Add-Type -AssemblyName System.Runtime.WindowsRuntime\n"
    "$null=[Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]\n"
    "$null=[Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]\n"
    "$null=[Windows.Graphics.Imaging.BitmapDecoder,Windows.Graphics,ContentType=WindowsRuntime]\n"
    "$null=[Windows.Graphics.Imaging.BitmapTransform,Windows.Graphics,ContentType=WindowsRuntime]\n"
    "$as=([System.WindowsRuntimeSystemExtensions].GetMethods()|?{$_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'})[0]\n"
    "function Await($op,$t){$task=$as.MakeGenericMethod($t).Invoke($null,@($op));$null=$task.Wait(-1);$task.Result}\n"
    "$file=Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])\n"
    "$stream=Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])\n"
    "$decoder=Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])\n"
    // OCR reads small screen text far better at twice the size, so enlarge, then scale every box back.
    "$max=[Windows.Media.Ocr.OcrEngine]::MaxImageDimension\n"
    "$k=[Math]::Min(2,[Math]::Min($max/$decoder.PixelWidth,$max/$decoder.PixelHeight))\n"
    "$tf=New-Object Windows.Graphics.Imaging.BitmapTransform\n"
    "$tf.ScaledWidth=[uint32][Math]::Floor($decoder.PixelWidth*$k);$tf.ScaledHeight=[uint32][Math]::Floor($decoder.PixelHeight*$k)\n"
    "$tf.InterpolationMode=[Windows.Graphics.Imaging.BitmapInterpolationMode]::Fant\n"
    "$bitmap=Await ($decoder.GetSoftwareBitmapAsync([Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,[Windows.Graphics.Imaging.BitmapAlphaMode]::Premultiplied,"
    "$tf,[Windows.Graphics.Imaging.ExifOrientationMode]::IgnoreExifOrientation,[Windows.Graphics.Imaging.ColorManagementMode]::DoNotColorManage)) "
    "([Windows.Graphics.Imaging.SoftwareBitmap])\n"
    "$engine=[Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()\n"
    "if(-not $engine){Write-Output 'NO OCR';exit 3}\n"
    "$result=Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])\n"
    "$lines=@($result.Lines|%{$b=$_.Words|%{$_.BoundingRect};$l=($b|%{$_.X}|measure -Minimum).Minimum;$t=($b|%{$_.Y}|measure -Minimum).Minimum;"
    "$rr=($b|%{$_.X+$_.Width}|measure -Maximum).Maximum;$bb=($b|%{$_.Y+$_.Height}|measure -Maximum).Maximum;"
    "[ordered]@{text=$_.Text;x=[int]($l/$k)+$ox;y=[int]($t/$k)+$oy;w=[int](($rr-$l)/$k);h=[int](($bb-$t)/$k)}})\n"
    "ConvertTo-Json -InputObject $lines -Compress -Depth 3\n");
  int code = powershell(r, s.s, 1, 60, &out);
  buf_free(&s);
  const char *problem = NULL;
  val *lines = code == 0 ? json_parse(r->a, out.s ? out.s : "[]", out.n, &problem) : NULL;
  buf_free(&out);
  if (code == 3) fail(r, "phrase", "Windows has no OCR language installed");
  if (!lines || problem) fail(r, "phrase", "Windows' OCR didn't answer");
  return lines->k == V_LIST ? lines : NOTHING;
}

////////// 5. POINTER AND KEYS //////////

static void send_input(INPUT *in, int n) { SendInput((UINT)n, in, sizeof *in); }

static void mouse(DWORD flags, DWORD data) {
  INPUT in = { .type = INPUT_MOUSE };
  in.mi.dwFlags = flags;
  in.mi.mouseData = data;
  send_input(&in, 1);
}

static void key(WORD vk, int up) {
  INPUT in = { .type = INPUT_KEYBOARD };
  in.ki.wVk = vk;
  in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
  send_input(&in, 1);
}

/* The hand: Onsay's arrow, drawn by the app, glides along an eased curve to every place he acts. real_mode:
     0  virtual, the default: the user's mouse is never touched. UI Automation, else click messages (some apps ignore them)
     1  real: the real mouse rides the same curve and clicks for real */
static int real_mode;
static int hand_x, hand_y, hand_known;

// One JSON line on stdout for the app that draws the hand and rings.
static void show(buf *b) {
  DWORD wrote;
  buf_chr(b, '\n');
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b->s, (DWORD)b->n, &wrote, NULL);
  buf_free(b);
}

static void show_press(int right, int times) {
  buf b = { 0 };
  buf_fmt(&b, "{\"show\":\"press\",\"button\":\"%s\",\"times\":%d}", right ? "right" : "left", times);
  show(&b);
}

static void wait_ms(run *r, double ms) {
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  Sleep((DWORD)(ms > 0 ? ms : 0));
}

// To x, y over seconds, or at a hand's pace (90 ms + 0.18 ms a pixel) when 0. A random bend makes a curve the app draws
// from the same numbers. Starts at the real mouse the first time, and always in real mode.
static void glide(run *r, int x, int y, double seconds) {
  if (real_mode || !hand_known) {
    POINT p;
    GetCursorPos(&p);
    hand_x = p.x, hand_y = p.y, hand_known = 1;
  }
  int fx = hand_x, fy = hand_y;
  double dx = x - fx, dy = y - fy, span = sqrt(dx * dx + dy * dy), ms = seconds > 0 ? seconds * 1000 : span < 1 ? 0 : 90 + 0.18 * span;
  if (ms > 60000) ms = 60000;
  double bend = span < 1 ? 0 : (rand() / (double)RAND_MAX * 2 - 1) * 0.1 * span, one = span > 1 ? span : 1;
  double cx = fx + dx / 2 - dy / one * bend, cy = fy + dy / 2 + dx / one * bend;
  buf b = { 0 };
  buf_fmt(&b, "{\"show\":\"glide\",\"from\":[%d,%d],\"to\":[%d,%d],\"ms\":%d,\"bend\":%.2f,\"real\":%s}", fx, fy, x, y, (int)ms, bend,
          real_mode ? "true" : "false");
  show(&b);
  hand_x = x, hand_y = y;
  // Steps follow the clock, not the sleeps, so the real mouse stays in step with the drawn arrow.
  double start = pl_now();
  for (double k = 0; k < 1;) {
    k = ms > 0 ? (pl_now() - start) * 1000 / ms : 1;
    if (k > 1) k = 1;
    if (real_mode) {
      double t = (1 - cos(3.14159265358979 * k)) / 2, u = 1 - t;
      SetCursorPos((int)lround(u * u * fx + 2 * u * t * cx + t * t * x), (int)lround(u * u * fy + 2 * u * t * cy + t * t * y));
    }
    if (k < 1) wait_ms(r, real_mode ? 4 : 15);
  }
}

// The window at a point and the point in its client terms, brought to the front so typing goes to it.
static HWND window_at(run *r, int x, int y, LPARAM *inside) {
  POINT p = { x, y };
  HWND h = WindowFromPoint(p), top = h ? GetAncestor(h, GA_ROOT) : NULL;
  if (!h) fail(r, "phrase", "there is no window at %d, %d", x, y);
  if (top && top != GetForegroundWindow()) {
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(top);
  }
  ScreenToClient(h, &p);
  *inside = MAKELPARAM(p.x, p.y);
  return h;
}

// A click without the real mouse: UI Automation on the element or its two parents, else click messages to the window.
static void virtual_click(run *r, int x, int y, int right, int times) {
  LPARAM inside;
  HWND h = window_at(r, x, y, &inside);
  if (!right && times == 1) {
    uia_start(r);
    IUIAutomationElement *e = NULL, *up = NULL;
    IUIAutomationTreeWalker *walk = NULL;
    POINT p = { x, y };
    IUIAutomation_ElementFromPoint(uia, p, &e);
    IUIAutomation_get_ControlViewWalker(uia, &walk);
    int done = 0;
    for (int level = 0; e && walk && level < 3 && !done; level++) {
      done = act(e);
      if (!done && SUCCEEDED(IUIAutomationTreeWalker_GetParentElement(walk, e, &up))) { IUIAutomationElement_Release(e); e = up; up = NULL; }
      else break;
    }
    if (e) IUIAutomationElement_Release(e);
    if (walk) IUIAutomationTreeWalker_Release(walk);
    if (done) return;
  }
  UINT down = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, again = right ? WM_RBUTTONDBLCLK : WM_LBUTTONDBLCLK, lift = right ? WM_RBUTTONUP : WM_LBUTTONUP;
  WPARAM held = right ? MK_RBUTTON : MK_LBUTTON;
  PostMessageW(h, WM_MOUSEMOVE, 0, inside);
  for (int i = 0; i < times; i++) {
    PostMessageW(h, i ? again : down, held, inside);
    PostMessageW(h, lift, 0, inside);
  }
}

static void click(run *r, int x, int y, int right, int times) {
  glide(r, x, y, 0);
  show_press(right, times);
  if (!real_mode) { virtual_click(r, x, y, right, times); return; }
  wait_ms(r, 30);
  for (int i = 0; i < times; i++) {
    mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN, 0);
    mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP, 0);
    wait_ms(r, 60);
  }
}

static int at(run *r, val *v) { return (int)num_of(r, v, NULL); }

// Held down at the start, carried to the end and let go. Virtually, the window under the start gets it all as messages.
static void drag(run *r, int x1, int y1, int x2, int y2) {
  glide(r, x1, y1, 0);
  show_press(0, 0);
  if (real_mode) {
    mouse(MOUSEEVENTF_LEFTDOWN, 0);
    glide(r, x2, y2, 0);
    mouse(MOUSEEVENTF_LEFTUP, 0);
    return;
  }
  LPARAM inside;
  HWND h = window_at(r, x1, y1, &inside);
  PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, inside);
  glide(r, x2, y2, 0);
  for (int i = 1; i <= 10; i++) {
    POINT p = { x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10 };
    ScreenToClient(h, &p);
    PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(p.x, p.y));
  }
  POINT end = { x2, y2 };
  ScreenToClient(h, &end);
  PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
}


static void wheel(run *r, int x, int y, int amount) {
  glide(r, x, y, 0);
  if (real_mode) { mouse(MOUSEEVENTF_WHEEL, (DWORD)(amount * WHEEL_DELTA)); return; }
  LPARAM inside;
  HWND h = window_at(r, x, y, &inside);
  PostMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(amount * WHEEL_DELTA)), MAKELPARAM(x, y));   // a wheel's point is on the screen
}


static val *mouse_mode(run *r, val **a) {
  if (a[0]->k != V_NOTHING) {
    const char *how = str(r, a[0]);
    if (strcmp(how, "virtual") && strcmp(how, "real")) fail(r, "phrase", "the mode is virtual or real, not %s", how);
    real_mode = !strcmp(how, "real");
  }
  return v_cstr(r->a, real_mode ? "real" : "virtual");
}

// A ring around a box with Onsay beside it saying say, until seconds pass, another ring is drawn, or clear.
static void ring(run *r, box where, val *say, double seconds) {
  buf b = { 0 };
  const char *words = str(r, say);   // nothing says nothing
  buf_fmt(&b, "{\"show\":\"point\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"seconds\":%g,\"say\":", (int)where.x, (int)where.y, (int)where.w, (int)where.h, seconds);
  buf_json_str(&b, words, strlen(words));
  buf_chr(&b, '}');
  show(&b);
}

static val *input_point_at(run *r, val **a) {
  ring(r, (box){ at(r, a[0]), at(r, a[1]), at(r, a[2]), at(r, a[3]), NOTHING }, a[4], num_or(a[5], 10));
  return NOTHING;
}

static val *show_clear(run *r, val **a) {
  buf b = { 0 };
  buf_str(&b, "{\"show\":\"clear\"}");
  show(&b);
  return NOTHING;
}

// A small rounded window of Onsay's own, drawn by the app, showing a page of HTML until it is hidden or closed.
static val *panel_show(run *r, val **a) {
  const char *html = str(r, a[0]), *title = str(r, a[1]);
  buf b = { 0 };
  buf_fmt(&b, "{\"show\":\"panel\",\"w\":%d,\"h\":%d,\"title\":", (int)num_or(a[2], 480), (int)num_or(a[3], 360));
  buf_json_str(&b, title, strlen(title));
  buf_str(&b, ",\"html\":");
  buf_json_str(&b, html, strlen(html));
  buf_chr(&b, '}');
  show(&b);
  return NOTHING;
}

static val *panel_hide(run *r, val **a) {
  buf b = { 0 };
  buf_str(&b, "{\"show\":\"unpanel\"}");
  show(&b);
  return NOTHING;
}

// Each character as Unicode input. A new line is Enter, because apps listen for the key, not the character.
static void type_text(run *r, const char *text) {
  wchar_t *w = win_wide(text);
  for (wchar_t *c = w; *c; c++) {
    if (*c == L'\r') continue;
    if (*c == L'\n') { key(VK_RETURN, 0); key(VK_RETURN, 1); }
    else {
      INPUT in[2] = { { .type = INPUT_KEYBOARD }, { .type = INPUT_KEYBOARD } };
      in[0].ki.wScan = in[1].ki.wScan = *c;
      in[0].ki.dwFlags = KEYEVENTF_UNICODE;
      in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      send_input(in, 2);
    }
    if (*r->stop) { free(w); fail(r, "stopped", "Stopped by the person"); }
    Sleep(5);
  }
  free(w);
}

static const struct { const char *name; WORD vk; } KEYS[] = {
  { "ctrl", VK_CONTROL }, { "control", VK_CONTROL }, { "shift", VK_SHIFT }, { "alt", VK_MENU }, { "win", VK_LWIN }, { "enter", VK_RETURN },
  { "return", VK_RETURN }, { "tab", VK_TAB }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE }, { "backspace", VK_BACK }, { "delete", VK_DELETE },
  { "del", VK_DELETE }, { "space", VK_SPACE }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
  { "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "insert", VK_INSERT },
};

// Like ctrl+s: every key down in order, then up in reverse.
static void press_keys(run *r, const char *combo) {
  WORD keys[8];
  int n = 0;
  for (const char *p = combo; *p && n < 8;) {
    while (*p == ' ' || *p == '+') p++;
    const char *e = p;
    while (*e && *e != '+') e++;
    size_t len = (size_t)(e - p);
    while (len && p[len - 1] == ' ') len--;
    if (!len) break;
    char k[16] = { 0 };
    for (size_t i = 0; i < len && i < 15; i++) k[i] = (char)(p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
    WORD vk = 0;
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) if (!strcmp(KEYS[i].name, k)) vk = KEYS[i].vk;
    if (!vk && len == 1 && ((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= '0' && k[0] <= '9'))) vk = (WORD)(k[0] >= 'a' ? k[0] - 32 : k[0]);
    if (!vk && k[0] == 'f' && atoi(k + 1) >= 1 && atoi(k + 1) <= 12) vk = (WORD)(VK_F1 + atoi(k + 1) - 1);
    if (!vk) fail(r, "phrase", "there is no key called %s", k);
    keys[n++] = vk;
    p = e;
  }
  for (int i = 0; i < n; i++) key(keys[i], 0);
  for (int i = n; i-- > 0;) key(keys[i], 1);
}


////////// 6. FILES //////////

static val *fs_read(run *r, val **a) {
  val *p = path_of(r, a[0]);
  buf b = { 0 };
  if (!pl_read(p->t.s, &b)) win_fail(r, "read", p->t.s);
  size_t skip = b.n >= 3 && !memcmp(b.s, "\xEF\xBB\xBF", 3) ? 3 : 0;   // a byte order mark isn't part of the text
  val *out = v_text(r->a, b.s + skip, b.n - skip);
  buf_free(&b);
  return out;
}

static void parent_folder(run *r, val *p) {
  wchar_t *w = win_wide(p->t.s), *slash = wcsrchr(w, L'\\');
  if (slash && slash > w + 2) { *slash = 0; SHCreateDirectoryExW(NULL, w, NULL); }
  free(w);
}

static val *fs_write(run *r, val **a) {
  val *p = path_of(r, a[0]);
  buf b = { 0 };
  if (a[1]->k == V_TEXT) buf_add(&b, a[1]->t.s, a[1]->t.len); else buf_json(&b, a[1]);
  parent_folder(r, p);
  int ok = pl_write(p->t.s, b.s ? b.s : "", b.n);
  buf_free(&b);
  if (!ok) win_fail(r, "write", p->t.s);
  return NOTHING;
}

static val *fs_append(run *r, val **a) {
  val *p = path_of(r, a[0]);
  const char *text = str(r, a[1]);
  wchar_t *w = win_wide(p->t.s);
  HANDLE f = CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  free(w);
  if (f == INVALID_HANDLE_VALUE) win_fail(r, "add to", p->t.s);
  DWORD wrote = 0;
  WriteFile(f, text, (DWORD)strlen(text), &wrote, NULL);
  CloseHandle(f);
  return NOTHING;
}

static DWORD attributes(val *p) {
  wchar_t *w = win_wide(p->t.s);
  DWORD at = GetFileAttributesW(w);
  free(w);
  return at;
}

static val *fs_exists(run *r, val **a) { return VBOOL(attributes(path_of(r, a[0])) != INVALID_FILE_ATTRIBUTES); }

// * is any run of characters and ? any one, ignoring case, as Windows names files.
static int glob(const wchar_t *p, const wchar_t *s) {
  if (!*p) return !*s;
  if (*p == L'*') return glob(p + 1, s) || (*s && glob(p, s + 1));
  return *s && (*p == L'?' || towlower(*p) == towlower(*s)) && glob(p + 1, s + 1);
}

static val *file_rec(run *r, const wchar_t *folder, WIN32_FIND_DATAW *d) {
  wchar_t full[MAX_PATH * 4];
  _snwprintf(full, sizeof full / sizeof *full, L"%ls\\%ls", folder, d->cFileName);
  SYSTEMTIME t;
  FileTimeToSystemTime(&d->ftLastWriteTime, &t);
  char when[32];
  snprintf(when, sizeof when, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  val *f = v_rec(r->a);
  put(r, f, "name", wtext(r, d->cFileName, -1));
  put(r, f, "path", wtext(r, full, -1));
  put_num(r, f, "size", (double)(((unsigned long long)d->nFileSizeHigh << 32) | d->nFileSizeLow));
  put(r, f, "folder", VBOOL(d->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
  put(r, f, "changed", v_cstr(r->a, when));
  return f;
}

// Entries in folder whose name fits pattern, recursive with deep, up to 10,000.
static void look_in(run *r, const wchar_t *folder, const wchar_t *pattern, int deep, val *out) {
  wchar_t all[MAX_PATH * 4];
  _snwprintf(all, sizeof all / sizeof *all, L"%ls\\*", folder);
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW(all, &d);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
    if (out->l.n >= 10000 || *r->stop) break;
    if (glob(pattern, d.cFileName)) list_push(r->a, out, file_rec(r, folder, &d));
    if (deep && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      wchar_t inner[MAX_PATH * 4];
      _snwprintf(inner, sizeof inner / sizeof *inner, L"%ls\\%ls", folder, d.cFileName);
      look_in(r, inner, pattern, deep, out);
    }
  } while (FindNextFileW(h, &d));
  FindClose(h);
}

static val *listing(run *r, val **a, int deep) {
  val *p = path_of(r, a[0]), *out = v_list(r->a);
  DWORD at = attributes(p);
  if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) fail(r, "phrase", "there is no folder %s", p->t.s);
  wchar_t *folder = win_wide(p->t.s), *pattern = win_wide(a[1]->k == V_NOTHING ? "*" : str(r, a[1]));
  look_in(r, folder, pattern, deep, out);
  free(folder);
  free(pattern);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  return out;
}

static val *fs_list(run *r, val **a) { return listing(r, a, 0); }
static val *fs_find(run *r, val **a) { return listing(r, a, 1); }

// Into to if it is a folder, otherwise as to, with the folders above it made.
static void copy_or_move(run *r, val **a, int move) {
  val *from = path_of(r, a[0]), *to = path_of(r, a[1]);
  if (attributes(from) == INVALID_FILE_ATTRIBUTES) fail(r, "phrase", "there is nothing at %s", from->t.s);
  DWORD at = attributes(to);
  if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
    const char *name = strrchr(from->t.s, '\\');
    buf b = { 0 };
    buf_fmt(&b, "%s%s", to->t.s, name ? name : "\\");
    to = v_text(r->a, b.s, b.n);
    buf_free(&b);
  }
  parent_folder(r, to);
  wchar_t *wa = win_wide(from->t.s), *wb = win_wide(to->t.s);
  BOOL ok = move ? MoveFileExW(wa, wb, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) : CopyFileW(wa, wb, FALSE);
  free(wa);
  free(wb);
  if (!ok) win_fail(r, move ? "move" : "copy", from->t.s);
}

static val *fs_copy(run *r, val **a) { copy_or_move(r, a, 0); return NOTHING; }
static val *fs_move(run *r, val **a) { copy_or_move(r, a, 1); return NOTHING; }

// To the Recycle Bin, so the person can always get it back.
static val *fs_delete(run *r, val **a) {
  val *p = path_of(r, a[0]);
  if (attributes(p) == INVALID_FILE_ATTRIBUTES) fail(r, "phrase", "there is nothing at %s", p->t.s);
  wchar_t *w = win_wide(p->t.s);
  size_t n = wcslen(w);
  wchar_t *two = calloc(n + 2, sizeof *two);   // the list of paths ends in two zeros
  memcpy(two, w, n * sizeof *w);
  SHFILEOPSTRUCTW op = { 0 };
  op.wFunc = FO_DELETE;
  op.pFrom = two;
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  int failed = SHFileOperationW(&op) || op.fAnyOperationsAborted;
  free(two);
  free(w);
  if (failed) fail(r, "phrase", "Windows couldn't move %s to the Recycle Bin", p->t.s);
  return NOTHING;
}

static val *fs_folder(run *r, val **a) {
  val *p = path_of(r, a[0]);
  wchar_t *w = win_wide(p->t.s);
  int e = SHCreateDirectoryExW(NULL, w, NULL);
  free(w);
  if (e != ERROR_SUCCESS && e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) fail(r, "phrase", "can't make the folder %s", p->t.s);
  return NOTHING;
}

////////// 7. CLIPBOARD, COMMANDS, NOTIFY //////////

// Another program can hold the clipboard for a moment, so opening it is tried a few times.
static void open_clipboard(run *r) {
  for (int i = 0; !OpenClipboard(NULL); i++) {
    if (i == 20) fail(r, "phrase", "another program is holding the clipboard");
    nap(r, 0.05);
  }
}

static val *clip_get(run *r, val **a) {
  open_clipboard(r);
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  wchar_t *w = h ? GlobalLock(h) : NULL;
  val *out = w ? wtext(r, w, -1) : v_cstr(r->a, "");
  if (w) GlobalUnlock(h);
  CloseClipboard();
  return out;
}

static val *clip_set(run *r, val **a) {
  const char *t = str(r, a[0]);
  wchar_t *w = win_wide(*t ? t : " ");
  size_t size = (wcslen(w) + 1) * sizeof *w;
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
  memcpy(GlobalLock(g), w, size);
  GlobalUnlock(g);
  free(w);
  open_clipboard(r);
  EmptyClipboard();
  if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
  CloseClipboard();
  return NOTHING;
}

static val *shell_run(run *r, val **a) {
  buf out = { 0 };
  int code = powershell(r, str(r, a[0]), 1, 600, &out);
  val *rec = v_rec(r->a);
  put_num(r, rec, "code", code);
  put(r, rec, "out", v_text(r->a, out.s ? out.s : "", out.n));
  buf_free(&out);
  return rec;
}

static val *shell_processes(run *r, val **a) {
  val *out = v_list(r->a);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W p = { .dwSize = sizeof p };
  for (BOOL more = Process32FirstW(snap, &p); more; more = Process32NextW(snap, &p)) {
    wchar_t *dot = wcsrchr(p.szExeFile, L'.');
    if (dot && !_wcsicmp(dot, L".exe")) *dot = 0;
    double mb = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.th32ProcessID);
    PROCESS_MEMORY_COUNTERS mem = { .cb = sizeof mem };
    if (h && K32GetProcessMemoryInfo(h, &mem, sizeof mem)) mb = floor(mem.WorkingSetSize / 1048576.0 * 10 + 0.5) / 10;
    if (h) CloseHandle(h);
    val *one = v_rec(r->a);
    put_num(r, one, "pid", p.th32ProcessID);
    put(r, one, "name", wtext(r, p.szExeFile, -1));
    put_num(r, one, "memory", mb);
    list_push(r->a, out, one);
  }
  CloseHandle(snap);
  return out;
}

static val *shell_stop(run *r, val **a) {
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)num_of(r, a[0], NULL));
  if (!h || !TerminateProcess(h, 1)) { if (h) CloseHandle(h); fail(r, "phrase", "process %g couldn't be stopped", a[0]->n); }
  CloseHandle(h);
  return NOTHING;
}

// A notification from the tray, which Windows shows as a toast. It shows on its own, so the program goes on.
static val *note_toast(run *r, val **a) {
  buf s = { 0 }, none = { 0 };
  buf_str(&s, "Add-Type -AssemblyName System.Windows.Forms,System.Drawing\n$i=New-Object System.Windows.Forms.NotifyIcon\n"
              "$i.Icon=[System.Drawing.SystemIcons]::Information;$i.Text='Onsay';$i.Visible=$true\n$i.ShowBalloonTip(6000,");
  ps_quote(&s, str(r, a[0]));
  buf_chr(&s, ',');
  ps_quote(&s, *str(r, a[1]) ? str(r, a[1]) : " ");
  buf_str(&s, ",'Info')\nStart-Sleep 7\n$i.Dispose()");
  powershell(r, s.s, 0, 0, &none);
  buf_free(&s);
  return NOTHING;
}

////////// 8. INPUT //////////

// window is one from window.list or window.wait_for, or nothing for the whole screen. Onsay's own pointer does the clicking,
// and input.cursor says whether the real mouse is used.

// The window given, or the front one that isn't Onsay's own chat.
static val *front(run *r, val *window) {
  val *w = window->k == V_NOTHING ? win_front(r, NULL) : window;
  if (w == NOTHING) fail(r, "phrase", "no window is open");
  return w;
}

// Where want, lowered, first is in text, lowered, in characters, or -1. *n is how many characters text has.
static long words_at(run *r, val *text, const wchar_t *want, size_t *n) {
  wchar_t *t = lowered(r, text), *found = wcsstr(t, want);
  long at = found ? (long)(found - t) : -1;
  *n = wcslen(t);
  free(t);
  return at;
}

// The box of the nth place the words show on screen, by OCR. OCR gives whole lines, so the box is cut from its line by
// how much of the line comes before the words. 0 when they aren't there, or with must a failure.
static int spot(run *r, val *words, val *window, double nth, int must, box *out) {
  val *lines = screen_read(r, window);
  wchar_t *want = lowered(r, words);
  size_t wn = wcslen(want), n;
  int seen = 0;
  for (size_t i = 0; lines->k == V_LIST && i < lines->l.n; i++) {
    long k = words_at(r, part(lines->l.v[i], "text"), want, &n);
    if (k < 0 || seen++ != nth) continue;
    box line = box_of(r, lines->l.v[i]);
    double w = n ? floor(line.w * wn / n + 0.5) : 0;
    *out = (box){ line.x + (n ? floor(line.w * k / n + 0.5) : 0), line.y, w > 8 ? w : 8, line.h, line.text };
    free(want);
    return 1;
  }
  free(want);
  if (must) fail(r, "phrase", "these words aren't on the screen: %s", str(r, words));
  return 0;
}

// The first control in the window matching {name,role,value}, or a failure that says what was looked for.
static val *control(run *r, val *window, val *match) {
  val *found = find(r, hwnd_of(r, window, "window"), match, 1);
  if (found->l.n) return found->l.v[0];
  buf m = { 0 };
  buf_json(&m, match);
  const char *said = v_text(r->a, m.s, m.n)->t.s;
  buf_free(&m);
  fail(r, "phrase", "no control in that window matches %s", said);
}

static void ring_click(run *r, box b) {
  ring(r, b, NOTHING, 2);
  click(r, mid(b.x, b.w), mid(b.y, b.h), 0, 1);
}

static val *input_click_text(run *r, val **a) {
  box b;
  spot(r, a[0], a[1], num_of(r, a[2], "nth"), 1, &b);
  ring_click(r, b);
  return box_val(r, b, 0);
}

static val *input_click_on(run *r, val **a) {
  val *c = control(r, a[0], a[1]);
  ring_click(r, box_of(r, c));
  return c;
}

static val *input_type_into(run *r, val **a) {
  input_click_on(r, a);
  press_keys(r, "ctrl+a");
  type_text(r, str(r, a[2]));
  return NOTHING;
}

// Through the clipboard, which is given back what it had.
static val *input_paste(run *r, val **a) {
  val *was = clip_get(r, a);
  clip_set(r, a);
  press_keys(r, "ctrl+v");
  nap(r, 0.2);
  clip_set(r, &was);
  return NOTHING;
}

static val *input_shortcut(run *r, val **a) {
  const char *combo = str(r, a[0]);
  for (int i = 1, times = (int)num_of(r, a[1], "times"); i <= times; i++) {
    press_keys(r, combo);
    if (i < times) nap(r, num_of(r, a[2], "gap"));
  }
  return NOTHING;
}

static val *input_type_lines(run *r, val **a) {
  if (a[0]->k != V_LIST) fail(r, "phrase", "lines is a list of text");
  for (size_t i = 0; i < a[0]->l.n; i++) { type_text(r, str(r, a[0]->l.v[i])); press_keys(r, "enter"); }
  return NOTHING;
}

static val *input_type(run *r, val **a) {
  type_text(r, str(r, a[0]));
  return NOTHING;
}

static val *input_scroll_to_text(run *r, val **a) {
  box b;
  for (int i = 1, tries = (int)num_of(r, a[2], "tries"); i <= tries; i++) {
    if (spot(r, a[0], a[1], 0, 0, &b)) return box_val(r, b, 0);
    box w = box_of(r, front(r, a[1]));
    wheel(r, mid(w.x, w.w), mid(w.y, w.h), -3);
    nap(r, 0.4);
  }
  return spot(r, a[0], a[1], 0, 0, &b) ? box_val(r, b, 0) : NOTHING;
}

static val *input_wait_for_text(run *r, val **a) {
  box b;
  for (double until = pl_now() + num_of(r, a[2], "seconds");; nap(r, 0.5)) {
    if (spot(r, a[0], a[1], 0, 0, &b)) return box_val(r, b, 0);
    if (pl_now() >= until) return NOTHING;
  }
}

static val *input_copy_all(run *r, val **a) {
  win_focus(r, a);
  nap(r, 0.2);
  press_keys(r, "ctrl+a");
  press_keys(r, "ctrl+c");
  nap(r, 0.3);
  return clip_get(r, a);
}

static val *input_point_text(run *r, val **a) {
  box b;
  spot(r, a[0], a[2], 0, 1, &b);
  ring(r, b, a[1], num_or(a[3], 10));
  return box_val(r, b, 1);
}

static val *input_point_on(run *r, val **a) {
  val *c = control(r, a[0], a[1]);
  ring(r, box_of(r, c), a[2], num_or(a[3], 10));
  return c;
}

// The window as it is now, looked up again each time so a place in it stays put. window may be a match like {app}, or
// nothing for the main screen.
static val *now(run *r, val *window) {
  if (window->k == V_NOTHING) {
    val *screen = v_rec(r->a);
    put_num(r, screen, "x", 0), put_num(r, screen, "y", 0);
    put_num(r, screen, "w", GetSystemMetrics(SM_CXSCREEN)), put_num(r, screen, "h", GetSystemMetrics(SM_CYSCREEN));
    return screen;
  }
  if ((window->k == V_REC ? part(window, "id") : window)->k == V_NOTHING) return win_wait(r, (val *[]){ window, v_num(r->a, 5) });
  return window_rec(r, hwnd_of(r, window, "window"));
}

// The window as it is now, restored and in front, ready to be pressed. The screen as it is.
static val *ready(run *r, val *window) {
  val *w = now(r, window);
  if (window->k == V_NOTHING) return w;
  HWND h = hwnd_of(r, w, "window");
  if (IsIconic(h)) { ShowWindow(h, SW_RESTORE); nap(r, 0.25); w = window_rec(r, h); }
  if (h != GetForegroundWindow()) { win_focus(r, &w); nap(r, 0.15); }
  return w;
}

// x, y in a window to the screen. Between -1 and 1 they are shares of its size; below 0 they count back from the far edge.
static double along(run *r, val *v, double size) {
  double n = num_of(r, v, NULL);
  if (n > -1 && n < 1 && n != 0) n = n < 0 ? size * (1 + n) : size * n;
  else if (n < 0) n += size;
  return floor(n + 0.5);
}

static POINT place(run *r, val *window, val *x, val *y) {
  box w = box_of(r, window);
  return (POINT){ (LONG)(w.x + along(r, x, w.w)), (LONG)(w.y + along(r, y, w.h)) };
}

static val *point_val(run *r, POINT p) {
  val *out = v_rec(r->a);
  put_num(r, out, "x", p.x);
  put_num(r, out, "y", p.y);
  return out;
}

static val *input_click_in(run *r, val **a) {
  const char *how = str(r, a[3]);
  int right = !strcmp(how, "right"), times = !strcmp(how, "double") ? 2 : 1;
  if (!right && times == 1 && strcmp(how, "left")) fail(r, "phrase", "how is left, double or right, not %s", how);
  POINT p = place(r, ready(r, a[0]), a[1], a[2]);
  click(r, p.x, p.y, right, times);
  return point_val(r, p);
}

static val *input_move_in(run *r, val **a) {
  POINT p = place(r, ready(r, a[0]), a[1], a[2]);
  glide(r, p.x, p.y, num_or(a[3], 0));
  return point_val(r, p);
}

static val *input_drag_in(run *r, val **a) {
  val *w = ready(r, a[0]);
  POINT from = place(r, w, a[1], a[2]), to = place(r, w, a[3], a[4]);
  drag(r, from.x, from.y, to.x, to.y);
  return NOTHING;
}

static val *input_scroll_in(run *r, val **a) {
  POINT p = place(r, ready(r, a[0]), a[2], a[3]);
  wheel(r, p.x, p.y, at(r, a[1]));
  return point_val(r, p);
}

static val *input_where_in(run *r, val **a) { return point_val(r, place(r, now(r, a[0]), a[1], a[2])); }

////////// 9. LOOK //////////

// Given no window, controls are the front window's, and words the whole screen's.

static const char *const UNMARKED[] = { "pane", "group", "window", "document", "table", "list", "menu", "text", "tree", "tool bar",
                                        "status bar", "menu bar", "title bar", "scroll bar", NULL };
static const char *const PRESSABLE[] = { "button", "link", "tab", "menu item", "checkbox", "radio", "list item", "tree item", NULL };
static const char *const TYPEABLE[] = { "field", "dropdown", "document", NULL };

static int role_is(val *c, const char *const *roles) {
  val *role = part(c, "role");
  for (size_t i = 0; role->k == V_TEXT && roles[i]; i++) if (!strcmp(role->t.s, roles[i])) return 1;
  return 0;
}

// A window's controls, asked for once and read many ways, since asking Windows is the slow part.
static val *controls_of(run *r, val *window) { return find(r, hwnd_of(r, front(r, window), "window"), NULL, 400); }

static val *focus_in(val *all) {
  for (size_t i = 0; i < all->l.n; i++) if (truthy(part(all->l.v[i], "focused"))) return all->l.v[i];
  return NOTHING;
}

// Up to most that can be pressed, by name, or with typing every one that can be typed in, with what is in it now. Each
// with its middle, to click.
static val *able(run *r, val *all, int typing, double most) {
  val *out = v_list(r->a);
  for (size_t i = 0; i < all->l.n && out->l.n < most; i++) {
    val *c = all->l.v[i];
    if (!role_is(c, typing ? TYPEABLE : PRESSABLE) || (!typing && (!truthy(part(c, "enabled")) || !*str(r, part(c, "name"))))) continue;
    val *one = v_rec(r->a);
    box b = box_of(r, c);
    put(r, one, "name", part(c, "name"));
    put(r, one, "role", part(c, "role"));
    if (typing) put(r, one, "value", part(c, "value"));
    put_num(r, one, "x", mid(b.x, b.w));
    put_num(r, one, "y", mid(b.y, b.h));
    list_push(r->a, out, one);
  }
  return out;
}

static val *names(run *r, val *list) {
  val *out = v_list(r->a);
  for (size_t i = 0; i < list->l.n; i++) list_push(r->a, out, part(list->l.v[i], *str(r, part(list->l.v[i], "name")) ? "name" : "role"));
  return out;
}

// The lines of OCR with the words in them.
static val *lines_with(run *r, val *words, val *lines) {
  val *out = v_list(r->a);
  wchar_t *want = lowered(r, words);
  size_t n;
  for (size_t i = 0; lines->k == V_LIST && i < lines->l.n; i++) if (words_at(r, part(lines->l.v[i], "text"), want, &n) >= 0) list_push(r->a, out, lines->l.v[i]);
  free(want);
  return out;
}

// Squared distance between two boxes' nearest edges, 0 when they touch. Squared is enough to compare.
static double gap(box a, box b) {
  double dx = fmax(0, fmax(a.x - (b.x + b.w), b.x - (a.x + a.w))), dy = fmax(0, fmax(a.y - (b.y + b.h), b.y - (a.y + a.h)));
  return dx * dx + dy * dy;
}

/* marked(): everything on screen that can be told apart, numbered in reading order: the window's controls, then OCR
   lines that aren't part of one. Places are in the window's pixels (for input.click_in), or the screen's with no window.
   At most 120 controls, then text up to 170 in all. A mark's box holds its name. */
typedef struct { const char *kind; box b; double order; } mark;

// What a control is, for marking, or NULL for containers, bars and plain text (OCR reads text better). A nameless
// button or image is an icon, marked anyway for a model that can see.
static const char *kind_of(run *r, val *c) {
  if (role_is(c, UNMARKED)) return NULL;
  const char *role = str(r, part(c, "role"));
  return (!strcmp(role, "button") || !strcmp(role, "image")) && !trimmed(r, part(c, "name"))->t.len ? "icon" : role;
}

// Which mark a line of text belongs to: a nameless one it sits in, or one whose name says the same. -1 when none.
static long owner(run *r, mark *found, size_t n, box t) {
  double cx = t.x + t.w / 2, cy = t.y + t.h / 2;
  wchar_t *b = lowered(r, trimmed(r, t.text));
  long out = -1;
  for (size_t i = 0; i < n && out < 0; i++) {
    box c = found[i].b;
    if (cx < c.x || cy < c.y || cx > c.x + c.w || cy > c.y + c.h) continue;
    wchar_t *a = lowered(r, trimmed(r, c.text));
    if (!*a || wcsstr(a, b) || wcsstr(b, a)) out = (long)i;
    free(a);
  }
  free(b);
  return out;
}

// In reading order, the first seen first where two tie.
static void by_order(mark *m, size_t n) {
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j > 0 && m[j - 1].order > m[j].order; j--) { mark t = m[j]; m[j] = m[j - 1]; m[j - 1] = t; }
}

static val *marked(run *r, val *window, val *area) {
  int whole = window->k == V_NOTHING;
  val *w = front(r, window), *controls = find(r, hwnd_of(r, w, "window"), NULL, 400), *lines = screen_read(r, window);
  box wb = box_of(r, w);
  double ox = whole ? 0 : wb.x, oy = whole ? 0 : wb.y;
  size_t n = 0, count = lines->k == V_LIST ? lines->l.n : 0;
  mark *found = arena_get(r->a, (controls->l.n + count + 1) * sizeof *found), *texts = arena_get(r->a, (controls->l.n + count + 1) * sizeof *texts);
  for (size_t i = 0; i < controls->l.n; i++) {
    val *c = controls->l.v[i];
    const char *kind = kind_of(r, c);
    box b = box_of(r, c);
    if (!kind || b.w < 3 || b.h < 3 || b.w * b.h > wb.w * wb.h * 0.4) continue;
    found[n++] = (mark){ kind, { b.x - ox, b.y - oy, b.w, b.h, part(c, "name") }, 0 };
  }
  for (size_t i = 0; i < count; i++) {
    box b = box_of(r, lines->l.v[i]);
    b.x -= ox, b.y -= oy;
    long k = owner(r, found, n, b);
    if (k < 0) found[n++] = (mark){ "text", b, 0 };
    else if (!trimmed(r, found[k].b.text)->t.len) {
      found[k].b.text = b.text;
      if (!strcmp(found[k].kind, "icon")) found[k].kind = "button";
    }
  }
  // Cut to the window or box. Controls come first, text fills what is left.
  box cut = area->k != V_NOTHING ? box_of(r, area) : whole ? (box){ -100000, -100000, 200000, 200000, NOTHING } : (box){ 0, 0, wb.w, wb.h, NOTHING };
  size_t kept = 0, read = 0;
  for (size_t i = 0; i < n; i++) {
    box e = found[i].b;
    double x1 = fmax(e.x, cut.x), y1 = fmax(e.y, cut.y), x2 = fmin(e.x + e.w, cut.x + cut.w), y2 = fmin(e.y + e.h, cut.y + cut.h);
    if (x2 - x1 < 3 || y2 - y1 < 3) continue;
    mark m = { found[i].kind, { x1, y1, x2 - x1, y2 - y1, e.text }, floor(y1 / 12 + 0.5) * 100000 + x1 };
    val *t = trimmed(r, e.text);
    if (strcmp(m.kind, "text")) found[kept++] = m;
    else if (utf8_count(t->t.s, t->t.len) > 1) texts[read++] = m;
  }
  by_order(found, kept);
  if (kept > 120) kept = 120;
  by_order(texts, read);
  for (size_t i = 0; i < read && kept < 170; i++) found[kept++] = texts[i];
  by_order(found, kept);
  val *out = v_list(r->a);
  for (size_t i = 0; i < kept; i++) {
    val *one = v_rec(r->a);
    put_num(r, one, "id", (double)(i + 1));
    put(r, one, "kind", v_cstr(r->a, found[i].kind));
    put(r, one, "name", found[i].b.text);
    put_num(r, one, "x", found[i].b.x);
    put_num(r, one, "y", found[i].b.y);
    put_num(r, one, "w", found[i].b.w);
    put_num(r, one, "h", found[i].b.h);
    list_push(r->a, out, one);
  }
  return out;
}

static val *look_elements(run *r, val **a) { return marked(r, a[0], a[1]); }

static val *look_overview(run *r, val **a) {
  val *w = front(r, a[0]), *all = controls_of(r, w), *f = focus_in(all), *out = v_rec(r->a), *focused = NOTHING;
  if (f != NOTHING) {
    focused = v_rec(r->a);
    put(r, focused, "name", part(f, "name"));
    put(r, focused, "role", part(f, "role"));
    put(r, focused, "value", part(f, "value"));
  }
  put(r, out, "app", part(w, "app"));
  put(r, out, "title", part(w, "title"));
  put(r, out, "focused", focused);
  put(r, out, "pressable", names(r, able(r, all, 0, 40)));
  put(r, out, "fields", names(r, able(r, all, 1, INFINITY)));
  return out;
}

static val *look_focused(run *r, val **a) { return focus_in(controls_of(r, a[0])); }

static val *look_lines(run *r, val **a) {
  val *lines = screen_read(r, a[0]), *out = v_list(r->a);
  for (size_t i = 0; lines->k == V_LIST && i < lines->l.n; i++) list_push(r->a, out, part(lines->l.v[i], "text"));
  return out;
}

// Its own text, or what is read off the screen when it gives none.
static val *look_page(run *r, val **a) {
  val *t = trimmed(r, ui_text(r, (val *[]){ front(r, a[0]) }));
  if (t->t.len) return t;
  val *lines = look_lines(r, a);
  buf b = { 0 };
  for (size_t i = 0; i < lines->l.n; i++) { if (i) buf_chr(&b, '\n'); buf_str(&b, str(r, lines->l.v[i])); }
  t = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return t;
}

static val *look_pressable(run *r, val **a) { return able(r, controls_of(r, a[0]), 0, num_of(r, a[1], "most")); }
static val *look_fields(run *r, val **a) { return able(r, controls_of(r, a[0]), 1, INFINITY); }

// The other lines within distance of the first with the words in it, closest first, the first seen first where two tie.
static val *look_near(run *r, val **a) {
  val *all = screen_read(r, a[1]), *with = lines_with(r, a[0], all), *out = v_list(r->a);
  if (!with->l.n) return out;
  double most = num_of(r, a[2], "distance");
  box here = box_of(r, with->l.v[0]);
  struct { double d; val *text; } *nearby = arena_get(r->a, (all->l.n + 1) * sizeof *nearby);
  size_t k = 0;
  for (size_t i = 0; i < all->l.n; i++) {
    double d = gap(here, box_of(r, all->l.v[i]));
    if (v_same(all->l.v[i], with->l.v[0]) || d > most * most) continue;
    size_t j = k++;
    for (; j > 0 && nearby[j - 1].d > d; j--) nearby[j] = nearby[j - 1];
    nearby[j].d = d, nearby[j].text = part(all->l.v[i], "text");
  }
  for (size_t i = 0; i < k; i++) list_push(r->a, out, nearby[i].text);
  return out;
}

static val *look_locate(run *r, val **a) { return lines_with(r, a[0], screen_read(r, a[1])); }

static val *look_open_windows(run *r, val **a) {
  val *all = win_list(r, a), *out = v_list(r->a);
  for (size_t i = 0; i < all->l.n; i++) {
    val *w = all->l.v[i], *one = v_rec(r->a);
    if (!*str(r, part(w, "title")) || truthy(part(w, "minimized"))) continue;
    put(r, one, "app", part(w, "app"));
    put(r, one, "title", part(w, "title"));
    list_push(r->a, out, one);
  }
  return out;
}

////////// 10. LINGO_PHRASES //////////

// What windows, controls and files are, written out where a phrase gives one.
#define WINDOW "{id,app,title,x,y,w,h,focused,minimized}"
#define CONTROL "{id,role,name,value,x,y,w,h,enabled,focused,can}"
#define FILE_ "{name,path,size,folder,changed}"

LINGO_ENTRY void lingo_phrases(void) {
  // Every position is a real pixel, on every monitor, whatever its scale.
  typedef BOOL(WINAPI * aware_fn)(HANDLE);
  aware_fn aware = (aware_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
  if (aware) aware((HANDLE)-4);   // per monitor aware, version 2

  phrase_add("window", "screen.read", "list() -> [" WINDOW "]: front to back", win_list);
  phrase_add("window", "screen.read", "current() -> " WINDOW ": the one in front, never Onsay's own chat", win_front);
  phrase_add("window", "screen.act", "open(target) -> " WINDOW "?: an app, file, folder or web address", win_open);
  phrase_add("window", "screen.read", "wait_for(match, seconds=10) -> " WINDOW ": match {app} or {title}, part of the text is enough", win_wait);
  phrase_add("window", "screen.act", "focus(id)", win_focus);
  phrase_add("window", "screen.act", "close(id)", win_close);
  phrase_add("window", "screen.act", "resize(id, how): how is min, max or normal", win_size);
  phrase_add("window", "screen.act", "move(id, x=nothing, y=nothing, w=nothing, h=nothing)", win_move);

  phrase_add("controls", "screen.read", "find(win, match) -> [" CONTROL "]: match {name,role,value}, part of the text is enough", ui_find);
  phrase_add("controls", "screen.read", "list(win, max=80) -> [" CONTROL "]", ui_all);
  phrase_add("controls", "screen.read", "read(win) -> text: every word of a page or document", ui_text);
  phrase_add("controls", "screen.act", "press(control)", ui_press);
  phrase_add("controls", "screen.act", "fill(control, text): replaces its text", ui_fill);
  phrase_add("controls", "screen.act", "toggle(control)", ui_toggle);
  phrase_add("controls", "screen.act", "pick(control, option): a dropdown or list option, by name", ui_choose);
  phrase_add("controls", "screen.act", "scroll(control, amount): positive is up", ui_scroll);
  phrase_add("controls", "screen.read", "wait_for(win, match, seconds=10) -> " CONTROL, ui_wait);

  phrase_add("files", "files.read", "read(path) -> text", fs_read);
  phrase_add("files", "files.write", "write(path, text): replaces it", fs_write);
  phrase_add("files", "files.write", "add_to(path, text)", fs_append);
  phrase_add("files", "files.read", "exists(path) -> bool", fs_exists);
  phrase_add("files", "files.read", "list(folder, pattern=\"*\") -> [" FILE_ "]", fs_list);
  phrase_add("files", "files.read", "find(folder, pattern=\"*\") -> [" FILE_ "]: every folder inside too", fs_find);
  phrase_add("files", "files.write", "copy(from, to): into to if it is a folder", fs_copy);
  phrase_add("files", "files.write", "move(from, to): into to if it is a folder", fs_move);
  phrase_add("files", "files.write", "delete(path): to the Recycle Bin", fs_delete);
  phrase_add("files", "files.write", "make_folder(path): makes it", fs_folder);

  phrase_add("clipboard", "screen.read", "read() -> text", clip_get);
  phrase_add("clipboard", "screen.act", "write(text)", clip_set);

  phrase_add("commands", "run", "run(command) -> {code,out}: Windows PowerShell", shell_run);
  phrase_add("commands", "none", "running() -> [{pid,name,memory}]: memory in MB", shell_processes);
  phrase_add("commands", "run", "stop(pid)", shell_stop);

  phrase_add("notify", "none", "show(title, text)", note_toast);

  phrase_add("panel", "none", "show(html, title=\"\", width=480, height=360): draws a small rounded window of Onsay's own on screen, beside him, "
             "showing html: a page, a table, a chart, a picture or an answer laid out. It stays until hidden or the person closes it, and "
             "showing again replaces it. Scripts in it run, with no reach to this computer", panel_show);
  phrase_add("panel", "none", "hide(): takes the window away", panel_hide);

  // input: the mouse and keyboard the way a person uses them. A window, where one is taken, may be nothing for the main
  // screen.
  phrase_add("input", "none", "cursor(how=nothing) -> text: how the pointer acts: virtual, the default, never touches the user's mouse, "
             "and its clicks may be ignored by browsers, games and some apps. real moves and clicks the real mouse and works everywhere. "
             "Given how, it changes until changed again", mouse_mode);
  phrase_add("input", "screen.act, screen.read", "click_text(words, window=nothing, nth=0) -> {x,y,text}: rings and clicks those words where "
             "they are on screen, read by OCR. nth picks a later one when they show more than once. Fails when they aren't there", input_click_text);
  phrase_add("input", "screen.act, screen.read", "click_on(window, match) -> " CONTROL ": rings and clicks the first control in the window "
             "matching {name,role,value}", input_click_on);
  phrase_add("input", "screen.act, screen.read", "type_into(window, match, text): clicks the control matching {name,role,value}, clears it and "
             "types text", input_type_into);
  phrase_add("input", "screen.act", "type(text): types it where the cursor is, a new line as enter", input_type);
  phrase_add("input", "screen.act, screen.read", "paste(text): puts text where the cursor is through the clipboard, fast for long text, then "
             "gives the clipboard back what it had", input_paste);
  phrase_add("input", "screen.act", "shortcut(combo, times=1, gap=0.1): presses a combo like \"ctrl+s\", times over, gap seconds apart", input_shortcut);
  phrase_add("input", "screen.act", "type_lines(lines): types each line of a list, pressing enter after each", input_type_lines);
  phrase_add("input", "screen.act, screen.read", "scroll_to_text(words, window=nothing, tries=10) -> {x,y,text}?: scrolls down until those "
             "words are on screen and gives where they are, or nothing", input_scroll_to_text);
  phrase_add("input", "screen.read", "wait_for_text(words, window=nothing, seconds=10) -> {x,y,text}?: waits until those words show on screen "
             "and gives where they are, or nothing when time runs out", input_wait_for_text);
  phrase_add("input", "screen.act, screen.read", "copy_all(window) -> text: brings the window to the front, selects everything in it and copies "
             "it, and gives the text", input_copy_all);
  phrase_add("input", "screen.read", "point_text(words, say=\"\", window=nothing, seconds=10) -> {x,y,w,h,text}: rings those words on screen "
             "and has Onsay beside them say say, to show the user something. Clicks nothing", input_point_text);
  phrase_add("input", "screen.read", "point_on(window, match, say=\"\", seconds=10) -> " CONTROL ": rings the control matching "
             "{name,role,value} and has Onsay beside it say say. Clicks nothing", input_point_on);
  phrase_add("input", "none", "point_at(x, y, w, h, say=\"\", seconds=10): rings that box on screen, in pixels, with Onsay beside it saying say",
             input_point_at);
  phrase_add("input", "none", "stop_pointing(): takes the ring away", show_clear);
  phrase_add("input", "screen.act, screen.read", "click_in(window, x, y, how=\"left\") -> {x,y}: clicks a place in a window, wherever the window "
             "is now: x, y from its top left in pixels, or between -1 and 1 a share of its size, 0.5 the middle, and below 0 back from its "
             "right or bottom. how is left, double or right. window may be a match like {app}, brought to the front first", input_click_in);
  phrase_add("input", "screen.act, screen.read", "move_in(window, x, y, seconds=0) -> {x,y}: glides the pointer to a place in a window, the "
             "same way as click_in, at a hand's pace or over seconds", input_move_in);
  phrase_add("input", "screen.act, screen.read", "drag_in(window, x1, y1, x2, y2): drags from one place in a window to another, the same way as "
             "click_in", input_drag_in);
  phrase_add("input", "screen.act, screen.read", "scroll_in(window, amount, x=0.5, y=0.5) -> {x,y}: scrolls at a place in a window, the middle "
             "unless given, positive up", input_scroll_in);
  phrase_add("input", "screen.read", "position_in(window, x, y) -> {x,y}: the place on screen of a place in a window, without touching "
             "anything", input_where_in);

  // look: the screen read as text, and a picture of it. With no window, controls are the front window's, and words the
  // whole screen's.
  phrase_add("look", "screen.read", "screenshot(window=nothing, box=nothing) -> {path,x,y,w,h}: a picture of a window, or the main screen, "
             "saved as a PNG file. box {x,y,w,h} in the window's pixels keeps only that part", look_screenshot);
  phrase_add("look", "screen.read", "elements(window=nothing, box=nothing) -> [{id,kind,name,x,y,w,h}]: everything on screen that can be told "
             "apart, numbered: the controls Windows knows, buttons, links, boxes, tabs, checkboxes and icons, with their names, and every "
             "line of text OCR reads. Places in the window's pixels for input.click_in, or the screen's when no window is given", look_elements);
  phrase_add("look", "screen.read", "overview(window=nothing) -> {app,title,focused,pressable,fields}: a window in brief: what it is, what has "
             "the keyboard, and the names of what can be pressed and typed in. The front window when none is given", look_overview);
  phrase_add("look", "screen.read", "focused(window=nothing) -> " CONTROL "?: the control that has the keyboard now", look_focused);
  phrase_add("look", "screen.read", "page(window=nothing) -> text: every word of a window, from its own text, or read off the screen when it "
             "gives none", look_page);
  phrase_add("look", "screen.read", "lines(window=nothing) -> [text]: the lines of text on screen, top to bottom, read by OCR. The whole screen "
             "when no window is given", look_lines);
  phrase_add("look", "screen.read", "buttons(window=nothing, most=60) -> [{name,role,x,y}]: the buttons, links, tabs, menu items and boxes to "
             "tick, by name, with where to click them", look_pressable);
  phrase_add("look", "screen.read", "fields(window=nothing) -> [{name,role,value,x,y}]: the boxes to type in and the dropdowns, with what is in "
             "them now", look_fields);
  phrase_add("look", "screen.read", "near(words, window=nothing, distance=150) -> [text]: the other lines on screen within distance pixels of "
             "those words, closest first: what a label belongs to, or the value beside it", look_near);
  phrase_add("look", "screen.read", "find_text(words, window=nothing) -> [{text,x,y,w,h}]: every line on screen with those words in it, and where "
             "it is", look_locate);
  phrase_add("look", "screen.read", "open_windows() -> [{app,title}]: the windows open now, front to back, in brief", look_open_windows);
}
