#include "noise.h"
#include "x25519.h"
#include "chacha20poly1305.h"
#include "sha256.h"

static void wipe(void* p, size_t n) { volatile uint8_t* v = (volatile uint8_t*)p; while (n--) *v++ = 0; }

static void mix_hash(noise_hs* hs, const uint8_t* data, size_t len) {
    sha256_ctx c;
    sha256_init(&c); sha256_update(&c, hs->h, 32); sha256_update(&c, data, len); sha256_final(&c, hs->h);
}

/* HKDF (RFC 5869) with two 32-byte outputs, as defined by Noise */
static void hkdf2(const uint8_t ck[32], const uint8_t* ikm, size_t ikm_len, uint8_t out1[32], uint8_t out2[32]) {
    uint8_t temp[32], buf[33];
    hmac_sha256(ck, 32, ikm, ikm_len, temp);
    buf[0] = 0x01;
    hmac_sha256(temp, 32, buf, 1, out1);
    for (int i = 0; i < 32; i++) buf[i] = out1[i];
    buf[32] = 0x02;
    hmac_sha256(temp, 32, buf, 33, out2);
    wipe(temp, sizeof(temp));
}

static void mix_key(noise_hs* hs, const uint8_t* ikm, size_t len) {
    uint8_t ck[32], k[32];
    hkdf2(hs->ck, ikm, len, ck, k);
    for (int i = 0; i < 32; i++) { hs->ck[i] = ck[i]; hs->k[i] = k[i]; }
    hs->n = 0;
    hs->has_k = true;
    wipe(ck, 32); wipe(k, 32);
}

static bool all_zero(const uint8_t* p, size_t n) { uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= p[i]; return d == 0; }

void noise_resp_init(noise_hs* hs, const uint8_t s_priv[32], const uint8_t s_pub[32], const uint8_t* prologue, size_t prologue_len) {
    static const char name[33] = "Noise_NK_25519_ChaChaPoly_SHA256";   /* exactly 32 bytes: used as the initial hash */
    for (int i = 0; i < 32; i++) { hs->h[i] = (uint8_t)name[i]; hs->ck[i] = (uint8_t)name[i]; hs->k[i] = 0; hs->s_priv[i] = s_priv[i]; hs->s_pub[i] = s_pub[i]; hs->re[i] = 0; }
    hs->n = 0;
    hs->has_k = false;
    mix_hash(hs, prologue, prologue_len);
    mix_hash(hs, hs->s_pub, 32);                   /* pre-message: <- s */
}

bool noise_resp_read_msg1(noise_hs* hs, const uint8_t* msg, size_t len) {
    if (len != 48) return false;
    for (int i = 0; i < 32; i++) hs->re[i] = msg[i];
    mix_hash(hs, hs->re, 32);                      /* e */
    uint8_t dh[32];
    x25519(dh, hs->s_priv, hs->re);                /* es */
    if (all_zero(dh, 32)) { wipe(dh, 32); return false; }
    mix_key(hs, dh, 32);
    wipe(dh, 32);

    uint8_t payload[1];                            /* the payload is empty: only the tag is carried */
    if (!aead_decrypt(hs->k, hs->n, hs->h, 32, msg + 32, 0, payload)) return false;
    hs->n++;
    mix_hash(hs, msg + 32, 16);
    return true;
}

void noise_resp_write_msg2(noise_hs* hs, const uint8_t e_priv[32], uint8_t out[48]) {
    uint8_t e_pub[32], dh[32];
    x25519_base(e_pub, e_priv);
    for (int i = 0; i < 32; i++) out[i] = e_pub[i];
    mix_hash(hs, e_pub, 32);                       /* e */
    x25519(dh, e_priv, hs->re);                    /* ee */
    mix_key(hs, dh, 32);
    wipe(dh, 32);

    aead_encrypt(hs->k, hs->n, hs->h, 32, 0, 0, out + 32);       /* empty payload: 16-byte tag */
    hs->n++;
    mix_hash(hs, out + 32, 16);

    uint8_t k1[32], k2[32];                        /* Split(): initiator->responder = k1, responder->initiator = k2 */
    hkdf2(hs->ck, 0, 0, k1, k2);
    for (int i = 0; i < 32; i++) { hs->recv.k[i] = k1[i]; hs->send.k[i] = k2[i]; }
    hs->recv.n = 0; hs->send.n = 0;
    wipe(k1, 32); wipe(k2, 32);
    wipe(hs->k, 32); wipe(hs->ck, 32); wipe(hs->s_priv, 32); wipe(hs->re, 32);
}

void noise_encrypt(noise_cs* cs, const uint8_t* pt, size_t len, uint8_t* out) {
    aead_encrypt(cs->k, cs->n++, 0, 0, pt, len, out);
}

bool noise_decrypt(noise_cs* cs, const uint8_t* in, size_t len, uint8_t* pt) {
    if (len < 16) return false;
    if (!aead_decrypt(cs->k, cs->n, 0, 0, in, len - 16, pt)) return false;     /* the counter only advances on success */
    cs->n++;
    return true;
}
