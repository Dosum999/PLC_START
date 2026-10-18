#pragma once

// LS XGT Cnet dedicated protocol (serial, ASCII). Reference: XBL-C21A Cnet
// manual V2.0, chapter 7. Frames: ENQ station cmd type ... EOT [BCC];
// responses ACK/NAK station cmd type ... ETX [BCC]. Lowercase commands (r, w)
// carry a BCC: low byte of the sum of all bytes from ENQ/ACK/NAK to EOT/ETX,
// as two hex characters. Numbers are hex text; data values are written most
// significant digit first per device unit.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/status.h"

namespace logict::xgt::cnet {

inline constexpr std::uint8_t ENQ = 0x05, ACK = 0x06, NAK = 0x15, EOT = 0x04, ETX = 0x03;
inline constexpr std::size_t kMaxContinuousWords = 60;
inline constexpr std::size_t kMaxBlocks = 16;

std::vector<std::uint8_t> read_individual(std::uint8_t station, bool bcc, std::span<const std::string> vars);
std::vector<std::uint8_t> read_continuous(std::uint8_t station, bool bcc, std::string_view var, unsigned count);
// `data` items are already hex text (one per variable / the whole block).
std::vector<std::uint8_t> write_individual(std::uint8_t station, bool bcc, std::span<const std::string> vars,
                                           std::span<const std::string> data);
std::vector<std::uint8_t> write_continuous(std::uint8_t station, bool bcc, std::string_view var, unsigned count,
                                           std::string_view data);

struct Response {
  bool ack = false;
  std::uint16_t error = 0;  // NAK error code
  // Read responses: per block, bytes in text order (most significant first
  // within each device unit).
  std::vector<std::vector<std::uint8_t>> blocks;
};

// Parses a complete response frame (header through ETX and BCC).
sdk::Result<Response> parse(std::span<const std::uint8_t> frame, std::uint8_t station, bool bcc, bool is_read);

// Hex text of `bytes` (two characters per byte).
std::string to_hex(std::span<const std::uint8_t> bytes);
std::uint8_t bcc_of(std::span<const std::uint8_t> bytes);

}  // namespace logict::xgt::cnet
