#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * OSISOS TCP - socket-style API.
 *
 * A socket is an small integer handle (index into a fixed table).
 *
 * Active open (client):            Passive open (server):
 *     s = tcp_socket();                l = tcp_socket();
 *     tcp_connect(s, ip, port, t);     tcp_listen(l, port);
 *     tcp_send(s, ...);                c = tcp_accept(l, t);
 *     tcp_recv(s, ...);                tcp_recv(c, ...); tcp_send(c, ...);
 *     tcp_close(s);                    tcp_close(c);
 *
 * Every call that can block takes a timeout in ticks (100 per second):
 *     TCP_NOWAIT   (0)           return TCP_AGAIN immediately if it would block
 *     TCP_FOREVER  (0xFFFFFFFF)  wait as long as it takes
 */

#define TCP_MAX_SOCKS      12
#define TCP_RX_BUF         4096          /* receive buffer per socket (power of 2) */
#define TCP_TX_BUF         4096          /* send buffer per socket    (power of 2) */
#define TCP_MSS            1460          /* what we advertise: 1500 - 20 - 20      */
#define TCP_BACKLOG        4             /* un-accepted connections per listener   */

#define TCP_RTO_INIT       100           /* 1 s initial retransmit timeout         */
#define TCP_RTO_MAX        3200          /* back off up to 32 s                    */
#define TCP_MAX_RETRIES    8
#define TCP_TIME_WAIT_TICKS 200          /* 2 s (RFC says 2*MSL = 2 min; shortened
                                            because we only have 12 sockets)       */
#define TCP_FIN_WAIT2_TICKS 6000         /* 60 s                                   */

#define TCP_NOWAIT         0u
#define TCP_FOREVER        0xFFFFFFFFu

/* Return codes (negative = error) */
#define TCP_OK             0
#define TCP_ERR           (-1)           /* bad handle / wrong state            */
#define TCP_AGAIN         (-2)           /* would block (TCP_NOWAIT)            */
#define TCP_TIMEOUT       (-3)
#define TCP_RESET         (-4)           /* connection reset by peer            */
#define TCP_REFUSED       (-5)           /* nobody listening on that port       */

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint16_t    rx_queued;       /* bytes waiting for tcp_recv()        */
    uint16_t    tx_queued;       /* bytes not yet acknowledged          */
} tcp_info_t;

/* Stack plumbing (called by net.c) */
void tcp_init(void);
void tcp_input(uint32_t src_ip, const uint8_t* segment, uint16_t len);
void tcp_timer(void);

/* Socket API */
int  tcp_socket(void);
int  tcp_listen(int s, uint16_t port);
int  tcp_accept(int s, uint32_t timeout);
int  tcp_connect(int s, uint32_t ip, uint16_t port, uint32_t timeout);
int  tcp_send(int s, const void* buf, int len, uint32_t timeout);
int  tcp_recv(int s, void* buf, int len, uint32_t timeout);   /* 0 = peer closed (EOF) */
int  tcp_close(int s);

/* Introspection */
int  tcp_send_space(int s);                       /* free bytes in the send buffer */
bool tcp_get_info(int s, tcp_info_t* out);        /* false if slot unused          */
const char* tcp_state_name(tcp_state_t st);

#endif
