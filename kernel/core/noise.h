#ifndef NOISE_H
#define NOISE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Noise Protocol Framework, responder side of the NK handshake:
 *
 *     Noise_NK_25519_ChaChaPoly_SHA256
 *       <- s                      (the client knows the server's static public key in advance)
 *       -> e, es                  message 1: 32-byte ephemeral key + 16-byte tag   (48 bytes)
 *       <- e, ee                  message 2: 32-byte ephemeral key + 16-byte tag   (48 bytes)
 *
 * After message 2 both sides share two forward-secret transport keys. The server is authenticated (only the holder
 * of the static private key can complete the handshake); the client is anonymous at this layer, so users
 * authenticate afterwards with their password INSIDE the encrypted channel.
 * Spec: https://noiseprotocol.org/noise.html
 */
typedef struct { uint8_t k[32]; uint64_t n; } noise_cs;

typedef struct {
    uint8_t h[32], ck[32], k[32];
    uint64_t n;
    bool    has_k;
    uint8_t s_priv[32], s_pub[32], re[32];
    noise_cs recv, send;                      /* valid once noise_resp_write_msg2() returned */
} noise_hs;

void noise_resp_init(noise_hs* hs, const uint8_t s_priv[32], const uint8_t s_pub[32], const uint8_t* prologue, size_t prologue_len);
bool noise_resp_read_msg1(noise_hs* hs, const uint8_t* msg, size_t len);               /* len must be 48 */
void noise_resp_write_msg2(noise_hs* hs, const uint8_t e_priv[32], uint8_t out[48]);   /* completes the handshake, wipes secrets */

void noise_encrypt(noise_cs* cs, const uint8_t* pt, size_t len, uint8_t* out);         /* out: len + 16 bytes */
bool noise_decrypt(noise_cs* cs, const uint8_t* in, size_t len, uint8_t* pt);          /* in: ciphertext + 16-byte tag */
#endif
