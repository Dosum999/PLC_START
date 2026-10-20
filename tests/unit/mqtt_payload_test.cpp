#include "mqtt_payload.h"

#include <gtest/gtest.h>

using namespace logict;
using namespace logict::mqtt_client;
using sdk::DataType;

namespace {
std::shared_ptr<MqttAddress> addr(std::string_view text, sdk::TagType t = {}) {
  auto a = parse_address(text, t);
  EXPECT_TRUE(a) << text;
  return a ? *a : nullptr;
}
}  // namespace

TEST(MqttPayload, Addresses) {
  auto a = addr("plant/line1/temp");
  EXPECT_EQ(a->topic, "plant/line1/temp");
  EXPECT_FALSE(a->has_path);
  EXPECT_EQ(a->type.scalar, DataType::String);
  a = addr(" plant/line1 | $.data.values[2] ");
  EXPECT_EQ(a->topic, "plant/line1");
  ASSERT_EQ(a->path.size(), 3u);
  EXPECT_EQ(std::get<std::string>(a->path[0]), "data");
  EXPECT_EQ(std::get<std::string>(a->path[1]), "values");
  EXPECT_EQ(std::get<std::size_t>(a->path[2]), 2u);
  EXPECT_EQ(a->type.scalar, DataType::Double);
  a = addr("plant/arr [3]", {DataType::Short, 1});
  EXPECT_EQ(a->topic, "plant/arr");
  EXPECT_EQ(a->type, (sdk::TagType{DataType::Short, 3}));
  a = addr("plant/m|rows [2][3]", {DataType::Word, 1});
  EXPECT_EQ(std::get<std::string>(a->path[0]), "rows");
  EXPECT_EQ(a->type, (sdk::TagType{DataType::Word, 2, 3}));
  EXPECT_FALSE(parse_address("plant/arr", {DataType::Short, 1}));  // array type without dimensions
  EXPECT_FALSE(parse_address("plant/arr [0]", {DataType::Short, 1}));
  EXPECT_TRUE(addr("plant/+/temp")->wildcard);
  EXPECT_TRUE(addr("plant/#")->wildcard);
  for (auto bad : {"", "|temp", "a/b#", "a/#/b", "a+/b", "a|", "a|x[", "a|x[q]", "a|x..y"})
    EXPECT_FALSE(parse_address(bad, {})) << bad;
}

TEST(MqttPayload, TopicMatching) {
  EXPECT_TRUE(topic_matches("a/b", "a/b"));
  EXPECT_FALSE(topic_matches("a/b", "a/b/c"));
  EXPECT_FALSE(topic_matches("a/b/c", "a/b"));
  EXPECT_TRUE(topic_matches("a/+/c", "a/x/c"));
  EXPECT_FALSE(topic_matches("a/+/c", "a/x"));
  EXPECT_TRUE(topic_matches("a/+", "a/"));  // empty level
  EXPECT_TRUE(topic_matches("a/#", "a"));   // # includes the parent level
  EXPECT_TRUE(topic_matches("a/#", "a/b/c"));
  EXPECT_TRUE(topic_matches("#", "x/y"));
  EXPECT_FALSE(topic_matches("+", "x/y"));
  EXPECT_FALSE(topic_matches("#", "$SYS/broker"));  // $-topics only match explicitly
  EXPECT_TRUE(topic_matches("$SYS/#", "$SYS/broker"));
}

TEST(MqttPayload, DecodeWholePayload) {
  EXPECT_EQ(*decode(*addr("t"), "hello world"), sdk::Value(std::string("hello world")));
  EXPECT_EQ(*decode(*addr("t"), "{\"a\":1}"), sdk::Value(std::string("{\"a\":1}")));  // String keeps raw text
  EXPECT_EQ(*decode(*addr("t", {DataType::Float}), " 25.5 "), sdk::Value(25.5f));
  EXPECT_EQ(*decode(*addr("t", {DataType::Boolean}), "true"), sdk::Value(true));
  EXPECT_EQ(*decode(*addr("t", {DataType::Long}), "-7"), sdk::Value(std::int32_t{-7}));
  EXPECT_FALSE(decode(*addr("t", {DataType::Word}), "70000"));   // out of range
  EXPECT_FALSE(decode(*addr("t", {DataType::Word}), "abc"));
  auto arr = decode(*addr("t [3]", {DataType::Short, 1}), "[1,-2,3]");
  ASSERT_TRUE(arr);
  EXPECT_EQ(*arr, sdk::Value(sdk::Array{std::int16_t{1}, std::int16_t{-2}, std::int16_t{3}}));
  EXPECT_FALSE(decode(*addr("t [2]", {DataType::Short, 1}), "[1,-2,3]"));  // length
}

TEST(MqttPayload, DecodeJsonPath) {
  const std::string p = R"({"temp":21.5,"ok":true,"name":"L1","data":{"values":[10,20,30]},"obj":{"x":1}})";
  EXPECT_EQ(*decode(*addr("t|temp"), p), sdk::Value(21.5));
  EXPECT_EQ(*decode(*addr("t|ok", {DataType::Boolean}), p), sdk::Value(true));
  EXPECT_EQ(*decode(*addr("t|name", {DataType::String}), p), sdk::Value(std::string("L1")));
  EXPECT_EQ(*decode(*addr("t|$.data.values[2]", {DataType::Word}), p), sdk::Value(std::uint16_t{30}));
  EXPECT_EQ(*decode(*addr("t|data.values [3]", {DataType::DWord, 1}), p),
            sdk::Value(sdk::Array{std::uint32_t{10}, std::uint32_t{20}, std::uint32_t{30}}));
  EXPECT_EQ(*decode(*addr("t|obj", {DataType::String}), p), sdk::Value(std::string(R"({"x":1})")));
  EXPECT_EQ(decode(*addr("t|missing"), p).error().code, sdk::status::BadOutOfRange);
  EXPECT_EQ(decode(*addr("t|data.values[5]"), p).error().code, sdk::status::BadOutOfRange);
  EXPECT_FALSE(decode(*addr("t|temp"), "not json"));
}

TEST(MqttPayload, Encode) {
  EXPECT_EQ(*encode(*addr("t"), sdk::Value(std::string("on"))), "on");
  EXPECT_EQ(*encode(*addr("t", {DataType::Word}), sdk::Value(std::uint16_t{42})), "42");
  EXPECT_EQ(*encode(*addr("t", {DataType::Boolean}), sdk::Value(true)), "true");
  EXPECT_EQ(*encode(*addr("t [2]", {DataType::Short, 1}), sdk::Value(sdk::Array{std::int16_t{1}, std::int16_t{-1}})), "[1,-1]");
  EXPECT_EQ(*encode(*addr("t|cmd.speed"), sdk::Value(12.5)), R"({"cmd":{"speed":12.5}})");
  EXPECT_EQ(*encode(*addr("t|run", {DataType::Byte}), sdk::Value(std::uint8_t{3})), R"({"run":3})");
  EXPECT_EQ(encode(*addr("t/+"), sdk::Value(std::string("x"))).error().code, sdk::status::BadNotWritable);
  EXPECT_EQ(encode(*addr("t|a[0]"), sdk::Value(1.0)).error().code, sdk::status::BadNotWritable);
}
