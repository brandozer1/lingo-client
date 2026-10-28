// core.h - the core as an engine a host drives: core.c never starts a thread or holds the connection itself.
//
// 1. core_read(text, n)                 -> the settings file, read once. NULL when it isn't a JSON record
// 2. core_begin(send, ask, wait)        -> every lingo read and loaded. 0 when it can't start
// 3. core_open()                        -> on each new connection: hello, with the catalog
// 4. core_heard(text, n)                -> what a message from Onsay is. A stop lands at once, from any thread
// 5. core_do(text, n)                   -> a CORE_JOB, on the host's job thread, one at a time. A request may also run
//                                          inside a wait for lingo(...) or for the person's answer, on the same thread
// 6. core_look()                        -> every CORE_LOOK seconds: reads the lingos folder again if it changed. 1 when
//                                          the policy lets in public or an ip, so the port opens to other computers
// 7. core_serve(request, n, from, out)  -> a web request, {type:"request"}, answered by the lingo at its route with a
//                                          {type:"response"} in out
// 8. core_control(a, verb, values, why) -> start, stop, the settings and the policy, for the command line
// 9. core_pack(folder, to)              -> a folder of a lingo's files made into its .lingo file
//    core_say(fmt)                      -> a line on stderr, the lingo client's log
//    zip_file, zip_make                 -> a file read out of a zip, and a zip made (zip.c)
// The host owns the connection and the port: connecting, the heartbeat, reconnecting, and reading each web request
// (host.c).
#ifndef CORE_H
#define CORE_H

#include "lingo.h"

#define CORE_CLIENT "lingo-client"
#define CORE_VERSION "0.6.0"
#define CORE_LOOK 5   // seconds between looks at the lingos folder

typedef struct {
  val *all;                                    // the whole file, as read
  const char *token, *home, *root, *lingos;    // token: NULL when there is none, and the lingo client runs bare. root:
  int port;                                    // home as a whole path
  long long steps;
  double ms;
} core_settings;

typedef int (*core_send)(const char *text, size_t n);   // 1 when it went, 0 with no connection
// lingo(...) or use(...) as a program reaches it, asked of Onsay on the job's thread: values is the rest of the message,
// {"ask","values","gives","context","asking"} or {"address","values"}, and the answer goes in answer as {"value"} or
// {"error","code"}. 0 when Onsay can't be asked, or stop was set while it waited.
typedef int (*core_ask)(const char *values, size_t n, volatile int *stop, buf *answer);
// Waits on the job's thread, answering the requests that come meanwhile, until done is set, stop is, or seconds pass.
typedef void (*core_wait)(volatile int *done, volatile int *stop, double seconds);

enum { CORE_DONE, CORE_JOB, CORE_PONG };   // what core_heard made of a message
enum { CORE_PORT, CORE_ONSAY };            // where a web request came from: the port, its ip in it, or Onsay

const core_settings *core_read(const char *text, size_t n);
int core_begin(core_send send, core_ask ask, core_wait wait);
void core_open(void);
int core_heard(const char *text, size_t n);
int core_do(const char *text, size_t n);   // 1 when it was ready: Onsay took the hello
int core_look(void);
void core_serve(const char *request, size_t n, int from, buf *out);
val *core_control(arena *a, const char *verb, val *values, const char **why);
int core_pack(const char *folder, const char *to);
void core_say(const char *fmt, ...);

int zip_file(const char *zip, size_t n, const char *name, buf *out);
void zip_make(buf *out, int count, const char *const *names, const buf *files);

#endif
