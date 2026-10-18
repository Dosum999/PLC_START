// Modbus ASCII Serial driver (':' + hex + LRC + CR LF frames).
// See modbus_serial.h for the shared serial line handling.

#include "modbus_serial.h"

namespace {

class ModbusAsciiDriver final : public logict::modbus::ModbusSerialDriver {
 public:
  ModbusAsciiDriver() : ModbusSerialDriver(logict::modbus::Framing::Ascii, "modbus_ascii", "Modbus ASCII Serial") {}
};

}  // namespace

LOGICT_DECLARE_DRIVER(ModbusAsciiDriver)
