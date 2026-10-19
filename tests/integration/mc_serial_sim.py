"""Mitsubishi serial communication module (C24) simulator.

python mc_serial_sim.py <tty> <frame 3C|4C> <format 1|4> <sum 0|1> <tcp port>

Answers MC protocol 3C/4C ASCII requests on the tty for stations 0 and 1
(others stay silent, as on a multidrop line) and serves the same device
memory as a binary 3E TCP port (mc_sim.py), so values can be preset and
checked with pymcprotocol. Message layouts follow the MC protocol reference
manual (SH-080008), section 4 and appendix 7.
"""
import sys
import threading

import serial

import mc_sim

STATIONS = (0, 1)


def checksum(data: bytes) -> bytes:
    return f"{sum(data) & 0xFF:02X}".encode()


def main(tty, frame, fmt, use_sum, tcp_port):
    server = mc_sim.Server(("127.0.0.1", tcp_port), mc_sim.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    header_len = 16 if frame == "4C" else 10
    frame_id = b"F8" if frame == "4C" else b"F9"
    port = serial.Serial(tty, 19200, timeout=0.05)
    buf = b""
    while True:
        chunk = port.read(512)
        if chunk:
            buf += chunk
            if fmt == 4 and b"\r\n" not in buf:
                continue
            if fmt == 1:
                continue  # format 1 has no terminator: wait for the line to go quiet
        if not buf or (fmt == 4 and b"\r\n" not in buf):
            continue
        start = buf.find(b"\x05")
        if start < 0:
            buf = b""
            continue
        msg = buf[start + 1:]
        buf = b""
        if fmt == 4:
            msg = msg[:msg.find(b"\r\n")]
        if use_sum:
            msg, sent = msg[:-2], msg[-2:]
            if checksum(msg) != sent:
                continue  # C24 answers NAK 7151-style errors; silence is enough for tests
        head, body = msg[:header_len], msg[header_len:]
        if head[:2] != frame_id or int(head[2:4], 16) not in STATIONS:
            continue
        binary, sub, points = mc_sim.ascii_to_binary(b"0004" + body)
        end, data = mc_sim.Handler.process(None, binary)
        tail = b"\r\n" if fmt == 4 else b""
        if end != 0:
            resp = b"\x15" + head + f"{end:04X}".encode() + tail
        elif body[:4] == b"0401":
            payload = head + mc_sim.binary_to_ascii_data(data, sub, points) + b"\x03"
            resp = b"\x02" + payload + (checksum(payload) if use_sum else b"") + tail
        else:
            resp = b"\x06" + head + tail
        port.write(resp)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] == "1", int(sys.argv[5]))
