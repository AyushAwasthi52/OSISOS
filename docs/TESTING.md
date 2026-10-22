# Testing

The kernel is tested end to end: a Python harness boots it in QEMU, types into the console through the QEMU monitor, reads
the VGA screen back, and talks to the guest over the network from the host through forwarded ports. Today the suite has
**137 checks** and runs in roughly half an hour.

## Running

Requirements: `qemu-system-i386`, `python3`, `tcpdump`, and the ability to bind **host port 53 on 127.0.0.1** (the fake DNS server):
run as root, or lower the privileged-port limit (`sudo sysctl net.ipv4.ip_unprivileged_port_start=0`).

```sh
cd kernel && make osisos.bin
python3 ../tools/nettest.py            # prints PASS/FAIL per check and a summary; exit status 1 on any failure
```

Host ports used: 18000 (test HTTP server), 18007 (echo), 18008 (UDP echo), 18023 (remote console), 18080 (guest HTTP), 15555, 53 (fake DNS).
Temporary files: `/tmp/osisos-*.img`, `/tmp/osisos-mon.sock`, `/tmp/osisos.pcap`.

## How the harness works

| Piece | Mechanism |
|---|---|
| Keyboard | QEMU monitor `sendkey` (one key at a time, ~0.08 s each) |
| Screen | monitor `pmemsave 0xb8000` of the VGA buffer, decoded to text (25 lines; characters outside ASCII become spaces) |
| Network (client) | the guest connects to the host at the QEMU gateway (10.0.2.2 / 192.168.76.2): a Python HTTP server, a raw TCP server, a fake DNS server |
| Network (server) | host connects through `hostfwd` ports into the guest's HTTP, echo, UDP echo and rcon |
| DHCP | QEMU runs the guest on a custom subnet (`192.168.76.0/24`, lease `.50`) so a real lease is distinguishable from the 10.0.2.15 fallback |
| Disks | blank raw images per scenario; a **v1 disk is synthesised byte by byte** for the migration test |
| Packet evidence | `-object filter-dump` pcap analysed with `tcpdump` (DHCP renewal on the wire) |
| Fault injection | the guest's own `netdrop` command, `crash` kinds |

## Coverage

| Area | Examples of what is checked |
|---|---|
| Boot, ARP, ICMP | boot banner, ping the gateway, ARP cache |
| DHCP | real lease, renewal is unicast on the wire, fallback on an isolated network |
| TCP client | HTTP GET, `nc`, multi-segment download (flow control), connection refused (RST) |
| TCP server | HTTP page, echo (1.9 KB), 15 sequential connections (no socket leak), 6 parallel downloads |
| Reliability | 8 KB random data echoed byte-for-byte under **15 % packet loss** |
| UDP, DNS | UDP echo (614 B), A record with compression pointer, CNAME, NXDOMAIN, cache, hostnames in `ping`/`http` |
| HTTP | byte-exact 20 KB and 150 KB files, HEAD, 404/405/301/400/414/431/418, listings, traversal attempts, `/etc` hidden |
| Filesystem | fetch into subdirectories, mv/rm/rmdir, space reuse without overlap, persistence across reboot, **v1 -> v2 migration** |
| Crash handling | every `crash` kind: decoded cause + registers + log; R reboots, H halts, automatic reboot after 15 s; web server **and disk intact** afterwards |
| Auth | PBKDF2 hash on disk recomputed in Python, wrong/unknown user, 70-char inputs, lockout, `passwd` (wrong/short/mismatch/ok/masked), persistence |
| Remote console | disabled by default, default-password refusal, separate lockout, CRLF output, long output, denied commands, busy session, pipelined commands, telnet negotiation, off/on persistence |
| Self-check | `selftest` passes (also after a crash/reboot cycle) |
| Fun | `snake` plays until it hits the wall, `matrix` animates and restores the screen, `sl`, easter eggs, `rm -rf /` deletes nothing |

Cryptography is additionally verified natively against Python `hashlib` (see `docs/SECURITY.md`).

## Writing a new test

Inside `main()` in `tools/nettest.py`, with the guest running and logged in:

```python
out = q.run("df", 3, until="inodes free")        # type a command, wait (up to 3 s) for text, return the screen
check("df shows OFS v2", "OFS v2" in out, out)   # name, condition, details printed on failure
st, headers, body = get("/data.bin")             # HTTP request to the guest via the forward
```

Helpers: `q.type(text)`, `q.screen()`, `q.cmd("system_reset")` (raw monitor command), `wait_saved(cmd, nbytes)` for `fetch`.

## Known fragilities (and how to avoid them)

* The screen is 25 lines, so anything printed earlier may have scrolled away; `dmesg` shows only 20 lines. Assert on *recent* output.
* A long log line wraps at 80 columns and splits the string you expect; match short substrings.
* Typing is slow (~0.08 s/char): long inputs make tests slow, and timing-based tests (lockouts) must act *immediately*.
* `until=` matches text anywhere on the screen, including *older* lines: include something unique to this command's result.
* Never `pkill -f` a pattern that appears in your own command line (it kills your shell); kill by exact process name.
