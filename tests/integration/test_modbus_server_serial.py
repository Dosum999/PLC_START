"""Modbus Server Serial: logictd answers as a slave on a virtual serial line
(socat); pymodbus is the master. Values written by the master show up in the
tags and tag writes are read by the master, for RTU and ASCII framing."""

import asyncio
import json
import shutil
import subprocess
import time

import pytest
from asyncua import Client, ua
from pymodbus.client import ModbusSerialClient
from pymodbus.exceptions import ModbusIOException
from pymodbus.framer import FramerType

SOCAT = shutil.which("socat")
pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(SOCAT is None, reason="socat not installed")]


@pytest.fixture(params=["RTU", "ASCII"])
def line(request, tmp_path):
    a, b = tmp_path / "ttyA", tmp_path / "ttyB"
    socat = subprocess.Popen([SOCAT, f"pty,raw,echo=0,link={a}", f"pty,raw,echo=0,link={b}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if a.exists() and b.exists():
            break
        time.sleep(0.05)
    yield a, b, request.param
    socat.terminate()
    socat.wait(timeout=5)


def project(tmp_path, tty, protocol):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Line",
        "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Modbus Server Serial",
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT": str(tty),
        "servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE": 19200,
        "modbus_server_serial.CHANNEL_PROTOCOL": protocol,
        "devices": [
            {"common.ALLTYPES_NAME": "S1", "servermain.DEVICE_ID_STRING": "1", "tags": [
                tag("Hold", "40001"), tag("Total", "40002", 7), tag("Coil", "000001", 1), tag("Input", "300001"),
                tag("DI", "100001", 1), tag("Bit", "40010.3"), tag("InBit", "300011.2"), tag("Arr", "40030 [3]", 24),
                tag("Name", "40040.6H", 0)]},
            {"common.ALLTYPES_NAME": "S2", "servermain.DEVICE_ID_STRING": "2", "tags": [tag("Hold", "40001")]},
        ],
    }]}}
    path = tmp_path / "server.json"
    path.write_text(json.dumps(p))
    return path


async def value(node, want, timeout=8.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and dv.Value.Value == want:
            return
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value} {dv.StatusCode}, want {want}")
        await asyncio.sleep(0.1)


async def test_modbus_server_serial(daemon_factory, line, tmp_path):
    tty_server, tty_master, protocol = line
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, tty_server, protocol))
    master = ModbusSerialClient(str(tty_master), framer=FramerType[protocol], baudrate=19200, timeout=1, retries=0)
    assert master.connect()
    call = lambda f, *a, **k: asyncio.to_thread(f, *a, **k)  # noqa: E731
    try:
        async with Client(d.url) as c:
            n = lambda dev, t: c.get_node(f"ns=2;s=Line.{dev}.{t}")  # noqa: E731
            await value(n("S1", "Hold"), 0)

            # Tags -> master, including the input areas a master cannot write.
            await n("S1", "Hold").write_value(ua.DataValue(ua.Variant(7, ua.VariantType.UInt16)))
            await n("S1", "Input").write_value(ua.DataValue(ua.Variant(1234, ua.VariantType.UInt16)))
            await n("S1", "DI").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
            await n("S1", "InBit").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
            await n("S1", "Name").write_value(ua.DataValue(ua.Variant("LOGICT", ua.VariantType.String)))
            r = await call(master.read_holding_registers, 0, count=1, device_id=1)
            assert not r.isError() and r.registers == [7]
            r = await call(master.read_input_registers, 0, count=1, device_id=1)
            assert not r.isError() and r.registers == [1234]
            r = await call(master.read_input_registers, 10, count=1, device_id=1)
            assert r.registers == [1 << 2]
            r = await call(master.read_discrete_inputs, 0, count=1, device_id=1)
            assert not r.isError() and r.bits[0] is True
            r = await call(master.read_holding_registers, 39, count=3, device_id=1)
            assert r.registers == [0x4C4F, 0x4749, 0x4354]                           # "LOGICT", high byte first
            r = await call(master.read_holding_registers, 0, count=1, device_id=2)
            assert r.registers == [0]                                                 # stations are separate

            # Master -> tags.
            assert not (await call(master.write_registers, 1, [0x5678, 0x1234], device_id=1)).isError()
            assert not (await call(master.write_coil, 0, True, device_id=1)).isError()
            assert not (await call(master.write_register, 9, 1 << 3, device_id=1)).isError()
            assert not (await call(master.write_registers, 29, [1, 65535, 3], device_id=1)).isError()
            assert not (await call(master.write_register, 0, 99, device_id=2)).isError()
            await value(n("S1", "Total"), 0x12345678)
            await value(n("S1", "Coil"), True)
            await value(n("S1", "Bit"), True)
            await value(n("S1", "Arr"), [1, -1, 3])
            await value(n("S2", "Hold"), 99)

            # Read/write multiple (23): the write happens before the read.
            r = await call(master.readwrite_registers, read_address=1, read_count=2, write_address=1,
                           values=[0xAAAA, 0xBBBB], device_id=1)
            assert not r.isError() and r.registers == [0xAAAA, 0xBBBB]
            await value(n("S1", "Total"), 0xBBBBAAAA)

            # Exceptions and stations nobody serves.
            r = await call(master.read_holding_registers, 65535, count=2, device_id=1)
            assert r.isError() and r.exception_code == 2
            with pytest.raises(ModbusIOException):                                    # no answer
                await call(master.read_holding_registers, 0, count=1, device_id=9)
            r = await call(master.read_holding_registers, 0, count=1, device_id=1)
            assert r.registers == [7]                                                 # line still in step
    finally:
        master.close()
