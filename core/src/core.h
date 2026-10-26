// core.h - the core as an engine a host drives: core.c never starts a thread or holds the connection itself.
//
// 1. core_begin(settings, send, ask)  -> reads the settings and every lingo, once. 0 when it can't start
// 2. core_open()                      -> on each new connection: hello, then every lingo folder sent up
// 3. core_heard(text, n)              -> what a message is. A stop lands at once, from any thread
// 4. core_do(text, n)                 -> runs a CORE_JOB on the host's job thread, one at a time, in order. A call may
//                                        also run inside an ask, on the same thread
// 5. core_look()                      -> every CORE_LOOK seconds while idle: reads the lingos folder again if it changed
//    core_manifests(out), core_say(fmt)  -> each native lingo's manifest, and a line on stderr
// The host owns the connection: connecting, pings, the heartbeat and reconnecting (host.c).
#ifndef CORE_H
#define CORE_H

#include "lingo.h"

#define CORE_CLIENT "onsay-core-c"
#define CORE_VERSION "0.3.0"
#define CORE_LOOK 5   // seconds between looks at the lingos folder while idle

typedef int (*core_send)(const char *text, size_t n);   // 1 when it went, 0 with no connection
// lingo(...), use(...) or setting(...) as a program reaches it, asked of Onsay on the job's thread: values is
// the phrase's values by name as JSON, {"ask","values","gives"} for lingo.run, {"address","values"} for lingo.use or
// {"key","reference"} for lingo.setting, and the answer
// goes in answer as {"value"} or {"error", "code"}, as pl_call gives one. 0 when Onsay can't be asked, or stop was set
// while it waited. NULL when the host answers them itself, through pl_call, like the web host.
typedef int (*core_ask)(const char *values, size_t n, volatile int *stop, buf *answer);

enum { CORE_DONE, CORE_JOB, CORE_PING, CORE_PONG };   // what core_heard made of a message

int core_begin(const char *settings, size_t n, core_send send, core_ask ask);
void core_open(void);
int core_heard(const char *text, size_t n);
int core_do(const char *text, size_t n);   // 1 when it was ready: Onsay took the hello
void core_look(void);
void core_manifests(buf *out);             // --- between them
void core_say(const char *fmt, ...);

#endif
