#include <gtest/gtest.h>

#include "runtime/scaling.h"

using namespace logict;
using sdk::DataType;
using sdk::Value;

namespace {
model::Scaling linear() {
  model::Scaling s;
  s.type = model::Scaling::Type::Linear;
  s.raw_low = 0;
  s.raw_high = 4000;
  s.scaled_low = 0;
  s.scaled_high = 100;
  return s;
}
}  // namespace

TEST(Scaling, Linear) {
  auto r = core::scale(Value(std::uint16_t{2000}), linear());
  ASSERT_TRUE(r);
  EXPECT_DOUBLE_EQ(*r->get_if<double>(), 50.0);
}

TEST(Scaling, LinearInverseRoundsToRawType) {
  auto r = core::unscale(Value(25.01), linear(), DataType::Word);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r->get_if<std::uint16_t>(), 1000);
}

TEST(Scaling, Clamp) {
  auto s = linear();
  s.clamp_high = true;
  auto r = core::scale(Value(std::uint16_t{5000}), s);
  EXPECT_DOUBLE_EQ(*r->get_if<double>(), 100.0);
}

TEST(Scaling, SquareRootAndNegate) {
  auto s = linear();
  s.type = model::Scaling::Type::SquareRoot;
  s.negate = true;
  auto r = core::scale(Value(std::uint16_t{1000}), s);  // sqrt(0.25) = 0.5
  EXPECT_DOUBLE_EQ(*r->get_if<double>(), -50.0);
  auto back = core::unscale(Value(-50.0), s, DataType::Word);
  EXPECT_EQ(*back->get_if<std::uint16_t>(), 1000);
}

TEST(Scaling, NoneConvertsOnly) {
  auto r = core::unscale(Value(12.0), model::Scaling{}, DataType::Short);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r->get_if<std::int16_t>(), 12);
}
