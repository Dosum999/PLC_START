#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace logict::sdk::codec {

// Order of the bytes of a multi-byte value as it appears on the wire, written
// relative to the big-endian value "ABCD" (A = most significant byte).
//   ABCD  big endian                  (Modbus default, S7)
//   CDAB  16-bit words swapped        (Kepware "first word low")
//   BADC  bytes swapped in each word
//   DCBA  little endian               (LS XGT, MELSEC binary, CIP)
// For 64-bit values the same rule applies to all four words.
enum class ByteOrder : std::uint8_t { ABCD, CDAB, BADC, DCBA };

// Reorders `n` bytes between wire order and big-endian (the operation is its
// own inverse). n must be even except for n == 1.
inline void reorder(std::uint8_t* p, std::size_t n, ByteOrder order) noexcept {
  if (n < 2) return;
  switch (order) {
    case ByteOrder::ABCD:
      break;
    case ByteOrder::DCBA:
      std::reverse(p, p + n);
      break;
    case ByteOrder::BADC:
      for (std::size_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]);
      break;
    case ByteOrder::CDAB:
      for (std::size_t i = 0, j = n - 2; i < j; i += 2, j -= 2) {
        std::swap(p[i], p[j]);
        std::swap(p[i + 1], p[j + 1]);
      }
      break;
  }
}

template <typename T>
  requires std::is_arithmetic_v<T>
T load(std::span<const std::uint8_t> src, ByteOrder order) noexcept {
  std::uint8_t buf[sizeof(T)];
  std::memcpy(buf, src.data(), sizeof(T));
  reorder(buf, sizeof(T), order);  // -> big endian
  if constexpr (std::endian::native == std::endian::little) std::reverse(buf, buf + sizeof(T));
  T v;
  std::memcpy(&v, buf, sizeof(T));
  return v;
}

template <typename T>
  requires std::is_arithmetic_v<T>
void store(T v, std::span<std::uint8_t> dst, ByteOrder order) noexcept {
  std::uint8_t buf[sizeof(T)];
  std::memcpy(buf, &v, sizeof(T));
  if constexpr (std::endian::native == std::endian::little) std::reverse(buf, buf + sizeof(T));
  reorder(buf, sizeof(T), order);  // big endian -> wire
  std::memcpy(dst.data(), buf, sizeof(T));
}

// Packed BCD <-> binary. nullopt if a nibble is not a decimal digit or the value
// does not fit in `digits` digits.
std::optional<std::uint32_t> bcd_decode(std::uint32_t bcd, int digits) noexcept;
std::optional<std::uint32_t> bcd_encode(std::uint32_t value, int digits) noexcept;

}  // namespace logict::sdk::codec
