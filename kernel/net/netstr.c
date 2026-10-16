/*
 * netstr.c - the handful of libc routines the network stack needs.
 *
 * memcpy/memset use `rep movsb/stosb` on purpose: with -O2, GCC turns a plain
 * C copy loop back into a call to memcpy(), which would recurse forever.
 */
#include "net.h"

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    /* Overlapping with dst after src: copy backwards (direction flag set). */
    d += n - 1;
    s += n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t net_strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int net_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
