#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace logict::sdk {

using Clock = std::chrono::system_clock;
using Timestamp = Clock::time_point;
using SteadyClock = std::chrono::steady_clock;
using Deadline = SteadyClock::time_point;

// Kepware scalar data types. "Default" lets the driver choose the natural type
// for an address (e.g. Modbus 4xxxx -> Word).
enum class DataType : std::uint8_t {
  Default,
  Boolean,
  Char,   // int8
  Byte,   // uint8
  Short,  // int16
  Word,   // uint16
  Long,   // int32
  DWord,  // uint32
  LLong,  // int64
  QWord,  // uint64
  Float,
  Double,
  BCD,   // 16-bit BCD, exposed as Word
  LBCD,  // 32-bit BCD, exposed as DWord
  Date,
  String,
};

// A tag type: scalar type plus optional array dimensions (Kepware allows 1D/2D).
struct TagType {
  DataType scalar = DataType::Default;
  std::uint32_t rows = 0;  // 0 = scalar
  std::uint32_t cols = 0;  // 0 = 1D array (when rows > 0)

  bool is_array() const noexcept { return rows > 0; }
  std::uint32_t element_count() const noexcept {
    return rows == 0 ? 1 : rows * (cols == 0 ? 1 : cols);
  }
  bool operator==(const TagType&) const = default;
};

std::string_view to_string(DataType t) noexcept;
std::optional<DataType> data_type_from_string(std::string_view s) noexcept;

// Kepware Configuration API integer encoding of servermain.TAG_DATA_TYPE.
// Scalars 0..14, arrays = scalar + 20, Default = -1.
int to_kepware_code(TagType t) noexcept;
std::optional<TagType> from_kepware_code(int code) noexcept;

// Size in bytes of one element in its native (device) representation; 0 for
// String/Default.
std::size_t native_size(DataType t) noexcept;

enum class Access : std::uint8_t { ReadOnly = 0, ReadWrite = 1 };

}  // namespace logict::sdk
