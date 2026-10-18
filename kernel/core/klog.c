#include "klog.h"
#include <stdarg.h>
#include <stdbool.h>

extern uint32_t tick;                        /* drivers/timer.c, 100 Hz */

#define KLOG_SIZE 4096                       /* power of two */

static char     ring[KLOG_SIZE];
static uint32_t head;                        /* total bytes ever written */

static inline uint32_t lock(void) {
    uint32_t f; __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory"); return f;
}
static inline void unlock(uint32_t f) { if (f & 0x200) __asm__ volatile ("sti" : : : "memory"); }

static void put(char c) { ring[head++ & (KLOG_SIZE - 1)] = c; }
static void puts_(const char* s) { while (*s) put(*s++); }

static void put_u(uint32_t v, int min_digits) {
    char tmp[11]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v || n < min_digits);
    while (n) put(tmp[--n]);
}

void klog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t f = lock();

    uint32_t t = *(volatile uint32_t*)&tick;
    put('[');
    put_u(t / 100, 5); put('.'); put_u(t % 100, 2);
    puts_("] ");

    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (*fmt != '\n') put(*fmt); continue; }
        fmt++;
        switch (*fmt) {
            case 's': { const char* s = va_arg(ap, const char*); puts_(s ? s : "(null)"); break; }
            case 'u': put_u(va_arg(ap, uint32_t), 1); break;
            case 'd': { int v = va_arg(ap, int); if (v < 0) { put('-'); put_u((uint32_t)-v, 1); } else put_u((uint32_t)v, 1); break; }
            case 'I': { uint32_t ip = va_arg(ap, uint32_t);
                        put_u(ip >> 24, 1); put('.'); put_u((ip >> 16) & 255, 1); put('.');
                        put_u((ip >> 8) & 255, 1); put('.'); put_u(ip & 255, 1); break; }
            case '%': put('%'); break;
            case '\0': fmt--; break;
            default: put('%'); put(*fmt); break;
        }
    }
    put('\n');

    unlock(f);
    va_end(ap);
}

uint32_t klog_read(char* out, uint32_t out_size, int max_lines) {
    if (out_size == 0) return 0;
    uint32_t f = lock();

    uint32_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    uint32_t start = head - avail;
    /* skip a partial first line if the ring has wrapped */
    if (head > KLOG_SIZE) { while (start < head && ring[start & (KLOG_SIZE - 1)] != '\n') start++; start++; }

    /* walk back from the end to find where the last `max_lines` lines begin */
    uint32_t from = head;
    int lines = 0;
    while (from > start) {
        if (ring[(from - 1) & (KLOG_SIZE - 1)] == '\n' && from != head) { if (++lines >= max_lines) break; }
        from--;
    }

    uint32_t n = 0;
    for (uint32_t i = from; i < head && n + 1 < out_size; i++) out[n++] = ring[i & (KLOG_SIZE - 1)];
    out[n] = '\0';
    unlock(f);
    return n;
}
