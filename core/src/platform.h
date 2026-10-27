// platform.h - everything the core needs from a device: porting the core is writing these for it, in UTF-8 throughout.
//
// 1. Time      the clock, sleeping, and a stop asked from outside
// 2. Threads   starting one, and a gate: a lock that can also wait to be woken
// 3. Links     a WebSocket to Onsay
// 4. Files     read, write, delete and list, for the core's own storage
// 5. Device    what is around it, a native lingo's library, the host's phrases, and web requests
// 6. Port      web requests coming in: a port to listen on, and each connection to it
// The engine (core.c) only uses time, the gate, files and the device. Threads, links and the port are for host.c alone.
// Native lingos may use all of it.
#ifndef PLATFORM_H
#define PLATFORM_H

#include "lingo.h"
#ifdef _WIN32
#include <wchar.h>
#endif

LINGO_SHARED

////////// 1. TIME //////////

double pl_now(void);               // seconds since 1970, with fractions
void pl_sleep(double seconds);
// 1 when a host that can't reach the core mid-job (a worker) was asked to stop it. Polled every 1024 steps.
int pl_stopped(void);

////////// 2. THREADS //////////

void pl_thread(void (*fn)(void *), void *arg);

typedef struct pl_gate pl_gate;
pl_gate *pl_gate_new(void);
void pl_enter(pl_gate *g);
void pl_leave(pl_gate *g);
void pl_wait(pl_gate *g, double seconds);   // inside the gate: let go, wait to be woken or for the time, take it again
void pl_wake(pl_gate *g);

////////// 3. LINKS //////////

// Any thread may send, and any thread may close, which ends a receive waiting on another thread. Only free is final.
typedef struct pl_link pl_link;
pl_link *pl_connect(const char *url, buf *error);   // ws:// or wss://
int pl_send(pl_link *l, const char *text, size_t n);
int pl_receive(pl_link *l, buf *out);               // one whole message into out; 0 once the link is closed
void pl_close(pl_link *l);
void pl_free(pl_link *l);

////////// 4. FILES //////////

int pl_read(const char *path, buf *out);
int pl_write(const char *path, const char *data, size_t n);   // replaces the file whole, never half written
int pl_delete(const char *path);
int pl_folder(const char *path);                              // makes it, and the folders above it, if missing
// Everything in a folder, one level: each name, whether it is a folder, its size, and when it last changed, in seconds.
void pl_list(const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg);

////////// 5. DEVICE //////////

// What was around a lingo(...) asked from here, as one JSON record of each it knows: "os", "time" its
// local time with its offset, like 2026-10-19T16:31:07-04:00, "zone", "language", and "app" and "window" in front.
void pl_context(buf *out);

// A native lingo's library, like windows.dll, loaded and kept: its lingo_phrases, or NULL with why in error.
lingo_entry pl_library(const char *path, buf *error);

// The host: the program around the core, which runs a host lingo's phrases in its own language. pl_hosts is 1 when
// there is one. pl_call runs a phrase by id with its values as a JSON list, putting {"value"} or {"error", "code"} in out.
int pl_hosts(void);
int pl_call(const char *id, const char *values, size_t n, buf *out);

// http.send, answered as pl_call answers: its values [method, url, headers, body, timeout] as JSON, and {"value":
// {status, headers, text}} or {"error"} in out. Redirects followed, and an answer over 4 MB, or not text, is an error.
// A device makes the request itself (platform_win.c, platform_linux.c).
int pl_http(const char *values, size_t n, buf *out);

////////// 6. PORT //////////

// A port this computer listens on, and each connection to it, both a pl_socket. Each connection is read on its own thread.
typedef struct pl_socket pl_socket;
pl_socket *pl_listen(int port, int network, buf *error);   // network 0: this computer only (127.0.0.1), 1: every address
pl_socket *pl_accept(pl_socket *port, int *here);          // the next connection, here 1 when it is from this computer
int pl_take(pl_socket *c, char *to, size_t n);             // what came, up to n bytes. 0 once it closed or was quiet 10 seconds
int pl_give(pl_socket *c, const char *data, size_t n);     // all of it, or 0
void pl_hang_up(pl_socket *c);                              // closes it and frees it

#ifdef _WIN32
// UTF-8 to and from Windows' UTF-16, for Windows lingos. win_wide's text is freed with free.
wchar_t *win_wide(const char *s);
void win_narrow(buf *out, const wchar_t *w, int len);   // len < 0: up to its 0
#endif

LINGO_SHARED_END

#endif
