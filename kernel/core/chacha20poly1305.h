#ifndef CHACHA20POLY1305_H
#define CHACHA20POLY1305_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ChaCha20-Poly1305 AEAD (RFC 8439). The 96-bit nonce is 4 zero bytes followed by a 64-bit little-endian
 * counter, as the Noise Protocol Framework specifies for "ChaChaPoly". */
#define AEAD_TAG 16
/* out = ciphertext (len bytes) followed by the 16-byte tag; `out` must hold len + 16 bytes. */
void aead_encrypt(const uint8_t key[32], uint64_t nonce, const uint8_t* ad, size_t ad_len,
                  const uint8_t* pt, size_t len, uint8_t* out);
/* in = ciphertext followed by the tag (len + 16 bytes). Returns false (and writes nothing useful) on a bad tag. */
bool aead_decrypt(const uint8_t key[32], uint64_t nonce, const uint8_t* ad, size_t ad_len,
                  const uint8_t* in, size_t len, uint8_t* pt);
#endif
