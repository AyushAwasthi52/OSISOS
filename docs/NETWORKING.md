# OSISOS TCP/IP stack

A from-scratch IPv4 + TCP stack for the OSISOS kernel, sitting on top of the
existing e1000 driver. Everything lives in `kernel/net/`.

```
 shell commands   ifconfig arp ping netstat netdrop nc http          net/netcmd.c
 built-in servers echo (port 7)   HTTP (port 80)                     net/services.c
 TCP              state machine, retransmit, flow control, sockets   net/tcp.c  tcp.h
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
Log in (`admin` / `osisos`), then:

| Command | What it does |
|---|---|
| `ifconfig` | MAC, IP, counters. `ifconfig <ip> <mask> <gw>` to change. |
| `ping 10.0.2.2` | ICMP echo x4 (exercises ARP + IPv4 + ICMP). |
| `arp` | ARP cache. |
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
- **No UDP, DHCP or DNS.** UDP is ~100 lines on top of `net_ip_send`; DHCP + DNS then give you
  `ping example.com` and real browsing. Static IP for now.
- **IP fragmentation is not supported** (fragments are dropped; we always set DF).
- **e1000 interrupts** instead of polling would cut latency (RX is currently serviced once per
  scheduler slice) and CPU use.
- Delayed ACKs, Nagle, window scaling, keepalives, `shutdown()` half-close.
- Two-instance mode (`make run` / `run2`, `-netdev socket`) assigns `10.0.2.15` / `10.0.2.16`
  by MAC; the single-instance QEMU path above is what the test-suite covers.
