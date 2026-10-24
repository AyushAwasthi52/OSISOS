# Developer Guide

## 1. Build and run

```sh
cd kernel
make osisos.bin         # gcc -m32 -ffreestanding ..., linked with linker.ld; writes dependency files (.d)
make run-direct         # QEMU, boots osisos.bin directly
make iso / run-user     # GRUB ISO (needs grub-mkrescue, xorriso)
make clean              # NOTE: also deletes drive.img
```

The build is freestanding: no libc. `net/netstr.c` provides `memcpy/memmove/memset/memcmp` (written with `rep movsb` on purpose: a
C loop is turned back into a call to `memcpy` by GCC at `-O2` and would recurse forever). Compile with warnings on
(`-Wall -Wextra`): the tree is warning-clean apart from a few legacy ones in `ofs.c` / `editor.c`.

## 2. Adding a shell command

Commands are dispatched in `execute_command()` (`core/shell.c`) through small hook functions, so new features do not need
to edit the big if-chain. Each hook gets the raw line and returns `true` if it handled it.

```c
/* core/mycmd.c */
#include <stdbool.h>
extern void terminal_print_string(const char*);
bool my_shell_command(const char* line) {
    if (line[0]=='h' && line[1]=='i' && line[2]=='\0') { terminal_print_string("hi!\n"); return true; }
    return false;                              /* not mine: the shell keeps looking */
}
```

1. Add the file to `OBJS` in `kernel/Makefile`.
2. In `shell.c`: declare `extern bool my_shell_command(const char*);` and call it next to the other hooks at the top of `execute_command`.
3. Add a line to the `help` text and to `docs/USER_GUIDE.md`.

Existing hooks: `extras_command` (core/extras.c), `fs_shell_command` (fs/fscmd.c), `net_shell_command` (net/netcmd.c).
Rules: tokenise a *copy* of the line (never mutate the caller's buffer if you may return `false`); print only through
`terminal_print_*` (that is what the remote console captures); do not draw on VGA directly unless you also add the command to
the remote console's deny list (`net/rcon.c: denied()`).

## 3. Writing a TCP server (non-blocking, runs in the network task)

```c
static int listener;
void myserver_init(void) { listener = tcp_socket(); tcp_listen(listener, 9000); }

void myserver_poll(void) {                         /* call from net_services_poll() */
    int c = tcp_accept(listener, TCP_NOWAIT);       /* TCP_AGAIN when nobody is waiting */
    if (c >= 0) { /* remember c in a connection table */ }
    for (each connection c) {
        char buf[128];
        int n = tcp_recv(c, buf, sizeof buf, TCP_NOWAIT);
        if (n > 0)  tcp_send(c, buf, n, TCP_NOWAIT);          /* check tcp_send_space() first for big replies */
        else if (n == 0 || (n < 0 && n != TCP_AGAIN)) tcp_close(c);   /* EOF or error */
    }
}
```

Never block in the network task: it also delivers packets. Look at `net/services.c` (echo) and `net/httpd.c` (streaming files) for complete examples.

## 4. Writing a TCP client (blocking, runs in the shell task)

```c
int s = tcp_socket();
if (tcp_connect(s, IP4(10,0,2,2), 8000, 5*NET_HZ) == TCP_OK) {
    tcp_send(s, "hello\n", 6, 5*NET_HZ);
    char buf[256]; int n = tcp_recv(s, buf, sizeof buf, 3*NET_HZ);     /* 0 = peer closed */
}
tcp_close(s);
```

Names: `dns_resolve("example.com", &ip, 6*NET_HZ)`. UDP: `udp_socket / udp_sendto / udp_recvfrom`. See [NETWORKING.md](NETWORKING.md).

## 5. Other APIs

| Need | API |
|---|---|
| Log an event | `klog("dhcp: bound %I", ip);` (`%s %d %u %I %%`) -> `dmesg` |
| Files | `ofs_put`, `ofs_lookup`, `ofs_stat`, `ofs_inode_read`, `ofs_remove`, `ofs_rename` (see [FILESYSTEM.md](FILESYSTEM.md)) |
| Passwords | `auth_verify_ch(channel, user, pass, &wait)`, `auth_set_password`, `auth_has_default_password` |
| Random bytes | `rng_bytes(buf, n)`; call `rng_event(x)` from new input/packet paths to feed the pool |
| Crypto | `sha256/hmac_sha256/pbkdf2_hmac_sha256`, `x25519/x25519_base`, `aead_encrypt/aead_decrypt` (ChaCha20-Poly1305), Noise NK responder `noise_resp_*` / `noise_encrypt/decrypt` |
| Time | `net_now()` (ticks, 100/s); `seconds_passed` |
| Memory | `kmalloc/kfree` (1 MB heap: do not use in per-packet paths); prefer static buffers |
| Fatal error | `kpanic("message")` |
| Remote output capture | print via `terminal_print_*`; `terminal_sink` redirects while a remote command runs |

### Cryptography code

Do not write new crypto casually. If you change anything in `kernel/core/{sha256,x25519,chacha20poly1305,noise}.c`, re-run
`python3 tools/crypto_verify.py` (compiles the *same* files natively and compares them with Python `hashlib`, `pyca/cryptography` and the
`noiseprotocol` library, 1682 checks). The X25519 routine uses static scratch space (not re-entrant) and must only be called from one task
(the network task). Secrets are wiped after use (`volatile` stores, so the compiler cannot delete them).

## 6. Rules of the road

* **Locking.** Single CPU: shared state is protected by disabling interrupts (`net_lock()` / `net_unlock(flags)`, nesting is safe).
  Never call `net_wait()`, `task_yield()` or anything that blocks while holding it. Keep critical sections short.
* **Stacks are small** (4 KB for the network task). Put big buffers in `static` storage, as `httpd.c` does.
* **Network task = never block.** Anything that waits belongs in the shell task or needs a state machine.
* **No undefined behaviour.** GCC deletes it (a C `1/0` compiled to *nothing*, so `crash div0` uses inline assembly).
* **Validate everything from the network** (lengths, checksums, bounds) before using it; see how `tcp_input`, `dns.c` and `httpd.c` do it.
* **Bounded loops on hardware** (`ide.c` has timeouts) - an unbounded wait with interrupts off hangs the machine.
* **Do not break the on-disk format** without a migration (see `migrate_v1`) - the kernel formats a disk it does not recognise.

## 7. Debugging

| Tool | Use |
|---|---|
| Panic screen | any CPU exception shows cause, EIP, registers, stack words, recent log; `R` reboots, `H` halts. Resolve EIP with `addr2line -e kernel/osisos.bin 0x...` or `objdump -d` |
| `dmesg` / `klog` | the first stop; add `klog()` lines freely |
| `ifconfig`, `netstat`, `arp`, `dhcp` | counters and tables |
| `netdrop N` | reproduce retransmission paths |
| `crash <kind>` | exercise the panic path |
| pcap | QEMU `-object filter-dump,id=f1,netdev=n0,file=x.pcap`, then `tcpdump -nn -r x.pcap` |
| QEMU monitor | `-monitor stdio`: `info registers`, `xp /16x 0x...`, `pmemsave` |
| GDB | `qemu-system-i386 -kernel osisos.bin -s -S`, then `gdb osisos.bin -ex 'target remote :1234'` |

## 8. Lessons from building this (pitfalls that bit)

* Stale objects after a header change silently corrupted memory: the Makefile now emits `.d` files (`-MMD -MP`).
* The kernel heap was 4 KB; the NIC wanted 70 KB: DMA buffers ended up at physical address 0. Check allocation results.
* An unbounded driver wait under `cli` can freeze the machine: always bound.
* `kpanic()` must disable interrupts first, or the timer redraws over the panic and the keyboard IRQ eats your keypress.
* GCC `-O2` turns copy loops into `memcpy` calls: implement `memcpy` with `rep movsb`, not a loop.
* A 1-byte overflow (`buffer[size]` with size == 512) and an unbounded copy at the login prompt both existed in the original code.

## 9. Code style

C99, 4 spaces, `snake_case`, a comment at the top of each file saying what it is and what it deliberately does not do,
`static` for file-local helpers, one-line comments for "why", not "what".
