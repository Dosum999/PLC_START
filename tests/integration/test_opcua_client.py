"""OPC UA Client driver against an asyncua server (independent implementation)."""

import asyncio
import json
import socket

import pytest
import pytest_asyncio
from asyncua import Client, Server, ua
from asyncua.server.user_managers import UserManager

from conftest import free_port

pytestmark = pytest.mark.asyncio


class Users(UserManager):
    def get_user(self, iserver, username=None, password=None, certificate=None):
        from asyncua.crypto.permission_rules import User, UserRole
        if username == "op" and password == "secret":
            return User(role=UserRole.User)
        return None


@pytest_asyncio.fixture
async def ua_server():
    port = free_port()
    srv = Server(user_manager=Users())
    await srv.init()
    srv.set_endpoint(f"opc.tcp://127.0.0.1:{port}/plc")
    srv.set_security_IDs(["Username"])
    idx = await srv.register_namespace("urn:test:plc")
    obj = await srv.nodes.objects.add_object(idx, "Line")
    nodes = {
        "temp": await obj.add_variable(ua.NodeId("Temp", idx), "Temp", 21.5, ua.VariantType.Double),
        "count": await obj.add_variable(ua.NodeId(1001, idx), "Count", 7, ua.VariantType.Int32),
        "run": await obj.add_variable(ua.NodeId("Run", idx), "Run", False, ua.VariantType.Boolean),
        "name": await obj.add_variable(ua.NodeId("Name", idx), "Name", "batch-1", ua.VariantType.String),
        "prof": await obj.add_variable(ua.NodeId("Profile", idx), "Profile", [1.0, 2.0, 3.0], ua.VariantType.Float),
        "ro": await obj.add_variable(ua.NodeId("ReadOnly", idx), "ReadOnly", 5, ua.VariantType.UInt16),
    }
    for k, n in nodes.items():
        if k != "ro":
            await n.set_writable()
    async with srv:
        yield port, idx, nodes


def project(tmp_path, port, idx, user=True):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    dev = {
        "common.ALLTYPES_NAME": "PLC",
        "servermain.DEVICE_ID_STRING": f"opc.tcp://127.0.0.1:{port}/plc",
        "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS": 2000,
        "tags": [
            tag("Temp", f"ns={idx};s=Temp"),
            tag("Count", f"ns={idx};i=1001"),
            tag("CountF", f"ns={idx};i=1001", 9),     # configured type: Double
            tag("Run", f"ns={idx};s=Run"),
            tag("Name", f"ns={idx};s=Name"),
            tag("Profile", f"ns={idx};s=Profile"),
            tag("ReadOnly", f"ns={idx};s=ReadOnly"),
            tag("Missing", f"ns={idx};s=NoSuchNode"),
            tag("ServerState", "ns=0;i=2259"),
        ],
    }
    dev["opcua_client.DEVICE_PKI_DIR"] = str(tmp_path / "client-pki")
    dev["opcua_client.DEVICE_TRUST_ALL_SERVERS"] = True  # asyncua's self-signed test certificate
    if user:
        # This asyncua test server only offers a plaintext UserName token on its None endpoint.
        dev["opcua_client.DEVICE_ALLOW_PLAINTEXT_PASSWORD"] = True
        dev["opcua_client.DEVICE_USERNAME"] = "op"
        dev["opcua_client.DEVICE_PASSWORD"] = "secret"
    p = {"project": {"channels": [{"common.ALLTYPES_NAME": "UA", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "OPC UA Client",
                                   "devices": [dev]}]}}
    path = tmp_path / "ua.json"
    path.write_text(json.dumps(p))
    return path


async def good(node, timeout=8.0, want=None):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and (want is None or dv.Value.Value == want):
            return dv.Value
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value} {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_reads_and_writes_through_gateway(daemon_factory, ua_server, tmp_path):
    port, idx, nodes = ua_server
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, port, idx))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=UA.PLC.{t}")  # noqa: E731
        v = await good(n("Temp"))
        assert v.Value == 21.5 and v.VariantType == ua.VariantType.Double
        assert (await good(n("Count"))).Value == 7
        cf = await good(n("CountF"))
        assert cf.Value == 7.0 and cf.VariantType == ua.VariantType.Double
        assert (await good(n("Run"))).Value is False
        assert (await good(n("Name"))).Value == "batch-1"
        assert (await good(n("Profile"))).Value == [1.0, 2.0, 3.0]
        assert (await good(n("ServerState"))).Value == 0  # ServerState Running

        dv = await n("Missing").read_data_value(raise_on_bad_status=False)
        for _ in range(40):
            if dv.StatusCode.value == ua.StatusCodes.BadNodeIdUnknown:
                break
            await asyncio.sleep(0.1)
            dv = await n("Missing").read_data_value(raise_on_bad_status=False)
        assert dv.StatusCode.value == ua.StatusCodes.BadNodeIdUnknown

        # Value changes on the far server show up through the gateway.
        await nodes["temp"].write_value(30.25)
        await good(n("Temp"), want=30.25)

        # Writes go through to the far server with its own data types.
        await n("Count").write_value(ua.DataValue(ua.Variant(42, ua.VariantType.Int32)))
        assert await nodes["count"].read_value() == 42
        await n("Run").write_value(ua.DataValue(ua.Variant(True, ua.VariantType.Boolean)))
        assert await nodes["run"].read_value() is True
        await n("Name").write_value(ua.DataValue(ua.Variant("batch-2", ua.VariantType.String)))
        assert await nodes["name"].read_value() == "batch-2"
        await n("CountF").write_value(ua.DataValue(ua.Variant(9.0, ua.VariantType.Double)))
        assert await nodes["count"].read_value() == 9
        with pytest.raises(ua.UaStatusCodeError):
            await n("ReadOnly").write_value(ua.DataValue(ua.Variant(1, ua.VariantType.UInt16)))


async def test_wrong_credentials_mark_device_bad(daemon_factory, ua_server, tmp_path):
    port, idx, _ = ua_server
    path = project(tmp_path, port, idx)
    p = json.loads(path.read_text())
    p["project"]["channels"][0]["devices"][0]["opcua_client.DEVICE_PASSWORD"] = "wrong"
    path.write_text(json.dumps(p))
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=path)
    async with Client(d.url) as c:
        err = c.get_node("ns=2;s=UA.PLC._System._Error")
        temp = c.get_node("ns=2;s=UA.PLC.Temp")  # reading a tag makes the gateway connect
        for _ in range(50):
            await temp.read_data_value(raise_on_bad_status=False)
            if await err.read_value():
                break
            await asyncio.sleep(0.1)
        assert await err.read_value() is True


async def test_secure_connection_to_logict_server(daemon_factory, tmp_path):
    """Basic256Sha256 + SignAndEncrypt + user name against a second logictd (open62541 server)."""
    from conftest import Daemon, ROOT
    upstream_dir = tmp_path / "upstream"
    upstream_dir.mkdir()
    upstream = Daemon(upstream_dir, ROOT / "config" / "example-project.json", {})  # secure-only, anonymous off
    try:
        tag = {"common.ALLTYPES_NAME": "Ramp", "servermain.TAG_ADDRESS": "ns=2;s=Simulator.Device1.Ramp"}
        dev = {
            "common.ALLTYPES_NAME": "Up",
            "servermain.DEVICE_ID_STRING": upstream.url,
            "opcua_client.DEVICE_SECURITY_POLICY": "Basic256Sha256",
            "opcua_client.DEVICE_USERNAME": "admin",
            "opcua_client.DEVICE_PASSWORD": "logict",
            "opcua_client.DEVICE_PKI_DIR": str(tmp_path / "client-pki"),
            "opcua_client.DEVICE_TRUST_ALL_SERVERS": True,
            "tags": [tag],
        }
        p = {"project": {"channels": [{"common.ALLTYPES_NAME": "UA", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "OPC UA Client",
                                       "devices": [dev]}]}}
        path = tmp_path / "chain.json"
        path.write_text(json.dumps(p))

        # Reading the tag makes the gateway connect; the upstream server first
        # rejects the unknown client certificate.
        d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=path)
        async with Client(d.url) as c:
            ramp = c.get_node("ns=2;s=UA.Up.Ramp")
            rejected = upstream.pki / "ApplCerts" / "rejected" / "certs"
            for _ in range(80):
                await ramp.read_data_value(raise_on_bad_status=False)
                if rejected.exists() and any(rejected.iterdir()):
                    break
                await asyncio.sleep(0.1)
            certs = list(rejected.iterdir())
            assert certs, "client certificate should be rejected until trusted"
            assert (await ramp.read_data_value(raise_on_bad_status=False)).StatusCode.is_bad()
            trusted = upstream.pki / "ApplCerts" / "trusted" / "certs"
            for cert in certs:
                cert.rename(trusted / cert.name)
            # The upstream file store is re-read on the next handshake.
            v = await good(ramp, timeout=20)
            assert isinstance(v.Value, int)
    finally:
        upstream.stop()
