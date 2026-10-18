"""Omron FINS PLC simulator: python fins_sim.py <udp|tcp> <port>.

Implements MEMORY AREA READ/WRITE (0101/0102) for CIO, W, H, A, D, EM banks
and timer/counter PVs and flags, in word and bit units, over FINS/UDP or
FINS/TCP (with the node address exchange). Its behaviour is checked against
the `fins` package (an independent client) in test_omron_fins.py.

The PLC is FINS node 1 and assigns node 11 to FINS/TCP clients; frames with
other node numbers are ignored like on a real CPU. D is limited to D0-D9999
so that larger addresses get end code 1103 (address range error).
"""
import socketserver
import struct
import sys

PLC_NODE = 1
TCP_CLIENT_NODE = 11
WORD_AREAS = {0xB0: 6144, 0xB1: 512, 0xB2: 1536, 0xB3: 11536, 0x82: 10000}
WORD_AREAS.update({0xA0 + b: 32768 for b in range(16)})
WORD_AREAS.update({0x60 + b: 32768 for b in range(9)})
BIT_TO_WORD = {0x30: 0xB0, 0x31: 0xB1, 0x32: 0xB2, 0x33: 0xB3, 0x02: 0x82}
BIT_TO_WORD.update({0x20 + b: 0xA0 + b for b in range(16)})
BIT_TO_WORD.update({0xE0 + b: 0x60 + b for b in range(9)})

words = {code: [0] * size for code, size in WORD_AREAS.items()}
tc_pv = [0] * 0x10000      # 0x89: timers 0-4095, counters 0x8000+
tc_flag = [0] * 0x10000    # 0x09


def process(cmd, expected_src_node):
    """Returns the response frame for one FINS command frame, or None to stay silent."""
    if len(cmd) < 12 or cmd[0] & 0x40 or cmd[4] != PLC_NODE:
        return None
    if expected_src_node is not None and cmd[7] != expected_src_node:
        return None
    head = bytes([0xC0, 0x00, 0x02, cmd[6], cmd[7], cmd[8], cmd[3], cmd[4], cmd[5], cmd[9], cmd[10], cmd[11]])
    end, data = execute(cmd[10], cmd[11], cmd[12:])
    return head + struct.pack(">H", end) + data


def execute(mrc, src, p):
    if mrc != 0x01 or src not in (0x01, 0x02):
        return 0x0401, b""  # undefined command
    if len(p) < 6:
        return 0x1001, b""
    code, word, bit, count = p[0], struct.unpack_from(">H", p, 1)[0], p[3], struct.unpack_from(">H", p, 4)[0]
    payload = p[6:]
    write = src == 0x02
    if code in WORD_AREAS:
        mem = words[code]
        if bit != 0:
            return 0x1103, b""
        if word + count > len(mem):
            return 0x1103, b""
        if not write:
            return 0, struct.pack(f">{count}H", *mem[word:word + count])
        if len(payload) != count * 2:
            return 0x1001, b""
        mem[word:word + count] = struct.unpack(f">{count}H", payload)
        return 0, b""
    if code in BIT_TO_WORD:
        mem = words[BIT_TO_WORD[code]]
        start = word * 16 + bit
        if bit > 15 or start + count > len(mem) * 16:
            return 0x1103, b""
        if not write:
            return 0, bytes((mem[(start + i) // 16] >> ((start + i) % 16)) & 1 for i in range(count))
        if len(payload) != count:
            return 0x1001, b""
        for i in range(count):
            w, b = divmod(start + i, 16)
            mem[w] = mem[w] | (1 << b) if payload[i] else mem[w] & ~(1 << b)
        return 0, b""
    if code in (0x89, 0x09):
        mem = tc_pv if code == 0x89 else tc_flag
        if (word & 0x7FFF) + count > 4096:
            return 0x1103, b""
        if code == 0x89:
            if not write:
                return 0, struct.pack(f">{count}H", *mem[word:word + count])
            mem[word:word + count] = struct.unpack(f">{count}H", payload)
            return 0, b""
        if not write:
            return 0, bytes(mem[word:word + count])
        return 0x2101, b""  # flags are not writable with 0102
    return 0x1101, b""  # area classification error


class UdpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        resp = process(data, None)
        if resp is not None:
            sock.sendto(resp, self.client_address)


class TcpHandler(socketserver.BaseRequestHandler):
    def recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def send(self, command, data, error=0):
        self.request.sendall(b"FINS" + struct.pack(">III", 8 + len(data), command, error) + data)

    def handle(self):
        client = None
        try:
            while True:
                hdr = self.recv(16)
                if hdr[:4] != b"FINS":
                    self.send(0, b"", error=1)
                    return
                length, command, _ = struct.unpack_from(">III", hdr, 4)
                body = self.recv(length - 8)
                if command == 0:
                    requested = struct.unpack(">I", body[:4])[0]
                    client = requested or TCP_CLIENT_NODE
                    self.send(1, struct.pack(">II", client, PLC_NODE))
                elif command == 2 and client is not None:
                    resp = process(body, client)
                    if resp is not None:
                        self.send(2, resp)
                else:
                    self.send(command, b"", error=3)
        except ConnectionError:
            return


class UdpServer(socketserver.ThreadingUDPServer):
    allow_reuse_address = True
    daemon_threads = True


class TcpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    mode, port = sys.argv[1], int(sys.argv[2])
    if mode == "udp":
        UdpServer(("127.0.0.1", port), UdpHandler).serve_forever()
    else:
        TcpServer(("127.0.0.1", port), TcpHandler).serve_forever()
