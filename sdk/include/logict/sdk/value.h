#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

namespace logict::sdk {

struct DateTime {
  Timestamp time{};
  bool operator==(const DateTime&) const = default;
};

struct Value;
using Array = std::vector<Value>;

// A tag value. Array elements are Values of one scalar type; 2D arrays are
// stored row-major with dimensions taken from the tag's TagType.
struct Value {
  using Storage = std::variant<std::monostate, bool, std::int8_t, std::uint8_t, std::int16_t,
                               std::uint16_t, std::int32_t, std::uint32_t, std::int64_t,
                               std::uint64_t, float, double, std::string, DateTime, Array>;
  Storage data;

  Value() = default;
  template <typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, Value> && std::is_constructible_v<Storage, T &&>)
  Value(T&& v) : data(std::forward<T>(v)) {}  // NOLINT(google-explicit-constructor)

  bool empty() const noexcept { return std::holds_alternative<std::monostate>(data); }
  bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
  template <typename T>
  const T* get_if() const noexcept { return std::get_if<T>(&data); }

  bool operator==(const Value&) const = default;
};

// Scalar DataType held by a value (Default for empty, element type for arrays).
DataType type_of(const Value& v) noexcept;

// Numeric view of a scalar value (bool -> 0/1). nullopt for strings that do not
// parse, arrays and empty values.
std::optional<double> to_double(const Value& v) noexcept;

// Human/JSON-friendly text. Arrays use Kepware notation: "[1,2,3]" or "[1,2][3,4]".
std::string to_string(const Value& v, const TagType* shape = nullptr);

// Converts to the given scalar type (element-wise for arrays). Fails with
// BadOutOfRange if the value does not fit, BadTypeMismatch if not convertible.
// BCD/LBCD convert to their Word/DWord carrier types; Date converts to DateTime.
Result<Value> convert(const Value& v, DataType target);

// Default value of a type (zero / empty string / epoch), used before first read.
Value default_value(TagType t);

// Value + Quality + Timestamps.
struct Vqt {
  Value value;
  StatusCode quality = status::BadWaitingForInitialData;
  Timestamp source_time{};
  Timestamp server_time{};
};

}  // namespace logict::sdk
