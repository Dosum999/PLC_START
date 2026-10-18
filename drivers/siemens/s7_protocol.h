#pragma once

// Siemens S7comm (ISO-on-TCP, port 102) for S7-300/400/1200/1500 with PUT/GET
// access: TPKT/COTP framing, S7 job/ack-data PDUs and Kepware-style addresses.
// No I/O here. S7comm is not publicly specified; the layout follows the
// widely used open implementations (snap7, Wireshark s7comm dissector).

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::s7 {

enum Area : std::uint8_t { Inputs = 0x81, Outputs = 0x82, Flags = 0x83, DataBlock = 0x84 };

struct S7Address final : sdk::Address {
  Area area = DataBlock;
  std::uint16_t db = 0;
  std::uint32_t byte_offset = 0;
  std::uint32_t byte_len = 0;
  int bit = -1;  // 0-7 for bit addresses
  std::uint32_t string_max = 0;  // S7 STRING: max chars (2 header bytes + chars)
};

// Kepware Siemens TCP/IP syntax: I0.0 IB0 IW0 ID0 (E alias), Q/A, M, DBn.DBXb.x
// DBn.DBBb DBn.DBWb DBn.DBDb DBn.STRINGb.len, arrays "[n]" / "[r][c]".
sdk::Result<std::shared_ptr<S7Address>> parse_address(std::string_view text, sdk::TagType requested);

sdk::Result<sdk::Value> decode(const S7Address& a, std::span<const std::uint8_t> bytes);
sdk::Result<std::vector<std::uint8_t>> encode(const S7Address& a, const sdk::Value& v);

// ---- framing --------------------------------------------------------------------

// COTP connection request (TPKT included).
std::vector<std::uint8_t> connection_request(std::uint16_t local_tsap, std::uint16_t remote_tsap);
// Wraps an S7 PDU in TPKT + COTP DT.
std::vector<std::uint8_t> data_frame(std::span<const std::uint8_t> s7pdu);

std::vector<std::uint8_t> setup_communication(std::uint16_t pdu_ref, std::uint16_t pdu_size);

struct ReadItem {
  Area area;
  std::uint16_t db;
  std::uint32_t start;   // byte offset
  std::uint16_t length;  // bytes
};
std::vector<std::uint8_t> read_var(std::uint16_t pdu_ref, std::span<const ReadItem> items);

// Byte write (bit < 0) or single-bit write.
std::vector<std::uint8_t> write_var(std::uint16_t pdu_ref, Area area, std::uint16_t db, std::uint32_t start, int bit,
                                    std::span<const std::uint8_t> data);

// Bytes a read request / its response occupy, for packing items into a PDU.
inline constexpr std::size_t kHeaderJob = 10, kHeaderAck = 12;
inline constexpr std::size_t request_size(std::size_t items) { return kHeaderJob + 2 + 12 * items; }
inline constexpr std::size_t response_item_size(std::size_t len) { return 4 + len + (len % 2); }
inline constexpr std::size_t kMaxItems = 20;

struct AckData {
  std::uint16_t pdu_ref = 0;
  std::uint8_t error_class = 0, error_code = 0;
  std::uint8_t function = 0;
  std::vector<std::uint8_t> param, data;
};
// Parses an ack-data PDU (ROSCTR 3), without TPKT/COTP.
sdk::Result<AckData> parse_ack(std::span<const std::uint8_t> pdu);

struct ItemResult {
  std::uint8_t code = 0;  // 0xFF = success
  std::vector<std::uint8_t> bytes;
};
sdk::Result<std::vector<ItemResult>> parse_read_data(std::span<const std::uint8_t> data, std::size_t items);

inline constexpr std::uint8_t kItemOk = 0xFF;
sdk::StatusCode item_status(std::uint8_t code);
std::string_view item_error_text(std::uint8_t code);

}  // namespace logict::s7
