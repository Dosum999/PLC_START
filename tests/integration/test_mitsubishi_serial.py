"""Mitsubishi Serial (3C/4C, ASCII formats 1 and 4) over a virtual serial line.

The simulator shares its device memory with a binary 3E TCP port, so values
are preset and verified with pymcprotocol from outside the serial path.
Frame layouts are pinned to the manual's examples in the unit tests.
"""

import asyncio
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

pymc = pytest.importorskip("pymcprotocol")
SOCAT = shutil.which("socat")
pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(SOCAT is None, reason="socat not installed")]
SIM = Path(__file__).with_name("mc_serial_sim.py")

# (frame, format, sum check)
VARIANTS = [("3C", 4, True), ("4C", 1, True), ("3C", 4, False)]


@pytest.fixture(params=VARIANTS, ids=lambda v: f"{v[0]}-f{v[1]}-{'sum' if v[2] else 'nosum'}")
def line(request, tmp_path):
    frame, fmt, use_sum = request.param
    a, b = tmp_path / "ttyA", tmp_path / "ttyB"
    socat = subprocess.Popen([SOCAT, f"pty,raw,echo=0,link={a}", f"pty,raw,echo=0,link={b}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if a.exists() and b.exists():
            break
        time.sleep(0.05)
    port = free_port()
    sim = subprocess.Popen([sys.executable, str(SIM), str(b), frame, str(fmt), "1" if use_sum else "0", str(port)],
                           cwd=Path(__file__).parent)
    wait_for_port(port, sim)
    mc = pymc.Type3E(plctype="Q")
    mc.connect("127.0.0.1", port)
    yield a, request.param, mc
    mc.close()
    sim.terminate()
    sim.wait(timeout=5)
    socat.terminate()
    socat.wait(timeout=5)


def project(tmp_path, tty, variant):
    frame, fmt, use_sum = variant
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    dev = lambda name, station, tags: {  # noqa: E731
        "common.ALLTYPES_NAME": name,
        "servermain.DEVICE_ID_STRING": str(station),
        "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 400,
        "servermain.DEVICE_RETRY_ATTEMPTS": 1,
        "mitsubishi_serial.DEVICE_FRAME": frame,
        "mitsubishi_serial.DEVICE_FORMAT": fmt,
        "mitsubishi_serial.DEVICE_SUM_CHECK": use_sum,
        "tags": tags,
    }
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "C24",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Mitsubishi Serial",
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT": str(tty),
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE": 19200,
        "devices": [
            dev("Q", 0, [tag("Count", "D100"), tag("Total", "D102", 7), tag("Name", "D110.6", 0),
                         tag("Bit", "D120.F"), tag("Run", "M10"), tag("Lamps", "M0 [16]", 21), tag("Sensor", "X1F"),
                         tag("Beyond", "D7999", 7)]),
            dev("Missing", 5, [tag("Count", "D100")]),
        ],
    }]}}
    path = tmp_path / "c24.json"
    path.write_text(json.dumps(p))
    return path


async def good(node, timeout=8.0, want=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (want is None or dv.Value.Value == want):
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_serial_driver(daemon_factory, line, tmp_path):
    tty, variant, mc = line
    mc.batchwrite_wordunits("D100", [4321])
    mc.batchwrite_wordunits("D102", [0x5678, 0x1234])
    mc.batchwrite_wordunits("D110", [0x4241, 0x0043])
    mc.batchwrite_wordunits("D120", [-0x8000])
    mc.batchwrite_bitunits("M10", [1])
    mc.batchwrite_bitunits("X1F", [1])

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, tty, variant))
    async with Client(d.url) as c:
        n = lambda dev, t: c.get_node(f"ns=2;s=C24.{dev}.{t}")  # noqa: E731
        assert await good(n("Q", "Count")) == 4321
        assert await good(n("Q", "Total")) == 0x12345678
        assert await good(n("Q", "Name")) == "ABC"
        assert await good(n("Q", "Bit")) is True
        assert await good(n("Q", "Run")) is True
        assert (await good(n("Q", "Lamps")))[10] is True
        assert await good(n("Q", "Sensor")) is True
        for _ in range(40):
            dv = await n("Q", "Beyond").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        for _ in range(40):
            dv = await n("Missing", "Count").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.is_bad() and dv.StatusCode.value != ua.StatusCodes.BadWaitingForInitialData:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value in (ua.StatusCodes.BadTimeout, ua.StatusCodes.BadNotConnected)

        await n("Q", "Total").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        assert mc.batchread_wordunits("D102", 2) == [70000 & 0xFFFF, 70000 >> 16]
        await n("Q", "Bit").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert mc.batchread_wordunits("D120", 1) == [0]
        await n("Q", "Lamps").write_value(ua.DataValue(ua.Variant([i % 2 == 1 for i in range(16)], ua.VariantType.Boolean)))
        assert mc.batchread_bitunits("M0", 4) == [0, 1, 0, 1]
        assert await good(n("Q", "Count")) == 4321  # the line still works for the answering station
