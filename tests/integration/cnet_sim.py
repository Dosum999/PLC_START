"""LS XGT Cnet station simulator on a serial port: python cnet_sim.py <tty> <station> [<station> ...].

Implements the XGT dedicated protocol (R/r SS, SB; W/w SS, SB) as described in
the XBL-C21A Cnet manual chapter 7. D is limited to D0-D7999 (NAK 1132 beyond).
Unknown stations stay silent like on a real RS-485 bus.
"""
import os
import re
import sys
import termios
import tty

ENQ, ACK, NAK, EOT, ETX = 0x05, 0x06, 0x15, 0x04, 0x03
UNIT = {"X": 1, "B": 1, "W": 2, "D": 4, "L": 8}
LIMIT = {"D": 16000}  # bytes
mem = {}


def area(name):
    return mem.setdefault(name, bytearray(65536))


def parse_var(v):
    m = re.fullmatch(r"%(ZR|[A-Z])([XBWDL])(\d+)", v)
    if not m:
        raise ValueError(v)
    return m.group(1), m.group(2), int(m.group(3))


def check(a, off, n):
    if off + n > LIMIT.get(a, 65536):
        raise IndexError


def read_unit(a, size, idx):
    if size == "X":
        b = area(a)[idx // 8]
        return f"{(b >> (idx % 8)) & 1:02X}"
    u = UNIT[size]
    check(a, idx * u, u)
    return area(a)[idx * u: idx * u + u][::-1].hex().upper()  # little endian memory, MSB first on the wire


def write_unit(a, size, idx, text):
    if size == "X":
        m = area(a)
        m[idx // 8] = (m[idx // 8] | (1 << (idx % 8))) if int(text, 16) & 1 else (m[idx // 8] & ~(1 << (idx % 8)))
        return
    u = UNIT[size]
    check(a, idx * u, u)
    area(a)[idx * u: idx * u + u] = bytes.fromhex(text)[::-1]


def handle(req):
    st, cmd, typ, body = req[1:3], req[3:4], req[4:6], req[6:]
    out = [st, cmd, typ]
    try:
        if cmd in "rR" and typ == "SS":
            n, pos, data = int(body[0:2], 16), 2, []
            for _ in range(n):
                ln = int(body[pos:pos + 2], 16)
                a, size, idx = parse_var(body[pos + 2:pos + 2 + ln])
                pos += 2 + ln
                data.append(read_unit(a, size, idx))
            out += [f"{n:02X}"] + [f"{len(d) // 2:02X}{d}" for d in data]
        elif cmd in "rR" and typ == "SB":
            ln = int(body[0:2], 16)
            a, size, idx = parse_var(body[2:2 + ln])
            count = int(body[2 + ln:4 + ln], 16)
            data = "".join(read_unit(a, size, idx + i) for i in range(count))
            out += ["01", f"{len(data) // 2:02X}", data]
        elif cmd in "wW" and typ == "SS":
            n, pos = int(body[0:2], 16), 2
            for _ in range(n):
                ln = int(body[pos:pos + 2], 16)
                a, size, idx = parse_var(body[pos + 2:pos + 2 + ln])
                pos += 2 + ln
                w = 2 * (1 if size == "X" else UNIT[size])
                write_unit(a, size, idx, body[pos:pos + w])
                pos += w
        elif cmd in "wW" and typ == "SB":
            ln = int(body[0:2], 16)
            a, size, idx = parse_var(body[2:2 + ln])
            count, pos = int(body[2 + ln:4 + ln], 16), 4 + ln
            for i in range(count):
                w = 2 * UNIT[size]
                write_unit(a, size, idx + i, body[pos:pos + w])
                pos += w
        else:
            return NAK, [st, cmd, typ, "1131"]
    except (ValueError, IndexError):
        return NAK, [st, cmd, typ, "1132"]
    return ACK, out


def main(dev, stations):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    buf = b""
    while True:
        buf += os.read(fd, 256)
        while True:
            start = buf.find(bytes([ENQ]))
            end = buf.find(bytes([EOT]), start + 1)
            if start < 0 or end < 0:
                break
            lower = buf[start + 3:start + 4].islower()
            if lower and len(buf) < end + 3:
                break
            frame = buf[start:end + 1].decode()
            given = buf[end + 1:end + 3] if lower else b""
            buf = buf[end + (3 if lower else 1):]
            if lower and int(given, 16) != sum(frame.encode()) & 0xFF:
                continue  # bad BCC: no answer
            if int(frame[1:3], 16) not in stations:
                continue
            head, parts = handle(frame[:-1])
            resp = bytes([head]) + "".join(parts).encode() + bytes([ETX])
            if lower:
                resp += f"{sum(resp) & 0xFF:02X}".encode()
            os.write(fd, resp)


if __name__ == "__main__":
    main(sys.argv[1], {int(s) for s in sys.argv[2:]})
