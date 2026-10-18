"""Modbus RTU driver over a virtual serial line (socat) against pymodbus RTU slaves."""

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
SIM = Path(__file__).with_name("modbus_rtu_sim.py")


@pytest.fixture
def serial_line(tmp_path):
    a, b = tmp_path / "ttyA", tmp_path / "ttyB"
    socat = subprocess.Popen([SOCAT, f"pty,raw,echo=0,link={a}", f"pty,raw,echo=0,link={b}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if a.exists() and b.exists():
            break
        time.sleep(0.05)
    # Linux ptys reject parity settings from pyserial (EINVAL), so the virtual line runs 8N1.
    sim = subprocess.Popen([sys.executable, str(SIM), str(b), "19200", "N"])
    time.sleep(1.0)
    yield a
    sim.terminate()
    sim.wait(timeout=5)
    socat.terminate()
    socat.wait(timeout=5)


def project(tmp_path, tty):
    dev = lambda name, station, tags: {  # noqa: E731
        "common.ALLTYPES_NAME": name,
        "servermain.DEVICE_ID_STRING": str(station),
        "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 300,
        "servermain.DEVICE_RETRY_ATTEMPTS": 1,
        "servermain.DEVICE_AUTO_DEMOTION_ENABLE_ON_COMMUNICATIONS_FAILURES": True,
        "servermain.DEVICE_AUTO_DEMOTION_DEMOTE_AFTER_SUCCESSIVE_TIMEOUTS": 2,
        "servermain.DEVICE_AUTO_DEMOTION_PERIOD_MS": 3000,
        "tags": [{"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a} for n, a in tags],
    }
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "RS485",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Modbus RTU Serial",
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT": str(tty),
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE": 19200,
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY": "None",
        "devices": [
            dev("Meter1", 1, [("A", "40001"), ("B", "40002"), ("Set", "40010")]),
            dev("Meter2", 2, [("A", "40001")]),
            dev("Missing", 9, [("A", "40001")]),
        ],
    }]}}
    path = tmp_path / "rtu.json"
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


async def test_rtu_stations_share_one_line(daemon_factory, serial_line, tmp_path):
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, serial_line))
    async with Client(d.url) as c:
        n = lambda dev, tag: c.get_node(f"ns=2;s=RS485.{dev}.{tag}")  # noqa: E731
        assert await value(n("Meter1", "A")) == 1111
        assert await value(n("Meter1", "B")) == 2222
        assert await value(n("Meter2", "A")) == 3333

        # A station that never answers goes bad (and is demoted) without
        # taking the line down for the others.
        for _ in range(40):
            dv = await n("Missing", "A").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.is_bad() and dv.StatusCode.value != ua.StatusCodes.BadWaitingForInitialData:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value in (ua.StatusCodes.BadTimeout, ua.StatusCodes.BadNotConnected)

        await n("Meter1", "Set").write_value(ua.DataValue(ua.Variant(4321, ua.VariantType.UInt16)))
        assert await value(n("Meter1", "Set"), want=4321) == 4321
        assert await value(n("Meter2", "A")) == 3333
