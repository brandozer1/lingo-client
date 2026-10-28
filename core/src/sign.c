// sign.c - signing and checking: Ed25519 (RFC 8032) on Monocypher, which comes as it is (../../NOTICE), SHA-256, and
// base64 for both.
//
// 1. sha256(data, n, out)              the SHA-256 of bytes
// 2. base64(out, data, n)              bytes as base64, with its = at the end
//    unbase64(s, n, out)               and back. 0 when it isn't base64
// 3. ed25519_key(seed, key)            the public key of a private one, its 32 byte seed
//    ed25519_sign(seed, msg, n, sig)   a signature of msg
//    ed25519_check(key, msg, n, sig)   1 when key signed msg
#include "lingo.h"
#include "monocypher-ed25519.h"
#include <string.h>

////////// 1. SHA-256 //////////

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
  0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
  0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
  0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
  0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

// One 64 byte block into the hash so far.
static void block(uint32_t h[8], const unsigned char *b) {
  uint32_t w[64], s[8];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; i++)
    w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
  memcpy(s, h, sizeof s);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] + w[i];
    uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
    memmove(s + 1, s, 7 * sizeof *s);
    s[4] += t1;
    s[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) h[i] += s[i];
}

// 1.
void sha256(const void *data, size_t n, unsigned char out[32]) {
  uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  const unsigned char *p = data;
  unsigned char last[128] = { 0 };
  size_t whole = n / 64 * 64, rest = n - whole, size = rest < 56 ? 64 : 128;
  for (size_t i = 0; i < whole; i += 64) block(h, p + i);
  if (rest) memcpy(last, p + whole, rest);
  last[rest] = 0x80;
  for (int i = 0; i < 8; i++) last[size - 1 - i] = (unsigned char)((unsigned long long)n * 8 >> (8 * i));
  block(h, last);
  if (size == 128) block(h, last + 64);
  for (int i = 0; i < 32; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

////////// 2. BASE64 //////////

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 2.
void base64(buf *out, const void *data, size_t n) {
  const unsigned char *s = data;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
    char four[4] = { ALPHABET[v >> 18 & 63], ALPHABET[v >> 12 & 63], i + 1 < n ? ALPHABET[v >> 6 & 63] : '=', i + 2 < n ? ALPHABET[v & 63] : '=' };
    buf_add(out, four, 4);
  }
  if (!out->s) buf_add(out, "", 0);
}

// 2.
int unbase64(const char *s, size_t n, buf *out) {
  unsigned v = 0;
  int bits = 0;
  while (n && s[n - 1] == '=') n--;
  for (size_t i = 0; i < n; i++) {
    const char *at = s[i] ? strchr(ALPHABET, s[i]) : NULL;
    if (!at) return 0;
    v = v << 6 | (unsigned)(at - ALPHABET), bits += 6;
    if (bits >= 8) bits -= 8, buf_chr(out, (char)(v >> bits & 255));
  }
  if (!out->s) buf_add(out, "", 0);
  return 1;
}

////////// 3. ED25519 //////////

// 3.
void ed25519_key(const unsigned char seed[32], unsigned char key[32]) {
  unsigned char copy[32], secret[64];
  memcpy(copy, seed, 32);   // Monocypher wipes the seed it is given
  crypto_ed25519_key_pair(secret, key, copy);
  crypto_wipe(secret, sizeof secret);
}

// 3.
void ed25519_sign(const unsigned char seed[32], const void *msg, size_t n, unsigned char sig[64]) {
  unsigned char copy[32], secret[64], key[32];
  memcpy(copy, seed, 32);
  crypto_ed25519_key_pair(secret, key, copy);
  crypto_ed25519_sign(sig, secret, msg, n);
  crypto_wipe(secret, sizeof secret);
}

// 3.
int ed25519_check(const unsigned char key[32], const void *msg, size_t n, const unsigned char sig[64]) {
  return crypto_ed25519_check(sig, key, msg, n) == 0;
}
