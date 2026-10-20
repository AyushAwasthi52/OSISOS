#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

/* SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 8018). */

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint32_t buf_len;
    uint64_t total;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, size_t len);
void sha256_final(sha256_ctx* c, uint8_t out[32]);
void sha256(const void* data, size_t len, uint8_t out[32]);

void hmac_sha256(const void* key, size_t key_len, const void* msg, size_t msg_len, uint8_t out[32]);

/* Derive 32 bytes: one PBKDF2 block, `iterations` rounds. */
void pbkdf2_hmac_sha256(const void* pass, size_t pass_len, const void* salt, size_t salt_len,
                        uint32_t iterations, uint8_t out[32]);

#endif
