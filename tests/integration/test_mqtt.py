"""MQTT agent end-to-end tests against a real Mosquitto broker and the simulator project."""

import json
import queue
import shutil
import subprocess
import time
import uuid

import paho.mqtt.client as paho
import pytest
from paho.mqtt.properties import Properties
from paho.mqtt.packettypes import PacketTypes

from conftest import free_port, wait_for_port

MOSQUITTO = shutil.which("mosquitto")
pytestmark = pytest.mark.skipif(MOSQUITTO is None, reason="mosquitto broker not installed")

INSECURE = {"security_none": True, "allow_anonymous": True}


class Broker:
    def __init__(self, tmp, port=None):
        self.port = port or free_port()
        self.conf = tmp / f"mosquitto-{self.port}.conf"
        self.conf.write_text(f"listener {self.port} 127.0.0.1\nallow_anonymous true\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen([MOSQUITTO, "-c", str(self.conf)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for_port(self.port, self.proc)

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self.proc.wait(timeout=5)
            self.proc = None


@pytest.fixture
def broker(tmp_path):
    b = Broker(tmp_path)
    b.start()
    yield b
    b.stop()


class Listener:
    """Test client collecting messages per topic."""

    def __init__(self, port, topics, v5=False):
        self.q = queue.Queue()
        proto = paho.MQTTv5 if v5 else paho.MQTTv311
        self.c = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"t-{uuid.uuid4().hex[:8]}", protocol=proto)
        self.c.on_message = lambda c, u, m: self.q.put(m)
        self.c.connect("127.0.0.1", port)
        for t in topics:
            self.c.subscribe(t, qos=1)
        self.c.loop_start()
        time.sleep(0.3)

    def next(self, topic=None, timeout=10.0, pred=None):
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.q.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                break
            if (topic is None or m.topic == topic) and (pred is None or pred(m)):
                return m
        raise AssertionError(f"no message on {topic} within {timeout}s")

    def publish(self, topic, payload, properties=None):
        self.c.publish(topic, payload, qos=1, properties=properties).wait_for_publish(5)

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()


def agent(port, items, **props):
    a = {
        "common.ALLTYPES_NAME": "Agent1",
        "iot_gateway.MQTT_CLIENT_URL": f"tcp://127.0.0.1:{port}",
        "iot_gateway.MQTT_CLIENT_TOPIC": "plant/line1",
        "iot_gateway.AGENTTYPES_RATE_MS": 300,
        "iot_items": [
            {"iot_gateway.IOT_ITEM_SERVER_TAG": t, "iot_gateway.IOT_ITEM_SCAN_RATE_MS": 100} for t in items
        ],
    }
    a.update(props)
    return {"mqtt_clients": [a]}


def test_standard_narrow_payload(daemon_factory, broker):
    lis = Listener(broker.port, ["plant/line1"])
    daemon_factory(INSECURE, iot_gateway=agent(broker.port, ["Simulator.Device1.Ramp", "Simulator.Device1.Line1.State"]))
    msg = json.loads(lis.next("plant/line1").payload)
    assert isinstance(msg["timestamp"], int)
    ids = {v["id"] for v in msg["values"]}
    assert ids == {"Simulator.Device1.Ramp", "Simulator.Device1.Line1.State"}  # initial update for every item
    for v in msg["values"]:
        assert set(v) == {"id", "v", "q", "t"} and v["q"] is True
    # Later messages carry only changed tags (narrow format).
    later = json.loads(lis.next("plant/line1", pred=lambda m: True).payload)
    assert all(v["id"] in ids for v in later["values"])
    lis.close()


def test_wide_format_has_every_item(daemon_factory, broker):
    lis = Listener(broker.port, ["plant/line1"])
    daemon_factory(INSECURE, iot_gateway=agent(broker.port, ["Simulator.Device1.Setpoint", "Simulator.Device1.Recipe"],
                                               **{"iot_gateway.AGENTTYPES_PUBLISH_FORMAT": 1}))
    for _ in range(2):  # static values, still published every interval
        msg = json.loads(lis.next("plant/line1").payload)
        assert {v["id"] for v in msg["values"]} == {"Simulator.Device1.Setpoint", "Simulator.Device1.Recipe"}
    lis.close()


def test_per_tag_topics_and_write_with_mqtt5_response(daemon_factory, broker):
    lis = Listener(broker.port, ["plant/#", "replies/#"], v5=True)
    gw = agent(broker.port, ["Simulator.Device1.Setpoint", "Simulator.Device1.Ramp"], **{
        "iot_gateway.AGENTTYPES_PUBLISH_TYPE": 1,
        "logict.MQTT_PROTOCOL_VERSION": 5,
        "logict.MQTT_TOPIC_MODE": 1,
        "logict.MQTT_TAG_TOPIC_TEMPLATE": "{topic}/{device}/{tag}",
        "iot_gateway.MQTT_CLIENT_ENABLE_WRITE_TOPIC": True,
        "iot_gateway.MQTT_CLIENT_WRITE_TOPIC": "plant/line1/write",
        "logict.MQTT_WRITE_QOS": 1,
    })
    daemon_factory(INSECURE, iot_gateway=gw)
    first = json.loads(lis.next("plant/line1/Device1/Setpoint").payload)
    assert first["values"][0]["v"] == 0

    props = Properties(PacketTypes.PUBLISH)
    props.ResponseTopic = "replies/abc"
    props.CorrelationData = b"req-42"
    lis.publish("plant/line1/write", json.dumps({"id": "Simulator.Device1.Setpoint", "v": 777}), props)
    resp = lis.next("replies/abc")
    assert json.loads(resp.payload) == {"id": "Simulator.Device1.Setpoint", "result": "Good", "code": 0}
    assert resp.properties.CorrelationData == b"req-42"
    upd = lis.next("plant/line1/Device1/Setpoint", pred=lambda m: json.loads(m.payload)["values"][0]["v"] == 777)
    assert upd is not None

    # Kepware rule: only tags configured on the agent are writable; read-only tags are refused.
    props.CorrelationData = b"req-43"
    lis.publish("plant/line1/write", json.dumps([
        {"id": "Simulator.Device1.Running", "v": True},
        {"id": "Simulator.Device1.Ramp", "v": 1},
        {"id": "Simulator.Device1.Setpoint", "v": -1},
    ]), props)
    results = json.loads(lis.next("replies/abc", pred=lambda m: m.properties.CorrelationData == b"req-43").payload)
    assert [r["result"] for r in results] == ["BadNodeIdUnknown", "BadNotWritable", "BadOutOfRange"]
    lis.close()


def test_last_will_online_shutdown_and_crash(daemon_factory, broker):
    lis = Listener(broker.port, ["plant/status"])
    gw = agent(broker.port, ["Simulator.Device1.Ramp"], **{
        "iot_gateway.MQTT_CLIENT_ENABLE_LAST_WILL": True,
        "iot_gateway.MQTT_CLIENT_LAST_WILL_TOPIC": "plant/status",
        "iot_gateway.MQTT_CLIENT_LAST_WILL_MESSAGE": "offline",
    })
    d = daemon_factory(INSECURE, iot_gateway=gw)
    assert lis.next("plant/status").payload == b"Server is online."
    assert d.stop() == 0
    assert lis.next("plant/status").payload == b"Server is shutting down."

    d2 = daemon_factory(INSECURE, iot_gateway=gw)
    assert lis.next("plant/status").payload == b"Server is online."
    d2.kill()
    assert lis.next("plant/status", timeout=90).payload == b"offline"  # broker publishes the will (keepalive 60s)
    lis.close()


def test_buffers_while_broker_is_down(daemon_factory, tmp_path):
    b = Broker(tmp_path)
    gw = agent(b.port, ["Simulator.Device1.Ramp"], **{"iot_gateway.AGENTTYPES_RATE_MS": 200})
    d = daemon_factory(INSECURE, iot_gateway=gw)
    time.sleep(2.5)  # Ramp(1000ms) changes ~2 times with no broker
    b.start()
    try:
        lis = Listener(b.port, ["plant/line1"])
        values = []
        end = time.time() + 15
        while time.time() < end and len(values) < 3:
            values += json.loads(lis.next("plant/line1", timeout=15).payload)["values"]
        assert len(values) >= 3, values  # initial + changes made while disconnected
        ts = [v["t"] for v in values]
        assert ts == sorted(ts) and ts[0] < time.time() * 1000 - 1500  # oldest is from before the broker came up
        lis.close()
    finally:
        d.stop()
        b.stop()


def test_advanced_template_csv(daemon_factory, broker):
    lis = Listener(broker.port, ["plant/line1"])
    gw = agent(broker.port, ["Simulator.Device1.Setpoint"], **{
        "iot_gateway.AGENTTYPES_MESSAGE_FORMAT": 1,
        "iot_gateway.AGENTTYPES_ADVANCED_TEMPLATE": "|#each VALUES||TAG|;|VALUE|;|QUALITY||LINEFEED||/each|",
    })
    daemon_factory(INSECURE, iot_gateway=gw)
    assert lis.next("plant/line1").payload == b"Setpoint;0;true\n"
    lis.close()


def test_agent_status_tags_in_opcua(daemon_factory, broker):
    from asyncua import Client
    import asyncio

    d = daemon_factory(INSECURE, iot_gateway=agent(broker.port, ["Simulator.Device1.Ramp"]))
    lis = Listener(broker.port, ["plant/line1"])
    lis.next("plant/line1")

    async def check():
        async with Client(d.url) as c:
            assert await c.get_node("ns=2;s=_IoT_Gateway.Agent1._Connected").read_value() is True
            assert await c.get_node("ns=2;s=_IoT_Gateway.Agent1._PublishesSent").read_value() >= 1
            assert await c.get_node("ns=2;s=_IoT_Gateway.Agent1._DroppedEvents").read_value() == 0

    asyncio.run(check())
    lis.close()
