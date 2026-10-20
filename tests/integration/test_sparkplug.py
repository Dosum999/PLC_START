"""MQTT Client as a Sparkplug B host application. The edge node is simulated
with the Eclipse Tahu protobuf module (sparkplug_b_pb2 + addMetric from
mqtt-spb-wrapper) and paho-mqtt against a real Mosquitto broker."""

import asyncio
import json
import queue
import threading
import time
import uuid

import paho.mqtt.client as paho
import pytest
from asyncua import Client, ua

from test_mqtt import MOSQUITTO, Broker

spb = pytest.importorskip("mqtt_spb_wrapper.spb_protobuf.sparkplug_b")
pb = pytest.importorskip("mqtt_spb_wrapper.spb_protobuf.sparkplug_b_pb2")
T = spb.MetricDataType
pytestmark = [pytest.mark.asyncio, pytest.mark.skipif(MOSQUITTO is None, reason="mosquitto broker not installed")]

G, E, D = "Plant1", "Edge1", "Press3"


class EdgeNode:
    """Minimal Sparkplug edge node: BIRTHs on request, echoes commands as DATA."""

    def __init__(self, port):
        self.seq = 0
        self.lock = threading.Lock()
        self.node = {"Motor/Speed": [1, T.Int32, -5], "Temp": [2, T.Float, 21.5], "Run": [3, T.Boolean, True],
                     "Name": [4, T.String, "L1"]}
        self.dev = {"Pressure": [10, T.Double, 1.5], "Count": [11, T.Int8, -1]}
        self.rebirths = 0
        self.commands = queue.Queue()
        self.c = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"edge-{uuid.uuid4().hex[:6]}")
        self.c.on_message = self.on_message
        self.c.connect("127.0.0.1", port)
        self.c.subscribe(f"spBv1.0/{G}/NCMD/{E}", qos=0)
        self.c.subscribe(f"spBv1.0/{G}/DCMD/{E}/{D}", qos=0)
        self.c.loop_start()
        time.sleep(0.3)

    def next_seq(self):
        with self.lock:
            s = self.seq
            self.seq = (self.seq + 1) % 256
            return s

    def send(self, kind, metrics, device=None, seq=None, by_alias=False):
        p = pb.Payload()
        p.timestamp = int(time.time() * 1000)
        if kind not in ("NDEATH", "DDEATH") or device:
            p.seq = self.next_seq() if seq is None else seq
        for name, (alias, dtype, value) in metrics.items():
            m = spb.addMetric(p, None if by_alias else name, alias, dtype, value)
            m.timestamp = p.timestamp
        topic = f"spBv1.0/{G}/{kind}/{E}" + (f"/{device}" if device else "")
        self.c.publish(topic, p.SerializeToString(), qos=0).wait_for_publish(5)

    def birth(self):
        with self.lock:
            self.seq = 0
        self.send("NBIRTH", self.node)
        self.send("DBIRTH", self.dev, device=D)

    def on_message(self, client, userdata, msg):
        p = pb.Payload()
        p.ParseFromString(msg.payload)
        for m in p.metrics:
            if m.name == "Node Control/Rebirth" and m.boolean_value:
                self.rebirths += 1
                threading.Thread(target=self.birth).start()
            else:
                self.commands.put((msg.topic, m))

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()


def project(tmp_path, port):
    tag = lambda n, a, t=None: {"common.ALLTYPES_NAME": n, "servermain.TAG_ADDRESS": a,  # noqa: E731
                                **({"servermain.TAG_DATA_TYPE": t} if t is not None else {})}
    tags = [tag("Speed", f"spb:{G}/{E}|Motor/Speed"), tag("Temp", f"spb:{G}/{E}|Temp"), tag("Run", f"spb:{G}/{E}|Run"),
            tag("Name", f"spb:{G}/{E}|Name"), tag("SpeedF", f"spb:{G}/{E}|Motor/Speed", 8),
            tag("Pressure", f"spb:{G}/{E}/{D}|Pressure"), tag("Count", f"spb:{G}/{E}/{D}|Count"),
            tag("Missing", f"spb:{G}/{E}|NoSuchMetric")]
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Spb", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "MQTT Client",
        "devices": [{"common.ALLTYPES_NAME": "Host", "servermain.DEVICE_ID_STRING": f"tcp://127.0.0.1:{port}",
                     "mqtt_client.DEVICE_SPARKPLUG_HOST_ID": "logict", "tags": tags}],
    }]}}
    path = tmp_path / "spb.json"
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
            return dv
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.StatusCode}, want {code:#x}")
        await asyncio.sleep(0.1)


def state_message(port):
    got = queue.Queue()
    c = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"st-{uuid.uuid4().hex[:6]}")
    c.on_message = lambda cl, u, m: got.put(json.loads(m.payload))
    c.connect("127.0.0.1", port)
    c.subscribe("spBv1.0/STATE/logict", qos=1)
    c.loop_start()
    try:
        return got.get(timeout=5)
    finally:
        c.loop_stop()
        c.disconnect()


async def test_sparkplug_host(daemon_factory, tmp_path):
    broker = Broker(tmp_path)
    broker.start()
    edge = EdgeNode(broker.port)
    edge.birth()                        # before the host is up: the host misses these BIRTHs
    try:
        d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, broker.port))
        async with Client(d.url) as c:
            n = lambda t: c.get_node(f"ns=2;s=Spb.Host.{t}")  # noqa: E731
            await status(n("Speed"), ua.StatusCodes.BadWaitingForInitialData)
            assert state_message(broker.port)["online"] is True               # retained host STATE

            # DATA for an unknown node: the host asks for a rebirth and gets the BIRTHs.
            end = time.time() + 10
            while edge.rebirths == 0:                                          # an edge node keeps sending DATA
                assert time.time() < end, "no rebirth request"
                edge.send("NDATA", {"Motor/Speed": [1, T.Int32, -5]}, by_alias=True)
                await asyncio.sleep(0.3)
            dv = await value(n("Speed"), -5)
            assert dv.Value.VariantType == ua.VariantType.Int32                # Default type = BIRTH type
            assert edge.rebirths == 1
            await value(n("Temp"), 21.5)
            await value(n("Run"), True)
            await value(n("Name"), "L1")
            await value(n("SpeedF"), -5.0)
            dv = await value(n("Count"), -1)                                    # Int8 as Tahu encodes it (255)
            assert dv.Value.VariantType == ua.VariantType.SByte
            await value(n("Pressure"), 1.5)
            await status(n("Missing"), ua.StatusCodes.BadOutOfRange)           # not in the BIRTH

            # DATA by alias.
            edge.send("NDATA", {"Motor/Speed": [1, T.Int32, -6]}, by_alias=True)
            edge.send("DDATA", {"Pressure": [10, T.Double, 2.25]}, device=D, by_alias=True)
            await value(n("Speed"), -6)
            await value(n("Pressure"), 2.25)

            # Writes become NCMD / DCMD with the BIRTH data type.
            await n("Speed").write_value(ua.DataValue(ua.Variant(42, ua.VariantType.Int32)))
            topic, m = edge.commands.get(timeout=5)
            assert topic == f"spBv1.0/{G}/NCMD/{E}"
            assert (m.name, m.datatype, m.int_value) == ("Motor/Speed", T.Int32, 42)
            await n("Pressure").write_value(ua.DataValue(ua.Variant(7.5, ua.VariantType.Double)))
            topic, m = edge.commands.get(timeout=5)
            assert topic == f"spBv1.0/{G}/DCMD/{E}/{D}"
            assert (m.name, m.datatype, m.double_value) == ("Pressure", T.Double, 7.5)
            await n("Count").write_value(ua.DataValue(ua.Variant(-3, ua.VariantType.SByte)))
            topic, m = edge.commands.get(timeout=5)
            assert (m.name, m.datatype, m.int_value) == ("Count", T.Int8, 253)  # width-specific two's complement

            # DEATH: last values stay, marked as no communication.
            edge.send("DDEATH", {}, device=D)
            await status(n("Pressure"), ua.StatusCodes.UncertainNoCommunicationLastUsableValue)
            await value(n("Speed"), -6)                                         # the node is still alive
            edge.send("NDEATH", {})
            dv = await status(n("Speed"), ua.StatusCodes.UncertainNoCommunicationLastUsableValue)
            assert dv.Value.Value == -6
            with pytest.raises(ua.UaStatusCodeError) as err:                    # the broker link stays up
                await n("Speed").write_value(ua.DataValue(ua.Variant(1, ua.VariantType.Int32)))
            assert err.value.code == ua.StatusCodes.BadOutOfService

            # A missed sequence number triggers another rebirth (at most one per 5 s per node).
            edge.birth()
            await value(n("Speed"), -5)
            await asyncio.sleep(5.2)
            edge.send("NDATA", {"Temp": [2, T.Float, 30.0]}, seq=(edge.seq + 3) % 256, by_alias=True)
            end = time.time() + 10
            while edge.rebirths < 2:
                assert time.time() < end, "no rebirth request after a sequence gap"
                await asyncio.sleep(0.1)
    finally:
        edge.close()
        broker.stop()
