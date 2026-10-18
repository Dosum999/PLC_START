"""Stand-alone Modbus TCP simulator for integration tests: python modbus_sim.py <port>."""

import asyncio
import sys

from pymodbus.datastore import ModbusDeviceContext, ModbusSequentialDataBlock, ModbusServerContext
from pymodbus.server import StartAsyncTcpServer


async def main(port: int) -> None:
    device = ModbusDeviceContext(
        co=ModbusSequentialDataBlock(1, [False] * 200),
        di=ModbusSequentialDataBlock(1, [False] * 200),
        hr=ModbusSequentialDataBlock(1, [0] * 200),
        ir=ModbusSequentialDataBlock(1, [0] * 200),
    )
    await StartAsyncTcpServer(context=ModbusServerContext(devices=device, single=True), address=("127.0.0.1", port))


if __name__ == "__main__":
    asyncio.run(main(int(sys.argv[1])))
