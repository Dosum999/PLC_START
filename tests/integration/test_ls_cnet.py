"""LS XGT Cnet driver over a virtual serial line against the Cnet station simulator."""

import asyncio
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest
from asyncua import Client, ua

SOCAT = shutil.which("socat")
pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(SOCAT is None, reason="socat not installed")]
SIM = Path(__file__).with_name("cnet_sim.py")


@pytest.fixture
def line(tmp_path):
    a, b = tmp_path / "ttyA", tmp_path / "ttyB"
    socat = subprocess.Popen([SOCAT, f"pty,raw,echo=0,link={a}", f"pty,raw,echo=0,link={b}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if a.exists() and b.exists():
            break
        time.sleep(0.05)
    sim = subprocess.Popen([sys.executable, str(SIM), str(b), "1", "5"])
    time.sleep(0.5)
    yield a
    sim.terminate()
    sim.wait(timeout=5)
    socat.terminate()
    socat.wait(timeout=5)


def project(tmp_path, tty, bcc=True):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    dev = lambda name, st, model, tags: {  # noqa: E731
        "common.ALLTYPES_NAME": name, "servermain.DEVICE_MODEL": model, "servermain.DEVICE_ID_STRING": str(st),
        "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 300, "servermain.DEVICE_RETRY_ATTEMPTS": 1,
        "ls_xgt_cnet.DEVICE_USE_BCC": bcc, "tags": tags}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Cnet",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "LS XGT Cnet",
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT": str(tty),
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE": 38400,
        "devices": [
            dev("XGB1", 1, "XGB", [tag("Counter", "D100"), tag("Speed", "D102", 8), tag("Run", "M0010A", 1),
                                   tag("Alarm", "D104.3"), tag("Name", "D110.6", 0), tag("Trend", "D120 [4]", 24),
                                   tag("Bad", "D9000")]),
            dev("XEC5", 5, "XEC", [tag("Level", "%MW50"), tag("Flag", "%MX16")]),
            dev("Gone", 9, "XGB", [tag("X", "D0")]),
        ],
    }]}}
    path = tmp_path / "cnet.json"
    path.write_text(json.dumps(p))
    return path


async def value(node, timeout=8.0, want=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (want is None or dv.Value.Value == want):
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


@pytest.mark.parametrize("bcc", [True, False])
async def test_cnet_stations(daemon_factory, line, tmp_path, bcc):
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, line, bcc))
    async with Client(d.url) as c:
        n = lambda dev, t: c.get_node(f"ns=2;s=Cnet.{dev}.{t}")  # noqa: E731
        assert await value(n("XGB1", "Counter")) == 0
        # Writes, then read back over the same serial protocol.
        await n("XGB1", "Counter").write_value(ua.DataValue(ua.Variant(4660, ua.VariantType.UInt16)))
        await n("XGB1", "Speed").write_value(ua.DataValue(ua.Variant(12.5, ua.VariantType.Float)))
        await n("XGB1", "Run").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        await n("XGB1", "Alarm").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        await n("XGB1", "Name").write_value(ua.DataValue(ua.Variant("LS-1", ua.VariantType.String)))
        await n("XGB1", "Trend").write_value(ua.DataValue(ua.Variant([1, -2, 3, -4], ua.VariantType.Int16)))
        await n("XEC5", "Level").write_value(ua.DataValue(ua.Variant(777, ua.VariantType.UInt16)))
        await n("XEC5", "Flag").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert await value(n("XGB1", "Counter"), want=4660) == 4660
        assert await value(n("XGB1", "Speed"), want=12.5) == 12.5
        assert await value(n("XGB1", "Run"), want=True) is True
        assert await value(n("XGB1", "Alarm"), want=True) is True
        assert await value(n("XGB1", "Name"), want="LS-1") == "LS-1"
        assert await value(n("XGB1", "Trend"), want=[1, -2, 3, -4]) == [1, -2, 3, -4]
        assert await value(n("XEC5", "Level"), want=777) == 777
        assert await value(n("XEC5", "Flag"), want=True) is True

        for _ in range(40):
            dv = await n("XGB1", "Bad").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        for _ in range(40):
            dv = await n("Gone", "X").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value in (ua.StatusCodes.BadTimeout, ua.StatusCodes.BadNotConnected):
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value in (ua.StatusCodes.BadTimeout, ua.StatusCodes.BadNotConnected)
        assert await value(n("XGB1", "Counter")) == 4660  # neighbours unaffected
