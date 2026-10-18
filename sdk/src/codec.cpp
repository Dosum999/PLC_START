#include "logict/sdk/codec.h"

namespace logict::sdk::codec {

std::optional<std::uint32_t> bcd_decode(std::uint32_t bcd, int digits) noexcept {
  std::uint32_t value = 0;
  std::uint32_t scale = 1;
  for (int i = 0; i < digits; ++i) {
    std::uint32_t nibble = (bcd >> (4 * i)) & 0xF;
    if (nibble > 9) return std::nullopt;
    value += nibble * scale;
    scale *= 10;
  }
  if (digits < 8 && (bcd >> (4 * digits)) != 0) return std::nullopt;
  return value;
}

std::optional<std::uint32_t> bcd_encode(std::uint32_t value, int digits) noexcept {
  std::uint32_t bcd = 0;
  for (int i = 0; i < digits; ++i) {
    bcd |= (value % 10) << (4 * i);
    value /= 10;
  }
  if (value != 0) return std::nullopt;
  return bcd;
}

}  // namespace logict::sdk::codec
