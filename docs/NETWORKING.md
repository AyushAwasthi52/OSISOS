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

OFS changes the web server needed (the old API still works; see the v2 notes below for what came after):

| Change | Why |
|---|---|
| `ofs_lookup/stat/readdir/inode_read/put` (absolute paths) | The old API is relative to the shell's `cd` global; a server must not move your working directory. |
| Files can span many sectors (stored contiguously, up to 2 MB) | The old limit was 512 bytes per file. |
| `ofs_put` writes data first, then publishes the inode | A reader never sees a half-written file. |
| IDE reads/writes run with interrupts off and have bounded waits | Shell and network tasks could interleave ATA command sequences and corrupt each other. |
| `ofs_read_file` clamps its terminator | It wrote `buffer[size]`: a 1-byte stack overflow for 512-byte files and a huge one for big files (`cat bigfile`). |

`fetch` buffers the whole download in RAM (max 256 KB).

## Filesystem v2, loopback, self-test and the extras

**OFS v2** (`fs/ofs.c`): 128 inodes (inode table in blocks 1-8, data from block 9). Every file is one contiguous
extent; free space is *derived from the inode table* (first-fit over the gaps), so there is no bitmap that can get out of
sync, deleted files' space is reused immediately, and shrinking a file frees its tail. New shell commands:
`rm`, `rmdir`, `mv` (also moves into a directory), `df`. **v1 disks are upgraded automatically at boot**
(data blocks are relocated, nothing is wiped; see `migrate_v1`). Limits: names up to 21 chars, files up to 2 MB, no
permissions or timestamps; a file being streamed by httpd while it is deleted just aborts that transfer.

**Loopback**: `127.0.0.x` and your own address are delivered internally, so `ping 127.0.0.1`, `nc localhost 7`,
`http localhost` work even with no network.

**`selftest`** runs the stack against itself and reports `[ OK ]` / `[FAIL]`: ICMP, UDP and a 3000-byte TCP echo
over loopback, the HTTP server, a filesystem write/read/delete, creating 100 files in the inode table, and the heap.

**`dmesg`** shows the kernel log (`klog()`; DHCP leases, HTTP requests, filesystem upgrades). Add your own with
`klog("something happened: %I", ip)`.

**Extras** (`core/extras.c`): `sysinfo`, `about`, `fortune`, `cowsay <text>`, `matrix`, `snake` (WASD, Q to quit), `sl`,
and some easter eggs. Full-screen programs run in the shell task, so the web server keeps serving while you play.

## Crash handling (`arch/i386/exceptions.S`, `core/panic.c`)

Before this, only divide-by-zero had an IDT entry: any other fault (page fault, GPF, invalid opcode ...) became a
triple fault and a silent reset with no clue why. Now **all 32 CPU exception vectors** reach `exception_handler()`, which
writes the event to the kernel log and paints a panic screen straight into VGA memory: the exception name and a
plain-English cause (e.g. *NULL pointer dereference (write, kernel mode) at 00000000*), EIP/registers/CR0/CR2/CR3, the top
stack words, uptime and the last kernel log lines. It then counts down 15 s and reboots (**R** = reboot now,
**H** = halt and keep the screen). The web server and the disk come back after the reboot.

- **NULL guard**: page 0 is unmapped after paging starts, so NULL-pointer bugs fault loudly instead of silently
  reading/writing low memory.
- **`kpanic("message")`**: call it from kernel code for unrecoverable conditions (it disables interrupts first).
- **`crash null|div0|ud|gpf|page|panic`**: deliberately fault the kernel to see/test all of this.
- Limits: a *double fault* caused by a stack overflow cannot be handled on the same stack (it triple-faults);
  fixing that needs a dedicated TSS task gate. The panic report is not persisted to disk yet.

## Authentication (`core/auth.c`, `core/sha256.c`)

Login no longer compares against a hardcoded string. Passwords are stored as **salted, iterated hashes**
(PBKDF2-HMAC-SHA256, 4096 iterations, 16-byte random salt, fresh salt on every change) in `/etc/shadow`; the
plaintext is never stored and the typed password is wiped from memory right after use. The SHA-256 / HMAC / PBKDF2
code is verified against Python's `hashlib` (including SHA-256 block-boundary lengths and the RFC 7914 vector), and
`selftest` re-checks known answers inside the OS.

- **First boot** creates `admin` / `osisos` and flags it *must change*: the shell warns on every login until you run **`passwd`**
  (asks for the current password, then the new one twice, input masked, 6-63 characters).
- **Throttling**: 3 consecutive failures lock logins for 5 s, doubling each further failure up to 5 min. While locked, the password
  is not even checked, so guessing is rate-limited. Unknown users take as long as wrong passwords (no user enumeration by timing),
  comparison is constant-time, and every success/failure goes to `dmesg` (user names are sanitised in the log).
- **`/etc` is invisible to the web server** (404, and hidden from listings) even when the document root is `/`.
- Fixed: login used to copy up to 255 typed characters into 32-byte buffers (a global buffer overflow at the login prompt).
- Limits: one account (`admin`) and no privilege levels; no password ageing; the default password is public until changed;
  the console is local, there is no remote login yet.

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
- **OFS**: contiguous extents mean a very fragmented disk can fail to fit a large file even with enough total free space; no permissions/timestamps. Subdirectory depth for `ofs_abspath` is 16.
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
