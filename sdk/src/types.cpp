#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

#include <algorithm>
#include <array>
#include <utility>

namespace logict::sdk {

namespace {
constexpr std::array<std::pair<DataType, std::string_view>, 16> kNames{{
    {DataType::Default, "Default"}, {DataType::Boolean, "Boolean"}, {DataType::Char, "Char"},
    {DataType::Byte, "Byte"},       {DataType::Short, "Short"},     {DataType::Word, "Word"},
    {DataType::Long, "Long"},       {DataType::DWord, "DWord"},     {DataType::LLong, "LLong"},
    {DataType::QWord, "QWord"},     {DataType::Float, "Float"},     {DataType::Double, "Double"},
    {DataType::BCD, "BCD"},         {DataType::LBCD, "LBCD"},       {DataType::Date, "Date"},
    {DataType::String, "String"},
}};

// Kepware Configuration API codes for servermain.TAG_DATA_TYPE.
// TODO(verify): taken from Kepware Config API examples; confirm against /config/v1/doc.
constexpr std::array<std::pair<DataType, int>, 15> kKepwareCodes{{
    {DataType::String, 0}, {DataType::Boolean, 1}, {DataType::Char, 2},  {DataType::Byte, 3},
    {DataType::Short, 4},  {DataType::Word, 5},    {DataType::Long, 6},  {DataType::DWord, 7},
    {DataType::Float, 8},  {DataType::Double, 9},  {DataType::BCD, 10},  {DataType::LBCD, 11},
    {DataType::Date, 12},  {DataType::LLong, 13},  {DataType::QWord, 14},
}};
constexpr int kArrayOffset = 20;
}  // namespace

std::string_view to_string(DataType t) noexcept {
  for (auto& [type, name] : kNames)
    if (type == t) return name;
  return "Unknown";
}

std::optional<DataType> data_type_from_string(std::string_view s) noexcept {
  for (auto& [type, name] : kNames)
    if (name.size() == s.size() &&
        std::equal(name.begin(), name.end(), s.begin(),
                   [](char a, char b) { return (a | 0x20) == (b | 0x20); }))
      return type;
  return std::nullopt;
}

int to_kepware_code(TagType t) noexcept {
  if (t.scalar == DataType::Default) return -1;
  for (auto& [type, code] : kKepwareCodes)
    if (type == t.scalar) return t.is_array() ? code + kArrayOffset : code;
  return -1;
}

std::optional<TagType> from_kepware_code(int code) noexcept {
  if (code == -1) return TagType{};
  bool array = code >= kArrayOffset;
  int base = array ? code - kArrayOffset : code;
  for (auto& [type, c] : kKepwareCodes)
    if (c == base) return TagType{type, array ? 1u : 0u, 0};
  return std::nullopt;
}

std::size_t native_size(DataType t) noexcept {
  switch (t) {
    case DataType::Boolean:
    case DataType::Char:
    case DataType::Byte:
      return 1;
    case DataType::Short:
    case DataType::Word:
    case DataType::BCD:
      return 2;
    case DataType::Long:
    case DataType::DWord:
    case DataType::Float:
    case DataType::LBCD:
      return 4;
    case DataType::LLong:
    case DataType::QWord:
    case DataType::Double:
    case DataType::Date:
      return 8;
    case DataType::Default:
    case DataType::String:
      return 0;
  }
  return 0;
}

std::string_view status_name(StatusCode s) noexcept {
  switch (s) {
    case status::Good: return "Good";
    case status::GoodLocalOverride: return "GoodLocalOverride";
    case status::Uncertain: return "Uncertain";
    case status::UncertainLastUsableValue: return "UncertainLastUsableValue";
    case status::UncertainNoCommunicationLastUsableValue: return "UncertainNoCommunicationLastUsableValue";
    case status::Bad: return "Bad";
    case status::BadInternalError: return "BadInternalError";
    case status::BadCommunicationError: return "BadCommunicationError";
    case status::BadTimeout: return "BadTimeout";
    case status::BadNoCommunication: return "BadNoCommunication";
    case status::BadWaitingForInitialData: return "BadWaitingForInitialData";
    case status::BadNodeIdUnknown: return "BadNodeIdUnknown";
    case status::BadNotReadable: return "BadNotReadable";
    case status::BadNotWritable: return "BadNotWritable";
    case status::BadOutOfRange: return "BadOutOfRange";
    case status::BadNotSupported: return "BadNotSupported";
    case status::BadTypeMismatch: return "BadTypeMismatch";
    case status::BadConfigurationError: return "BadConfigurationError";
    case status::BadNotConnected: return "BadNotConnected";
    case status::BadDeviceFailure: return "BadDeviceFailure";
    case status::BadOutOfService: return "BadOutOfService";
    case status::BadInvalidArgument: return "BadInvalidArgument";
    default:
      return is_good(s) ? "Good" : is_uncertain(s) ? "Uncertain" : "Bad";
  }
}

}  // namespace logict::sdk
