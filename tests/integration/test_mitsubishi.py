"""Mitsubishi Ethernet (MC 3E binary) driver tests.

test_simulator_matches_pymcprotocol checks the simulator with pymcprotocol, an
independent client, so the driver tests below are not a closed loop.
"""

import asyncio
import json
import subprocess
import sys
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

pymc = pytest.importorskip("pymcprotocol")
SIM = Path(__file__).with_name("mc_sim.py")


def s16(*words):
    """pymcprotocol takes and returns signed 16-bit words."""
    return [w - 0x10000 if w >= 0x8000 else w for w in words]


@pytest.fixture
def plc():
    port = free_port()
    proc = subprocess.Popen([sys.executable, str(SIM), str(port)])
    wait_for_port(port, proc)
    mc = pymc.Type3E(plctype="Q")
    mc.connect("127.0.0.1", port)
    yield port, mc
    mc.close()
    proc.terminate()
    proc.wait(timeout=5)


def test_simulator_matches_pymcprotocol(plc):
    _, mc = plc
    mc.batchwrite_wordunits("D100", s16(1, 2, 0xFFFF))
    assert mc.batchread_wordunits("D100", 3) == [1, 2, -1]  # pymcprotocol returns signed words
    mc.batchwrite_bitunits("M10", [1, 0, 1])
    assert mc.batchread_bitunits("M10", 3) == [1, 0, 1]
    assert mc.batchread_wordunits("M0", 1) == [(1 << 10) | (1 << 12)]
    mc.batchwrite_bitunits("X1F", [1])
    assert mc.batchread_bitunits("X1E", 2) == [0, 1]
    with pytest.raises(Exception):
        mc.batchread_wordunits("D7999", 2)


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "MC",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Mitsubishi Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "Q03",
            "servermain.DEVICE_MODEL": "Q Series",
            "servermain.DEVICE_ID_STRING": "127.0.0.1",
            "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 500,
            "mitsubishi_ethernet.DEVICE_PORT": port,
            "tags": [
                tag("Count", "D100"),
                tag("Neg", "D101", 4),
                tag("Total", "D102", 7),
                tag("Temp", "D104", 8),
                tag("Name", "D110.6", 0),
                tag("Bit", "D120.F"),
                tag("Run", "M10"),
                tag("Lamps", "M0 [16]", 21),
                tag("MWord", "M16", 5),
                tag("Sensor", "X1F"),
                tag("Out", "Y20"),
                tag("Timer", "TN5"),
                tag("Beyond", "D7999", 7),
            ],
        }],
    }]}}
    path = tmp_path / "mc.json"
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
    port, mc = plc
    mc.batchwrite_wordunits("D100", s16(4321, 0xFFFE))                 # D100, D101 = -2
    mc.batchwrite_wordunits("D102", [0x5678, 0x1234])               # DWord low word first
    mc.batchwrite_wordunits("D104", [0x0000, 0x41C8])               # 25.0f
    mc.batchwrite_wordunits("D110", [0x4241, 0x0043])               # "ABC"
    mc.batchwrite_wordunits("D120", s16(0x8000))
    mc.batchwrite_bitunits("M10", [1])
    mc.batchwrite_bitunits("M16", [1, 1])
    mc.batchwrite_bitunits("X1F", [1])
    mc.batchwrite_wordunits("TN5", [77])

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, port))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=MC.Q03.{t}")  # noqa: E731
        assert await good(n("Count")) == 4321
        assert await good(n("Neg")) == -2
        assert await good(n("Total")) == 0x12345678
        assert await good(n("Temp")) == 25.0
        assert await good(n("Name")) == "ABC"
        assert await good(n("Bit")) is True
        assert await good(n("Run")) is True
        lamps = await good(n("Lamps"))
        assert lamps[10] is True and lamps[0] is False
        assert await good(n("MWord")) == 0b11
        assert await good(n("Sensor")) is True
        assert await good(n("Timer")) == 77
        for _ in range(30):
            dv = await n("Beyond").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        assert await good(n("Count")) == 4321  # still fine next to the bad tag

        # Writes, verified with pymcprotocol.
        await n("Count").write_value(ua.DataValue(ua.Variant(1000, ua.VariantType.UInt16)))
        assert mc.batchread_wordunits("D100", 1) == [1000]
        await n("Total").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        assert mc.batchread_wordunits("D102", 2) == [70000 & 0xFFFF, 70000 >> 16]
        await n("Bit").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert mc.batchread_wordunits("D120", 1) == [0]
        await n("Out").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert mc.batchread_bitunits("Y20", 1) == [1]
        await n("Lamps").write_value(ua.DataValue(ua.Variant([i % 2 == 1 for i in range(16)], ua.VariantType.Boolean)))
        assert mc.batchread_bitunits("M0", 4) == [0, 1, 0, 1]
        await n("Name").write_value(ua.DataValue(ua.Variant("XYZW", ua.VariantType.String)))
        assert mc.batchread_wordunits("D110", 3) == [0x5958, 0x575A, 0]
