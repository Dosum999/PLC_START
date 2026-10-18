"""Siemens S7 driver vs. python-snap7's S7 server (independent implementation)."""

import asyncio
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

snap7 = pytest.importorskip("snap7")
from snap7.type import Areas  # noqa: E402

pytestmark = pytest.mark.asyncio
SIM = Path(__file__).with_name("s7_sim.py")


class Plc:
    def __init__(self):
        self.port = free_port()
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen([sys.executable, str(SIM), str(self.port)], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        wait_for_port(self.port, self.proc)
        self.client = snap7.client.Client()
        self.client.connect("127.0.0.1", 0, 1, self.port)

    def stop(self):
        self.client.disconnect()
        self.proc.terminate()
        self.proc.wait(timeout=5)


@pytest.fixture
def plc():
    p = Plc()
    p.start()
    yield p
    if p.proc.poll() is None:
        p.stop()


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "S7",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Siemens TCP/IP Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "CPU1500",
            "servermain.DEVICE_MODEL": "S7-1500",
            "servermain.DEVICE_ID_STRING": "127.0.0.1",
            "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 500,
            "servermain.DEVICE_RETRY_ATTEMPTS": 1,
            "siemens_tcpip.DEVICE_PORT": port,
            "tags": [
                tag("Speed", "DB1.DBW10"),
                tag("Temp", "DB1.DBD20", 8),
                tag("Valve", "DB1.DBX30.3"),
                tag("Recipe", "DB1.STRING40.10"),
                tag("Profile", "DB1.DBW100 [5]", 24),
                tag("Big", "DB1.DBB200 [600]", 23),
                tag("Total", "DB2.DBD0", 6),
                tag("Flags", "MW0"),
                tag("Input", "I0.1"),
                tag("Output", "QB1"),
                tag("NoDB", "DB9.DBW0"),
            ],
        }],
    }]}}
    path = tmp_path / "s7.json"
    path.write_text(json.dumps(p))
    return path


async def good(node, timeout=5.0, value=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (value is None or dv.Value.Value == value):
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_read_write_against_snap7_server(daemon_factory, plc, tmp_path):
    c7 = plc.client
    c7.db_write(1, 10, bytearray(struct.pack(">H", 1500)))
    c7.db_write(1, 20, bytearray(struct.pack(">f", 36.6)))
    c7.db_write(1, 30, bytearray([0b0000_1000]))
    c7.db_write(1, 40, bytearray([10, 5]) + b"HELLO")
    c7.db_write(1, 100, bytearray(struct.pack(">5h", 1, -2, 3, -4, 5)))
    c7.db_write(1, 200, bytearray(range(256)) * 2 + bytearray(88))
    c7.db_write(2, 0, bytearray(struct.pack(">i", -123456)))
    c7.write_area(Areas.MK, 0, 0, bytearray(struct.pack(">H", 0xBEEF)))
    c7.write_area(Areas.PE, 0, 0, bytearray([0b10]))

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, plc.port))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=S7.CPU1500.{t}")  # noqa: E731
        assert await good(n("Speed")) == 1500
        assert await good(n("Temp")) == pytest.approx(36.6, rel=1e-6)
        assert await good(n("Valve")) is True
        assert await good(n("Recipe")) == "HELLO"
        assert await good(n("Profile")) == [1, -2, 3, -4, 5]
        big = await good(n("Big"))  # larger than one PDU: split into several items
        assert len(big) == 600 and big[:3] == [0, 1, 2] and big[511] == 255 and big[512] == 0
        assert await good(n("Total")) == -123456
        assert await good(n("Flags")) == 0xBEEF
        assert await good(n("Input")) is True

        dv = await n("NoDB").read_data_value(raise_on_bad_status=False)
        for _ in range(30):
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
            dv = await n("NoDB").read_data_value(raise_on_bad_status=False)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange

        # Writes, verified with snap7's own client.
        await n("Speed").write_value(ua.DataValue(ua.Variant(2500, ua.VariantType.UInt16)))
        assert struct.unpack(">H", c7.db_read(1, 10, 2))[0] == 2500
        await n("Temp").write_value(ua.DataValue(ua.Variant(-1.25, ua.VariantType.Float)))
        assert struct.unpack(">f", c7.db_read(1, 20, 4))[0] == -1.25
        await n("Valve").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert c7.db_read(1, 30, 1)[0] == 0
        await n("Recipe").write_value(ua.DataValue(ua.Variant("ABC", ua.VariantType.String)))
        assert bytes(c7.db_read(1, 40, 5)) == bytes([10, 3]) + b"ABC"
        await n("Output").write_value(ua.DataValue(ua.Variant(0x5A, ua.VariantType.Byte)))
        assert c7.read_area(Areas.PA, 0, 1, 1)[0] == 0x5A
        await n("Big").write_value(ua.DataValue(ua.Variant([7] * 600, ua.VariantType.Byte)))
        assert c7.db_read(1, 200, 600) == bytearray([7] * 600)


async def test_reconnects_after_plc_restart(daemon_factory, plc, tmp_path):
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, plc.port))
    async with Client(d.url) as c:
        speed = c.get_node("ns=2;s=S7.CPU1500.Speed")
        assert await good(speed) == 0
        plc.stop()
        for _ in range(50):
            dv = await speed.read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.is_bad():
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.is_bad()
        assert await c.get_node("ns=2;s=S7.CPU1500._System._Error").read_value() is True
        plc.start()
        plc.client.db_write(1, 10, bytearray(struct.pack(">H", 99)))
        assert await good(speed, timeout=15, value=99) == 99
