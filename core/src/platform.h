// platform.h - everything the core needs from a device: porting the core is writing these for it, in UTF-8 throughout.
//
// 1. Time      the clock and sleeping
// 2. Threads   starting one, and a gate: a lock that can also wait to be woken
// 3. Links     a WebSocket out, like the onsay lingo's to Onsay
// 4. Files     read, write, add to, delete and list, for the core's own storage and its log, and secrets sealed
// 5. Device    what is around it, chance, a native lingo's library, and web requests going out
// 6. Port      web requests coming in: a port to listen on, and each connection to it
// The engine (core.c) only uses time, the gate, files and the device. Threads and the port are for host.c, and links for
// native lingos, which may use all of it.
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
int pl_append(const char *path, const char *data, size_t n);  // adds to its end, making it when missing
int pl_delete(const char *path);
int pl_folder(const char *path);                              // makes it, and the folders above it, if missing
void pl_whole(const char *path, buf *out);                    // the path whole, from the folder it runs in
// Everything in a folder, one level: each name, whether it is a folder, its size, and when it last changed, in seconds.
void pl_list(const char *folder, void (*each)(const char *name, int folder, double size, double changed, void *arg), void *arg);
// A secret kept at path for this computer's user alone, like a private key: on Windows sealed by Windows (DPAPI), so it is
// never a plain file, and on Linux a file only its user can read. 0 when it couldn't be kept, or read back.
int pl_seal(const char *path, const void *data, size_t n);
int pl_unseal(const char *path, buf *out);

////////// 5. DEVICE //////////

// What was around a lingo(...) asked from here, as one JSON record of each it knows: "os", "time" its
// local time with its offset, like 2026-10-19T16:31:07-04:00, "zone", "language", and "app" and "window" in front.
void pl_context(buf *out);

void pl_random(void *to, size_t n);   // bytes no one can guess, for keys

// A native lingo's library, like notes.dll, loaded and kept: its lingo_phrases, or NULL with why in error.
lingo_entry pl_library(const char *path, buf *error);

// One web request, for http.send: method a word in capitals, url whole with no #, headers a record of text, each a whole
// line. Its answer, {status, headers, text}, headers a record by lower case name, redirects followed, or NULL with why in
// error, in words. An answer over 4 MB, or not text, is an error too.
val *pl_http(arena *a, const char *method, const char *url, val *headers, const char *body, size_t n, double seconds, buf *error);

////////// 6. PORT //////////

// A port this computer listens on, on every address it has when everywhere, else on 127.0.0.1 alone, and each connection
// to it, both a pl_socket. Each connection is read on its own thread.
typedef struct pl_socket pl_socket;
pl_socket *pl_listen(int port, int everywhere, buf *error);
pl_socket *pl_accept(pl_socket *port, buf *from);          // the next connection, and the address it is from, like
                                                           // 127.0.0.1, or NULL when none came within a second
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
