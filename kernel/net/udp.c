/*
 * udp.c - UDP sockets (RFC 768).
 *
 * Each socket owns a small queue of received datagrams. Datagrams for ports
 * nobody listens on are silently dropped (no ICMP port-unreachable yet).
 * Like the rest of the stack, entry points run with interrupts disabled.
 */
#include "udp.h"
#include "net.h"

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[UDP_MAX_PAYLOAD];
} udp_dgram_t;

typedef struct {
    bool     used;
    uint16_t port;                   /* 0 = not bound yet */
    uint8_t  head, count;
    udp_dgram_t q[UDP_QUEUE_DEPTH];
} udp_sock_t;

static udp_sock_t socks[UDP_MAX_SOCKS];
static uint8_t    udp_buf[NET_MTU];
static uint16_t   next_port = 49152;

void udp_init(void) {
    memset(socks, 0, sizeof(socks));
    next_port = 49152;
}

static udp_sock_t* sk(int s) {
    if (s < 0 || s >= UDP_MAX_SOCKS || !socks[s].used) return NULL;
    return &socks[s];
}

static bool port_used(uint16_t port) {
    for (int i = 0; i < UDP_MAX_SOCKS; i++)
        if (socks[i].used && socks[i].port == port) return true;
    return false;
}

static uint16_t csum_udp(uint32_t src, uint32_t dst, const uint8_t* dgram, uint16_t len) {
    uint8_t ph[12];
    put_ip(ph, src);
    put_ip(ph + 4, dst);
    ph[8] = 0; ph[9] = IP_PROTO_UDP; ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    return net_csum_fold(net_csum_add(net_csum_add(0, ph, sizeof(ph)), dgram, len));
}

void udp_input(uint32_t src_ip, uint32_t dst_ip, const uint8_t* p, uint16_t len) {
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t* h = (const udp_hdr_t*)p;
    uint16_t ulen = ntohs(h->len);
    if (ulen < sizeof(udp_hdr_t) || ulen > len) return;
    if (h->csum != 0 && csum_udp(src_ip, dst_ip, p, ulen) != 0) return;   /* 0 = sender skipped it */

    net_stats.udp_rx++;
    uint16_t dport = ntohs(h->dport);
    for (int i = 0; i < UDP_MAX_SOCKS; i++) {
        udp_sock_t* s = &socks[i];
        if (!s->used || s->port != dport) continue;
        if (s->count >= UDP_QUEUE_DEPTH) return;                 /* queue full: drop */
        udp_dgram_t* d = &s->q[(s->head + s->count) % UDP_QUEUE_DEPTH];
        d->src_ip   = src_ip;
        d->src_port = ntohs(h->sport);
        d->len      = (uint16_t)(ulen - sizeof(udp_hdr_t));
        memcpy(d->data, p + sizeof(udp_hdr_t), d->len);
        s->count++;
        return;
    }
}

int udp_socket(void) {
    uint32_t f = net_lock();
    int r = UDP_ERR;
    for (int i = 0; i < UDP_MAX_SOCKS; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(udp_sock_t));
            socks[i].used = true;
            r = i;
            break;
        }
    }
    net_unlock(f);
    return r;
}

int udp_bind(int s, uint16_t port) {
    uint32_t f = net_lock();
    udp_sock_t* u = sk(s);
    int r = UDP_ERR;
    if (u && u->port == 0 && port != 0 && !port_used(port)) { u->port = port; r = 0; }
    net_unlock(f);
    return r;
}

int udp_sendto(int s, uint32_t ip, uint16_t port, const void* buf, int len) {
    ip = net_canon(ip);
    uint32_t f = net_lock();
    udp_sock_t* u = sk(s);
    if (!u || len < 0 || len > UDP_MAX_PAYLOAD) { net_unlock(f); return UDP_ERR; }

    if (u->port == 0) {                                            /* auto-bind */
        for (int i = 0; i < 16384 && u->port == 0; i++) {
            if (next_port < 49152) next_port = 49152;
            uint16_t p = next_port++;
            if (!port_used(p)) u->port = p;
        }
    }

    uint16_t total = (uint16_t)(sizeof(udp_hdr_t) + len);
    udp_hdr_t* h = (udp_hdr_t*)udp_buf;
    h->sport = htons(u->port);
    h->dport = htons(port);
    h->len   = htons(total);
    h->csum  = 0;
    memcpy(udp_buf + sizeof(udp_hdr_t), buf, (size_t)len);
    uint16_t c = csum_udp(netif.ip, ip, udp_buf, total);
    h->csum = c ? c : 0xFFFF;                                      /* 0 means "no checksum" */

    bool ok = net_ip_send(ip, IP_PROTO_UDP, udp_buf, total);
    if (ok) net_stats.udp_tx++;
    net_unlock(f);
    return ok ? len : UDP_ERR;
}

int udp_recvfrom(int s, void* buf, int max, uint32_t* src_ip, uint16_t* src_port, uint32_t timeout) {
    uint32_t start = net_now();
    for (;;) {
        uint32_t f = net_lock();
        udp_sock_t* u = sk(s);
        if (!u) { net_unlock(f); return UDP_ERR; }
        if (u->count > 0) {
            udp_dgram_t* d = &u->q[u->head];
            int n = d->len < max ? d->len : max;
            memcpy(buf, d->data, (size_t)n);
            if (src_ip)   *src_ip = d->src_ip;
            if (src_port) *src_port = d->src_port;
            u->head = (uint8_t)((u->head + 1) % UDP_QUEUE_DEPTH);
            u->count--;
            net_unlock(f);
            return n;
        }
        net_unlock(f);
        if (timeout == 0) return UDP_AGAIN;
        if (timeout != 0xFFFFFFFFu && (uint32_t)(net_now() - start) >= timeout) return UDP_TIMEOUT;
        net_wait();
    }
}

int udp_close(int s) {
    uint32_t f = net_lock();
    udp_sock_t* u = sk(s);
    if (u) u->used = false;
    net_unlock(f);
    return u ? 0 : UDP_ERR;
}
