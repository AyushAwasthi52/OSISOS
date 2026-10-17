#!/usr/bin/env python3
"""
Automated test of the OSISOS TCP/IP stack under QEMU (user-mode networking).

Boots build `kernel/osisos.bin` with `qemu -kernel`, logs in through the QEMU
monitor (sendkey), runs shell commands, reads the VGA text buffer back, and
talks to the guest's echo/HTTP servers from the host through port forwards.

    cd kernel && make osisos.bin && python3 ../tools/nettest.py
"""
import os, socket, subprocess, sys, threading, time, http.server, socketserver, urllib.request

KERNEL = os.path.join(os.path.dirname(__file__), "..", "kernel", "osisos.bin")
MON = "/tmp/osisos-mon.sock"
HOST_HTTP_PORT = 18000
FWD_HTTP, FWD_ECHO, FWD_UDP = 18080, 18007, 18008
NET = "192.168.76"; GW = NET + ".2"; GUEST = NET + ".50"   # DHCP pool differs from the 10.0.2.15 fallback on purpose
KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '\n': 'ret', '-': 'minus', '\b': 'backspace'}

class Qemu:
    def __init__(self):
        if os.path.exists(MON): os.remove(MON)
        self.p = subprocess.Popen([
            "qemu-system-i386", "-m", "32M", "-kernel", KERNEL, "-display", "none",
            "-monitor", f"unix:{MON},server,nowait",
            "-netdev", f"user,id=n0,net={NET}.0/24,dhcpstart={NET}.50,hostfwd=tcp::{FWD_HTTP}-:80,hostfwd=tcp::{FWD_ECHO}-:7,hostfwd=udp::{FWD_UDP}-:7",
            "-object", "filter-dump,id=f1,netdev=n0,file=/tmp/osisos.pcap",
            "-device", "e1000,netdev=n0,mac=52:54:00:12:34:56"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists(MON): break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(MON); time.sleep(0.3); self.s.recv(65536)
    def cmd(self, c):
        self.s.sendall((c + "\n").encode()); time.sleep(0.05)
        try: self.s.settimeout(0.3); self.s.recv(65536)
        except Exception: pass
    def type(self, text):
        for ch in text:
            self.cmd("sendkey " + KEYS.get(ch, ch)); time.sleep(0.03)
    def screen(self):
        f = "/tmp/osisos-vga.bin"
        if os.path.exists(f): os.remove(f)
        self.cmd('pmemsave 0xb8000 4000 "%s"' % f); time.sleep(0.2)
        d = open(f, "rb").read()
        return "\n".join("".join(chr(d[(r*80+c)*2]) if 32 <= d[(r*80+c)*2] < 127 else " "
                                 for c in range(80)).rstrip() for r in range(25))
    def run(self, line, wait=3.0, until=None):
        self.type(line + "\n"); end = time.time() + wait
        while time.time() < end:
            time.sleep(0.25)
            if until and until in self.screen(): break
        return self.screen()
    def close(self):
        self.p.kill()

passed = failed = 0
def check(name, cond, detail=""):
    global passed, failed
    if cond: passed += 1; print(f"  PASS  {name}")
    else:    failed += 1; print(f"  FAIL  {name}\n{detail}")

def main():
    # ---- host-side HTTP server (the guest connects OUT to 10.0.2.2:HOST_HTTP_PORT) ----
    BIG = ("0123456789" * 100 + "\n") * 6              # ~6 KB: forces several segments + window use
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = ("HELLO-FROM-HOST\n" + BIG).encode() if self.path == "/big" else b"HELLO-FROM-HOST\n"
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body)
        def log_message(self, *a): pass
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", HOST_HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # ---- raw TCP server on the host for nc: replies and then closes ----
    nc_srv = socket.socket(); nc_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    nc_srv.bind(("127.0.0.1", 15555)); nc_srv.listen(1); got = []
    def nc_thread():
        c, _ = nc_srv.accept(); c.settimeout(5); got.append(c.recv(100)); c.sendall(b"PONG-FROM-HOST\n"); c.close()
    threading.Thread(target=nc_thread, daemon=True).start()


    # ---- fake DNS server on the host (guest -> GW:53 -> SLIRP -> 127.0.0.1:53) ----
    dns_queries = []
    def label(n): return b"".join(bytes([len(p)]) + p.encode() for p in n.split(".")) + b"\0"
    def dns_thread():
        ds = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ds.bind(("127.0.0.1", 53)); ds.settimeout(0.5)
        while not stop.is_set():
            try: data, addr = ds.recvfrom(512)
            except socket.timeout: continue
            i = 12; name = []
            while data[i]: name.append(data[i+1:i+1+data[i]].decode()); i += 1 + data[i]
            name = ".".join(name); question = data[12:i+5]; dns_queries.append(name)
            hdr = data[:2]
            def a_rec(owner_ptr, ip): return owner_ptr + b"\0\1\0\1" + (60).to_bytes(4, "big") + b"\0\4" + bytes(map(int, ip.split(".")))
            if name == "osisos.test":
                resp = hdr + b"\x81\x80\0\1\0\1\0\0\0\0" + question + a_rec(b"\xc0\x0c", GW)         # compression pointer
            elif name == "alias.test":
                target = label("osisos.test")
                cname = b"\xc0\x0c\0\5\0\1" + (60).to_bytes(4, "big") + len(target).to_bytes(2, "big") + target
                resp = hdr + b"\x81\x80\0\1\0\2\0\0\0\0" + question + cname + a_rec(target, GW)   # CNAME then A
            else:
                resp = hdr + b"\x81\x83\0\1\0\0\0\0\0\0" + question                                  # NXDOMAIN
            ds.sendto(resp, addr)
    stop = threading.Event()
    threading.Thread(target=dns_thread, daemon=True).start()

    q = Qemu()
    try:
        time.sleep(3)
        scr = q.screen()
        check("boot + stack up message", "Network stack up" in scr, scr)
        q.type("admin\nosisos\n"); time.sleep(1.5)

        out = q.run("ifconfig", 1.5)
        check("DHCP: leased %s (not the 10.0.2.15 fallback)" % GUEST,
              "inet " + GUEST in out and "gw " + GW in out and "dhcp BOUND" in out and "dns " + NET + ".3" in out, out)

        out = q.run("ping " + GW, 8, until="seq=4")
        check("ICMP ping gateway (4/4 replies)", out.count("reply") >= 4, out)

        out = q.run("arp", 1.5)
        check("ARP cache has gateway", GW + "" in out, out)

        out = q.run("http %s %d /" % (GW, HOST_HTTP_PORT), 8, until="HELLO-FROM-HOST")
        check("TCP client: connect + HTTP GET (small)", "connected to " + GW in out and "HELLO-FROM-HOST" in out, out)

        out = q.run("nc %s 15555 ping" % GW, 8, until="PONG-FROM-HOST")
        check("TCP client: nc send+receive", "PONG-FROM-HOST" in out and got and got[0] == b"ping\n", out + str(got))

        out = q.run("http %s %d /big" % (GW, HOST_HTTP_PORT), 12, until="truncated")
        check("TCP client: multi-segment download (flow control)", "output truncated" in out and "0123456789" in out, out)

        out = q.run("nc %s 9 x" % GW, 8, until="refused")
        check("connection refused -> RST handled", "refused" in out, out)


        # ---- DNS (point the guest at the host-side fake server via the gateway address) ----
        q.run("dns " + GW, 1.5)
        out = q.run("nslookup osisos.test", 8, until="resolving osisos.test ... " + GW)
        check("DNS: A record with compression pointer", "osisos.test ... " + GW in out, out)
        out = q.run("nslookup alias.test", 8, until="alias.test ... " + GW)
        check("DNS: CNAME chain resolved", "alias.test ... " + GW in out, out)
        out = q.run("nslookup nope.test", 8, until="no such host")
        check("DNS: NXDOMAIN reported", "no such host" in out, out)
        out = q.run("ping osisos.test", 8, until="seq=4")
        check("ping by hostname", out.count("reply") >= 4, out)
        out = q.run("http osisos.test %d /" % HOST_HTTP_PORT, 8, until="HELLO-FROM-HOST")
        check("http by hostname", "HELLO-FROM-HOST" in out, out)
        check("DNS cache: repeated lookups did not re-query the server",
              dns_queries == ["osisos.test", "alias.test", "nope.test"], str(dns_queries))

        # ---- DHCP renewal: a unicast REQUEST from our leased address to the server ----
        q.run("dhcp renew", 4, until="BOUND")
        time.sleep(1.5)
        out = q.run("ifconfig", 2)
        check("DHCP renew: still BOUND with the same address", "inet " + GUEST in out and "dhcp BOUND" in out, out)
        dh = subprocess.run("tcpdump -nn -r /tmp/osisos.pcap 'udp port 67 or udp port 68' 2>/dev/null", shell=True, capture_output=True, text=True).stdout
        reqs = dh.count("BOOTP/DHCP, Request"); reps = dh.count("BOOTP/DHCP, Reply")
        check("DHCP on the wire: DISCOVER+REQUEST+renewal sent, OFFER+2xACK received; renewal is unicast",
              reqs >= 3 and reps >= 3 and (GUEST + ".68 > " + GW + ".67") in dh, "requests=%d replies=%d\n%s" % (reqs, reps, dh[-900:]))

        # ---- UDP server: echo a datagram through the hostfwd ----
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); us.settimeout(5)
        us.sendto(b"udp-echo-test-\x00\xff" + b"x" * 600, ("127.0.0.1", FWD_UDP))
        try: reply, _ = us.recvfrom(2000)
        except Exception as e: reply = repr(e).encode()
        check("UDP echo server (614-byte datagram)", reply == b"udp-echo-test-\x00\xff" + b"x" * 600, str(reply[:40]))

        # ---- server side: host connects into the guest through hostfwd ----
        time.sleep(0.5)
        try:
            body = urllib.request.urlopen(f"http://127.0.0.1:{FWD_HTTP}/", timeout=8).read().decode()
            check("TCP server: HTTP page served by the kernel", "Hello from the OSISOS kernel" in body, body)
        except Exception as e:
            check("TCP server: HTTP page served by the kernel", False, repr(e))

        try:
            c = socket.create_connection(("127.0.0.1", FWD_ECHO), timeout=8)
            payload = bytes(range(33, 127)) * 20            # ~1.9 KB, several segments
            c.sendall(payload); c.settimeout(8); buf = b""
            while len(buf) < len(payload): 
                d = c.recv(4096)
                if not d: break
                buf += d
            c.shutdown(socket.SHUT_WR)                      # FIN -> server must close too
            tail = c.recv(10); c.close()
            check("TCP server: echo round-trip (%d bytes)" % len(payload), buf == payload and tail == b"", str(len(buf)))
        except Exception as e:
            check("TCP server: echo round-trip", False, repr(e))

        # several quick connections to exercise teardown + socket reuse
        ok = 0
        for _ in range(15):
            try:
                if b"OSISOS" in urllib.request.urlopen(f"http://127.0.0.1:{FWD_HTTP}/", timeout=5).read(): ok += 1
            except Exception: pass
        check("15 sequential HTTP connections (no socket leak)", ok == 15, f"{ok}/15")

        # ---- reliability: random loss in both directions, data must still arrive intact ----
        q.run("netdrop 15", 1.5)
        try:
            payload = os.urandom(8000)
            c = socket.create_connection(("127.0.0.1", FWD_ECHO), timeout=60); c.settimeout(60)
            threading.Thread(target=lambda: c.sendall(payload), daemon=True).start()
            buf = b""
            while len(buf) < len(payload):
                d = c.recv(4096)
                if not d: break
                buf += d
            c.close()
            check("echo 8000 random bytes under 15% packet loss (retransmission)", buf == payload, f"{len(buf)} bytes")
        except Exception as e:
            check("echo under packet loss", False, repr(e))
        q.run("netdrop 0", 1.5)

        time.sleep(3)
        out = q.run("netstat", 1.5)
        check("netstat: only listeners left after teardown", out.count("ESTABLISHED") == 0 and "LISTEN" in out, out)
        out = q.run("ifconfig", 1.5)
        print("\n--- final screen ---\n" + out)
    finally:
        q.close()
    print(f"\n{passed} passed, {failed} failed"); sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
