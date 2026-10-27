#!/usr/bin/env python3
"""
OSISOS Netcat <-> Web Runner Bridge (Pure Python / Zero Dependencies)

Bridges raw TCP Netcat connections on port 9999 into the browser WebAssembly
runner (web-runner.html) via an RFC 6455 WebSocket on port 9998.

Usage:
    python3 tools/nc_web_bridge.py

Then from any machine / terminal on the same Wi-Fi:
    nc <host-ip> 9999
"""

import socket
import threading
import hashlib
import base64
import sys
import time

WS_PORT = 9998
NC_PORT = 9999

ws_clients = []
ws_lock = threading.Lock()

def ws_handshake(client_sock):
    request = b""
    while b"\r\n\r\n" not in request:
        data = client_sock.recv(1024)
        if not data:
            return False
        request += data

    key = None
    for line in request.decode('utf-8', errors='ignore').split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
            break

    if not key:
        return False

    magic = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    digest = hashlib.sha1(key.encode('utf-8') + magic).digest()
    accept_val = base64.b64encode(digest).decode('utf-8')

    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept_val}\r\n"
        "\r\n"
    )
    client_sock.sendall(response.encode('utf-8'))
    return True

def make_ws_frame(text):
    payload = text.encode('utf-8')
    length = len(payload)
    if length < 126:
        header = bytes([0x81, length])
    elif length <= 0xFFFF:
        header = bytes([0x81, 126]) + length.to_bytes(2, 'big')
    else:
        header = bytes([0x81, 127]) + length.to_bytes(8, 'big')
    return header + payload

def broadcast_to_browsers(message):
    frame = make_ws_frame(message)
    with ws_lock:
        dead = []
        for c in ws_clients:
            try:
                c.sendall(frame)
            except Exception:
                dead.append(c)
        for d in dead:
            ws_clients.remove(d)

def ws_server_loop():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", WS_PORT))
    server.listen(5)
    print(f"[*] WebSocket Server active on ws://127.0.0.1:{WS_PORT}")

    while True:
        try:
            client, addr = server.accept()
            if ws_handshake(client):
                with ws_lock:
                    ws_clients.append(client)
                print(f"[+] Web browser client connected from {addr[0]}:{addr[1]}")
            else:
                client.close()
        except Exception as e:
            time.sleep(0.5)

def nc_client_handler(client, addr):
    print(f"[+] Netcat TCP connection accepted from {addr[0]}:{addr[1]}")
    ack = b"Message delivered to OSISOS screen!\r\n"
    try:
        data = client.recv(1024)
        if data:
            msg = data.decode('utf-8', errors='ignore').strip()
            print(f"[>] Netcat packet payload: '{msg}'")
            broadcast_to_browsers(msg)
            client.sendall(ack)
    except Exception as e:
        print(f"[-] Error handling netcat client: {e}")
    finally:
        client.close()

def nc_server_loop():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", NC_PORT))
    server.listen(5)
    print(f"[*] Netcat TCP Server listening on 0.0.0.0:{NC_PORT}")

    while True:
        try:
            client, addr = server.accept()
            t = threading.Thread(target=nc_client_handler, args=(client, addr), daemon=True)
            t.start()
        except Exception as e:
            time.sleep(0.5)

def main():
    print("=" * 64)
    print("  OSISOS Netcat <-> Web Runner Bridge (Port 9999 -> WebAssembly)")
    print("=" * 64)

    t_ws = threading.Thread(target=ws_server_loop, daemon=True)
    t_ws.start()

    t_nc = threading.Thread(target=nc_server_loop, daemon=True)
    t_nc.start()

    print("\nBridge is running! Instructions:")
    print(" 1. Open http://localhost:8000/web-runner.html (or https://osisos.vercel.app/web-runner.html)")
    print(" 2. Send messages via netcat:")
    print(f"      echo 'Hello OSISOS' | nc 127.0.0.1 {NC_PORT}")
    print("    Or from another computer on your Wi-Fi:")
    print(f"      echo 'Hello OSISOS' | nc <your-ip> {NC_PORT}")
    print("\nPress Ctrl+C to exit.\n" + "=" * 64)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[*] Shutting down bridge.")
        sys.exit(0)

if __name__ == "__main__":
    main()
