#ifndef X25519_H
#define X25519_H
#include <stdint.h>

/* X25519 (RFC 7748) Diffie-Hellman on Curve25519, after TweetNaCl (public domain).
 * Constant-time (no secret-dependent branches or table lookups).
 * NOT re-entrant: it uses static scratch space to keep stacks small (kernel tasks have 4 KB). */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);      /* public key from a private key */

#endif
