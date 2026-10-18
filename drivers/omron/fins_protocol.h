#pragma once

// Omron FINS (CS/CJ/CP/NJ/NX in CS/CJ mode). Memory area read/write (0101 /
// 0102) in word and bit units, FINS/UDP and FINS/TCP framing. No I/O here.
// Reference: FINS Commands Reference Manual (W227), CJ-series Ethernet Unit
// Operation Manual (W421) section 6/7 for FINS/TCP.
//
// Words are big endian; values spanning several words store the lowest word
// first by default (Omron "first word low"), i.e. ByteOrder::CDAB.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "logict/sdk/codec.h"
#include "logict/sdk/driver.h"

namespace logict::fins {

struct FinsAddress final : sdk::Address {
  std::uint8_t code = 0;      // memory area code (bit or word variant)
  bool bit_units = false;     // bit area code: one byte (0/1) per point
  std::uint32_t index = 0;    // linear start: word, or word*16+bit (timer/counter flags: the number)
  std::uint32_t points = 0;   // words, or bits for bit units
  std::uint32_t string_len = 0;
};

// Kepware style: CIO100, CIO100.05, A10, D100, D100.15, H5, W3, E0:100 (EM
// bank hex 0-18), T5 / C5 (timer/counter PV; Boolean = completion flag),
// D100.20 with type String (length), D100 [10] (array).
sdk::Result<std::shared_ptr<FinsAddress>> parse_address(std::string_view text, sdk::TagType requested);

// Area of an area code with bit units (planning space: linear index -> FINS
// word/bit). Timer/counter flags (0x09) are numbered without a bit part.
inline bool linear_bits(std::uint8_t code) { return code != 0x09; }
inline std::uint16_t word_of(std::uint8_t code, std::uint32_t index, bool bit_units) {
  return static_cast<std::uint16_t>(bit_units && linear_bits(code) ? index / 16 : index);
}
inline std::uint8_t bit_of(std::uint8_t code, std::uint32_t index, bool bit_units) {
  return static_cast<std::uint8_t>(bit_units && linear_bits(code) ? index % 16 : 0);
}

// `data`: words (2 bytes each) or one 0/1 byte per bit.
sdk::Result<sdk::Value> decode(const FinsAddress& a, std::span<const std::uint8_t> data, sdk::codec::ByteOrder order);
sdk::Result<std::vector<std::uint8_t>> encode(const FinsAddress& a, const sdk::Value& v, sdk::codec::ByteOrder order);

// ---- frames -------------------------------------------------------------------

struct Header {
  std::uint8_t dna = 0, da1 = 0, da2 = 0;  // destination network / node / unit
  std::uint8_t sna = 0, sa1 = 0, sa2 = 0;  // source network / node / unit
};

inline constexpr std::uint8_t kMrcMemory = 0x01, kSrcRead = 0x01, kSrcWrite = 0x02;
inline constexpr std::uint32_t kMaxWords = 990, kMaxBits = 990;
inline constexpr std::size_t kResponseHeader = 14;  // 10 header + command + end code

std::vector<std::uint8_t> memory_read(const Header& h, std::uint8_t sid, std::uint8_t code, std::uint16_t word,
                                      std::uint8_t bit, std::uint16_t count);
std::vector<std::uint8_t> memory_write(const Header& h, std::uint8_t sid, std::uint8_t code, std::uint16_t word,
                                       std::uint8_t bit, std::uint16_t count, std::span<const std::uint8_t> payload);

struct Response {
  std::uint8_t sid = 0;
  std::uint16_t end_code = 0;  // relay / CPU error flag bits masked off
  std::vector<std::uint8_t> data;
};
sdk::Result<Response> parse_response(std::span<const std::uint8_t> frame, std::uint8_t mrc, std::uint8_t src);

sdk::StatusCode end_code_status(std::uint16_t code);

// ---- FINS/TCP -------------------------------------------------------------------

inline constexpr std::size_t kTcpHeader = 16;  // "FINS", length, command, error code
inline constexpr std::uint32_t kTcpNodeRequest = 0, kTcpNodeResponse = 1, kTcpFrame = 2;

std::vector<std::uint8_t> tcp_node_request(std::uint8_t client_node);  // 0 = assigned by the server
std::vector<std::uint8_t> tcp_frame(std::span<const std::uint8_t> fins);

struct TcpHeader {
  std::uint32_t length = 0;  // bytes after the length field
  std::uint32_t command = 0;
  std::uint32_t error = 0;
};
sdk::Result<TcpHeader> parse_tcp_header(std::span<const std::uint8_t> h);

}  // namespace logict::fins
