/*
 * dns.c - minimal stub resolver (RFC 1035): one recursive query, A records only.
 *
 * We ask the configured server (recursion desired) and take the first A record
 * in the answer section. A recursive server includes the A record for the final
 * target after any CNAMEs, so CNAME chains work without extra queries.
 * Not implemented: AAAA, TCP fallback for truncated answers, search domains.
 */
#include "dns.h"
#include "udp.h"
#include "net.h"

#define DNS_PORT        53
#define DNS_MAX_NAME    253
#define DNS_CACHE_SIZE  8
#define DNS_MAX_TTL     300
#define DNS_TRIES       3

typedef struct {
    bool     valid;
    char     name[DNS_MAX_NAME + 1];
    uint32_t ip;
    uint32_t expires;                          /* tick */
} dns_cache_t;

static dns_cache_t cache[DNS_CACHE_SIZE];
static int cache_rr;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool name_eq(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static bool cache_get(const char* name, uint32_t* ip) {
    uint32_t now = net_now();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (cache[i].valid && name_eq(cache[i].name, name)) {
            if ((int32_t)(now - cache[i].expires) >= 0) { cache[i].valid = false; return false; }
            *ip = cache[i].ip;
            return true;
        }
    }
    return false;
}

static void cache_put(const char* name, uint32_t ip, uint32_t ttl) {
    if (ttl == 0) return;
    if (ttl > DNS_MAX_TTL) ttl = DNS_MAX_TTL;
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (cache[i].valid && name_eq(cache[i].name, name)) { slot = i; break; }
        if (!cache[i].valid && slot < 0) slot = i;
    }
    if (slot < 0) { slot = cache_rr; cache_rr = (cache_rr + 1) % DNS_CACHE_SIZE; }
    size_t n = net_strlen(name);
    memcpy(cache[slot].name, name, n + 1);
    cache[slot].ip = ip;
    cache[slot].expires = net_now() + ttl * NET_HZ;
    cache[slot].valid = true;
}

/* Encode "a.b.c" as 1a1b1c0. Returns bytes written or -1 on a bad name. */
static int encode_name(const char* name, uint8_t* out, int max) {
    int pos = 0;
    while (*name) {
        const char* dot = name;
        while (*dot && *dot != '.') {
            char c = *dot;
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return -1;
            dot++;
        }
        int l = (int)(dot - name);
        if (l == 0 || l > 63 || pos + 1 + l + 1 > max) return -1;
        out[pos++] = (uint8_t)l;
        memcpy(out + pos, name, (size_t)l);
        pos += l;
        name = *dot ? dot + 1 : dot;
    }
    if (pos == 0 || pos + 1 > max) return -1;
    out[pos++] = 0;
    return pos;
}

/* Skip a (possibly compressed) name; returns the offset just after it, or -1. */
static int skip_name(const uint8_t* m, int len, int off) {
    while (off < len) {
        uint8_t l = m[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return (off + 2 <= len) ? off + 2 : -1;   /* pointer ends the name */
        if (l & 0xC0) return -1;
        off += 1 + l;
    }
    return -1;
}

/* Parse a response. Returns DNS_OK (filling the ip and ttl outputs) or an error code. */
static int parse_response(const uint8_t* m, int len, uint16_t id, uint32_t* ip, uint32_t* ttl) {
    if (len < 12) return DNS_ERR;
    if (((m[0] << 8) | m[1]) != id) return DNS_ERR;               /* not ours: caller keeps waiting */
    if (!(m[2] & 0x80)) return DNS_ERR;                           /* not a response */
    int rcode = m[3] & 0x0F;
    if (rcode == 3) return DNS_NXDOMAIN;
    if (rcode != 0) return DNS_FAIL;

    int qd = (m[4] << 8) | m[5];
    int an = (m[6] << 8) | m[7];
    int off = 12;
    for (int i = 0; i < qd; i++) {
        off = skip_name(m, len, off);
        if (off < 0 || off + 4 > len) return DNS_FAIL;
        off += 4;                                                  /* QTYPE + QCLASS */
    }
    for (int i = 0; i < an; i++) {
        off = skip_name(m, len, off);
        if (off < 0 || off + 10 > len) return DNS_FAIL;
        int type  = (m[off] << 8) | m[off + 1];
        int cls   = (m[off + 2] << 8) | m[off + 3];
        uint32_t t = ((uint32_t)m[off + 4] << 24) | ((uint32_t)m[off + 5] << 16) |
                     ((uint32_t)m[off + 6] << 8) | m[off + 7];
        int rdlen = (m[off + 8] << 8) | m[off + 9];
        off += 10;
        if (off + rdlen > len) return DNS_FAIL;
        if (type == 1 && cls == 1 && rdlen == 4) { *ip = get_ip(m + off); *ttl = t; return DNS_OK; }
        off += rdlen;
    }
    return DNS_FAIL;                                               /* no A record */
}

int dns_resolve(const char* name, uint32_t* ip, uint32_t timeout_ticks) {
    char clean[DNS_MAX_NAME + 1];
    size_t n = net_strlen(name);
    if (n == 0 || n > DNS_MAX_NAME) return DNS_ERR;
    memcpy(clean, name, n + 1);
    if (clean[n - 1] == '.') clean[n - 1] = '\0';                  /* "example.com." */

    if (cache_get(clean, ip)) return DNS_OK;
    if (netif.dns == 0 || netif.ip == 0) return DNS_ERR;

    uint8_t q[12 + DNS_MAX_NAME + 2 + 4];
    memset(q, 0, sizeof(q));
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint16_t id = (uint16_t)(lo ^ (lo >> 16) ^ net_now());
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01;                                                   /* RD: recursion desired */
    q[5] = 1;                                                      /* one question */
    int nl = encode_name(clean, q + 12, (int)sizeof(q) - 12 - 4);
    if (nl < 0) return DNS_ERR;
    int qlen = 12 + nl;
    q[qlen++] = 0; q[qlen++] = 1;                                  /* QTYPE  = A  */
    q[qlen++] = 0; q[qlen++] = 1;                                  /* QCLASS = IN */

    int s = udp_socket();
    if (s < 0) return DNS_ERR;

    uint32_t start = net_now();
    uint32_t per_try = timeout_ticks / DNS_TRIES;
    if (per_try < NET_HZ) per_try = NET_HZ;
    int result = DNS_TIMEOUT;

    for (int attempt = 0; attempt < DNS_TRIES && result == DNS_TIMEOUT; attempt++) {
        if (udp_sendto(s, netif.dns, DNS_PORT, q, qlen) < 0) { result = DNS_ERR; break; }
        uint32_t sent = net_now();
        while ((uint32_t)(net_now() - sent) < per_try && (uint32_t)(net_now() - start) < timeout_ticks) {
            uint8_t resp[512];
            uint32_t from; uint16_t fport;
            uint32_t left = per_try - (net_now() - sent);
            int r = udp_recvfrom(s, resp, sizeof(resp), &from, &fport, left ? left : 1);
            if (r < 0) break;                                      /* timed out: next attempt */
            if (from != netif.dns || fport != DNS_PORT) continue;  /* spoof / stray packet */
            uint32_t ttl = 0, a = 0;
            int pr = parse_response(resp, r, id, &a, &ttl);
            if (pr == DNS_OK) { *ip = a; cache_put(clean, a, ttl); result = DNS_OK; break; }
            if (pr == DNS_NXDOMAIN || pr == DNS_FAIL) { result = pr; break; }
            /* DNS_ERR here means "wrong id": keep waiting */
        }
    }
    udp_close(s);
    return result;
}
