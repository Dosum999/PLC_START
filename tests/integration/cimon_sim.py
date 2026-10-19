"""CIMON PLC simulator after the CIMON PLC manual (cimon.atlassian.net).

python cimon_sim.py serial <tty> <control port>
python cimon_sim.py enet <tcp port> <control port>

Serial: computer link frames (ENQ station cmd len data BCC EOT), stations
0 and 1 answer. Ethernet: "KDT_PLC_M" exclusive service frames. Devices are
words; bit devices X/Y/M/L/K/F are also addressed as word + hex bit. D is
limited to D0-D9999 so larger addresses get error 06 / 0003h.

The control port takes text lines "get D 100 2" (prints words) and
"set D 100 1 2 3" so tests can preset and check memory without going
through the protocol under test.
"""
import socketserver
import sys
import threading

WORDS = {name: [0] * 100000 for name in ("X", "Y", "M", "L", "K", "F", "D", "S", "Z", "TC", "TS", "CC", "CS")}
LIMIT = {"D": 10000}
BIT_DEVICES = {"X", "Y", "M", "L", "K", "F"}
CONTACTS = {"T": "TC", "C": "CC"}  # contact on when the current value reaches the preset


class Err(Exception):
    def __init__(self, serial_code, enet_code):
        super().__init__()
        self.serial, self.enet = serial_code, enet_code


def split_address(addr: str):
    dev = addr[0] if addr[1] == "0" else addr[:2]
    return dev, addr[2:]


def word_mem(dev, start, count):
    if dev not in WORDS:
        raise Err(4, 2)
    if start + count > LIMIT.get(dev, len(WORDS[dev])):
        raise Err(6, 3)
    return WORDS[dev]


def read_words(addr, count):
    dev, digits = split_address(addr)
    start = int(digits[:5]) if dev in BIT_DEVICES else int(digits)
    mem = word_mem(dev, start, count)
    return mem[start:start + count]


def write_words(addr, values):
    dev, digits = split_address(addr)
    start = int(digits[:5]) if dev in BIT_DEVICES else int(digits)
    mem = word_mem(dev, start, len(values))
    mem[start:start + len(values)] = values


def bit_location(addr):
    dev, digits = split_address(addr)
    if dev in CONTACTS:
        return dev, int(digits)
    if dev not in BIT_DEVICES:
        raise Err(4, 2)
    return dev, int(digits[:5]) * 16 + int(digits[5], 16)


def read_bits(addr, count):
    dev, start = bit_location(addr)
    if dev in CONTACTS:
        cur, pre = WORDS[CONTACTS[dev]], WORDS[CONTACTS[dev][0] + "S"]
        return [1 if 0 < pre[i] <= cur[i] else 0 for i in range(start, start + count)]
    mem = word_mem(dev, (start + count + 15) // 16 - 1, 1)
    return [(mem[i // 16] >> (i % 16)) & 1 for i in range(start, start + count)]


def write_bits(addr, values):
    dev, start = bit_location(addr)
    if dev in CONTACTS:
        raise Err(15, 0x0F)
    mem = word_mem(dev, (start + len(values) + 15) // 16 - 1, 1)
    for i, v in enumerate(values, start):
        mem[i // 16] = mem[i // 16] | (1 << (i % 16)) if v else mem[i // 16] & ~(1 << (i % 16))


# ---- serial -------------------------------------------------------------------------

def bcc(s: bytes) -> bytes:
    return f"{sum(s) & 0xFF:02X}".encode()


def serial_answer(station, cmd, data: bytes) -> bytes:
    body = cmd + f"{len(data):02X}".encode() + data
    return b"\x02" + f"{station:02X}".encode() + body + bcc(body) + b"\x03"


def serial_process(frame: bytes):
    """frame: ENQ .. EOT. Returns the answer or None (other station / bad frame)."""
    station = int(frame[1:3], 16)
    if station not in (0, 1):
        return None
    cmd = frame[3:4]
    length = int(frame[4:6], 16)
    data = frame[6:6 + length]
    if bcc(frame[3:6 + length]) != frame[6 + length:8 + length]:
        return serial_answer(station, b"E", b"02")
    try:
        addr, count = data[:8].decode(), int(data[8:10], 16)
        if cmd == b"R":
            return serial_answer(station, cmd, "".join(f"{w:04X}" for w in read_words(addr, count)).encode())
        if cmd == b"W":
            write_words(addr, [int(data[10 + 4 * i:14 + 4 * i], 16) for i in range(count)])
            return serial_answer(station, cmd, b"")
        if cmd == b"r":
            return serial_answer(station, cmd, "".join(str(b) for b in read_bits(addr, count)).encode())
        if cmd == b"w":
            write_bits(addr, [1 if c == ord("1") else 0 for c in data[10:10 + count]])
            return serial_answer(station, cmd, b"")
        return serial_answer(station, b"E", b"01")
    except Err as e:
        return serial_answer(station, b"E", f"{e.serial:02d}".encode())


def run_serial(tty):
    import serial
    port = serial.Serial(tty, 19200, timeout=0.05)
    buf = b""
    while True:
        buf += port.read(512)
        start = buf.find(b"\x05")
        end = buf.find(b"\x04", start + 1) if start >= 0 else -1
        if start < 0 or end < 0:
            continue
        frame, buf = buf[start:end + 1], buf[end + 1:]
        answer = serial_process(frame)
        if answer:
            port.write(answer)


# ---- Ethernet --------------------------------------------------------------------------

def enet_answer(frame_no, cmd, data: bytes) -> bytes:
    f = b"KDT_PLC_S" + bytes([frame_no + 128, cmd, 0]) + len(data).to_bytes(2, "big") + data
    return f + (sum(f) & 0xFFFF).to_bytes(2, "big")


def enet_process(frame: bytes) -> bytes:
    frame_no, cmd = frame[9], frame[10]
    data = frame[14:-2]
    if (sum(frame[:-2]) & 0xFFFF).to_bytes(2, "big") != frame[-2:]:
        return enet_answer(frame_no, 0x41, b"\x00\x0a")
    try:
        addr, count = data[:8].decode(), int.from_bytes(data[8:10], "big")
        if cmd == 0x52:
            return enet_answer(frame_no, 0x52, data[:10] + b"".join(w.to_bytes(2, "big") for w in read_words(addr, count)))
        if cmd == 0x57:
            write_words(addr, [int.from_bytes(data[10 + 2 * i:12 + 2 * i], "big") for i in range(count)])
            return enet_answer(frame_no, 0x41, b"\x00\x00")
        if cmd == 0x72:
            return enet_answer(frame_no, 0x72, data[:10] + "".join(str(b) for b in read_bits(addr, count)).encode())
        if cmd == 0x77:
            write_bits(addr, [1 if c == ord("1") else 0 for c in data[10:10 + count]])
            return enet_answer(frame_no, 0x41, b"\x00\x00")
        return enet_answer(frame_no, 0x41, b"\x00\x0e")
    except Err as e:
        return enet_answer(frame_no, 0x41, e.enet.to_bytes(2, "big"))


class EnetHandler(socketserver.BaseRequestHandler):
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
                head = self.recv(14)
                if head[:9] != b"KDT_PLC_M":
                    return
                rest = self.recv(int.from_bytes(head[12:14], "big") + 2)
                self.request.sendall(enet_process(head + rest))
        except ConnectionError:
            return


class Control(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            parts = line.decode().split()
            if parts[0] == "get":
                dev, start, count = parts[1], int(parts[2]), int(parts[3])
                self.wfile.write((" ".join(str(w) for w in WORDS[dev][start:start + count]) + "\n").encode())
            elif parts[0] == "set":
                dev, start = parts[1], int(parts[2])
                vals = [int(v) for v in parts[3:]]
                WORDS[dev][start:start + len(vals)] = vals
                self.wfile.write(b"ok\n")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    mode, where, control = sys.argv[1], sys.argv[2], int(sys.argv[3])
    threading.Thread(target=Server(("127.0.0.1", control), Control).serve_forever, daemon=True).start()
    if mode == "serial":
        run_serial(where)
    else:
        Server(("127.0.0.1", int(where)), EnetHandler).serve_forever()
