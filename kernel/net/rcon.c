/*
 * rcon.c - password-protected remote shell (see rcon.h).
 *
 * Session state machine (network task):
 *
 *     NONE -> USER -> PASS -> SHELL <-> BUSY
 *                                 (BUSY = the shell task is running a command; no input is read
 *                                  meanwhile, so TCP flow control holds back anything the client
 *                                  sent ahead, e.g. `printf 'df\nifconfig\n' | nc host 2323`)
 */
#include "rcon.h"
#include "net.h"
#include "tcp.h"
#include "auth.h"
#include "klog.h"
#include "ofs.h"
#include "task.h"
#include "noise.h"
#include "x25519.h"
#include "rng.h"
#include "sha256.h"

#define FLAG_PLAIN  "/etc/rcon"
#define FLAG_SECURE "/etc/rcon.secure"
#define KEY_FILE    "/etc/rcon.key"
#define FRAME_MAX   700
#define HANDSHAKE_TICKS (10 * NET_HZ)
#define MAX_LOGIN_TRIES 3

/* telnet protocol bytes */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define OPT_ECHO 1

extern void (*terminal_sink)(char c);                  /* core/kernel.c: when set, terminal output goes here */
extern volatile task_t* current_task;

typedef enum { RS_NONE = 0, RS_HANDSHAKE, RS_USER, RS_PASS, RS_SHELL, RS_BUSY } rstate_t;

static bool    enabled[2];
static int     listener[2] = { -1, -1 };

static uint8_t srv_priv[32], srv_pub[32];       /* the server identity (static X25519 key) */
static bool    have_key, key_persistent;

static struct {
    int      sock;
    rstate_t state;
    char     user[24];
    char     line[RCON_LINE_MAX + 1];
    int      len;
    uint8_t  iac;                  /* 0 normal, 1 after IAC, 2 after IAC verb, 3 inside SB, 4 SB saw IAC */
    uint8_t  verb;
    bool     telnet;               /* the client speaks telnet (we saw an IAC) */
    uint8_t  tries;
    uint32_t last_active;
    uint32_t peer;
    char     pending[RCON_LINE_MAX + 1];
    bool     has_pending;
    bool     dead;                 /* client vanished while a command was running */

    /* secure sessions */
    bool     secure;
    noise_hs hs;
    uint8_t  frame[FRAME_MAX];                /* a Noise message being assembled: 2-byte length, then body */
    int      flen, fneed;                     /* fneed < 0: still reading the length prefix */
    uint8_t  pbuf[FRAME_MAX];                 /* decrypted bytes not yet consumed by the line editor */
    int      pp, pl;
} s = { .sock = -1 };

static uint8_t in_buf[128];
static int     in_len, in_pos;

static char    obuf[256];
static int     olen;
static uint32_t sink_owner;

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

/* one encrypted transport message: 2-byte big-endian length, ciphertext, 16-byte tag */
static void send_secure(const uint8_t* data, int n, uint32_t timeout) {
    static uint8_t tx[2 + 256 + 16];
    while (n > 0 && s.sock >= 0) {
        int c = n > 256 ? 256 : n;
        int total = c + 16;
        tx[0] = (uint8_t)(total >> 8); tx[1] = (uint8_t)total;
        noise_encrypt(&s.hs.send, data, (size_t)c, tx + 2);
        if (tcp_send(s.sock, tx, 2 + total, timeout) != 2 + total) { s.dead = true; return; }
        data += c; n -= c;
    }
}

static void out_flush(uint32_t timeout) {
    if (olen > 0 && s.sock >= 0) {
        if (s.secure) send_secure((const uint8_t*)obuf, olen, timeout);
        else if (tcp_send(s.sock, obuf, olen, timeout) < 0) s.dead = true;
    }
    olen = 0;
}

static void out_char(char c, uint32_t timeout) {
    if (c == '\n') { if (olen + 2 >= (int)sizeof(obuf)) out_flush(timeout); obuf[olen++] = '\r'; obuf[olen++] = '\n'; return; }
    if (c == '\b' || (c < 32 && c != '\t') || c == 127) return;
    if (olen + 1 >= (int)sizeof(obuf)) out_flush(timeout);
    obuf[olen++] = c;
}

static void out_str(const char* str, uint32_t timeout) { while (*str) out_char(*str++, timeout); }

static void send_str(const char* str) { out_str(str, TCP_NOWAIT); out_flush(TCP_NOWAIT); }

static void send_bytes(const uint8_t* b, int n) { if (!s.secure && s.sock >= 0 && tcp_send(s.sock, b, n, TCP_NOWAIT) < 0) s.dead = true; }   /* telnet options: plaintext only */

static void prompt(void) { out_str(s.user, TCP_NOWAIT); out_str("@osisos> ", TCP_NOWAIT); out_flush(TCP_NOWAIT); }

/* terminal sink: runs in the shell task while a remote command executes */
static void rcon_sink(char c) {
    if (current_task && current_task->pid != sink_owner) return;   /* other tasks' output is not ours */
    out_char(c, 5 * NET_HZ);
}

/* ------------------------------------------------------------------ */
/* session lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void session_end(const char* why) {
    if (s.sock >= 0) { klog("rcon: session closed (%s)", why); tcp_close(s.sock); }
    s.sock = -1;
    s.state = RS_NONE;
    s.has_pending = false;
    s.dead = false;
    in_len = in_pos = 0;
    olen = 0;
}

static void session_start(int sock, bool secure) {
    s.sock = sock;
    s.secure = secure;
    s.state = secure ? RS_HANDSHAKE : RS_USER;
    s.user[0] = '\0';
    s.len = 0;
    s.iac = 0;
    s.telnet = false;
    s.tries = 0;
    s.dead = false;
    s.has_pending = false;
    s.last_active = net_now();
    s.flen = 0; s.fneed = -1; s.pp = s.pl = 0;
    in_len = in_pos = 0;
    olen = 0;
    tcp_info_t info;
    s.peer = tcp_get_info(sock, &info) ? info.remote_ip : 0;
    klog("rcon: %s connection from %I", secure ? "secure" : "plain", s.peer);
    if (secure) noise_resp_init(&s.hs, srv_priv, srv_pub, (const uint8_t*)RCON_PROLOGUE, sizeof(RCON_PROLOGUE) - 1);
    else send_str("OSISOS remote console (UNENCRYPTED - trusted networks only)\nlogin: ");
}

static bool word_is(const char* line, const char* w) {
    while (*w) { if (*line++ != *w++) return false; }
    return *line == '\0' || *line == ' ';
}

static bool denied(const char* line) {
    static const char* const bad[] = { "snake", "matrix", "sl", "passwd", "edit", "clear", "crash", "login" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) if (word_is(line, bad[i])) return true;
    return false;
}

static void safe_copy(char* dst, const char* src, int max) {
    int n = 0;
    for (; src[n] && n < max - 1; n++) dst[n] = (src[n] >= 32 && src[n] < 127) ? src[n] : '?';
    dst[n] = '\0';
}

static void handle_line(void) {
    s.line[s.len] = '\0';
    s.len = 0;
    s.last_active = net_now();

    if (s.state == RS_USER) {
        safe_copy(s.user, s.line, sizeof(s.user));
        s.state = RS_PASS;
        if (s.telnet) { static const uint8_t will_echo[3] = { IAC, WILL, OPT_ECHO }; send_bytes(will_echo, 3); }   /* hide the typed password */
        send_str("password: ");
        return;
    }

    if (s.state == RS_PASS) {
        if (s.telnet) { static const uint8_t wont_echo[3] = { IAC, WONT, OPT_ECHO }; send_bytes(wont_echo, 3); }
        send_str("\n");
        uint32_t wait = 0;
        auth_result_t r = auth_verify_ch(AUTH_CH_REMOTE, s.user, s.line, &wait);
        for (int i = 0; i < (int)sizeof(s.line); i++) s.line[i] = 0;                       /* do not keep the password */

        if (r == AUTH_OK) {
            if (auth_has_default_password(s.user)) {
                klog("rcon: login refused for '%s': default password still set", s.user);
                send_str("Remote login is disabled until the default password is changed.\nRun 'passwd' on the local console first.\n");
                session_end("default password");
                return;
            }
            klog("rcon: login ok for '%s' from %I (%s)", s.user, s.peer, s.secure ? "encrypted" : "PLAINTEXT");
            s.state = RS_SHELL;
            send_str("Welcome. Type 'help' for commands, 'exit' to disconnect.\n");
            prompt();
            return;
        }
        if (r == AUTH_LOCKED) {
            out_str("Too many failed attempts. Try again in ", TCP_NOWAIT);
            char t[11]; int n = 0; uint32_t v = wait; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (n) out_char(t[--n], TCP_NOWAIT);
            send_str(" s.\n");
            session_end("locked out");
            return;
        }
        send_str("Login incorrect.\n");
        if (++s.tries >= MAX_LOGIN_TRIES) { session_end("too many failures"); return; }
        s.state = RS_USER;
        send_str("login: ");
        return;
    }

    if (s.state == RS_SHELL) {
        char* p = s.line;
        while (*p == ' ') p++;
        if (*p == '\0') { prompt(); return; }
        if (word_is(p, "exit") || word_is(p, "quit") || word_is(p, "logout")) { send_str("Bye.\n"); session_end("client logout"); return; }
        if (denied(p)) { send_str("That command needs the local console and is not available remotely.\n"); prompt(); return; }

        char logged[60];
        safe_copy(logged, p, sizeof(logged));
        klog("rcon: '%s' ran '%s'", s.user, logged);
        int i = 0;
        for (; p[i] && i < RCON_LINE_MAX; i++) s.pending[i] = p[i];
        s.pending[i] = '\0';
        s.has_pending = true;
        s.state = RS_BUSY;
    }
}

/* Feed one received byte through the telnet filter and the line editor. */
static void feed(uint8_t c) {
    switch (s.iac) {
        case 0:
            if (c == IAC) { s.iac = 1; s.telnet = true; return; }
            break;
        case 1:
            if (c == IAC) { s.iac = 0; break; }                          /* escaped 0xFF: not valid in our text input */
            if (c == SB) { s.iac = 3; return; }
            if (c >= WILL && c <= DONT) { s.verb = c; s.iac = 2; return; }
            s.iac = 0; return;                                            /* other 2-byte commands: ignore */
        case 2: {                                                         /* refuse every option except our own ECHO */
            uint8_t reply[3] = { IAC, 0, c };
            if (c != OPT_ECHO) {
                if (s.verb == DO)        { reply[1] = WONT; send_bytes(reply, 3); }
                else if (s.verb == WILL) { reply[1] = DONT; send_bytes(reply, 3); }
            }
            s.iac = 0; return;
        }
        case 3: if (c == IAC) s.iac = 4; return;
        case 4: s.iac = (c == SE) ? 0 : 3; return;
    }

    if (c == '\r' || c == '\0') return;
    if (c == '\n') { handle_line(); return; }
    if (c == '\b' || c == 127) { if (s.len > 0) s.len--; return; }
    if (c >= 32 && c < 127 && s.len < RCON_LINE_MAX) s.line[s.len++] = (char)c;
}

/* ------------------------------------------------------------------ */
/* network task                                                        */
/* ------------------------------------------------------------------ */

/* A complete Noise message (frame) has arrived. */
static void handle_frame(void) {
    if (s.state == RS_HANDSHAKE) {
        if (!noise_resp_read_msg1(&s.hs, s.frame, (size_t)s.fneed)) { klog("rcon: handshake failed from %I", s.peer); session_end("handshake failed"); return; }
        uint8_t e[32], m2[2 + 48];
        rng_bytes(e, 32);
        m2[0] = 0; m2[1] = 48;
        noise_resp_write_msg2(&s.hs, e, m2 + 2);
        for (int i = 0; i < 32; i++) ((volatile uint8_t*)e)[i] = 0;
        if (tcp_send(s.sock, m2, sizeof(m2), TCP_NOWAIT) != (int)sizeof(m2)) { session_end("send failed"); return; }
        s.state = RS_USER;
        klog("rcon: encrypted channel established with %I", s.peer);
        send_str("OSISOS secure console (Noise NK, ChaCha20-Poly1305)\nlogin: ");
        return;
    }
    if (!noise_decrypt(&s.hs.recv, s.frame, (size_t)s.fneed, s.pbuf)) {      /* forged, corrupted or replayed */
        klog("rcon: bad encrypted frame from %I, closing", s.peer);
        session_end("decrypt failed");
        return;
    }
    s.pp = 0; s.pl = s.fneed - 16;
}

static void frame_byte(uint8_t c) {
    s.frame[s.flen++] = c;
    if (s.fneed < 0) {
        if (s.flen < 2) return;
        s.fneed = (s.frame[0] << 8) | s.frame[1];
        s.flen = 0;
        if (s.fneed < 16 || s.fneed > FRAME_MAX) { klog("rcon: bad frame length from %I", s.peer); session_end("bad frame length"); }
        return;
    }
    if (s.flen == s.fneed) { handle_frame(); s.flen = 0; s.fneed = -1; }
}

void rcon_poll(void) {
    if (!enabled[0] && !enabled[1]) return;

    for (int m = 0; m < 2; m++) {                                          /* new connections */
        if (listener[m] < 0) continue;
        for (;;) {
            int c = tcp_accept(listener[m], TCP_NOWAIT);
            if (c < 0) break;
            if (s.sock < 0) session_start(c, m == RCON_SECURE);
            else {
                if (m == RCON_PLAIN) tcp_send(c, "Console busy: another session is active.\r\n", 41, TCP_NOWAIT);
                tcp_close(c);                                              /* (a secure client cannot be told in the clear) */
            }
        }
    }

    if (s.sock < 0) return;
    if (s.state == RS_BUSY) return;                                        /* the shell task owns the session */

    uint32_t limit = (s.state == RS_HANDSHAKE) ? HANDSHAKE_TICKS : RCON_IDLE_TICKS;
    if ((uint32_t)(net_now() - s.last_active) > limit) {
        if (!s.secure) send_str("\nIdle timeout.\n");
        session_end(s.state == RS_HANDSHAKE ? "handshake timeout" : "idle timeout");
        return;
    }

    for (int rounds = 0; rounds < 8 && s.sock >= 0 && s.state != RS_BUSY; rounds++) {
        if (s.secure) {
            if (s.pp < s.pl) {                                             /* decrypted bytes waiting for the line editor */
                while (s.pp < s.pl && s.sock >= 0 && s.state != RS_BUSY) feed(s.pbuf[s.pp++]);
                continue;
            }
            if (in_pos < in_len) {                                         /* raw bytes waiting for the frame assembler */
                while (in_pos < in_len && s.sock >= 0 && s.pp >= s.pl) frame_byte(in_buf[in_pos++]);
                continue;
            }
        } else if (in_pos < in_len) {
            while (in_pos < in_len && s.sock >= 0 && s.state != RS_BUSY) feed(in_buf[in_pos++]);
            continue;
        }
        int n = tcp_recv(s.sock, in_buf, sizeof(in_buf), TCP_NOWAIT);
        if (n == TCP_AGAIN) return;
        if (n <= 0) { session_end(n == 0 ? "client closed" : "connection error"); return; }
        in_len = n; in_pos = 0;
        if (s.secure) s.last_active = net_now();
    }
}

/* ------------------------------------------------------------------ */
/* shell task side                                                     */
/* ------------------------------------------------------------------ */

bool rcon_take_command(char* out, int max) {
    uint32_t f = net_lock();
    bool r = false;
    if (s.sock >= 0 && s.state == RS_BUSY && s.has_pending) {
        int i = 0;
        for (; s.pending[i] && i < max - 1; i++) out[i] = s.pending[i];
        out[i] = '\0';
        s.has_pending = false;
        r = true;
    }
    net_unlock(f);
    return r;
}

void rcon_exec_begin(void) {
    olen = 0;
    sink_owner = current_task ? current_task->pid : 0;
    terminal_sink = rcon_sink;
}

void rcon_exec_end(void) {
    terminal_sink = 0;
    out_flush(5 * NET_HZ);
    if (s.sock < 0) return;
    if (s.dead) { session_end("client vanished"); return; }
    s.last_active = net_now();
    s.state = RS_SHELL;
    prompt();
}

/* ------------------------------------------------------------------ */
/* enable / disable / status                                           */
/* ------------------------------------------------------------------ */

static void write_flag(const char* path, bool on) {
    if (!ofs_available()) return;
    if (on) ofs_put(path, "1", 1);
    else if (ofs_lookup(path) >= 0) ofs_remove(path);
}

static int hexv(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; }

/* Load the server identity from disk, or create it (once) and save it. */
static bool key_ensure(bool force_new) {
    if (have_key && !force_new) return true;
    if (!force_new && ofs_available()) {
        int idx = ofs_lookup(KEY_FILE);
        char hex[65];
        if (idx >= 0 && ofs_inode_read(idx, 0, hex, 64) == 64) {
            bool ok = true;
            for (int i = 0; i < 32 && ok; i++) { int h = hexv(hex[2*i]), l = hexv(hex[2*i+1]); if (h < 0 || l < 0) ok = false; else srv_priv[i] = (uint8_t)(h * 16 + l); }
            if (ok) { x25519_base(srv_pub, srv_priv); have_key = true; key_persistent = true; return true; }
        }
    }
    rng_bytes(srv_priv, 32);
    x25519_base(srv_pub, srv_priv);
    have_key = true;
    key_persistent = false;
    if (ofs_available()) {
        static const char* d = "0123456789abcdef";
        char hex[65];
        for (int i = 0; i < 32; i++) { hex[2*i] = d[srv_priv[i] >> 4]; hex[2*i+1] = d[srv_priv[i] & 15]; }
        key_persistent = ofs_put(KEY_FILE, hex, 64);
    }
    klog("rcon: new server identity key created (%s)", key_persistent ? "saved to /etc/rcon.key" : "NOT saved: no disk");
    return true;
}

void rcon_set(int mode, bool on, bool persist) {
    if (mode < 0 || mode > 1) return;
    if (on && mode == RCON_SECURE) key_ensure(false);
    if (on != enabled[mode]) {
        enabled[mode] = on;
        if (on) {
            int port = (mode == RCON_SECURE) ? RCON_SECURE_PORT : RCON_PORT;
            listener[mode] = tcp_socket();
            if (listener[mode] >= 0 && tcp_listen(listener[mode], (uint16_t)port) != TCP_OK) { tcp_close(listener[mode]); listener[mode] = -1; enabled[mode] = false; }
            if (enabled[mode]) klog("rcon: %s console enabled on port %u", mode == RCON_SECURE ? "secure" : "PLAINTEXT (unencrypted)", (uint32_t)port);
        } else {
            if (s.sock >= 0 && s.secure == (mode == RCON_SECURE)) { if (!s.secure) send_str("\nRemote console disabled.\n"); session_end("disabled"); }
            if (listener[mode] >= 0) { tcp_close(listener[mode]); listener[mode] = -1; }
            klog("rcon: %s console disabled", mode == RCON_SECURE ? "secure" : "plain");
        }
    }
    if (persist) write_flag(mode == RCON_SECURE ? FLAG_SECURE : FLAG_PLAIN, enabled[mode]);
}

bool rcon_new_key(void) {
    if (s.sock >= 0 && s.secure) session_end("identity changed");
    return key_ensure(true);
}

bool rcon_get_pubkey(uint8_t out[32]) {
    if (!have_key) return false;
    for (int i = 0; i < 32; i++) out[i] = srv_pub[i];
    return true;
}

bool rcon_key_persistent(void) { return key_persistent; }

void rcon_init(void) {
    s.sock = -1;
    if (ofs_available() && ofs_lookup(FLAG_PLAIN) >= 0)  rcon_set(RCON_PLAIN, true, false);
    if (ofs_available() && ofs_lookup(FLAG_SECURE) >= 0) rcon_set(RCON_SECURE, true, false);
}

void rcon_get_status(rcon_status_t* out) {
    out->plain_enabled = enabled[RCON_PLAIN];
    out->secure_enabled = enabled[RCON_SECURE];
    out->session_active = (s.sock >= 0);
    out->session_secure = s.secure;
    out->peer_ip = s.peer;
    for (int i = 0; i < 24; i++) out->user[i] = s.user[i];
    out->idle_secs = s.sock >= 0 ? (net_now() - s.last_active) / NET_HZ : 0;
}
