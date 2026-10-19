#pragma once

// Schema helpers shared by the CIMON drivers.

#include <nlohmann/json.hpp>

namespace logict::cimon {

inline nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

inline constexpr const char* kAddressHelp =
    "Bits: X0000, Y0010, M0104 (word 10, bit 4), L, K, F; contacts T5, C5; words D100, S10, Z0, TC5, TS5, CC5, CS5; "
    "bit of a word D100.5; bit devices with a word type (M0010 as Word) read word 10; strings D100.20; arrays D100 [10].";

}  // namespace logict::cimon
