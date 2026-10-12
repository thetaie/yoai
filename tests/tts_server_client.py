#!/usr/bin/env python3
"""Usage: python tests/tts_server_client.py [options] [text]

A client for yoai-server (src/server/README.md), for trying it from a shell.
It sends the text a character at a time at --rate characters a second, the
way a language model writes a reply, and saves the audio it gets back.

  text          what to read; default is a short Chinese passage
  --url URL     default ws://127.0.0.1:8765/
  --rate N      characters a second; 0 sends everything at once (default 15)
  --out FILE    WAV to write (default tts_server_client.wav)
  --cancel S    send cancel S seconds after the first text, to try that path

The first-audio time it prints is from the first text sent, which is the
wait a listener would have; the server's own figure is from `start`.

Python 3.8 or newer, standard library only: the WebSocket client is written
out below rather than installed.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import wave
from urllib.parse import urlparse

sys.dont_write_bytecode = True

PASSAGE = ("我查了一下，明天上海多云转小雨，最高气温二十六度，最低十九度。"
           "早上出门的话，记得带把伞。")


class WebSocket:
    """Just enough RFC 6455 for this: one client, no extensions."""

    def __init__(self, url):
        u = urlparse(url)
        if u.scheme != "ws":
            raise ValueError("only ws:// URLs")
        self.sock = socket.create_connection((u.hostname, u.port or 80))
        self.lock = threading.Lock()   # send() is called from two threads
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path or "/"
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (path, u.hostname, u.port or 80, key))
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                raise ConnectionError("closed during the handshake")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(head.decode(errors="replace").strip())

    def send(self, payload, opcode=1):
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        elif n < 65536:
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, n)
        mask = os.urandom(4)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self.lock:
            self.sock.sendall(header + mask + body)

    def send_json(self, obj):
        self.send(json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def _exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed by the server")
            buf += chunk
        return buf

    def recv(self):
        """One whole message: (opcode, payload). Pings are answered here."""
        data, first = b"", None
        while True:
            b0, b1 = self._exact(2)
            op, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._exact(8))[0]
            payload = self._exact(n)
            if op == 9:
                self.send(payload, 10)
                continue
            if op == 10:
                continue
            if op == 8:
                return 8, payload
            if op != 0:
                first = op
            data += payload
            if b0 & 0x80:
                return first, data


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("text", nargs="?", default=PASSAGE)
    ap.add_argument("--url", default="ws://127.0.0.1:8765/")
    ap.add_argument("--rate", type=float, default=15.0)
    ap.add_argument("--out", default="tts_server_client.wav")
    ap.add_argument("--cancel", type=float, default=0.0)
    a = ap.parse_args()

    ws = WebSocket(a.url)
    ws.send_json({"type": "start", "format": "s16"})
    op, msg = ws.recv()
    started = json.loads(msg.decode("utf-8"))
    if started.get("type") != "started":
        print(started, file=sys.stderr)
        return 1
    rate = started["sample_rate"]

    t0 = [None]

    def writer():
        t0[0] = time.monotonic()
        if a.rate <= 0:
            ws.send_json({"type": "text", "text": a.text})
        else:
            for k, ch in enumerate(a.text):
                target = t0[0] + (k + 1) / a.rate
                time.sleep(max(0.0, target - time.monotonic()))
                ws.send_json({"type": "text", "text": ch})
        if a.cancel <= 0:
            ws.send_json({"type": "finish"})

    def canceller():
        time.sleep(a.cancel)
        ws.send_json({"type": "cancel"})

    threading.Thread(target=writer, daemon=True).start()
    if a.cancel > 0:
        threading.Thread(target=canceller, daemon=True).start()

    pcm, first = bytearray(), None
    while True:
        op, msg = ws.recv()
        if op == 2:
            if first is None:
                first = time.monotonic() - t0[0]
            pcm += msg
            continue
        if op == 8:
            print("closed by the server", file=sys.stderr)
            return 1
        ev = json.loads(msg.decode("utf-8"))
        if ev.get("type") in ("done", "cancelled"):
            print(json.dumps(ev, ensure_ascii=False))
            break
        print(json.dumps(ev, ensure_ascii=False), file=sys.stderr)
        if ev.get("type") == "error" and ev.get("code") == "synthesis":
            return 1

    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(pcm))
    secs = len(pcm) / 2.0 / rate
    print("%.2f s of audio -> %s; first audio %.2f s after the first text"
          % (secs, a.out, first if first is not None else -1.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
