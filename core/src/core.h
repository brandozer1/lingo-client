// core.h - the core as an engine a host drives: core.c never starts a thread or holds the connection itself.
//
// 1. core_read(text, n)                -> the settings file, read once. NULL when it isn't a JSON record
// 2. core_begin(send, ask)             -> every lingo read and loaded. 0 when it can't start
// 3. core_open()                       -> on each new connection: hello, with the catalog
// 4. core_heard(text, n)               -> what a message from Onsay is. A stop lands at once, from any thread
// 5. core_do(text, n)                  -> a CORE_JOB, on the host's job thread, one at a time. A request may also run
//                                         inside a wait for lingo(...), on the same thread
// 6. core_look()                       -> every CORE_LOOK seconds while idle: reads the lingos folder again if it changed
// 7. core_serve(request, n, from, out) -> a web request, {type:"request"}, answered by the lingo at its route with a
//                                         {type:"response"} in out
// 8. core_call(a, fn, values, ...)     -> one function of a lingo here, run, for the command line
//    core_say(fmt)                     -> a line on stderr, the lingo client's log
// The host owns the connection and the port: connecting, the heartbeat, reconnecting, and reading each web request
// (host.c).
#ifndef CORE_H
#define CORE_H

#include "lingo.h"

#define CORE_CLIENT "lingo-client"
#define CORE_VERSION "0.5.0"
#define CORE_LOOK 5   // seconds between looks at the lingos folder while idle

typedef struct {
  val *all;                              // the whole file, as read
  const char *token, *home, *lingos;     // token: NULL when there is none, and the lingo client runs bare
  const char **allow;
  int allowed, port, network, inbound;   // network: listen says network. inbound: requests through Onsay are taken too
  long long steps;
  double ms;
} core_settings;

typedef int (*core_send)(const char *text, size_t n);   // 1 when it went, 0 with no connection
// lingo(...) or use(...) as a program reaches it, asked of Onsay on the job's thread: values is the rest of the message,
// {"ask","values","gives","context","asking"} or {"address","values"}, and the answer goes in answer as {"value"} or
// {"error","code"}. 0 when Onsay can't be asked, or stop was set while it waited.
typedef int (*core_ask)(const char *values, size_t n, volatile int *stop, buf *answer);

enum { CORE_DONE, CORE_JOB, CORE_PONG };       // what core_heard made of a message
enum { CORE_HERE, CORE_NETWORK, CORE_ONSAY };  // where a web request came from: this computer, or not

const core_settings *core_read(const char *text, size_t n);
int core_begin(core_send send, core_ask ask);
void core_open(void);
int core_heard(const char *text, size_t n);
int core_do(const char *text, size_t n);   // 1 when it was ready: Onsay took the hello
void core_look(void);
void core_serve(const char *request, size_t n, int from, buf *out);
val *core_call(arena *a, const char *fn, val *values, volatile int *stop, const char **code, const char **message);
void core_say(const char *fmt, ...);

#endif
