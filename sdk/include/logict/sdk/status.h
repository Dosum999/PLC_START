#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace logict::sdk {

// Quality is carried as an OPC UA StatusCode end to end, so the OPC UA server
// needs no translation and no information is lost (MQTT reduces it to a bool).
using StatusCode = std::uint32_t;

namespace status {
inline constexpr StatusCode Good = 0x00000000;
inline constexpr StatusCode GoodLocalOverride = 0x00960000;
inline constexpr StatusCode Uncertain = 0x40000000;
inline constexpr StatusCode UncertainLastUsableValue = 0x40900000;
inline constexpr StatusCode UncertainNoCommunicationLastUsableValue = 0x408F0000;
inline constexpr StatusCode Bad = 0x80000000;
inline constexpr StatusCode BadInternalError = 0x80020000;
inline constexpr StatusCode BadCommunicationError = 0x80050000;
inline constexpr StatusCode BadTimeout = 0x800A0000;
inline constexpr StatusCode BadNoCommunication = 0x80310000;
inline constexpr StatusCode BadWaitingForInitialData = 0x80320000;
inline constexpr StatusCode BadNodeIdUnknown = 0x80340000;
inline constexpr StatusCode BadNotReadable = 0x803A0000;
inline constexpr StatusCode BadNotWritable = 0x803B0000;
inline constexpr StatusCode BadOutOfRange = 0x803C0000;
inline constexpr StatusCode BadNotSupported = 0x803D0000;
inline constexpr StatusCode BadTypeMismatch = 0x80740000;
inline constexpr StatusCode BadConfigurationError = 0x80890000;
inline constexpr StatusCode BadNotConnected = 0x808A0000;
inline constexpr StatusCode BadDeviceFailure = 0x808B0000;
inline constexpr StatusCode BadOutOfService = 0x808D0000;
inline constexpr StatusCode BadInvalidArgument = 0x80AB0000;
}  // namespace status

constexpr bool is_good(StatusCode s) noexcept { return (s & 0xC0000000u) == 0; }
constexpr bool is_uncertain(StatusCode s) noexcept { return (s & 0xC0000000u) == 0x40000000u; }
constexpr bool is_bad(StatusCode s) noexcept { return (s & 0x80000000u) != 0; }

std::string_view status_name(StatusCode s) noexcept;

struct Error {
  StatusCode code = status::Bad;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> make_error(StatusCode code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message)});
}

}  // namespace logict::sdk
