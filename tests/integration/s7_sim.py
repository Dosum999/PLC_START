"""S7 PLC simulator for integration tests: python s7_sim.py <port>.

Uses python-snap7's server, an implementation independent of logict, so the
driver is checked against someone else's reading of S7comm.
"""
import ctypes
import sys
import time

from snap7.server import Server, SrvArea

server = Server(log=False)
keep = []
for area, index, size in [(SrvArea.DB, 1, 1024), (SrvArea.DB, 2, 256), (SrvArea.MK, 0, 256), (SrvArea.PE, 0, 64),
                          (SrvArea.PA, 0, 64)]:
    buf = (ctypes.c_uint8 * size)()
    server.register_area(area, index, buf)
    keep.append(buf)
server.start(tcp_port=int(sys.argv[1]))
while True:
    time.sleep(1)
