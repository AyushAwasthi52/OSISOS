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
| Remote console | **off by default** (both modes); enabling is a local, persistent, deliberate act; refused until the default password is changed; one session; 5-minute idle timeout; 3 failures drop the connection; denied interactive commands; no output leaks to the local screen |
| **Encrypted console** (`rcon on`, port 2324) | **Noise `NK`** (X25519 + ChaCha20-Poly1305 + SHA-256): the server is authenticated by a **pinned public key** (a man-in-the-middle cannot complete the handshake), traffic is encrypted and integrity-protected with **forward-secret** session keys, replayed/forged/reordered frames kill the session, handshake must finish in 10 s, frame sizes are bounded. The password is sent only inside the encrypted channel |
| Cryptography assurance | every primitive is checked against independent implementations (`tools/crypto_verify.py`, 1682 checks: Python `hashlib`, `pyca/cryptography`, and the `noiseprotocol` library as the *client* of our C server) plus known-answer tests inside `selftest` |
| Audit | logins, failures, lockouts, remote commands, httpd requests, password changes -> `dmesg` (attacker-supplied names sanitised and truncated) |
| Web server | only GET/HEAD; request/target size limits; `.`/`..`, backslash, control characters and bad escapes rejected before any filesystem access; `/etc` (credentials) never served, even with document root `/`; 30 s idle timeout |
| Network input | all headers/lengths/checksums validated; TCP resets need an exact sequence match (blind-reset resistance); DNS replies need the right server, port and id; ARP cache not populated by strangers |
| Kernel | all CPU exceptions handled (no silent triple-fault); page 0 unmapped so NULL dereferences fault; bounded disk waits; bounded copies (the login buffer overflow was fixed) |
| Disk | data written before metadata is published; migrations never wipe a disk |

## Known weaknesses (read before deploying)

1. **The legacy plaintext console (`rcon plain on`, port 2323) is unencrypted.** Anyone who can sniff the path can read the password
   and commands. It exists for `nc`/`telnet` convenience on trusted links: prefer `rcon on` (encrypted, port 2324). Both are off by default.
2. **No isolation inside the machine.** Everything runs in ring 0; the 32 MB identity map is writable (and executable) for all code.
   The ring-3 demo can overwrite the kernel. There is no per-process address space, no W^X, no stack canaries.
3. **One account, no privileges.** Anyone who logs in can do anything, including `crash`, `reboot`, and overwriting `/etc/shadow` with `fetch`.
4. **Default password window.** Until the first `passwd`, the console password is public. (Remote access stays closed in that window.)
5. **Limited entropy.** A bare VM has no hardware RNG. The pool (`core/rng.c`) is fed with TSC jitter at every keystroke and received frame
   plus tick counts, and is used for password salts, the console identity key and session keys; keys are generated on demand
   (`rcon on`), after the administrator has been typing and the NIC has seen traffic, which is when the pool is best seeded - but a freshly booted,
   idle machine has little real entropy. TCP initial sequence numbers and DNS ids still use a cheaper TSC/tick mix (unique, not unpredictable):
   an on-path attacker can plausibly guess them.
5b. **Encrypted-console specifics.** The client is anonymous at the Noise layer (users authenticate by password inside the channel, throttled
   like the console); the server's **private key sits unencrypted in `/etc/rcon.key`** (anyone with the disk has it, as with `/etc/shadow`);
   there is no automatic key rotation (`rcon newkey` does it by hand); the implementation (TweetNaCl-style X25519, 8-bit-limb Poly1305) is
   constant-time in structure but **not audited** and not hardened against fault or power attacks. Pin the key by comparing the
   fingerprint shown on the physical console - a first-use ("trust on first connect") model would be vulnerable to a MITM at that moment.
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
* Leave `rcon` off unless you need it; use `rcon on` (encrypted) rather than `rcon plain on`, and turn it off when finished (`rcon off`).
* Pin the server key out-of-band: read `rcon key` from the local console and pass it with `--server-key`.
* Watch `dmesg` for `auth: FAILED login` and `rcon:` lines.

## Reporting and hardening ideas

See [ROADMAP.md](ROADMAP.md) for planned hardening (memory protection, privilege separation, client authentication keys).
