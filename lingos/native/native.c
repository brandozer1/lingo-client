// native.c - the native lingo, for Windows: call a function in Windows or any DLL directly, by its name and the shape of
// what it takes and returns, so any of them can be run without code written for each one. Built into the lingo client for
// Windows x64 by core/build.ps1, with a small vendored part of libffi (libffi/, MIT) to arrange the call. Needs the run permission,
// since a call can do anything the computer can.
//
// 1. type(name)              -> the libffi type for a layout name, or NULL
//    call(r, a)              -> native.call(library, function, returns, args): loads library, finds function, calls it
//                              with args and gives back its answer as returns says
// 2. lingo_phrases()         -> the one phrase, called by the core on load
#include "lingo.h"
#include <ffi.h>
#include <stdint.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

////////// 1. CALLING //////////

// A value, as text, failing when it is not.
static const char *str(run *r, val *x) { return text_of(r, x)->t.s; }

// A layout name to its libffi type. text and cstring are a pointer; a number is sent and read as that width.
static ffi_type *type(const char *name) {
  if (!strcmp(name, "none")) return &ffi_type_void;
  if (!strcmp(name, "i8")) return &ffi_type_sint8;
  if (!strcmp(name, "u8")) return &ffi_type_uint8;
  if (!strcmp(name, "i16")) return &ffi_type_sint16;
  if (!strcmp(name, "u16")) return &ffi_type_uint16;
  if (!strcmp(name, "i32")) return &ffi_type_sint32;
  if (!strcmp(name, "u32")) return &ffi_type_uint32;
  if (!strcmp(name, "i64")) return &ffi_type_sint64;
  if (!strcmp(name, "u64")) return &ffi_type_uint64;
  if (!strcmp(name, "f32")) return &ffi_type_float;
  if (!strcmp(name, "f64")) return &ffi_type_double;
  if (!strcmp(name, "pointer") || !strcmp(name, "text") || !strcmp(name, "cstring")) return &ffi_type_pointer;
  return NULL;
}

// One argument's room, big enough for any scalar or a pointer, and its address is what libffi is given.
typedef union { int8_t i8; uint8_t u8; int16_t i16; uint16_t u16; int32_t i32; uint32_t u32; int64_t i64; uint64_t u64; float f32; double f64; void *ptr; } slot;

// A number put into a slot as the layout's width, or a text's bytes as a pointer.
static void put(run *r, slot *s, const char *t, val *v) {
  if (!strcmp(t, "text") || !strcmp(t, "cstring")) { s->ptr = (void *)str(r, v); return; }
  if (!strcmp(t, "pointer")) { s->ptr = (void *)(uintptr_t)num_of(r, v, "a pointer"); return; }
  double n = num_of(r, v, "an argument");
  if (!strcmp(t, "i8")) s->i8 = (int8_t)n; else if (!strcmp(t, "u8")) s->u8 = (uint8_t)n;
  else if (!strcmp(t, "i16")) s->i16 = (int16_t)n; else if (!strcmp(t, "u16")) s->u16 = (uint16_t)n;
  else if (!strcmp(t, "i32")) s->i32 = (int32_t)n; else if (!strcmp(t, "u32")) s->u32 = (uint32_t)n;
  else if (!strcmp(t, "i64")) s->i64 = (int64_t)n; else if (!strcmp(t, "u64")) s->u64 = (uint64_t)n;
  else if (!strcmp(t, "f32")) s->f32 = (float)n; else if (!strcmp(t, "f64")) s->f64 = n;
}

// 1.
static val *call(run *r, val **a) {
  const char *library = str(r, a[0]), *function = str(r, a[1]), *ret = str(r, a[2]);
  val *args = a[3] && a[3]->k == V_LIST ? a[3] : NULL;
  size_t n = args ? args->l.n : 0;
  ffi_type *rtype = type(ret);
  if (!rtype) fail(r, "phrase", "native.call doesn't know the return type %s", ret);
  // An empty library is this program itself.
  HMODULE m = library[0] ? LoadLibraryA(library) : GetModuleHandleA(NULL);
  void *sym = m ? (void *)(void (*)(void))GetProcAddress(m, function) : NULL;
  if (!sym) fail(r, "phrase", "%s was not found%s%s", function, library[0] ? " in " : "", library);
  ffi_type **atypes = arena_get(r->a, (n ? n : 1) * sizeof *atypes);
  void **avalues = arena_get(r->a, (n ? n : 1) * sizeof *avalues);
  slot *slots = arena_get(r->a, (n ? n : 1) * sizeof *slots);
  for (size_t i = 0; i < n; i++) {
    val *arg = args->l.v[i];
    if (arg->k != V_REC) fail(r, "phrase", "each argument is a record like {type: \"i32\", value: 5}");
    const char *t = str(r, rec_get(arg, "type") ? rec_get(arg, "type") : NOTHING);
    atypes[i] = type(t);
    if (!atypes[i] || atypes[i] == &ffi_type_void) fail(r, "phrase", "native.call doesn't know the argument type %s", t);
    put(r, &slots[i], t, rec_get(arg, "value") ? rec_get(arg, "value") : NOTHING);
    avalues[i] = &slots[i];
  }
  ffi_cif cif;
  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, atypes) != FFI_OK) fail(r, "phrase", "native.call couldn't arrange the call to %s", function);
  union { ffi_arg i; float f32; double f64; void *ptr; } out;
  ffi_call(&cif, FFI_FN(sym), &out, avalues);
  if (!strcmp(ret, "none")) return NOTHING;
  if (!strcmp(ret, "f32")) return v_num(r->a, out.f32);
  if (!strcmp(ret, "f64")) return v_num(r->a, out.f64);
  if (!strcmp(ret, "cstring")) return out.ptr ? v_cstr(r->a, (const char *)out.ptr) : NOTHING;
  if (!strcmp(ret, "pointer")) return v_num(r->a, (double)(uintptr_t)out.ptr);
  if (!strcmp(ret, "i8")) return v_num(r->a, (int8_t)out.i);
  if (!strcmp(ret, "i16")) return v_num(r->a, (int16_t)out.i);
  if (!strcmp(ret, "i32")) return v_num(r->a, (int32_t)out.i);
  if (!strcmp(ret, "i64")) return v_num(r->a, (double)(int64_t)out.i);
  if (!strcmp(ret, "u64")) return v_num(r->a, (double)(uint64_t)out.i);
  return v_num(r->a, (double)(uint64_t)(uint32_t)out.i);   // u8, u16, u32
}

////////// 2. THE PHRASE //////////

// Called once as the lingo client starts (core.c, BUILTINS).
void native_lingo(void) {
  phrase_add("native", "run", "call(library, function, returns, args=nothing) -> value: calls a function in Windows or a DLL "
             "by name. library is its file, like \"kernel32.dll\", or \"\" for this program. returns and each argument's type is "
             "one of i8 u8 i16 u16 i32 u32 i64 u64 (whole numbers), f32 f64 (decimals), pointer, text, cstring, or none. args is "
             "a list of {type, value}", call);
}
