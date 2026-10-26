// lingo.h - the Lingo core's own words, in plain C11 that knows nothing of any device (platform.h is the only door to one).
//
// 1. Values      nothing, true/false, numbers, text, lists and records, in an arena freed all at once
// 2. Buffers     growing text, for JSON and messages
// 3. Phrases     what the device can do: an id, its manifest line, what it needs, and the C function behind it
// 4. Running     run a tree, or one phrase, to a result message
// 5. Compiling   Lingo code to a tree of native phrases, through every lingo it is written on
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
val *rec_get(val *r, const char *key);
void rec_set(arena *a, val *r, const char *key, val *x);
val *v_copy(arena *a, val *x);
int v_same(val *a, val *b);
int truthy(val *x);
const char *kind_name(val *x);

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
void buf_num(buf *b, double n);        // numbers written the way JavaScript writes them, so every runner agrees
void buf_text(buf *b, val *x);         // a value as text: nothing is empty, lists and records as JSON
void buf_json(buf *b, val *x);         // a value as JSON
void buf_json_str(buf *b, const char *s, size_t n);

val *json_parse(arena *a, const char *s, size_t n, const char **error);

////////// 3. PHRASES //////////

typedef struct run run;
typedef val *(*phrase_fn)(run *r, val **args);

typedef struct {
  const char *id;      // group.name
  const char *line;    // its line in the manifest, exactly as Onsay reads it
  const char *needs;   // the permission it uses
  phrase_fn fn;
  int count;           // how many values it takes, read from line
  char **params;
  val **defaults;      // NULL where a value has no default
} phrase;

void native_begin(const char *name, const char *version, const char *uses);   // a native lingo, like core 0.1. The core's to call
typedef void (*lingo_entry)(void);   // a library's lingo_phrases: group_add, shape_add and phrase_add, nothing else
void group_add(const char *name, const char *line);   // line is what follows "name: ", like "none, pure"
void shape_add(const char *name, const char *fields); // a record a phrase gives, like window = {id,title}
void phrase_add(const char *group, const char *needs, const char *line, phrase_fn fn);
phrase *phrase_find(const char *id);
const char *phrase_running(void);   // the id of the phrase being called, for one fn that serves many, like a host's
size_t native_count(void);
void phrase_manifest(buf *b, size_t lingo);         // native lingo number lingo, as its manifest text
void std_phrases(void);   // std.c

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
  val *log;                 // notes for the person, a list of text
  val *pictures;            // pictures for Onsay to see, sent with the result: records a device lingo makes, see picture()
  const char *const *allow; // permissions given
  int allowed;
};

// Both write a whole result message into out, {"type":"result","id","ok","value"} or with an error. The caller frees a.
void run_program(arena *a, const char *id_json, val *program, val *input, long long steps, double ms, volatile int *stop,
                 const char *const *allow, int allowed, buf *out);
void run_phrase(arena *a, const char *id_json, const char *phrase_id, val *values, volatile int *stop,
                const char *const *allow, int allowed, buf *out);

////////// 5. COMPILING //////////

// On failure each gives NULL, with the problem in *message and its *line. Everything made goes in a.
val *lingo_manifest(arena *a, const char *text, size_t n, const char **message);   // a native lingo's manifest, read
val *lingo_read(arena *a, val *manifest, const char *folder, const char **message);  // a native or host lingo's lingo.json
val *lingo_schema(arena *a, const char *gives, size_t n, val *shapes);             // what a listing gives, as JSON Schema
val *lingo_gives(arena *a, val *schema, val *shapes);                              // and back

void lingo_line(buf *b, val *phrase, const char *group_needs);                     // a phrase as its listing's line
val *lingo_parse(arena *a, const char *code, size_t n, val *phrases, const char *own, int *line, const char **message);   // own: its lingo's name
val *lingo_link(arena *a, val *program, val *stack);
val *lingo_load(arena *a, val *natives, val *stacked, const char **message);        // natives: [text], stacked: [{file, code}]
val *lingo_face(arena *a, val *inst, val *names, const char **message);             // what Onsay is shown, as text
val *lingo_prepare(arena *a, val *inst, const char *code, size_t n, val *names, int *line, const char **message);   // to run
void lingo_request(const char *json, size_t n, buf *out);                          // all of it, as one JSON call

// For phrases.
_Noreturn void fail(run *r, const char *code, const char *fmt, ...);   // stops with an error, never returns
double num_of(run *r, val *x, const char *what);
val *text_of(run *r, val *x);                                           // any value as text
int order(run *r, val *a, val *b);                                      // -1, 0 or 1, for two numbers or two texts
void note(run *r, val *x);
int picture(run *r, val *x);   // {data (base64), mime, ...} sent with the result for Onsay to see. Gives its number, from 1

LINGO_SHARED_END

#endif
