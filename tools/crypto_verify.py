#!/usr/bin/env python3
"""
crypto_verify.py - verify the kernel's cryptography against INDEPENDENT implementations.

The kernel's SHA-256 / HMAC / PBKDF2 / X25519 / ChaCha20-Poly1305 / Noise NK code (kernel/core/*.c) is
freestanding, so the very same source files are compiled natively here with small test drivers
(tools/crypto/*.c) and compared with:

    Python hashlib / hmac            SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256
    pyca/cryptography                X25519 (incl. the RFC 7748 vector), ChaCha20-Poly1305
    noiseprotocol (pip)              the Noise_NK_25519_ChaChaPoly_SHA256 handshake and transport,
                                     with the independent library as the CLIENT and our C code as the SERVER

    pip install cryptography noiseprotocol
    python3 tools/crypto_verify.py          (needs gcc; exit status 1 on any mismatch)
"""
import hashlib, hmac, os, random, string, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, "..", "kernel", "core")
DRV = os.path.join(HERE, "crypto")
BIN = tempfile.mkdtemp(prefix="osisos-crypto-")
ok = bad = 0

def check(name, cond):
    global ok, bad
    if cond: ok += 1
    else: bad += 1; print("  MISMATCH:", name)

def build(out, driver, *sources):
    cmd = ["gcc", "-O2", "-Wall", "-I", CORE, "-o", os.path.join(BIN, out), os.path.join(DRV, driver)] + [os.path.join(CORE, s) for s in sources]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: sys.exit("build failed:\n" + r.stderr)

def run(binary, *args):
    return subprocess.run([os.path.join(BIN, binary), *map(str, args)], capture_output=True, text=True).stdout.strip()

def main():
    build("sha", "sha_driver.c", "sha256.c")
    build("prim", "prim_driver.c", "x25519.c", "chacha20poly1305.c")
    build("noise", "noise_driver.c", "noise.c", "x25519.c", "chacha20poly1305.c", "sha256.c")
    rnd = random.Random(7)
    rs = lambda k: "".join(rnd.choice(string.ascii_letters + string.digits + " !@#") for _ in range(k))

    # --- SHA-256 / HMAC / PBKDF2 ---
    for L in [1, 2, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 300, 1000]:       # block boundaries
        s = rs(L); check("sha256 len %d" % L, run("sha", "sha", s) == hashlib.sha256(s.encode()).hexdigest())
    for kl, ml in [(5, 5), (20, 100), (64, 10), (65, 10), (100, 200)]:
        k, m = rs(kl), rs(ml); check("hmac %d/%d" % (kl, ml), run("sha", "hmac", k, m) == hmac.new(k.encode(), m.encode(), hashlib.sha256).hexdigest())
    for p, s, c in [("password", "salt", 1), ("password", "salt", 2), ("password", "salt", 4096), ("osisos", "0123456789abcdef", 4096), ("a" * 70, "x" * 16, 100)]:
        check("pbkdf2 c=%d" % c, run("sha", "pbkdf2", p, s, c) == hashlib.pbkdf2_hmac("sha256", p.encode(), s.encode(), c, 32).hex())
    print("  SHA-256 / HMAC / PBKDF2 vs hashlib: done")

    # --- X25519 / ChaCha20-Poly1305 ---
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    from cryptography.hazmat.primitives import serialization as S
    raw = lambda k: k.public_bytes(S.Encoding.Raw, S.PublicFormat.Raw)
    k = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"; u = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"
    check("X25519 RFC 7748 vector", run("prim", "x25519", k, u) == "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
    for _ in range(40):
        priv = os.urandom(32); peer = raw(X25519PrivateKey.from_private_bytes(os.urandom(32)).public_key())
        check("x25519 shared secret", run("prim", "x25519", priv.hex(), peer.hex()) == X25519PrivateKey.from_private_bytes(priv).exchange(X25519PublicKey.from_public_bytes(peer)).hex())
        check("x25519 public key", run("prim", "base", priv.hex()) == raw(X25519PrivateKey.from_private_bytes(priv).public_key()).hex())
    for L in [0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 200, 500, 1000, 1100]:
        for adl in [0, 1, 16, 20, 33]:
            key, nonce, ad, pt = os.urandom(32), rnd.getrandbits(64), os.urandom(adl), os.urandom(L)
            ref = ChaCha20Poly1305(key).encrypt(b"\0\0\0\0" + nonce.to_bytes(8, "little"), pt, ad)
            check("aead encrypt L=%d ad=%d" % (L, adl), run("prim", "enc", key.hex(), nonce, ad.hex(), pt.hex()) == ref.hex())
            back = run("prim", "dec", key.hex(), nonce, ad.hex(), ref.hex())
            check("aead decrypt L=%d ad=%d" % (L, adl), back == pt.hex())
            bad_ct = bytearray(ref); bad_ct[len(bad_ct) // 2] ^= 1
            check("aead tamper rejected L=%d" % L, run("prim", "dec", key.hex(), nonce, ad.hex(), bytes(bad_ct).hex()) == "FAIL")
    print("  X25519 / ChaCha20-Poly1305 vs pyca/cryptography: done")

    # --- Noise NK: independent library as client, our C code as server ---
    from noise.connection import NoiseConnection, Keypair
    class Server:
        def __init__(self, priv, prologue):
            self.p = subprocess.Popen([os.path.join(BIN, "noise"), priv.hex(), prologue.hex()], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        def cmd(self, c, data):
            self.p.stdin.write("%s %s\n" % (c, data.hex())); self.p.stdin.flush()
            r = self.p.stdout.readline().split()
            return None if r[0] == "err" else bytes.fromhex(r[1]) if len(r) > 1 else b""
        def close(self): self.p.stdin.close(); self.p.wait()
    def client(pub, prologue):
        c = NoiseConnection.from_name(b"Noise_NK_25519_ChaChaPoly_SHA256"); c.set_as_initiator()
        c.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, pub); c.set_prologue(prologue); c.start_handshake(); return c
    for _ in range(20):
        priv = os.urandom(32); pub = raw(X25519PrivateKey.from_private_bytes(priv).public_key()); pro = os.urandom(rnd.choice([0, 1, 13, 32, 70]))
        s = Server(priv, pro); c = client(pub, pro)
        m2 = s.cmd("m1", c.write_message()); check("noise: server accepts message 1", m2 is not None and len(m2) == 48)
        if m2 is None: s.close(); continue
        c.read_message(m2); check("noise: handshake completes", c.handshake_finished)
        for _ in range(30):
            pt = os.urandom(rnd.choice([0, 1, 15, 16, 17, 100, 500, 1000])); check("noise: client->server", s.cmd("dec", c.encrypt(pt)) == pt)
            pt2 = os.urandom(rnd.choice([0, 1, 64, 300])); check("noise: server->client", c.decrypt(s.cmd("enc", pt2)) == pt2)
        ct = c.encrypt(b"replay"); check("noise: first delivery", s.cmd("dec", ct) == b"replay"); check("noise: replay rejected", s.cmd("dec", ct) is None)
        t = bytearray(c.encrypt(b"tamper")); t[3] ^= 0x40; check("noise: tamper rejected", s.cmd("dec", bytes(t)) is None)
        s.close()
    priv = os.urandom(32); pub = raw(X25519PrivateKey.from_private_bytes(priv).public_key())
    s = Server(priv, b"good"); check("noise: wrong prologue rejected", s.cmd("m1", client(pub, b"evil").write_message()) is None); s.close()
    s = Server(priv, b"p"); check("noise: wrong pinned server key rejected", s.cmd("m1", client(raw(X25519PrivateKey.generate().public_key()), b"p").write_message()) is None); s.close()
    s = Server(priv, b"p"); m = bytearray(client(pub, b"p").write_message()); m[40] ^= 1; check("noise: bit flip in message 1 rejected", s.cmd("m1", bytes(m)) is None); s.close()
    s = Server(priv, b"p"); check("noise: truncated message 1 rejected", s.cmd("m1", client(pub, b"p").write_message()[:47]) is None); s.close()
    s = Server(priv, b"p"); check("noise: all-zero ephemeral key rejected", s.cmd("m1", bytes(32) + os.urandom(16)) is None); s.close()
    print("  Noise NK (independent client vs our server): done")

    print("\n%d/%d checks match the independent implementations" % (ok, ok + bad))
    sys.exit(1 if bad else 0)

main()
