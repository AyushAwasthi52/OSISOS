# User Guide

How to run, use and administer an OSISOS machine.

## 1. Running it

```sh
cd kernel
make                 # kernel/osisos.bin
make run-direct      # QEMU, boots the binary directly (no GRUB)
make run-user        # QEMU via the GRUB ISO (needs grub-mkrescue + xorriso); user-mode networking
make run / run2      # two instances joined by a virtual cable (see NETWORKING.md)
```

`run-direct` and `run-user` forward **host port 8080 -> guest HTTP (80)** and **8007 -> guest echo (7)**.
Add `-hda disk.img` for persistence (the Makefile targets create a 10 MB `drive.img`). A blank disk is
formatted automatically at first boot; an old v1 disk is upgraded in place.

Log in at the console: **admin / osisos**. On first boot the system warns you until you change that password
with `passwd`.

## 2. Command reference

Commands are lowercase (the keyboard driver has no Shift key yet). Arguments are separated by spaces.

### General

| Command | Description |
|---|---|
| `help` | list commands |
| `sysinfo` (`neofetch`) | logo + CPU, memory, uptime, network, disk, web and TCP statistics |
| `about` | one-screen summary |
| `date`, `uptime`, `free`, `ps` | time (RTC), uptime, memory, task list |
| `dmesg` | the last 20 kernel-log lines (DHCP, auth, httpd, rcon, panics) |
| `selftest` | checks ICMP/UDP/TCP over loopback, the HTTP server, filesystem, 100-file inode test, crypto, heap |
| `echo <text>`, `clear` | print text; clear the screen |
| `passwd` | change your password (current, new, confirm; 6-63 characters, input masked) |
| `logout` | back to the login prompt |
| `reboot`, `shutdown` | restart / power off |

### Files (OFS)

| Command | Description |
|---|---|
| `ls` | list the current directory |
| `cd <dir>`, `mkdir <dir>` | change / create a directory (`cd /` for the root) |
| `cat <file>` | print the first 511 bytes of a file |
| `edit <file>` | tiny editor for files up to 512 bytes |
| `rm <file>`, `rmdir <dir>` | delete a file / an *empty* directory |
| `mv <from> <to>` | rename or move (into `<to>` if it is a directory); never overwrites |
| `df` | data blocks used/free, files, directories, free inodes |
| `fetch <host> <port> <url-path> <ofs-path>` | download over HTTP into a file (up to 256 KB) |

Paths may be absolute (`/www/a.html`) or relative to the current directory; `.` and `..` are understood by
`rm`, `rmdir`, `mv`. Names are at most 21 characters.

### Network

| Command | Description |
|---|---|
| `ifconfig` | MAC, IP, mask, gateway, DNS, DHCP state, counters. `ifconfig <ip> <mask> <gw>` sets a static address (turns DHCP off) |
| `dhcp`, `dhcp renew` | lease status / renew now (or restart discovery) |
| `ping <host>` | 4 ICMP echoes; `<host>` is an IP, a DNS name, `localhost` or `127.0.0.1`. Plain `ping` sends the legacy raw demo frame |
| `nslookup <name>`, `dns [ip]` | resolve a name / show or set the DNS server |
| `arp` | ARP cache |
| `netstat` | TCP socket table: local/remote address, state, queued bytes |
| `nc <host> <port> [text]` | TCP client: connect, send `text`+newline, print the reply |
| `http <host> [port] [path]` | minimal HTTP/1.0 GET |
| `netdrop <0-90>` | randomly drop that percentage of IP frames (rx+tx) to watch TCP retransmit |
| `httpd`, `httpd root <dir>` | web server status, last 8 requests, document root (runtime only) |
| `rcon`, `rcon on`, `rcon off` | remote console status / enable (persistent) / disable |

### Fun and debug

`fortune`, `cowsay <text>`, `matrix` (any key quits), `snake` (WASD, Q quits), `sl`, and a few easter eggs
(try `sudo make me a sandwich`, `xyzzy`, `42`, `brew coffee`, or `/teapot` on the web server).
`crash null|div0|ud|gpf|page|panic` deliberately faults the kernel to show the panic screen (**debug only**).

## 3. Built-in services

| Port | Protocol | Service |
|---|---|---|
| 7 | TCP, UDP | echo |
| 80 | TCP | HTTP file server (documents from `/www`) |
| 2323 | TCP | remote console (**off by default**) |
| 68 | UDP | DHCP client |

## 4. Publishing a website

```
mkdir www
fetch 10.0.2.2 8000 /index.html /www/index.html     # e.g. python3 -m http.server 8000 on the host
cd www
mkdir img
fetch 10.0.2.2 8000 /logo.png /www/img/logo.png
```

Then browse to the guest (`http://127.0.0.1:8080/` with the QEMU forwards). Behaviour: `GET`/`HEAD` only;
`/dir` redirects to `/dir/`; a directory without `index.html` gets an automatic listing; `/_status` shows
statistics; content types come from the extension; with no `/www` a built-in welcome page appears.
`/etc` is never served. Limits: 128 inodes per disk, files up to 2 MB (256 KB via `fetch`).

## 5. Remote administration

The remote console gives you the same shell over the network.

1. At the **local console**: run `passwd` (remote logins are refused while the default password is set), then
   `rcon on`. The setting persists across reboots.
2. From another machine: `nc <ip> 2323` (or `telnet <ip> 2323`), then log in as `admin`.
3. Use commands normally; `exit` disconnects. Scripting works: `printf 'df\nifconfig\nexit\n' | nc <ip> 2323`
   (after login lines: `printf 'admin\nPASSWORD\ndf\nexit\n' | nc ...`).

Things to know:

* **The connection is not encrypted.** Use it only on networks you trust (see [SECURITY.md](SECURITY.md)).
* One session at a time; 5-minute idle timeout; 3 wrong passwords drop the connection and start a lockout that is
  *separate* from the local console's.
* Commands that need the screen or keyboard (`snake`, `matrix`, `sl`, `edit`, `passwd`, `clear`, `crash`) are refused remotely.
* Commands run on the console's shell task, so a long command (e.g. `fetch`) also occupies the local console.
* Every login, failure and command is recorded in `dmesg`.

## 6. Configuration and persistence

| Setting | Where | Persists? |
|---|---|---|
| Password hash | `/etc/shadow` | yes |
| Remote console on/off | `/etc/rcon` (present = on) | yes |
| IP address | DHCP, or `ifconfig` | no (DHCP re-runs each boot; static is lost) |
| DNS server | DHCP, or `dns <ip>` | no |
| Web document root | `httpd root <dir>` | no (default `/www`) |

Without a disk, the system still runs, but changed passwords and the console setting are lost at reboot (and it says so).

**Forgot the password?** There is no recovery account. Delete `/etc/shadow` from the disk image (or recreate
the image) and the default `admin`/`osisos` account is created again at the next boot.

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| `ifconfig` shows `0.0.0.0` | DHCP takes up to ~15 s; then the fallback `10.0.2.15` is used. `dhcp` shows the state. |
| Web page does not load | `httpd` (document root exists? requests counted?), `netstat` (is port 80 LISTEN?), QEMU port forward flags |
| `fetch` says "did not answer 200 OK" | wrong URL path / the server is not reachable from the guest (host = `10.0.2.2` in QEMU user networking) |
| Remote console: "Remote login is disabled until the default password is changed" | run `passwd` at the local console |
| Remote console: connection closes immediately | `rcon` at the console: is it enabled? |
| "Too many failed attempts. Try again in N s." | wait; lockouts double (5 s, 10 s ... max 5 min) |
| Kernel panic screen | note the cause and EIP; `R` reboots; see the developer guide |
| `qemu -kernel` refuses to boot | rebuild with the current Makefile (`--build-id=none` keeps the Multiboot header in the first 8 KB) |
| Strange behaviour after editing a header | the Makefile tracks header dependencies now; `make clean && make` if in doubt |
