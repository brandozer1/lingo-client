// zip.c - a .lingo file is a zip: one file read out of one, stored or deflated (puff.c), and one made, its files stored.
//
// 1. zip_file(zip, n, name, out)        -> 1 with the file called name in out, 0 when it isn't there or isn't whole
// 2. zip_make(out, count, names, files) -> a zip of those files, each stored as it is
#include "core.h"
#include "puff.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MOST (64u << 20)   // the most one file in a zip may be, unpacked

static uint32_t crc(const unsigned char *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0 - (c & 1)));
  }
  return ~c;
}

static uint32_t num(const unsigned char *p, int bytes) {
  uint32_t v = 0;
  for (int i = bytes - 1; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

static void put(buf *b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++) buf_chr(b, (char)(v >> (8 * i)));
}

////////// 1. READING //////////

// Its end record names where the list of its files is, and each in the list where the file is.
int zip_file(const char *zip, size_t n, const char *name, buf *out) {
  const unsigned char *z = (const unsigned char *)zip, *end = NULL;
  for (size_t i = n >= 22 ? n - 22 : 0; n >= 22 && !end; i--) {
    if (num(z + i, 4) == 0x06054b50) end = z + i;
    if (!i || n - i > 22 + 65535) break;
  }
  if (!end) return 0;
  size_t at = num(end + 16, 4), count = num(end + 10, 2), len = strlen(name);
  for (size_t k = 0; k < count && at + 46 <= n && num(z + at, 4) == 0x02014b50; k++) {
    const unsigned char *e = z + at;
    size_t nl = num(e + 28, 2), method = num(e + 10, 2), packed = num(e + 20, 4), size = num(e + 24, 4), local = num(e + 42, 4);
    at += 46 + nl + num(e + 30, 2) + num(e + 32, 2);
    if (at > n || nl != len || memcmp(e + 46, name, len)) continue;
    if (local + 30 > n || num(z + local, 4) != 0x04034b50 || size > MOST) return 0;
    size_t from = local + 30 + num(z + local + 26, 2) + num(z + local + 28, 2);
    if (from > n || packed > n - from || (method != 0 && method != 8)) return 0;
    unsigned char *data = malloc(size + 1);
    unsigned long got = size, used = packed;
    int ok = method == 0 ? packed == size && (memcpy(data, z + from, size), 1) : !puff(data, &got, z + from, &used) && got == size;
    ok = ok && crc(data, size) == num(e + 16, 4);
    if (ok) buf_add(out, (const char *)data, size);
    if (ok && !out->s) buf_add(out, "", 0);
    free(data);
    return ok;
  }
  return 0;
}

////////// 2. MAKING //////////

// Every time is 1980-01-01, the first a zip can say, so the same files always make the same zip.
void zip_make(buf *out, int count, const char *const *names, const buf *files) {
  buf list = { 0 };
  for (int i = 0; i < count; i++) {
    const unsigned char *data = (const unsigned char *)(files[i].s ? files[i].s : "");
    uint32_t sum = crc(data, files[i].n), size = (uint32_t)files[i].n, at = (uint32_t)out->n, nl = (uint32_t)strlen(names[i]);
    put(out, 0x04034b50, 4), put(out, 20, 2), put(out, 0, 2), put(out, 0, 2), put(out, 0, 2), put(out, 33, 2);
    put(out, sum, 4), put(out, size, 4), put(out, size, 4), put(out, nl, 2), put(out, 0, 2);
    buf_add(out, names[i], nl);
    buf_add(out, (const char *)data, size);
    put(&list, 0x02014b50, 4), put(&list, 20, 2), put(&list, 20, 2), put(&list, 0, 2), put(&list, 0, 2), put(&list, 0, 2), put(&list, 33, 2);
    put(&list, sum, 4), put(&list, size, 4), put(&list, size, 4), put(&list, nl, 2), put(&list, 0, 2), put(&list, 0, 2);
    put(&list, 0, 2), put(&list, 0, 2), put(&list, 0, 4), put(&list, at, 4);
    buf_add(&list, names[i], nl);
  }
  uint32_t at = (uint32_t)out->n;
  buf_add(out, list.s ? list.s : "", list.n);
  put(out, 0x06054b50, 4), put(out, 0, 2), put(out, 0, 2), put(out, (uint32_t)count, 2), put(out, (uint32_t)count, 2);
  put(out, (uint32_t)list.n, 4), put(out, at, 4), put(out, 0, 2);
  buf_free(&list);
}
