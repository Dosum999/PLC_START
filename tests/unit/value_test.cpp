#include <gtest/gtest.h>

#include "logict/sdk/value.h"

using namespace logict::sdk;

TEST(Value, ConvertsWithinRange) {
  auto r = convert(Value(std::int32_t{300}), DataType::Word);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r->get_if<std::uint16_t>(), 300);
}

TEST(Value, RejectsOutOfRange) {
  auto r = convert(Value(std::int32_t{-1}), DataType::Word);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, status::BadOutOfRange);
  EXPECT_FALSE(convert(Value(70000.0), DataType::Short));
}

TEST(Value, ParsesStrings) {
  auto r = convert(Value(std::string("42")), DataType::Long);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r->get_if<std::int32_t>(), 42);
  auto f = convert(Value(std::string("1.5")), DataType::Float);
  ASSERT_TRUE(f);
  EXPECT_FLOAT_EQ(*f->get_if<float>(), 1.5f);
  EXPECT_FALSE(convert(Value(std::string("abc")), DataType::Long));
  auto b = convert(Value(std::string("true")), DataType::Boolean);
  ASSERT_TRUE(b);
  EXPECT_TRUE(*b->get_if<bool>());
}

TEST(Value, BcdRange) {
  EXPECT_TRUE(convert(Value(9999), DataType::BCD));
  EXPECT_FALSE(convert(Value(10000), DataType::BCD));
  EXPECT_TRUE(convert(Value(99999999), DataType::LBCD));
}

TEST(Value, ArrayToStringUsesKepwareNotation) {
  Value v(Array{Value(1), Value(2), Value(3), Value(4)});
  EXPECT_EQ(to_string(v), "[1,2,3,4]");
  TagType shape{DataType::Long, 2, 2};
  EXPECT_EQ(to_string(v, &shape), "[1,2][3,4]");
}

TEST(Value, ConvertArrayElementwise) {
  auto r = convert(Value(Array{Value(1.0), Value(2.0)}), DataType::Short);
  ASSERT_TRUE(r);
  const auto* arr = r->get_if<Array>();
  ASSERT_NE(arr, nullptr);
  EXPECT_EQ(*(*arr)[1].get_if<std::int16_t>(), 2);
  EXPECT_EQ(type_of(*r), DataType::Short);
}

TEST(Value, DefaultValueMatchesType) {
  EXPECT_EQ(type_of(default_value({DataType::Float})), DataType::Float);
  auto arr = default_value({DataType::Word, 3, 0});
  ASSERT_TRUE(arr.is_array());
  EXPECT_EQ(arr.get_if<Array>()->size(), 3u);
}

TEST(Types, KepwareCodesRoundTrip) {
  for (int code : {0, 1, 5, 9, 14, 20, 25, 34}) {
    auto t = from_kepware_code(code);
    ASSERT_TRUE(t) << code;
    EXPECT_EQ(to_kepware_code(*t), code);
  }
  EXPECT_EQ(from_kepware_code(-1)->scalar, DataType::Default);
  EXPECT_FALSE(from_kepware_code(15));
}
