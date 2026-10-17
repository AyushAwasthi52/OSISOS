/*
 * net.c - Ethernet, ARP, IPv4 and ICMP for OSISOS.
 *
 * Receive path:  net_poll() -> e1000_receive_packet() -> net_rx_frame()
 *                  -> arp_input() | ip_input() -> icmp_input() | tcp_input()
 * Transmit path: tcp/icmp -> net_ip_send() -> (ARP lookup) -> eth_send()
 *                  -> e1000_send_packet()
 *
 * Everything here runs with interrupts disabled (see net_lock in net.h).
 */
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "dhcp.h"
#include "e1000.h"
#include "pci.h"

extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);

uint8_t     net_loss_pct;

/* xorshift32 PRNG, only used for fault injection */
static bool loss_roll(void) {
    static uint32_t x = 2463534242u;
    if (net_loss_pct == 0) return false;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (x % 100) < net_loss_pct;
}

netif_t     netif;
net_stats_t net_stats;

static const uint8_t MAC_BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool mac_is_bcast(const uint8_t* m) { return memcmp(m, MAC_BCAST, 6) == 0; }

/* ================================================================== */
/* Ethernet                                                            */
/* ================================================================== */

static uint8_t tx_frame[ETH_HLEN + NET_MTU + 16] __attribute__((aligned(16)));

static void eth_send(const uint8_t* dst_mac, uint16_t type, const void* payload, uint16_t len) {
    if (len > NET_MTU) return;
    if (type == ETH_TYPE_IPV4 && loss_roll()) return;      /* simulated loss on the wire */

    eth_hdr_t* eh = (eth_hdr_t*)tx_frame;
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, netif.mac, 6);
    eh->type = htons(type);
    memcpy(tx_frame + ETH_HLEN, payload, len);

    uint16_t total = ETH_HLEN + len;
    if (total < 60) {                       /* pad to the 60-byte Ethernet minimum */
        memset(tx_frame + total, 0, 60 - total);
        total = 60;
    }
    e1000_send_packet(tx_frame, total);     /* driver copies the frame */
    net_stats.tx_frames++;
}

/* ================================================================== */
/* Internet checksum                                                   */
/* ================================================================== */

/* Sums 16-bit words in memory order (little-endian load). Because the
 * checksum is byte-order independent (RFC 1071), the folded result can be
 * stored into the packet as-is, with no htons(). */
uint32_t net_csum_add(uint32_t sum, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) {
        sum += (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p += 2;
        len -= 2;
    }
    if (len) sum += p[0];
    return sum;
}

uint16_t net_csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ================================================================== */
/* ARP                                                                 */
/* ================================================================== */

#define ARP_TTL_TICKS     (300 * NET_HZ)   /* entries live 5 minutes   */
#define ARP_RETRY_TICKS   NET_HZ           /* re-ask once per second   */
#define ARP_MAX_TRIES     3
#define ARP_PENDING_MAX   4

typedef struct {
    bool     valid;
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t updated;
} arp_entry_t;

/* An IP datagram waiting for an ARP reply so it can be sent. */
typedef struct {
    bool     used;
    uint32_t hop;
    uint16_t len;
    uint8_t  tries;
    uint32_t last;
    uint8_t  data[NET_MTU];
} arp_pending_t;

static arp_entry_t   arp_cache[ARP_CACHE_SIZE];
static arp_pending_t arp_pending[ARP_PENDING_MAX];
static int           arp_pending_rr;

static void arp_send(uint16_t oper, const uint8_t* tha, uint32_t tpa, const uint8_t* eth_dst) {
    arp_pkt_t a;
    a.htype = htons(1);
    a.ptype = htons(ETH_TYPE_IPV4);
    a.hlen  = 6;
    a.plen  = 4;
    a.oper  = htons(oper);
    memcpy(a.sha, netif.mac, 6);
    put_ip(a.spa, netif.ip);
    memcpy(a.tha, tha, 6);
    put_ip(a.tpa, tpa);
    eth_send(eth_dst, ETH_TYPE_ARP, &a, sizeof(a));
    net_stats.arp_tx++;
}

static void arp_request(uint32_t ip) {
    static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    arp_send(1, zero, ip, MAC_BCAST);
}

static arp_entry_t* arp_find(uint32_t ip) {
    uint32_t now = net_now();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_entry_t* e = &arp_cache[i];
        if (e->valid && e->ip == ip) {
            if ((int32_t)(now - e->updated) > ARP_TTL_TICKS) { e->valid = false; return NULL; }
            return e;
        }
    }
    return NULL;
}

bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    arp_entry_t* e = arp_find(ip);
    if (!e) return false;
    memcpy(mac, e->mac, 6);
    return true;
}

bool arp_get_entry(int idx, uint32_t* ip, uint8_t mac[6]) {
    if (idx < 0 || idx >= ARP_CACHE_SIZE) return false;
    uint32_t f = net_lock();
    bool ok = arp_cache[idx].valid;
    if (ok) { *ip = arp_cache[idx].ip; memcpy(mac, arp_cache[idx].mac, 6); }
    net_unlock(f);
    return ok;
}

static void arp_update(uint32_t ip, const uint8_t* mac) {
    arp_entry_t* slot = arp_find(ip);
    if (!slot) {                                   /* free slot, else evict the oldest */
        uint32_t now = net_now(), best_age = 0;
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!arp_cache[i].valid) { slot = &arp_cache[i]; break; }
            uint32_t age = now - arp_cache[i].updated;
            if (!slot || age > best_age) { slot = &arp_cache[i]; best_age = age; }
        }
    }
    slot->valid = true;
    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->updated = net_now();

    /* Release any datagrams that were waiting for this address. */
    for (int i = 0; i < ARP_PENDING_MAX; i++) {
        if (arp_pending[i].used && arp_pending[i].hop == ip) {
            eth_send(mac, ETH_TYPE_IPV4, arp_pending[i].data, arp_pending[i].len);
            arp_pending[i].used = false;
        }
    }
}

static void arp_queue(uint32_t hop, const uint8_t* ip_pkt, uint16_t len) {
    int slot = -1;
    bool already_asking = false;
    for (int i = 0; i < ARP_PENDING_MAX; i++) {
        if (!arp_pending[i].used) { if (slot < 0) slot = i; }
        else if (arp_pending[i].hop == hop) already_asking = true;
    }
    if (slot < 0) {                              /* full: evict round-robin */
        slot = arp_pending_rr;
        arp_pending_rr = (arp_pending_rr + 1) % ARP_PENDING_MAX;
    }
    arp_pending[slot].used  = true;
    arp_pending[slot].hop   = hop;
    arp_pending[slot].len   = len;
    arp_pending[slot].tries = 1;
    arp_pending[slot].last  = net_now();
    memcpy(arp_pending[slot].data, ip_pkt, len);
    if (!already_asking) arp_request(hop);
}

static void arp_timer(void) {
    uint32_t now = net_now();
    for (int i = 0; i < ARP_PENDING_MAX; i++) {
        arp_pending_t* p = &arp_pending[i];
        if (!p->used) continue;
        if ((int32_t)(now - p->last) >= ARP_RETRY_TICKS) {
            if (p->tries >= ARP_MAX_TRIES) { p->used = false; continue; }
            p->tries++;
            p->last = now;
            arp_request(p->hop);
        }
    }
}

static void arp_input(const uint8_t* p, uint16_t len) {
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t* a = (const arp_pkt_t*)p;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_TYPE_IPV4 || a->hlen != 6 || a->plen != 4) return;

    net_stats.arp_rx++;
    uint32_t spa = get_ip(a->spa);
    uint32_t tpa = get_ip(a->tpa);
    uint16_t oper = ntohs(a->oper);

    /* Only learn senders that talk to us, or that we already know
     * (stops unrelated hosts from filling / poisoning the cache). */
    if (spa != 0 && (tpa == netif.ip || arp_find(spa))) arp_update(spa, a->sha);

    if (oper == 1 && netif.ip != 0 && tpa == netif.ip) arp_send(2, a->sha, spa, a->sha);
}

/* ================================================================== */
/* IPv4                                                                */
/* ================================================================== */

static uint16_t ip_id_counter = 1;
static uint8_t  ip_buf[NET_MTU];

bool net_ip_send(uint32_t dst, uint8_t proto, const void* payload, uint16_t len) {
    if (len + sizeof(ip_hdr_t) > NET_MTU) return false;
    /* Before we have an address only broadcasts (DHCP DISCOVER/REQUEST) may be sent. */
    if (netif.ip == 0 && dst != 0xFFFFFFFF) return false;

    uint16_t total = (uint16_t)(sizeof(ip_hdr_t) + len);
    ip_hdr_t* h = (ip_hdr_t*)ip_buf;
    h->ver_ihl   = 0x45;
    h->tos       = 0;
    h->total_len = htons(total);
    h->id        = htons(ip_id_counter++);
    h->frag      = htons(0x4000);               /* Don't Fragment */
    h->ttl       = 64;
    h->proto     = proto;
    h->csum      = 0;
    put_ip(h->src, netif.ip);
    put_ip(h->dst, dst);
    h->csum = net_csum_fold(net_csum_add(0, h, sizeof(ip_hdr_t)));
    memcpy(ip_buf + sizeof(ip_hdr_t), payload, len);

    /* Broadcasts go straight to the Ethernet broadcast address. */
    if (dst == 0xFFFFFFFF || dst == (netif.ip | ~netif.mask)) {
        eth_send(MAC_BCAST, ETH_TYPE_IPV4, ip_buf, total);
        return true;
    }

    /* Next hop: the destination itself if it is on-link, else the gateway. */
    uint32_t hop = ((dst & netif.mask) == (netif.ip & netif.mask)) ? dst : netif.gateway;
    if (hop == 0) return false;

    uint8_t mac[6];
    if (arp_lookup(hop, mac)) {
        eth_send(mac, ETH_TYPE_IPV4, ip_buf, total);
    } else {
        arp_queue(hop, ip_buf, total);
    }
    return true;
}

/* ================================================================== */
/* ICMP                                                                */
/* ================================================================== */

#define PING_ID 0x4F53                         /* "OS" */

static uint8_t           icmp_buf[NET_MTU];
static volatile bool     ping_active;
static volatile bool     ping_got;
static volatile uint16_t ping_seq;
static volatile uint32_t ping_reply_tick;

static void icmp_input(uint32_t src, const uint8_t* p, uint16_t len) {
    if (len < sizeof(icmp_hdr_t)) return;
    if (net_csum_fold(net_csum_add(0, p, len)) != 0) return;   /* bad checksum */
    net_stats.icmp_rx++;

    const icmp_hdr_t* h = (const icmp_hdr_t*)p;
    if (h->type == 8) {                                         /* echo request -> reply */
        memcpy(icmp_buf, p, len);
        icmp_hdr_t* r = (icmp_hdr_t*)icmp_buf;
        r->type = 0;
        r->csum = 0;
        r->csum = net_csum_fold(net_csum_add(0, icmp_buf, len));
        net_ip_send(src, IP_PROTO_ICMP, icmp_buf, len);
    } else if (h->type == 0) {                                  /* echo reply */
        if (ping_active && ntohs(h->id) == PING_ID && ntohs(h->seq) == ping_seq) {
            ping_reply_tick = net_now();
            ping_got = true;
        }
    }
}

int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ticks) {
    uint8_t pkt[8 + 32];
    icmp_hdr_t* h = (icmp_hdr_t*)pkt;
    h->type = 8;
    h->code = 0;
    h->csum = 0;
    h->id   = htons(PING_ID);
    h->seq  = htons(seq);
    for (int i = 0; i < 32; i++) pkt[8 + i] = (uint8_t)('a' + (i % 26));
    h->csum = net_csum_fold(net_csum_add(0, pkt, sizeof(pkt)));

    uint32_t f = net_lock();
    ping_seq = seq;
    ping_got = false;
    ping_active = true;
    uint32_t start = net_now();
    bool sent = net_ip_send(dst, IP_PROTO_ICMP, pkt, sizeof(pkt));
    net_unlock(f);
    if (!sent) { ping_active = false; return -1; }

    for (;;) {
        f = net_lock();
        bool got = ping_got;
        uint32_t when = ping_reply_tick;
        net_unlock(f);
        if (got) { ping_active = false; return (int)(when - start); }
        if ((uint32_t)(net_now() - start) >= timeout_ticks) { ping_active = false; return -1; }
        net_wait();
    }
}

/* ================================================================== */
/* IP receive                                                          */
/* ================================================================== */

static void ip_input(const uint8_t* pkt, uint16_t len) {
    if (len < sizeof(ip_hdr_t)) { net_stats.ip_dropped++; return; }
    const ip_hdr_t* h = (const ip_hdr_t*)pkt;

    uint16_t ihl = (uint16_t)((h->ver_ihl & 0x0F) * 4);
    uint16_t total = ntohs(h->total_len);
    if ((h->ver_ihl >> 4) != 4 || ihl < sizeof(ip_hdr_t) || ihl > len ||
        total < ihl || total > len) { net_stats.ip_dropped++; return; }
    if (net_csum_fold(net_csum_add(0, pkt, ihl)) != 0) { net_stats.ip_dropped++; return; }

    /* No fragment reassembly: drop anything with MF set or a fragment offset. */
    if (ntohs(h->frag) & 0x3FFF) { net_stats.ip_dropped++; return; }

    uint32_t dst = get_ip(h->dst);
    uint32_t src = get_ip(h->src);
    bool unicast = (netif.ip != 0 && dst == netif.ip);
    bool bcast   = (dst == 0xFFFFFFFF) || (netif.ip != 0 && dst == (netif.ip | ~netif.mask));
    if (!unicast && !bcast) { net_stats.ip_dropped++; return; }

    net_stats.ip_rx++;
    const uint8_t* payload = pkt + ihl;
    uint16_t plen = (uint16_t)(total - ihl);

    switch (h->proto) {
        case IP_PROTO_ICMP: icmp_input(src, payload, plen); break;
        case IP_PROTO_TCP:  if (unicast) tcp_input(src, payload, plen); break;
        case IP_PROTO_UDP:  udp_input(src, dst, payload, plen); break;
        default: break;                         /* UDP etc.: not implemented yet */
    }
}

/* ================================================================== */
/* Frame dispatch and polling                                          */
/* ================================================================== */

static void net_rx_frame(const uint8_t* f, uint16_t len) {
    if (len < ETH_HLEN) return;
    const eth_hdr_t* e = (const eth_hdr_t*)f;

    /* The e1000 is configured promiscuous, so filter by destination MAC here. */
    if (memcmp(e->dst, netif.mac, 6) != 0 && !mac_is_bcast(e->dst)) {
        net_stats.rx_dropped++;
        return;
    }
    net_stats.rx_frames++;

    switch (ntohs(e->type)) {
        case ETH_TYPE_ARP:  arp_input(f + ETH_HLEN, len - ETH_HLEN); break;
        case ETH_TYPE_IPV4:
            if (loss_roll()) break;                             /* simulated loss */
            ip_input(f + ETH_HLEN, len - ETH_HLEN);
            break;
        case ETH_TYPE_LEGACY:                   /* original OSISOS raw "ping" demo */
            terminal_print_string("\n[Network] MSG: ");
            for (uint16_t i = ETH_HLEN; i < len && f[i] != '\0'; i++) terminal_putchar((char)f[i]);
            terminal_print_string("\n> ");
            break;
        default: break;
    }
}

void net_poll(void) {
    for (int i = 0; i < 64; i++) {
        uint32_t f = net_lock();
        uint16_t len = 0;
        /* The returned buffer stays valid until the next e1000_receive_packet(). */
        uint8_t* frame = (uint8_t*)e1000_receive_packet(&len);
        if (frame) net_rx_frame(frame, len);
        net_unlock(f);
        if (!frame) break;
    }
    uint32_t f = net_lock();
    arp_timer();
    tcp_timer();
    net_unlock(f);
}

/* ================================================================== */
/* Init / configuration                                                */
/* ================================================================== */

void net_set_ip(uint32_t ip, uint32_t mask, uint32_t gateway) {
    uint32_t f = net_lock();
    netif.ip = ip;
    netif.mask = mask;
    netif.gateway = gateway;
    memset(arp_cache, 0, sizeof(arp_cache));
    if (ip != 0) arp_request(ip);               /* gratuitous ARP: announce ourselves */
    net_unlock(f);
}

void net_init(void) {
    memset(&netif, 0, sizeof(netif));
    memset(&net_stats, 0, sizeof(net_stats));
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(arp_pending, 0, sizeof(arp_pending));
    memcpy(netif.mac, e1000_mac, 6);

    /* The address comes from DHCP (see dhcp.c). If no server answers within ~15 s a
     * static fallback (10.0.2.15, QEMU's user network) is installed. Override at any
     * time with:  ifconfig <ip> <mask> <gateway>   |   dhcp renew */
    udp_init();
    tcp_init();
    net_services_init();
    dhcp_init();
}

/* ------------------------------------------------------------------ */

bool net_parse_ip(const char* s, uint32_t* out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) return false;
            s++;
        }
        ip = (ip << 8) | v;
        if (part < 3) { if (*s != '.') return false; s++; }
    }
    if (*s != '\0') return false;
    *out = ip;
    return true;
}
