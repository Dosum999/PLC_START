"""Mitsubishi MC protocol (3E frame, binary) PLC simulator: python mc_sim.py <port>.

Implements batch read/write (0401/1401) in word and bit units. Its behaviour is
checked against pymcprotocol (an independent client) in test_mitsubishi.py
before the driver is tested against it. D is limited to D0-D7999 so that
out-of-range accesses return end code C056 like a real CPU.
"""
import socketserver
import struct
import sys

# Binary device codes (MC protocol reference manual).
WORD = {0xA8: "D", 0xB4: "W", 0xAF: "R", 0xB0: "ZR", 0xC2: "TN", 0xC5: "CN", 0xA9: "SD", 0xCC: "Z", 0xB5: "SW"}
BIT = {0x9C: "X", 0x9D: "Y", 0x90: "M", 0x92: "L", 0x93: "F", 0x94: "V", 0xA0: "B", 0x91: "SM", 0xC1: "TS", 0xC0: "TC",
       0xC4: "CS", 0xC3: "CC", 0xA1: "SB"}
LIMIT = {"D": 8000}

words = {name: [0] * 65536 for name in WORD.values()}
bits = {name: [0] * 65536 for name in BIT.values()}


class Handler(socketserver.BaseRequestHandler):
    def recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def handle(self):
        try:
            while True:
                hdr = self.recv(9)
                if hdr[0:2] != b"\x50\x00":
                    return
                length = struct.unpack_from("<H", hdr, 7)[0]
                body = self.recv(length)
                end, data = self.process(body)
                resp = b"\xd0\x00" + hdr[2:7] + struct.pack("<HH", 2 + len(data), end) + data
                self.request.sendall(resp)
        except ConnectionError:
            return

    def process(self, body):
        _timer, cmd, sub = struct.unpack_from("<HHH", body, 0)
        if cmd not in (0x0401, 0x1401) or sub not in (0, 1):
            return 0xC059, b""
        number = body[6] | (body[7] << 8) | (body[8] << 16)
        code, points = body[9], struct.unpack_from("<H", body, 10)[0]
        payload = body[12:]
        bit_units = sub == 1
        if code in WORD:
            if bit_units:
                return 0xC05B, b""
            name = WORD[code]
            if number + points > LIMIT.get(name, 65536):
                return 0xC056, b""
            mem = words[name]
            if cmd == 0x0401:
                return 0, struct.pack(f"<{points}H", *mem[number:number + points])
            mem[number:number + points] = struct.unpack(f"<{points}H", payload[:points * 2])
            return 0, b""
        if code in BIT:
            mem = bits[BIT[code]]
            if bit_units:
                if cmd == 0x0401:
                    vals = mem[number:number + points] + [0]
                    return 0, bytes((vals[i] << 4) | vals[i + 1] for i in range(0, points, 2))
                for i in range(points):
                    mem[number + i] = (payload[i // 2] >> (4 if i % 2 == 0 else 0)) & 1
                return 0, b""
            # Word units on a bit device: 16 points per word, lowest point = bit 0.
            if cmd == 0x0401:
                out = b""
                for w in range(points):
                    v = sum(mem[number + w * 16 + b] << b for b in range(16))
                    out += struct.pack("<H", v)
                return 0, out
            for w in range(points):
                v = struct.unpack_from("<H", payload, w * 2)[0]
                for b in range(16):
                    mem[number + w * 16 + b] = (v >> b) & 1
            return 0, b""
        return 0xC05C, b""


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    Server(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
