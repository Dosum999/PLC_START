"""CIMON Serial and Ethernet drivers against a simulator written from the
CIMON PLC manual. The serial frames are pinned to the manual's examples in
the unit tests; the Ethernet byte order is inferred from the manual and is
not verified against a PLC."""

import asyncio
import json
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

pytestmark = pytest.mark.asyncio
SIM = Path(__file__).with_name("cimon_sim.py")
SOCAT = shutil.which("socat")


class Control:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.file = self.sock.makefile("rw")

    def get(self, dev, start, count=1):
        self.file.write(f"get {dev} {start} {count}\n")
        self.file.flush()
        return [int(v) for v in self.file.readline().split()]

    def set(self, dev, start, *values):
        self.file.write(f"set {dev} {start} {' '.join(str(v) for v in values)}\n")
        self.file.flush()
        assert self.file.readline().strip() == "ok"


@pytest.fixture(params=["serial", "enet"])
def plc(request, tmp_path):
    mode = request.param
    control = free_port()
    procs = []
    if mode == "serial":
        if SOCAT is None:
            pytest.skip("socat not installed")
        a, b = tmp_path / "ttyA", tmp_path / "ttyB"
        procs.append(subprocess.Popen([SOCAT, f"pty,raw,echo=0,link={a}", f"pty,raw,echo=0,link={b}"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for _ in range(50):
            if a.exists() and b.exists():
                break
            time.sleep(0.05)
        sim = subprocess.Popen([sys.executable, str(SIM), "serial", str(b), str(control)])
        where = a
    else:
        port = free_port()
        sim = subprocess.Popen([sys.executable, str(SIM), "enet", str(port), str(control)])
        wait_for_port(port, sim)
        where = port
    procs.insert(0, sim)
    wait_for_port(control, sim)
    yield mode, where, Control(control)
    for p in procs:
        p.terminate()
        p.wait(timeout=5)


def project(tmp_path, mode, where):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    tags = [tag("Count", "D100"), tag("Neg", "D101", 4), tag("Total", "D102", 7), tag("Temp", "D104", 8),
            tag("Name", "D110.6", 0), tag("Bit", "D120.15"), tag("Run", "M0104"), tag("Lamps", "M0000 [16]", 21),
            tag("MWord", "M0011", 5), tag("Out", "Y0010"), tag("TimerCur", "TC5"), tag("TimerDone", "T5"),
            tag("Beyond", "D9999", 7)]
    if mode == "serial":
        channel = {"servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "CIMON Serial",
                   "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT": str(where),
                   "servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE": 19200}
        device = {"servermain.DEVICE_ID_STRING": "1"}
    else:
        channel = {"servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "CIMON Ethernet"}
        device = {"servermain.DEVICE_ID_STRING": "127.0.0.1", "cimon_ethernet.DEVICE_PORT": where}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "CIMON", **channel,
        "devices": [{"common.ALLTYPES_NAME": "CP", "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 500,
                     "servermain.DEVICE_RETRY_ATTEMPTS": 1, **device, "tags": tags}],
    }]}}
    path = tmp_path / "cimon.json"
    path.write_text(json.dumps(p))
    return path


async def good(node, timeout=8.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good():
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_cimon_driver(daemon_factory, plc, tmp_path):
    mode, where, ctl = plc
    ctl.set("D", 100, 4321, 0xFFFE, 0x5678, 0x1234, 0x0000, 0x41C8)   # Count, Neg, Total (low word first), 25.0f
    ctl.set("D", 110, 0x4142, 0x4300)                                  # "ABC", first character in the high byte
    ctl.set("D", 120, 0x8000)
    ctl.set("M", 10, 1 << 4)                                           # M0104
    ctl.set("M", 0, 1 << 10)
    ctl.set("M", 11, 0x1234)
    ctl.set("TC", 5, 100)
    ctl.set("TS", 5, 100)

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, mode, where))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=CIMON.CP.{t}")  # noqa: E731
        assert await good(n("Count")) == 4321
        assert await good(n("Neg")) == -2
        assert await good(n("Total")) == 0x12345678
        assert await good(n("Temp")) == 25.0
        assert await good(n("Name")) == "ABC"
        assert await good(n("Bit")) is True
        assert await good(n("Run")) is True
        lamps = await good(n("Lamps"))
        assert lamps[10] is True and lamps[0] is False
        assert await good(n("MWord")) == 0x1234
        assert await good(n("TimerCur")) == 100
        assert await good(n("TimerDone")) is True
        for _ in range(40):
            dv = await n("Beyond").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        assert await good(n("Count")) == 4321

        await n("Total").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        assert ctl.get("D", 102, 2) == [70000 & 0xFFFF, 70000 >> 16]
        await n("Bit").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert ctl.get("D", 120) == [0]
        await n("Out").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert ctl.get("Y", 1) == [1]                                   # Y0010 = word 1, bit 0
        await n("Lamps").write_value(ua.DataValue(ua.Variant([i % 2 == 1 for i in range(16)], ua.VariantType.Boolean)))
        assert ctl.get("M", 0) == [0xAAAA]
        await n("Name").write_value(ua.DataValue(ua.Variant("XYZW", ua.VariantType.String)))
        assert ctl.get("D", 110, 3) == [0x5859, 0x5A57, 0]
        await n("MWord").write_value(ua.DataValue(ua.Variant(7, ua.VariantType.UInt16)))
        assert ctl.get("M", 11) == [7]
