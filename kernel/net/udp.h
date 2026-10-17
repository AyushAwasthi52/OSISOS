#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * OSISOS UDP - datagram sockets.
 *
 *     int s = udp_socket();
 *     udp_bind(s, 5000);                                  // optional: sendto auto-binds
 *     udp_sendto(s, IP4(10,0,2,2), 5000, "hi", 2);
 *     n = udp_recvfrom(s, buf, sizeof buf, &src_ip, &src_port, TCP_NOWAIT);
 *     udp_close(s);
 *
 * Timeouts are in ticks (100/s): 0 = non-blocking, 0xFFFFFFFF = forever.
 */

#define UDP_MAX_SOCKS     8
#define UDP_QUEUE_DEPTH   4          /* datagrams buffered per socket (extra ones are dropped) */
#define UDP_MAX_PAYLOAD   1472       /* 1500 - 20 (IP) - 8 (UDP); no fragmentation          */

#define UDP_ERR          (-1)
#define UDP_AGAIN        (-2)
#define UDP_TIMEOUT      (-3)

typedef struct {
    uint16_t sport, dport;
    uint16_t len;                    /* header + payload */
    uint16_t csum;
} __attribute__((packed)) udp_hdr_t;

void udp_init(void);
void udp_input(uint32_t src_ip, uint32_t dst_ip, const uint8_t* dgram, uint16_t len);

int  udp_socket(void);
int  udp_bind(int s, uint16_t port);
int  udp_sendto(int s, uint32_t ip, uint16_t port, const void* buf, int len);
/* Returns bytes copied (truncated to `max`), or a negative UDP_* error. */
int  udp_recvfrom(int s, void* buf, int max, uint32_t* src_ip, uint16_t* src_port, uint32_t timeout);
int  udp_close(int s);

#endif
