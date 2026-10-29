// lingo.h - the Lingo core's own words, in plain C11 that knows nothing of any device (platform.h is the only door to one).
//
// 1. Values      nothing, true/false, numbers, text, lists and records, in an arena freed all at once
// 2. Buffers     growing text, for JSON and messages
// 3. Phrases     what this computer can do: an id, its line, what it needs, and the C function behind it
// 4. Running     a tree to its value or its error
// 5. Compiling   every lingo in the lingos folder, and a call of one of their functions, linked down to native phrases
// 6. Signing     SHA-256, base64, Ed25519, and this device's own keys
// 7. Hooks       what a native lingo fills to connect this lingo client to more than its port, like the onsay lingo
// A native lingo's library is written against this and platform.h. Its one entry is lingo_phrases (LINGO_ENTRY).
#ifndef LINGO_H
#define LINGO_H

#include <stddef.h>

// The core is built with LINGO_CORE and shares what's between LINGO_SHARED and LINGO_SHARED_END; a library takes them.
#if defined(_WIN32) && (defined(LINGO_CORE) || defined(LINGO_LIBRARY))
#ifdef LINGO_LIBRARY
#define LINGO_SIDE dllimport
#else
#define LINGO_SIDE dllexport
#endif
#define LINGO_SHARED _Pragma("clang attribute push (__attribute__((LINGO_SIDE)), apply_to = any(function, variable(is_global)))")
#define LINGO_SHARED_END _Pragma("clang attribute pop")
#define LINGO_ENTRY __declspec(dllexport)
#else
#define LINGO_SHARED
#define LINGO_SHARED_END
#define LINGO_ENTRY __attribute__((visibility("default")))
#endif

LINGO_SHARED

////////// 1. VALUES //////////

typedef enum { V_NOTHING, V_BOOL, V_NUM, V_TEXT, V_LIST, V_REC } vkind;

typedef struct val val;
struct val {
  vkind k;
  int tag;   // a tree node's kind, worked out the first time it runs, see run.c
  union {
    int b;
    double n;
    struct { char *s; size_t len; } t;                  // UTF-8, also ending in a 0 byte
    struct { val **v; size_t n, cap; } l;
    struct { char **key; val **v; size_t n, cap; } r;   // parts kept in the order they were added
  };
};

// Everything one program makes lives in one arena, freed in one go when it ends.
typedef struct arena arena;
arena *arena_new(void);
void arena_free(arena *a);
void *arena_get(arena *a, size_t size);
size_t arena_used(arena *a);

extern val V_NOTHING_VAL, V_TRUE_VAL, V_FALSE_VAL;
#define NOTHING (&V_NOTHING_VAL)
#define VBOOL(x) ((x) ? &V_TRUE_VAL : &V_FALSE_VAL)

val *v_num(arena *a, double n);
val *v_text(arena *a, const char *s, size_t len);
val *v_cstr(arena *a, const char *s);
val *v_list(arena *a);
val *v_rec(arena *a);
void list_push(arena *a, val *list, val *x);
val *rec_get(val *r, const char *key);                                // NULL when r isn't a record or hasn't key
void rec_set(arena *a, val *r, const char *key, val *x);
const char *text_or(val *r, const char *key, const char *fallback);   // a record's text by key, else fallback
val *v_copy(arena *a, val *x);
int v_same(val *a, val *b);
int truthy(val *x);
const char *kind_name(val *x);
// One header into a record by its lower case name. A name said twice keeps both values, joined by ", ".
void header_add(arena *a, val *headers, const char *name, size_t n, const char *value, size_t vn);
val *header_lines(arena *a, const char *s);   // "Name: value" lines, each ending \r\n, as that record

// UTF-8: how many characters, where character i starts, and whether bytes are text at all.
size_t utf8_count(const char *s, size_t len);
size_t utf8_offset(const char *s, size_t len, size_t index);
int utf8_ok(const char *s, size_t len);

////////// 2. BUFFERS //////////

typedef struct { char *s; size_t n, cap; } buf;
void buf_add(buf *b, const char *s, size_t n);
void buf_str(buf *b, const char *s);
void buf_chr(buf *b, char c);
void buf_fmt(buf *b, const char *fmt, ...);
void buf_free(buf *b);
void buf_num(buf *b, double n);        // numbers written the way JavaScript writes them
void buf_text(buf *b, val *x);         // a value as text: nothing is empty, lists and records as JSON
void buf_json(buf *b, val *x);         // a value as JSON
void buf_json_str(buf *b, const char *s, size_t n);

val *json_parse(arena *a, const char *s, size_t n, const char **error);

////////// 3. PHRASES //////////

typedef struct run run;
typedef val *(*phrase_fn)(run *r, val **args);

typedef struct {
  const char *id;      // group.name
  const char *needs;   // everything it needs on the device, like "screen.act, screen.read", or "none"
  const char *does;    // its line after the signature, like "-> [text]: sorted"
  phrase_fn fn;
  size_t lingo;        // the native lingo it is of, by number, core being 0
  int count;           // how many values it takes, read from its line
  char **params;
  val **defaults;      // NULL where a value has no default, NOTHING where it is nothing
} phrase;

typedef void (*lingo_entry)(void);   // a library's lingo_phrases, which calls phrase_add for each of its phrases
// line is its signature, then -> what it gives and : what it does, as it needs, like "keys(prefix=\"\") -> [text]: sorted".
void phrase_add(const char *group, const char *needs, const char *line, phrase_fn fn);
phrase *phrase_find(const char *id);
void native_begin(size_t lingo);              // the phrases added from now on are native lingo number lingo's
val *native_phrases(arena *a, size_t lingo);  // its phrases as the compiler reads them: [{id, params: [{name, default?}], does, needs}]
void std_phrases(void);                       // std.c
// Stops the run with denied unless it may use need, like run or files.read, on path, a whole path, or on nothing when
// path is NULL (core.c). Every phrase's needs are checked as it is called, and a native lingo's file phrases check each
// path they reach.
void may(run *r, const char *need, const char *path);

////////// 4. RUNNING //////////

struct run {
  arena *a;
  val *program;
  long long left;           // steps left before the limit
  double until;             // the time it must end by
  volatile int *stop;       // set from outside to stop between two steps
  int depth;
  void *trap;               // where an error jumps to
  char code[16];
  buf message, at, at_error;
};

// What a run may take, and what stops it.
typedef struct { long long steps; double ms; volatile int *stop; } limits;

// The program's value, or NULL with the error's code and message, its file and line first when it is in a lingo.
val *run_program(arena *a, val *program, val *input, limits l, const char **code, const char **message);

////////// 5. COMPILING //////////

// natives: [{name, phrases}], each from native_phrases. stacked: [{file, code}], the lingos written in Lingo, file like
// memo.ls. Gives the instance, {lingos: name -> listing, stack}, each phrase of a listing with its inputSchema and, for a
// Lingo lingo's, its lingos, how many lingo(...) calls it makes, and its needs, through every function it reaches. NULL
// with a message that starts with the file at fault.
val *lingo_load(arena *a, val *natives, val *stacked, const char **message);
// A program whose tool takes fn's values, as a0, a1 and on, and gives what fn gives for them, linked down to native
// phrases. NULL when no lingo here has fn.
val *lingo_call(arena *a, val *inst, const char *fn);

// For phrases.
_Noreturn void fail(run *r, const char *code, const char *fmt, ...);   // stops with an error, never returns
double num_of(run *r, val *x, const char *what);
val *text_of(run *r, val *x);                                           // any value as text

////////// 6. SIGNING //////////

// sign.c, on Monocypher. A key is 32 bytes, a private one its seed, and a signature 64.
void sha256(const void *data, size_t n, unsigned char out[32]);
void base64(buf *out, const void *data, size_t n);
int unbase64(const char *s, size_t n, buf *out);   // 0 when it isn't base64
void ed25519_key(const unsigned char seed[32], unsigned char key[32]);
void ed25519_sign(const unsigned char seed[32], const void *msg, size_t n, unsigned char sig[64]);
int ed25519_check(const unsigned char key[32], const void *msg, size_t n, const unsigned char sig[64]);   // 1 when key signed it

// This device's own key pairs, by a name like device, kept in home/keys, the private half sealed by the device (pl_seal)
// and never handed out (core.c). key_of gives the public half, made first when missing and make is set. 0 when there is
// none, or it couldn't be kept or signed with.
int key_of(const char *name, int make, unsigned char key[32]);
int key_sign(const char *name, const void *msg, size_t n, unsigned char sig[64]);
void key_forget(const char *name);

////////// 7. HOOKS //////////

/* A native lingo that connects this lingo client to more than its port, like the onsay lingo, fills these in its
   lingo_phrases with lingo_hook. They are used only while it runs, in a lingo client that serves, never at the command
   line, and any it leaves NULL do nothing.
     open      who reaches it when no rule says otherwise, like "public": its own default access rule
     running   called as it starts and each time the catalog changes, with the catalog as JSON (core/README.md), and with
               NULL as it stops. On the job thread
     decided   each line of decisions.log as it is written, as JSON
     ask       lingo(...) and use(...) as a program reaches them, on the job thread: values is {"ask","values","gives",
               "context","asking"} or {"address","values"} as JSON, and the answer goes in answer, a record with value, or
               error and code. 0 when it can't be asked, or stop was set while it waited (lingo_wait)
     key       a request's signer by its keyid, like laptop.brandon: {key: "ed25519:<base64>", handle, mine}, mine true
               when it is a device of this computer's own handle. NULL when it isn't known */
typedef struct {
  const char *open;
  void (*running)(const char *catalog);
  void (*decided)(const char *line);
  int (*ask)(const char *values, size_t n, volatile int *stop, buf *answer);
  val *(*key)(arena *a, const char *keyid);
} lingo_hooks;
void lingo_hook(const lingo_hooks *hooks);

/* What the lingo client offers it back, from any thread unless it says.
     lingo_hand     a web request from elsewhere, {id, method, path, headers, body, base64} as JSON, answered in turn as
                    the port answers one, its caller "me", the account's own call, or "ip <address>". answered gets
                    {id, status, headers, body, base64}, on the job thread
     lingo_stop     stops the request whose id, as JSON, is id
     lingo_wait     on the job thread: waits until done or stop is set, or seconds pass, answering requests meanwhile
     lingo_done     sets done, and wakes the job thread's wait
     lingo_policy   the account's policy, with its version, kept as the one in force, its grants for this computer by
                    device, its name on the account. NULL with why when its version is older than the one kept. Gives the
                    grants the ceiling clamps, a record by lingo
     lingo_setting  one of a lingo's settings as this computer keeps them, or NULL. lingo_set keeps one, 1 when it was
     lingo_home     this lingo client's home folder, as a whole path, for a file a native lingo keeps there, like a log
     lingo_serves   1 in a lingo client that serves, whether or not the native lingo asking runs, 0 at the command line */
void lingo_hand(const char *request, size_t n, const char *caller, void (*answered)(void *arg, const char *response, size_t n), void *arg);
void lingo_stop(const char *id);
void lingo_wait(volatile int *done, volatile int *stop, double seconds);
void lingo_done(volatile int *done);
val *lingo_policy(arena *a, val *account, const char *device, const char **why);
val *lingo_setting(arena *a, const char *lingo, const char *key);
int lingo_set(const char *lingo, const char *key, val *value);
const char *lingo_home(void);
int lingo_serves(void);

LINGO_SHARED_END

#endif
