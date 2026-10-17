/*
 * dhcp.c - DHCP client (RFC 2131), event driven.
 *
 *   INIT -> SELECTING -> REQUESTING -> BOUND -> RENEWING -> BOUND ...
 *                |            |                     |
 *            (4 tries)    (NAK / timeout)      (lease expires)
 *                v                                  v
 *            FALLBACK  <------- INIT <--------------+
 *
 * - DISCOVER/REQUEST are broadcast with src 0.0.0.0 while we have no address;
 *   we set the BROADCAST flag so servers answer to 255.255.255.255 (we cannot
 *   receive unicast to an address we do not own yet).
 * - At T1 (50% of the lease) we unicast a REQUEST to the server; after T2
 *   (87.5%) we broadcast it; at expiry we drop the address and start over.
 * - If nobody answers, a static fallback address is installed (matching QEMU's
 *   user network) and discovery is retried once a minute in the background.
 *
 * Not implemented: ARP conflict probing before use, DECLINE, RELEASE, INIT-REBOOT.
 */
#include "dhcp.h"
#include "udp.h"
#include "net.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

#define OPT_MASK     1
#define OPT_ROUTER   3
#define OPT_DNS      6
#define OPT_HOSTNAME 12
#define OPT_REQ_IP   50
#define OPT_LEASE    51
#define OPT_MSGTYPE  53
#define OPT_SERVER   54
#define OPT_PARAMS   55
#define OPT_END      255

#define MAX_LEASE_SECS   (30u * 86400u)         /* cap so tick arithmetic cannot overflow */
#define MAX_DISCOVER_TRIES 4
#define FALLBACK_RETRY_TICKS (60 * NET_HZ)

typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
} __attribute__((packed)) dhcp_hdr_t;           /* 240 bytes, options follow */

typedef struct {
    uint8_t  type;
    uint32_t yiaddr, server, mask, router, dns, lease;
} dhcp_reply_t;

static struct {
    dhcp_state_t state;
    int      sock;
    uint32_t xid;
    uint32_t server;
    uint32_t offered;
    uint8_t  tries;
    uint32_t t_sent;                            /* when we last transmitted */
    uint32_t t1, t2, expire;                    /* absolute tick deadlines  */
    bool     send_now;                          /* force a transmission on the next poll */
} d;

static uint8_t rxbuf[600];

static uint32_t new_xid(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (net_now() * 2654435761u) ^ ((uint32_t)netif.mac[5] << 24);
}

static bool elapsed(uint32_t since, uint32_t ticks) { return (uint32_t)(net_now() - since) >= ticks; }

static const char* names[] = { "INIT", "SELECTING", "REQUESTING", "BOUND", "RENEWING", "FALLBACK", "STATIC" };
const char*  dhcp_state_name(void) { return names[d.state]; }
dhcp_state_t dhcp_state(void)      { return d.state; }
uint32_t     dhcp_server(void)     { return d.server; }

uint32_t dhcp_lease_left_secs(void) {
    if (d.state != DHCP_BOUND && d.state != DHCP_RENEWING) return 0;
    int32_t left = (int32_t)(d.expire - net_now());
    return left > 0 ? (uint32_t)left / NET_HZ : 0;
}

/* ---------------------------------------------------------------- send */

static void dhcp_send(uint8_t type, uint32_t ciaddr, uint32_t req_ip, uint32_t server, uint32_t dst) {
    static uint8_t buf[300];
    memset(buf, 0, sizeof(buf));                /* also pads to the 300-byte BOOTP minimum */

    dhcp_hdr_t* h = (dhcp_hdr_t*)buf;
    h->op = 1; h->htype = 1; h->hlen = 6;
    h->xid    = htonl(d.xid);
    h->flags  = htons(0x8000);                  /* ask for a broadcast reply */
    put_ip(h->ciaddr, ciaddr);
    memcpy(h->chaddr, netif.mac, 6);
    h->cookie = htonl(DHCP_MAGIC);

    uint8_t* o = buf + sizeof(dhcp_hdr_t);
    *o++ = OPT_MSGTYPE; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = OPT_REQ_IP; *o++ = 4; put_ip(o, req_ip); o += 4; }
    if (server) { *o++ = OPT_SERVER; *o++ = 4; put_ip(o, server); o += 4; }
    *o++ = OPT_HOSTNAME; *o++ = 6; memcpy(o, "osisos", 6); o += 6;
    *o++ = OPT_PARAMS; *o++ = 4; *o++ = OPT_MASK; *o++ = OPT_ROUTER; *o++ = OPT_DNS; *o++ = OPT_LEASE;
    *o++ = OPT_END;

    udp_sendto(d.sock, dst, DHCP_SERVER_PORT, buf, sizeof(buf));
    d.t_sent = net_now();
}

/* -------------------------------------------------------------- parse */

static bool parse_reply(const uint8_t* p, int len, dhcp_reply_t* r) {
    if (len < (int)sizeof(dhcp_hdr_t) + 4) return false;
    const dhcp_hdr_t* h = (const dhcp_hdr_t*)p;
    if (h->op != 2 || ntohl(h->xid) != d.xid || ntohl(h->cookie) != DHCP_MAGIC) return false;
    if (memcmp(h->chaddr, netif.mac, 6) != 0) return false;

    memset(r, 0, sizeof(*r));
    r->yiaddr = get_ip(h->yiaddr);

    const uint8_t* o = p + sizeof(dhcp_hdr_t);
    const uint8_t* end = p + len;
    while (o < end) {
        uint8_t code = *o++;
        if (code == 0) continue;                /* pad */
        if (code == OPT_END || o >= end) break;
        uint8_t l = *o++;
        if (o + l > end) break;
        switch (code) {
            case OPT_MSGTYPE: if (l >= 1) r->type = o[0];            break;
            case OPT_SERVER:  if (l >= 4) r->server = get_ip(o);     break;
            case OPT_MASK:    if (l >= 4) r->mask = get_ip(o);       break;
            case OPT_ROUTER:  if (l >= 4) r->router = get_ip(o);     break;   /* first router */
            case OPT_DNS:     if (l >= 4) r->dns = get_ip(o);        break;   /* first server */
            case OPT_LEASE:   if (l >= 4) r->lease = ntohl(*(const uint32_t*)o); break;
            default: break;
        }
        o += l;
    }
    return r->type != 0;
}

/* -------------------------------------------------------------- state */

static void fallback(void) {
    uint32_t ip = (netif.mac[5] == 0x57) ? IP4(10, 0, 2, 16) : IP4(10, 0, 2, 15);
    if (netif.ip != ip) {
        net_set_ip(ip, IP4(255, 255, 255, 0), IP4(10, 0, 2, 2));
        netif.dns = IP4(10, 0, 2, 3);
    }
    d.state = DHCP_FALLBACK;
    d.t_sent = net_now();
}

static void apply_lease(const dhcp_reply_t* r) {
    uint32_t mask = r->mask ? r->mask : IP4(255, 255, 255, 0);
    if (r->yiaddr != netif.ip || mask != netif.mask || r->router != netif.gateway)
        net_set_ip(r->yiaddr, mask, r->router);
    if (r->dns) netif.dns = r->dns;
    else if (r->router && netif.dns == 0) netif.dns = r->router;

    uint32_t lease = r->lease ? r->lease : 3600;
    if (lease > MAX_LEASE_SECS) lease = MAX_LEASE_SECS;        /* includes "infinite" */
    uint32_t now = net_now();
    d.t1     = now + (lease / 2) * NET_HZ;
    d.t2     = now + (lease / 8 * 7) * NET_HZ;
    d.expire = now + lease * NET_HZ;
    d.server = r->server ? r->server : d.server;
    d.state  = DHCP_BOUND;
    d.tries  = 0;
}

static void start_discover(void) {
    d.xid = new_xid();
    d.state = DHCP_SELECTING;
    dhcp_send(DHCPDISCOVER, 0, 0, 0, 0xFFFFFFFF);
}

void dhcp_init(void) {
    memset(&d, 0, sizeof(d));
    d.sock = udp_socket();
    if (d.sock >= 0) udp_bind(d.sock, DHCP_CLIENT_PORT);
    netif.ip = 0;
    netif.dns = 0;
    d.state = DHCP_INIT;
}

void dhcp_set_static(void) { d.state = DHCP_STATIC; }

void dhcp_renew_now(void) {
    if (d.state == DHCP_BOUND) { d.state = DHCP_RENEWING; d.send_now = true; }
    else d.state = DHCP_INIT;
    d.tries = 0;
}

void dhcp_poll(void) {
    if (d.sock < 0 || d.state == DHCP_STATIC) return;

    /* 1. incoming replies */
    for (;;) {
        int n = udp_recvfrom(d.sock, rxbuf, sizeof(rxbuf), NULL, NULL, 0);
        if (n < 0) break;
        dhcp_reply_t r;
        if (!parse_reply(rxbuf, n, &r)) continue;

        switch (d.state) {
            case DHCP_SELECTING:
                if (r.type == DHCPOFFER && r.server) {
                    d.offered = r.yiaddr;
                    d.server  = r.server;
                    d.state   = DHCP_REQUESTING;
                    d.tries   = 0;
                    dhcp_send(DHCPREQUEST, 0, r.yiaddr, r.server, 0xFFFFFFFF);
                }
                break;
            case DHCP_REQUESTING:
            case DHCP_RENEWING:
                if (r.type == DHCPACK) {
                    if (!r.server) r.server = d.server;
                    apply_lease(&r);
                } else if (r.type == DHCPNAK) {
                    if (d.state == DHCP_RENEWING) net_set_ip(0, 0, 0);
                    d.state = DHCP_INIT;
                    d.tries = 0;
                }
                break;
            default: break;
        }
    }

    /* 2. timers */
    switch (d.state) {
        case DHCP_INIT:
            start_discover();
            break;

        case DHCP_SELECTING:
            if (elapsed(d.t_sent, (uint32_t)NET_HZ << d.tries)) {          /* 1 s, 2 s, 4 s, 8 s */
                if (++d.tries >= MAX_DISCOVER_TRIES) fallback();
                else dhcp_send(DHCPDISCOVER, 0, 0, 0, 0xFFFFFFFF);
            }
            break;

        case DHCP_REQUESTING:
            if (elapsed(d.t_sent, (uint32_t)NET_HZ << d.tries)) {
                if (++d.tries >= 3) { d.state = DHCP_INIT; d.tries = 0; }
                else dhcp_send(DHCPREQUEST, 0, d.offered, d.server, 0xFFFFFFFF);
            }
            break;

        case DHCP_BOUND:
            if ((int32_t)(net_now() - d.t1) >= 0) {
                d.state = DHCP_RENEWING;
                d.send_now = true;
            }
            break;

        case DHCP_RENEWING:
            if ((int32_t)(net_now() - d.expire) >= 0) {                    /* lease ran out */
                net_set_ip(0, 0, 0);
                d.state = DHCP_INIT;
                break;
            }
            if (d.send_now || elapsed(d.t_sent, 10 * NET_HZ)) {
                d.send_now = false;
                d.xid = new_xid();
                /* unicast to the server until T2, then broadcast to any server */
                uint32_t dst = ((int32_t)(net_now() - d.t2) >= 0 || !d.server) ? 0xFFFFFFFF : d.server;
                dhcp_send(DHCPREQUEST, netif.ip, 0, 0, dst);
            }
            break;

        case DHCP_FALLBACK:
            if (elapsed(d.t_sent, FALLBACK_RETRY_TICKS)) { d.tries = 0; d.state = DHCP_INIT; }
            break;

        default: break;
    }
}
