#include "logict/sdk/block_planner.h"

#include <algorithm>

namespace logict::sdk {

std::vector<Block> plan_blocks(std::span<const PlanItem> items, const LimitsFn& limits) {
  std::vector<const PlanItem*> sorted;
  sorted.reserve(items.size());
  for (const auto& it : items) sorted.push_back(&it);
  std::sort(sorted.begin(), sorted.end(), [](const PlanItem* a, const PlanItem* b) {
    return a->area != b->area ? a->area < b->area : a->start < b->start;
  });

  std::vector<Block> blocks;
  PlanLimits lim{};
  for (const PlanItem* it : sorted) {
    if (blocks.empty() || blocks.back().area != it->area) lim = limits(it->area);
    const std::uint64_t end = it->start + it->length;

    if (!blocks.empty() && blocks.back().area == it->area && !blocks.back().oversize) {
      Block& b = blocks.back();
      const std::uint64_t b_end = b.start + b.length;
      const std::uint64_t gap = it->start > b_end ? it->start - b_end : 0;
      const std::uint64_t new_end = std::max(b_end, end);
      if (gap <= lim.max_gap && new_end - b.start <= lim.max_length) {
        b.length = static_cast<std::uint32_t>(new_end - b.start);
        b.items.push_back(it->index);
        continue;
      }
    }
    blocks.push_back(Block{it->area, it->start, it->length, {it->index}, it->length > lim.max_length});
  }
  return blocks;
}

}  // namespace logict::sdk
