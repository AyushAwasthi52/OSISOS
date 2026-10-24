# Networking

The network stack lives in `kernel/net/` and sits on the e1000 driver. All of it is written from scratch.
Time everywhere is the 100 Hz PIT tick (`net_now()`; `NET_HZ = 100`).

```
 shell commands / rcon     netcmd.c   rcon.c
 HTTP, echo                httpd.c    services.c
 DHCP    DNS               dhcp.c     dns.c
 TCP     UDP               tcp.c      udp.c
 IPv4 + ICMP + ARP         net.c
 e1000 driver              drivers/e1000.c
```

Conventions: IPv4 addresses in APIs are host-order `uint32_t` (`IP4(10,0,2,15)`); header structs hold wire-order
bytes/network-order fields; every public entry point takes `net_lock()` (interrupts off) and must not block while holding it.

## 1. Driver and Ethernet (`drivers/e1000.c`, `net.c`)

* 32 receive descriptors (2 KB buffers) and 8 transmit descriptors; polled, no interrupts yet. The NIC is
  configured promiscuous, so `net_rx_frame()` filters on the destination MAC (ours or broadcast).
* RX buffers are driver-owned; a frame stays valid until the next `e1000_receive_packet()` (descriptors are handed back lazily).
  Frames with errors or without EOP are dropped.
* TX copies the frame into a driver-owned buffer and waits (bounded) for a free descriptor, so callers may reuse their buffer at once.
* Setup fails loudly if a DMA buffer allocation fails.

## 2. ARP

8-entry cache (5-minute TTL, oldest evicted). Senders are learned only if the packet targets us or they are already
known (no unsolicited cache filling). A datagram whose next hop is unknown is **queued** (4 slots) while the request is
broadcast; it is retried once a second, 3 times, then dropped. The queue is flushed when the reply arrives.
`net_set_ip()` sends a gratuitous ARP.

## 3. IPv4 and ICMP

* Receive: version/IHL/length/checksum validated, fragments dropped (we never fragment: DF is set), accepted if addressed to us,
  to 255.255.255.255 or to the subnet broadcast. Protocol dispatch: ICMP, UDP, TCP (unicast only).
* Send: next hop = destination if on-link (mask), else the gateway; broadcasts go to the Ethernet broadcast address. With no
  address configured only limited broadcasts may be sent (that is what lets DHCP work).
* **Loopback**: `127.x.x.x` is mapped to our own address (`net_canon`), and packets addressed to ourselves are queued internally
  (8 slots) and delivered by the next `net_poll()` as if received from the wire.
* ICMP: echo request -> reply; `icmp_ping()` is a blocking client with an RTT in ticks. No ICMP error generation yet.
* Checksums use the byte-order-independent RFC 1071 routine (`net_csum_add` / `net_csum_fold`).

## 4. UDP (`udp.c`)

8 sockets, each with a queue of 4 datagrams (up to 1472 bytes). API: `udp_socket / bind / sendto / recvfrom / close`;
`sendto` auto-binds an ephemeral port. Checksums are verified (0 = "none" accepted) and generated (0 -> 0xFFFF).
Datagrams for unbound ports are silently dropped (no ICMP port-unreachable).

## 5. TCP (`tcp.c`)

A full RFC 793 state machine (LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1/2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT)
including simultaneous open/close, with RFC 5961-style reset hardening.

| Item | Value |
|---|---|
| Sockets | 12 (static table); each has a 4 KB receive ring and a 4 KB send ring |
| MSS | 1460 advertised; the peer's MSS option is honoured (536 if absent) |
| Retransmission | one timer per connection, go-back-N from `SND.UNA`; RTO 1 s doubling to 32 s; abort after 8 retries |
| Flow control | advertised window = free receive space; peer window honoured; 1-byte zero-window probes; window-update ACK when the app drains the buffer |
| Close | TIME_WAIT 2 s (shortened, only 12 sockets); a full table recycles the oldest TIME_WAIT socket; FIN_WAIT_2 times out after 60 s |
| Listen | backlog of 4 un-accepted connections per listener |
| ISN | TSC- and tick-mixed counter |
| Handling of old duplicates | still honours the ACK field (fixes simultaneous-close stalls) |

Socket API (`tcp.h`): `tcp_socket, tcp_listen, tcp_accept, tcp_connect, tcp_send, tcp_recv, tcp_close, tcp_send_space, tcp_get_info`.
Timeouts are in ticks: `TCP_NOWAIT` (0) never blocks and returns `TCP_AGAIN`; `TCP_FOREVER` waits. Errors are negative:
`TCP_ERR, TCP_AGAIN, TCP_TIMEOUT, TCP_RESET, TCP_REFUSED`. `tcp_recv` returns 0 at EOF.

**Not implemented:** out-of-order reassembly (out-of-order segments are dropped and dup-ACKed; the sender retransmits),
congestion control (slow start / fast retransmit), RTT estimation, SACK, window scaling, timestamps, delayed ACK, Nagle, urgent data.

## 6. DHCP (`dhcp.c`)

Non-blocking client state machine: `INIT -> SELECTING -> REQUESTING -> BOUND -> RENEWING`. DISCOVER/REQUEST are broadcast from
0.0.0.0 with the BROADCAST flag; DISCOVER retries after 1, 2, 4, 8 s. A lease is applied (address, mask, router, DNS) and
renewed by unicast at T1 (50 %), by broadcast after T2 (87.5 %); at expiry the address is dropped. If nothing answers in ~15 s
a **static fallback** (`10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`) is installed and discovery retries every minute.
`ifconfig <ip> <mask> <gw>` switches DHCP off. Not implemented: ARP-conflict probing, DECLINE, RELEASE, INIT-REBOOT.

## 7. DNS (`dns.c`)

Blocking stub resolver for A records: one recursive query to `netif.dns`, 3 tries, 8-entry cache (TTL capped at 5 minutes).
Replies are accepted only from the configured server on port 53 with the matching 16-bit id. Names are validated, compression
pointers are parsed with bounds checks, CNAME chains work through the A records recursive servers include. Do not call it from the network task.

## 8. HTTP server (`httpd.c`)

Port 80, six concurrent connections, HTTP/1.1 with `Connection: close`. A per-connection non-blocking state machine reads the
request (max 768 bytes), builds the response and **streams the body from disk in 1 KB chunks**, copying only what the TCP send
buffer has room for, so slow clients never block others. Extra clients wait in the TCP backlog.

| Status | When |
|---|---|
| 200 | file, directory listing, `/_status`, welcome page |
| 301 | `/dir` -> `/dir/` |
| 400 | malformed request line, `.`/`..` segments, backslash/control characters, bad `%xx` |
| 404 | not found; anything under `/etc` |
| 405 | method other than GET/HEAD (with `Allow:`) |
| 414 / 431 | target / headers too long |
| 418 | `/teapot` |

30-second idle timeout. An access log (last 16) is kept and every request is written to the kernel log.

## 9. Remote console (`rcon.c`)

Two listeners, one shared session slot. **Port 2324 is encrypted**; port 2323 is the legacy plaintext mode.

Encrypted framing and protocol: `Noise_NK_25519_ChaChaPoly_SHA256` (prologue `OSISOS-RCON-1`). Every message on the TCP stream is
`2-byte big-endian length | body`, bodies at most 700 bytes.

```
client                                                    server (static key s, pinned by the client)
  -> 48 B: e, es                                            handshake message 1   (ephemeral key + AEAD tag)
  <- 48 B: e, ee                                            handshake message 2
  == transport: ChaCha20-Poly1305, 64-bit counter nonces, one key per direction (forward secret) ==
  <- "login: "  -> user  <- "password: "  -> password       (text, but inside encrypted messages)
```

The network task runs the handshake (3 X25519 operations, about 0.1 s) and the framing; decrypted bytes feed the same line editor as the
plaintext mode. Any invalid length, failed decryption (forgery, corruption, replay: the nonce counter only advances on success) or
10-second handshake stall closes the connection. Commands execute in the shell task, with output encrypted back (`send_secure`).
Crypto code: `core/x25519.c`, `chacha20poly1305.c`, `noise.c`, `sha256.c`, `rng.c`. Key management: `/etc/rcon.key` (see [SECURITY.md](SECURITY.md)).

## 10. Packet walk-through: `http 10.0.2.2 8000 /`

1. The shell task calls `tcp_connect()` -> `send_syn()` -> `tcp_xmit()` builds the segment with a pseudo-header checksum ->
   `net_ip_send()` -> next hop is the gateway; its MAC is unknown, so the datagram is queued and an ARP request goes out.
2. The network task's `net_poll()` receives the ARP reply, `arp_update()` flushes the queued SYN through `eth_send()` -> `e1000_send_packet()`.
3. The SYN-ACK arrives: `net_rx_frame` -> `ip_input` -> `tcp_input` -> `tcp_segment` (SYN_SENT) -> ESTABLISHED, ACK sent.
4. The shell task, sleeping in `net_wait()`, sees the state change and returns from `tcp_connect()`; the request and response
   then flow through the send/receive rings with ACKs, and `tcp_close()` runs the FIN handshake.

## 11. Diagnostics

* `ifconfig`: frame/segment counters, retransmissions, dropped SYNs. `netstat`: socket table. `arp`, `dhcp`, `dns`.
* `netdrop N`: random loss, both directions. `dmesg`: DHCP, httpd, auth, rcon events.
* Packet capture under QEMU: add `-object filter-dump,id=f1,netdev=n0,file=/tmp/net.pcap` and read it with `tcpdump -nn -r`.
* Two-instance mode (`make run`, `make run2`) joins two VMs with `-netdev socket`; with no DHCP server each falls back to `10.0.2.15` / `10.0.2.16` (chosen by MAC).
