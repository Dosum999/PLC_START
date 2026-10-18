"""Modbus serial slaves (pymodbus): python modbus_rtu_sim.py <tty> <baud> <parity> [RTU|ASCII].

Station 1: holding registers 40001.. = 1111, 2222; station 2: 40001 = 3333.
"""
import asyncio
import sys

from pymodbus import FramerType
from pymodbus.datastore import ModbusDeviceContext, ModbusSequentialDataBlock, ModbusServerContext
from pymodbus.server import StartAsyncSerialServer


def station(values):
    return ModbusDeviceContext(
        co=ModbusSequentialDataBlock(1, [False] * 100),
        di=ModbusSequentialDataBlock(1, [False] * 100),
        hr=ModbusSequentialDataBlock(1, values + [0] * (100 - len(values))),
        ir=ModbusSequentialDataBlock(1, [0] * 100),
    )


STATIONS = (1, 2)


def silent_for_unknown(sending: bool, data: bytes) -> bytes:
    """Unknown stations stay silent, as on a real RS-485 bus (pymodbus would
    otherwise answer with a device-failure exception)."""
    if not sending or not data:
        return data
    station = int(data[1:3], 16) if data[:1] == b":" else data[0]  # ASCII frames start with ':'
    return b"" if station not in STATIONS else data


async def main(tty, baud, parity, framer):
    ctx = ModbusServerContext(devices={1: station([1111, 2222]), 2: station([3333])}, single=False)
    await StartAsyncSerialServer(context=ctx, framer=FramerType[framer], port=tty, baudrate=baud, parity=parity,
                                 bytesize=8, stopbits=1, trace_packet=silent_for_unknown)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else "RTU"))
