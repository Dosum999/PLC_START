#pragma once

// Modbus slave side: a data table per unit and a request PDU processor. Used by
// the Modbus Server Serial driver both for requests from a master on the line
// and for the server's own tag reads and writes. No I/O and no locking here.

#include <cstdint>
#include <span>
#include <vector>

namespace logict::modbus {

struct DataTable {
  std::vector<std::uint8_t> coils = std::vector<std::uint8_t>(65536);     // 0/1
  std::vector<std::uint8_t> discrete = std::vector<std::uint8_t>(65536);  // 0/1
  std::vector<std::uint16_t> input = std::vector<std::uint16_t>(65536);
  std::vector<std::uint16_t> holding = std::vector<std::uint16_t>(65536);
};

// Handles a request PDU (function code first) and returns the response PDU,
// or an exception response (function | 0x80, code). Functions 1-6, 15, 16,
// 22 and 23. A master can only read discrete inputs and input registers; the
// server's own tags write them by passing `redirect`, which sends the coil
// functions (1, 5, 15) to the discrete inputs or the register functions
// (3, 6, 16, 22, 23) to the input registers.
enum class Redirect : std::uint8_t { None, DiscreteInputs, InputRegisters };
std::vector<std::uint8_t> process_request(DataTable& t, std::span<const std::uint8_t> pdu,
                                          Redirect redirect = Redirect::None);

// Exception codes.
inline constexpr std::uint8_t kIllegalFunction = 1, kIllegalDataAddress = 2, kIllegalDataValue = 3;

// RTU request framing: `request_head_size(function)` is how many leading
// bytes (address included) give the frame length, 0 for unsupported
// functions; `rtu_request_length(head)` is then the whole frame length
// (address .. CRC).
std::size_t request_head_size(std::uint8_t function);
std::size_t rtu_request_length(std::span<const std::uint8_t> head);

}  // namespace logict::modbus
