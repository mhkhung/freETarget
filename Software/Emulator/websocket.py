"""Small synchronous RFC 6455 client used to test the actual firmware API."""
import base64
import hashlib
import json
import os
import socket
import struct
from urllib.parse import urlsplit

from protocol import exact


class WebSocket:
    pongs = 0          # Pong frames received (answers to our pings)
    opened = False     # Handshake done, so a close frame can be sent

    def __init__(self, url, timeout=10, headers=None):
        parsed = urlsplit(url)
        if parsed.scheme != "ws" or parsed.hostname not in ("127.0.0.1", "localhost"):
            raise ValueError("test clients connect only to local ws:// endpoints")
        extra = ""
        for name, value in (headers or {}).items():
            if not name or any(c in name for c in "\r\n:") or any(c in value for c in "\r\n"):
                raise ValueError("invalid WebSocket request header")
            extra += f"{name}: {value}\r\n"
        self.sock = socket.create_connection((parsed.hostname, parsed.port or 80), timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()
        self.fragments = bytearray()
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f"GET {parsed.path or '/'} HTTP/1.1\r\nHost: {parsed.netloc}\r\n"
                   f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{extra}\r\n")
        self.sock.sendall(request.encode())
        header = bytearray()
        while not header.endswith(b"\r\n\r\n") and len(header) < 16384:
            header.extend(exact(self.sock, 1))
        lines = header.decode("latin1").split("\r\n")
        headers = {name.lower(): value.strip() for line in lines[1:] if ":" in line for name, value in [line.split(":", 1)]}
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        if " 101 " not in lines[0] or headers.get("sec-websocket-accept", "").strip() != expected:
            self.close()
            raise ConnectionError("WebSocket handshake refused: " + lines[0])
        self.opened = True

    def frame(self, opcode, data):
        mask = os.urandom(4)
        head = bytes([0x80 | opcode])
        if len(data) < 126:
            head += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            head += bytes([0x80 | 126]) + struct.pack("!H", len(data))
        else:
            head += bytes([0x80 | 127]) + struct.pack("!Q", len(data))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def send(self, obj):
        self.frame(1, json.dumps(obj, separators=(",", ":")).encode())

    def receive(self):
        def need(size):
            while len(self.buffer) < size:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise EOFError("WebSocket closed")
                self.buffer.extend(chunk)

        while True:
            need(2)
            first, second = self.buffer[:2]
            opcode, size = first & 15, second & 127
            head = 2
            if size == 126:
                need(4)
                size, = struct.unpack("!H", self.buffer[2:4])
                head = 4
            elif size == 127:
                need(10)
                size, = struct.unpack("!Q", self.buffer[2:10])
                head = 10
            if size > 1024 * 1024 or second & 128:
                raise ValueError("invalid/oversized server frame")
            need(head + size)
            payload = bytes(self.buffer[head:head + size])
            del self.buffer[:head + size]
            if opcode == 8:
                raise EOFError("WebSocket closed")
            if opcode == 9:
                self.frame(10, payload)
                continue
            if opcode == 10:
                self.pongs += 1
                continue
            if opcode in (0, 1):
                self.fragments.extend(payload)
                if len(self.fragments) > 1024 * 1024:
                    raise ValueError("oversized fragmented message")
                if first & 128:
                    text = self.fragments.decode()
                    self.fragments.clear()
                    try:
                        return json.loads(text)
                    except json.JSONDecodeError:
                        return {"console": text}

    def close(self):
        # Say goodbye first: without a close frame the firmware's web server
        # logs "Failed to read header byte (socket FD invalid)".
        if self.opened:
            self.opened = False
            try:
                self.frame(8, struct.pack("!H", 1000))
            except OSError:
                pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
