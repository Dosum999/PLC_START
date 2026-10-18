#include <gtest/gtest.h>

#include "xgt_protocol.h"

using namespace logict;
using namespace logict::xgt;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;

namespace {
std::shared_ptr<XgtAddress> k(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t}, Series::XGK);
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
std::shared_ptr<XgtAddress> i(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t}, Series::XGI);
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(XgtAddress, XgkWordDevices) {
  auto d = k("D100");
  EXPECT_EQ(d->area, "D");
  EXPECT_EQ(d->byte_offset, 200u);
  EXPECT_EQ(d->byte_len, 2u);
  EXPECT_EQ(d->type.scalar, DataType::Word);
  EXPECT_TRUE(d->block);
  EXPECT_EQ(k("d100", DataType::Float)->byte_len, 4u);
  EXPECT_EQ(k("D100 [10]", DataType::Short)->byte_len, 20u);
  auto s = k("D100.20", DataType::String);
  EXPECT_EQ(s->string_len, 20u);
  EXPECT_EQ(s->byte_len, 20u);
  EXPECT_EQ(k("D100.5", DataType::String)->byte_len, 6u);  // rounded up to whole words
}

TEST(XgtAddress, XgkBits) {
  auto m = k("M0010A", DataType::Boolean);  // XG5000: word 10, bit A
  EXPECT_EQ(m->bit_index, 170u);
  EXPECT_EQ(m->byte_offset, 21u);
  EXPECT_EQ(m->bit, 2);
  EXPECT_EQ(m->var, "%MX170");
  EXPECT_TRUE(m->bit_write_direct);
  EXPECT_EQ(k("M5", DataType::Boolean)->bit_index, 5u);
  // Manual example: bit C of M172 is %MX2764.
  EXPECT_EQ(k("M172C", DataType::Boolean)->var, "%MX2764");
  auto db = k("D100.A");
  EXPECT_EQ(db->type.scalar, DataType::Boolean);
  EXPECT_EQ(db->bit_index, 1610u);
  EXPECT_FALSE(db->bit_write_direct);  // D bits are written by read-modify-write
  auto t = k("T5", DataType::Boolean);
  EXPECT_EQ(t->var, "%TX5");
  EXPECT_FALSE(t->block);
  EXPECT_EQ(k("T5")->byte_offset, 10u);  // current value
}

TEST(XgtAddress, XgkNonBlockAreas) {
  auto zr = k("ZR100", DataType::DWord);
  EXPECT_FALSE(zr->block);
  EXPECT_EQ(unit_vars(*zr), (std::vector<std::string>{"%ZRW100", "%ZRW101"}));
  EXPECT_EQ(unit_vars(*k("R7")), (std::vector<std::string>{"%RW7"}));
  EXPECT_EQ(k("%DW100")->byte_offset, 200u);
  EXPECT_EQ(k("%DD10")->byte_offset, 40u);
}

TEST(XgtAddress, Rejects) {
  for (auto s : {"X100", "D", "DABC", "D100.G", "Q5", "ZR100 [20]", "%DQ1", "%XW1", "D100 [0]"})
    EXPECT_FALSE(parse_address(s, TagType{}, Series::XGK)) << s;
  EXPECT_FALSE(parse_address("T5", TagType{DataType::Float}, Series::XGK));
  EXPECT_FALSE(parse_address("D100.A", TagType{DataType::Word}, Series::XGK));
  EXPECT_FALSE(parse_address("MW100", TagType{}, Series::XGI));
  EXPECT_FALSE(parse_address("%MD10", TagType{DataType::Word}, Series::XGI));
  EXPECT_FALSE(parse_address("%IW0.1.0 [2]", TagType{}, Series::XGI));
  EXPECT_FALSE(parse_address("%MW100.16", TagType{}, Series::XGI));
}

TEST(XgtAddress, XgiDirectVariables) {
  auto w = i("%MW100");
  EXPECT_EQ(w->byte_offset, 200u);
  EXPECT_EQ(w->type.scalar, DataType::Word);
  auto f = i("%MD10", DataType::Float);
  EXPECT_EQ(f->byte_offset, 40u);
  EXPECT_EQ(f->byte_len, 4u);
  EXPECT_EQ(i("%ML1")->type.scalar, DataType::QWord);
  auto x = i("%MX5");
  EXPECT_EQ(x->type.scalar, DataType::Boolean);
  EXPECT_EQ(x->byte_offset, 0u);
  EXPECT_EQ(x->bit, 5);
  auto wb = i("%MW100.3");
  EXPECT_EQ(wb->var, "%MX1603");
  EXPECT_TRUE(wb->bit_write_direct);
  auto ix = i("%IX0.0.3");
  EXPECT_FALSE(ix->block);
  EXPECT_EQ(ix->var, "%IX0.0.3");
  EXPECT_EQ(i("%QW0.1.0")->type.scalar, DataType::Word);
  EXPECT_EQ(i("%MB10.8", DataType::String)->byte_len, 8u);
}

TEST(XgtFrame, ReadIndividualMatchesManualExample) {
  std::vector<std::string> vars{"%MW100"};
  auto body = read_individual(kTypeWord, vars);
  EXPECT_EQ(body, (std::vector<std::uint8_t>{0x54, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, '%', 'M', 'W', '1',
                                             '0', '0'}));
  Header h;
  h.cpu_info = 0xA4;
  h.invoke_id = 0x1234;
  auto f = frame(h, body);
  ASSERT_EQ(f.size(), kHeaderSize + body.size());
  EXPECT_EQ(std::string(f.begin(), f.begin() + 8), "LSIS-XGT");
  EXPECT_EQ(f[12], 0xA4);
  EXPECT_EQ(f[13], 0x33);
  EXPECT_EQ(f[14], 0x34);
  EXPECT_EQ(f[15], 0x12);
  EXPECT_EQ(f[16], body.size());
  std::uint8_t sum = 0;
  for (int b = 0; b < 19; ++b) sum = static_cast<std::uint8_t>(sum + f[b]);
  EXPECT_EQ(f[19], sum);
}

TEST(XgtFrame, ContinuousAndWriteBodies) {
  EXPECT_EQ(read_continuous("%MB0", 6),
            (std::vector<std::uint8_t>{0x54, 0, 0x14, 0, 0, 0, 1, 0, 4, 0, '%', 'M', 'B', '0', 6, 0}));
  std::vector<std::string> vars{"%MW100"};
  std::vector<std::vector<std::uint8_t>> data{{0x34, 0x12}};
  auto w = write_individual(kTypeWord, vars, data);
  EXPECT_EQ(std::vector<std::uint8_t>(w.end() - 4, w.end()), (std::vector<std::uint8_t>{2, 0, 0x34, 0x12}));
}

TEST(XgtFrame, ParseResponses) {
  std::vector<std::uint8_t> ack{0x55, 0, 2, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0x34, 0x12};
  auto r = parse_body(ack, true);
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->nak());
  ASSERT_EQ(r->data.size(), 1u);
  EXPECT_EQ(r->data[0], (std::vector<std::uint8_t>{0x34, 0x12}));
  std::vector<std::uint8_t> nak{0x55, 0, 2, 0, 0, 0, 0xFF, 0xFF, 0x21, 0};
  auto n = parse_body(nak, true);
  ASSERT_TRUE(n);
  EXPECT_TRUE(n->nak());
  EXPECT_EQ(n->error_code, 0x21);
  EXPECT_FALSE(parse_body(std::vector<std::uint8_t>{0x55, 0, 2, 0, 0, 0, 0, 0, 1, 0, 9, 0, 1}, true));
}

TEST(XgtValues, LittleEndianCodec) {
  auto d = k("D0", DataType::DWord);
  auto bytes = encode(*d, Value(std::uint32_t{0x11223344}));
  EXPECT_EQ(*bytes, (std::vector<std::uint8_t>{0x44, 0x33, 0x22, 0x11}));
  EXPECT_EQ(*decode(*d, *bytes)->get_if<std::uint32_t>(), 0x11223344u);
  auto f = k("D0", DataType::Float);
  EXPECT_FLOAT_EQ(*decode(*f, *encode(*f, Value(1.5)))->get_if<float>(), 1.5f);
  auto s = k("D0.5", DataType::String);
  auto sb = encode(*s, Value(std::string("ABC")));
  EXPECT_EQ(*sb, (std::vector<std::uint8_t>{'A', 'B', 'C', 0, 0, 0}));
  EXPECT_EQ(*decode(*s, *sb)->get_if<std::string>(), "ABC");
  auto bcd = k("D0", DataType::BCD);
  EXPECT_EQ(*decode(*bcd, std::vector<std::uint8_t>{0x34, 0x12})->get_if<std::uint16_t>(), 1234);
}
