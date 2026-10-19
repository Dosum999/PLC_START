"""Memory Based and System Monitor drivers. System Monitor values are compared
with what Python reads from the same host (os, shutil)."""

import asyncio
import json
import os
import shutil
import socket

import pytest
from asyncua import Client, ua

pytestmark = pytest.mark.asyncio


def tag(name, address, data_type=None):
    t = {"common.ALLTYPES_NAME": name, "servermain.TAG_ADDRESS": address}
    if data_type is not None:
        t["servermain.TAG_DATA_TYPE"] = data_type
    return t


def project(tmp_path, driver, tags):
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Ch", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": driver,
        "devices": [{"common.ALLTYPES_NAME": "Dev", "tags": tags}],
    }]}}
    path = tmp_path / "project.json"
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


async def eventually(node, expected, timeout=8.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        value = await good(node)
        if value == expected or asyncio.get_running_loop().time() > end:
            assert value == expected
            return
        await asyncio.sleep(0.1)


async def test_memory_based(daemon_factory, tmp_path):
    tags = [tag("A", "RAM"), tag("B", "RAM"), tag("Count", "RAM", 7), tag("Name", "ram", 0),
            tag("Shared1", "RAM.level", 8), tag("Shared2", "RAM.Level", 4), tag("Arr", "RAM [4]", 24),
            tag("Bad", "ROM")]
    d = daemon_factory({"security_none": True, "allow_anonymous": True},
                       project=project(tmp_path, "Memory Based", tags))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=Ch.Dev.{t}")  # noqa: E731
        assert await good(n("A")) == 0.0
        assert await good(n("Name")) == ""
        assert await good(n("Arr")) == [0, 0, 0, 0]

        await n("A").write_value(ua.DataValue(ua.Variant(1.5, ua.VariantType.Double)))
        await n("Count").write_value(ua.DataValue(ua.Variant(70000, ua.VariantType.UInt32)))
        await n("Name").write_value(ua.DataValue(ua.Variant("hello", ua.VariantType.String)))
        await n("Arr").write_value(ua.DataValue(ua.Variant([1, -2, 3, -4], ua.VariantType.Int16)))
        await eventually(n("A"), 1.5)
        await eventually(n("Count"), 70000)
        await eventually(n("Name"), "hello")
        await eventually(n("Arr"), [1, -2, 3, -4])
        assert await good(n("B")) == 0.0                    # RAM: every tag has its own value

        await n("Shared1").write_value(ua.DataValue(ua.Variant(42.0, ua.VariantType.Float)))
        await eventually(n("Shared2"), 42)                  # RAM.<name>: shared, case-insensitive

        with pytest.raises(ua.UaStatusCodeError):           # wrong array length
            await n("Arr").write_value(ua.DataValue(ua.Variant([1, 2], ua.VariantType.Int16)))
        dv = await n("Bad").read_data_value(raise_on_bad_status=False)
        assert dv.StatusCode.value == ua.StatusCodes.BadConfigurationError


def default_interface():
    return "lo" if os.path.exists("/sys/class/net/lo") else sorted(os.listdir("/sys/class/net"))[0]


def meminfo():
    out = {}
    for line in open("/proc/meminfo"):
        key, rest = line.split(":", 1)
        out[key] = int(rest.split()[0]) * 1024
    return out


async def test_system_monitor(daemon_factory, tmp_path):
    iface = default_interface()
    tags = [tag("Cpu", "CPU.Usage"), tag("Cores", "CPU.Count"), tag("MemTotal", "Memory.Total"),
            tag("MemUsedPct", "memory.usedpercent"), tag("Load1", "Load.1"), tag("Uptime", "System.Uptime"),
            tag("Host", "System.Hostname"), tag("Procs", "System.ProcessCount"),
            tag("RootTotal", "Disk(/).Total"), tag("RootAvail", "Disk(/).Available"), tag("RootFree", "Disk(/).Free"), tag("RootPct", "Disk(/).UsedPercent"),
            tag("Rx", f"Network({iface}).RxBytes"), tag("Up", f"Network({iface}).Up"),
            tag("Daemons", "Process(logictd).Count"), tag("UptimeWord", "System.Uptime", 7),
            tag("NoDisk", "Disk(/no/such/path).Free"), tag("NoIface", "Network(nosuch0).RxBytes"),
            tag("BadItem", "CPU.Temperature"), tag("NoArg", "Disk.Free")]
    d = daemon_factory({"security_none": True, "allow_anonymous": True},
                       project=project(tmp_path, "System Monitor", tags))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=Ch.Dev.{t}")  # noqa: E731
        assert await good(n("Cores")) == os.cpu_count()
        assert await good(n("MemTotal")) == meminfo()["MemTotal"]
        assert 0 < await good(n("MemUsedPct")) < 100
        assert abs(await good(n("Load1")) - os.getloadavg()[0]) < 2.0
        uptime = float(open("/proc/uptime").read().split()[0])
        assert abs(await good(n("Uptime")) - uptime) < 10
        assert abs(await good(n("UptimeWord")) - uptime) < 10        # converted to DWord
        assert await good(n("Host")) == socket.gethostname()
        assert await good(n("Procs")) > 1
        disk = shutil.disk_usage("/")
        assert await good(n("RootTotal")) == disk.total
        assert abs(await good(n("RootAvail")) - disk.free) < 512 * 1024 * 1024     # shutil "free" = f_bavail
        assert await good(n("RootFree")) >= await good(n("RootAvail"))            # includes root-reserved blocks
        assert 0 <= await good(n("RootPct")) <= 100
        assert await good(n("Rx")) >= 0
        operstate = open(f"/sys/class/net/{iface}/operstate").read().strip()
        assert await good(n("Up")) is (operstate == "up")
        assert await good(n("Daemons")) >= 1

        await asyncio.sleep(1.5)                                       # CPU usage needs two samples
        assert 0 <= await good(n("Cpu")) <= 100

        for name in ("NoDisk", "NoIface"):
            dv = await n(name).read_data_value(raise_on_bad_status=False)
            assert dv.StatusCode.value in (ua.StatusCodes.BadOutOfRange, ua.StatusCodes.BadWaitingForInitialData)
        for name in ("BadItem", "NoArg"):
            dv = await n(name).read_data_value(raise_on_bad_status=False)
            assert dv.StatusCode.value == ua.StatusCodes.BadConfigurationError
        with pytest.raises(ua.UaStatusCodeError):
            await n("Cores").write_value(ua.DataValue(ua.Variant(3, ua.VariantType.UInt32)))
