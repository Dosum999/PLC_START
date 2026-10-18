"""Keyence KV host link PLC simulator: python kv_sim.py <port>.

Implements RD / RDS / WR / WRS over TCP with the data formats "", .U, .S,
.D, .L and .H, KEYENCE device notation (R/MR/LR/CR channel+bit, B/VB/W hex)
and the XYM aliases. Timer/counter devices T/C answer "contact,current,preset".
Its behaviour is checked against the kv-hostlink package (an independent
client) in test_keyence.py. DM is limited to DM0-DM9999 so that larger
addresses get E0 like a real CPU with a smaller device range.
"""
import re
import socketserver
import sys

BANK = {"R", "MR", "LR", "CR"}
HEX = {"B", "VB", "W"}
XY = {"X", "Y"}
BIT_DEVICES = {"R", "MR", "LR", "CR", "B", "VB", "X", "Y", "M", "L"}
WORD_DEVICES = {"DM": "DM", "EM": "EM", "FM": "FM", "ZF": "ZF", "W": "W", "TM": "TM", "CM": "CM", "VM": "VM",
                "D": "DM", "E": "EM", "F": "FM"}
TIMER_VALUES = {"TC", "TS", "CC", "CS"}
LIMIT = {"DM": 10000, "TM": 512}

words = {name: [0] * 600000 for name in set(WORD_DEVICES.values())}
bits = {name: [0] * 70000 for name in BIT_DEVICES}
timer = {name: [0] * 4000 for name in TIMER_VALUES}

DEVICE_RE = re.compile(r"^(DM|EM|FM|ZF|TM|CM|VM|VB|MR|LR|CR|TC|TS|CC|CS|R|B|W|X|Y|M|L|D|E|F|T|C)([0-9A-F]+)(\.[USDLH])?$")


class E(Exception):
    pass


def parse(token):
    m = DEVICE_RE.match(token)
    if not m:
        raise E("E0")
    dev, num, fmt = m.group(1), m.group(2), m.group(3) or ""
    if dev in BANK:
        n = int(num, 10)
        if n % 100 > 15:
            raise E("E0")
        index = n // 100 * 16 + n % 100
    elif dev in XY:
        index = int(num[:-1] or "0", 10) * 16 + int(num[-1], 16)
    elif dev in HEX:
        index = int(num, 16)
    else:
        index = int(num, 10)
    return dev, index, fmt


def to_text(value, fmt):
    if fmt == ".H":
        return f"{value & 0xFFFF:04X}"
    if fmt == ".S":
        v = value - 0x10000 if value & 0x8000 else value
        return f"{'-' if v < 0 else '+'}{abs(v):05d}"
    if fmt == ".D":
        return f"{value & 0xFFFFFFFF:010d}"
    if fmt == ".L":
        v = value - (1 << 32) if value & 0x80000000 else value
        return f"{'-' if v < 0 else '+'}{abs(v):010d}"
    return f"{value & 0xFFFF:05d}"


def from_text(text, fmt):
    if fmt == ".H":
        return int(text, 16) & 0xFFFF
    v = int(text, 10)
    return v & (0xFFFFFFFF if fmt in (".D", ".L") else 0xFFFF)


class Accessor:
    """Reads/writes one device unit in the requested format."""

    def __init__(self, dev, index, fmt):
        self.dev, self.index, self.fmt = dev, index, fmt
        if dev in WORD_DEVICES:
            self.mem, self.kind = words[WORD_DEVICES[dev]], "word"
            self.limit = LIMIT.get(WORD_DEVICES[dev], len(self.mem))
            self.fmt = fmt or ".U"
            self.width = 2 if self.fmt in (".D", ".L") else 1
        elif dev in BIT_DEVICES:
            self.mem, self.kind = bits[dev], "bit"
            self.limit = len(self.mem)
            self.width = 1 if fmt == "" else 32 if fmt in (".D", ".L") else 16
        elif dev in TIMER_VALUES:
            self.mem, self.kind, self.limit, self.width = timer[dev], "timer", 4000, 1
            self.fmt = fmt or ".D"
        else:
            self.kind, self.limit, self.width = "contact", 4000, 1

    def check(self, count):
        if self.index + count * self.width > self.limit:
            raise E("E0")

    def read(self, i):
        at = self.index + i * self.width
        if self.kind == "contact":
            name = "TC" if self.dev == "T" else "CC"
            pre = "TS" if self.dev == "T" else "CS"
            done = 1 if 0 < timer[pre][at] <= timer[name][at] else 0  # contact set once current reaches preset
            return f"{done},{timer[name][at]:010d},{timer[pre][at]:010d}"
        if self.kind == "timer":
            return to_text(self.mem[at], self.fmt)
        if self.kind == "bit":
            if self.width == 1:
                return str(self.mem[at])
            return to_text(sum(self.mem[at + b] << b for b in range(self.width)), self.fmt)
        if self.width == 2:
            return to_text(self.mem[at] | self.mem[at + 1] << 16, self.fmt)
        return to_text(self.mem[at], self.fmt)

    def write(self, i, text):
        at = self.index + i * self.width
        if self.kind == "contact":
            raise E("E4")
        if self.kind == "timer":
            self.mem[at] = from_text(text, self.fmt)
        elif self.kind == "bit":
            if self.width == 1:
                if text not in ("0", "1"):
                    raise E("E0")
                self.mem[at] = int(text)
            else:
                v = from_text(text, self.fmt)
                for b in range(self.width):
                    self.mem[at + b] = (v >> b) & 1
        elif self.width == 2:
            v = from_text(text, self.fmt)
            self.mem[at], self.mem[at + 1] = v & 0xFFFF, v >> 16
        else:
            self.mem[at] = from_text(text, self.fmt)


def execute(line):
    parts = line.split()
    if not parts:
        raise E("E1")
    cmd = parts[0]
    if cmd == "RD" and len(parts) == 2:
        acc = Accessor(*parse(parts[1]))
        acc.check(1)
        return acc.read(0)
    if cmd == "RDS" and len(parts) == 3:
        acc = Accessor(*parse(parts[1]))
        count = int(parts[2])
        if not 1 <= count <= 1000:
            raise E("E0")
        acc.check(count)
        return " ".join(acc.read(i) for i in range(count))
    if cmd == "WR" and len(parts) == 3:
        acc = Accessor(*parse(parts[1]))
        acc.check(1)
        acc.write(0, parts[2])
        return "OK"
    if cmd == "WRS" and len(parts) >= 4:
        acc = Accessor(*parse(parts[1]))
        count = int(parts[2])
        if count != len(parts) - 3:
            raise E("E0")
        acc.check(count)
        for i in range(count):
            acc.write(i, parts[3 + i])
        return "OK"
    raise E("E1")


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        buf = b""
        while True:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            buf += chunk
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                text = line.decode("ascii").strip()
                if text.startswith("\n"):
                    text = text[1:]
                try:
                    answer = execute(text)
                except E as e:
                    answer = str(e)
                except ValueError:
                    answer = "E0"
                self.request.sendall(answer.encode("ascii") + b"\r\n")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    Server(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
