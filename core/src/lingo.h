// lingo.h - the Lingo core's own words, in plain C11 that knows nothing of any device (platform.h is the only door to one).
//
// 1. Values      nothing, true/false, numbers, text, lists and records, in an arena freed all at once
// 2. Buffers     growing text, for JSON and messages
// 3. Phrases     what this computer can do: an id, its line, what it needs, and the C function behind it
// 4. Running     a tree to its value or its error
// 5. Compiling   every lingo in the lingos folder, and a call of one of their functions, linked down to native phrases
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
  const char *needs;   // every permission it uses, like "screen.act, screen.read"
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
val *native_phrases(arena *a, size_t lingo);  // its phrases as the compiler reads them: [{id, params: [{name, default?}], does}]
void std_phrases(void);                       // std.c

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
  const char *const *allow; // permissions given
  int allowed;
};

// What a run may take, and what stops it.
typedef struct { long long steps; double ms; volatile int *stop; const char *const *allow; int allowed; } limits;

// The program's value, or NULL with the error's code and message, its file and line first when it is in a lingo.
val *run_program(arena *a, val *program, val *input, limits l, const char **code, const char **message);

////////// 5. COMPILING //////////

// natives: [{name, phrases}], each from native_phrases. stacked: [{file, code}], the lingos written in Lingo. Gives the
// instance, {lingos: name -> listing, stack}, or NULL with a message that starts with the file at fault.
val *lingo_load(arena *a, val *natives, val *stacked, const char **message);
// A program whose tool takes fn's values, as a0, a1 and on, and gives what fn gives for them, linked down to native
// phrases. NULL when no lingo here has fn.
val *lingo_call(arena *a, val *inst, const char *fn);

// For phrases.
_Noreturn void fail(run *r, const char *code, const char *fmt, ...);   // stops with an error, never returns
double num_of(run *r, val *x, const char *what);
val *text_of(run *r, val *x);                                           // any value as text

LINGO_SHARED_END

#endif
