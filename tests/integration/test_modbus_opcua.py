"""End to end: Modbus TCP device (pymodbus) -> logictd -> OPC UA client."""

import asyncio
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest
from asyncua import Client, ua
from pymodbus.client import AsyncModbusTcpClient

from conftest import free_port, wait_for_port

pytestmark = pytest.mark.asyncio

SIM = Path(__file__).with_name("modbus_sim.py")


class Sim:
    def __init__(self, port: int):
        self.port = port
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen([sys.executable, str(SIM), str(self.port)])
        wait_for_port(self.port, self.proc)

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self.proc.wait(timeout=5)
            self.proc = None


@pytest.fixture
def sim():
    s = Sim(free_port())
    s.start()
    yield s
    s.stop()


def project(tmp_path: Path, port: int) -> Path:
    tag = lambda name, addr, dtype=None, **extra: {  # noqa: E731
        "common.ALLTYPES_NAME": name,
        "servermain.TAG_ADDRESS": addr,
        **({"servermain.TAG_DATA_TYPE": dtype} if dtype is not None else {}),
        **extra,
    }
    p = {
        "project": {
            "channels": [
                {
                    "common.ALLTYPES_NAME": "Line1",
                    "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "Modbus TCP/IP Ethernet",
                    "devices": [
                        {
                            "common.ALLTYPES_NAME": "PLC1",
                            "servermain.DEVICE_ID_STRING": "127.0.0.1.1",
                            "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 300,
                            "servermain.DEVICE_RETRY_ATTEMPTS": 1,
                            "modbus_tcp.DEVICE_PORT": port,
                            "tags": [
                                tag("Counter", "40001"),
                                tag("Temperature", "40011", 8),
                                tag("Setpoint", "40021"),
                                tag("Run", "40031.3"),
                                tag("Pump", "000001"),
                                tag("Missing", "40500"),
                            ],
                        }
                    ],
                }
            ]
        }
    }
    path = tmp_path / "modbus-project.json"
    path.write_text(json.dumps(p))
    return path


async def read_until(node, pred, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if pred(dv):
            return dv
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: last value {dv.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_modbus_device_through_opcua(daemon_factory, sim, tmp_path):
    mb = AsyncModbusTcpClient("127.0.0.1", port=sim.port)
    await mb.connect()
    await mb.write_register(0, 4242)
    # 21.5 as float32, Kepware default "first word low" (CDAB).
    hi, lo = struct.unpack(">HH", struct.pack(">f", 21.5))
    await mb.write_registers(10, [lo, hi])

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, sim.port))
    async with Client(d.url) as c:
        node = lambda n: c.get_node(f"ns=2;s=Line1.PLC1.{n}")  # noqa: E731
        good = lambda dv: dv.StatusCode.is_good()  # noqa: E731

        assert (await read_until(node("Counter"), good)).Value.Value == 4242
        assert (await read_until(node("Temperature"), good)).Value.Value == pytest.approx(21.5)
        missing = await read_until(node("Missing"), lambda dv: dv.StatusCode.value == ua.StatusCodes.BadOutOfRange)
        assert missing.StatusCode.value == ua.StatusCodes.BadOutOfRange

        # OPC UA writes reach the device.
        await node("Setpoint").write_value(ua.DataValue(ua.Variant(4321, ua.VariantType.UInt16)))
        assert (await mb.read_holding_registers(20, count=1)).registers == [4321]
        await node("Run").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert (await mb.read_holding_registers(30, count=1)).registers == [8]
        await node("Pump").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert (await mb.read_coils(0, count=1)).bits[0] is True

        # Device-side changes show up in a subscription.
        seen = []

        class Handler:
            def datachange_notification(self, n, val, data):
                seen.append(val)

        sub = await c.create_subscription(100, Handler())
        await sub.subscribe_data_change(node("Counter"))
        await mb.write_register(0, 1)
        await mb.write_register(0, 2)
        for _ in range(30):
            if 2 in seen:
                break
            await asyncio.sleep(0.1)
        assert 2 in seen, seen
        await sub.delete()

        # Losing the device turns tags bad and raises _System._Error; it recovers on its own.
        mb.close()
        sim.stop()
        await read_until(node("Counter"), lambda dv: dv.StatusCode.is_bad() and not dv.StatusCode.value == ua.StatusCodes.BadOutOfRange)
        assert await c.get_node("ns=2;s=Line1.PLC1._System._Error").read_value() is True
        sim.start()
        assert (await read_until(node("Counter"), good, timeout=10)).Value.Value == 0  # fresh simulator
        assert await c.get_node("ns=2;s=Line1.PLC1._System._Error").read_value() is False
