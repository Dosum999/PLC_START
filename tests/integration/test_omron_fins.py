"""Omron FINS Ethernet driver tests over FINS/UDP and FINS/TCP.

test_simulator_matches_fins_package checks the simulator with the `fins`
package, an independent client, so the driver tests below are not a closed loop.
"""

import asyncio
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

pytest.importorskip("fins")
from fins.tcp import TCPFinsConnection  # noqa: E402
from fins.udp import UDPFinsConnection  # noqa: E402

SIM = Path(__file__).with_name("fins_sim.py")
DM, CIO_BIT, TIMER_PV = b"\x82", b"\x30", b"\x89"


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connect_client(mode, port):
    if mode == "udp":
        c = UDPFinsConnection()
        c.dest_node_add = 1   # the simulated PLC is node 1
        c.srce_node_add = 2
        c.connect("127.0.0.1", port, bind_port=free_udp_port())
    else:
        c = TCPFinsConnection()
        c.connect("127.0.0.1", port, bind_port=0)  # node numbers from the FINS/TCP handshake
    return c


def raw_read(c, area, word, bit, count):
    r = c.memory_area_read(area, struct.pack(">HB", word, bit), count)
    assert r[12:14] == b"\x00\x00", r[12:14].hex()
    return r[14:]


def raw_write(c, area, word, bit, data, count):
    r = c.memory_area_write(area, struct.pack(">HB", word, bit), data, count)
    assert r[12:14] == b"\x00\x00", r[12:14].hex()


@pytest.fixture(params=["udp", "tcp"])
def plc(request):
    mode = request.param
    port = free_udp_port() if mode == "udp" else free_port()
    proc = subprocess.Popen([sys.executable, str(SIM), mode, str(port)])
    if mode == "tcp":
        wait_for_port(port, proc)
    else:
        time.sleep(0.5)
    c = connect_client(mode, port)
    yield mode, port, c
    proc.terminate()
    proc.wait(timeout=5)


def test_simulator_matches_fins_package(plc):
    _, _, c = plc
    c.write(1234, "d", 100, "ui")
    assert c.read("d", 100, "ui") == 1234
    c.write(0x12345678, "d", 102, "udi")       # the package stores the low word first
    assert c.read("d", 102, "ui") == 0x5678
    assert c.read("d", 102, "udi") == 0x12345678
    c.write(-5, "h", 3, "i")
    assert c.read("h", 3, "i") == -5
    c.write(25.0, "w", 10, "r")
    assert c.read("w", 10, "r") == 25.0
    raw_write(c, CIO_BIT, 100, 5, b"\x01", 1)
    assert c.read("c", 100, "ui") == 1 << 5
    assert raw_read(c, CIO_BIT, 100, 4, 3) == b"\x00\x01\x00"
    r = c.memory_area_read(DM, struct.pack(">HB", 9999, 0), 2)
    assert r[12:14] == b"\x11\x03"


def project(tmp_path, mode, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "FINS",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Omron FINS Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "CJ2",
            "servermain.DEVICE_MODEL": "CJ2",
            "servermain.DEVICE_ID_STRING": "127.0.0.1",
            "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 500,
            "omron_fins.DEVICE_PROTOCOL": mode.upper(),
            "omron_fins.DEVICE_PORT": port,
            "tags": [
                tag("Count", "D100"),
                tag("Neg", "D101", 4),
                tag("Total", "D102", 7),
                tag("Temp", "W10", 8),
                tag("Name", "D110.6", 0),
                tag("Bit", "D120.15"),
                tag("Run", "CIO100.05"),
                tag("Lamps", "CIO0.00 [16]", 21),
                tag("Hold", "H3", 4),
                tag("Bank", "E2:50"),
                tag("TimerPV", "T5"),
                tag("TimerDone", "T5", 1),
                tag("CounterPV", "C7"),
                tag("Beyond", "D9999", 7),
            ],
        }],
    }]}}
    path = tmp_path / "fins.json"
    path.write_text(json.dumps(p))
    return path


async def good(node, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good():
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.StatusCode}")
        await asyncio.sleep(0.1)


@pytest.mark.asyncio
async def test_driver_against_simulator(daemon_factory, plc, tmp_path):
    mode, port, c = plc
    c.write(4321, "d", 100, "ui")
    c.write(-2, "d", 101, "i")
    c.write(0x12345678, "d", 102, "udi")
    c.write(25.0, "w", 10, "r")
    raw_write(c, DM, 110, 0, b"ABC\x00", 2)
    raw_write(c, DM, 120, 0, b"\x80\x00", 1)
    raw_write(c, CIO_BIT, 100, 5, b"\x01", 1)
    raw_write(c, CIO_BIT, 0, 10, b"\x01", 1)
    c.write(-7, "h", 3, "i")
    raw_write(c, b"\xA2", 50, 0, b"\x00\x2A", 1)
    raw_write(c, TIMER_PV, 5, 0, b"\x00\x4D", 1)
    raw_write(c, TIMER_PV, 0x8007, 0, b"\x00\x09", 1)

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, mode, port))
    async with Client(d.url) as ua_client:
        n = lambda t: ua_client.get_node(f"ns=2;s=FINS.CJ2.{t}")  # noqa: E731
        assert await good(n("Count")) == 4321
        assert await good(n("Neg")) == -2
        assert await good(n("Total")) == 0x12345678
        assert await good(n("Temp")) == 25.0
        assert await good(n("Name")) == "ABC"
        assert await good(n("Bit")) is True
        assert await good(n("Run")) is True
        lamps = await good(n("Lamps"))
        assert lamps[10] is True and lamps[0] is False
        assert await good(n("Hold")) == -7
        assert await good(n("Bank")) == 42
        assert await good(n("TimerPV")) == 77
        assert await good(n("TimerDone")) is False
        assert await good(n("CounterPV")) == 9
        for _ in range(30):
            dv = await n("Beyond").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        assert await good(n("Count")) == 4321  # still fine next to the bad tag

        # Writes, verified with the fins package.
        await n("Count").write_value(ua.DataValue(ua.Variant(1000, ua.VariantType.UInt16)))
        assert c.read("d", 100, "ui") == 1000
        await n("Total").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        assert c.read("d", 102, "udi") == 70000
        await n("Temp").write_value(ua.DataValue(ua.Variant(-1.5, ua.VariantType.Float)))
        assert c.read("w", 10, "r") == -1.5
        await n("Bit").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert c.read("d", 120, "ui") == 0
        await n("Run").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert raw_read(c, CIO_BIT, 100, 5, 1) == b"\x00"
        await n("Lamps").write_value(ua.DataValue(ua.Variant([i % 2 == 1 for i in range(16)], ua.VariantType.Boolean)))
        assert c.read("c", 0, "ui") == 0xAAAA
        await n("Name").write_value(ua.DataValue(ua.Variant("XYZW", ua.VariantType.String)))
        assert raw_read(c, DM, 110, 0, 3) == b"XYZW\x00\x00"
        await n("Bank").write_value(ua.DataValue(ua.Variant(7, ua.VariantType.UInt16)))
        assert raw_read(c, b"\xA2", 50, 0, 1) == b"\x00\x07"
