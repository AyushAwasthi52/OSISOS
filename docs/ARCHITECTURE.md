# Architecture

OSISOS is a monolithic, single-address-space, uniprocessor kernel for 32-bit x86. Everything runs in ring 0
(there is a small ring-3 demo). Code lives in `kernel/`; this document explains how the pieces fit together.

## 1. Boot sequence

`arch/i386/boot.S` carries the Multiboot header (so GRUB or `qemu -kernel` can load it at 1 MiB), sets up a
16 KiB stack and jumps to `kernel_main()` (`core/kernel.c`), which initialises, in order:

| # | Step | Notes |
|---|---|---|
| 1 | `init_gdt` / `init_idt` | flat segments + TSS; **all 32 CPU exceptions** -> `exception_handler`, IRQs, `int 0x80` syscalls |
| 2 | PIC remap, `init_timer(100)` | PIT at 100 Hz: the scheduler tick and the time base of the whole network stack |
| 3 | `init_pmm` | bitmap page-frame allocator; first 4 MB reserved for the kernel |
| 4 | `init_paging` | identity-maps 32 MB; then `paging_guard_null_page()` unmaps page 0 |
| 5 | `init_kheap` | 1 MB of physically contiguous frames, first-fit with coalescing |
| 6 | `init_tasking` | the boot thread becomes PID 1 |
| 7 | `init_ide` + `init_ofs` | if a disk exists: mount, **upgrade v1**, or format a blank disk |
| 8 | `init_pci` -> `init_e1000` -> `net_init` | stack tables, UDP/TCP, services, DHCP start |
| 9 | `create_task(network_task)` | PID 2: the network task |
| 10 | `sti`, `shell_main()` | login prompt; the boot thread now runs the console shell |

## 2. Memory

```
0x00000000  page 0 UNMAPPED (NULL guard: dereferencing NULL page-faults)
0x000B8000  VGA text buffer (80x25)
0x00100000  kernel image: .text .rodata .data .bss  (~340 KB; all network tables are static in .bss)
0x00400000  first physical frame handed out by the PMM
            1 MB kernel heap (contiguous), task stacks (4 KB each, from the heap), e1000 DMA buffers
0x02000000  end of the 32 MB identity map (virtual == physical everywhere)
0xFEBxxxxx  e1000 MMIO (mapped explicitly)
```

Design choices worth knowing: **no `malloc` on hot paths** (sockets, packet buffers and tables are static, so the
network stack cannot fragment the heap or run out of memory under load); DMA needs no translation because memory is
identity-mapped; the NIC receives into driver-owned buffers and transmits from driver-owned copies.

## 3. Scheduling and concurrency

* The PIT interrupt calls `task_yield()`: **preemptive round-robin**, 10 ms slices. There are two tasks:
  **PID 1** (console shell; also executes remote-console commands) and **PID 2** (`network_task`).
* `network_task` polls: `net_poll()` (drain the NIC ring, loopback queue, ARP/TCP timers), then
  `net_services_poll()` (DHCP, UDP echo, TCP echo, httpd, rcon).
* Uniprocessor mutual exclusion is **"disable interrupts"**: `net_lock()` (save EFLAGS + `cli`) around every public
  network entry point, the same inside the IDE driver (one sector transfer at a time) and `klog`.
  *Rule: never block (`net_wait()`, `task_yield()`) while holding one.* Blocking API calls release it and
  `sti; hlt` until the next tick.
* There is no `fork`/`exec`; "processes" are kernel tasks. User mode exists as a one-way demo (`jump_usermode`).

## 4. The network task and the layering

```
 shell cmds / rcon      net/netcmd.c  net/rcon.c        (PID 1 executes commands)
 services               net/services.c  net/httpd.c     (PID 2, non-blocking)
 DHCP | DNS | TCP | UDP net/dhcp.c dns.c tcp.c udp.c
 IPv4 + ICMP + ARP      net/net.c
 e1000 driver           drivers/e1000.c
```

See [NETWORKING.md](NETWORKING.md) for a walk through each layer.

## 5. Failure handling

Any CPU exception reaches `core/panic.c`: the event is logged, a panic screen is drawn directly into VGA memory
(cause in plain English, registers, stack words, last log lines) and the machine reboots after 15 s (R = now,
H = halt). `kpanic("msg")` does the same for software-detected fatal errors. NULL dereferences fault because page 0
is unmapped. See [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md#7-debugging).

## 6. Persistence

Disk: ATA PIO on the primary channel. Filesystem: OFS v2 ([FILESYSTEM.md](FILESYSTEM.md)). Persistent state is
only what is on disk: files, `/etc/shadow` (credentials), `/etc/rcon` (remote console enabled flag). Everything
else (leases, ARP cache, sockets, kernel log) is rebuilt at boot.

## 7. Source map (approximate lines)

| Dir | Files |
|---|---|
| `arch/i386` | boot.S, exceptions.S (87), gdt, idt, pic, switch.S, usermode.S, paging_flush.S, io.h |
| `core` | kernel.c, shell.c, extras.c, panic.c, auth.c, **sha256.c, x25519.c, chacha20poly1305.c, noise.c, rng.c** (crypto), klog.c, task.c, editor.c, syscall.c |
| `drivers` | e1000.c (164), ide.c (147), keyboard.c, pci.c, rtc.c, timer.c |
| `mm` | pmm.c, paging.c, kheap.c |
| `fs` | ofs.c (562), fscmd.c (87) |
| `net` | tcp.c (942), httpd.c (544), net.c (508), netcmd.c (585), rcon.c (342), dhcp.c (297), dns.c (183), udp.c (160), services.c (98), netstr.c (53) |
