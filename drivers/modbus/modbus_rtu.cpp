// Modbus RTU Serial driver (binary frames with CRC-16, 3.5-character gaps).
// See modbus_serial.h for the shared serial line handling.

#include "modbus_serial.h"

namespace {

class ModbusRtuDriver final : public logict::modbus::ModbusSerialDriver {
 public:
  ModbusRtuDriver() : ModbusSerialDriver(logict::modbus::Framing::Rtu, "modbus_rtu", "Modbus RTU Serial") {}
};

}  // namespace

LOGICT_DECLARE_DRIVER(ModbusRtuDriver)
