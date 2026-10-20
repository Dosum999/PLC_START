// Sparkplug B payloads against bytes serialised by the Eclipse Tahu protobuf
// module (sparkplug_b_pb2 + addMetric, as shipped in mqtt-spb-wrapper).

#include "sparkplug.h"

#include <gtest/gtest.h>

using namespace logict;
using namespace logict::sparkplug;

namespace {

std::vector<std::uint8_t> hex(std::string_view s) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
  return out;
}

// NBIRTH-like payload: timestamp 1700000000000, seq 0, nine metrics with
// aliases 1-9 (Count also carries a property set).
constexpr std::string_view kBirth =
    "0880d095ffbc31121e0a0b4d6f746f722f537065656410011880d095ffbc31200350fbffffff0f12160a0454656d7010021880d095ffbc3120"
    "09650000ac4112120a0352756e10031880d095ffbc31200b700112150a044e616d6510041880d095ffbc31200c7a024c3112290a05436f756e"
    "7410051880d095ffbc3120014a120a07656e67556e69741207080c420372706d50ff01121b0a0342696710061880d095ffbc31200858858080"
    "8080808080800112170a014410071880d095ffbc31200a69000000000000f43f12180a045768656e10081880d095ffbc31200d58fbd095ffbc"
    "3112140a0353313610091880d095ffbc31200250d4fd031800";

// NCMD-like payload: Motor/Speed Int32 -7, Run Boolean false, no aliases.
constexpr std::string_view kCmd =
    "0880d095ffbc31121c0a0b4d6f746f722f53706565641880d095ffbc31200350f9ffffff0f12100a0352756e1880d095ffbc31200b7000";

}  // namespace

TEST(Sparkplug, Topics) {
  auto t = parse_topic("spBv1.0/Plant1/DDATA/Edge1/Press3");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->group, "Plant1");
  EXPECT_EQ(t->type, MsgType::DData);
  EXPECT_EQ(t->edge, "Edge1");
  EXPECT_EQ(t->device, "Press3");
  t = parse_topic("spBv1.0/Plant1/NBIRTH/Edge1");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->type, MsgType::NBirth);
  EXPECT_TRUE(t->device.empty());
  EXPECT_FALSE(parse_topic("spBv1.0/Plant1/NBIRTH/Edge1/Dev"));  // node message with a device
  EXPECT_FALSE(parse_topic("spBv1.0/Plant1/DDATA/Edge1"));       // device message without one
  EXPECT_FALSE(parse_topic("spBv1.0/STATE/host1"));
  EXPECT_FALSE(parse_topic("spAv1.0/G/NDATA/E"));
  EXPECT_EQ(make_topic("G", MsgType::DCmd, "E", "D"), "spBv1.0/G/DCMD/E/D");
  EXPECT_EQ(make_topic("G", MsgType::NCmd, "E"), "spBv1.0/G/NCMD/E");
}

TEST(Sparkplug, DecodeTahuBirth) {
  auto p = decode(hex(kBirth));
  ASSERT_TRUE(p) << p.error().message;
  EXPECT_EQ(p->timestamp, 1700000000000u);
  EXPECT_EQ(p->seq, 0u);
  ASSERT_EQ(p->metrics.size(), 9u);
  auto val = [&](std::size_t i) {
    const auto& m = p->metrics[i];
    EXPECT_EQ(m.alias, i + 1);
    EXPECT_EQ(m.timestamp, 1700000000000u);
    return *value(m, m.datatype);
  };
  EXPECT_EQ(p->metrics[0].name, "Motor/Speed");
  EXPECT_EQ(val(0), sdk::Value(std::int32_t{-5}));
  EXPECT_EQ(val(1), sdk::Value(21.5f));
  EXPECT_EQ(val(2), sdk::Value(true));
  EXPECT_EQ(val(3), sdk::Value(std::string("L1")));
  EXPECT_EQ(val(4), sdk::Value(std::int8_t{-1}));  // Tahu writes 255; the property set is skipped
  EXPECT_EQ(val(5), sdk::Value(std::uint64_t{(1ull << 63) + 5}));
  EXPECT_EQ(val(6), sdk::Value(1.25));
  EXPECT_EQ(val(7), sdk::Value(sdk::DateTime{sdk::Timestamp(std::chrono::milliseconds(1700000000123))}));
  EXPECT_EQ(val(8), sdk::Value(std::int16_t{-300}));
}

TEST(Sparkplug, SignExtendedIntegersToo) {
  Metric m;
  m.field = Metric::Field::Int;
  m.bits = 0xFFFFFFFFu;  // -1 sign-extended to 32 bits
  EXPECT_EQ(*value(m, Int8), sdk::Value(std::int8_t{-1}));
  EXPECT_EQ(*value(m, Int16), sdk::Value(std::int16_t{-1}));
  EXPECT_FALSE(value(m, Float));  // wrong value field for the type
}

TEST(Sparkplug, EncodeMatchesTahu) {
  Payload p;
  p.timestamp = 1700000000000;
  Metric speed, run;
  speed.name = "Motor/Speed", speed.has_name = true, speed.timestamp = 1700000000000;
  ASSERT_EQ(set_value(speed, Int32, sdk::Value(-7.0)), sdk::status::Good);
  run.name = "Run", run.has_name = true, run.timestamp = 1700000000000;
  ASSERT_EQ(set_value(run, Boolean, sdk::Value(false)), sdk::status::Good);
  p.metrics = {speed, run};
  EXPECT_EQ(encode(p), hex(kCmd));
  EXPECT_NE(set_value(speed, UInt8, sdk::Value(300)), sdk::status::Good);  // out of range
}

TEST(Sparkplug, RoundTrip) {
  Payload p;
  p.timestamp = 5;
  p.seq = 255;
  for (auto [type, v] : std::vector<std::pair<std::uint32_t, sdk::Value>>{
           {Int8, sdk::Value(std::int8_t{-100})}, {Int64, sdk::Value(std::int64_t{-1})}, {UInt32, sdk::Value(std::uint32_t{4000000000u})},
           {Double, sdk::Value(-0.5)}, {String, sdk::Value(std::string("x"))}}) {
    Metric m;
    m.alias = p.metrics.size(), m.has_alias = true;
    ASSERT_EQ(set_value(m, type, v), sdk::status::Good);
    p.metrics.push_back(m);
  }
  auto back = decode(encode(p));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->seq, 255u);
  EXPECT_EQ(*value(back->metrics[0], Int8), sdk::Value(std::int8_t{-100}));
  EXPECT_EQ(*value(back->metrics[1], Int64), sdk::Value(std::int64_t{-1}));
  EXPECT_EQ(*value(back->metrics[2], UInt32), sdk::Value(std::uint32_t{4000000000u}));
  EXPECT_EQ(*value(back->metrics[3], Double), sdk::Value(-0.5));
  EXPECT_EQ(*value(back->metrics[4], String), sdk::Value(std::string("x")));
  EXPECT_FALSE(decode(hex("0a05")));  // truncated
}
