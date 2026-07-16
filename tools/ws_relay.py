#!/usr/bin/env python3
"""Bridge the native engine's UDP book stream to the browser over WebSocket.

The C++ binary (`orderbook <tape> --live SYM`) sends binary frames to UDP
127.0.0.1:12345; this relay forwards each datagram, unmodified, to every
connected WebSocket client on ws://127.0.0.1:8765. Stdlib only — no pip installs.

Run it, then open report/live-native.html.
"""
import base64
import hashlib
import selectors
import socket
import struct
import sys

UDP_ADDR = ('127.0.0.1', 12345)       # engine -> browser (book/stats frames)
CTRL_ADDR = ('127.0.0.1', 12346)      # browser -> engine (subscribe SYM)
WS_ADDR = ('127.0.0.1', 8765)
GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


def ws_read_client(conn):
    """Read one masked client frame; return its text payload, or None."""
    hdr = conn.recv(2)
    if len(hdr) < 2:
        return None
    opcode = hdr[0] & 0x0F
    length = hdr[1] & 0x7F
    if length == 126:
        length = struct.unpack('!H', conn.recv(2))[0]
    elif length == 127:
        length = struct.unpack('!Q', conn.recv(8))[0]
    mask = conn.recv(4) if hdr[1] & 0x80 else b'\0\0\0\0'
    data = bytearray(conn.recv(length))
    for i in range(len(data)):
        data[i] ^= mask[i % 4]
    if opcode == 0x8:                 # close
        return None
    return bytes(data).decode('utf-8', 'replace')


def ws_handshake(conn):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = conn.recv(1024)
        if not chunk:
            return False
        data += chunk
    key = None
    for line in data.split(b'\r\n'):
        if line.lower().startswith(b'sec-websocket-key:'):
            key = line.split(b':', 1)[1].strip()
    if not key:
        return False
    accept = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest()).decode()
    conn.send(('HTTP/1.1 101 Switching Protocols\r\n'
               'Upgrade: websocket\r\nConnection: Upgrade\r\n'
               f'Sec-WebSocket-Accept: {accept}\r\n\r\n').encode())
    return True


def ws_frame(payload):
    # single binary frame (opcode 0x2), server->client is never masked
    n = len(payload)
    if n < 126:
        header = struct.pack('!BB', 0x82, n)
    elif n < 65536:
        header = struct.pack('!BBH', 0x82, 126, n)
    else:
        header = struct.pack('!BBQ', 0x82, 127, n)
    return header + payload


def main():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        udp.bind(UDP_ADDR)
    except OSError as e:
        sys.exit(f'cannot bind UDP {UDP_ADDR[1]}: {e}\n'
                 f'another relay is already running — find it with '
                 f'`ss -ulpn | grep {UDP_ADDR[1]}` and kill that PID, or just use it.')
    udp.setblocking(False)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(WS_ADDR)
    srv.listen(8)
    srv.setblocking(False)

    sel = selectors.DefaultSelector()
    sel.register(udp, selectors.EVENT_READ, 'udp')
    sel.register(srv, selectors.EVENT_READ, 'srv')
    clients = set()
    print(f'relay: UDP {UDP_ADDR[1]} -> ws://{WS_ADDR[0]}:{WS_ADDR[1]} · run the engine with --live', flush=True)

    while True:
        for key, _ in sel.select(timeout=1):
            tag = key.data
            if tag == 'udp':
                while True:
                    try:
                        msg, _ = udp.recvfrom(2048)
                    except BlockingIOError:
                        break
                    frame = ws_frame(msg)
                    for c in list(clients):
                        try:
                            c.sendall(frame)
                        except OSError:
                            clients.discard(c); sel.unregister(c); c.close()
            elif tag == 'srv':
                conn, _ = srv.accept()
                try:
                    conn.setblocking(True)
                    if ws_handshake(conn):
                        conn.setblocking(False)
                        clients.add(conn)
                        sel.register(conn, selectors.EVENT_READ, 'client')
                        print(f'client connected ({len(clients)} total)', flush=True)
                    else:
                        conn.close()
                except OSError:
                    conn.close()
            else:  # client -> engine: "SYM" subscribe requests
                try:
                    sym = ws_read_client(key.fileobj)
                    if sym is None:
                        raise OSError
                    sym = sym.strip().upper()
                    if sym:
                        udp.sendto(sym.encode(), CTRL_ADDR)
                except OSError:
                    clients.discard(key.fileobj); sel.unregister(key.fileobj); key.fileobj.close()
                    print(f'client left ({len(clients)} total)', flush=True)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
