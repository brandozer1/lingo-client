// core.h - the core as an engine a host drives: core.c never starts a thread or holds a connection itself.
//
// 1. core_read(text, n)                   -> the settings file, read once. NULL when it isn't a JSON record
// 2. core_begin(serving)                  -> every lingo read and loaded. 0 when it can't start. serving: it answers
//                                            requests, so its native lingos that run are told (lingo_hooks in lingo.h)
// 3. core_look()                          -> every CORE_LOOK seconds: reads the lingos folder again if it changed. 1 when
//                                            a rule written in the policy lets in public or an ip, so the port opens to
//                                            other computers
// 4. core_serve(request, n, caller, out)  -> a web request, {id, method, path, headers, body, base64}, answered by the
//                                            lingo at its route, {id, status, headers, body, base64} in out. caller is
//                                            NULL for one to the port, its ip in it, or one lingo_hand was given
// 5. core_stop(id)                        -> stops the request running now when its id, as JSON, is id. From any thread
// 6. core_control(a, verb, values, why)   -> start, stop, the settings, the policy and call, for the command line
// 7. core_pack(folder, to)                -> a folder of a lingo's files made into its .lingo file
//    core_say(fmt)                        -> a line on stderr, the lingo client's log
//    zip_file, zip_make                   -> a file read out of a zip, and a zip made (zip.c)
// The host owns the threads and the port: the job queue, reading each web request, and lingo_hand, lingo_stop,
// lingo_wait and lingo_done for native lingos (host.c).
#ifndef CORE_H
#define CORE_H

#include "lingo.h"

#define CORE_VERSION "0.10.0"
#define CORE_LOOK 5   // seconds between looks at the lingos folder

// Where Onsay is, built in: its API, which the onsay lingo talks to, and its resolve endpoint, the one address the lingo
// client asks where an Onsay name goes (../../docs/lingo.md, section 15). The test build asks test/server.mjs.
#ifdef TEST
#define ONSAY "http://127.0.0.1:8787"
#else
#define ONSAY "https://api.onsay.dev"
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
void core_say(const char *fmt, ...);

int zip_file(const char *zip, size_t n, const char *name, buf *out);
void zip_make(buf *out, int count, const char *const *names, const buf *files);

#endif
