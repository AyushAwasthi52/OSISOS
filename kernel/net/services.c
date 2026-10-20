/*
 * services.c - the built-in network services, polled from the network task.
 *
 *   TCP/UDP 7   echo server
 *   TCP 80      HTTP file server (see httpd.c)
 *   UDP 68      DHCP client (see dhcp.c)
 *
 * The echo server doubles as a reference for writing a non-blocking server on the
 * tcp_* API: accept with TCP_NOWAIT, recv with TCP_NOWAIT, close on EOF or error.
 */
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "dhcp.h"
#include "httpd.h"

#define SVC_ECHO_PORT   7
#define SVC_MAX_CONN    3
#define SVC_IDLE_TICKS  (30 * NET_HZ)

typedef struct {
    int      sock;                        /* -1 = slot free */
    uint32_t last_active;
} svc_conn_t;

static int        echo_listener = -1;
static svc_conn_t echo_conns[SVC_MAX_CONN];
static int        udp_echo_sock = -1;

void net_services_init(void) {
    for (int i = 0; i < SVC_MAX_CONN; i++) echo_conns[i].sock = -1;

    echo_listener = tcp_socket();
    if (echo_listener >= 0 && tcp_listen(echo_listener, SVC_ECHO_PORT) != TCP_OK) echo_listener = -1;

    udp_echo_sock = udp_socket();
    if (udp_echo_sock >= 0 && udp_bind(udp_echo_sock, SVC_ECHO_PORT) != 0) udp_echo_sock = -1;

    httpd_init();
}

static void echo_accept(void) {
    if (echo_listener < 0) return;
    for (;;) {
        int slot = -1;
        for (int i = 0; i < SVC_MAX_CONN; i++) if (echo_conns[i].sock < 0) { slot = i; break; }
        if (slot < 0) return;                              /* full: leave the rest in the backlog */
        int c = tcp_accept(echo_listener, TCP_NOWAIT);
        if (c < 0) return;
        echo_conns[slot].sock = c;
        echo_conns[slot].last_active = net_now();
    }
}

static void echo_done(svc_conn_t* c) {
    tcp_close(c->sock);
    c->sock = -1;
}

static void echo_serve(svc_conn_t* c) {
    char buf[128];
    int space = tcp_send_space(c->sock);
    if (space > 0) {
        int want = space < (int)sizeof(buf) ? space : (int)sizeof(buf);
        int n = tcp_recv(c->sock, buf, want, TCP_NOWAIT);
        if (n > 0) {
            tcp_send(c->sock, buf, n, TCP_NOWAIT);
            c->last_active = net_now();
        } else if (n == 0 || (n < 0 && n != TCP_AGAIN)) {   /* EOF or error */
            echo_done(c);
            return;
        }
    }
    if ((uint32_t)(net_now() - c->last_active) > SVC_IDLE_TICKS) echo_done(c);
}

static void udp_echo_poll(void) {
    if (udp_echo_sock < 0) return;
    for (int i = 0; i < 8; i++) {                           /* bounded work per poll */
        uint8_t buf[UDP_MAX_PAYLOAD];
        uint32_t ip; uint16_t port;
        int n = udp_recvfrom(udp_echo_sock, buf, sizeof(buf), &ip, &port, 0);
        if (n < 0) return;
        udp_sendto(udp_echo_sock, ip, port, buf, n);
    }
}

void net_services_poll(void) {
    dhcp_poll();                                            /* lease acquisition / renewal */
    udp_echo_poll();
    echo_accept();
    for (int i = 0; i < SVC_MAX_CONN; i++)
        if (echo_conns[i].sock >= 0) echo_serve(&echo_conns[i]);
    httpd_poll();
}
