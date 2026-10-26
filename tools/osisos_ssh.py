#!/usr/bin/env python3
"""
osisos_ssh.py - client for the OSISOS encrypted remote console (port 2324).

Protocol: Noise_NK_25519_ChaChaPoly_SHA256 (https://noiseprotocol.org). The server's static public key
must be known in advance (print it on the OSISOS console with `rcon key`); the handshake only completes
with the real server, so a man-in-the-middle cannot impersonate it. All traffic is then encrypted and
authenticated with forward-secret keys. Messages are framed with a 2-byte big-endian length. You log in
with your password INSIDE the encrypted channel.

    pip install noiseprotocol
    python3 osisos_ssh.py 10.0.2.15 --server-key <64 hex chars>                    # interactive
    python3 osisos_ssh.py 10.0.2.15 --server-key <key> -c "df" -c "ifconfig"       # run commands, print output
    OSISOS_PASSWORD=... python3 osisos_ssh.py ... -c "dmesg"                       # no password prompt
"""
import argparse, getpass, os, socket, sys, threading, time
from noise.connection import NoiseConnection, Keypair

PROLOGUE = b"OSISOS-RCON-1"
PROTOCOL = b"Noise_NK_25519_ChaChaPoly_SHA256"
MAX_PLAINTEXT = 500            # per message; the server accepts up to 684


class SecureChannel:
    def __init__(self, host, port, server_key_hex, timeout=15):
        pub = bytes.fromhex(server_key_hex.strip())
        if len(pub) != 32:
            raise ValueError("the server key must be 64 hex characters (see `rcon key` on the console)")
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.raw_tx, self.raw_rx = bytearray(), bytearray()          # wire bytes, handy for tests / debugging
        c = NoiseConnection.from_name(PROTOCOL)
        c.set_as_initiator()
        c.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, pub)
        c.set_prologue(PROLOGUE)
        c.start_handshake()
        self.send_frame(c.write_message())
        try:
            c.read_message(self.recv_frame())
        except Exception as e:
            self.sock.close()
            raise ConnectionError("handshake failed: wrong server key, or not an OSISOS secure console (%s)" % e)
        if not c.handshake_finished:
            raise ConnectionError("handshake did not complete")
        self.noise = c

    def _recvn(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("connection closed by the server")
            self.raw_rx += chunk
            data += chunk
        return data

    def send_frame(self, body):
        wire = len(body).to_bytes(2, "big") + body
        self.raw_tx += wire
        self.sock.sendall(wire)

    def recv_frame(self):
        n = int.from_bytes(self._recvn(2), "big")
        return self._recvn(n)

    def send(self, data):
        for i in range(0, len(data), MAX_PLAINTEXT):
            self.send_frame(self.noise.encrypt(data[i:i + MAX_PLAINTEXT]))

    def recv(self):
        return self.noise.decrypt(self.recv_frame())

    def recv_until(self, token, timeout=15, eof_ok=False):
        """Read until `token` (bytes) appears in the stream; returns everything read.
        With eof_ok, a connection closed by the server returns what arrived so far (e.g. a refusal message)."""
        token = token if isinstance(token, bytes) else token.encode()
        buf, end = b"", time.time() + timeout
        while token not in buf:
            if time.time() > end:
                raise TimeoutError("timed out waiting for %r; got %r" % (token, buf[-200:]))
            try:
                buf += self.recv()
            except ConnectionError:
                if eof_ok:
                    return buf
                raise
        return buf

    def close(self):
        try: self.sock.close()
        except OSError: pass


def login(ch, user, password):
    ch.recv_until("login: ")
    ch.send(user.encode() + b"\n")
    ch.recv_until("password: ")
    ch.send(password.encode() + b"\n")
    return ch.recv_until(b"> ", 20, eof_ok=True)            # the prompt, or an error message followed by the server closing


def main():
    ap = argparse.ArgumentParser(description="OSISOS encrypted remote console client")
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=2324)
    ap.add_argument("--server-key", required=True, help="server public key, 64 hex chars (`rcon key`)")
    ap.add_argument("--user", default="admin")
    ap.add_argument("-c", "--command", action="append", help="run this command and print its output (repeatable)")
    a = ap.parse_args()

    try:
        ch = SecureChannel(a.host, a.port, a.server_key)
    except (ConnectionError, OSError, ValueError) as e:
        sys.exit("error: %s" % e)
    print("# encrypted channel established (server key verified)", file=sys.stderr)
    password = os.environ.get("OSISOS_PASSWORD") or getpass.getpass("password for %s: " % a.user)
    try:
        out = login(ch, a.user, password)
    except (ConnectionError, TimeoutError) as e:
        sys.exit("login failed: %s" % e)
    prompt = (a.user + "@osisos> ").encode()
    if not out.rstrip().endswith(b">"):
        sys.exit(out.decode(errors="replace").replace("\r", ""))

    if a.command:
        for cmd in a.command:
            ch.send(cmd.encode() + b"\n")
            out = ch.recv_until(prompt, 60)
            print(out.decode(errors="replace").replace("\r", "").rsplit(prompt.decode(), 1)[0], end="")
        ch.send(b"exit\n")
        ch.close()
        return

    def reader():
        try:
            while True:
                sys.stdout.write(ch.recv().decode(errors="replace").replace("\r", "")); sys.stdout.flush()
        except Exception:
            os._exit(0)
    threading.Thread(target=reader, daemon=True).start()
    sys.stdout.write(out.decode(errors="replace").replace("\r", "")); sys.stdout.flush()
    try:
        for line in sys.stdin:
            ch.send(line.encode())
            if line.strip() in ("exit", "quit", "logout"):
                time.sleep(0.5); break
    except KeyboardInterrupt:
        pass
    ch.close()


if __name__ == "__main__":
    main()
