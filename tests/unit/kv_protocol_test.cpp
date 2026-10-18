#include <gtest/gtest.h>

#include "kv_protocol.h"

using namespace logict;
using namespace logict::kv;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;
using Raw = std::vector<std::uint32_t>;

namespace {
std::shared_ptr<KvAddress> p(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t});
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(KvAddress, Devices) {
  auto dm = p("DM100");
  EXPECT_STREQ(dm->device->name, "DM");
  EXPECT_EQ(dm->mode, Mode::Words);
  EXPECT_EQ(dm->start, 100u);
  EXPECT_EQ(p("DM100", DataType::Float)->points, 2u);
  EXPECT_STREQ(p("D100")->device->name, "D");      // XYM alias
  EXPECT_STREQ(p("FM5")->device->name, "FM");
  EXPECT_STREQ(p("F5")->device->name, "F");
  EXPECT_EQ(p("W1F")->start, 0x1Fu);
  auto r = p("R1015");
  EXPECT_EQ(r->mode, Mode::Bits);
  EXPECT_EQ(r->start, 10u * 16 + 15);
  EXPECT_EQ(device_text(*r->device, r->mode, r->start), "R1015");
  EXPECT_EQ(device_text(*r->device, Mode::Bits, 5), "R005");
  auto x = p("X1F");
  EXPECT_EQ(x->start, 31u);
  EXPECT_EQ(device_text(*x->device, Mode::Bits, 31), "X1F");
  auto rw = p("R200", DataType::Word);
  EXPECT_EQ(rw->mode, Mode::Words);
  EXPECT_EQ(rw->start, 2u);
  EXPECT_EQ(device_text(*rw->device, Mode::Words, 2), "R200");
  EXPECT_EQ(device_text(*p("B10")->device, Mode::Words, 1), "B10");  // B is hex: word 1 = B10
  auto tc = p("TC5");
  EXPECT_EQ(tc->mode, Mode::Dwords);
  EXPECT_EQ(tc->type.scalar, DataType::DWord);
  auto t = p("T5");
  EXPECT_EQ(t->mode, Mode::Contact);
  EXPECT_EQ(t->type.scalar, DataType::Boolean);
  EXPECT_STREQ(p("CS3")->device->name, "CS");
  EXPECT_STREQ(p("CR3")->device->name, "CR");
  EXPECT_STREQ(p("CM3")->device->name, "CM");
  auto bit = p("DM100.15");
  EXPECT_EQ(bit->bit, 15);
  EXPECT_EQ(p("DM10.5", DataType::String)->points, 3u);
  EXPECT_EQ(p("MR0 [16]", DataType::Boolean)->points, 16u);
}

TEST(KvAddress, Rejects) {
  for (auto s : {"Q1", "DM", "R016", "DM65535", "DM100.16", "R100.1", "X1G", "TM512", "DM0 [1001]"})
    EXPECT_FALSE(parse_address(s, TagType{})) << s;
  EXPECT_FALSE(parse_address("DM0", TagType{DataType::Boolean}));
  EXPECT_FALSE(parse_address("DM0", TagType{DataType::Byte}));
  EXPECT_FALSE(parse_address("R001", TagType{DataType::Word}));  // word access starts at bit 00
  EXPECT_FALSE(parse_address("TC0", TagType{DataType::Float}));
  EXPECT_FALSE(parse_address("T0", TagType{DataType::DWord}));
}

TEST(KvCommands, Format) {
  EXPECT_EQ(read_command(*find_device("DM"), Mode::Words, 100, 3), "RDS DM100.H 3\r");
  EXPECT_EQ(read_command(*find_device("R"), Mode::Bits, 16, 4), "RDS R100 4\r");
  EXPECT_EQ(read_command(*find_device("TC"), Mode::Dwords, 5, 2), "RDS TC5.D 2\r");
  EXPECT_EQ(read_command(*find_device("T"), Mode::Contact, 5, 1), "RD T5.D\r");
  std::vector<std::string> v{format_token(Mode::Words, 0x1A), format_token(Mode::Words, 0xFFFF)};
  EXPECT_EQ(write_command(*find_device("DM"), Mode::Words, 0, v), "WRS DM0.H 2 001A FFFF\r");
  std::vector<std::string> b{"1", "0"};
  EXPECT_EQ(write_command(*find_device("MR"), Mode::Bits, 17, b), "WRS MR101 2 1 0\r");
}

TEST(KvCommands, Answers) {
  auto w = parse_read_answer("1234 ABCD 0", Mode::Words, 3);
  ASSERT_TRUE(w);
  EXPECT_EQ(w->values, (Raw{0x1234, 0xABCD, 0}));
  EXPECT_EQ(parse_read_answer("E0", Mode::Words, 3)->error, "E0");
  EXPECT_EQ(error_status("E4"), sdk::status::BadNotWritable);
  EXPECT_FALSE(parse_read_answer("1234", Mode::Words, 2));
  EXPECT_FALSE(parse_read_answer("12345", Mode::Words, 1));
  EXPECT_FALSE(parse_read_answer("2", Mode::Bits, 1));
  EXPECT_EQ(parse_read_answer("4294967295", Mode::Dwords, 1)->values, (Raw{4294967295u}));
  EXPECT_EQ(parse_read_answer("1,0000000100,0000000200", Mode::Contact, 1)->values, (Raw{1}));
}

TEST(KvValues, LowWordFirst) {
  auto dw = p("DM0", DataType::DWord);
  EXPECT_EQ(*encode(*dw, Value(std::uint32_t{0x12345678})), (Raw{0x5678, 0x1234}));
  EXPECT_EQ(*decode(*dw, Raw{0x5678, 0x1234})->get_if<std::uint32_t>(), 0x12345678u);
  auto f = p("DM0", DataType::Float);
  EXPECT_EQ(*encode(*f, Value(25.0f)), (Raw{0x0000, 0x41C8}));
  auto s = p("DM0.3", DataType::String);
  EXPECT_EQ(*encode(*s, Value(std::string("ABC"))), (Raw{0x4142, 0x4300}));
  EXPECT_EQ(*decode(*s, Raw{0x4142, 0x4300})->get_if<std::string>(), "ABC");
  auto l = p("CC0", DataType::Long);
  EXPECT_EQ(*decode(*l, Raw{0xFFFFFFFF})->get_if<std::int32_t>(), -1);
  EXPECT_FALSE(encode(*p("T0"), Value(true)));
}
