# Security

OSISOS is an educational system. This document states what it protects against, how, and - just as important -
what it does **not**.

## Threat model

Assumed: a small trusted network (home lab, classroom, an isolated VM network). Not assumed: the internet, hostile
local users, or physical attackers. Do not expose it to the internet.

## Protections in place

| Area | Protection |
|---|---|
| Passwords | **PBKDF2-HMAC-SHA256**, 4096 iterations, 16-byte random salt, fresh salt on every change; plaintext never stored; typed passwords wiped from memory after use; constant-time compare |
| Verified crypto | SHA-256/HMAC/PBKDF2 checked against Python `hashlib` (27 vectors incl. block boundaries, RFC vector) and re-checked by `selftest` |
| Brute force | 3 failures -> lockout 5 s, doubling to 5 min; locked logins are not even evaluated; unknown users cost the same time as wrong passwords |
| Separate channels | console and remote logins have independent counters: guessing over the network cannot lock the keyboard |
| Default credentials | first boot flags `admin/osisos` as *must change*; the shell nags; **remote logins are refused until it is changed** |
| Remote console | **off by default**; enabling is a local, persistent, deliberate act; one session; 5-minute idle timeout; 3 failures drop the connection; denied interactive commands; no output leaks to the local screen |
| Audit | logins, failures, lockouts, remote commands, httpd requests, password changes -> `dmesg` (attacker-supplied names sanitised and truncated) |
| Web server | only GET/HEAD; request/target size limits; `.`/`..`, backslash, control characters and bad escapes rejected before any filesystem access; `/etc` (credentials) never served, even with document root `/`; 30 s idle timeout |
| Network input | all headers/lengths/checksums validated; TCP resets need an exact sequence match (blind-reset resistance); DNS replies need the right server, port and id; ARP cache not populated by strangers |
| Kernel | all CPU exceptions handled (no silent triple-fault); page 0 unmapped so NULL dereferences fault; bounded disk waits; bounded copies (the login buffer overflow was fixed) |
| Disk | data written before metadata is published; migrations never wipe a disk |

## Known weaknesses (read before deploying)

1. **The remote console is unencrypted.** Passwords and commands cross the network in clear text. Anyone who can sniff the
   path can take over the machine. Use it only on a trusted link (or tunnel it, e.g. through an SSH port forward from a jump host).
2. **No isolation inside the machine.** Everything runs in ring 0; the 32 MB identity map is writable (and executable) for all code.
   The ring-3 demo can overwrite the kernel. There is no per-process address space, no W^X, no stack canaries.
3. **One account, no privileges.** Anyone who logs in can do anything, including `crash`, `reboot`, and overwriting `/etc/shadow` with `fetch`.
4. **Default password window.** Until the first `passwd`, the console password is public. (Remote access stays closed in that window.)
5. **Weak entropy.** Salts, TCP initial sequence numbers and DNS ids derive from the TSC and the tick counter mixed through SHA-256 /
   an LCG-style counter: unique, not cryptographically unpredictable. An on-path attacker can plausibly guess ISNs and DNS ids.
6. **PBKDF2 cost is low** (4096 iterations, chosen for a 300 MHz-class VM). Stolen `/etc/shadow` hashes are cheaper to attack than
   modern guidance (hundreds of thousands of iterations); the count is stored per hash, so it can be raised (`AUTH_ITERATIONS`).
7. **Denial of service is easy.** 12 TCP sockets total, 6 HTTP connections, one remote session; a SYN flood or a few idle connections
   can exhaust them (idle timeouts bound the damage to 30-300 s). The remote channel can be locked out by anyone who can reach port 2323.
8. **No TLS**, no HTTP authentication, no virtual hosts; the web root is public by design.
9. **L2/L3 spoofing.** No ARP authentication, no DNS validation beyond id/port, no ICMP rate limiting.
10. **Panic screen shows internals** (registers, log lines) on the local display.
11. **No recovery account.** A lost password means editing the disk image (delete `/etc/shadow`) - which also shows that physical access = full access.

## Recommendations

* Put OSISOS on an isolated network or behind a firewall that only admits the ports you need (80, optionally 2323 from one admin host).
* Change the password immediately; use a long one (the hash is cheap to attack offline).
* Leave `rcon` off unless you need it; turn it off when finished (`rcon off`).
* Watch `dmesg` for `auth: FAILED login` and `rcon:` lines.

## Reporting and hardening ideas

See [ROADMAP.md](ROADMAP.md) for planned hardening (encrypted remote shell, memory protection, privilege separation).
