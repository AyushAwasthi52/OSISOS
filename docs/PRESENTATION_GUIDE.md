# OSISOS Capstone Presentation & Defense Guide

This guide accompanies the interactive web slide deck located at [`presentation/index.html`](../presentation/index.html). It contains the complete presentation script, slide-by-slide technical talking points, defense preparation Q&A, and live demonstration instructions for your project presentation.

---

## 1. Quick Launch Instructions

### Option A: Direct Browser Launch
Simply double-click [`presentation/index.html`](../presentation/index.html) or open it directly in Google Chrome, Firefox, Brave, or Edge:

```bash
# On Linux / Arch:
xdg-open presentation/index.html
```

### Option B: Local Web Server (Recommended)
```bash
python3 -m http.server 3000 --directory presentation
# Open: http://localhost:3000/
```

### Keyboard Shortcuts
| Key | Action |
|---|---|
| <kbd>→</kbd> / <kbd>Space</kbd> / <kbd>PageDown</kbd> / <kbd>L</kbd> | Next Slide |
| <kbd>←</kbd> / <kbd>Backspace</kbd> / <kbd>PageUp</kbd> / <kbd>H</kbd> | Previous Slide |
| <kbd>Home</kbd> / <kbd>End</kbd> | First / Last Slide |
| <kbd>F</kbd> | Toggle Fullscreen Mode |
| <kbd>O</kbd> or <kbd>Esc</kbd> | Toggle Slide Overview Thumbnail Grid |
| <kbd>N</kbd> | Toggle Speaker Notes & Script Drawer |
| <kbd>Ctrl</kbd> + <kbd>P</kbd> | Export Slide Deck to PDF (Pre-formatted 16:9 Landscape) |

---

## 2. Slide-by-Slide Script & Defense Walkthrough

### Slide 1: Title & Project Identity
* **Headline:** OSISOS — 32-bit Multitasking Operating System & Custom TCP/IP Stack
* **Speaker Script:**
  > "Good morning/afternoon, professors and members of the evaluation panel. Today, we present **OSISOS** — an independent, freestanding operating system kernel and complete TCP/IP network protocol stack built entirely from bare metal in x86 Assembly and freestanding C.
  > 
  > Unlike educational projects that run on top of Linux userland, OSISOS runs on bare physical or virtual hardware. It controls the CPU, manages memory pages, coordinates preemptive multitasking, talks directly to hard drives and network cards, and implements a full networking suite with zero third-party libraries."

---

### Slide 2: Engineering Vision & System Constraints
* **Headline:** Engineering Vision & Architectural Constraints
* **Key Technical Points:**
  - Strict **~32 MB RAM** target to enforce lean data structures and eliminate memory waste.
  - **1 vCPU (~300 MHz class)** for single-core preemptive context switching.
  - **Zero external dependencies**: Freestanding compilation (`-ffreestanding`, `-nostdlib`, `-fno-pie`).
  - **Dual-Environment Architecture**: The repository keeps a strict barrier between `host/` (prototyping only) and `kernel/` (bare metal Ring 0).
* **Speaker Script:**
  > "Why did we set a 32 MB RAM constraint? Because modern systems hide inefficiencies behind gigabytes of RAM. Operating in 32 MB forced us to engineer tight bitmapped memory allocators, bounded ring buffers, and defensive resource reclamation. Everything you see today boots in less than 200 milliseconds."

---

### Slide 3: The Boot Journey: Bare Metal to Protected Mode
* **Headline:** The Boot Journey: Bare Metal to C
* **Key Technical Points:**
  - **Multiboot1 Standard:** Handshake via header in `boot.S` containing magic `0x1BADB002`.
  - **GRUB Bootloader:** Switches CPU to 32-bit Protected Mode, disables interrupts, loads kernel ELF at 1MB (0x100000).
  - **Kernel Stack:** `boot.S` allocates 16KB stack before jumping to `kernel_main()`.
  - **GDT (Global Descriptor Table):** Configures 5 flat 4GB segment descriptors: Null, Kernel Code (0x08), Kernel Data (0x10), User Code (0x18), User Data (0x20), and TSS (0x28).
  - **PIC Remapping:** Remaps Master 8259 PIC to vectors 0x20-0x27 and Slave to 0x28-0x2F to prevent hardware IRQs from colliding with CPU exception vectors 0-31.
* **Speaker Script:**
  > "When the machine powers on, BIOS loads GRUB, which inspects our multiboot header. GRUB handles the transition to 32-bit Protected Mode and jumps to our assembly entry point `_start`. Here, we establish our initial stack and call `kernel_main()`.
  > 
  > Our first critical task in C is initializing the GDT and remapping the legacy 8259 PIC. Without PIC remapping, an ordinary keyboard press or timer tick would fire vector 0x08, which the CPU treats as a Double Fault exception!"

---

### Slide 4: Memory Management Architecture
* **Headline:** PMM, Two-Level Paging, NULL-Pointer Guard & KHeap
* **Key Technical Points:**
  - **Physical Memory Manager (PMM):** Manages 8,192 physical 4KB frames using an 8KB bitmap.
  - **Two-Level Paging:** Page Directory containing 1024 Page Table pointers, loaded into `CR3` and activated via bit 31 of `CR0`.
  - **NULL-Page Guard:** Virtual address range `0x00000000 - 0x00000FFF` (Page 0) is deliberately left unmapped. Dereferencing a NULL pointer causes an immediate hardware Page Fault (#PF) rather than silent data corruption.
  - **Kernel Heap (`kmalloc` / `kfree`):** Boundary-tag allocator with adjacent block coalescing.
* **Speaker Script:**
  > "Memory management follows a 3-tier hierarchy. At the bottom, our Physical Memory Manager uses an 8KB bitmap to manage all 32MB of RAM in 4KB frames. Above that, we enable virtual memory through two-level x86 paging.
  > 
  > A key defensive feature is our NULL-page guard: we intentionally leave page 0 unmapped. If any kernel code dereferences `NULL`, the CPU immediately triggers a hardware Page Fault rather than corrupting memory. Finally, `kmalloc` and `kfree` provide dynamic heap allocation with automatic block coalescing."

---

### Slide 5: Preemptive Multitasking & Context Switching
* **Headline:** Preemptive Multitasking & Context Switching (PID 1 & PID 2)
* **Key Technical Points:**
  - Driven by the **100 Hz Programmable Interval Timer (PIT)** via IRQ 0.
  - Context switch preserves entire CPU register frame: Hardware pushes EIP, CS, EFLAGS; Assembly handler executes `pushal` (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI).
  - Stack pointer `ESP` is saved into old task's Process Control Block (PCB) and restored from the next task's PCB before executing `iret`.
  - **PID 1:** Main interactive kernel shell.
  - **PID 2:** Background network polling task draining DMA rings and handling TCP retransmissions.
* **Speaker Script:**
  > "Every 10 milliseconds, our Programmable Interval Timer fires IRQ 0. Our assembly interrupt wrapper saves all registers onto the current task's stack, switches the ESP pointer to the next task's stack in round-robin fashion, and issues `iret`.
  > 
  > This allows PID 1 (our interactive shell) and PID 2 (our background network daemon) to run truly concurrently. When you type in the shell, PID 2 is simultaneously servicing incoming TCP segments and web requests in the background."

---

### Slide 6: Hardware Drivers & Bus Enumeration
* **Headline:** Device Drivers, PCI Discovery & Intel E1000 NIC
* **Key Technical Points:**
  - **VGA Text Driver:** 80x25 memory-mapped video buffer at `0xB8000`, cursor control via ports `0x3D4/0x3D5`.
  - **PS/2 Keyboard:** Scancode set 1 processing with modifier key state tracking.
  - **Real-Time Clock (RTC):** Reads CMOS registers 0x70/0x71 with BCD to binary decoding.
  - **PCI Enumeration:** Scans 256 buses × 32 slots × 8 functions using I/O ports `0xCF8` and `0xCFC`.
  - **Intel E1000 Gigabit NIC:** Discovers Intel 8254x chipsets (82540EM, 82545EM, 82543GC), enables bus mastering, and sets up 64-descriptor circular DMA rings.
* **Speaker Script:**
  > "OSISOS communicates directly with bare metal hardware. Our PCI driver scans the PCI bus to discover installed cards. When it detects an Intel E1000 Gigabit Ethernet card, it reads its Base Address Register (BAR0) for Memory Mapped I/O and configures circular DMA rings.
  > 
  > Because the NIC uses Direct Memory Access, packet reception and transmission occur directly in RAM without CPU intervention."

---

### Slide 7: Storage Engine: OFS v2 Inode Filesystem
* **Headline:** Storage Engine: OFS v2 Inode Filesystem & ATA IDE PIO
* **Key Technical Points:**
  - **ATA IDE Driver:** 28-bit LBA PIO sector reads and writes via primary ports `0x1F0-0x1F7`.
  - **OFS v2 Structure:**
    - Sector 0: Superblock with magic `0x4F465332` ("OFS2").
    - Sectors 1-16: Inode Table containing 128 inodes (64 bytes each).
    - Data Area: 1024-byte clusters with contiguous allocation.
  - **Capabilities:** Hierarchical paths (`/www/index.html`), disk usage statistics (`df`), and persistence across cold reboots.
* **Speaker Script:**
  > "For permanent storage, we developed OFS v2 — the OSISOS File System. Sitting on top of our ATA IDE hard disk driver, OFS v2 features a superblock, a 128-entry inode table, and 1024-byte data blocks.
  > 
  > It supports multi-level directory paths, contiguous multi-block files, and persistent file creation that survives reboots. Files created via our shell editor are immediately saved to disk."

---

### Slide 8: Security & Fault Tolerance
* **Headline:** Cryptographic Authentication & CPU Panic Screen
* **Key Technical Points:**
  - **SHA-256 & PBKDF2:** Written from mathematical first principles in freestanding C.
  - **Shadow File:** `/etc/shadow` stores `username:salt:hash` using 10,000 PBKDF2 iterations.
  - **Lockout Mechanism:** Exponential delay (2s, 4s, 8s...) on repeated failed password entries.
  - **Kernel Panic Screen (BSOD):** Custom exception handler capturing vectors 0-31 with full register dumps (EIP, CR0, CR2, CR3, EFLAGS) and stack backtraces.
* **Speaker Script:**
  > "Security in OSISOS is not an afterthought. We implemented the SHA-256 algorithm and PBKDF2 key derivation completely from scratch. User passwords are salted with an 8-byte random salt and iterated 10,000 times before being stored in `/etc/shadow`.
  > 
  > If a kernel panic occurs, our CPU exception vectors intercept the fault and render a full diagnostic register dump on screen, showing the exact instruction pointer and fault address in CR2."

---

### Slide 9: Custom TCP/IP Stack: Layers 2 and 3
* **Headline:** Custom TCP/IP Stack: Ethernet, ARP, IPv4 & ICMP
* **Key Technical Points:**
  - **Ethernet II:** 14-byte frame header parsing, source/destination MAC validation, EtherType routing.
  - **ARP Engine:** 16-entry dynamic cache, broadcast requests, pending packet transmit queues.
  - **IPv4 Protocol:** Header validation, 16-bit one's complement internet checksum computation, TTL decrement, protocol multiplexing.
  - **ICMP Echo:** Instant Ping replies and round-trip time latency calculations.
* **Speaker Script:**
  > "The crown jewel of OSISOS is its networking stack. At Layer 2, our Ethernet driver parses incoming frames and filters by MAC address.
  > 
  > Our ARP engine maintains a dynamic address cache, queuing outgoing packets while resolving IP addresses to physical MACs. At Layer 3, our IPv4 engine handles checksums, loopback routing (127.0.0.1), and ICMP ping echo replies."

---

### Slide 10: Dynamic Configuration: DHCP & DNS
* **Headline:** Dynamic Configuration: DHCP Client & DNS Resolver
* **Key Technical Points:**
  - **DHCP Client:** Autonomous 4-stage state machine (DISCOVER -> OFFER -> REQUEST -> ACK) on UDP ports 68/67.
  - Acquires IP address (10.0.2.15), subnet mask (255.255.255.0), default gateway (10.0.2.2), and DNS server (10.0.2.3).
  - **DNS Resolver:** RFC 1035 UDP client capable of parsing compressed label pointers (`0xC0xx`) and caching A-records.
* **Speaker Script:**
  > "OSISOS requires zero manual network configuration. Upon boot, our DHCP state machine broadcasts a DISCOVER packet and completes the 4-way handshake, binding to an IP address and configuring the default gateway.
  > 
  > Our RFC 1035 DNS resolver decodes DNS name compression pointers and caches results, allowing commands like `ping example.com` or `http example.com` to work seamlessly."

---

### Slide 11: TCP Implementation: The Crown Jewel
* **Headline:** TCP Engine: Full RFC 793 State Machine
* **Key Technical Points:**
  - 950+ lines of freestanding C in `kernel/net/tcp.c`.
  - **State Machine:** CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1/2, CLOSE_WAIT, TIME_WAIT, LAST_ACK.
  - **Sliding Window:** Dynamic flow control tracking advertised window sizes to prevent peer buffer overrun.
  - **Retransmission:** Segment retransmit timers with exponential backoff.
  - **Socket API:** Clean non-blocking interface (`tcp_socket`, `tcp_connect`, `tcp_send`, `tcp_recv`).
* **Speaker Script:**
  > "TCP is notoriously difficult to implement in an operating system kernel. OSISOS implements the full RFC 793 state machine.
  > 
  > It tracks sequence and acknowledgment numbers, enforces sliding-window flow control so fast senders don't overwhelm slow receivers, and manages retransmission timers with exponential backoff. It provides a non-blocking socket API that powers both client and server applications."

---

### Slide 12: Network Services & Wi-Fi LAN Reachability
* **Headline:** Built-in Network Services & Local Wi-Fi Reachability
* **Key Technical Points:**
  - **Port 80 (HTTP 1.1 Web Server):** `httpd.c` streams HTML and image files straight from the OFS filesystem (`/www` directory).
  - **Port 7 (Echo Server):** TCP and UDP echo service.
  - **Port 9999 (Live Console Message Service):** Custom TCP listener (`msg_serve`) that accepts remote text and prints it live on the OSISOS console monitor!
  - **Wi-Fi Reachability:** QEMU host forwards `8080->80`, `8007->7`, and `9999->9999` bound to `0.0.0.0`.
* **Speaker Script:**
  > "OSISOS acts as a full-fledged network server. It hosts an HTTP/1.1 web server on port 80 serving files from our disk filesystem, an echo server on port 7, and a live console messaging server on port 9999.
  > 
  > Any friend or device connected to the same Wi-Fi network can run `nc <host-ip> 9999` and send text that appears live on the OSISOS screen in real time!"

---

### Slide 13: Interactive Shell, Editor & Games
* **Headline:** Userland Environment: Shell, Editor & Utilities
* **Key Technical Points:**
  - 25+ built-in commands covering system diagnostics, networking, and disk management.
  - In-kernel full-screen text editor with cursor controls and disk save capabilities.
  - Interactive terminal games: Real-time Snake, Matrix digital rain, Cowsay, Fortune, and Steam Locomotive (`sl`).
* **Speaker Script:**
  > "The user experience in OSISOS is fast and responsive. Users have access to over 25 built-in shell utilities, including `ifconfig`, `netstat`, `df`, and `dmesg`.
  > 
  > We even built a full-screen interactive text editor directly into the kernel for editing files on disk, alongside arcade games like Snake and Matrix rain to demonstrate real-time timer handling."

---

### Slide 14: System Architecture Deep Dive
* **Headline:** Full Stack Architectural Blueprint
* **Key Technical Points:**
  - Visual breakdown of all five architectural layers: Hardware -> Kernel Core & Memory -> Storage & Drivers -> Network Stack -> Shell & Userland.
* **Speaker Script:**
  > "This diagram illustrates the holistic architecture of OSISOS. Notice the clean separation of concerns: hardware drivers isolate physical I/O ports; our memory and core kernel manage CPU state and page tables; the filesystem and networking engines handle storage and transport; and the user interface interacts with clean system call abstractions."

---

### Slide 15: Live Interactive Terminal Demo
* **Headline:** Interactive OSISOS Terminal Demo
* **Presenter Action:** Click the quick chips (`sysinfo`, `ifconfig`, `httpd`, `df`, `dmesg`, `matrix`) to demonstrate the look, feel, and responses of the OSISOS kernel right inside the slide deck!
* **Speaker Script:**
  > "Right here on screen, we have an interactive simulation of the OSISOS shell. When we type `sysinfo`, we see our 32MB RAM usage and active PIDs. Running `ifconfig` reveals our leased DHCP IP and MAC address, and `httpd` shows our active web server metrics."

---

### Slide 16: Engineering Challenges & War Stories
* **Headline:** Engineering Challenges & Debugging War Stories
* **Key Technical Points:**
  - **Hurdle 1 (VGA Scrolling):** Timer ISR was writing heartbeat to Row 2 while scrolling, causing text corruption -> Solved by decoupling console writes from interrupts.
  - **Hurdle 2 (VirtualBox PCI):** VirtualBox used different device IDs than QEMU (`0x100F` vs `0x100E`) -> Solved by broad Intel 8254x PCI matching.
  - **Hurdle 3 (Memory Leaks under Traffic):** Packet heap allocations overwhelmed 32MB RAM -> Solved with bounded RX rings and socket timeouts.
  - **Hurdle 4 (ACPI Power-Off):** VirtualBox shutdown hung CPU -> Solved with multi-port ACPI write to `0x4004`.
* **Speaker Script:**
  > "Building an operating system without Linux tools or GDB requires tenacity. We solved race conditions where the timer interrupt collided with VGA screen scrolling. We resolved hypervisor discrepancies where VirtualBox used different PCI device IDs and ACPI shutdown ports than QEMU. Every bug required inspecting CPU registers and memory addresses directly."

---

### Slide 17: Project Verification & Metrics
* **Headline:** Verification Metrics & Automated Test Suite
* **Key Technical Points:**
  - Over **8,780 lines of code** across 34 kernel modules.
  - Automated Python test suite (`tools/nettest.py`) running headless QEMU to test DHCP, ping latency, TCP multi-segment transfers, and HTTP downloads.
* **Speaker Script:**
  > "To verify correctness, we wrote an automated Python test harness that boots the kernel headlessly in QEMU and exercises every network layer. The project spans nearly 9,000 lines of freestanding C and Assembly, passing all end-to-end integration tests."

---

### Slide 18: Conclusion & Future Roadmap
* **Headline:** Conclusion & Future Expansion Roadmap
* **Key Technical Points:**
  - Summarize key learnings: Computer architecture, assembly, operating system design, and network protocols.
  - Future goals: 64-bit Long Mode, SMP multi-core scheduler, ELF binary loader.
* **Speaker Script:**
  > "In conclusion, OSISOS demonstrates that complex, production-style operating system concepts — from virtual memory and preemptive scheduling to an RFC 793 TCP stack and persistent filesystem — can be designed and built from scratch.
  > 
  > In the future, we plan to transition to 64-bit Long Mode and introduce multicore SMP scheduling. Thank you for your time, and we now welcome any questions from the panel!"

---

## 3. Defense Preparation: Top 12 College Viva Questions & Answers

### Q1: Why did you choose 32-bit Protected Mode instead of jumping directly into 64-bit Long Mode?
**Answer:**
> "32-bit Protected Mode (i686) is the ideal architectural foundation for understanding fundamental x86 concepts. It requires building the GDT, IDT, PIC remapping, hardware task state segment (TSS), and 2-level paging from scratch. 64-bit Long Mode requires 4-level paging and often skips many foundational CPU mechanics that are educational and crucial to master first. Our architecture is designed so the physical memory manager and drivers can transition to 64-bit in the future."

---

### Q2: Why is PIC remapping mandatory on x86 architectures?
**Answer:**
> "By default, the legacy IBM PC BIOS maps Master PIC IRQs 0-7 to interrupt vectors `0x08 - 0x0F`. However, on the Intel x86 processor in Protected Mode, vectors `0x00 - 0x1F` are strictly reserved for CPU hardware exceptions (like Vector 8 = Double Fault, Vector 13 = General Protection Fault, Vector 14 = Page Fault).
> 
> If you do not remap the PIC, every time the hardware timer ticks (IRQ 0), the CPU executes Vector 8, mistaking an ordinary timer interrupt for a catastrophic Double Fault exception! We remap Master PIC to `0x20` and Slave to `0x28` so hardware IRQs start safely at vector 32."

---

### Q3: How does your NULL-pointer guard work under the hood?
**Answer:**
> "In virtual memory paging, virtual addresses are mapped to physical frames via the Page Directory and Page Tables. The first 4KB of virtual memory spans addresses `0x00000000` to `0x00000FFF`, which corresponds to entry 0 in the first Page Table.
> 
> In `paging_guard_null_page()`, we explicitly clear the present bit of Page Table Entry 0 (`page_table[0] = 0`) and flush the Translation Lookaside Buffer (TLB). Now, if any C pointer is `NULL` (0) and gets dereferenced, the CPU Memory Management Unit (MMU) halts execution and triggers Page Fault `#PF` (Exception 14) with the faulting address stored in the `CR2` register."

---

### Q4: How does context switching work without crashing the stack?
**Answer:**
> "When the 100 Hz timer interrupt fires, the CPU hardware automatically pushes `EFLAGS`, `CS`, and `EIP` onto the current task's stack. In our assembly ISR (`switch.S`), we execute `pushal`, saving all general-purpose registers (`EAX`, `ECX`, `EDX`, `EBX`, `ESP`, `EBP`, `ESI`, `EDI`).
> 
> We then save the current `%esp` register into the old task's Process Control Block (PCB) structure. Next, we read the `%esp` of the next ready task from its PCB and load it into the CPU's `%esp` register. Finally, we execute `popal` and `iret`. Because the stack now points to the new task's stack frame, `popal` restores its registers, and `iret` pops its `EIP`, seamlessly resuming execution exactly where it left off."

---

### Q5: How does the Intel E1000 Gigabit NIC receive packets via DMA?
**Answer:**
> "The Intel E1000 controller uses circular descriptor rings mapped in physical memory. We allocate a ring of 64 Receive Descriptors (each 16 bytes) and assign each descriptor a physical 2KB packet buffer allocated via our Physical Memory Manager.
> 
> We write the physical base address of this ring and its length to the E1000 MMIO registers `RDBAL` and `RDLEN`. When an Ethernet packet arrives on the wire, the E1000 hardware controller writes the packet bytes directly into our RAM buffer via Direct Memory Access (DMA) and updates the ring's Receive Head pointer (`RDH`). The CPU doesn't copy any packet bytes — it simply reads the buffer when polling `RDT` and `RDH`."

---

### Q6: What is the difference between OFS v1 and OFS v2?
**Answer:**
> "OFS v1 was a simple flat directory filesystem where file metadata and data were constrained to single sectors, limiting file sizes and lacking hierarchical directories.
> 
> OFS v2 introduced authentic Inode architecture:
> 1. A dedicated Superblock with filesystem magic `0x4F465332` and block counts.
> 2. A 128-entry Inode Table (64 bytes per inode) storing file size, timestamps, type (file vs directory), and contiguous block cluster pointers.
> 3. Support for multi-block files spanning up to several megabytes.
> 4. Hierarchical directory paths (like `/www/index.html` and `/etc/shadow`)."

---

### Q7: How does your TCP sliding window flow control work in 32MB of RAM?
**Answer:**
> "Every TCP segment header contains a 16-bit Window size field. When OSISOS receives data, it advertises how many bytes of buffer space remain in our socket receive queue (`TCP_RX_BUF_SIZE - rx_queued`).
> 
> The sender is legally prohibited by RFC 793 from transmitting more bytes than our advertised window. When our application reads data out of the socket via `tcp_recv()`, window space frees up, and OSISOS transmits an ACK packet with an updated window size, allowing the sender to resume streaming. This ensures that rapid downloads (like `fetch`) never overflow our 32MB physical memory limit."

---

### Q8: How does the DHCP client work without an operating system network daemon?
**Answer:**
> "Our DHCP client operates as a finite state machine in `kernel/net/dhcp.c`. Upon boot, it constructs a raw UDP datagram containing a DHCP DISCOVER payload with options (Requested IP, Parameter Request List for subnet, gateway, DNS). It broadcasts this packet to `255.255.255.255:67`.
> 
> Our background network task (`PID 2`) polls incoming UDP packets on port 68. When a DHCP OFFER arrives, the state machine transitions to REQUEST. Upon receiving the router's ACK, it extracts the assigned IP, netmask, gateway, and DNS, updating our kernel `netif` structure, and starts a lease timer for renewal."

---

### Q9: How do you protect passwords against offline dictionary and brute force attacks?
**Answer:**
> "We implement two layers of defense:
> 1. **Cryptographic Salt & Iterations:** We implemented SHA-256 and PBKDF2 with 10,000 hash iterations combined with an 8-byte random salt. Even if an attacker extracts `/etc/shadow`, precomputed rainbow tables are useless because of the salt, and brute-force cracking is computationally expensive due to 10,000 iterations per attempt.
> 2. **Runtime Backoff Delay:** If an attacker attempts to guess passwords interactively in the shell, our authentication module enforces exponential backoff delays (2 seconds, 4 seconds, 8 seconds...) that temporarily lock out input after failed attempts."

---

### Q10: How can another laptop on the same Wi-Fi communicate with your OSISOS virtual machine?
**Answer:**
> "In QEMU user-mode networking, the virtual machine resides behind a virtual NAT router (`10.0.2.15`). To make OSISOS reachable from outside, we configure QEMU host port forwarding in our `kernel/Makefile`:
> ```makefile
> -netdev user,id=n0,hostfwd=tcp::8080-:80,hostfwd=tcp::8007-:7,hostfwd=tcp::9999-:9999
> ```
> By omitting the IP before the colon (`::9999`), QEMU binds the port to `0.0.0.0` (all host network interfaces). We open port 9999 in our host firewall (`sudo ufw allow 9999/tcp`). Any computer on the same Wi-Fi can run `nc <host-wifi-ip> 9999` to stream text directly into our kernel message listener!"

---

### Q11: What happens if an unhandled CPU exception occurs (like dividing by zero)?
**Answer:**
> "In primitive kernels, dividing by zero causes an unhandled fault that triggers a CPU triple fault and instant machine reboot.
> 
> In OSISOS, all 32 CPU exceptions have registered assembly ISRs in `exceptions.S`. When a fault occurs, the assembly handler preserves register states and calls `kernel_panic()`. The kernel switches VGA text mode to high-contrast blue, prints the fault name (e.g. `EXCEPTION 0: DIVIDE ERROR`), dumps all registers (`EAX`, `EBX`, `ECX`, `EDX`, `ESP`, `EBP`, `EIP`, `EFLAGS`, `CR0`, `CR2`, `CR3`), logs the event to our 4KB ring buffer (`klog`), and executes `cli; hlt`."

---

### Q12: What was the hardest bug you encountered and how did you diagnose it?
**Answer:**
> "The hardest bug was an intermittent screen corruption issue where line 2 of our terminal would randomly display garbage characters when typing commands.
> 
> Because we had no GDB or GUI debugger, we analyzed the timings. We noticed that corruption only occurred when the screen was scrolling up. We realized that our 100Hz timer interrupt was asynchronously executing `kernel_print()` on row 2 to display a clock heartbeat at the exact instant `terminal_scroll()` was copying rows `y` to `y-1`.
> 
> We resolved the race condition by eliminating raw video memory writes from the timer ISR, storing uptime in a system variable, and making terminal scrolling atomic with respect to cursor updates."

---

## 4. Live Demonstration Cheat Sheet

Run this alongside your presentation for maximum impact:

```bash
# 1. Boot OSISOS directly in QEMU:
cd /home/ayush/College/OSISOS/kernel
make run

# 2. Log in with default credentials:
# Username: admin
# Password: osisos

# 3. Impress the panel with these commands in OSISOS:
sysinfo              # Show CPU, 32MB RAM, PIDs
ifconfig             # Show DHCP leased IP 10.0.2.15 and MAC
df                   # Show OFS v2 disk storage statistics
httpd                # Show active HTTP web server status
dmesg                # Show kernel boot ring buffer log
cat hello.txt        # Show persistent disk read
cowsay OSISOS Rocks  # Show ASCII terminal capabilities
matrix               # Show real-time falling code animation

# 4. Open a second terminal on your host Linux machine:
curl http://127.0.0.1:8080/    # Web page served BY the OSISOS kernel!
nc 127.0.0.1 8007              # Echo test (type words, see them echoed)
nc 127.0.0.1 9999              # Type: "Live Message from Host!" -> appears on OS screen!
```
