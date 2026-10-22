# OSISOS

**A lightweight server operating system, written from scratch for 32-bit x86.**

OSISOS boots on bare metal (or in QEMU), runs its own preemptive kernel, drives a real network card, speaks
IPv4/TCP/UDP/DHCP/DNS/HTTP, serves web pages from its own filesystem, and can be administered remotely. About
9,000 lines of C and assembly, no libc, no third-party code.

```
 login: admin                      sysinfo                        $ curl http://10.0.2.15/
 password: ******                     .---------.     admin@osisos   <html>... served by the kernel
 admin@osisos> selftest             ===|  O S    |===  OS:      OSISOS (i386, from scratch)
  [ OK ] ICMP echo to 127.0.0.1     ===|   I S   |===  Memory:  4 / 31 MB
  [ OK ] TCP echo, 3000 bytes       ===|    O S  |===  Network: 10.0.2.15 via BOUND
  [ OK ] HTTP server (GET /_status)    '---------'     Web:     120 requests, 640 KB served
  9/9 checks passed. ALL GOOD.
```

## Features

| Area | What you get |
|---|---|
| **Kernel** | GDT/IDT, PIC/PIT, paging (32 MB identity map, NULL-page guard), page-frame allocator, 1 MB kernel heap, preemptive round-robin scheduler, ring-3 demo, full CPU-exception handling with a **kernel panic screen** and auto-reboot |
| **Drivers** | VGA text console, PS/2 keyboard, PIT, CMOS RTC, ATA PIO disk (locked + bounded), PCI, **Intel e1000** NIC |
| **Network** | Ethernet, **ARP**, **IPv4** (+loopback), **ICMP**, **UDP**, **TCP** (full state machine, retransmission, flow control), **DHCP** client with lease renewal, **DNS** resolver with cache |
| **Servers** | **HTTP/1.1 static file server** (streams from disk), TCP/UDP echo, **remote admin console** (opt-in, password-protected) |
| **Storage** | **OFS v2**: 128 inodes, contiguous extents, delete/rename, space reuse, automatic v1 -> v2 migration |
| **Security** | PBKDF2-HMAC-SHA256 password hashes, login throttling, audit log, `/etc` never served, safe-by-default remote access |
| **Operations** | `dmesg` kernel log, `sysinfo`, `selftest`, `netstat`, packet-loss injection, crash screen |
| **Fun** | `snake`, `matrix`, `sl`, `fortune`, `cowsay`, easter eggs |
| **Quality** | An end-to-end test suite (QEMU + a Python harness) with 130+ checks |

## Quick start

Prerequisites (Ubuntu/Debian): `build-essential gcc-multilib make qemu-system-x86` (for an ISO also
`grub-pc-bin xorriso mtools`; for the test suite `python3 tcpdump`).

```sh
cd kernel
make                 # builds osisos.bin
make run-direct      # boots it in QEMU (no GRUB needed); host :8080 -> guest HTTP, :8007 -> echo
```

Log in with **admin / osisos** (you will be nagged to run `passwd`), then try:

```
sysinfo        selftest        ifconfig        dmesg
ping 127.0.0.1                 mkdir www       fetch 10.0.2.2 8000 /index.html /www/index.html
```

From your host, with `python3 -m http.server 8000` running, that `fetch` publishes a page that
`curl http://127.0.0.1:8080/` then reads **from the OSISOS kernel**.

## Documentation

| Document | Read it to... |
|---|---|
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md) | use the system: every command, the services, the remote console, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | understand how it works: boot, memory, scheduling, locking, source map |
| [docs/NETWORKING.md](docs/NETWORKING.md) | the network stack layer by layer (ARP ... TCP, DHCP, DNS, HTTP) |
| [docs/FILESYSTEM.md](docs/FILESYSTEM.md) | OFS v2: on-disk format, allocator, API, migration |
| [docs/SECURITY.md](docs/SECURITY.md) | threat model, protections, and the honest list of weaknesses |
| [docs/DEVELOPER_GUIDE.md](docs/DEVELOPER_GUIDE.md) | extend it: add commands/services, APIs, conventions, debugging |
| [docs/TESTING.md](docs/TESTING.md) | run and extend the test suite |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is done, known limits, and what to build next |

> The original plan (a Linux-hosted wrapper) is preserved in [docs/legacy/](docs/legacy/). The code in this
> repository is a standalone kernel and does not use Linux.

## Project layout

```
kernel/
  arch/i386/   boot, GDT/IDT, PIC, context switch, exception stubs, ring-3 entry
  core/        kernel_main, scheduler, shell, panic screen, klog, auth + SHA-256, extras (sysinfo, games)
  drivers/     keyboard, timer, RTC, ATA/IDE, PCI, e1000
  mm/          physical frames, paging, kernel heap
  fs/          OFS v2 + the rm/mv/df commands
  net/         net (Eth/ARP/IP/ICMP), udp, tcp, dhcp, dns, httpd, rcon, services, shell commands
docs/          this documentation
tools/         nettest.py - the end-to-end test harness
```

## Status

Educational / hobby-grade. It is tested in QEMU only. See [SECURITY.md](docs/SECURITY.md) before exposing it to
any network you do not control.
