#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace logict::sdk {

// Request coalescing helper. A driver maps each tag to (area, start, length) in
// protocol units (registers, bytes, words, ...) and gets back a list of block
// reads that cover all items within per-area limits.
struct PlanItem {
  std::uint32_t area = 0;
  std::uint64_t start = 0;
  std::uint32_t length = 1;
  std::size_t index = 0;  // caller's item index, echoed back in Block::items
};

struct PlanLimits {
  std::uint32_t max_length = 1;  // largest single request, in units
  std::uint32_t max_gap = 0;     // unused units allowed between merged items
};

struct Block {
  std::uint32_t area = 0;
  std::uint64_t start = 0;
  std::uint32_t length = 0;
  std::vector<std::size_t> items;
  bool oversize = false;  // a single item longer than max_length; driver must split
};

using LimitsFn = std::function<PlanLimits(std::uint32_t area)>;

// Sorts by (area, start) and greedily merges items while the block stays within
// max_length and the hole before the next item is <= max_gap.
std::vector<Block> plan_blocks(std::span<const PlanItem> items, const LimitsFn& limits);

}  // namespace logict::sdk
