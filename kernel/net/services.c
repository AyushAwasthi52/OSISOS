/*
 * services.c - the built-in network services, polled from the network task.
 *
 *   TCP/UDP 7   echo server
 *   TCP 80      HTTP file server (see httpd.c)
 *   TCP 9999    live network message receiver (prints to console)
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

extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);

#define SVC_ECHO_PORT   7
#define SVC_MSG_PORT    9999
#define SVC_MAX_CONN    3
#define SVC_IDLE_TICKS  (30 * NET_HZ)

typedef struct {
    int      sock;                        /* -1 = slot free */
    uint32_t last_active;
} svc_conn_t;

static int        echo_listener = -1;
static int        msg_listener  = -1;
static svc_conn_t echo_conns[SVC_MAX_CONN];
static svc_conn_t msg_conns[SVC_MAX_CONN];
static int        udp_echo_sock = -1;

void net_services_init(void) {
    for (int i = 0; i < SVC_MAX_CONN; i++) {
        echo_conns[i].sock = -1;
        msg_conns[i].sock = -1;
    }

    echo_listener = tcp_socket();
    if (echo_listener >= 0 && tcp_listen(echo_listener, SVC_ECHO_PORT) != TCP_OK) echo_listener = -1;

    msg_listener = tcp_socket();
    if (msg_listener >= 0 && tcp_listen(msg_listener, SVC_MSG_PORT) != TCP_OK) msg_listener = -1;

    udp_echo_sock = udp_socket();
    if (udp_echo_sock >= 0 && udp_bind(udp_echo_sock, SVC_ECHO_PORT) != 0) udp_echo_sock = -1;

    httpd_init();
}

static void accept_into(int listener, svc_conn_t* table) {
    if (listener < 0) return;
    for (;;) {
        int c = tcp_accept(listener, TCP_NOWAIT);
        if (c < 0) return;
        int slot = -1;
        for (int i = 0; i < SVC_MAX_CONN; i++) if (table[i].sock < 0) { slot = i; break; }
        if (slot < 0) { tcp_close(c); continue; }          /* server busy: drop */
        table[slot].sock = c;
        table[slot].last_active = net_now();
    }
}

static void conn_done(svc_conn_t* c) {
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
            conn_done(c);
            return;
        }
    }
    if ((uint32_t)(net_now() - c->last_active) > SVC_IDLE_TICKS) conn_done(c);
}

static void msg_serve(svc_conn_t* c) {
    char buf[128];
    int n = tcp_recv(c->sock, buf, sizeof(buf) - 1, TCP_NOWAIT);
    if (n > 0) {
        buf[n] = '\0';
        terminal_print_string("\n[Network Message]: ");
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\r') terminal_putchar(buf[i]);
        }
        if (n > 0 && buf[n - 1] != '\n') terminal_putchar('\n');
        terminal_print_string("admin@osisos> ");

        const char* ack = "Message delivered to OSISOS screen!\r\n";
        tcp_send(c->sock, ack, (int)net_strlen(ack), TCP_NOWAIT);
        c->last_active = net_now();
    } else if (n == 0 || (n < 0 && n != TCP_AGAIN)) {
        conn_done(c);
        return;
    }
    if ((uint32_t)(net_now() - c->last_active) > SVC_IDLE_TICKS) conn_done(c);
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
    accept_into(echo_listener, echo_conns);
    accept_into(msg_listener, msg_conns);
    for (int i = 0; i < SVC_MAX_CONN; i++) {
        if (echo_conns[i].sock >= 0) echo_serve(&echo_conns[i]);
        if (msg_conns[i].sock >= 0) msg_serve(&msg_conns[i]);
    }
    httpd_poll();
}
