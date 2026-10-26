// wasm.c - the compiler's door as WebAssembly, wrapped by web/compiler.js for the tests.
// JavaScript writes the request into lingo_alloc memory, calls lingo_call, and reads lingo_length bytes of JSON back.
#include "lingo.h"
#include <stdlib.h>

static buf answer;

__attribute__((export_name("lingo_alloc"))) void *lingo_alloc(size_t n) { return malloc(n ? n : 1); }
__attribute__((export_name("lingo_free"))) void lingo_free(void *p) { free(p); }
__attribute__((export_name("lingo_length"))) size_t lingo_length(void) { return answer.n; }

__attribute__((export_name("lingo_call"))) const char *lingo_call(const char *request, size_t n) {
  answer.n = 0;
  lingo_request(request, n, &answer);
  return answer.s;
}
