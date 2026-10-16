/*
 * tcp.c - TCP for OSISOS (RFC 793 core, plus RFC 5961-style RST hardening).
 *
 * What is implemented
 *   - Full state machine: LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1/2,
 *     CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT
 *   - 3-way handshake (active + passive open, simultaneous open), 4-way close
 *   - MSS option (sent on SYN, parsed from peer)
 *   - Reliable delivery: sequence numbers, cumulative ACKs, retransmission
 *     timer with exponential backoff (go-back-N: on timeout, resend from SND.UNA)
 *   - Flow control: advertised receive window, peer window honoured,
 *     zero-window probes, window-update ACK when the app drains the buffer
 *   - RST generation / handling, bounded retries, TIME_WAIT
 *
 * What is NOT implemented (good next steps)
 *   - Out-of-order reassembly (out-of-order segments are dropped + dup-ACKed;
 *     the sender's retransmit recovers them)
 *   - Congestion control (slow start / congestion avoidance / fast retransmit)
 *   - RTT estimation (RTO is a fixed 1 s with backoff)
 *   - Window scaling, SACK, timestamps, delayed ACK, Nagle, urgent data
 *
 * Concurrency: all state lives in the static tcbs[] table. Public API calls
 * and tcp_input()/tcp_timer() run with interrupts disabled (net_lock).
 * Blocking calls drop the lock while they wait (net_wait()).
 */
#include "tcp.h"
#include "net.h"

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     used;           /* slot allocated (app handle or live connection) */
    bool     app_closed;     /* app called tcp_close(): free when state==CLOSED */
    bool     accepted;       /* child of a listener: handed to the app already */
    int      parent;         /* listener slot for passive connections, else -1 */
    tcp_state_t state;
    int      error;          /* sticky error reported by recv/send/connect     */

    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;

    /* send sequence space */
    uint32_t iss;            /* initial send sequence number                   */
    uint32_t snd_una;        /* oldest unacknowledged sequence number          */
    uint32_t snd_nxt;        /* next sequence number to send                   */
    uint32_t snd_max;        /* highest sequence number ever sent (+1)         */
    uint32_t snd_wnd;        /* peer's advertised window                       */
    uint16_t peer_mss;       /* 0 = peer did not say (assume 536)              */

    /* receive sequence space */
    uint32_t irs;            /* initial receive sequence number                */
    uint32_t rcv_nxt;        /* next sequence number we expect                 */

    /* send buffer: ring holding bytes from sequence number tx_seq onward.
     * Bytes [tx_seq, snd_una) never exist: acked bytes are removed at once.   */
    uint32_t tx_seq;
    uint16_t tx_head, tx_len;
    /* receive buffer: in-order bytes waiting for the application              */
    uint16_t rx_head, rx_len;

    bool     fin_pending;    /* app wants to close: send FIN after the data    */
    bool     fin_sent;       /* FIN transmitted since the last go-back         */
    bool     fin_ever;       /* FIN transmitted at least once                  */
    bool     fin_acked;      /* peer acknowledged our FIN                      */
    bool     fin_rcvd;       /* peer's FIN received (EOF after the rx data)    */

    /* timers */
    bool     rto_on;
    uint32_t rto_deadline;
    uint32_t rto;
    uint8_t  retries;
    uint32_t tw_deadline;    /* TIME_WAIT / FIN_WAIT_2 expiry                  */

    uint8_t  txbuf[TCP_TX_BUF];
    uint8_t  rxbuf[TCP_RX_BUF];
} tcb_t;

/* A parsed incoming segment */
typedef struct {
    uint32_t src_ip;
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  flags;
    uint16_t wnd;
    uint16_t mss;            /* from the MSS option, 0 if absent */
    const uint8_t* data;
    uint32_t len;
} seg_t;

static tcb_t   tcbs[TCP_MAX_SOCKS];
static uint8_t seg_buf[NET_MTU];       /* outgoing segment under construction */
static uint8_t stage_buf[TCP_MSS];     /* linearised copy of ring data        */
static uint16_t next_ephemeral;

#define SEQ_LT(a, b)   ((int32_t)((a) - (b)) <  0)
#define SEQ_LEQ(a, b)  ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)   ((int32_t)((a) - (b)) >  0)
#define SEQ_GEQ(a, b)  ((int32_t)((a) - (b)) >= 0)

static inline uint32_t rcv_wnd(const tcb_t* t) { return TCP_RX_BUF - t->rx_len; }

static inline bool time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

const char* tcp_state_name(tcp_state_t st) {
    static const char* names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
        "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
    };
    return (st <= TCP_TIME_WAIT) ? names[st] : "?";
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t tcp_isn(void) {
    static uint32_t counter;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    counter += 0x1F3D5B79u;
    return lo ^ counter ^ (net_now() << 12);
}

static tcb_t* sock(int s) {
    if (s < 0 || s >= TCP_MAX_SOCKS || !tcbs[s].used) return NULL;
    return &tcbs[s];
}

static tcb_t* tcb_alloc(void) {
    int slot = -1;
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        if (!tcbs[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Table full. A busy server leaves many sockets in TIME_WAIT; nobody holds
         * a handle to those (app_closed), so recycle the one closest to expiry
         * rather than refusing new connections. */
        for (int i = 0; i < TCP_MAX_SOCKS; i++) {
            if (tcbs[i].state == TCP_TIME_WAIT && tcbs[i].app_closed &&
                (slot < 0 || SEQ_LT(tcbs[i].tw_deadline, tcbs[slot].tw_deadline))) slot = i;
        }
    }
    if (slot < 0) return NULL;

    memset(&tcbs[slot], 0, sizeof(tcb_t));
    tcbs[slot].used = true;
    tcbs[slot].parent = -1;
    tcbs[slot].state = TCP_CLOSED;
    tcbs[slot].rto = TCP_RTO_INIT;
    return &tcbs[slot];
}

static bool port_in_use(uint16_t port) {
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        if (tcbs[i].used && tcbs[i].local_port == port && tcbs[i].state != TCP_CLOSED) return true;
    }
    return false;
}

static uint16_t pick_ephemeral_port(void) {
    for (int i = 0; i < 16384; i++) {
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        uint16_t p = next_ephemeral++;
        if (!port_in_use(p)) return p;
    }
    return 0;
}

static void rto_arm(tcb_t* t) {
    t->rto_on = true;
    t->rto_deadline = net_now() + t->rto;
}

/* ------------------------------------------------------------------ */
/* Segment transmission                                                */
/* ------------------------------------------------------------------ */

static void tcp_xmit(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                     uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                     const uint8_t* data, uint16_t dlen, bool mss_opt)
{
    uint16_t hlen = (uint16_t)(sizeof(tcp_hdr_t) + (mss_opt ? 4 : 0));
    if ((uint32_t)hlen + dlen + sizeof(ip_hdr_t) > NET_MTU) return;

    tcp_hdr_t* h = (tcp_hdr_t*)seg_buf;
    h->sport  = htons(sport);
    h->dport  = htons(dport);
    h->seq    = htonl(seq);
    h->ack    = htonl(ack);
    h->off    = (uint8_t)((hlen / 4) << 4);
    h->flags  = flags;
    h->window = htons(wnd);
    h->csum   = 0;
    h->urg    = 0;
    if (mss_opt) {                                  /* kind=2, len=4, MSS */
        seg_buf[20] = 2;
        seg_buf[21] = 4;
        seg_buf[22] = (uint8_t)(TCP_MSS >> 8);
        seg_buf[23] = (uint8_t)(TCP_MSS & 0xFF);
    }
    if (dlen) memcpy(seg_buf + hlen, data, dlen);

    uint16_t total = (uint16_t)(hlen + dlen);

    /* Pseudo-header: src ip, dst ip, 0, protocol, TCP length */
    uint8_t ph[12];
    put_ip(ph, netif.ip);
    put_ip(ph + 4, dst_ip);
    ph[8]  = 0;
    ph[9]  = IP_PROTO_TCP;
    ph[10] = (uint8_t)(total >> 8);
    ph[11] = (uint8_t)total;
    h->csum = net_csum_fold(net_csum_add(net_csum_add(0, ph, sizeof(ph)), seg_buf, total));

    net_stats.tcp_tx++;
    net_ip_send(dst_ip, IP_PROTO_TCP, seg_buf, total);
}

static void tcp_send_ack(tcb_t* t) {
    tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
             t->snd_nxt, t->rcv_nxt, TCP_ACK, (uint16_t)rcv_wnd(t), NULL, 0, false);
}

static void tcp_send_rst(tcb_t* t) {
    tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
             t->snd_nxt, t->rcv_nxt, TCP_RST | TCP_ACK, 0, NULL, 0, false);
    net_stats.tcp_rst++;
}

/* Reply with RST to a segment that belongs to no connection (RFC 793, p.65) */
static void send_reset_for(const seg_t* s) {
    uint32_t seglen = s->len + ((s->flags & TCP_SYN) ? 1 : 0) + ((s->flags & TCP_FIN) ? 1 : 0);
    if (s->flags & TCP_ACK) {
        tcp_xmit(s->src_ip, s->dport, s->sport, s->ack, 0, TCP_RST, 0, NULL, 0, false);
    } else {
        tcp_xmit(s->src_ip, s->dport, s->sport, 0, s->seq + seglen, TCP_RST | TCP_ACK, 0, NULL, 0, false);
    }
    net_stats.tcp_rst++;
}

static void send_syn(tcb_t* t) {
    tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
             t->iss, 0, TCP_SYN, (uint16_t)rcv_wnd(t), NULL, 0, true);
    t->snd_nxt = t->iss + 1;
    if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
    if (!t->rto_on) rto_arm(t);
}

static void send_synack(tcb_t* t) {
    tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
             t->iss, t->rcv_nxt, TCP_SYN | TCP_ACK, (uint16_t)rcv_wnd(t), NULL, 0, true);
    t->snd_nxt = t->iss + 1;
    if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
    if (!t->rto_on) rto_arm(t);
}

/* Send as much queued data as the peer's window allows, then the FIN if the
 * app asked to close. Returns the number of segments sent. */
static int tcp_output(tcb_t* t) {
    switch (t->state) {
        case TCP_ESTABLISHED: case TCP_CLOSE_WAIT:
        case TCP_FIN_WAIT_1:  case TCP_CLOSING: case TCP_LAST_ACK:
            break;
        default:
            return 0;
    }

    int sent = 0;
    uint32_t mss = t->peer_mss ? t->peer_mss : 536;
    if (mss > TCP_MSS) mss = TCP_MSS;

    for (;;) {
        uint32_t data_end = t->tx_seq + t->tx_len;
        if (!SEQ_LT(t->snd_nxt, data_end)) break;                 /* nothing unsent */

        uint32_t unsent  = data_end - t->snd_nxt;
        uint32_t wnd_end = t->snd_una + t->snd_wnd;
        uint32_t avail   = SEQ_LT(t->snd_nxt, wnd_end) ? wnd_end - t->snd_nxt : 0;
        uint32_t n = unsent < avail ? unsent : avail;
        if (n > mss) n = mss;

        if (n == 0) {
            /* Peer window is closed and nothing is in flight: send a 1-byte
             * probe so we notice when it re-opens (it also arms the RTO). */
            if (t->snd_wnd == 0 && t->snd_nxt == t->snd_una) n = 1;
            else break;
        }

        /* Copy from the ring (it may wrap) into a linear staging buffer. */
        uint32_t off = t->snd_nxt - t->tx_seq;
        uint32_t idx = (t->tx_head + off) & (TCP_TX_BUF - 1);
        for (uint32_t i = 0; i < n; i++) {
            stage_buf[i] = t->txbuf[idx];
            idx = (idx + 1) & (TCP_TX_BUF - 1);
        }

        uint8_t flags = TCP_ACK | ((n == unsent) ? TCP_PSH : 0);
        tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
                 t->snd_nxt, t->rcv_nxt, flags, (uint16_t)rcv_wnd(t), stage_buf, (uint16_t)n, false);
        t->snd_nxt += n;
        if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        if (!t->rto_on) rto_arm(t);
        sent++;
    }

    /* FIN goes out once every data byte has been transmitted. */
    if (t->fin_pending && !t->fin_sent && !t->fin_acked &&
        t->snd_nxt == t->tx_seq + t->tx_len) {
        tcp_xmit(t->remote_ip, t->local_port, t->remote_port,
                 t->snd_nxt, t->rcv_nxt, TCP_FIN | TCP_ACK, (uint16_t)rcv_wnd(t), NULL, 0, false);
        t->snd_nxt++;
        if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        t->fin_sent = true;
        t->fin_ever = true;
        if (t->state == TCP_ESTABLISHED)     t->state = TCP_FIN_WAIT_1;
        else if (t->state == TCP_CLOSE_WAIT) t->state = TCP_LAST_ACK;
        if (!t->rto_on) rto_arm(t);
        sent++;
    }
    return sent;
}

/* ------------------------------------------------------------------ */
/* Connection teardown helpers                                         */
/* ------------------------------------------------------------------ */

static void tcb_release(tcb_t* t) {
    t->used = false;
    t->state = TCP_CLOSED;
}

/* Kill the connection immediately. Sends a RST if the peer knows about us. */
static void tcp_abort(tcb_t* t, int err, bool send_rst) {
    if (send_rst && t->state != TCP_CLOSED && t->state != TCP_LISTEN &&
        t->state != TCP_SYN_SENT && t->state != TCP_TIME_WAIT) {
        tcp_send_rst(t);
    }
    t->state = TCP_CLOSED;
    t->error = err;
    t->rto_on = false;
}

static void enter_time_wait(tcb_t* t) {
    t->state = TCP_TIME_WAIT;
    t->rto_on = false;
    t->tw_deadline = net_now() + TCP_TIME_WAIT_TICKS;
}

/* ------------------------------------------------------------------ */
/* ACK processing                                                      */
/* ------------------------------------------------------------------ */

/* Precondition: snd_una <= ack <= snd_max. */
static void tcp_process_ack(tcb_t* t, uint32_t ack, uint16_t wnd) {
    if (SEQ_GT(ack, t->snd_una)) {
        uint32_t acked_data = 0;
        if (SEQ_GT(ack, t->tx_seq)) {
            acked_data = ack - t->tx_seq;
            if (acked_data > t->tx_len) acked_data = t->tx_len;   /* rest = the FIN */
        }
        t->tx_head = (uint16_t)((t->tx_head + acked_data) & (TCP_TX_BUF - 1));
        t->tx_len  = (uint16_t)(t->tx_len - acked_data);
        t->tx_seq += acked_data;

        if (t->fin_ever && t->tx_len == 0 && SEQ_GEQ(ack, t->tx_seq + 1)) t->fin_acked = true;

        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, ack)) t->snd_nxt = ack;   /* we had gone back; peer already has it */

        t->retries = 0;
        t->rto = TCP_RTO_INIT;
        if (t->snd_una == t->snd_max) t->rto_on = false;
        else rto_arm(t);
    }
    t->snd_wnd = wnd;
}

/* ------------------------------------------------------------------ */
/* Segment arrival                                                     */
/* ------------------------------------------------------------------ */

static bool in_window(const tcb_t* t, uint32_t seq) {
    uint32_t w = rcv_wnd(t);
    return SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + w);
}

/* Our FIN was acknowledged: advance the closing state machine. */
static void fin_acked_transition(tcb_t* t) {
    if (!t->fin_acked) return;
    switch (t->state) {
        case TCP_FIN_WAIT_1: t->state = TCP_FIN_WAIT_2;
                             t->tw_deadline = net_now() + TCP_FIN_WAIT2_TICKS; break;
        case TCP_CLOSING:    enter_time_wait(t); break;
        case TCP_LAST_ACK:   t->state = TCP_CLOSED; break;
        default: break;
    }
}

static void tcp_segment(tcb_t* t, const seg_t* s) {

    /* ---- SYN_SENT (RFC 793, "SYN-SENT STATE") ---- */
    if (t->state == TCP_SYN_SENT) {
        bool ack_ok = false;
        if (s->flags & TCP_ACK) {
            if (SEQ_LEQ(s->ack, t->iss) || SEQ_GT(s->ack, t->snd_max)) {
                if (!(s->flags & TCP_RST)) {
                    tcp_xmit(s->src_ip, s->dport, s->sport, s->ack, 0, TCP_RST, 0, NULL, 0, false);
                }
                return;
            }
            ack_ok = true;
        }
        if (s->flags & TCP_RST) {
            if (ack_ok) tcp_abort(t, TCP_REFUSED, false);
            return;
        }
        if (s->flags & TCP_SYN) {
            t->irs = s->seq;
            t->rcv_nxt = s->seq + 1;
            t->peer_mss = s->mss;
            if (ack_ok) {
                tcp_process_ack(t, s->ack, s->wnd);
                t->state = TCP_ESTABLISHED;
                tcp_send_ack(t);
                tcp_output(t);                    /* data queued while connecting */
            } else {                              /* simultaneous open */
                t->snd_wnd = s->wnd;
                t->state = TCP_SYN_RCVD;
                t->rto_on = false;
                send_synack(t);
            }
        }
        return;
    }

    /* ---- a retransmitted SYN while we wait for the final ACK ---- */
    if (t->state == TCP_SYN_RCVD && (s->flags & TCP_SYN) && s->seq == t->irs) {
        t->rto_on = false;
        send_synack(t);
        return;
    }

    /* ---- 1. is the segment acceptable? ---- */
    uint32_t seglen = s->len + ((s->flags & TCP_SYN) ? 1 : 0) + ((s->flags & TCP_FIN) ? 1 : 0);
    bool ok;
    if (seglen == 0) {
        ok = (rcv_wnd(t) == 0) ? (s->seq == t->rcv_nxt) : in_window(t, s->seq);
    } else if (rcv_wnd(t) == 0) {
        ok = false;
    } else {
        ok = in_window(t, s->seq) || in_window(t, s->seq + seglen - 1);
    }
    if (!ok) {
        if (!(s->flags & TCP_RST)) {
            /* An old duplicate (e.g. the peer re-sending its FIN) can still carry a
             * NEWER acknowledgement of our data/FIN. Ignoring it would leave us stuck
             * in CLOSING / FIN_WAIT_1 until the retransmit timer fires. Like Linux,
             * honour the ACK field but discard the payload. */
            if ((s->flags & TCP_ACK) && t->state != TCP_SYN_RCVD && t->state != TCP_TIME_WAIT &&
                SEQ_GT(s->ack, t->snd_una) && SEQ_LEQ(s->ack, t->snd_max)) {
                tcp_process_ack(t, s->ack, s->wnd);
                fin_acked_transition(t);
                if (t->state == TCP_CLOSED) return;
            }
            tcp_send_ack(t);
            if (t->state == TCP_TIME_WAIT && (s->flags & TCP_FIN)) enter_time_wait(t);
        }
        return;
    }

    /* ---- 2. RST: only an exact match resets us (blind-reset protection) ---- */
    if (s->flags & TCP_RST) {
        if (s->seq == t->rcv_nxt) {
            if (t->state == TCP_SYN_RCVD && t->parent >= 0) tcb_release(t);   /* embryonic child */
            else tcp_abort(t, TCP_RESET, false);
        } else {
            tcp_send_ack(t);
        }
        return;
    }

    /* ---- 3. a stray SYN inside the window ---- */
    if (s->flags & TCP_SYN) { tcp_send_ack(t); return; }

    /* ---- 4. ACK ---- */
    if (!(s->flags & TCP_ACK)) return;

    if (t->state == TCP_SYN_RCVD) {
        if (SEQ_LT(t->snd_una, s->ack) && SEQ_LEQ(s->ack, t->snd_max)) {
            t->state = TCP_ESTABLISHED;
            tcp_process_ack(t, s->ack, s->wnd);
        } else {
            tcp_xmit(s->src_ip, s->dport, s->sport, s->ack, 0, TCP_RST, 0, NULL, 0, false);
            return;
        }
    } else if (t->state != TCP_TIME_WAIT) {
        if (SEQ_GT(s->ack, t->snd_max)) { tcp_send_ack(t); return; }   /* acks data we never sent */
        if (SEQ_GEQ(s->ack, t->snd_una)) tcp_process_ack(t, s->ack, s->wnd);
    }

    fin_acked_transition(t);
    if (t->state == TCP_CLOSED) return;

    /* ---- 5. payload and FIN ---- */
    bool need_ack = false;
    const uint8_t* data = s->data;
    uint32_t dlen = s->len;
    uint32_t seq  = s->seq;
    bool fin = (s->flags & TCP_FIN) != 0;

    if (dlen || fin) {
        if (SEQ_LT(seq, t->rcv_nxt)) {                 /* overlaps data we already have: trim */
            uint32_t skip = t->rcv_nxt - seq;
            if (skip > dlen) { dlen = 0; fin = false; }   /* even the FIN is old */
            else { data += skip; dlen -= skip; }
            seq = t->rcv_nxt;
        }
        if (seq != t->rcv_nxt) {                       /* a hole before this segment */
            tcp_send_ack(t);                           /* dup-ACK: sender will retransmit */
            return;
        }

        if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) {
            if (dlen) {
                uint32_t space = TCP_RX_BUF - t->rx_len;
                uint32_t n = t->app_closed ? dlen : (dlen < space ? dlen : space);
                if (!t->app_closed) {                  /* (closed sockets just discard) */
                    uint32_t idx = (t->rx_head + t->rx_len) & (TCP_RX_BUF - 1);
                    for (uint32_t i = 0; i < n; i++) {
                        t->rxbuf[idx] = data[i];
                        idx = (idx + 1) & (TCP_RX_BUF - 1);
                    }
                    t->rx_len = (uint16_t)(t->rx_len + n);
                }
                t->rcv_nxt += n;
                if (n < dlen) fin = false;             /* tail was dropped: FIN not reached yet */
                need_ack = true;
            }
            if (fin) {
                t->rcv_nxt++;
                t->fin_rcvd = true;
                need_ack = true;
                switch (t->state) {
                    case TCP_ESTABLISHED: t->state = TCP_CLOSE_WAIT; break;
                    case TCP_FIN_WAIT_1:  t->state = TCP_CLOSING;    break;   /* our FIN not acked yet */
                    case TCP_FIN_WAIT_2:  enter_time_wait(t);        break;
                    default: break;
                }
            }
        } else if (dlen || fin) {
            need_ack = true;                           /* duplicate FIN etc. */
        }
    }

    /* ---- 6. respond ---- */
    int sent = tcp_output(t);                          /* data/FIN carry the ACK */
    if (need_ack && sent == 0) tcp_send_ack(t);
}

/* A SYN arrived for a listening socket: create the connection in SYN_RCVD. */
static void tcp_accept_syn(tcb_t* l, const seg_t* s) {
    int lidx = (int)(l - tcbs);
    int pending = 0;
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        if (tcbs[i].used && tcbs[i].parent == lidx && !tcbs[i].accepted) pending++;
    }
    if (pending >= TCP_BACKLOG) { net_stats.tcp_syn_drop++; return; }   /* client will retry */

    tcb_t* c = tcb_alloc();
    if (!c) { net_stats.tcp_syn_drop++; return; }

    c->parent      = lidx;
    c->local_port  = l->local_port;
    c->remote_port = s->sport;
    c->remote_ip   = s->src_ip;
    c->state       = TCP_SYN_RCVD;
    c->irs         = s->seq;
    c->rcv_nxt     = s->seq + 1;
    c->iss         = tcp_isn();
    c->snd_una = c->snd_nxt = c->snd_max = c->iss;
    c->tx_seq      = c->iss + 1;
    c->snd_wnd     = s->wnd;
    c->peer_mss    = s->mss;
    send_synack(c);
}

void tcp_input(uint32_t src_ip, const uint8_t* pkt, uint16_t len) {
    net_stats.tcp_rx++;
    if (len < sizeof(tcp_hdr_t)) return;

    uint8_t ph[12];
    put_ip(ph, src_ip);
    put_ip(ph + 4, netif.ip);
    ph[8] = 0; ph[9] = IP_PROTO_TCP; ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    if (net_csum_fold(net_csum_add(net_csum_add(0, ph, sizeof(ph)), pkt, len)) != 0) {
        net_stats.tcp_badsum++;
        return;
    }

    const tcp_hdr_t* h = (const tcp_hdr_t*)pkt;
    uint16_t hlen = (uint16_t)((h->off >> 4) * 4);
    if (hlen < sizeof(tcp_hdr_t) || hlen > len) return;

    seg_t s;
    s.src_ip = src_ip;
    s.sport  = ntohs(h->sport);
    s.dport  = ntohs(h->dport);
    s.seq    = ntohl(h->seq);
    s.ack    = ntohl(h->ack);
    s.flags  = h->flags;
    s.wnd    = ntohs(h->window);
    s.data   = pkt + hlen;
    s.len    = len - hlen;
    s.mss    = 0;

    for (uint16_t i = sizeof(tcp_hdr_t); i < hlen; ) {          /* options */
        uint8_t kind = pkt[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= hlen) break;
        uint8_t olen = pkt[i + 1];
        if (olen < 2 || i + olen > hlen) break;
        if (kind == 2 && olen == 4) s.mss = (uint16_t)((pkt[i + 2] << 8) | pkt[i + 3]);
        i = (uint16_t)(i + olen);
    }

    /* Established / connecting sockets: exact 4-tuple match. */
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        tcb_t* t = &tcbs[i];
        if (t->used && t->state != TCP_CLOSED && t->state != TCP_LISTEN &&
            t->local_port == s.dport && t->remote_port == s.sport && t->remote_ip == s.src_ip) {
            tcp_segment(t, &s);
            return;
        }
    }

    /* Otherwise: a listener on that port? */
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        tcb_t* l = &tcbs[i];
        if (l->used && l->state == TCP_LISTEN && l->local_port == s.dport) {
            if (s.flags & TCP_RST) return;
            if (s.flags & TCP_ACK) send_reset_for(&s);
            else if (s.flags & TCP_SYN) tcp_accept_syn(l, &s);
            return;
        }
    }

    /* Nobody home. */
    if (!(s.flags & TCP_RST)) send_reset_for(&s);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

static void tcp_rto_fire(tcb_t* t) {
    bool probing = (t->snd_wnd == 0 && t->state != TCP_SYN_SENT && t->state != TCP_SYN_RCVD);
    if (!probing && ++t->retries > TCP_MAX_RETRIES) {
        tcp_abort(t, TCP_TIMEOUT, true);
        return;
    }
    t->rto = (t->rto * 2 > TCP_RTO_MAX) ? TCP_RTO_MAX : t->rto * 2;
    t->rto_on = false;
    net_stats.tcp_retx++;
    if (t->state <= TCP_TIME_WAIT) net_stats.tcp_retx_state[t->state]++;

    switch (t->state) {
        case TCP_SYN_SENT: send_syn(t);    break;
        case TCP_SYN_RCVD: send_synack(t); break;
        default:
            t->snd_nxt = t->snd_una;                   /* go back N: resend from the oldest unacked byte */
            t->fin_sent = false;
            tcp_output(t);
            break;
    }
    if (!t->rto_on && t->snd_una != t->snd_max) rto_arm(t);
}

void tcp_timer(void) {
    uint32_t now = net_now();
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        tcb_t* t = &tcbs[i];
        if (!t->used) continue;

        if (t->state == TCP_TIME_WAIT && time_reached(now, t->tw_deadline)) {
            t->state = TCP_CLOSED;
        } else if (t->state == TCP_FIN_WAIT_2 && time_reached(now, t->tw_deadline)) {
            tcp_abort(t, TCP_TIMEOUT, true);
        } else if (t->rto_on && time_reached(now, t->rto_deadline)) {
            tcp_rto_fire(t);
        }

        /* Garbage-collect connections nobody will ever look at again. */
        if (t->used && t->state == TCP_CLOSED &&
            (t->app_closed || (t->parent >= 0 && !t->accepted))) {
            tcb_release(t);
        }
    }
}

void tcp_init(void) {
    memset(tcbs, 0, sizeof(tcbs));
    next_ephemeral = 49152;
}

/* ------------------------------------------------------------------ */
/* Socket API                                                          */
/* ------------------------------------------------------------------ */

static inline bool timed_out(uint32_t start, uint32_t timeout) {
    return timeout != TCP_FOREVER && (uint32_t)(net_now() - start) >= timeout;
}

int tcp_socket(void) {
    uint32_t f = net_lock();
    tcb_t* t = tcb_alloc();
    int r = t ? (int)(t - tcbs) : TCP_ERR;
    net_unlock(f);
    return r;
}

int tcp_listen(int s, uint16_t port) {
    uint32_t f = net_lock();
    tcb_t* t = sock(s);
    int r = TCP_ERR;
    if (t && t->state == TCP_CLOSED && port != 0) {
        bool busy = false;
        for (int i = 0; i < TCP_MAX_SOCKS; i++) {
            if (tcbs[i].used && tcbs[i].state == TCP_LISTEN && tcbs[i].local_port == port) busy = true;
        }
        if (!busy) { t->local_port = port; t->state = TCP_LISTEN; r = TCP_OK; }
    }
    net_unlock(f);
    return r;
}

int tcp_accept(int s, uint32_t timeout) {
    uint32_t start = net_now();
    for (;;) {
        uint32_t f = net_lock();
        tcb_t* l = sock(s);
        if (!l || l->state != TCP_LISTEN) { net_unlock(f); return TCP_ERR; }
        for (int i = 0; i < TCP_MAX_SOCKS; i++) {
            tcb_t* c = &tcbs[i];
            if (c->used && c->parent == s && !c->accepted &&
                c->state != TCP_SYN_RCVD && c->state != TCP_CLOSED) {
                c->accepted = true;
                net_unlock(f);
                return i;
            }
        }
        net_unlock(f);
        if (timeout == TCP_NOWAIT) return TCP_AGAIN;
        if (timed_out(start, timeout)) return TCP_TIMEOUT;
        net_wait();
    }
}

int tcp_connect(int s, uint32_t ip, uint16_t port, uint32_t timeout) {
    uint32_t f = net_lock();
    tcb_t* t = sock(s);
    if (!t || t->state != TCP_CLOSED || netif.ip == 0) { net_unlock(f); return TCP_ERR; }

    t->error       = 0;
    t->remote_ip   = ip;
    t->remote_port = port;
    t->local_port  = pick_ephemeral_port();
    t->iss         = tcp_isn();
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    t->tx_seq      = t->iss + 1;
    t->tx_head = t->tx_len = t->rx_head = t->rx_len = 0;
    t->fin_pending = t->fin_sent = t->fin_ever = t->fin_acked = t->fin_rcvd = false;
    t->peer_mss    = 0;
    t->snd_wnd     = 0;
    t->rto         = TCP_RTO_INIT;
    t->retries     = 0;
    t->rto_on      = false;
    t->state       = TCP_SYN_SENT;
    send_syn(t);
    net_unlock(f);

    uint32_t start = net_now();
    for (;;) {
        f = net_lock();
        int r = 1;                                        /* 1 = keep waiting */
        if (t->state != TCP_SYN_SENT && t->state != TCP_SYN_RCVD) {
            r = (t->state == TCP_CLOSED) ? (t->error ? t->error : TCP_ERR) : TCP_OK;
        }
        if (r == 1 && timed_out(start, timeout)) {
            t->state = TCP_CLOSED;
            t->error = TCP_TIMEOUT;
            t->rto_on = false;
            r = TCP_TIMEOUT;
        }
        net_unlock(f);
        if (r != 1) return r;
        net_wait();
    }
}

int tcp_send_space(int s) {
    uint32_t f = net_lock();
    tcb_t* t = sock(s);
    int r = t ? (int)(TCP_TX_BUF - t->tx_len) : TCP_ERR;
    net_unlock(f);
    return r;
}

int tcp_send(int s, const void* buf, int len, uint32_t timeout) {
    const uint8_t* p = (const uint8_t*)buf;
    int done = 0;
    uint32_t start = net_now();

    while (done < len) {
        uint32_t f = net_lock();
        tcb_t* t = sock(s);
        int err = 0;
        uint32_t n = 0;

        if (!t) err = TCP_ERR;
        else if (t->error) err = t->error;
        else if ((t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) || t->fin_pending) err = TCP_ERR;
        else {
            uint32_t space = TCP_TX_BUF - t->tx_len;
            n = (uint32_t)(len - done) < space ? (uint32_t)(len - done) : space;
            uint32_t idx = (t->tx_head + t->tx_len) & (TCP_TX_BUF - 1);
            for (uint32_t i = 0; i < n; i++) {
                t->txbuf[idx] = p[done + (int)i];
                idx = (idx + 1) & (TCP_TX_BUF - 1);
            }
            t->tx_len = (uint16_t)(t->tx_len + n);
            if (n) tcp_output(t);
        }
        net_unlock(f);

        if (err) return done > 0 ? done : err;
        done += (int)n;
        if (done >= len) break;
        if (timeout == TCP_NOWAIT) return done > 0 ? done : TCP_AGAIN;
        if (timed_out(start, timeout)) return done > 0 ? done : TCP_TIMEOUT;
        if (n == 0) net_wait();
    }
    return done;
}

int tcp_recv(int s, void* buf, int len, uint32_t timeout) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t start = net_now();
    if (len <= 0) return 0;

    for (;;) {
        uint32_t f = net_lock();
        tcb_t* t = sock(s);
        if (!t) { net_unlock(f); return TCP_ERR; }

        if (t->rx_len > 0) {
            uint32_t before = rcv_wnd(t);
            uint32_t n = (uint32_t)len < t->rx_len ? (uint32_t)len : t->rx_len;
            for (uint32_t i = 0; i < n; i++) {
                p[i] = t->rxbuf[t->rx_head];
                t->rx_head = (uint16_t)((t->rx_head + 1) & (TCP_RX_BUF - 1));
            }
            t->rx_len = (uint16_t)(t->rx_len - n);
            uint32_t after = rcv_wnd(t);
            /* Tell the sender the window re-opened (otherwise it may stall). */
            if ((before == 0 || (before < TCP_MSS && after >= TCP_MSS)) &&
                (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)) {
                tcp_send_ack(t);
            }
            net_unlock(f);
            return (int)n;
        }

        int r = 1;                                        /* 1 = keep waiting */
        if (t->error) r = t->error;
        else if (t->fin_rcvd) r = 0;                      /* clean EOF */
        else if (t->state == TCP_CLOSED || t->state == TCP_LISTEN || t->state == TCP_TIME_WAIT) r = TCP_ERR;
        net_unlock(f);
        if (r != 1) return r;

        if (timeout == TCP_NOWAIT) return TCP_AGAIN;
        if (timed_out(start, timeout)) return TCP_TIMEOUT;
        net_wait();
    }
}

int tcp_close(int s) {
    uint32_t f = net_lock();
    tcb_t* t = sock(s);
    if (!t) { net_unlock(f); return TCP_ERR; }

    switch (t->state) {
        case TCP_LISTEN:
            for (int i = 0; i < TCP_MAX_SOCKS; i++) {           /* abort unaccepted children */
                tcb_t* c = &tcbs[i];
                if (c->used && c->parent == s && !c->accepted) {
                    tcp_abort(c, TCP_RESET, true);
                    tcb_release(c);
                }
            }
            tcb_release(t);
            break;
        case TCP_CLOSED:
        case TCP_SYN_SENT:
            tcb_release(t);
            break;
        case TCP_SYN_RCVD:
        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            t->app_closed = true;
            t->fin_pending = true;
            tcp_output(t);                      /* flushes data, then the FIN */
            break;
        default:                                /* already closing: just let it finish */
            t->app_closed = true;
            break;
    }
    net_unlock(f);
    return TCP_OK;
}

bool tcp_get_info(int s, tcp_info_t* out) {
    uint32_t f = net_lock();
    tcb_t* t = sock(s);
    bool ok = (t != NULL);
    if (ok) {
        out->state       = t->state;
        out->local_port  = t->local_port;
        out->remote_ip   = t->remote_ip;
        out->remote_port = t->remote_port;
        out->rx_queued   = t->rx_len;
        out->tx_queued   = t->tx_len;
    }
    net_unlock(f);
    return ok;
}
