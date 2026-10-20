#pragma once

// Sparkplug B (Eclipse Sparkplug 3.0) pieces for the MQTT Client driver:
// topic names and the protobuf payload (scalar metrics; data sets, templates
// and properties are skipped). No I/O here.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::sparkplug {

enum class MsgType { NBirth, NDeath, DBirth, DDeath, NData, DData, NCmd, DCmd };

// spBv1.0/<group>/<type>/<edge node>[/<device>]; STATE topics are not parsed.
struct Topic {
  std::string group;
  MsgType type{};
  std::string edge, device;
};
std::optional<Topic> parse_topic(std::string_view topic);
std::string make_topic(std::string_view group, MsgType type, std::string_view edge, std::string_view device = {});

// Metric data types (Sparkplug B DataType enum).
enum DataType : std::uint32_t {
  Int8 = 1, Int16 = 2, Int32 = 3, Int64 = 4, UInt8 = 5, UInt16 = 6, UInt32 = 7, UInt64 = 8,
  Float = 9, Double = 10, Boolean = 11, String = 12, DateTime = 13, Text = 14, UUID = 15,
};

struct Metric {
  std::string name;
  bool has_name = false;
  std::uint64_t alias = 0;
  bool has_alias = false;
  std::uint64_t timestamp = 0;  // ms since the epoch, 0 if absent
  std::uint32_t datatype = 0;   // 0 if absent (DATA messages may leave it out)
  bool is_null = false;

  // The value field as received; interpreted with the datatype by value().
  enum class Field { None, Int, Long, Float, Double, Bool, String, Other } field = Field::None;
  std::uint64_t bits = 0;  // Int/Long/Bool raw, Float/Double IEEE bits
  std::string text;        // String
};

struct Payload {
  std::uint64_t timestamp = 0;
  std::optional<std::uint64_t> seq;
  std::vector<Metric> metrics;
};

sdk::Result<Payload> decode(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> encode(const Payload& p);

// Value of a metric for the given data type (its own, or the one from BIRTH).
// Signed integers accept both the width-specific two's complement written by
// Eclipse Tahu (Int8 -1 = 255) and sign-extended 32-bit values.
sdk::Result<sdk::Value> value(const Metric& m, std::uint32_t datatype);

// Fills the value field of `m` from `v` converted to `datatype` (sets m.datatype).
sdk::StatusCode set_value(Metric& m, std::uint32_t datatype, const sdk::Value& v);

}  // namespace logict::sparkplug
