"""Allen-Bradley driver against libplctag's ab_server.

Values are preset and verified with libplctag's tag_rw2 command-line tool, so
the driver's own type mapping, offsets and bit handling are checked from the
outside. (pycomm3 cannot register a session with ab_server.)
"""

import asyncio
import json
import subprocess
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import BUILD, free_port, wait_for_port

AB_SERVER = BUILD / "bin_dist" / "ab_server"
TAG_RW2 = BUILD / "bin_dist" / "tag_rw2"
pytestmark = [pytest.mark.asyncio,
              pytest.mark.skipif(not (AB_SERVER.exists() and TAG_RW2.exists()), reason="ab_server/tag_rw2 not built")]


def rw2(port, typ, name, count=1, write=None):
    """Reads (returns list of strings) or writes a tag with libplctag's tag_rw2."""
    tag = f"protocol=ab-eip&gateway=127.0.0.1:{port}&path=1,0&plc=ControlLogix&name={name}&elem_count={count}"
    args = [str(TAG_RW2), f"--type={typ}", f"--tag={tag}"]
    if write is not None:
        args.append("--write=" + ",".join(str(v) for v in write))
    out = subprocess.run(args, capture_output=True, text=True, timeout=20).stdout
    return [line.split("=", 1)[1].split(" ")[0].strip('"') for line in out.splitlines() if line.startswith("data[")]

TAGS = ["Counter:DINT[1]", "Temps:REAL[5]", "Run:BOOL[1]", "Name:STRING[1]", "Big:LINT[1]", "Ints:INT[10]", "Small:SINT[1]",
        "Mask:DINT[1]"]


@pytest.fixture
def plc():
    port = free_port()
    proc = subprocess.Popen([str(AB_SERVER), "--plc=ControlLogix", "--path=1,0", f"--port={port}",
                             *[f"--tag={t}" for t in TAGS]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(port, proc)
    yield port
    proc.terminate()
    proc.wait(timeout=5)


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "AB",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Allen-Bradley ControlLogix Ethernet",
        "devices": [{
            "common.ALLTYPES_NAME": "L85",
            "servermain.DEVICE_MODEL": "ControlLogix",
            "servermain.DEVICE_ID_STRING": "127.0.0.1,1,0",
            "ab.DEVICE_PORT": port,
            "tags": [
                tag("Counter", "Counter"),          # Default: DINT from the PLC
                tag("Temps", "Temps{5}"),
                tag("Temp2", "Temps[2]"),
                tag("Run", "Run"),
                tag("Name", "Name"),
                tag("Big", "Big"),
                tag("Ints", "Ints{10}", 24),         # Short array
                tag("Small", "Small"),
                tag("MaskBit3", "Mask.3"),
                tag("Nope", "DoesNotExist"),
            ],
        }],
    }]}}
    path = tmp_path / "ab.json"
    path.write_text(json.dumps(p))
    return path


async def value(node, timeout=10.0, want=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (want is None or dv.Value.Value == want):
            return dv.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_logix_through_gateway(daemon_factory, plc, tmp_path):
    rw2(plc, "sint32", "Counter", write=[123456])
    rw2(plc, "real32", "Temps", 5, write=[1.5, 2.5, 3.5, 4.5, 5.5])
    rw2(plc, "uint8", "Run", write=[1])
    rw2(plc, "string", "Name", write=["hello"])
    rw2(plc, "sint16", "Ints", 10, write=list(range(-5, 5)))
    rw2(plc, "sint8", "Small", write=[-7])
    rw2(plc, "sint32", "Mask", write=[8])

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, plc))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=AB.L85.{t}")  # noqa: E731
        v = await value(n("Counter"))
        assert v.Value == 123456 and v.VariantType == ua.VariantType.Int32  # type resolved from the PLC
        assert (await value(n("Temps"))).Value == [1.5, 2.5, 3.5, 4.5, 5.5]
        t2 = await value(n("Temp2"))
        assert t2.Value == 3.5 and t2.VariantType == ua.VariantType.Float
        assert (await value(n("Run"))).Value is True
        assert (await value(n("Name"))).Value == "hello"
        await n("Big").write_value(ua.DataValue(ua.Variant(2**40, ua.VariantType.Int64)))
        assert (await value(n("Big"), want=2**40)).Value == 2**40
        assert (await value(n("Ints"))).Value == list(range(-5, 5))
        assert (await value(n("Small"))).Value == -7
        assert (await value(n("MaskBit3"))).Value is True

        for _ in range(50):
            dv = await n("Nope").read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadOutOfRange:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadOutOfRange
        assert (await value(n("Counter"))).Value == 123456  # unaffected by the bad tag

        await n("Counter").write_value(ua.DataValue(ua.Variant(-42, ua.VariantType.Int32)))
        await n("Temp2").write_value(ua.DataValue(ua.Variant(9.75, ua.VariantType.Float)))
        await n("Run").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        await n("Name").write_value(ua.DataValue(ua.Variant("logict", ua.VariantType.String)))
        await n("Ints").write_value(ua.DataValue(ua.Variant([7] * 10, ua.VariantType.Int16)))
        await n("MaskBit3").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))

    assert rw2(plc, "sint32", "Counter") == ["-42"]
    assert float(rw2(plc, "real32", "Temps", 5)[2]) == 9.75
    assert rw2(plc, "uint8", "Run") == ["0"]
    assert rw2(plc, "string", "Name") == ["logict"]
    assert rw2(plc, "sint16", "Ints", 10) == ["7"] * 10
    assert rw2(plc, "sint32", "Mask") == ["0"]


async def test_micrologix_pccc_files(daemon_factory, tmp_path):
    port = free_port()
    proc = subprocess.Popen([str(AB_SERVER), "--plc=Micrologix", f"--port={port}", "--tag=N7[10]", "--tag=F8[5]",
                             "--tag=B3[10]"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_port(port, proc)

        def pccc(typ, name, write=None):
            tag = f"protocol=ab-eip&gateway=127.0.0.1:{port}&plc=micrologix&name={name}&elem_count=1"
            args = [str(TAG_RW2), f"--type={typ}", f"--tag={tag}"] + ([f"--write={write}"] if write is not None else [])
            out = subprocess.run(args, capture_output=True, text=True, timeout=20).stdout
            return [l.split("=", 1)[1].split(" ")[0] for l in out.splitlines() if l.startswith("data[")]

        pccc("sint16", "N7:3", -1234)
        pccc("real32", "F8:1", 6.5)
        pccc("uint16", "B3:0", 0b100000)  # B3:0/5
        p = {"project": {"channels": [{
            "common.ALLTYPES_NAME": "AB",
            "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Allen-Bradley ControlLogix Ethernet",
            "devices": [{
                "common.ALLTYPES_NAME": "ML1400",
                "servermain.DEVICE_MODEL": "MicroLogix",
                "servermain.DEVICE_ID_STRING": "127.0.0.1",
                "ab.DEVICE_PORT": port,
                "tags": [
                    {"common.ALLTYPES_NAME": "N", "servermain.TAG_ADDRESS": "N7:3"},
                    {"common.ALLTYPES_NAME": "F", "servermain.TAG_ADDRESS": "F8:1"},
                    {"common.ALLTYPES_NAME": "B", "servermain.TAG_ADDRESS": "B3:0/5"},
                ],
            }],
        }]}}
        path = tmp_path / "ml.json"
        path.write_text(json.dumps(p))
        d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=path)
        async with Client(d.url) as c:
            n = lambda t: c.get_node(f"ns=2;s=AB.ML1400.{t}")  # noqa: E731
            assert (await value(n("N"))).Value == -1234
            assert (await value(n("F"))).Value == 6.5
            assert (await value(n("B"))).Value is True
            await n("N").write_value(ua.DataValue(ua.Variant(77, ua.VariantType.Int16)))
            await n("B").write_value(ua.DataValue(ua.Variant(False, ua.VariantType.Boolean)))
        assert pccc("sint16", "N7:3") == ["77"]
        assert pccc("uint16", "B3:0") == ["0"]
    finally:
        proc.terminate()
        proc.wait(timeout=5)
