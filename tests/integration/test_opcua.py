"""OPC UA server end-to-end tests against the simulator project."""

import asyncio
import shutil
import subprocess

import pytest
from asyncua import Client, ua

pytestmark = pytest.mark.asyncio

INSECURE = {"security_none": True, "allow_anonymous": True}


async def read_good(node, timeout=3.0):
    """Reads until the value is Good (first read of an unpolled tag starts its polling)."""
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good():
            return dv
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node} stayed {dv.StatusCode}")
        await asyncio.sleep(0.1)


async def test_browse_uses_kepware_node_ids(daemon_factory):
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        assert await c.get_namespace_index("urn:logict:tags") == 2
        children = {(await n.read_browse_name()).Name for n in await c.nodes.objects.get_children()}
        assert {"Simulator", "_System"} <= children
        dev = c.get_node("ns=2;s=Simulator.Device1")
        names = {(await n.read_browse_name()).Name for n in await dev.get_children()}
        assert {"Ramp", "Setpoint", "Line1", "_System"} <= names
        assert await c.get_node("ns=2;s=Simulator.Device1.Line1.State").read_data_type_as_variant_type() == ua.VariantType.String


async def test_read_scaled_and_system_tags(daemon_factory):
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        temp = await read_good(c.get_node("ns=2;s=Simulator.Device1.Temperature"))
        assert temp.Value.VariantType == ua.VariantType.Double
        assert 0.0 <= temp.Value.Value <= 100.0
        version = await c.get_node("ns=2;s=_System._ProductVersion").read_value()
        assert version == "0.1.0"
        err = await c.get_node("ns=2;s=Simulator.Offline._System._Error").read_value()
        assert isinstance(err, bool)


async def test_write_and_read_back(daemon_factory):
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        sp = c.get_node("ns=2;s=Simulator.Device1.Setpoint")
        await sp.write_value(ua.DataValue(ua.Variant(1234, ua.VariantType.UInt16)))
        await asyncio.sleep(0.3)
        assert (await read_good(sp)).Value.Value == 1234

        prof = c.get_node("ns=2;s=Simulator.Device1.Profile")
        await prof.write_value(ua.DataValue(ua.Variant([1, 2, 3, 4], ua.VariantType.UInt16)))
        await asyncio.sleep(0.3)
        assert (await read_good(prof)).Value.Value == [1, 2, 3, 4]


async def test_write_errors(daemon_factory):
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        with pytest.raises(ua.UaStatusCodeError) as e:
            await c.get_node("ns=2;s=Simulator.Device1.Ramp").write_value(ua.DataValue(ua.Variant(5, ua.VariantType.Int32)))
        assert e.value.code in (ua.StatusCodes.BadNotWritable, ua.StatusCodes.BadUserAccessDenied)
        # The stack enforces the node's DataType (UInt16) before the value reaches the device.
        with pytest.raises(ua.UaStatusCodeError) as e:
            await c.get_node("ns=2;s=Simulator.Device1.Setpoint").write_value(ua.DataValue(ua.Variant(-1, ua.VariantType.Int32)))
        assert e.value.code == ua.StatusCodes.BadTypeMismatch
        with pytest.raises(ua.UaStatusCodeError):
            await c.get_node("ns=2;s=Simulator.Offline.Value").write_value(ua.DataValue(ua.Variant(1, ua.VariantType.UInt16)))


async def test_subscription_delivers_changes(daemon_factory):
    d = daemon_factory(INSECURE)
    changes = []

    class Handler:
        def datachange_notification(self, node, val, data):
            changes.append(val)

    async with Client(d.url) as c:
        sub = await c.create_subscription(100, Handler())
        await sub.subscribe_data_change(c.get_node("ns=2;s=Simulator.Device1.Sine"))
        await asyncio.sleep(2.0)
        await sub.delete()
    assert len(changes) >= 3, changes
    assert all(-10.0 <= v <= 10.0 for v in changes if v is not None)


async def test_anonymous_rejected_by_default(daemon_factory):
    d = daemon_factory({"security_none": True})
    with pytest.raises(Exception):
        async with Client(d.url) as c:
            await c.nodes.objects.get_children()


@pytest.fixture
def client_cert(tmp_path):
    openssl = shutil.which("openssl")
    if not openssl:
        pytest.skip("openssl CLI not available")
    key, pem, der = tmp_path / "client.key.pem", tmp_path / "client.pem", tmp_path / "client.der"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-keyout", str(key),
                    "-out", str(pem), "-subj", "/CN=logict-test", "-addext",
                    "subjectAltName=URI:urn:logict:test,DNS:localhost", "-addext",
                    "keyUsage=digitalSignature,nonRepudiation,keyEncipherment,dataEncipherment", "-addext",
                    "extendedKeyUsage=clientAuth"], check=True, capture_output=True)
    subprocess.run([openssl, "x509", "-in", str(pem), "-outform", "der", "-out", str(der)], check=True)
    return der, key


async def test_secure_endpoint_with_username(daemon_factory, client_cert):
    der, key = client_cert
    d = daemon_factory({}, trusted_certs=[der])  # production defaults: no None endpoint, anonymous off

    c = Client(d.url)
    c.application_uri = "urn:logict:test"
    await c.set_security_string(f"Basic256Sha256,SignAndEncrypt,{der},{key}")
    c.set_user("admin")
    c.set_password("logict")
    async with c:
        dv = await read_good(c.get_node("ns=2;s=Simulator.Device1.Ramp"))
        assert dv.Value.VariantType == ua.VariantType.Int32

    bad = Client(d.url)
    bad.application_uri = "urn:logict:test"
    await bad.set_security_string(f"Basic256Sha256,SignAndEncrypt,{der},{key}")
    bad.set_user("admin")
    bad.set_password("wrong")
    with pytest.raises(Exception):
        async with bad:
            pass


async def test_untrusted_client_certificate_is_rejected(daemon_factory, client_cert):
    der, key = client_cert
    d = daemon_factory({})
    c = Client(d.url)
    c.application_uri = "urn:logict:test"
    await c.set_security_string(f"Basic256Sha256,SignAndEncrypt,{der},{key}")
    c.set_user("admin")
    c.set_password("logict")
    with pytest.raises(Exception):
        async with c:
            pass
    assert list((d.pki / "ApplCerts" / "rejected" / "certs").iterdir()), "untrusted cert should be stored in rejected/"


async def test_startup_never_writes_to_devices(daemon_factory):
    """Regression: building the address space must not push default values to PLCs."""
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        await asyncio.sleep(0.5)
        assert await c.get_node("ns=2;s=Simulator._Statistics._SuccessfulWrites").read_value() == 0
        assert await c.get_node("ns=2;s=Simulator._Statistics._FailedWrites").read_value() == 0


async def test_no_x509_user_tokens_advertised(daemon_factory):
    """The stack's default accept-all session PKI must not be exposed."""
    d = daemon_factory(INSECURE)
    endpoints = await Client(d.url).connect_and_get_server_endpoints()
    token_types = {t.TokenType for e in endpoints for t in e.UserIdentityTokens}
    assert ua.UserTokenType.Certificate not in token_types
    assert ua.UserTokenType.UserName in token_types


async def test_bad_quality_reaches_client(daemon_factory):
    d = daemon_factory(INSECURE)
    async with Client(d.url) as c:
        node = c.get_node("ns=2;s=Simulator.Offline.Value")
        for _ in range(30):
            dv = await node.read_data_value(raise_on_bad_status=False)
            if dv.StatusCode.value == ua.StatusCodes.BadNotConnected:
                break
            await asyncio.sleep(0.1)
        assert dv.StatusCode.value == ua.StatusCodes.BadNotConnected
