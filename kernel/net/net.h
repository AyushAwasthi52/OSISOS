#ifndef NET_H
#define NET_H

/*
 * OSISOS network stack - common definitions.
 *
 * Layering (bottom to top):
 *
 *   e1000 driver  (drivers/e1000.c)      raw Ethernet frames
 *   net.c         Ethernet + ARP + IPv4 + ICMP
 *   tcp.c         TCP state machine + socket API
 *   services.c    built-in echo (7) and HTTP (80) servers
 *   netcmd.c      shell commands: ifconfig arp ping netstat nc http
 *
 * Conventions:
 *   - IPv4 addresses in the public API are uint32_t in HOST byte order
 *     (10.0.2.15 == IP4(10,0,2,15) == 0x0A00020F).
 *   - Inside packet structs, addresses are byte arrays in wire order, and
 *     16/32-bit fields are in network byte order (use htons/htonl).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define NET_HZ          100          /* timer ticks per second (init_timer(100)) */
#define NET_MTU         1500         /* max IP datagram we send/receive           */
#define ETH_HLEN        14

#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_LEGACY 0x1337       /* the original OSISOS "PING!" demo frames   */

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6

#define IP4(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                      ((uint32_t)(c) << 8)  |  (uint32_t)(d))

/* ------------------------------------------------------------------ */
/* Byte order helpers (x86 is little-endian)                           */
/* ------------------------------------------------------------------ */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* Read / write a wire-order IPv4 address (4 bytes) as a host-order uint32_t */
static inline uint32_t get_ip(const uint8_t* b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static inline void put_ip(uint8_t* b, uint32_t ip) {
    b[0] = (uint8_t)(ip >> 24); b[1] = (uint8_t)(ip >> 16);
    b[2] = (uint8_t)(ip >> 8);  b[3] = (uint8_t)ip;
}

/* ------------------------------------------------------------------ */
/* Wire formats                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t htype;      /* 1 = Ethernet */
    uint16_t ptype;      /* 0x0800 = IPv4 */
    uint8_t  hlen;       /* 6 */
    uint8_t  plen;       /* 4 */
    uint16_t oper;       /* 1 = request, 2 = reply */
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl;    /* version (4) << 4 | header length in 32-bit words */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;       /* flags (3 bits) + fragment offset (13 bits) */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint8_t  type;       /* 8 = echo request, 0 = echo reply */
    uint8_t  code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef struct {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;        /* data offset (32-bit words) << 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed)) tcp_hdr_t;

/* ------------------------------------------------------------------ */
/* Interface configuration and statistics                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;         /* host order, 0 = unconfigured */
    uint32_t mask;
    uint32_t gateway;    /* 0 = none (on-link only)      */
} netif_t;

typedef struct {
    uint32_t rx_frames, tx_frames, rx_dropped;
    uint32_t arp_rx, arp_tx;
    uint32_t ip_rx, ip_dropped;
    uint32_t icmp_rx;
    uint32_t tcp_rx, tcp_tx, tcp_badsum, tcp_retx, tcp_rst;
    uint32_t tcp_syn_drop;         /* SYNs dropped (backlog full / no free socket) */
    uint32_t tcp_retx_state[11];   /* RTO expirations per tcp_state_t (debugging)   */
} net_stats_t;

/* Fault injection for testing/teaching: drop this % of IPv4 frames (rx and tx). */
extern uint8_t      net_loss_pct;

extern netif_t      netif;
extern net_stats_t  net_stats;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

/* Bring the stack up (call once after init_e1000 succeeded). */
void net_init(void);

/* Drain the NIC receive ring and run all protocol timers.
 * Called repeatedly by the network task. */
void net_poll(void);

/* Set IP / netmask / default gateway (host order). */
void net_set_ip(uint32_t ip, uint32_t mask, uint32_t gateway);

/* Send an IPv4 datagram. Resolves the next hop via ARP (queues the packet
 * if the MAC is not known yet). Returns false if it could not be sent. */
bool net_ip_send(uint32_t dst, uint8_t proto, const void* payload, uint16_t len);

/* Blocking ICMP echo. Returns round-trip time in ticks (>=0), or -1 on timeout. */
int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ticks);

/* ARP cache inspection (for the `arp` shell command). idx in [0, ARP_CACHE_SIZE). */
#define ARP_CACHE_SIZE 8
bool arp_lookup(uint32_t ip, uint8_t mac[6]);
bool arp_get_entry(int idx, uint32_t* ip, uint8_t mac[6]);

/* Internet checksum (RFC 1071). Accumulate with net_csum_add(), then fold. */
uint32_t net_csum_add(uint32_t sum, const void* data, size_t len);
uint16_t net_csum_fold(uint32_t sum);

/* ------------------------------------------------------------------ */
/* Time, locking, waiting                                              */
/* ------------------------------------------------------------------ */

extern uint32_t tick;                      /* defined in drivers/timer.c, 100 Hz */
static inline uint32_t net_now(void) { return *(volatile uint32_t*)&tick; }

/* The stack is shared between the shell task and the network task, and the
 * PIT preempts tasks. Every public entry point therefore runs with
 * interrupts disabled. Never call task_yield() / net_wait() while holding it. */
static inline uint32_t net_lock(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void net_unlock(uint32_t flags) {
    if (flags & 0x200) __asm__ volatile ("sti" : : : "memory");
}

/* Sleep until the next interrupt (the timer preempts us to other tasks). */
static inline void net_wait(void) { __asm__ volatile ("sti; hlt" : : : "memory"); }

/* ------------------------------------------------------------------ */
/* Tiny libc (the kernel is freestanding; GCC also emits calls to these) */
/* ------------------------------------------------------------------ */

void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t net_strlen(const char* s);
int    net_strcmp(const char* a, const char* b);

/* ------------------------------------------------------------------ */
/* Services and shell integration                                      */
/* ------------------------------------------------------------------ */

void net_services_init(void);      /* start listeners on port 7 and 80 */
void net_services_poll(void);      /* serve connections (non-blocking) */

/* Try to run `line` as a network shell command. Returns true if handled. */
bool net_shell_command(const char* line);

bool net_parse_ip(const char* s, uint32_t* out);

#endif
