#pragma once

// Keyence KV host link (上位リンク) commands over Ethernet: RDS / WRS / RD in
// ASCII, one command per line ("\r"), answers end with "\r\n". KV-5000/7000/
// 8000/NANO with KEYENCE device notation, plus the XYM aliases. No I/O here.
//
// Values spanning two words store the low word first (ByteOrder::CDAB);
// strings keep the first character in the high byte of each word.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::kv {

enum class Notation : std::uint8_t {
  Decimal,  // DM100, M100
  Hex,      // B1F, W1A
  Bank,     // R1015: channel 10, bit 15 (bits 00-15)
  Xy,       // X1F: bank 1 (decimal), bit F (hex)
};

enum class Kind : std::uint8_t { Bit, Word, Dword /* TC, TS, CC, CS */, Contact /* T, C */ };

struct Device {
  const char* name;
  Kind kind;
  Notation notation;
  std::uint32_t count;  // points (bits for bit devices) in the device
};

const Device* find_device(std::string_view name);
// Stable small number for a device (block planning areas).
std::uint32_t device_index(const Device& d);
const Device& device_at(std::uint32_t index);

// How a tag is transferred. Bits: one point per bit (no suffix). Words: 16-bit
// words as .H (bit devices: 16 points per word). Dwords: .D (one timer or
// counter value per point). Contact: RD T5.D, first field.
enum class Mode : std::uint8_t { Bits, Words, Dwords, Contact };

struct KvAddress final : sdk::Address {
  const Device* device = nullptr;
  Mode mode = Mode::Words;
  std::uint32_t start = 0;   // in mode units: bit index, word index, timer number
  std::uint32_t points = 0;  // in mode units
  int bit = -1;              // bit of a word (DM100.5), -1 otherwise
  std::uint32_t string_len = 0;
};

// DM100, DM100.5 (bit), R1015, MR0 [16], B1F, W10, TM5, EM0, FM0, ZF0, Z1,
// CM0, VM0, X1F, Y20, M100, L5, D100, E0, F0, TC5 / TS5 / CC5 / CS5 (current /
// preset, 32-bit), T5 / C5 (Boolean contact), DM100.20 with type String.
sdk::Result<std::shared_ptr<KvAddress>> parse_address(std::string_view text, sdk::TagType requested);

// Device text for a mode-unit index ("R1015", "B1F", "DM100").
std::string device_text(const Device& d, Mode mode, std::uint32_t start);

inline constexpr std::uint32_t kMaxPoints = 1000, kMaxTimers = 120;
std::uint32_t max_points(Mode mode);

// "RDS DM100.H 3\r", "RD T5.D\r"
std::string read_command(const Device& d, Mode mode, std::uint32_t start, std::uint32_t count);
// `values` are already formatted tokens.
std::string write_command(const Device& d, Mode mode, std::uint32_t start, std::span<const std::string> values);

// Parses an answer line (without "\r\n") for `count` points into mode-unit
// raw values: bits 0/1, words, dwords, contacts 0/1. "E0".."E6" yield the error.
struct Answer {
  std::string error;  // "E0".. or empty
  std::vector<std::uint32_t> values;
};
sdk::Result<Answer> parse_read_answer(std::string_view line, Mode mode, std::uint32_t count);
sdk::StatusCode error_status(std::string_view code);

// Raw words (big endian bytes per word) <-> values.
sdk::Result<sdk::Value> decode(const KvAddress& a, std::span<const std::uint32_t> raw);
// Returns the raw mode-unit values for a write.
sdk::Result<std::vector<std::uint32_t>> encode(const KvAddress& a, const sdk::Value& v);
std::string format_token(Mode mode, std::uint32_t raw);

}  // namespace logict::kv
