# OSISOS TCP/IP stack

A from-scratch IPv4 + TCP stack for the OSISOS kernel, sitting on top of the
existing e1000 driver. Everything lives in `kernel/net/`.

```
 shell commands   ifconfig arp ping nslookup dns dhcp netstat nc http  net/netcmd.c
 httpd            static file server on port 80, streams from OFS      net/httpd.c
 built-in servers echo (TCP+UDP port 7), polling of all services      net/services.c
 DHCP / DNS       lease acquisition+renewal; A-record resolver+cache net/dhcp.c  dns.c
 TCP              state machine, retransmit, flow control, sockets   net/tcp.c  tcp.h
 UDP              datagram sockets with receive queues               net/udp.c  udp.h
 IPv4 + ICMP      routing, checksums, echo                           net/net.c
 ARP              cache, pending-packet queue                        net/net.c
 Ethernet         dst-MAC filtering                                  net/net.c
 e1000 driver     raw frames in/out                                  drivers/e1000.c
```

## Try it

```sh
cd kernel
make run-direct        # boots osisos.bin straight in QEMU (no GRUB needed)
# or: make run-user    # via the ISO
```

QEMU's user-mode network gives the guest `10.0.2.15`, gateway/host `10.0.2.2`.
The address now comes from **DHCP** (QEMU's built-in server hands out 10.0.2.15,
gateway/host `10.0.2.2`, DNS `10.0.2.3`). Log in (`admin` / `osisos`), then:

| Command | What it does |
|---|---|
| `ifconfig` | MAC, IP, counters. `ifconfig <ip> <mask> <gw>` to change. |
| `ping 10.0.2.2` | ICMP echo x4. Accepts a hostname too: `ping example.com`. |
| `nslookup <name>` | Resolve through DNS. `dns` shows the server, `dns <ip>` changes it. |
| `dhcp` / `dhcp renew` | Show DHCP state and lease / renew the lease now. |
| `arp` | ARP cache. |
| `mkdir www` + `fetch <host> <port> <url> /www/<name>` | Download a file over HTTP into OFS: publishes it on the web server. |
| `httpd` / `httpd root <dir>` | Server stats, last requests (access log), document root. |
| `http 10.0.2.2 8000 /` | TCP client: HTTP GET to a server on your host. |
| `nc 10.0.2.2 5555 hello` | TCP client: connect, send `hello\n`, print the reply. |
| `netstat` | TCP socket table and states. |
| `netdrop 15` | Drop 15% of IP frames (rx+tx) to watch retransmission. `netdrop 0` to stop. |

Host side (while the VM runs):

```sh
python3 -m http.server 8000            # then, in OSISOS:  http 10.0.2.2 8000 /
curl http://127.0.0.1:8080/            # served BY the OSISOS kernel (port 80)
nc 127.0.0.1 8007                      # echo server inside OSISOS (port 7)
```

Run the automated suite (needs `qemu-system-i386`, Python 3):

```sh
cd kernel && make osisos.bin && python3 ../tools/nettest.py
```

## Using TCP from kernel code

```c
#include "tcp.h"

/* client */
int s = tcp_socket();
if (tcp_connect(s, IP4(10,0,2,2), 8000, 5 * NET_HZ) == TCP_OK) {
    tcp_send(s, "hello", 5, TCP_FOREVER);
    char buf[64];
    int n = tcp_recv(s, buf, sizeof buf, 3 * NET_HZ);   /* 0 = peer closed */
}
tcp_close(s);

/* server (non-blocking style, see net/services.c) */
int l = tcp_socket();
tcp_listen(l, 8080);
int c = tcp_accept(l, TCP_NOWAIT);                      /* TCP_AGAIN if none yet */
```

Timeouts are in ticks (100/s). `TCP_NOWAIT` never blocks, `TCP_FOREVER` waits
indefinitely. Errors are negative: `TCP_ERR, TCP_AGAIN, TCP_TIMEOUT, TCP_RESET,
TCP_REFUSED`.

## UDP, DHCP and DNS

```c
#include "udp.h"
int s = udp_socket();
udp_bind(s, 5000);                                         /* optional: sendto auto-binds */
udp_sendto(s, IP4(10,0,2,2), 5000, "hi", 2);
n = udp_recvfrom(s, buf, sizeof buf, &src_ip, &src_port, 3 * NET_HZ);

#include "dns.h"
uint32_t ip;
if (dns_resolve("example.com", &ip, 6 * NET_HZ) == DNS_OK) tcp_connect(s, ip, 80, ...);
```

**DHCP** (`dhcp.c`) runs as a non-blocking state machine inside the network task:
`INIT -> SELECTING -> REQUESTING -> BOUND -> RENEWING`. It broadcasts DISCOVER/REQUEST
from 0.0.0.0 with the BROADCAST flag (we cannot receive unicast to an address we do not own
yet), renews by unicast at T1 (50% of the lease), falls back to broadcast at T2 (87.5%), and drops the
address if the lease expires. If no server answers after 4 attempts (~15 s) it installs a static
fallback (`10.0.2.15`, DNS `10.0.2.3`) and retries in the background every minute.
`ifconfig <ip> <mask> <gw>` switches DHCP off (state `STATIC`); `dhcp renew` turns it back on.

**DNS** (`dns.c`) is a blocking stub resolver: one recursive A query to `netif.dns`, 3 tries, TTL cache
(max 5 min, 8 entries), compression-pointer parsing with bounds checks, and it ignores replies with the
wrong transaction id or source. Never call it from the network task (it waits for that task to deliver the reply).

## Web server (`httpd.c`) and the filesystem it needs

```
mkdir www
fetch 10.0.2.2 8000 /index.html /www/index.html     # from `python3 -m http.server 8000` on your host
curl http://127.0.0.1:8080/                         # (run-user forwards host 8080 -> guest 80)
```

- Serves files from the OFS directory `/www` (change with `httpd root /other`). `GET` and `HEAD` only (405 otherwise);
  `Content-Type` from the extension; `/dir` redirects to `/dir/`; a directory without `index.html` gets a listing;
  `/_status` returns plain-text statistics; with no `/www` the built-in welcome page is served.
- Files are **streamed from disk in 1 KB chunks**, so size is limited by OFS, not RAM. Slow clients cannot block
  others (it only copies what the TCP send buffer has room for). 6 connections at once; the rest wait in the backlog.
- Hardening: header size limit (431), target length limit (414), `.`/`..` segments, backslashes, control
  characters and bad `%` escapes rejected with 400 *before* any filesystem lookup, 30 s idle timeout.

OFS changes this needed (the old API still works):

| Change | Why |
|---|---|
| `ofs_lookup/stat/readdir/inode_read/put` (absolute paths) | The old API is relative to the shell's `cd` global; a server must not move your working directory. |
| Files can span many sectors (stored contiguously, up to 2 MB) | The old limit was 512 bytes per file. |
| `ofs_put` writes data first, then publishes the inode | A reader never sees a half-written file. |
| IDE reads/writes run with interrupts off and have bounded waits | Shell and network tasks could interleave ATA command sequences and corrupt each other. |
| `ofs_read_file` clamps its terminator | It wrote `buffer[size]`: a 1-byte stack overflow for 512-byte files and a huge one for big files (`cat bigfile`). |

OFS limits that remain: **32 inodes in total**, no delete/rename, no free-space reuse (blocks are bump-allocated and never
reclaimed), and `fetch` buffers the whole download in RAM (max 256 KB).

## Design notes (things worth knowing)

**Concurrency.** The PIT preempts tasks at 100 Hz, so the shell task and the
network task can interleave anywhere. Every public entry point takes
`net_lock()` (save EFLAGS + `cli`). Blocking calls release it and `hlt` until
the next interrupt. Never call `net_wait()`/`task_yield()` while holding it.

**Receive path is polled.** `network_task` calls `net_poll()` in a loop; there is
no e1000 interrupt handler yet. The returned RX buffer stays valid until the next
`e1000_receive_packet()` because the driver hands descriptors back lazily.

**Addresses.** Public API uses host-order `uint32_t` (`IP4(10,0,2,15)`). Packet
structs hold wire-order bytes.

**TCP in one paragraph.** `tcp_input()` verifies the checksum, finds the
connection by 4-tuple (or a listener), then `tcp_segment()` follows RFC 793:
acceptability check, RST, SYN, ACK processing (frees acked send-buffer bytes,
advances the closing states), payload into the receive ring, FIN handling, then
`tcp_output()` sends whatever the peer's window allows. A single retransmit timer
per connection resends from the oldest unacked byte (go-back-N) with exponential
backoff (1 s -> 32 s, 8 tries, then abort). A 1-byte probe is sent into a closed
window.

## Bugs found and fixed in the existing code while building this

1. **Kernel heap was 4 KB total and never grew.** The e1000 driver wants ~70 KB, so
   most RX buffers silently ended up at physical address 0 (and tasks' 4 KB stacks
   too). Heap is now 1 MB of contiguous frames (`pmm_alloc_contiguous`) with free-block
   coalescing, and driver init fails loudly if allocation fails.
2. **`e1000_send_packet` didn't copy or wait.** The caller's buffer was handed to DMA
   and could be reused/freed before the NIC read it. Now copies into driver-owned
   buffers and waits (bounded) for a free descriptor.
3. **Linker put `.note.gnu.build-id` first**, pushing the Multiboot header past the 8 KB
   search window with some toolchains (`qemu -kernel` refuses to boot). Added
   `--build-id=none`.
4. **No header dependency tracking in the Makefile**: editing a struct in a header left
   stale objects that silently corrupted memory. Added `-MMD -MP`.

## Known limitations / next steps (roughly in order of payoff)

- **Out-of-order segments are dropped** (sender retransmits). Add a reassembly queue / SACK.
- **No congestion control** (slow start, fast retransmit/recovery). Fine on a LAN; wrong on the internet.
- **Fixed RTO**; add RTT estimation (RFC 6298).
- **OFS**: see above (32 inodes, no delete, no space reuse). Replacing it with a bitmap allocator + more inodes is the next filesystem step.
- **httpd**: no keep-alive, no `Range`, no `If-Modified-Since`/ETag (OFS has no timestamps), no upload (`PUT`).
- **UDP**: no ICMP port-unreachable for closed ports (datagrams are silently dropped), no IP-level
  fragmentation of large datagrams.
- **DHCP**: no ARP-conflict probe before using a lease, no DECLINE/RELEASE, no INIT-REBOOT.
  **DNS**: A records only (no AAAA), no TCP fallback for truncated answers, no search domains.
- **IP fragmentation is not supported** (fragments are dropped; we always set DF).
- **e1000 interrupts** instead of polling would cut latency (RX is currently serviced once per
  scheduler slice) and CPU use.
- Delayed ACKs, Nagle, window scaling, keepalives, `shutdown()` half-close.
- Two-instance mode (`make run` / `run2`, `-netdev socket`) has no DHCP server, so each instance falls
  back to `10.0.2.15` / `10.0.2.16` (by MAC) after ~15 s. Untested; the single-instance QEMU path is what the suite covers.
