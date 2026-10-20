"""MQTT Client driver against a real Mosquitto broker; paho-mqtt (Python)
publishes the values and checks what the driver publishes on writes."""

import asyncio
import json

import pytest
from asyncua import Client, ua

from test_mqtt import MOSQUITTO, Broker, Listener

pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(MOSQUITTO is None, reason="mosquitto broker not installed")]


@pytest.fixture
def broker(tmp_path):
    b = Broker(tmp_path)
    b.start()
    yield b
    b.stop()


def tag(name, address, data_type=None):
    t = {"common.ALLTYPES_NAME": name, "servermain.TAG_ADDRESS": address}
    if data_type is not None:
        t["servermain.TAG_DATA_TYPE"] = data_type
    return t


def project(tmp_path, port, version):
    tags = [tag("Temp", "plant/line1|temp"), tag("State", "plant/line1|state", 0), tag("Third", "plant/line1|$.v[2]", 5),
            tag("Num", "plant/num", 8), tag("Raw", "plant/raw"), tag("Arr", "plant/arr [3]", 24),
            tag("Wild", "plant/+/status"), tag("Cmd", "plant/cmd|speed"), tag("Flag", "plant/flag", 1)]
    device = lambda name, extra, tags: {  # noqa: E731
        "common.ALLTYPES_NAME": name, "servermain.DEVICE_ID_STRING": f"tcp://127.0.0.1:{port}",
        "servermain.DEVICE_CONNECT_TIMEOUT_SECONDS": 2, "mqtt_client.DEVICE_PROTOCOL_VERSION": version,
        **extra, "tags": tags}
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Mqtt", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "MQTT Client",
        "devices": [device("Broker", {}, tags),
                    device("Stale", {"mqtt_client.DEVICE_STALE_TIMEOUT_SECONDS": 1}, [tag("Num", "plant/num", 8)])],
    }]}}
    path = tmp_path / "mqtt-client.json"
    path.write_text(json.dumps(p))
    return path


async def value(node, want, timeout=10.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.is_good() and dv.Value.Value == want:
            return dv
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value} {dv.StatusCode}, want {want}")
        await asyncio.sleep(0.1)


async def status(node, code, timeout=10.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.value == code:
            return
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.StatusCode}, want {code:#x}")
        await asyncio.sleep(0.1)


@pytest.mark.parametrize("version", ["3.1.1", "5"])
async def test_mqtt_client(daemon_factory, broker, tmp_path, version):
    lis = Listener(broker.port, ["plant/cmd", "plant/raw"])
    lis.c.publish("plant/num", "12.5", qos=1, retain=True).wait_for_publish(5)   # delivered on subscribe

    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, broker.port, version))
    async with Client(d.url) as c:
        n = lambda t, dev="Broker": c.get_node(f"ns=2;s=Mqtt.{dev}.{t}")  # noqa: E731
        await value(n("Num"), 12.5)
        await status(n("Temp"), ua.StatusCodes.BadWaitingForInitialData)

        lis.publish("plant/line1", json.dumps({"temp": 21.5, "state": "RUN", "v": [1, 2, 3]}))
        lis.publish("plant/arr", "[1, -2, 3]")
        lis.publish("plant/flag", "true")
        dv = await value(n("Temp"), 21.5)
        assert dv.SourceTimestamp is not None
        await value(n("State"), "RUN")
        await value(n("Third"), 3)
        await value(n("Arr"), [1, -2, 3])
        await value(n("Flag"), True)

        lis.publish("plant/a/status", "ok")
        await value(n("Wild"), "ok")
        lis.publish("plant/b/status", "fault")
        await value(n("Wild"), "fault")                                        # newest message on any match

        lis.publish("plant/line1", json.dumps({"state": "STOP"}))
        await value(n("State"), "STOP")
        await status(n("Temp"), ua.StatusCodes.BadOutOfRange)                  # member missing from the payload

        # Writes publish to the tag's topic.
        await n("Cmd").write_value(ua.DataValue(ua.Variant(40.5, ua.VariantType.Double)))
        assert json.loads(lis.next("plant/cmd").payload) == {"speed": 40.5}
        await n("Raw").write_value(ua.DataValue(ua.Variant("hello", ua.VariantType.String)))
        assert lis.next("plant/raw").payload == b"hello"
        await value(n("Raw"), "hello")                                         # the driver's own subscription
        with pytest.raises(ua.UaStatusCodeError):
            await n("Wild").write_value(ua.DataValue(ua.Variant("x", ua.VariantType.String)))

        await status(n("Num", "Stale"), ua.StatusCodes.UncertainLastUsableValue)  # 1 s without a message

        # Broker restart: the device reconnects and subscribes again.
        broker.stop()
        await asyncio.sleep(1.0)
        broker.start()
        lis.close()
        lis = Listener(broker.port, [])
        lis.publish("plant/line1", json.dumps({"temp": 30.0}))
        end = asyncio.get_running_loop().time() + 30
        while (await n("Temp").read_data_value(raise_on_bad_status=False)).Value.Value != 30.0:
            assert asyncio.get_running_loop().time() < end, "no value after broker restart"
            lis.publish("plant/line1", json.dumps({"temp": 30.0}))
            await asyncio.sleep(0.5)
    lis.close()
