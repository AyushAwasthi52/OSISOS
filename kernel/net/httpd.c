/*
 * httpd.c - static file HTTP server on top of the OFS path API.
 *
 * One non-blocking state machine per connection, driven from the network task:
 *
 *     READ request  -->  build response (headers + body source)  -->  PUMP  -->  close
 *
 * The body comes either from a small in-memory buffer (error pages, directory
 * listings, status page) or is streamed from an OFS file inode. PUMP only copies
 * as much as the TCP send buffer has room for, so a slow client never blocks the
 * server or other clients.
 *
 * Safety: request size is bounded (431), target length is bounded (414),
 * "." / ".." segments, backslashes, control characters and bad %-escapes are
 * rejected (400) before any filesystem lookup, only GET/HEAD are allowed (405),
 * idle connections are dropped after 30 s, and connections are only accepted
 * while a slot is free (the rest wait in the TCP backlog).
 */
#include "httpd.h"
#include "net.h"
#include "tcp.h"
#include "ofs.h"
#include "klog.h"

extern uint32_t seconds_passed;           /* drivers/timer.c */

#define HTTPD_PORT       80
#define HTTPD_MAX_CONN   6
#define HTTPD_REQ_MAX    768
#define HTTPD_HDR_MAX    384
#define HTTPD_BODY_MAX   2560
#define HTTPD_CHUNK      1024
#define HTTPD_IDLE_TICKS (30 * NET_HZ)

typedef struct {
    int      sock;                        /* -1 = slot free */
    uint32_t peer_ip;
    uint32_t last_active;
    bool     responding;
    bool     head_only;
    uint16_t status;
    char     method[8];
    char     path[40];

    uint16_t req_len;
    char     req[HTTPD_REQ_MAX + 1];

    char     hdr[HTTPD_HDR_MAX];
    uint16_t hdr_len, hdr_sent;

    char     body[HTTPD_BODY_MAX];        /* generated bodies */
    uint16_t body_len, body_sent;

    int      file_inode;                  /* -1 = no file body */
    uint32_t file_size, file_off;
    uint32_t body_bytes;                  /* body bytes actually sent (for the log) */
} conn_t;

static conn_t        conns[HTTPD_MAX_CONN];
static int           listener = -1;
static char          docroot[64] = "/www";
static httpd_stats_t stats;
static httpd_log_t   logbuf[HTTPD_LOG_SIZE];
static uint32_t      log_count;

/* shared scratch space (the poll loop is single-threaded; keeps the task stack small) */
static uint8_t chunk[HTTPD_CHUNK];
static char    full_path[160];
static char    dec_path[96];

/* ------------------------------------------------------------------ */
/* tiny string helpers                                                 */
/* ------------------------------------------------------------------ */

static int sapp(char* out, int pos, int max, const char* s) {
    while (*s && pos < max - 1) out[pos++] = *s++;
    out[pos] = '\0';
    return pos;
}

static int sdec(char* out, int pos, int max, uint32_t v) {
    char tmp[11]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && pos < max - 1) out[pos++] = tmp[--n];
    out[pos] = '\0';
    return pos;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ends_with_ci(const char* s, const char* suffix) {
    size_t a = net_strlen(s), b = net_strlen(suffix);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++) if (lc(s[a - b + i]) != suffix[i]) return false;
    return true;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char* mime_for(const char* name) {
    static const struct { const char* ext; const char* type; } map[] = {
        { ".html", "text/html" }, { ".htm", "text/html" }, { ".txt", "text/plain" },
        { ".css", "text/css" },   { ".js", "application/javascript" },
        { ".json", "application/json" }, { ".xml", "application/xml" },
        { ".png", "image/png" },  { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },  { ".ico", "image/x-icon" }, { ".svg", "image/svg+xml" },
        { ".pdf", "application/pdf" }, { ".md", "text/plain" },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (ends_with_ci(name, map[i].ext)) return map[i].type;
    return "application/octet-stream";
}

static const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 418: return "I'm a teapot";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

/* ------------------------------------------------------------------ */
/* request parsing                                                     */
/* ------------------------------------------------------------------ */

/* Percent-decode the target's path part into dec_path.
 * Returns 0 on success, or the HTTP status to answer with (400 / 414). */
static int decode_target(const char* t) {
    if (t[0] != '/') return 400;
    int n = 0;
    for (int i = 0; t[i] && t[i] != '?' && t[i] != '#'; i++) {
        int c = (unsigned char)t[i];
        if (c == '%') {
            int h = hexval(t[i + 1]), l = (h >= 0) ? hexval(t[i + 2]) : -1;
            if (h < 0 || l < 0) return 400;
            c = h * 16 + l;
            i += 2;
        }
        if (c < 32 || c == 127 || c == '\\') return 400;       /* control chars, NUL, backslash */
        if (n >= (int)sizeof(dec_path) - 1) return 414;
        dec_path[n++] = (char)c;
    }
    dec_path[n] = '\0';

    /* reject "." and ".." path segments (no directory traversal, ever) */
    for (int i = 0; dec_path[i]; ) {
        while (dec_path[i] == '/') i++;
        int s = i;
        while (dec_path[i] && dec_path[i] != '/') i++;
        int len = i - s;
        if ((len == 1 && dec_path[s] == '.') || (len == 2 && dec_path[s] == '.' && dec_path[s + 1] == '.'))
            return 400;
    }
    return 0;
}

static bool request_complete(const char* r, uint16_t len) {
    for (uint16_t i = 0; i + 1 < len; i++) {
        if (r[i] == '\n' && (r[i + 1] == '\n' || (r[i + 1] == '\r' && i + 2 < len && r[i + 2] == '\n')))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* response construction                                               */
/* ------------------------------------------------------------------ */

static void set_headers(conn_t* c, int status, const char* type, uint32_t length, const char* extra) {
    int n = 0;
    c->status = (uint16_t)status;
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, "HTTP/1.1 ");
    n = sdec(c->hdr, n, HTTPD_HDR_MAX, (uint32_t)status);
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, " ");
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, reason(status));
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, "\r\nServer: OSISOS\r\nContent-Type: ");
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, type);
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, "\r\nContent-Length: ");
    n = sdec(c->hdr, n, HTTPD_HDR_MAX, length);
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, "\r\nConnection: close\r\n");
    if (extra) n = sapp(c->hdr, n, HTTPD_HDR_MAX, extra);
    n = sapp(c->hdr, n, HTTPD_HDR_MAX, "\r\n");
    c->hdr_len = (uint16_t)n;
    c->hdr_sent = 0;
}

static void reply_text(conn_t* c, int status, const char* type, const char* body, const char* extra) {
    c->body_len = (uint16_t)sapp(c->body, 0, HTTPD_BODY_MAX, body);
    c->body_sent = 0;
    c->file_inode = -1;
    set_headers(c, status, type, c->body_len, extra);
}

static void reply_error(conn_t* c, int status, const char* extra) {
    char b[160];
    int n = 0;
    n = sapp(b, n, sizeof(b), "<html><body><h1>");
    n = sdec(b, n, sizeof(b), (uint32_t)status);
    n = sapp(b, n, sizeof(b), " ");
    n = sapp(b, n, sizeof(b), reason(status));
    n = sapp(b, n, sizeof(b), "</h1></body></html>\n");
    reply_text(c, status, "text/html", b, extra);
}

static void reply_welcome(conn_t* c) {
    char* b = c->body;
    int n = 0;
    n = sapp(b, n, HTTPD_BODY_MAX, "<html><body><h1>OSISOS</h1>"
                                   "<p>Hello from the OSISOS kernel TCP/IP stack.</p><p>Uptime: ");
    n = sdec(b, n, HTTPD_BODY_MAX, seconds_passed);
    n = sapp(b, n, HTTPD_BODY_MAX, " s. Requests served: ");
    n = sdec(b, n, HTTPD_BODY_MAX, stats.requests);
    n = sapp(b, n, HTTPD_BODY_MAX, ".</p><p>No <code>");
    n = sapp(b, n, HTTPD_BODY_MAX, docroot);
    n = sapp(b, n, HTTPD_BODY_MAX, "/index.html</code> yet: use <code>mkdir</code> and <code>fetch</code> "
                                   "to publish a site.</p></body></html>\n");
    c->body_len = (uint16_t)n;
    c->body_sent = 0;
    c->file_inode = -1;
    set_headers(c, 200, "text/html", c->body_len, NULL);
}

static void reply_status(conn_t* c) {
    char* b = c->body;
    int n = 0;
    n = sapp(b, n, HTTPD_BODY_MAX, "OSISOS httpd\nuptime_s: ");   n = sdec(b, n, HTTPD_BODY_MAX, seconds_passed);
    n = sapp(b, n, HTTPD_BODY_MAX, "\nrequests: ");              n = sdec(b, n, HTTPD_BODY_MAX, stats.requests);
    n = sapp(b, n, HTTPD_BODY_MAX, "\nbytes_sent: ");            n = sdec(b, n, HTTPD_BODY_MAX, stats.bytes_sent);
    n = sapp(b, n, HTTPD_BODY_MAX, "\nactive_connections: ");    n = sdec(b, n, HTTPD_BODY_MAX, stats.active);
    n = sapp(b, n, HTTPD_BODY_MAX, "\nstatus_2xx: ");            n = sdec(b, n, HTTPD_BODY_MAX, stats.s2xx);
    n = sapp(b, n, HTTPD_BODY_MAX, "\nstatus_4xx: ");            n = sdec(b, n, HTTPD_BODY_MAX, stats.s4xx);
    n = sapp(b, n, HTTPD_BODY_MAX, "\ntcp_segments_rx: ");       n = sdec(b, n, HTTPD_BODY_MAX, net_stats.tcp_rx);
    n = sapp(b, n, HTTPD_BODY_MAX, "\ntcp_segments_tx: ");       n = sdec(b, n, HTTPD_BODY_MAX, net_stats.tcp_tx);
    n = sapp(b, n, HTTPD_BODY_MAX, "\ntcp_retransmits: ");       n = sdec(b, n, HTTPD_BODY_MAX, net_stats.tcp_retx);
    n = sapp(b, n, HTTPD_BODY_MAX, "\ndocroot: ");               n = sapp(b, n, HTTPD_BODY_MAX, docroot);
    n = sapp(b, n, HTTPD_BODY_MAX, "\n");
    c->body_len = (uint16_t)n;
    c->body_sent = 0;
    c->file_inode = -1;
    set_headers(c, 200, "text/plain", c->body_len, NULL);
}

static void reply_file(conn_t* c, int inode, const char* name) {
    ofs_stat_t st;
    if (!ofs_stat(full_path[0] ? full_path : "/", &st) || st.type != OFS_TYPE_FILE) { reply_error(c, 500, NULL); return; }

    c->file_inode = inode;
    c->file_size = st.size;
    c->file_off = 0;
    c->body_len = c->body_sent = 0;
    set_headers(c, 200, mime_for(name), st.size, NULL);
}

static void reply_listing(conn_t* c, int dir_inode, const char* url_path) {
    char* b = c->body;
    int n = 0;
    n = sapp(b, n, HTTPD_BODY_MAX, "<html><body><h1>Index of ");
    n = sapp(b, n, HTTPD_BODY_MAX, url_path);
    n = sapp(b, n, HTTPD_BODY_MAX, "</h1><ul>\n");
    ofs_dirent_t e;
    int pos = 0, shown = 0;
    while ((pos = ofs_readdir(dir_inode, pos, &e)) >= 0) {
        if (n > HTTPD_BODY_MAX - 140) { n = sapp(b, n, HTTPD_BODY_MAX, "<li>...</li>\n"); break; }
        n = sapp(b, n, HTTPD_BODY_MAX, "<li><a href=\"");
        n = sapp(b, n, HTTPD_BODY_MAX, e.name);
        if (e.type == OFS_TYPE_DIR) n = sapp(b, n, HTTPD_BODY_MAX, "/");
        n = sapp(b, n, HTTPD_BODY_MAX, "\">");
        n = sapp(b, n, HTTPD_BODY_MAX, e.name);
        if (e.type == OFS_TYPE_DIR) n = sapp(b, n, HTTPD_BODY_MAX, "/");
        n = sapp(b, n, HTTPD_BODY_MAX, "</a>");
        if (e.type == OFS_TYPE_FILE) { n = sapp(b, n, HTTPD_BODY_MAX, " (");
                                       n = sdec(b, n, HTTPD_BODY_MAX, e.size);
                                       n = sapp(b, n, HTTPD_BODY_MAX, " bytes)"); }
        n = sapp(b, n, HTTPD_BODY_MAX, "</li>\n");
        shown++;
    }
    if (!shown) n = sapp(b, n, HTTPD_BODY_MAX, "<li>(empty)</li>\n");
    n = sapp(b, n, HTTPD_BODY_MAX, "</ul></body></html>\n");
    c->body_len = (uint16_t)n;
    c->body_sent = 0;
    c->file_inode = -1;
    set_headers(c, 200, "text/html", c->body_len, NULL);
}

/* Parse the request in c->req and prepare the response. */
static void build_response(conn_t* c) {
    /* --- request line: METHOD SP TARGET SP HTTP/x.y --- */
    char* p = c->req;
    int m = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') { if (m < (int)sizeof(c->method) - 1) c->method[m++] = *p; p++; }
    c->method[m] = '\0';
    if (*p != ' ') { reply_error(c, 400, NULL); return; }
    while (*p == ' ') p++;

    char* target = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
    if (*p != ' ' || p == target) { reply_error(c, 400, NULL); return; }       /* HTTP/0.9 / malformed */
    *p++ = '\0';
    while (*p == ' ') p++;
    if (!(p[0] == 'H' && p[1] == 'T' && p[2] == 'T' && p[3] == 'P' && p[4] == '/')) { reply_error(c, 400, NULL); return; }

    /* remember the target for the log (truncated) */
    int k = 0;
    while (target[k] && k < (int)sizeof(c->path) - 1) { c->path[k] = target[k]; k++; }
    c->path[k] = '\0';

    bool is_get  = net_strcmp(c->method, "GET") == 0;
    bool is_head = net_strcmp(c->method, "HEAD") == 0;
    if (!is_get && !is_head) { reply_error(c, 405, "Allow: GET, HEAD\r\n"); return; }
    c->head_only = is_head;

    int bad = decode_target(target);
    if (bad) { reply_error(c, bad, NULL); return; }

    if (net_strcmp(dec_path, "/_status") == 0) { reply_status(c); return; }
    if (net_strcmp(dec_path, "/teapot") == 0) {                    /* RFC 2324 (HTCPCP), April 1st 1998 */
        reply_text(c, 418, "text/plain",
            "418 I'm a teapot.\n\n"
            "            ;,'\n"
            "    _o_    ;:;'\n"
            ",-.'---`.__ ;\n"
            "((j`=====',-'\n"
            " `-\\     /\n"
            "    `-=-'\n\n"
            "OSISOS refuses to brew coffee: it is, permanently, a teapot.\n", NULL);
        return;
    }

    /* --- map URL path to an OFS path under the document root --- */
    int fl = 0;
    if (net_strcmp(docroot, "/") != 0) fl = sapp(full_path, 0, sizeof(full_path), docroot);
    else full_path[0] = '\0';
    if (fl + (int)net_strlen(dec_path) + 12 >= (int)sizeof(full_path)) { reply_error(c, 414, NULL); return; }
    sapp(full_path, fl, sizeof(full_path), dec_path);

    bool url_is_root = (net_strcmp(dec_path, "/") == 0);
    int idx = ofs_lookup(full_path[0] ? full_path : "/");

    if (idx < 0) {
        if (url_is_root) { reply_welcome(c); return; }       /* docroot missing */
        reply_error(c, 404, NULL);
        return;
    }

    ofs_stat_t st;
    if (!ofs_stat(full_path[0] ? full_path : "/", &st)) { reply_error(c, 500, NULL); return; }

    if (st.type == OFS_TYPE_DIR) {
        size_t ul = net_strlen(dec_path);
        if (dec_path[ul - 1] != '/') {                        /* /dir -> 301 /dir/ */
            char loc[HTTPD_HDR_MAX / 2];
            int n = sapp(loc, 0, sizeof(loc), "Location: ");
            n = sapp(loc, n, sizeof(loc), dec_path);
            n = sapp(loc, n, sizeof(loc), "/\r\n");
            reply_error(c, 301, loc);
            return;
        }
        /* index.html inside the directory? */
        size_t fl2 = net_strlen(full_path);
        if (fl2 == 0 || full_path[fl2 - 1] != '/') { full_path[fl2++] = '/'; full_path[fl2] = '\0'; }
        sapp(full_path, (int)fl2, sizeof(full_path), "index.html");
        int ii = ofs_lookup(full_path);
        if (ii >= 0 && ii != OFS_ROOT_DIR) { reply_file(c, ii, "index.html"); return; }
        reply_listing(c, idx, dec_path);
        return;
    }

    /* a regular file: name = last path component */
    const char* name = dec_path;
    for (const char* q = dec_path; *q; q++) if (*q == '/') name = q + 1;
    reply_file(c, idx, name);
}

/* ------------------------------------------------------------------ */
/* connection handling                                                 */
/* ------------------------------------------------------------------ */

static void log_request(conn_t* c) {
    httpd_log_t* e = &logbuf[log_count % HTTPD_LOG_SIZE];
    e->ip = c->peer_ip;
    e->when = net_now();
    e->status = c->status;
    e->bytes = c->body_bytes;
    for (int i = 0; i < 8; i++) e->method[i] = c->method[i];
    for (int i = 0; i < 40; i++) e->path[i] = c->path[i];
    log_count++;
    klog("httpd: %I %s %s -> %u (%u B)", c->peer_ip, c->method, c->path, (uint32_t)c->status, c->body_bytes);

    stats.requests++;
    if (c->status >= 200 && c->status < 300) stats.s2xx++;
    else if (c->status >= 300 && c->status < 400) stats.s3xx++;
    else if (c->status >= 400 && c->status < 500) stats.s4xx++;
    else stats.s5xx++;
}

static void conn_finish(conn_t* c, bool log) {
    if (log && c->responding) log_request(c);
    tcp_close(c->sock);
    c->sock = -1;
    if (stats.active) stats.active--;
}

/* Push as much of the response as the TCP send buffer accepts right now. */
static void pump(conn_t* c) {
    for (int guard = 0; guard < 16; guard++) {
        int space = tcp_send_space(c->sock);
        if (space < 0) { conn_finish(c, true); return; }
        if (space == 0) return;                                   /* wait for ACKs */

        int sent = 0;
        if (c->hdr_sent < c->hdr_len) {
            int n = c->hdr_len - c->hdr_sent;
            if (n > space) n = space;
            sent = tcp_send(c->sock, c->hdr + c->hdr_sent, n, TCP_NOWAIT);
            if (sent > 0) c->hdr_sent = (uint16_t)(c->hdr_sent + sent);
        } else if (c->head_only) {
            conn_finish(c, true); return;
        } else if (c->body_sent < c->body_len) {
            int n = c->body_len - c->body_sent;
            if (n > space) n = space;
            sent = tcp_send(c->sock, c->body + c->body_sent, n, TCP_NOWAIT);
            if (sent > 0) { c->body_sent = (uint16_t)(c->body_sent + sent); c->body_bytes += (uint32_t)sent; stats.bytes_sent += (uint32_t)sent; }
        } else if (c->file_inode >= 0 && c->file_off < c->file_size) {
            int n = HTTPD_CHUNK;
            if (n > space) n = space;
            if ((uint32_t)n > c->file_size - c->file_off) n = (int)(c->file_size - c->file_off);
            int got = ofs_inode_read(c->file_inode, c->file_off, chunk, (uint32_t)n);
            if (got <= 0) { conn_finish(c, true); return; }       /* disk error: abort the transfer */
            sent = tcp_send(c->sock, chunk, got, TCP_NOWAIT);
            if (sent > 0) {
                c->file_off += (uint32_t)sent; c->body_bytes += (uint32_t)sent; stats.bytes_sent += (uint32_t)sent;
                if (sent < got) return;   /* cannot happen (we bounded by space) but never lose bytes silently */
            }
        } else {
            conn_finish(c, true);                                 /* everything sent */
            return;
        }
        if (sent < 0 && sent != TCP_AGAIN) { conn_finish(c, true); return; }
        if (sent > 0) c->last_active = net_now();
    }
}

static void conn_poll(conn_t* c) {
    if (!c->responding) {
        int room = HTTPD_REQ_MAX - c->req_len;
        int n = (room > 0) ? tcp_recv(c->sock, c->req + c->req_len, room, TCP_NOWAIT) : 0;
        if (n > 0) {
            c->req_len = (uint16_t)(c->req_len + n);
            c->req[c->req_len] = '\0';
            c->last_active = net_now();
        } else if (n == 0 && room > 0) { conn_finish(c, false); return; }          /* client left */
        else if (n < 0 && n != TCP_AGAIN) { conn_finish(c, false); return; }

        bool complete = request_complete(c->req, c->req_len);
        if (complete || c->req_len >= HTTPD_REQ_MAX) {
            c->responding = true;
            c->file_inode = -1;
            if (!complete) { reply_error(c, 431, NULL); c->method[0] = '\0'; c->path[0] = '\0'; }
            else build_response(c);
        }
    }
    if (c->responding) pump(c);
    if (c->sock >= 0 && (uint32_t)(net_now() - c->last_active) > HTTPD_IDLE_TICKS) conn_finish(c, c->responding);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void httpd_init(void) {
    for (int i = 0; i < HTTPD_MAX_CONN; i++) conns[i].sock = -1;
    listener = tcp_socket();
    if (listener >= 0 && tcp_listen(listener, HTTPD_PORT) != TCP_OK) listener = -1;
}

void httpd_poll(void) {
    if (listener < 0) return;

    /* accept only while a slot is free: extra clients wait in the TCP backlog */
    for (;;) {
        int slot = -1;
        for (int i = 0; i < HTTPD_MAX_CONN; i++) if (conns[i].sock < 0) { slot = i; break; }
        if (slot < 0) break;
        int s = tcp_accept(listener, TCP_NOWAIT);
        if (s < 0) break;

        conn_t* c = &conns[slot];
        tcp_info_t info;
        c->sock = s;
        c->peer_ip = tcp_get_info(s, &info) ? info.remote_ip : 0;
        c->last_active = net_now();
        c->responding = false;
        c->head_only = false;
        c->status = 0;
        c->req_len = 0;
        c->req[0] = '\0';
        c->method[0] = '\0';
        c->path[0] = '\0';
        c->hdr_len = c->hdr_sent = 0;
        c->body_len = c->body_sent = 0;
        c->file_inode = -1;
        c->body_bytes = 0;
        stats.active++;
    }

    for (int i = 0; i < HTTPD_MAX_CONN; i++)
        if (conns[i].sock >= 0) conn_poll(&conns[i]);
}

void httpd_set_root(const char* dir) {
    size_t n = net_strlen(dir);
    if (n == 0 || n >= sizeof(docroot) || dir[0] != '/') return;
    while (n > 1 && dir[n - 1] == '/') n--;                      /* "/www/" -> "/www" */
    memcpy(docroot, dir, n);
    docroot[n] = '\0';
}

const char* httpd_root(void) { return docroot; }

void httpd_get_stats(httpd_stats_t* out) { *out = stats; }

bool httpd_get_log(int n_from_newest, httpd_log_t* out) {
    if (n_from_newest < 0 || (uint32_t)n_from_newest >= log_count || n_from_newest >= HTTPD_LOG_SIZE) return false;
    *out = logbuf[(log_count - 1 - (uint32_t)n_from_newest) % HTTPD_LOG_SIZE];
    return true;
}
