#!/usr/bin/env python3
"""
Automated test of the OSISOS TCP/IP stack under QEMU (user-mode networking).

Boots build `kernel/osisos.bin` with `qemu -kernel`, logs in through the QEMU
monitor (sendkey), runs shell commands, reads the VGA text buffer back, and
talks to the guest's echo/HTTP servers from the host through port forwards.

    cd kernel && make osisos.bin && python3 ../tools/nettest.py
"""
import os, socket, subprocess, sys, threading, time, http.server, http.client, socketserver, urllib.request

KERNEL = os.path.join(os.path.dirname(__file__), "..", "kernel", "osisos.bin")
MON = "/tmp/osisos-mon.sock"
HOST_HTTP_PORT = 18000
DISK = "/tmp/osisos-test.img"
FWD_HTTP, FWD_ECHO, FWD_UDP = 18080, 18007, 18008
NET = "192.168.76"; GW = NET + ".2"; GUEST = NET + ".50"   # DHCP pool differs from the 10.0.2.15 fallback on purpose
KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '\n': 'ret', '-': 'minus', '\b': 'backspace'}

class Qemu:
    def __init__(self, disk=None):
        if os.path.exists(MON): os.remove(MON)
        self.p = subprocess.Popen([
            "qemu-system-i386", "-m", "32M", "-kernel", KERNEL, "-display", "none",
            "-monitor", f"unix:{MON},server,nowait",
            "-netdev", f"user,id=n0,net={NET}.0/24,dhcpstart={NET}.50,hostfwd=tcp::{FWD_HTTP}-:80,hostfwd=tcp::{FWD_ECHO}-:7,hostfwd=udp::{FWD_UDP}-:7",
            "-object", "filter-dump,id=f1,netdev=n0,file=/tmp/osisos.pcap",
            "-device", "e1000,netdev=n0,mac=52:54:00:12:34:56"] +
            (["-drive", f"file={disk},format=raw,if=ide,index=0"] if disk else []),
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
    import random
    rnd = random.Random(1234); DATA = bytes(rnd.getrandbits(8) for _ in range(20000)); HUGE = bytes(rnd.getrandbits(8) for _ in range(150000))
    BIG = ("0123456789" * 100 + "\n") * 6              # ~6 KB: forces several segments + window use
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            routes = {"/bin": DATA, "/huge": HUGE, "/page": b"<html><body>PAGE-INDEX</body></html>\n", "/small": b"note-in-docs\n"}
            if self.path in routes: body = routes[self.path]
            else: body = ("HELLO-FROM-HOST\n" + BIG).encode() if self.path == "/big" else b"HELLO-FROM-HOST\n"
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

    with open(DISK, "wb") as f: f.truncate(10 * 1024 * 1024)       # blank 10 MB disk: the kernel formats it on first boot
    q = Qemu(DISK)
    try:
        time.sleep(3)
        scr = q.screen()
        check("boot + stack up message", "Network stack up" in scr, scr)
        q.type("admin\nosisos\n"); time.sleep(1.5)

        out = q.run("ifconfig", 1.5)
        check("DHCP: leased %s (not the 10.0.2.15 fallback)" % GUEST,
              "inet " + GUEST in out and "gw " + GW in out and "dhcp BOUND" in out and "dns " + NET + ".3" in out, out)

        out = q.run("dmesg", 3)
        check("dmesg: DHCP lease and first-boot auth setup are in the kernel log", "dhcp: bound" in out and "auth: first boot" in out, out)
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


        # ================= selftest, loopback, system info, fun =================
        out = q.run("selftest", 30, until="checks passed")
        check("selftest: every check passes (ICMP/UDP/TCP loopback, HTTP, fs, heap)",
              "ALL GOOD" in out and "ICMP echo to 127.0.0.1" in out and "TCP echo, 3000 bytes" in out and "[FAIL]" not in out, out)
        out = q.run("ping 127.0.0.1", 8, until="seq=4")
        check("ping 127.0.0.1 (loopback)", out.count("reply") >= 4, out)
        out = q.run("nc localhost 7 hello-loop", 8, until="hello-loop")
        check("nc localhost 7: TCP over loopback to our own echo server", "connected to" in out and out.count("hello-loop") >= 2, out)
        out = q.run("sysinfo", 3, until="TCP:")
        check("sysinfo: shows CPU, memory, uptime, network, disk, web, tcp",
              all(k in out for k in ("admin@osisos", "CPU:", "Memory:", "Uptime:", "Network: " + GUEST, "Disk:    OFS v2", "Web:", "TCP:")), out)
        out = q.run("cowsay hi there", 2, until="||     ||")
        check("cowsay", "< hi there >" in out and "(oo)" in out, out)
        out = q.run("fortune", 2)
        check("fortune prints something", len(out.strip().splitlines()) > 5 and "unknown" not in out.lower(), out)
        out = q.run("sudo make me a sandwich", 2, until="Okay.")
        check("easter egg: sudo make me a sandwich", "Okay." in out, out)
        out = q.run("xyzzy", 2, until="Nothing happens")
        check("easter egg: xyzzy", "Nothing happens." in out, out)
        out = q.run("brew coffee", 2, until="teapot")
        check("easter egg: brew coffee -> 418", "418 I'm a teapot" in out, out)

        q.type("matrix\n"); time.sleep(3)
        scr = q.screen(); rain = sum(1 for ch in scr.replace("\n", "") if ch not in " ")
        q.type("x"); time.sleep(1.2)
        back = q.screen()
        check("matrix: rain animates, any key restores the shell screen", rain > 120 and "matrix" in back and "any key to leave" not in back, f"rain={rain}\n{back}")
        out = q.run("fortune", 2)
        check("shell still works after matrix", len(out.strip().splitlines()) > 5, out)

        q.type("snake\n"); time.sleep(1.5)
        scr = q.screen()
        started = "SNAKE" in scr and "score:" in scr
        time.sleep(7)                                     # it heads straight for the wall
        scr2 = q.screen()
        check("snake: runs, and ends with GAME OVER when it hits the wall", started and "G A M E" in scr2, scr + "\n----\n" + scr2)
        q.type("z"); time.sleep(1.2)
        check("snake: a key press returns to the shell", "G A M E" not in q.screen())
        q.type("snake\n"); time.sleep(1.5); q.type("w"); time.sleep(0.5); q.type("q"); time.sleep(1.2)
        check("snake: steer with W, quit with Q", "SNAKE" not in q.screen() and "snake" in q.screen())

        out = q.run("sl", 8, until="you meant ls")
        check("sl: the train arrives and leaves", "you meant ls" in out, out)

        # ================= HTTP file server (OFS-backed) =================
        def wait_saved(cmd, n):
            want = "saved %d bytes" % n                      # match THIS command's result, not an older "saved" line
            o = q.run(cmd, 40, until=want); return want in o, o
        q.run("mkdir www", 1.5)
        ok, o = wait_saved("fetch %s %d /bin /www/data.bin" % (GW, HOST_HTTP_PORT), 20000)
        check("fetch: 20000-byte binary downloaded into OFS (40 sectors)", ok, o)
        ok, o = wait_saved("fetch %s %d /page /www/index.html" % (GW, HOST_HTTP_PORT), 37)
        check("fetch: index.html saved", ok, o)
        q.run("cd www", 1.2); q.run("mkdir docs", 1.2); q.run("cd /", 1.2)
        ok, o = wait_saved("fetch %s %d /small /www/docs/note.txt" % (GW, HOST_HTTP_PORT), 13)
        check("fetch into a subdirectory by absolute path", ok, o)
        ok, o = wait_saved("fetch %s %d /huge /www/huge.bin" % (GW, HOST_HTTP_PORT), 150000)
        check("fetch: 150000-byte file (293 sectors)", ok, o)

        def get(path, method="GET", body=None):
            c = http.client.HTTPConnection("127.0.0.1", FWD_HTTP, timeout=30)
            c.request(method, path, body=body); r = c.getresponse(); data = r.read(); hdrs = dict(r.getheaders()); c.close()
            return r.status, hdrs, data
        def raw(req, wait=10):
            s = socket.create_connection(("127.0.0.1", FWD_HTTP), timeout=wait); s.sendall(req); out = b""
            try:
                while True:
                    d = s.recv(4096)
                    if not d: break
                    out += d
            except Exception: pass
            s.close(); return out

        st, h, body = get("/data.bin")
        check("GET file: 20000 bytes byte-identical, correct headers",
              st == 200 and body == DATA and h.get("Content-Length") == "20000" and h.get("Content-Type") == "application/octet-stream", f"{st} {len(body)} {h}")
        st, h, body = get("/huge.bin")
        check("GET 150000-byte file byte-identical (streamed from disk)", st == 200 and body == HUGE, f"{st} {len(body)}")
        st, h, body = get("/")
        check("GET / serves index.html (text/html)", st == 200 and b"PAGE-INDEX" in body and h.get("Content-Type") == "text/html", f"{st} {h} {body[:60]}")
        st, h, body = get("/data.bin", "HEAD")
        check("HEAD: headers only, no body", st == 200 and body == b"" and h.get("Content-Length") == "20000", f"{st} {h} {len(body)}")
        st, h, body = get("/nope.txt")
        check("404 for a missing file", st == 404, str(st))
        st, h, body = get("/", "POST", b"x=1")
        check("405 + Allow header for POST", st == 405 and "GET" in h.get("Allow", ""), f"{st} {h}")
        st, h, body = get("/docs")
        check("301 redirect /docs -> /docs/", st == 301 and h.get("Location") == "/docs/", f"{st} {h}")
        st, h, body = get("/docs/")
        check("directory listing (no index.html)", st == 200 and b"note.txt" in body and b"13 bytes" in body, f"{st} {body[:200]}")
        st, h, body = get("/docs/note.txt")
        check("GET nested file (text/plain)", st == 200 and body == b"note-in-docs\n" and h.get("Content-Type") == "text/plain", f"{st} {h} {body}")
        for evil in (b"GET /../../etc/passwd HTTP/1.0\r\n\r\n", b"GET /%2e%2e/data.bin HTTP/1.0\r\n\r\n",
                     b"GET /docs/..%2fdata.bin HTTP/1.0\r\n\r\n", b"GET /a\\b HTTP/1.0\r\n\r\n", b"GET /x%zz HTTP/1.0\r\n\r\n"):
            r = raw(evil)
            check("path traversal / bad escape rejected: " + evil[4:24].decode(errors="replace").strip(), r.startswith(b"HTTP/1.1 400") or r.startswith(b"HTTP/1.1 404"), repr(r[:80]))
        check("garbage request line -> 400", raw(b"GARBAGE\r\n\r\n").startswith(b"HTTP/1.1 400"))
        check("oversized headers -> 431", raw(b"GET / HTTP/1.1\r\nX: " + b"a" * 2000).startswith(b"HTTP/1.1 431"))
        st, h, body = get("/_status")
        check("/_status reports statistics", st == 200 and b"requests:" in body and b"docroot: /www" in body, body.decode(errors="replace")[:200])

        # parallel clients: 6 simultaneous 20 KB downloads
        results = []
        def dl(): 
            try: results.append(get("/data.bin")[2] == DATA)
            except Exception: results.append(False)
        ths = [threading.Thread(target=dl) for _ in range(6)]
        [x.start() for x in ths]; [x.join(60) for x in ths]
        check("6 parallel downloads all byte-identical", results.count(True) == 6, str(results))

        out = q.run("httpd", 2)
        check("httpd shell command shows stats + access log", "document root /www" in out and "GET /data.bin -> 200" in out, out)

        # ---- reboot with the same disk: everything must still be there ----
        q.close(); time.sleep(1)
        q = Qemu(DISK); time.sleep(4)
        st, h, body = get("/data.bin")
        check("after REBOOT: file still byte-identical (persisted on disk)", st == 200 and body == DATA, f"{st} {len(body)}")
        st, h, body = get("/docs/note.txt")
        check("after REBOOT: nested file intact", st == 200 and body == b"note-in-docs\n", f"{st} {body}")
        st, h, body = get("/huge.bin")
        check("after REBOOT: 150000-byte file intact", st == 200 and body == HUGE, f"{st} {len(body)}")
        # a file created after the reboot must not overwrite old data (allocator restored correctly)
        q.type("admin\nosisos\n"); time.sleep(1.5)
        ok, o = wait_saved("fetch %s %d /small /www/second.txt" % (GW, HOST_HTTP_PORT), 13)
        st, h, body = get("/data.bin"); st2, h2, body2 = get("/second.txt")
        check("after REBOOT: new file does not clobber old ones", ok and body == DATA and body2 == b"note-in-docs\n", o)


        # ================= filesystem management: mv / rm / rmdir / df, space reuse =================
        out = q.run("df", 2, until="inodes free")
        check("df: reports OFS v2 usage", "OFS v2" in out and "inodes free" in out and "/128" in out, out)

        q.run("mv /www/second.txt /www/docs", 2)             # into an existing directory
        st, h, body = get("/docs/second.txt")
        st2, _, _ = get("/second.txt")
        check("mv into a directory: new path serves the file, old path is 404", st == 200 and body == b"note-in-docs\n" and st2 == 404, f"{st} {st2}")
        q.run("mv /www/docs/second.txt /www/docs/renamed.txt", 2)
        check("mv rename in place", get("/docs/renamed.txt")[0] == 200 and get("/docs/second.txt")[0] == 404)
        out = q.run("mv /www /www/docs", 2, until="failed")
        check("mv refuses to move a directory into itself", "failed" in out and get("/data.bin")[0] == 200, out)
        out = q.run("rmdir /www/docs", 2, until="cannot remove")
        check("rmdir refuses a non-empty directory", "cannot remove" in out, out)
        q.run("rm /www/docs/renamed.txt", 2)
        check("rm deletes a file (now 404)", get("/docs/renamed.txt")[0] == 404)

        # churn: delete a hole, fill around it, nothing may overlap
        wait_saved("fetch %s %d /bin /www/a.bin" % (GW, HOST_HTTP_PORT), 20000)
        wait_saved("fetch %s %d /bin /www/b.bin" % (GW, HOST_HTTP_PORT), 20000)
        q.run("rm /www/a.bin", 2)                           # leaves a 40-block hole before b.bin
        ok1, o1 = wait_saved("fetch %s %d /small /www/c.txt" % (GW, HOST_HTTP_PORT), 13)   # should reuse the hole
        ok2, o2 = wait_saved("fetch %s %d /huge /www/d.bin" % (GW, HOST_HTTP_PORT), 150000)  # too big for the hole
        sa, _, ba = get("/a.bin"); sb, _, bb = get("/b.bin"); sc, _, bc = get("/c.txt"); sd, _, bd = get("/d.bin")
        check("space reuse after delete: all surviving files byte-identical (no overlap)",
              ok1 and ok2 and sa == 404 and bb == DATA and bc == b"note-in-docs\n" and bd == HUGE and get("/data.bin")[2] == DATA,
              f"{sa} b_ok={bb == DATA} c={bc[:20]} d_ok={bd == HUGE}")
        out = q.run("df", 2, until="inodes free")
        check("df: free space accounts for deletions", "used" in out and "free" in out, out)
        q.run("rm /www/d.bin", 2); q.run("rm /www/b.bin", 2); q.run("rm /www/c.txt", 2)

        # the rm -rf / joke must not delete anything
        out = q.run("rm -rf /", 8, until="Just kidding")
        check("easter egg: rm -rf / is only a joke (nothing deleted)", "Just kidding" in out and get("/data.bin")[2] == DATA and get("/huge.bin")[2] == HUGE, out)

        st, h, body = get("/teapot")
        check("HTTP 418 I'm a teapot", st == 418 and b"teapot" in body, f"{st} {body[:60]}")

        out = q.run("dmesg", 3, until="httpd:")
        check("dmesg: shows recent httpd requests (last 20 lines)", "httpd: " in out and "GET /teapot" in out, out)
        out = q.run("dmesg", 3)
        check("dmesg: logs httpd requests", "httpd: " in out and "GET /" in out, out)


        # ================= crash handling: panic screen, reboot, recovery =================
        def relogin(wait=4.5):
            time.sleep(wait); q.type("admin\nosisos\n"); time.sleep(1.5)

        cases = (("null", ["Page Fault", "NULL pointer dereference", "write, kernel mode", "CR2=00000000"]),
                 ("div0", ["Divide Error", "Division by zero"]),
                 ("ud",   ["Invalid Opcode", "cannot execute"]),
                 ("gpf",  ["General Protection Fault"]),
                 ("page", ["Page Fault", "unmapped address", "1F000000"]),
                 ("panic", ["manual panic requested"]))
        for kind, expect in cases:
            q.type("crash %s\n" % kind); time.sleep(2.5); s = q.screen()
            need = ["K E R N E L   P A N I C", "Rebooting in", "Uptime:", "Last kernel log lines"] + expect + (["EIP="] if kind != "panic" else [])
            q.type("r"); time.sleep(4.5)
            check("panic screen: crash %s (decoded cause, registers, log) and R reboots" % kind,
                  all(n in s for n in need) and "login:" in q.screen(), s)
            q.type("admin\nosisos\n"); time.sleep(1.5)

        q.type("crash null\n"); time.sleep(2.5); q.type("h"); time.sleep(3)
        s = q.screen(); time.sleep(18)                    # longer than the 15 s countdown
        s2 = q.screen()
        check("panic screen: H halts and keeps the screen (no reboot after 20 s)", "System halted" in s and "System halted" in s2 and "login:" not in s2, s2)

        # leave the halted machine: reset it through the QEMU monitor, then test the AUTOMATIC reboot path
        q.cmd("system_reset"); relogin(5)
        q.type("crash null\n"); time.sleep(2.5)
        shown = "K E R N E L   P A N I C" in q.screen()
        time.sleep(17)                                    # 15 s countdown + reboot
        back = q.screen()
        check("automatic reboot after the 15 s countdown", shown and "login:" in back and "P A N I C" not in back, back)
        time.sleep(4)
        st, h, body = get("/data.bin")
        check("recovery: web server is back after the crash and the DISK is intact (20000 B identical)", st == 200 and body == DATA, f"{st} {len(body)}")
        st, h, body = get("/huge.bin")
        check("recovery: 150000-byte file still byte-identical", st == 200 and body == HUGE, f"{st} {len(body)}")
        q.type("admin\nosisos\n"); time.sleep(1.5)
        out = q.run("selftest", 30, until="checks passed")
        check("recovery: selftest passes after a crash/reboot cycle", "ALL GOOD" in out, out)

        time.sleep(3)
        out = q.run("netstat", 1.5)
        check("netstat: only listeners left after teardown", out.count("ESTABLISHED") == 0 and "LISTEN" in out, out)
        out = q.run("ifconfig", 1.5)
        print("\n--- final screen ---\n" + out)

        # ================= OFS v1 -> v2 migration (a disk made by the OLD kernel) =================
        import struct
        v1 = bytearray(10 * 1024 * 1024)
        v1[0:12] = struct.pack("<III", 0x05150515, 32, 20480)
        def inode(name, typ, parent, size, start):
            return name.encode().ljust(22, b"\0") + struct.pack("<BBII", typ, parent, size, start)
        HELLO = b"Welcome to the OSISOS File System!"
        NOTE = bytes((i * 31 + 7) & 255 for i in range(700))                # 2 sectors
        table = inode("hello.txt", 0, 255, len(HELLO), 3) + inode("docs", 1, 255, 0, 0) + inode("note.txt", 0, 1, len(NOTE), 4)
        v1[512:512 + len(table)] = table
        v1[3 * 512:3 * 512 + len(HELLO)] = HELLO
        v1[4 * 512:4 * 512 + len(NOTE)] = NOTE
        V1DISK = "/tmp/osisos-v1.img"
        open(V1DISK, "wb").write(v1)

        q.close(); time.sleep(1.5)                         # free the forwarded ports
        q = Qemu(V1DISK); time.sleep(4)
        boot = q.screen()
        check("v1 disk: migrated automatically at boot, nothing wiped", "upgraded disk v1 -> v2 (2 files kept" in boot, boot)
        q.type("admin\nosisos\n"); time.sleep(1.5)
        q.run("httpd root /", 2)
        st, h, body = get("/hello.txt"); st2, h2, body2 = get("/docs/note.txt")
        check("v1 disk: old files read back byte-identical from their new location", st == 200 and body == HELLO and st2 == 200 and body2 == NOTE, f"{st} {body} {st2} {len(body2)}")
        st3, _, _ = get("/etc/shadow"); st4, _, lst = get("/")
        check("httpd never serves /etc (password hashes) even with the docroot set to /", st3 == 404 and b"etc" not in lst and b"hello.txt" in lst, f"{st3} {lst[:200]}")
        out = q.run("df", 2, until="inodes free")
        check("v1 disk: now 128 inodes (2 migrated files + docs dir + the new /etc/shadow)", "files: 3" in out and "dirs: 2" in out and "/128" in out, out)
        ok, o = wait_saved("fetch %s %d /small /second.txt" % (GW, HOST_HTTP_PORT), 13)
        check("v1 disk: new files can be written beside migrated ones", ok and get("/docs/note.txt")[2] == NOTE and get("/second.txt")[2] == b"note-in-docs\n", o)
        q.close(); time.sleep(1)
        q = Qemu(V1DISK); time.sleep(4)
        boot = q.screen(); q.type("admin\nosisos\n"); time.sleep(1.5); q.run("httpd root /", 2)
        check("v1 disk: second boot is plain v2 (no migration again) and data persists",
              "upgraded" not in boot and get("/hello.txt")[2] == HELLO and get("/docs/note.txt")[2] == NOTE and get("/second.txt")[0] == 200, boot)
        q.close()


        # ================= authentication: hashed passwords, throttling, passwd =================
        import hashlib, re
        AUTHDISK = "/tmp/osisos-auth.img"
        with open(AUTHDISK, "wb") as f: f.truncate(10 * 1024 * 1024)
        try: q.close()
        except Exception: pass
        time.sleep(1.5)
        q = Qemu(AUTHDISK); time.sleep(4)

        def login(user, pw, wait=2.5):
            q.type(user + "\n"); time.sleep(0.4); q.type(pw + "\n"); time.sleep(wait); return q.screen()
        def shadow_entry():
            data = open(AUTHDISK, "rb").read()
            m = re.search(rb"admin:(\d+):([0-9a-f]{32}):([0-9a-f]{64}):([01])\n", data)
            return (int(m.group(1)), m.group(2).decode(), m.group(3).decode(), m.group(4).decode()) if m else None

        s = login("admin", "osisos")
        check("auth: first boot - default admin/osisos works and warns to change it", "Login successful" in s and "default password" in s, s)
        out = q.run("dmesg", 3, until="auth:")
        check("auth: first boot creates the account and says so in the kernel log",
              "auth: first boot, created default admin account" in out, out)

        e1 = shadow_entry()
        check("auth: /etc/shadow on disk holds a salted PBKDF2 hash that matches Python's hashlib",
              e1 is not None and e1[0] == 4096 and e1[3] == "1" and
              hashlib.pbkdf2_hmac("sha256", b"osisos", bytes.fromhex(e1[1]), 4096, 32).hex() == e1[2], str(e1))
        check("auth: the plaintext password is not on the disk", b"admin:osisos" not in open(AUTHDISK, "rb").read())

        q.run("logout", 1.5)
        s = login("admin", "wrongpass1")
        check("auth: wrong password rejected", "Login incorrect" in s and "Login successful" not in s, s)
        s = login("nobody", "osisos")
        check("auth: unknown user rejected (same message)", "Login incorrect" in s and "Login successful" not in s, s)
        s = login("a" * 70, "b" * 70, 3)
        check("auth: 70-char user/password handled safely (the old code overflowed 32-byte buffers); 3rd failure locks out",
              "Login incorrect" in s and "Try again in" in s, s)
        s = login("admin", "osisos", 2)
        check("auth: the CORRECT password is refused while locked out", "Too many failed attempts" in s and "Login successful" not in s, s)
        time.sleep(6)
        s = login("admin", "osisos", 2.5)
        check("auth: login works again once the lockout expires", "Login successful" in s, s)

        # ---- passwd ----
        q.type("passwd\n"); time.sleep(1); q.type("wrongone1\n"); time.sleep(2.5)
        check("passwd: wrong current password refused", "Authentication failed" in q.screen(), q.screen())
        q.type("passwd\n"); time.sleep(1); q.type("osisos\n"); time.sleep(2.5); q.type("abc\n"); time.sleep(0.8)
        check("passwd: too-short password refused", "6 to 63" in q.screen(), q.screen())
        q.type("passwd\n"); time.sleep(1); q.type("osisos\n"); time.sleep(2.5); q.type("s3cretpw9\n"); time.sleep(0.8); q.type("different99\n"); time.sleep(0.8)
        check("passwd: mismatched confirmation refused", "do not match" in q.screen(), q.screen())
        q.type("passwd\n"); time.sleep(1); q.type("osisos\n"); time.sleep(2.5); q.type("s3cretpw9\n"); time.sleep(0.8); q.type("s3cretpw9\n"); time.sleep(0.8)
        s = q.screen()
        check("passwd: password changed, input was masked", "Password updated." in s and "s3cretpw9" not in s and "********" in s, s)

        e2 = shadow_entry()
        check("auth: new hash on disk is correct PBKDF2 of the new password, with a FRESH salt, default flag cleared",
              e2 is not None and e2[1] != e1[1] and e2[2] != e1[2] and e2[3] == "0" and
              hashlib.pbkdf2_hmac("sha256", b"s3cretpw9", bytes.fromhex(e2[1]), 4096, 32).hex() == e2[2], str(e2))

        q.run("logout", 1.5)
        s = login("admin", "osisos")
        check("auth: the OLD password no longer works", "Login incorrect" in s and "Login successful" not in s, s)
        s = login("admin", "s3cretpw9", 2.5)
        check("auth: the new password works and no default-password warning any more",
              "Login successful" in s and "default password" not in s, s)

        out = q.run("dmesg", 3, until="password changed")
        check("auth: failures, lockout and password change are in the kernel log",
              "auth: FAILED login" in out and "locked for 5 s" in out and "password changed for 'admin' (saved)" in out, out)

        # ---- persistence across a reboot ----
        q.type("reboot\n"); time.sleep(6)
        s = login("admin", "osisos")
        ok_old_rejected = "Login incorrect" in s and "Login successful" not in s
        s = login("admin", "s3cretpw9", 2.5)
        check("auth: after a reboot the changed password persists (old rejected, new accepted)",
              ok_old_rejected and "Login successful" in s and "default password" not in s, s)
        out = q.run("dmesg", 3, until="auth:")
        check("auth: existing /etc/shadow is loaded (not recreated) on the next boot", "auth: loaded /etc/shadow" in out, out)
        q.close()

    finally:
        q.close()
    print(f"\n{passed} passed, {failed} failed"); sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
