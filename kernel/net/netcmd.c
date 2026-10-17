/*
 * netcmd.c - shell commands for the network stack.
 *
 *   ifconfig                       show interface, DHCP state + counters
 *   ifconfig <ip> <mask> <gw>      configure (e.g. ifconfig 10.0.2.15 255.255.255.0 10.0.2.2)
 *   arp                            show ARP cache
 *   ping <host>                    4 ICMP echo requests   (<host> = IP address or DNS name)
 *   nslookup <name>                resolve a name through DNS
 *   dns [<ip>]                     show / set the DNS server
 *   dhcp [renew]                   show DHCP state / renew the lease now
 *   netstat                        list TCP sockets
 *   nc <host> <port> [text...]     TCP client: connect, send "text\n", print the reply
 *   http <host> [port] [path]      minimal HTTP/1.0 GET
 *   fetch <host> <port> <url-path> <ofs-path>   download a file over HTTP into OFS
 *   httpd [root <dir>]             HTTP server status, access log, document root
 *
 * (The keyboard driver has no Shift key yet, so everything is lowercase.)
 */
#include "net.h"
#include "tcp.h"
#include "dns.h"
#include "dhcp.h"
#include "httpd.h"
#include "ofs.h"
#include "kheap.h"

extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);
extern void terminal_print_dec(uint32_t val);

/* ---------------- printing helpers (return characters printed) ------- */

static int p_str(const char* s) { terminal_print_string(s); return (int)net_strlen(s); }

static int p_dec(uint32_t v) {
    char tmp[11]; int n = 0, len;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    len = n;
    while (n) terminal_putchar(tmp[--n]);
    return len;
}

static int p_ip(uint32_t ip) {
    int n = p_dec(ip >> 24); terminal_putchar('.'); n++;
    n += p_dec((ip >> 16) & 255); terminal_putchar('.'); n++;
    n += p_dec((ip >> 8) & 255);  terminal_putchar('.'); n++;
    return n + p_dec(ip & 255);
}

static void p_mac(const uint8_t* m) {
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        terminal_putchar(hex[m[i] >> 4]);
        terminal_putchar(hex[m[i] & 15]);
        if (i < 5) terminal_putchar(':');
    }
}

static void pad(int printed, int width) { while (printed++ < width) terminal_putchar(' '); }

/* ---------------- parsing helpers ------------------------------------ */

static int split(char* s, char** argv, int max) {
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ') *s++ = '\0';
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ') s++;
    }
    return argc;
}

static bool parse_uint(const char* s, uint32_t* out) {
    if (*s < '0' || *s > '9') return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    if (*s) return false;
    *out = v;
    return true;
}

/* Accept a dotted IP or resolve a hostname via DNS (prints progress). */
static bool resolve_host(const char* s, uint32_t* ip) {
    if (net_parse_ip(s, ip)) return true;
    p_str("resolving "); p_str(s); p_str(" ... ");
    int r = dns_resolve(s, ip, 6 * NET_HZ);
    if (r == DNS_OK) { p_ip(*ip); p_str("\n"); return true; }
    switch (r) {
        case DNS_NXDOMAIN: p_str("no such host\n"); break;
        case DNS_TIMEOUT:  p_str("DNS timeout (server: "); p_ip(netif.dns); p_str(")\n"); break;
        case DNS_ERR:      p_str("failed (no DNS server? bad name? try: dns <ip>)\n"); break;
        default:           p_str("DNS failure\n"); break;
    }
    return false;
}

static const char* tcp_err_str(int e) {
    switch (e) {
        case TCP_TIMEOUT: return "timed out";
        case TCP_RESET:   return "connection reset";
        case TCP_REFUSED: return "connection refused";
        default:          return "error";
    }
}

/* ---------------- commands ------------------------------------------- */

static void cmd_ifconfig(int argc, char** argv) {
    if (argc == 4) {
        uint32_t ip, mask, gw;
        if (net_parse_ip(argv[1], &ip) && net_parse_ip(argv[2], &mask) && net_parse_ip(argv[3], &gw)) {
            dhcp_set_static();                       /* manual config wins over DHCP */
            net_set_ip(ip, mask, gw);
        } else {
            p_str("usage: ifconfig <ip> <netmask> <gateway>\n");
            return;
        }
    } else if (argc != 1) {
        p_str("usage: ifconfig [<ip> <netmask> <gateway>]\n");
        return;
    }
    p_str("eth0  MAC "); p_mac(netif.mac); p_str("\n");
    p_str("      inet "); p_ip(netif.ip);
    p_str("  mask "); p_ip(netif.mask);
    p_str("  gw ");   p_ip(netif.gateway); p_str("\n");
    p_str("      dns "); p_ip(netif.dns);
    p_str("  dhcp "); p_str(dhcp_state_name());
    if (dhcp_lease_left_secs()) { p_str(" (lease "); p_dec(dhcp_lease_left_secs()); p_str(" s left, server "); p_ip(dhcp_server()); p_str(")"); }
    p_str("\n");
    p_str("      frames rx "); p_dec(net_stats.rx_frames);
    p_str(" tx "); p_dec(net_stats.tx_frames);
    p_str("  tcp rx "); p_dec(net_stats.tcp_rx);
    p_str(" tx "); p_dec(net_stats.tcp_tx);
    p_str(" retx "); p_dec(net_stats.tcp_retx);
    p_str("\n      syn-drop "); p_dec(net_stats.tcp_syn_drop);
    p_str("  rto-by-state:");
    for (int i = 0; i <= TCP_TIME_WAIT; i++) {
        if (net_stats.tcp_retx_state[i]) { p_str(" "); p_str(tcp_state_name((tcp_state_t)i)); p_str("="); p_dec(net_stats.tcp_retx_state[i]); }
    }
    p_str("\n");
}

static void cmd_arp(void) {
    bool any = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        uint32_t ip; uint8_t mac[6];
        if (arp_get_entry(i, &ip, mac)) {
            pad(p_ip(ip), 17); p_mac(mac); p_str("\n");
            any = true;
        }
    }
    if (!any) p_str("ARP cache is empty\n");
}

static void cmd_ping(const char* arg) {
    uint32_t ip;
    if (!resolve_host(arg, &ip)) return;
    p_str("PING "); p_ip(ip); p_str("\n");
    for (uint16_t seq = 1; seq <= 4; seq++) {
        uint32_t t0 = net_now();
        int rtt = icmp_ping(ip, seq, 2 * NET_HZ);
        p_str("  seq="); p_dec(seq);
        if (rtt < 0) p_str(" timeout\n");
        else if (rtt == 0) p_str(" reply, time<10 ms\n");
        else { p_str(" reply, time="); p_dec((uint32_t)rtt * 10); p_str(" ms\n"); }
        while ((uint32_t)(net_now() - t0) < NET_HZ) net_wait();      /* 1 per second */
    }
}

static void cmd_netstat(void) {
    p_str("Proto Local              Remote             State       RxQ  TxQ\n");
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        tcp_info_t in;
        if (!tcp_get_info(i, &in)) continue;
        int n = p_str("tcp   ");
        if (in.state == TCP_LISTEN || in.state == TCP_CLOSED) n += p_str("*:"); else n += p_ip(netif.ip), n += p_str(":");
        n += p_dec(in.local_port);
        pad(n, 24);
        if (in.state == TCP_LISTEN || in.state == TCP_CLOSED) n = p_str("*:*");
        else { n = p_ip(in.remote_ip); n += p_str(":"); n += p_dec(in.remote_port); }
        pad(n, 19);
        pad(p_str(tcp_state_name(in.state)), 12);
        pad(p_dec(in.rx_queued), 5);
        p_dec(in.tx_queued);
        p_str("\n");
    }
}

/* Print everything the peer sends until EOF / error / idle timeout. */
static void dump_reply(int s, uint32_t idle_ticks) {
    char buf[128];
    int total = 0;
    for (;;) {
        int n = tcp_recv(s, buf, sizeof(buf), idle_ticks);
        if (n == 0) break;
        if (n < 0) { if (n != TCP_TIMEOUT) { p_str("\n["); p_str(tcp_err_str(n)); p_str("]"); } break; }
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\r') continue;
            terminal_putchar(buf[i] == '\n' || (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
        }
        total += n;
        if (total > 3000) { p_str("\n[output truncated]"); break; }
    }
    terminal_putchar('\n');
}

static void cmd_client(const char* ip_s, const char* port_s, const char* request, int req_len, bool http) {
    uint32_t ip, port;
    if (!parse_uint(port_s, &port) || port == 0 || port > 65535) { p_str("bad port\n"); return; }
    if (!resolve_host(ip_s, &ip)) return;
    int s = tcp_socket();
    if (s < 0) { p_str("no free sockets\n"); return; }

    int r = tcp_connect(s, ip, (uint16_t)port, 5 * NET_HZ);
    if (r != TCP_OK) {
        p_str("connect: "); p_str(tcp_err_str(r)); p_str("\n");
        tcp_close(s);
        return;
    }
    p_str("connected to "); p_ip(ip); p_str(":"); p_dec(port); p_str("\n");

    if (req_len > 0) tcp_send(s, request, req_len, 5 * NET_HZ);
    dump_reply(s, http ? 3 * NET_HZ : 2 * NET_HZ);
    tcp_close(s);
}

/* ---------------- fetch: HTTP download into the filesystem ----------- */

#define FETCH_MAX (256 * 1024)

static int ci_prefix(const char* s, const char* prefix) {     /* case-insensitive "starts with" */
    while (*prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
        s++; prefix++;
    }
    return 1;
}

static void cmd_fetch(const char* host, const char* port_s, const char* url, const char* dest) {
    uint32_t ip, port;
    if (!ofs_available()) { p_str("fetch: no disk / filesystem\n"); return; }
    if (!parse_uint(port_s, &port) || port == 0 || port > 65535) { p_str("bad port\n"); return; }
    if (url[0] != '/' || dest[0] != '/') { p_str("fetch: url-path and ofs-path must start with /\n"); return; }
    if (!resolve_host(host, &ip)) return;

    uint8_t* buf = (uint8_t*)kmalloc(FETCH_MAX);
    if (!buf) { p_str("fetch: out of memory\n"); return; }

    int s = tcp_socket();
    if (s < 0) { p_str("no free sockets\n"); kfree(buf); return; }
    int r = tcp_connect(s, ip, (uint16_t)port, 5 * NET_HZ);
    if (r != TCP_OK) { p_str("connect: "); p_str(tcp_err_str(r)); p_str("\n"); tcp_close(s); kfree(buf); return; }

    char req[192];
    int n = 0;
    const char* parts[3] = { "GET ", url, " HTTP/1.0\r\nHost: osisos\r\nConnection: close\r\n\r\n" };
    for (int i = 0; i < 3; i++)
        for (const char* p = parts[i]; *p && n < (int)sizeof(req) - 1; p++) req[n++] = *p;
    tcp_send(s, req, n, 5 * NET_HZ);

    uint32_t total = 0, t_start = net_now();
    bool too_big = false;
    for (;;) {
        int got = tcp_recv(s, buf + total, (int)(FETCH_MAX - total), 5 * NET_HZ);
        if (got == 0) break;                                   /* server closed: transfer complete */
        if (got < 0) { p_str("fetch: "); p_str(tcp_err_str(got)); p_str("\n"); tcp_close(s); kfree(buf); return; }
        total += (uint32_t)got;
        if (total >= FETCH_MAX) { too_big = true; break; }
    }
    tcp_close(s);
    uint32_t t_dl = net_now() - t_start;
    if (too_big) { p_str("fetch: file larger than 256 KB\n"); kfree(buf); return; }

    /* split headers / body */
    uint32_t hdr_end = 0;
    for (uint32_t i = 0; i + 3 < total; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') { hdr_end = i + 4; break; }
    }
    if (!hdr_end || total < 12 || !ci_prefix((const char*)buf, "http/1.") || buf[9] != '2' || buf[10] != '0' || buf[11] != '0') {
        p_str("fetch: server did not answer 200 OK\n");
        kfree(buf);
        return;
    }

    uint32_t body_len = total - hdr_end;
    for (uint32_t i = 0; i < hdr_end; i++) {                    /* honour Content-Length if present */
        if ((i == 0 || buf[i-1] == '\n') && ci_prefix((const char*)buf + i, "content-length:")) {
            uint32_t v = 0, j = i + 15;
            while (buf[j] == ' ') j++;
            while (buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (uint32_t)(buf[j++] - '0');
            if (v > body_len) { p_str("fetch: truncated download\n"); kfree(buf); return; }
            body_len = v;
            break;
        }
    }

    uint32_t t_w = net_now();
    if (ofs_put(dest, buf + hdr_end, body_len)) {
        p_str("saved "); p_dec(body_len); p_str(" bytes to "); p_str(dest);
        p_str("  (download "); p_dec(t_dl * 10); p_str(" ms, disk write "); p_dec((net_now() - t_w) * 10); p_str(" ms)\n");
    } else {
        p_str("fetch: cannot write "); p_str(dest); p_str(" (missing directory? disk or inode table full?)\n");
    }
    kfree(buf);
}

static void cmd_httpd(int argc, char** argv) {
    if (argc == 3 && net_strcmp(argv[1], "root") == 0) {
        if (argv[2][0] != '/') { p_str("httpd: root must be an absolute path like /www\n"); return; }
        httpd_set_root(argv[2]);
    } else if (argc != 1) { p_str("usage: httpd [root <dir>]\n"); return; }

    httpd_stats_t st;
    httpd_get_stats(&st);
    p_str("httpd on port 80, document root "); p_str(httpd_root());
    p_str(ofs_lookup(httpd_root()) < 0 ? "  (does not exist: serving the built-in page)\n" : "\n");
    p_str("  requests "); p_dec(st.requests); p_str("  active "); p_dec(st.active);
    p_str("  2xx "); p_dec(st.s2xx); p_str("  3xx "); p_dec(st.s3xx);
    p_str("  4xx "); p_dec(st.s4xx); p_str("  5xx "); p_dec(st.s5xx);
    p_str("  sent "); p_dec(st.bytes_sent); p_str(" B\n");
    for (int i = 0; i < 8; i++) {
        httpd_log_t e;
        if (!httpd_get_log(i, &e)) break;
        p_str("  "); p_ip(e.ip); p_str(" "); p_str(e.method); p_str(" "); p_str(e.path);
        p_str(" -> "); p_dec(e.status); p_str(" ("); p_dec(e.bytes); p_str(" B)\n");
    }
}

/* ---------------- dispatcher ----------------------------------------- */

bool net_shell_command(const char* line) {
    char buf[128];
    char* argv[8];
    size_t n = net_strlen(line);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';

    int argc = split(buf, argv, 8);
    if (argc == 0) return false;

    if (net_strcmp(argv[0], "ifconfig") == 0) { cmd_ifconfig(argc, argv); return true; }
    if (net_strcmp(argv[0], "arp") == 0)      { cmd_arp();                return true; }
    if (net_strcmp(argv[0], "httpd") == 0) { cmd_httpd(argc, argv); return true; }
    if (net_strcmp(argv[0], "fetch") == 0) {
        if (argc != 5) p_str("usage: fetch <host> <port> <url-path> <ofs-path>   e.g. fetch 10.0.2.2 8000 /a.html /www/a.html\n");
        else cmd_fetch(argv[1], argv[2], argv[3], argv[4]);
        return true;
    }
    if (net_strcmp(argv[0], "nslookup") == 0) {
        uint32_t ip, lit;
        if (argc != 2) p_str("usage: nslookup <name>\n");
        else if (resolve_host(argv[1], &ip) && net_parse_ip(argv[1], &lit)) { p_ip(ip); p_str("\n"); }
        return true;                               /* (names already printed their answer) */
    }
    if (net_strcmp(argv[0], "dns") == 0) {
        uint32_t ip;
        if (argc == 2 && net_parse_ip(argv[1], &ip)) netif.dns = ip;
        else if (argc != 1) { p_str("usage: dns [<server-ip>]\n"); return true; }
        p_str("DNS server: "); p_ip(netif.dns); p_str("\n");
        return true;
    }
    if (net_strcmp(argv[0], "dhcp") == 0) {
        if (argc == 2 && net_strcmp(argv[1], "renew") == 0) { dhcp_renew_now(); p_str("renewing...\n"); return true; }
        p_str("DHCP: "); p_str(dhcp_state_name());
        if (dhcp_server()) { p_str("  server "); p_ip(dhcp_server()); }
        if (dhcp_lease_left_secs()) { p_str("  lease left "); p_dec(dhcp_lease_left_secs()); p_str(" s"); }
        p_str("\n");
        return true;
    }
    if (net_strcmp(argv[0], "netdrop") == 0) {
        uint32_t pct;
        if (argc == 2 && parse_uint(argv[1], &pct) && pct <= 90) {
            net_loss_pct = (uint8_t)pct;
            p_str("simulated packet loss: "); p_dec(pct); p_str("%\n");
        } else p_str("usage: netdrop <0-90>   (random loss of IP frames, rx+tx)\n");
        return true;
    }
    if (net_strcmp(argv[0], "netstat") == 0)  { cmd_netstat();            return true; }
    if (net_strcmp(argv[0], "ping") == 0 && argc >= 2) { cmd_ping(argv[1]); return true; }

    if (net_strcmp(argv[0], "nc") == 0) {
        if (argc < 3) { p_str("usage: nc <ip> <port> [text...]\n"); return true; }
        char req[100];
        int len = 0;
        for (int i = 3; i < argc; i++) {                         /* re-join the words */
            for (const char* p = argv[i]; *p && len < (int)sizeof(req) - 2; p++) req[len++] = *p;
            if (i + 1 < argc && len < (int)sizeof(req) - 2) req[len++] = ' ';
        }
        if (argc > 3) req[len++] = '\n';
        cmd_client(argv[1], argv[2], req, len, false);
        return true;
    }

    if (net_strcmp(argv[0], "http") == 0) {
        if (argc < 2) { p_str("usage: http <ip> [port] [path]\n"); return true; }
        const char* port = argc > 2 ? argv[2] : "80";
        const char* path = argc > 3 ? argv[3] : "/";
        char req[128];
        int len = 0;
        const char* parts[3] = { "GET ", path, " HTTP/1.0\r\nHost: osisos\r\nConnection: close\r\n\r\n" };
        for (int i = 0; i < 3; i++)
            for (const char* p = parts[i]; *p && len < (int)sizeof(req) - 1; p++) req[len++] = *p;
        cmd_client(argv[1], port, req, len, true);
        return true;
    }

    return false;      /* not ours: the shell keeps looking (this also keeps bare `ping`) */
}
