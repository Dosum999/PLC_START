"""Omron NJ/NX driver against libplctag's ab_server in Omron mode.

Values are preset and verified with libplctag's tag_rw2 tool (plc=omron-njnx),
as in test_rockwell.py. STRING variables are not covered: tag_rw2 itself does
not round-trip Omron strings against ab_server, so there is no oracle.
"""

import asyncio
import json
import subprocess

import pytest
from asyncua import Client, ua

from conftest import BUILD, PREFIX, free_port, wait_for_port

AB_SERVER = BUILD / "bin_dist" / "ab_server"
TAG_RW2 = BUILD / "bin_dist" / "tag_rw2"
pytestmark = [pytest.mark.asyncio,
              pytest.mark.skipif(not (AB_SERVER.exists() and TAG_RW2.exists()), reason="ab_server/tag_rw2 not built")]

TAGS = ["Counter:DINT[1]", "Temps:REAL[5]", "Run:BOOL[1]", "Ints:INT[10]", "Mask:DINT[1]"]


def rw2(port, typ, name, count=1, write=None):
    tag = f"protocol=ab-eip&gateway=127.0.0.1:{port}&path=18,127.0.0.1&plc=omron-njnx&name={name}&elem_count={count}"
    args = [*PREFIX, str(TAG_RW2), f"--type={typ}", f"--tag={tag}"]
    if write is not None:
        args.append("--write=" + ",".join(str(v) for v in write))
    out = subprocess.run(args, capture_output=True, text=True, timeout=20).stdout
    return [line.split("=", 1)[1].split(" ")[0].strip('"') for line in out.splitlines() if line.startswith("data[")]


@pytest.fixture
def plc():
    port = free_port()
    proc = subprocess.Popen([*PREFIX, str(AB_SERVER), "--plc=Omron", f"--port={port}", *[f"--tag={t}" for t in TAGS]],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(port, proc)
    yield port
    proc.terminate()
    proc.wait(timeout=5)


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "NJ",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Omron NJ Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "NJ501",
            "servermain.DEVICE_MODEL": "NJ",
            "servermain.DEVICE_ID_STRING": "127.0.0.1",
            "omron_nj.DEVICE_PORT": port,
            "tags": [
                tag("Counter", "Counter"),          # Default: DINT from the PLC
                tag("Temps", "Temps{5}"),
                tag("Temp2", "Temps[2]"),
                tag("Run", "Run"),
                tag("Ints", "Ints{10}", 24),         # Short array
                tag("MaskBit3", "Mask.3"),
                tag("Nope", "DoesNotExist"),
            ],
        }],
    }]}}
    path = tmp_path / "nj.json"
    path.write_text(json.dumps(p))
    return path


async def value(node, timeout=10.0, want=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (want is None or dv.Value.Value == want):
            return dv.Value.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_omron_nj_against_ab_server(daemon_factory, plc, tmp_path):
    port = plc
    rw2(port, "sint32", "Counter", write=[1234])
    rw2(port, "real32", "Temps", 5, write=[1.5, 2.5, 3.5, 4.5, 5.5])
    rw2(port, "sint16", "Ints", 10, write=list(range(10)))
    rw2(port, "sint32", "Mask", write=[8])
    assert rw2(port, "sint32", "Counter") == ["1234"]

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, port))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=NJ.NJ501.{t}")  # noqa: E731
        assert await value(n("Counter")) == 1234
        assert await value(n("Temps")) == [1.5, 2.5, 3.5, 4.5, 5.5]
        assert await value(n("Temp2")) == 3.5
        assert await value(n("Ints")) == list(range(10))
        assert await value(n("MaskBit3")) is True
        for _ in range(50):
            dv = await n("Nope").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.is_bad() and dv.StatusCode.value != ua.StatusCodes.BadWaitingForInitialData:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange

        await n("Counter").write_value(ua.DataValue(ua.Variant(-7, ua.VariantType.Int32)))
        assert rw2(port, "sint32", "Counter") == ["-7"]
        await n("Temp2").write_value(ua.DataValue(ua.Variant(9.25, ua.VariantType.Float)))
        assert rw2(port, "real32", "Temps", 5)[2] == "9.250000"
        await n("MaskBit3").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert rw2(port, "sint32", "Mask") == ["0"]
        await n("Run").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert await value(n("Run"), want=True) is True
