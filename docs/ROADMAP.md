# Roadmap

## Done

Kernel foundations, e1000 driver, Ethernet/ARP/IPv4/ICMP, UDP, TCP, DHCP, DNS, loopback, HTTP file server, OFS v2 with migration,
exception handling + panic screen + NULL guard, PBKDF2 authentication with throttling, remote admin console, kernel log,
self-test, system info, fun programs, documentation, and an end-to-end test suite.

## Known limitations

| Area | Limitation |
|---|---|
| TCP | no out-of-order reassembly, congestion control, RTT estimation, SACK, window scaling |
| IP | no fragmentation/reassembly, no IPv6, no ICMP errors, no routing table (one gateway) |
| DHCP/DNS | no ARP probing / RELEASE; A records only, no TCP fallback |
| HTTP | no keep-alive, `Range`, `POST`/upload, caching headers, TLS |
| Filesystem | contiguous extents (can fragment), 128 inodes, 2 MB files, no journal/permissions/timestamps |
| Kernel | polled NIC (no interrupts), 2 tasks, no processes/address spaces, ring-3 is a demo, no SMP |
| Security | unencrypted remote console, single account, no memory protection (see SECURITY.md) |
| Input | no Shift/arrow keys, no command history |
| Double fault on stack overflow | cannot be handled on the same stack (needs a TSS task gate) |

## Suggested next steps (highest value first)

1. **e1000 receive/transmit interrupts** - cut latency and CPU use; prerequisite for throughput work.
2. **Encrypted remote shell** (an SSH-like protocol: key exchange, AEAD) to remove the biggest security caveat.
3. **TCP**: out-of-order queue, RTT-based RTO, slow start / congestion avoidance / fast retransmit, SACK.
4. **Memory protection**: per-task page tables, read-only kernel text (W^X), a real user-mode ABI; then an ELF loader for programs.
5. **Multi-user and permissions**; a bigger/journaled filesystem (extent tree or FAT-style allocation, timestamps).
6. **HTTP**: keep-alive, `Range`, upload (`PUT`) behind authentication, simple CGI-style handlers.
7. **Shell UX**: Shift and arrow keys, history, tab completion, pipes.
8. **Observability**: persist the panic report to disk, serial console logging, `top`.
9. **Platform**: IPv6, NTP client, USB keyboard, real-hardware testing, SMP.
