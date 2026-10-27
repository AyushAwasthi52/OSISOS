/* ==========================================================================
   OSISOS Presentation Engine & Interactive Subsystems
   ========================================================================== */

document.addEventListener('DOMContentLoaded', () => {
  const slides = Array.from(document.querySelectorAll('section.slide'));
  const totalSlides = slides.length;
  let currentSlide = 0;

  // DOM Elements
  const prevBtn = document.getElementById('prev-btn');
  const nextBtn = document.getElementById('next-btn');
  const currentSlideEl = document.getElementById('current-slide-num');
  const totalSlidesEl = document.getElementById('total-slides-num');
  const progressBar = document.getElementById('progress-bar');
  const overviewModal = document.getElementById('overview-modal');
  const overviewGrid = document.getElementById('overview-grid');
  const notesDrawer = document.getElementById('notes-drawer');
  const notesContent = document.getElementById('notes-content');
  const notesBtn = document.getElementById('notes-btn');
  const overviewBtn = document.getElementById('overview-btn');
  const fullscreenBtn = document.getElementById('fullscreen-btn');
  const timerDisplay = document.getElementById('hud-timer');

  totalSlidesEl.textContent = totalSlides;

  // Presenter Elapsed Timer
  let elapsedSeconds = 0;
  setInterval(() => {
    elapsedSeconds++;
    const mins = String(Math.floor(elapsedSeconds / 60)).padStart(2, '0');
    const secs = String(elapsedSeconds % 60).padStart(2, '0');
    if (timerDisplay) timerDisplay.textContent = `${mins}:${secs}`;
  }, 1000);

  // Speaker Notes Content Mapping
  const speakerNotes = {
    0: `<p><strong>Welcome & Introduction</strong></p>
        <p>Good morning/afternoon, professors and colleagues. Today I am thrilled to present <strong>OSISOS</strong> — an educational, from-scratch 32-bit multitasking operating system featuring its own freestanding TCP/IP network stack, inode-based filesystem, and security subsystem.</p>
        <p><em>Key takeaway:</em> No Linux libraries, no third-party code. Built from bare metal x86 assembly and freestanding C.</p>`,
    
    1: `<p><strong>Project Vision & System Constraints</strong></p>
        <p>Explain the deliberate design constraints:</p>
        <ul>
          <li>Target: 32MB physical RAM and a single 300MHz-class virtual CPU core.</li>
          <li>Why? Extreme constraints force optimal data structures, zero memory waste, and deep architectural clarity.</li>
          <li>Dual-environment structure: <code>host/</code> prototyping vs pure <code>kernel/</code> freestanding ring 0 execution.</li>
        </ul>`,

    2: `<p><strong>The Boot Journey: Bare Metal to C</strong></p>
        <p>Walk the audience through the boot sequence:</p>
        <ul>
          <li>GRUB locates the Multiboot1 header in <code>boot.S</code>, puts the CPU into 32-bit Protected Mode, and jumps to <code>_start</code>.</li>
          <li>Assembly creates the initial kernel stack and jumps to <code>kernel_main()</code>.</li>
          <li>We construct the Global Descriptor Table (GDT) and Interrupt Descriptor Table (IDT).</li>
          <li>We remap the legacy dual 8259 PIC so hardware IRQs 0-15 map to interrupt vectors 0x20-0x2F (preventing collisions with CPU exception vectors).</li>
        </ul>`,

    3: `<p><strong>Memory Architecture: PMM, Paging & Heap</strong></p>
        <p>Explain our three-tier memory model:</p>
        <ul>
          <li><strong>PMM (Physical Memory Manager):</strong> A compact bitmap tracks 4KB page frames. 32MB requires only an 8KB bitmap!</li>
          <li><strong>Virtual Memory (Paging):</strong> 2-level paging (1 Page Directory + Page Tables). We identity-map the first 8MB.</li>
          <li><strong>NULL Page Guard:</strong> Page 0 (0x00000000 - 0x00000FFF) is deliberately unmapped, converting any C null-pointer dereference into an immediate hardware page fault rather than silently corrupting memory.</li>
          <li><strong>KHeap:</strong> Dynamic <code>kmalloc</code>/<code>kfree</code> using boundary-tag metadata for OS data structures.</li>
        </ul>`,

    4: `<p><strong>Preemptive Multitasking & Context Switching</strong></p>
        <p>Highlight how our scheduler operates:</p>
        <ul>
          <li>The 100 Hz Programmable Interval Timer (PIT) triggers an interrupt every 10 milliseconds.</li>
          <li>On each tick, the CPU hardware pushes EFLAGS, CS, and EIP. Our assembly handler pushes all general-purpose registers (<code>pushal</code>).</li>
          <li>We save the stack pointer into the Process Control Block (PCB) and swap stacks.</li>
          <li>Demonstrate PID 1 (Interactive Shell) and PID 2 (Background Network Polling Task) running concurrently!</li>
        </ul>`,

    5: `<p><strong>Device Drivers & Hardware Discovery</strong></p>
        <p>Detail how OSISOS talks directly to x86 hardware:</p>
        <ul>
          <li><strong>VGA:</strong> Memory-mapped text buffer at 0xB8000 with custom scrolling and color attributes.</li>
          <li><strong>PS/2 Keyboard:</strong> IRQ 1 handler reading scancode set 1.</li>
          <li><strong>PCI Bus Scanner:</strong> Scans all 256 buses, 32 slots, and 8 functions using I/O ports 0xCF8 / 0xCFC.</li>
          <li><strong>Intel E1000 Gigabit NIC:</strong> Automatically discovers Intel 8254x cards in QEMU and VirtualBox, configures MMIO registers, and sets up 64-descriptor circular DMA rings.</li>
        </ul>`,

    6: `<p><strong>Storage Engine: OFS v2 (OSISOS File System)</strong></p>
        <p>Explain the architecture of our persistent disk storage:</p>
        <ul>
          <li>ATA IDE driver: 28-bit LBA PIO sector reads and writes to virtual disk.</li>
          <li>OFS v2 layout: Superblock (magic <code>0x4F465332</code>), 128-entry Inode Table (64 bytes per inode), and 1024-byte data blocks.</li>
          <li>Supports contiguous multi-block files, directory structures, path lookups (<code>/www/index.html</code>), disk usage stats (<code>df</code>), and file editor.</li>
        </ul>`,

    7: `<p><strong>Security & Exception Handling</strong></p>
        <p>Showcase reliability and defensive programming:</p>
        <ul>
          <li><strong>Shadow Passwords:</strong> Freestanding SHA-256 and PBKDF2 hash algorithm (10,000 iterations) with salted entries stored in <code>/etc/shadow</code>.</li>
          <li>Exponential lockout delay to neutralize brute-force login attempts.</li>
          <li><strong>Kernel Panic / BSOD:</strong> Captures CPU exception vectors (Divide Error, Page Fault, GPF) and renders a full register state dump (EAX..EDX, CR0, CR2, CR3, EIP) and stack backtrace.</li>
        </ul>`,

    8: `<p><strong>The Network Stack: Layer 2 to Layer 3</strong></p>
        <p>Breakdown of the custom TCP/IP implementation:</p>
        <ul>
          <li>Zero external networking libraries. Everything lives in <code>kernel/net/</code>.</li>
          <li>Ethernet II frame parsing & destination MAC filtering.</li>
          <li>ARP Cache with pending request queues and automated address resolution.</li>
          <li>IPv4 header validation, checksum generation, routing, and ICMP Ping echo replies.</li>
        </ul>`,

    9: `<p><strong>Dynamic Configuration: DHCP & DNS</strong></p>
        <p>Demonstrate zero-configuration networking:</p>
        <ul>
          <li><strong>DHCP Client:</strong> Automatic lease acquisition from router/QEMU (DISCOVER -> OFFER -> REQUEST -> ACK), leasing 10.0.2.15, gateway 10.0.2.2, and DNS 10.0.2.3.</li>
          <li><strong>DNS Resolver:</strong> RFC 1035 UDP client capable of decoding DNS A-records, parsing label compression pointers, and maintaining a local name cache.</li>
        </ul>`,

    10: `<p><strong>TCP Implementation: The Crown Jewel</strong></p>
        <p>Emphasize the complexity of our TCP engine:</p>
        <ul>
          <li>Over 900 lines of rigorous state-machine code in <code>kernel/net/tcp.c</code>.</li>
          <li>Full RFC 793 state machine: LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT, CLOSE_WAIT, TIME_WAIT.</li>
          <li>Sequence and ACK tracking, sliding window flow control, and timeout retransmission with exponential backoff.</li>
          <li>Clean socket API: <code>tcp_socket()</code>, <code>tcp_connect()</code>, <code>tcp_send()</code>, <code>tcp_recv()</code>.</li>
        </ul>`,

    11: `<p><strong>Built-in Services & Local Wi-Fi Reachability</strong></p>
        <p>Showcase how other computers communicate with OSISOS:</p>
        <ul>
          <li><strong>HTTP Server (port 80):</strong> Streams static HTML files straight from the OFS filesystem.</li>
          <li><strong>Echo Server (port 7):</strong> TCP and UDP bounce-back.</li>
          <li><strong>Remote Console Message Service (port 9999):</strong> Anyone on the local Wi-Fi can run <code>nc &lt;ip&gt; 9999</code> to send text that prints live on the OSISOS console!</li>
        </ul>`,

    12: `<p><strong>Interactive Shell, Utilities & Games</strong></p>
        <p>Explain the userland environment:</p>
        <ul>
          <li>25+ built-in commands: <code>ifconfig</code>, <code>ping</code>, <code>netstat</code>, <code>http</code>, <code>fetch</code>, <code>edit</code>, <code>dmesg</code>, <code>sysinfo</code>.</li>
          <li>In-kernel text editor with cursor navigation and disk persistence.</li>
          <li>Easter eggs: Snake game, Matrix falling glyphs, Cowsay, Fortune, ASCII train (<code>sl</code>).</li>
        </ul>`,

    13: `<p><strong>End-to-End System Architecture</strong></p>
        <p>Review the full architectural diagram:</p>
        <ul>
          <li>Show how Hardware Drivers, Kernel Core, Memory Management, File System, and Network Stack cooperate without collisions.</li>
          <li>Point out the clean separation between Ring 0 kernel space and user interfaces.</li>
        </ul>`,

    14: `<p><strong>Live Interactive Terminal Demonstration</strong></p>
        <p>Use the embedded terminal on screen to demonstrate commands live:</p>
        <ul>
          <li>Type <code>help</code>, <code>sysinfo</code>, <code>ifconfig</code>, <code>httpd</code>, <code>df</code>, <code>dmesg</code>.</li>
          <li>Run <code>matrix</code> or <code>cowsay OSISOS Rocks!</code> for visual impact.</li>
        </ul>`,

    15: `<p><strong>Engineering Challenges & War Stories</strong></p>
        <p>Show the engineering depth by discussing hurdles overcome:</p>
        <ul>
          <li>VirtualBox vs QEMU PCI IDs and ACPI power-off differences.</li>
          <li>Scrolling the VGA text mode screen without corrupting live timer prints.</li>
          <li>Managing network buffers without running out of the 32MB physical memory limit.</li>
        </ul>`,

    16: `<p><strong>Verification, Test Suite & Results</strong></p>
        <p>Present the testing evidence:</p>
        <ul>
          <li>Automated Python testing harness (<code>tools/nettest.py</code>) running QEMU headless and verifying packet exchanges.</li>
          <li>Over 8,800 lines of code across 34 kernel source files.</li>
          <li>Sub-second boot speed, stable TCP streams, and rock-solid reliability.</li>
        </ul>`,

    17: `<p><strong>Conclusion & Future Roadmap</strong></p>
        <p>Wrap up the presentation:</p>
        <ul>
          <li>Summarize the primary takeaways: building an OS from scratch demystifies computers completely.</li>
          <li>Future goals: 64-bit Long Mode transition, SMP multi-core support, ELF usermode binary loading.</li>
          <li>Invite questions from the panel!</li>
        </ul>`
  };

  // Switch Slide Function
  function goToSlide(index) {
    if (index < 0 || index >= totalSlides) return;
    
    slides[currentSlide].classList.remove('active');
    currentSlide = index;
    slides[currentSlide].classList.add('active');

    // Update UI Indicators
    currentSlideEl.textContent = currentSlide + 1;
    const progressPercent = ((currentSlide + 1) / totalSlides) * 100;
    progressBar.style.width = `${progressPercent}%`;

    prevBtn.disabled = currentSlide === 0;
    nextBtn.disabled = currentSlide === totalSlides - 1;

    // Update Speaker Notes
    if (speakerNotes[currentSlide]) {
      notesContent.innerHTML = speakerNotes[currentSlide];
    } else {
      notesContent.innerHTML = `<p>Slide ${currentSlide + 1} notes.</p>`;
    }

    // Update active state in overview modal
    const overviewCards = document.querySelectorAll('.overview-card');
    overviewCards.forEach((c, i) => {
      c.classList.toggle('active', i === currentSlide);
    });

    // Reset slide scroll position
    slides[currentSlide].scrollTop = 0;
  }

  // Next / Prev Actions
  function nextSlide() {
    if (currentSlide < totalSlides - 1) goToSlide(currentSlide + 1);
  }

  function prevSlide() {
    if (currentSlide > 0) goToSlide(currentSlide - 1);
  }

  prevBtn.addEventListener('click', prevSlide);
  nextBtn.addEventListener('click', nextSlide);

  // Keyboard Navigation
  window.addEventListener('keydown', (e) => {
    // If user is typing inside the interactive terminal, do not hijack normal typing keys
    if (document.activeElement && document.activeElement.classList.contains('terminal-input')) {
      if (e.key === 'Escape') {
        document.activeElement.blur();
      }
      return;
    }

    switch (e.key) {
      case 'ArrowRight':
      case ' ':
      case 'PageDown':
      case 'l':
      case 'L':
        e.preventDefault();
        nextSlide();
        break;

      case 'ArrowLeft':
      case 'Backspace':
      case 'PageUp':
      case 'h':
      case 'H':
        e.preventDefault();
        prevSlide();
        break;

      case 'Home':
        e.preventDefault();
        goToSlide(0);
        break;

      case 'End':
        e.preventDefault();
        goToSlide(totalSlides - 1);
        break;

      case 'f':
      case 'F':
        toggleFullscreen();
        break;

      case 'o':
      case 'O':
      case 'Escape':
        toggleOverview();
        break;

      case 'n':
      case 'N':
        toggleNotes();
        break;
    }
  });

  // Fullscreen Handler
  function toggleFullscreen() {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen().catch(() => {});
      fullscreenBtn.classList.add('active');
    } else {
      if (document.exitFullscreen) {
        document.exitFullscreen();
        fullscreenBtn.classList.remove('active');
      }
    }
  }
  fullscreenBtn.addEventListener('click', toggleFullscreen);

  // Speaker Notes Toggle
  function toggleNotes() {
    notesDrawer.classList.toggle('open');
    notesBtn.classList.toggle('active');
  }
  notesBtn.addEventListener('click', toggleNotes);
  document.getElementById('close-notes').addEventListener('click', toggleNotes);

  // Overview Grid Modal
  function buildOverviewGrid() {
    overviewGrid.innerHTML = '';
    slides.forEach((slide, i) => {
      const card = document.createElement('div');
      card.className = `overview-card ${i === currentSlide ? 'active' : ''}`;
      
      const catEl = slide.querySelector('.slide-category');
      const titleEl = slide.querySelector('.slide-title');
      
      const category = catEl ? catEl.textContent.trim() : 'SLIDE';
      const title = titleEl ? titleEl.textContent.trim() : `Slide ${i + 1}`;

      card.innerHTML = `
        <div>
          <span class="overview-num">SLIDE ${String(i + 1).padStart(2, '0')}</span>
          <div class="overview-name">${title}</div>
        </div>
        <div class="overview-cat">${category}</div>
      `;

      card.addEventListener('click', () => {
        goToSlide(i);
        toggleOverview();
      });

      overviewGrid.appendChild(card);
    });
  }

  function toggleOverview() {
    overviewModal.classList.toggle('open');
    overviewBtn.classList.toggle('active');
    if (overviewModal.classList.contains('open')) {
      buildOverviewGrid();
    }
  }
  overviewBtn.addEventListener('click', toggleOverview);
  document.getElementById('close-overview').addEventListener('click', toggleOverview);

  // --------------------------------------------------------------------------
  // Interactive OSISOS Terminal Simulator
  // --------------------------------------------------------------------------
  const termBody = document.getElementById('term-body');
  const termInput = document.getElementById('term-input');
  const termChips = document.querySelectorAll('.terminal-chip');

  const termResponses = {
    'help': `Available OSISOS Commands:
  System:     sysinfo, uptime, dmesg, clear, reboot, shutdown
  Memory:     pmm, vmm, kheap, selftest
  Storage:    ls, cat <file>, edit <file>, ofs, df, rm, mv
  Network:    ifconfig, ping <host>, nslookup <name>, arp, netstat, nc, http, fetch, httpd
  Security:   passwd, auth, logout
  Fun:        matrix, snake, cowsay <msg>, fortune, sl`,

    'sysinfo': `OSISOS Kernel v1.0 (i686 Protected Mode)
---------------------------------------
CPU:         x86 32-bit (1 vCPU, Ring 0)
RAM:         32 MB Installed (Bitmap PMM)
Scheduler:   Preemptive Round-Robin (100 Hz PIT)
Active PIDs: 2 (PID 1: Shell, PID 2: Net Poller)
Storage:     IDE ATA Primary (OFS v2 Inode FS)
NIC:         Intel 82540EM Gigabit Ethernet (E1000)
Uptime:      428 seconds`,

    'ifconfig': `eth0: flags=UP,BROADCAST,RUNNING
      inet 10.0.2.15  netmask 255.255.255.0  broadcast 10.0.2.255
      gateway 10.0.2.2  dns 10.0.2.3 (via DHCP)
      ether 52:54:00:12:34:56  txqueuelen 64 (E1000 MMIO)
      RX packets 142 bytes 98420 (0 errors, 0 dropped)
      TX packets 118 bytes 84192 (0 errors, 0 dropped)`,

    'httpd': `OSISOS HTTP/1.1 Server Status
-----------------------------
State:          ACTIVE (Listening on TCP port 80)
Document Root:  /www
Files Loaded:   index.html, style.css, favicon.ico
Requests Total: 18
Total Bytes Tx: 49,280 bytes
Keep-Alive:     Supported`,

    'df': `Filesystem      1K-blocks      Used Available Use% Mounted on
ofs2 (IDE 0)         10240       384      9856   4% /
Inodes: 128 total, 8 used, 120 free`,

    'dmesg': `[    0.000000] Multiboot magic 0x2BADB002 detected
[    0.001204] GDT initialized (Kernel & User descriptors loaded)
[    0.002410] IDT initialized (256 vectors remapped)
[    0.004112] PMM: 32 MB detected. 8192 frames tracked in bitmap
[    0.007890] Paging enabled. Page 0 NULL-guard active
[    0.012400] KHeap: boundary-tag allocator online
[    0.021000] Tasking: established PID 1 (kernel shell)
[    0.035000] IDE: Primary drive detected (10 MB qcow2)
[    0.041000] OFS v2: Superblock verified. 128 inodes online
[    0.052000] PCI: Found Intel E1000 NIC at bus 0 slot 3 func 0
[    0.061000] E1000: MAC 52:54:00:12:34:56. DMA rings initialized
[    0.075000] Tasking: spawned PID 2 (network poller)
[    0.110000] DHCP: Bound to 10.0.2.15/24 via gateway 10.0.2.2
[    0.120000] TCP: Listening on port 7 (echo), 80 (httpd), 9999 (msg)`,

    'ping 10.0.2.2': `PING 10.0.2.2 (10.0.2.2): 56 data bytes
64 bytes from 10.0.2.2: icmp_seq=1 ttl=64 time=1.20 ms
64 bytes from 10.0.2.2: icmp_seq=2 ttl=64 time=0.98 ms
64 bytes from 10.0.2.2: icmp_seq=3 ttl=64 time=1.04 ms
64 bytes from 10.0.2.2: icmp_seq=4 ttl=64 time=0.95 ms
--- 10.0.2.2 ping statistics ---
4 packets transmitted, 4 packets received, 0.0% packet loss`,

    'ls': `hello.txt     142 B   [FILE]
etc/          <DIR>   [DIRECTORY]
www/          <DIR>   [DIRECTORY]
selftest.tmp  1.5 KB  [FILE]`,

    'cat hello.txt': `Welcome to the OSISOS File System (OFS v2)!
Persistent storage powered by 28-bit LBA IDE ATA driver.
Contiguous multi-block allocation & 128 inodes supported.`,

    'cowsay OSISOS': `  _____________
< OSISOS Rocks! >
  -------------
         \\   ^__^
          \\  (oo)\\_______
             (__)\\       )\\/\\
                 ||----w |
                 ||     ||`,

    'matrix': `[Matrix Digital Rain Simulation Triggered]
01001111 01010011 01001001 01010011 01001111 01010011
Wake up, Neo... The Matrix has you.
Follow the white rabbit.
Knock, knock, Neo.`,

    'sl': `      ====        ________                ___________
  _D _|  |_______/        \\__I_I_____===__|_________|
   |(_)---  |   H\\________/ _____ |   (|) |         |
   /     |  |   H  |  |     |   | |     | |         |
  |      |  |   H  |__--------------------| [OSISOS]|
  | ________|___H__/__|_____/[][]~\\_______|_________|
  |/ |   |_____/____\\______/     \\_\\_____/____\\_____|
  (O)(O)        (O)(O)            (O)(O)       (O)(O)`
  };

  function executeTerminalCommand(raw) {
    const cmd = raw.trim();
    if (!cmd) return;

    // Echo prompt and command
    const promptLine = document.createElement('div');
    promptLine.innerHTML = `<span class="terminal-prompt">admin@osisos&gt;</span> <span style="color:#fff;">${escapeHtml(cmd)}</span>`;
    termBody.appendChild(promptLine);

    if (cmd === 'clear') {
      termBody.innerHTML = '';
    } else {
      const outputLine = document.createElement('div');
      outputLine.style.marginBottom = '8px';
      outputLine.style.color = '#cbd5e1';

      if (termResponses[cmd]) {
        outputLine.textContent = termResponses[cmd];
      } else if (cmd.startsWith('cowsay ')) {
        const msg = cmd.replace('cowsay ', '');
        outputLine.textContent = `  ${'_'.repeat(msg.length + 2)}
< ${msg} >
  ${'-'.repeat(msg.length + 2)}
        \\   ^__^
         \\  (oo)\\_______
            (__)\\       )\\/\\
                ||----w |
                ||     ||`;
      } else if (cmd.startsWith('echo ')) {
        outputLine.textContent = cmd.replace('echo ', '');
      } else {
        outputLine.textContent = `Unknown command: ${cmd}. Type 'help' for available commands.`;
        outputLine.style.color = '#f87171';
      }
      termBody.appendChild(outputLine);
    }

    termBody.scrollTop = termBody.scrollHeight;
    termInput.value = '';
  }

  function escapeHtml(text) {
    return text.replace(/[&<>"']/g, (m) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;'
    }[m]));
  }

  if (termInput) {
    termInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        executeTerminalCommand(termInput.value);
      }
    });
  }

  termChips.forEach((chip) => {
    chip.addEventListener('click', () => {
      const cmd = chip.dataset.cmd;
      if (termInput) {
        termInput.value = cmd;
        executeTerminalCommand(cmd);
      }
    });
  });

  // Initialize first slide
  goToSlide(0);
});
