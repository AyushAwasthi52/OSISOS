/*
 * services.c - tiny built-in TCP servers, polled from the network task.
 *
 *   port 7   echo server  (every byte received is sent back)
 *   port 80  HTTP server  (answers any request with a small status page)
 *
 * They also serve as a reference for how to write a non-blocking server on
 * top of the tcp_* API: accept with TCP_NOWAIT, recv with TCP_NOWAIT, close
 * on EOF or error.
 */
#include "net.h"
#include "tcp.h"

extern uint32_t seconds_passed;           /* drivers/timer.c */

#define SVC_ECHO_PORT   7
#define SVC_HTTP_PORT   80
#define SVC_MAX_CONN    3
#define SVC_IDLE_TICKS  (30 * NET_HZ)

typedef struct {
    int      sock;                        /* -1 = slot free */
    uint32_t last_active;
    uint16_t used;                        /* bytes in req[] (HTTP only) */
    char     req[192];
} svc_conn_t;

static int        echo_listener = -1;
static int        http_listener = -1;
static svc_conn_t echo_conns[SVC_MAX_CONN];
static svc_conn_t http_conns[SVC_MAX_CONN];
static uint32_t   http_served;

void net_services_init(void) {
    for (int i = 0; i < SVC_MAX_CONN; i++) { echo_conns[i].sock = -1; http_conns[i].sock = -1; }

    echo_listener = tcp_socket();
    if (echo_listener >= 0 && tcp_listen(echo_listener, SVC_ECHO_PORT) != TCP_OK) echo_listener = -1;
    http_listener = tcp_socket();
    if (http_listener >= 0 && tcp_listen(http_listener, SVC_HTTP_PORT) != TCP_OK) http_listener = -1;
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
        table[slot].used = 0;
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

/* --- HTTP ----------------------------------------------------------- */

static int append_str(char* out, int pos, int max, const char* s) {
    while (*s && pos < max - 1) out[pos++] = *s++;
    out[pos] = '\0';
    return pos;
}

static int append_dec(char* out, int pos, int max, uint32_t v) {
    char tmp[11];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && pos < max - 1) out[pos++] = tmp[--n];
    out[pos] = '\0';
    return pos;
}

static bool request_complete(const char* r, uint16_t len) {
    for (uint16_t i = 0; i + 1 < len; i++) {
        if (r[i] == '\n' && (r[i + 1] == '\n' || (r[i + 1] == '\r' && i + 2 < len && r[i + 2] == '\n')))
            return true;
    }
    return false;
}

static void http_respond(svc_conn_t* c) {
    char body[320];
    int b = 0;
    b = append_str(body, b, sizeof(body), "<html><body><h1>OSISOS</h1>"
                                          "<p>Hello from the OSISOS kernel TCP/IP stack.</p><p>Uptime: ");
    b = append_dec(body, b, sizeof(body), seconds_passed);
    b = append_str(body, b, sizeof(body), " s. Requests served: ");
    b = append_dec(body, b, sizeof(body), ++http_served);
    b = append_str(body, b, sizeof(body), ". TCP segments in/out: ");
    b = append_dec(body, b, sizeof(body), net_stats.tcp_rx);
    b = append_str(body, b, sizeof(body), "/");
    b = append_dec(body, b, sizeof(body), net_stats.tcp_tx);
    b = append_str(body, b, sizeof(body), ".</p></body></html>\n");

    char resp[640];
    int r = 0;
    r = append_str(resp, r, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: ");
    r = append_dec(resp, r, sizeof(resp), (uint32_t)b);
    r = append_str(resp, r, sizeof(resp), "\r\nConnection: close\r\n\r\n");
    r = append_str(resp, r, sizeof(resp), body);

    tcp_send(c->sock, resp, r, TCP_NOWAIT);
}

static void http_serve(svc_conn_t* c) {
    int room = (int)sizeof(c->req) - 1 - c->used;
    if (room > 0) {
        int n = tcp_recv(c->sock, c->req + c->used, room, TCP_NOWAIT);
        if (n > 0) { c->used = (uint16_t)(c->used + n); c->last_active = net_now(); }
        else if (n == 0 || (n < 0 && n != TCP_AGAIN)) { conn_done(c); return; }
    }
    if (c->used > 0 && (request_complete(c->req, c->used) || room <= 0)) {
        http_respond(c);
        conn_done(c);                          /* HTTP/1.0: close after the response */
        return;
    }
    if ((uint32_t)(net_now() - c->last_active) > SVC_IDLE_TICKS) conn_done(c);
}

void net_services_poll(void) {
    accept_into(echo_listener, echo_conns);
    accept_into(http_listener, http_conns);
    for (int i = 0; i < SVC_MAX_CONN; i++) {
        if (echo_conns[i].sock >= 0) echo_serve(&echo_conns[i]);
        if (http_conns[i].sock >= 0) http_serve(&http_conns[i]);
    }
}
