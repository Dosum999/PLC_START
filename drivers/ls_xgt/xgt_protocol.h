#pragma once

// LS ELECTRIC XGT dedicated protocol (FEnet, TCP 2004): address parsing and
// frame encoding. Reference: XGT FEnet I/F Module User's Manual V2.30, ch. 8.
// No I/O here.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::xgt {

enum class Series : std::uint8_t {
  XGK,  // XGK, XGB (XBM/XBC): XG5000 device notation (D100, M0010A, ...)
  XGI,  // XGI, XGR, XGB-XEC: IEC direct variables (%MW100, %IX0.0.3, ...)
};

// Header "CPU Info" byte sent in requests. TODO(verify): the manual lists
// XGK 0xA0 / XGI 0xA4 / XGR 0xA8 and calls the field don't-care for requests.
std::uint8_t cpu_info_byte(std::string_view model);
Series series_of(std::string_view model);

struct XgtAddress final : sdk::Address {
  std::string area;   // "D", "M", "ZR", ...
  bool block = true;  // readable with continuous (byte block) reads
  // Block-capable items: byte range in the area.
  std::uint64_t byte_offset = 0;
  std::uint32_t byte_len = 0;
  int bit = -1;  // bit inside the item's first byte (0-7), -1 if not a bit
  std::uint64_t bit_index = 0;  // linear bit number (for %X writes)
  bool bit_write_direct = false;  // area supports %?X bit writes
  // Bits and I/Q/U items: protocol variable name (else see unit_vars()).
  std::string var;  // e.g. "%IW0.1.0", "%TX10", "%MX2764"
  char size = 'W';  // X B W D L of `var`
  std::uint32_t string_len = 0;
};

sdk::Result<std::shared_ptr<XgtAddress>> parse_address(std::string_view text, sdk::TagType requested, Series series);

// Protocol variables used to read/write a non-block item individually, one
// per device unit (`size`), all of the same data type.
std::vector<std::string> unit_vars(const XgtAddress& a);

// Decodes a value from little-endian device bytes starting at the item.
sdk::Result<sdk::Value> decode(const XgtAddress& a, std::span<const std::uint8_t> bytes);
// Encodes a value into device bytes (not for bits).
sdk::Result<std::vector<std::uint8_t>> encode(const XgtAddress& a, const sdk::Value& v);

// ---- frames -------------------------------------------------------------------

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kReadRequest = 0x0054, kReadResponse = 0x0055, kWriteRequest = 0x0058,
                               kWriteResponse = 0x0059;
inline constexpr std::uint16_t kTypeBit = 0x00, kTypeByte = 0x01, kTypeWord = 0x02, kTypeDWord = 0x03,
                               kTypeLWord = 0x04, kTypeContinuous = 0x14;
inline constexpr std::size_t kMaxBlocks = 16;
inline constexpr std::size_t kMaxContinuousBytes = 1400;
inline constexpr std::size_t kMaxVarLength = 16;

std::uint16_t type_code(char size);

struct Header {
  std::uint8_t cpu_info = 0xA0;
  std::uint8_t fenet_position = 0;  // bits 0-3 slot, 4-7 base
  std::uint16_t invoke_id = 0;
};

std::vector<std::uint8_t> frame(const Header& h, std::span<const std::uint8_t> body);

std::vector<std::uint8_t> read_individual(std::uint16_t type, std::span<const std::string> vars);
std::vector<std::uint8_t> read_continuous(std::string_view var, std::uint16_t bytes);
std::vector<std::uint8_t> write_individual(std::uint16_t type, std::span<const std::string> vars,
                                           std::span<const std::vector<std::uint8_t>> data);
std::vector<std::uint8_t> write_continuous(std::string_view var, std::span<const std::uint8_t> data);

struct ParsedHeader {
  std::uint16_t invoke_id = 0;
  std::uint16_t length = 0;  // body length
  std::uint16_t plc_info = 0;
};
sdk::Result<ParsedHeader> parse_header(std::span<const std::uint8_t> h);

struct Response {
  std::uint16_t command = 0, type = 0;
  std::uint16_t error_status = 0;  // 0 = ACK
  std::uint8_t error_code = 0;     // NAK reason
  std::uint16_t blocks = 0;
  std::vector<std::vector<std::uint8_t>> data;  // read responses
  bool nak() const { return error_status != 0; }
};
sdk::Result<Response> parse_body(std::span<const std::uint8_t> body, bool has_data);

}  // namespace logict::xgt
