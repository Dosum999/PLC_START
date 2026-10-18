#pragma once

// Mitsubishi MC protocol / SLMP, 3E frame, binary code (Q, L, iQ-R, iQ-F).
// Batch read/write in word and bit units (commands 0401/1401). No I/O here.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::mc {

struct Device {
  const char* name;
  std::uint8_t code;  // binary device code
  bool bit;           // bit device
  int base;           // number base of the device number in GX Works notation
};

// Device table (Q/L/iQ-R binary codes, MC protocol reference manual).
const Device* find_device(std::string_view name);

struct McAddress final : sdk::Address {
  const Device* device = nullptr;
  std::uint32_t number = 0;  // first device number
  // Word devices (or bit devices read as words): words occupied.
  // Bit devices read as bits: points occupied.
  std::uint32_t points = 0;
  bool bit_units = false;  // read/write in bit units (Boolean on a bit device)
  int bit = -1;            // bit of a word (D100.A), -1 otherwise
  std::uint32_t string_len = 0;
};

struct AddressOptions {
  bool xy_octal = false;  // iQ-F (FX5): X/Y are numbered in octal
};

// GX Works style: D100, D100.A (bit, hex), M100, X1F, TN5 (timer value), TS5
// (timer contact), R10, ZR100, D100.20 (String), D100 [10] (array).
sdk::Result<std::shared_ptr<McAddress>> parse_address(std::string_view text, sdk::TagType requested,
                                                      const AddressOptions& opt);

sdk::Result<sdk::Value> decode(const McAddress& a, std::span<const std::uint8_t> words_le);
sdk::Result<std::vector<std::uint8_t>> encode(const McAddress& a, const sdk::Value& v);

// ---- frames -------------------------------------------------------------------

struct Route {
  std::uint8_t network = 0x00;
  std::uint8_t pc = 0xFF;
  std::uint16_t io = 0x03FF;
  std::uint8_t station = 0x00;
};

inline constexpr std::uint16_t kBatchRead = 0x0401, kBatchWrite = 0x1401;
inline constexpr std::uint16_t kSubWord = 0x0000, kSubBit = 0x0001;
inline constexpr std::uint32_t kMaxWords = 960, kMaxBits = 3584;

// Full 3E request frame. `timer` is the monitoring timer in 250 ms units.
std::vector<std::uint8_t> request(const Route& r, std::uint16_t timer, std::uint16_t command, std::uint16_t sub,
                                  std::span<const std::uint8_t> data);
std::vector<std::uint8_t> batch_read(const Route& r, std::uint16_t timer, const Device& d, std::uint32_t number,
                                     std::uint16_t points, bool bit_units);
std::vector<std::uint8_t> batch_write(const Route& r, std::uint16_t timer, const Device& d, std::uint32_t number,
                                      std::uint16_t points, bool bit_units, std::span<const std::uint8_t> payload);

// Packs/unpacks bit-unit data (two points per byte, first point in the high nibble).
std::vector<std::uint8_t> pack_bits(std::span<const std::uint8_t> points);  // one 0/1 byte per point
bool unpack_bit(std::span<const std::uint8_t> data, std::uint32_t index);

inline constexpr std::size_t kResponseHeader = 9;  // subheader .. data length
struct Response {
  std::uint16_t end_code = 0;
  std::vector<std::uint8_t> data;
};
// Parses the 9-byte response header; returns the number of bytes that follow.
sdk::Result<std::uint16_t> response_length(std::span<const std::uint8_t> header);
sdk::Result<Response> parse_response(std::span<const std::uint8_t> rest);

sdk::StatusCode end_code_status(std::uint16_t code);

// ---- 3E frame, ASCII code ------------------------------------------------------
// Same fields as binary, written as upper-case hex text, multi-byte fields most
// significant first; device code is the name padded with '*' ("D*", "SM") and
// the device number 6 digits in the device's numbering base (decimal, hex, or
// octal for iQ-F X/Y). Data: 4 hex digits per word, one '0'/'1' per bit point.
// The builders and parser take/return data in the binary layout (words little
// endian, bits two per byte) so callers handle both codes alike.

inline constexpr std::size_t kAsciiResponseHeader = 18;  // "D000" .. data length
std::vector<std::uint8_t> batch_read_ascii(const Route& r, std::uint16_t timer, const Device& d, int base,
                                           std::uint32_t number, std::uint16_t points, bool bit_units);
std::vector<std::uint8_t> batch_write_ascii(const Route& r, std::uint16_t timer, const Device& d, int base,
                                            std::uint32_t number, std::uint16_t points, bool bit_units,
                                            std::span<const std::uint8_t> payload);
// Returns the number of characters that follow the header.
sdk::Result<std::uint16_t> response_length_ascii(std::span<const std::uint8_t> header);
// `rest`: end code + data characters. `points` is used for bit units.
sdk::Result<Response> parse_response_ascii(std::span<const std::uint8_t> rest, bool bit_units, std::uint16_t points);

// ---- 1E frame (A-compatible, binary): FX3U-ENET(-ADP), A series ---------------
// Request: command (00 bit read, 01 word read, 02 bit write, 03 word write),
// PC number, monitoring timer, head device (4 bytes LE), device code (2 bytes
// LE, e.g. 0x4420 = "D "), points (1 byte, 0 = 256), 0x00, data. Response:
// command | 0x80, end code (0x5B is followed by an abnormal code), data.
// Data use the 3E binary layout (words LE, bits two per byte).

inline constexpr std::uint32_t kMaxWords1E = 64, kMaxBits1E = 256;
// 1E device code, 0 if the device has none.
std::uint16_t device_code_1e(const Device& d);
std::vector<std::uint8_t> batch_read_1e(std::uint8_t pc, std::uint16_t timer, std::uint16_t code, std::uint32_t number,
                                        std::uint16_t points, bool bit_units);
std::vector<std::uint8_t> batch_write_1e(std::uint8_t pc, std::uint16_t timer, std::uint16_t code, std::uint32_t number,
                                         std::uint16_t points, bool bit_units, std::span<const std::uint8_t> payload);
// End code (and abnormal code) -> status.
sdk::StatusCode end_code_status_1e(std::uint8_t end, std::uint8_t abnormal);

}  // namespace logict::mc
