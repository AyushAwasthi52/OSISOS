#ifndef RNG_H
#define RNG_H
#include <stdint.h>
#include <stddef.h>

/*
 * Random numbers for salts, session keys and the server identity key.
 *
 * A bare VM has no hardware entropy source, so the pool is fed with timing jitter: the TSC at every
 * keystroke and every received frame, plus the tick counter and time of boot. Output is
 * SHA-256(pool || counter) with the pool re-keyed after every call (forward secrecy within the pool).
 *
 * HONEST LIMIT: right after boot, with no input yet, there is little real entropy. Keys are generated on
 * demand (`rcon on`), after the administrator has typed commands and the NIC has seen traffic, which
 * is when the pool is best seeded. See docs/SECURITY.md.
 */
void rng_event(uint32_t extra);                 /* cheap; call from input / packet paths */
void rng_bytes(uint8_t* out, size_t n);
#endif
