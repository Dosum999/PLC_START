"""Keyence KV Ethernet (host link) driver tests.

test_simulator_matches_kv_hostlink checks the simulator with the kv-hostlink
package, an independent client, so the driver tests below are not a closed loop.
"""

import asyncio
import json
import subprocess
import sys
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port, wait_for_port

pytest.importorskip("hostlink")
from hostlink import HostLinkClient  # noqa: E402
from hostlink.errors import HostLinkError  # noqa: E402

SIM = Path(__file__).with_name("kv_sim.py")


@pytest.fixture
def plc():
    port = free_port()
    proc = subprocess.Popen([sys.executable, str(SIM), str(port)])
    wait_for_port(port, proc)
    kv = HostLinkClient("127.0.0.1", port=port, plc_profile="keyence:kv-8000")
    yield port, kv
    kv.close()
    proc.terminate()
    proc.wait(timeout=5)


def test_simulator_matches_kv_hostlink(plc):
    _, kv = plc
    kv.write_consecutive("DM100", [1, 2, 65535], data_format=".U")
    assert kv.read_consecutive("DM100", 3, data_format=".U") == [1, 2, 65535]
    assert kv.read("DM102", data_format=".S") == -1
    kv.write("DM200", 0x12345678, data_format=".D")
    assert kv.read_consecutive("DM200", 2, data_format=".U") == [0x5678, 0x1234]  # low word first
    kv.write_consecutive("R1014", [1, 0, 1])                                   # R1014, R1015, R1100
    assert kv.read_consecutive("R1014", 3) == [1, 0, 1]
    assert kv.read("R1100", data_format=".U") == 1
    kv.write("B1F", 1)
    assert kv.read("B10", data_format=".H") == "8000"
    kv.write("TS5", 100, data_format=".D")
    kv.write("TC5", 100, data_format=".D")
    assert str(kv.send_raw("RD T5.D")).startswith("1,")
    with pytest.raises(HostLinkError):
        kv.read_consecutive("DM9999", 2, data_format=".U")


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "KV",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Keyence KV Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "KV8000",
            "servermain.DEVICE_MODEL": "KV-8000",
            "servermain.DEVICE_ID_STRING": "127.0.0.1",
            "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 500,
            "keyence_kv.DEVICE_PORT": port,
            "tags": [
                tag("Count", "DM100"),
                tag("Neg", "DM101", 4),
                tag("Total", "DM102", 7),
                tag("Temp", "DM104", 8),
                tag("Name", "DM110.6", 0),
                tag("Bit", "DM120.15"),
                tag("Run", "R1015"),
                tag("Relays", "MR0 [16]", 21),
                tag("RWord", "R200", 5),
                tag("Link", "B1F"),
                tag("Out", "Y20"),
                tag("TimerCur", "TC5"),
                tag("TimerDone", "T5"),
                tag("Bank", "EM50"),
                tag("Beyond", "DM9999", 7),
            ],
        }],
    }]}}
    path = tmp_path / "kv.json"
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
    port, kv = plc
    kv.write_consecutive("DM100", [4321, 65534], data_format=".U")   # DM101 = -2
    kv.write("DM102", 0x12345678, data_format=".D")
    kv.write_consecutive("DM104", [0x0000, 0x41C8], data_format=".U")  # 25.0f, low word first
    kv.write_consecutive("DM110", [0x4142, 0x4300], data_format=".U")  # "ABC"
    kv.write("DM120", 0x8000, data_format=".U")
    kv.write("R1015", 1)
    kv.write("MR010", 1)
    kv.write_consecutive("R200", [1, 1])
    kv.write("B1F", 1)
    kv.write("TS5", 100, data_format=".D")
    kv.write("TC5", 100, data_format=".D")
    kv.write("EM50", 42, data_format=".U")

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, port))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=KV.KV8000.{t}")  # noqa: E731
        assert await good(n("Count")) == 4321
        assert await good(n("Neg")) == -2
        assert await good(n("Total")) == 0x12345678
        assert await good(n("Temp")) == 25.0
        assert await good(n("Name")) == "ABC"
        assert await good(n("Bit")) is True
        assert await good(n("Run")) is True
        relays = await good(n("Relays"))
        assert relays[10] is True and relays[0] is False
        assert await good(n("RWord")) == 0b11
        assert await good(n("Link")) is True
        assert await good(n("TimerCur")) == 100
        assert await good(n("TimerDone")) is True
        assert await good(n("Bank")) == 42
        for _ in range(30):
            dv = await n("Beyond").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        assert await good(n("Count")) == 4321  # still fine next to the bad tag

        # Writes, verified with kv-hostlink.
        await n("Count").write_value(ua.DataValue(ua.Variant(1000, ua.VariantType.UInt16)))
        assert kv.read("DM100", data_format=".U") == 1000
        await n("Total").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        assert kv.read("DM102", data_format=".D") == 70000
        await n("Temp").write_value(ua.DataValue(ua.Variant(-1.5, ua.VariantType.Float)))
        assert kv.read_consecutive("DM104", 2, data_format=".U") == [0x0000, 0xBFC0]
        await n("Bit").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert kv.read("DM120", data_format=".U") == 0
        await n("Out").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert kv.read("Y20") == 1
        await n("Relays").write_value(ua.DataValue(ua.Variant([i % 2 == 1 for i in range(16)], ua.VariantType.Boolean)))
        assert kv.read_consecutive("MR000", 4) == [0, 1, 0, 1]
        await n("RWord").write_value(ua.DataValue(ua.Variant(0x8001, ua.VariantType.UInt16)))
        assert kv.read_consecutive("R200", 2) == [1, 0] and kv.read("R215") == 1
        await n("Name").write_value(ua.DataValue(ua.Variant("XYZW", ua.VariantType.String)))
        assert kv.read_consecutive("DM110", 3, data_format=".H") == ["5859", "5A57", "0000"]
        await n("TimerCur").write_value(ua.DataValue(ua.Variant(7, ua.VariantType.UInt32)))
        assert kv.read("TC5", data_format=".D") == 7
