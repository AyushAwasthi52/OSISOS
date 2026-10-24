#include "rng.h"
#include "sha256.h"

extern uint32_t tick;

static uint8_t  pool[32];
static uint32_t acc, events, counter;

static inline uint32_t tsc_lo(void) { uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); return lo; }

static void stir(const void* data, size_t len) {
    sha256_ctx c;
    sha256_init(&c); sha256_update(&c, pool, 32); sha256_update(&c, data, len); sha256_final(&c, pool);
}

void rng_event(uint32_t extra) {
    acc = ((acc << 5) | (acc >> 27)) ^ tsc_lo() ^ extra;
    if ((++events & 31) == 0) { uint32_t a = acc; stir(&a, sizeof(a)); }       /* fold into the pool every 32 events */
}

void rng_bytes(uint8_t* out, size_t n) {
    uint32_t seed[4] = { tsc_lo(), *(volatile uint32_t*)&tick, ++counter, acc };
    stir(seed, sizeof(seed));
    size_t done = 0;
    uint32_t block = 0;
    while (done < n) {
        uint8_t o[32];
        sha256_ctx c;
        sha256_init(&c); sha256_update(&c, pool, 32); sha256_update(&c, &block, 4); sha256_final(&c, o);
        block++;
        for (int i = 0; i < 32 && done < n; i++) out[done++] = o[i];
        for (int i = 0; i < 32; i++) ((volatile uint8_t*)o)[i] = 0;
    }
    stir(&block, sizeof(block));                                                 /* re-key: output cannot be rederived from the pool */
}
