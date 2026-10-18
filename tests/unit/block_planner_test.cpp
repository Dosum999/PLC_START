#include <gtest/gtest.h>

#include "logict/sdk/block_planner.h"

using namespace logict::sdk;

namespace {
LimitsFn fixed(std::uint32_t max_len, std::uint32_t gap) {
  return [=](std::uint32_t) { return PlanLimits{max_len, gap}; };
}
}  // namespace

TEST(BlockPlanner, MergesAdjacentAndSmallGaps) {
  std::vector<PlanItem> items{{4, 10, 1, 0}, {4, 11, 2, 1}, {4, 15, 1, 2}, {4, 40, 1, 3}};
  auto blocks = plan_blocks(items, fixed(125, 4));
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[0].start, 10u);
  EXPECT_EQ(blocks[0].length, 6u);
  EXPECT_EQ(blocks[0].items, (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(blocks[1].start, 40u);
}

TEST(BlockPlanner, RespectsMaxLength) {
  std::vector<PlanItem> items;
  for (std::size_t i = 0; i < 300; ++i) items.push_back({4, i, 1, i});
  auto blocks = plan_blocks(items, fixed(125, 0));
  ASSERT_EQ(blocks.size(), 3u);
  EXPECT_EQ(blocks[0].length, 125u);
  EXPECT_EQ(blocks[1].start, 125u);
  EXPECT_EQ(blocks[2].length, 50u);
}

TEST(BlockPlanner, SeparatesAreasAndSortsInput) {
  std::vector<PlanItem> items{{3, 5, 1, 0}, {4, 5, 1, 1}, {3, 1, 1, 2}};
  auto blocks = plan_blocks(items, fixed(100, 10));
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[0].area, 3u);
  EXPECT_EQ(blocks[0].start, 1u);
  EXPECT_EQ(blocks[0].items, (std::vector<std::size_t>{2, 0}));
  EXPECT_EQ(blocks[1].area, 4u);
}

TEST(BlockPlanner, OverlappingItemsShareBlock) {
  std::vector<PlanItem> items{{4, 0, 2, 0}, {4, 0, 1, 1}, {4, 1, 1, 2}};
  auto blocks = plan_blocks(items, fixed(125, 0));
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_EQ(blocks[0].length, 2u);
}

TEST(BlockPlanner, FlagsOversizeItem) {
  std::vector<PlanItem> items{{4, 0, 200, 0}, {4, 200, 1, 1}};
  auto blocks = plan_blocks(items, fixed(125, 0));
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_TRUE(blocks[0].oversize);
  EXPECT_FALSE(blocks[1].oversize);
}
