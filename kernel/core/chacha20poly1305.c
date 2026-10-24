#include "chacha20poly1305.h"

/* ---------------- ChaCha20 ---------------- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) do { a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); \
                            a += b; d ^= a; d = ROL(d, 8);  c += d; b ^= c; b = ROL(b, 7); } while (0)

static uint32_t ld32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
}

static void chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t block[64];
    while (len) {
        chacha_block(key, counter++, nonce, block);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ block[i];
        in += n; out += n; len -= n;
    }
}

/* ---------------- Poly1305 (8-bit limbs, after TweetNaCl: small and simple, not fast) ---------------- */

typedef struct { uint32_t r[17], h[17], pad[17]; uint8_t buf[16]; unsigned buflen; } poly_t;

static void poly_add(uint32_t* h, const uint32_t* c) {
    uint32_t u = 0;
    for (int j = 0; j < 17; j++) { u += h[j] + c[j]; h[j] = u & 255; u >>= 8; }
}

static void poly_init(poly_t* p, const uint8_t key[32]) {
    for (int j = 0; j < 17; j++) p->r[j] = p->h[j] = p->pad[j] = 0;
    for (int j = 0; j < 16; j++) { p->r[j] = key[j]; p->pad[j] = key[16 + j]; }
    p->r[3] &= 15; p->r[4] &= 252; p->r[7] &= 15; p->r[8] &= 252; p->r[11] &= 15; p->r[12] &= 252; p->r[15] &= 15;
    p->buflen = 0;
}

static void poly_block(poly_t* p, const uint8_t* m, unsigned len) {      /* len = 16 for full blocks */
    uint32_t c[17], x[17];
    for (int j = 0; j < 17; j++) c[j] = 0;
    for (unsigned j = 0; j < len; j++) c[j] = m[j];
    c[len] = 1;
    poly_add(p->h, c);
    for (int i = 0; i < 17; i++) {
        x[i] = 0;
        for (int j = 0; j < 17; j++) x[i] += p->h[j] * ((j <= i) ? p->r[i - j] : 320 * p->r[i + 17 - j]);
    }
    for (int i = 0; i < 17; i++) p->h[i] = x[i];
    uint32_t u = 0;
    for (int j = 0; j < 16; j++) { u += p->h[j]; p->h[j] = u & 255; u >>= 8; }
    u += p->h[16]; p->h[16] = u & 3;
    u = 5 * (u >> 2);
    for (int j = 0; j < 16; j++) { u += p->h[j]; p->h[j] = u & 255; u >>= 8; }
    u += p->h[16]; p->h[16] = u;
}

static void poly_update(poly_t* p, const uint8_t* m, size_t len) {
    while (len) {
        unsigned n = 16 - p->buflen;
        if (n > len) n = (unsigned)len;
        for (unsigned i = 0; i < n; i++) p->buf[p->buflen + i] = m[i];
        p->buflen += n; m += n; len -= n;
        if (p->buflen == 16) { poly_block(p, p->buf, 16); p->buflen = 0; }
    }
}

static void poly_final(poly_t* p, uint8_t out[16]) {
    static const uint32_t minusp[17] = { 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252 };
    if (p->buflen) poly_block(p, p->buf, p->buflen);
    uint32_t g[17];
    for (int j = 0; j < 17; j++) g[j] = p->h[j];
    poly_add(p->h, minusp);
    uint32_t s = (uint32_t)(-(int32_t)(p->h[16] >> 7));
    for (int j = 0; j < 17; j++) p->h[j] ^= s & (g[j] ^ p->h[j]);
    poly_add(p->h, p->pad);
    for (int j = 0; j < 16; j++) out[j] = (uint8_t)p->h[j];
}

/* ---------------- AEAD ---------------- */

static void make_nonce(uint8_t n[12], uint64_t counter) {
    n[0] = n[1] = n[2] = n[3] = 0;
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(counter >> (8 * i));
}

static void compute_tag(const uint8_t polykey[32], const uint8_t* ad, size_t ad_len, const uint8_t* ct, size_t len, uint8_t tag[16]) {
    static const uint8_t zeros[16] = { 0 };
    poly_t p;
    poly_init(&p, polykey);
    poly_update(&p, ad, ad_len);
    if (ad_len % 16) poly_update(&p, zeros, 16 - ad_len % 16);
    poly_update(&p, ct, len);
    if (len % 16) poly_update(&p, zeros, 16 - len % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)ad_len >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i)); }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
}

void aead_encrypt(const uint8_t key[32], uint64_t nonce, const uint8_t* ad, size_t ad_len, const uint8_t* pt, size_t len, uint8_t* out) {
    uint8_t n[12], block[64];
    make_nonce(n, nonce);
    chacha_block(key, 0, n, block);                       /* first 32 bytes = one-time Poly1305 key */
    chacha_xor(key, 1, n, pt, out, len);
    compute_tag(block, ad, ad_len, out, len, out + len);
    for (int i = 0; i < 64; i++) ((volatile uint8_t*)block)[i] = 0;
}

bool aead_decrypt(const uint8_t key[32], uint64_t nonce, const uint8_t* ad, size_t ad_len, const uint8_t* in, size_t len, uint8_t* pt) {
    uint8_t n[12], block[64], tag[16];
    make_nonce(n, nonce);
    chacha_block(key, 0, n, block);
    compute_tag(block, ad, ad_len, in, len, tag);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(tag[i] ^ in[len + i]);        /* constant-time compare */
    for (int i = 0; i < 64; i++) ((volatile uint8_t*)block)[i] = 0;
    if (diff) return false;
    chacha_xor(key, 1, n, in, pt, len);
    return true;
}
