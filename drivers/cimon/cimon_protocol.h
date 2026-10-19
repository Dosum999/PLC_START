#pragma once

// KDT CIMON PLC (CP / XP / PLC-S) communication, after the CIMON PLC manual
// (cimon.atlassian.net, space PLC): "Details of Command" (serial computer
// link, ASCII) and "CIMON PLC HMI Protocol Service" (Ethernet exclusive
// service "KDT_PLC_M", TCP 10260 / UDP 10262). No I/O here.
//
// Devices: bit devices X, Y, M, L, K, F (word + hex bit: M0104 = word 10,
// bit 4), timer/counter contacts T, C, word devices D, S, Z and timer/counter
// values TC, TS, CC, CS. Both protocols address a device with 8 characters:
// prefix, second prefix letter or '0', then 6 characters - a 5-digit word and
// a hex bit for bit access, 6 digits otherwise. Word access to a bit device
// uses the bit form with bit 0 (the manual: "the last number should be 0").
// Multi-word values store the low word first (ByteOrder::CDAB).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::cimon {

enum class Kind : std::uint8_t { Bit, Contact, Word };

struct Device {
  const char* name;
  Kind kind;
  std::uint32_t count;  // words (Bit/Word) or points (Contact)
};

const Device* find_device(std::string_view name);
std::uint32_t device_index(const Device& d);
const Device& device_at(std::uint32_t index);

struct CimonAddress final : sdk::Address {
  const Device* device = nullptr;
  bool bit_units = false;    // bit commands (r / w, 72h / 77h)
  std::uint32_t start = 0;   // bits: word*16+bit (contacts: number); words: word
  std::uint32_t points = 0;  // bits or words
  int bit = -1;              // bit of a word device (D100.5), -1 otherwise
  std::uint32_t string_len = 0;
};

// CIMON notation: M0104 (bit; with a word type: word 104), D100, D100.5,
// TC10, T5, D100.20 with type String, D100 [10].
sdk::Result<std::shared_ptr<CimonAddress>> parse_address(std::string_view text, sdk::TagType requested);

// 8-character wire address for a start in bit or word units.
std::string wire_address(const Device& d, bool bit_units, std::uint32_t start);

// Raw data: words (2 bytes, big endian each) or one 0/1 byte per bit.
sdk::Result<sdk::Value> decode(const CimonAddress& a, std::span<const std::uint8_t> raw);
sdk::Result<std::vector<std::uint8_t>> encode(const CimonAddress& a, const sdk::Value& v);

// ---- serial computer link (ASCII) -----------------------------------------------
// ENQ, station (2 hex), command, length (2 hex, data characters), data,
// BCC (2 hex: low byte of the sum from the command through the data), EOT.
// Answers: STX ... ETX with the same command, or 'E' + 2-digit error code.

inline constexpr std::uint32_t kSerialReadWords = 63, kSerialWriteWords = 61;
inline constexpr std::uint32_t kSerialReadBits = 255, kSerialWriteBits = 245;

std::vector<std::uint8_t> serial_read(std::uint8_t station, const Device& d, bool bit_units, std::uint32_t start,
                                      std::uint32_t count);
std::vector<std::uint8_t> serial_write(std::uint8_t station, const Device& d, bool bit_units, std::uint32_t start,
                                       std::span<const std::uint8_t> raw);

struct Answer {
  int error = 0;  // CIMON error code, 0 = none
  std::vector<std::uint8_t> data;  // raw layout (see decode)
};
// `frame`: STX through ETX.
sdk::Result<Answer> parse_serial(std::span<const std::uint8_t> frame, std::uint8_t station, char command,
                                 bool bit_units, std::uint32_t count);
sdk::StatusCode serial_error_status(int code);

// ---- Ethernet exclusive service (binary) ------------------------------------------
// "KDT_PLC_M", frame number (0-127), command, 00h, length (2), data, checksum
// (2: low 16 bits of the byte sum of everything before it). Answers start with
// "KDT_PLC_S" and frame number + 128. Two-byte fields are big endian, as the
// manual's size and error code examples show (00h 80h = 128).

inline constexpr std::size_t kEnetHeader = 14;  // ID .. length
inline constexpr std::uint32_t kEnetWords = 512, kEnetBits = 1024, kEnetWriteBits = 256;

std::vector<std::uint8_t> enet_read(std::uint8_t frame_no, const Device& d, bool bit_units, std::uint32_t start,
                                    std::uint32_t count);
std::vector<std::uint8_t> enet_write(std::uint8_t frame_no, const Device& d, bool bit_units, std::uint32_t start,
                                     std::span<const std::uint8_t> raw);
// Data length from a response header; error if the header is not a slave frame.
sdk::Result<std::uint16_t> enet_length(std::span<const std::uint8_t> header);
// `frame`: header + data + checksum.
sdk::Result<Answer> parse_enet(std::span<const std::uint8_t> frame, std::uint8_t frame_no, bool bit_units,
                               std::uint32_t count, bool is_write);
sdk::StatusCode enet_error_status(int code);

}  // namespace logict::cimon
