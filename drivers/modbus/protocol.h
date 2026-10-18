#pragma once

// Modbus protocol pieces shared by the TCP and RTU drivers: Kepware-style
// address parsing, data encoding and PDU helpers. No I/O here.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::modbus {

enum class Area : std::uint8_t { Coil = 0, DiscreteInput = 1, InputRegister = 3, HoldingRegister = 4 };

constexpr bool is_bit_area(Area a) { return a == Area::Coil || a == Area::DiscreteInput; }

struct AddressOptions {
  bool zero_based = true;       // Kepware "Zero-Based Addressing": 40001 -> register 0
  bool zero_based_bits = true;  // Kepware "Zero-Based Bit Addressing": bits .0-.15
};

// Kepware Modbus addressing:
//   000001-065536 coils, 100001-165536 discrete inputs, 300001-365536 input
//   registers, 400001-465536 holding registers; 5-digit forms (40001) too.
//   Suffixes: ".b" bit of a register (Boolean), ".nH"/".nL" string of n chars
//   with high/low byte first, " [r]" or " [r][c]" arrays.
struct ModbusAddress final : sdk::Address {
  Area area = Area::HoldingRegister;
  std::uint32_t offset = 0;  // 0-based protocol address
  int bit = -1;              // bit 0-15 within a register, -1 if none
  std::uint32_t string_len = 0;
  bool string_high_first = true;

  // Coils or registers occupied.
  std::uint32_t units() const;
};

sdk::Result<std::shared_ptr<ModbusAddress>> parse_address(std::string_view text, sdk::TagType requested,
                                                          const AddressOptions& opt);

// Kepware "Data Encoding" device properties.
struct Encoding {
  bool modbus_byte_order = true;  // high byte first within a register
  bool first_word_low = true;     // 32-bit values: low word first
  bool first_dword_low = true;    // 64-bit values: low dword first
};

// Decodes a value from register data as it appears on the wire (2 bytes per
// register, starting at the item's first register).
sdk::Result<sdk::Value> decode_registers(const ModbusAddress& a, std::span<const std::uint8_t> wire, const Encoding& enc);
// Encodes a value into wire register bytes (not valid for register bits).
sdk::Result<std::vector<std::uint8_t>> encode_registers(const ModbusAddress& a, const sdk::Value& v,
                                                        const Encoding& enc);
// Decodes coils/discrete inputs from packed bits starting at bit `first`.
sdk::Value decode_bits(const ModbusAddress& a, std::span<const std::uint8_t> packed, std::uint32_t first);

// Function codes.
inline constexpr std::uint8_t kReadCoils = 0x01, kReadDiscreteInputs = 0x02, kReadHoldingRegisters = 0x03,
                              kReadInputRegisters = 0x04, kWriteSingleCoil = 0x05, kWriteSingleRegister = 0x06,
                              kWriteMultipleCoils = 0x0F, kWriteMultipleRegisters = 0x10, kMaskWriteRegister = 0x16;

std::uint8_t read_function(Area a);
std::vector<std::uint8_t> make_read(Area a, std::uint16_t start, std::uint16_t count);

// Maps a Modbus exception code to a status.
sdk::StatusCode exception_status(std::uint8_t code);

// Modbus RTU CRC-16 (polynomial 0xA001, initial 0xFFFF), sent low byte first.
std::uint16_t crc16(std::span<const std::uint8_t> data);

// Length of an RTU response after the address and function code bytes, from
// the function code and (for reads) the byte count; 0 if unknown.
std::size_t rtu_remaining(std::uint8_t function, std::uint8_t first_byte_after_function);

inline void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}
inline std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

}  // namespace logict::modbus
