"""ODBC Client driver against a real PostgreSQL server through unixODBC and the
psqlODBC driver. The database side is set up and checked with psycopg, which
talks to PostgreSQL directly (no ODBC)."""

import asyncio
import datetime as dt
import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from asyncua import Client, ua

from conftest import free_port

psycopg = pytest.importorskip("psycopg")
PG_CTL = shutil.which("pg_ctl")
INITDB = shutil.which("initdb")
PSQLODBC = os.environ.get("LOGICT_PSQLODBC") or (str(Path(PG_CTL).parent.parent / "lib" / "psqlodbcw.so") if PG_CTL else "")
pytestmark = [pytest.mark.asyncio,
              pytest.mark.skipif(not (PG_CTL and INITDB and Path(PSQLODBC).exists()), reason="PostgreSQL or psqlODBC not installed")]

SCHEMA = """
CREATE TABLE tank (level double precision, temp real, running boolean, name text, count integer, big bigint,
                   price numeric(10,2), updated timestamp, note text);
INSERT INTO tank VALUES (1.5, 21.25, true, '탱크1', -7, 9000000000, 12.50, '2024-02-29 12:34:56.789', NULL);
CREATE SCHEMA plant;
CREATE TABLE plant.tags (name text PRIMARY KEY, value double precision, id integer);
INSERT INTO plant.tags VALUES ('L1', 10, 1), ('L2', 20, 2), ('Line.1 [A]', 30, 3);
CREATE TABLE multi (v integer);
INSERT INTO multi VALUES (1), (2);
CREATE TABLE log (v integer);
CREATE PROCEDURE set_speed(s integer) LANGUAGE sql AS $$ INSERT INTO log VALUES (s) $$;
"""


class Postgres:
    def __init__(self, tmp):
        self.dir = tmp / "pgdata"
        self.port = free_port()
        subprocess.run([INITDB, "-D", str(self.dir), "-U", "postgres", "-A", "trust", "-E", "UTF8", "--no-locale"],
                       check=True, capture_output=True)

    def start(self):
        subprocess.run([PG_CTL, "-D", str(self.dir), "-w", "-l", str(self.dir / "log"), "-o",
                        f"-p {self.port} -c unix_socket_directories='' -c listen_addresses=127.0.0.1", "start"], check=True, capture_output=True)

    def stop(self):
        subprocess.run([PG_CTL, "-D", str(self.dir), "-w", "-m", "fast", "stop"], capture_output=True)

    def sql(self, query, *args):
        with psycopg.connect(host="127.0.0.1", port=self.port, user="postgres", dbname="postgres", autocommit=True) as c:
            cur = c.execute(query, args or None)
            return cur.fetchall() if cur.description else None

    def connection_string(self):
        return f"Driver={PSQLODBC};Server=127.0.0.1;Port={self.port};Database=postgres;BoolsAsChar=0"


@pytest.fixture
def pg(tmp_path):
    p = Postgres(tmp_path)
    p.start()
    p.sql(SCHEMA)
    yield p
    p.stop()


def tag(name, address, data_type=None):
    t = {"common.ALLTYPES_NAME": name, "servermain.TAG_ADDRESS": address}
    if data_type is not None:
        t["servermain.TAG_DATA_TYPE"] = data_type
    return t


TAGS = [
    tag("Level", "tank.level"), tag("Temp", "tank.temp"), tag("Running", "tank.running"), tag("Name", "tank.name"),
    tag("Count", "tank.count"), tag("Big", "tank.big"), tag("Price", "tank.price"), tag("Updated", "tank.updated"),
    tag("Note", "tank.note"), tag("NoColumn", "tank.nosuch"), tag("NoTable", "nosuch.v"),
    tag("LevelW", "tank.level", 4),                         # same column as Short
    tag("L1", "plant.tags[name='L1'].value"), tag("L2", "plant.tags[name='L2'].value"),
    tag("Dotted", "plant.tags[name='Line.1 [A]'].value"), tag("ById", "plant.tags[id=2].name"),
    tag("NoRow", "plant.tags[name='L9'].value"), tag("Multi", "multi.v"),
    tag("Rows", "sql:SELECT count(*) FROM plant.tags"),
    tag("Total", "sql.total:SELECT 'x' AS label, sum(value) AS total FROM plant.tags"),
    tag("BadQuery", "sql:SELECT FROM WHERE"), tag("Speed", "exec:CALL set_speed(?)", 6),
]


def project(tmp_path, pg):
    p = {"project": {"channels": [{
        "common.ALLTYPES_NAME": "Db", "servermain.MULTIPLE_TYPES_DEVICE_DRIVER": "ODBC Client",
        "devices": [{"common.ALLTYPES_NAME": "Pg", "servermain.DEVICE_ID_STRING": pg.connection_string(),
                     "odbc_client.DEVICE_USERNAME": "postgres", "tags": TAGS}],
    }]}}
    path = tmp_path / "odbc.json"
    path.write_text(json.dumps(p))
    return path


async def read(node, want=None, code=0, timeout=10.0):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        dv = await node.read_data_value(raise_on_bad_status=False)
        if dv.StatusCode.value == code and (want is None or dv.Value.Value == want):
            return dv
        if asyncio.get_running_loop().time() > end:
            raise AssertionError(f"{node}: {dv.Value.Value if dv.Value else None} {dv.StatusCode}, want {want} {code:#x}")
        await asyncio.sleep(0.1)


async def write_status(node, variant):
    try:
        await node.write_value(ua.DataValue(variant))
        return 0
    except ua.UaStatusCodeError as e:
        return e.code


async def test_odbc_client(daemon_factory, tmp_path, pg):
    d = daemon_factory({"security_none": True, "allow_anonymous": True}, project=project(tmp_path, pg))
    async with Client(d.url) as c:
        n = lambda t: c.get_node(f"ns=2;s=Db.Pg.{t}")  # noqa: E731
        S = ua.StatusCodes

        # Default types follow the column types.
        expected = [("Level", 1.5, ua.VariantType.Double), ("Temp", 21.25, ua.VariantType.Float),
                    ("Running", True, ua.VariantType.Boolean), ("Name", "탱크1", ua.VariantType.String),
                    ("Count", -7, ua.VariantType.Int32), ("Big", 9000000000, ua.VariantType.Int64),
                    ("Price", 12.5, ua.VariantType.Double), ("LevelW", 1, ua.VariantType.Int16)]
        for name, value, vtype in expected:
            dv = await read(n(name), value)
            assert dv.Value.VariantType == vtype, name
        dv = await read(n("Updated"))
        assert dv.Value.Value == dt.datetime(2024, 2, 29, 12, 34, 56, 789000, tzinfo=dt.timezone.utc)  # stored as UTC
        await read(n("Note"), code=S.BadNoData)                      # NULL
        await read(n("NoColumn"), code=S.BadConfigurationError)       # the rest of the row still reads
        await read(n("NoTable"), code=S.BadConfigurationError)
        await read(n("NoRow"), code=S.BadNoData)

        # Keyed rows.
        await read(n("L1"), 10.0)
        await read(n("L2"), 20.0)
        await read(n("Dotted"), 30.0)
        await read(n("ById"), "L2")

        # Queries.
        dv = await read(n("Rows"), 3)
        assert dv.Value.VariantType == ua.VariantType.Int64
        await read(n("Total"), 60.0)
        await read(n("BadQuery"), code=S.BadConfigurationError)
        await read(n("Speed"), code=S.BadNotReadable)

        # Changes made by others show up.
        pg.sql("UPDATE plant.tags SET value = 11.5 WHERE name = 'L1'")
        await read(n("L1"), 11.5)

        # Writes.
        assert await write_status(n("Level"), ua.Variant(3.25, ua.VariantType.Double)) == 0
        assert pg.sql("SELECT level FROM tank") == [(3.25,)]
        assert await write_status(n("Name"), ua.Variant("탱크2", ua.VariantType.String)) == 0
        assert pg.sql("SELECT name FROM tank") == [("탱크2",)]
        assert await write_status(n("Running"), ua.Variant(False, ua.VariantType.Boolean)) == 0
        assert pg.sql("SELECT running FROM tank") == [(False,)]
        assert await write_status(n("Count"), ua.Variant(123456, ua.VariantType.Int32)) == 0
        assert await write_status(n("Updated"), ua.Variant(dt.datetime(2025, 1, 2, 3, 4, 5, 678000, tzinfo=dt.timezone.utc), ua.VariantType.DateTime)) == 0
        assert pg.sql("SELECT count, updated FROM tank") == [(123456, dt.datetime(2025, 1, 2, 3, 4, 5, 678000))]
        assert await write_status(n("L2"), ua.Variant(22.5, ua.VariantType.Double)) == 0
        assert pg.sql("SELECT name, value FROM plant.tags ORDER BY id") == [("L1", 11.5), ("L2", 22.5), ("Line.1 [A]", 30.0)]
        await read(n("L2"), 22.5)
        assert await write_status(n("Count"), ua.Variant("abc", ua.VariantType.String)) == S.BadTypeMismatch

        # Only single-row updates are committed.
        assert await write_status(n("NoRow"), ua.Variant(1.0, ua.VariantType.Double)) == S.BadNoData
        assert await write_status(n("Multi"), ua.Variant(5, ua.VariantType.Int32)) == S.BadNotWritable
        assert pg.sql("SELECT v FROM multi ORDER BY v") == [(1,), (2,)]
        assert await write_status(n("Rows"), ua.Variant(1, ua.VariantType.Int64)) == S.BadNotWritable

        # Statements run with the written value (stored procedure).
        assert await write_status(n("Speed"), ua.Variant(42, ua.VariantType.Int32)) == 0
        assert await write_status(n("Speed"), ua.Variant(43, ua.VariantType.Int32)) == 0
        assert pg.sql("SELECT v FROM log ORDER BY v") == [(42,), (43,)]

        # Database restart: tags go bad, then recover on reconnect.
        pg.stop()
        dv = await n("L1").read_data_value(raise_on_bad_status=False)
        for _ in range(100):
            dv = await n("L1").read_data_value(raise_on_bad_status=False)
            if not dv.StatusCode.is_good():
                break
            await asyncio.sleep(0.1)
        assert not dv.StatusCode.is_good()
        pg.start()
        pg.sql("UPDATE plant.tags SET value = 99 WHERE name = 'L1'")
        await read(n("L1"), 99.0, timeout=20)
        assert await write_status(n("Level"), ua.Variant(4.5, ua.VariantType.Double)) == 0
        assert pg.sql("SELECT level FROM tank") == [(4.5,)]
