#include <gtest/gtest.h>

#include "fins_protocol.h"

using namespace logict;
using namespace logict::fins;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;
using sdk::codec::ByteOrder;
using Bytes = std::vector<std::uint8_t>;

namespace {
std::shared_ptr<FinsAddress> p(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t});
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(FinsFrame, MemoryAreaReadMatchesManualLayout) {
  // W227 5-3-2 MEMORY AREA READ: DM 100, 10 words, to node 1 from node 10.
  Header h;
  h.da1 = 0x01;
  h.sa1 = 0x0A;
  EXPECT_EQ(memory_read(h, 0x05, 0x82, 100, 0, 10),
            (Bytes{0x80, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x05, 0x01, 0x01, 0x82, 0x00, 0x64, 0x00, 0x00, 0x0A}));
  auto w = memory_write(h, 0x06, 0x30, 100, 5, 2, Bytes{1, 0});
  EXPECT_EQ(Bytes(w.begin() + 10, w.end()), (Bytes{0x01, 0x02, 0x30, 0x00, 0x64, 0x05, 0x00, 0x02, 0x01, 0x00}));
}

TEST(FinsFrame, ParsesResponses) {
  Bytes ok{0xC0, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x00, 0x01, 0x00, 0x05, 0x01, 0x01, 0x00, 0x00, 0x12, 0x34};
  auto r = parse_response(ok, 0x01, 0x01);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->sid, 0x05);
  EXPECT_EQ(r->end_code, 0);
  EXPECT_EQ(r->data, (Bytes{0x12, 0x34}));
  Bytes err = ok;
  err.resize(14);
  err[12] = 0x11;
  err[13] = 0x43;  // address range error + non-fatal CPU error flag
  EXPECT_EQ(parse_response(err, 0x01, 0x01)->end_code, 0x1103);
  EXPECT_EQ(end_code_status(0x1103), sdk::status::BadOutOfRange);
  EXPECT_EQ(end_code_status(0x2101), sdk::status::BadNotWritable);
  EXPECT_FALSE(parse_response(ok, 0x01, 0x02));  // answer to another command
  ok[0] = 0x80;
  EXPECT_FALSE(parse_response(ok, 0x01, 0x01));  // a command, not a response
}

TEST(FinsFrame, Tcp) {
  EXPECT_EQ(tcp_node_request(0), (Bytes{'F', 'I', 'N', 'S', 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  auto f = tcp_frame(Bytes{0xAA, 0xBB});
  EXPECT_EQ(f, (Bytes{'F', 'I', 'N', 'S', 0, 0, 0, 10, 0, 0, 0, 2, 0, 0, 0, 0, 0xAA, 0xBB}));
  auto h = parse_tcp_header(f);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->length, 10u);
  EXPECT_EQ(h->command, kTcpFrame);
  f[0] = 'X';
  EXPECT_FALSE(parse_tcp_header(f));
}

TEST(FinsAddress, Areas) {
  auto d = p("D100");
  EXPECT_EQ(d->code, 0x82);
  EXPECT_EQ(d->index, 100u);
  EXPECT_EQ(d->points, 1u);
  EXPECT_EQ(d->type.scalar, DataType::Word);
  EXPECT_EQ(p("CIO 10")->code, 0xB0);
  EXPECT_EQ(p("A10")->code, 0xB3);
  EXPECT_EQ(p("H5")->code, 0xB2);
  EXPECT_EQ(p("W3", DataType::Float)->points, 2u);
  EXPECT_EQ(p("E0:100")->code, 0xA0);
  EXPECT_EQ(p("EC:5")->code, 0xAC);
  EXPECT_EQ(p("E10:5")->code, 0x60);
  EXPECT_EQ(p("E10:5.3")->code, 0xE0);
  auto bit = p("CIO100.05");
  EXPECT_TRUE(bit->bit_units);
  EXPECT_EQ(bit->code, 0x30);
  EXPECT_EQ(bit->index, 100u * 16 + 5);
  EXPECT_EQ(word_of(bit->code, bit->index, true), 100);
  EXPECT_EQ(bit_of(bit->code, bit->index, true), 5);
  EXPECT_EQ(p("D100.15")->code, 0x02);
  auto t = p("T5");
  EXPECT_EQ(t->code, 0x89);
  EXPECT_EQ(t->index, 5u);
  auto cf = p("C5", DataType::Boolean);
  EXPECT_EQ(cf->code, 0x09);
  EXPECT_EQ(cf->index, 0x8005u);
  EXPECT_EQ(word_of(cf->code, cf->index, true), 0x8005);
  EXPECT_EQ(bit_of(cf->code, cf->index, true), 0);
  EXPECT_EQ(p("D110.5", DataType::String)->points, 3u);
  EXPECT_EQ(p("D0 [4]", DataType::Double)->points, 16u);
  EXPECT_EQ(p("CIO0.00 [16]", DataType::Boolean)->points, 16u);
}

TEST(FinsAddress, Rejects) {
  for (auto s : {"X100", "D", "D100.16", "D100.1.2", "CIO6144", "W512", "E19:0", "T4096", "D32767 [2]", "T5.1"})
    EXPECT_FALSE(parse_address(s, TagType{})) << s;
  EXPECT_FALSE(parse_address("D100", TagType{DataType::Boolean}));
  EXPECT_FALSE(parse_address("D100", TagType{DataType::Byte}));
  EXPECT_FALSE(parse_address("T5", TagType{DataType::DWord}));
  EXPECT_FALSE(parse_address("D100.3", TagType{DataType::Word}));
}

TEST(FinsValues, WordOrder) {
  auto dw = p("D0", DataType::DWord);
  EXPECT_EQ(*encode(*dw, Value(std::uint32_t{0x12345678}), ByteOrder::CDAB), (Bytes{0x56, 0x78, 0x12, 0x34}));
  EXPECT_EQ(*encode(*dw, Value(std::uint32_t{0x12345678}), ByteOrder::ABCD), (Bytes{0x12, 0x34, 0x56, 0x78}));
  EXPECT_EQ(*decode(*dw, Bytes{0x56, 0x78, 0x12, 0x34}, ByteOrder::CDAB)->get_if<std::uint32_t>(), 0x12345678u);
  auto w = p("D0");
  EXPECT_EQ(*encode(*w, Value(std::uint16_t{0x1234}), ByteOrder::CDAB), (Bytes{0x12, 0x34}));
  auto s = p("D0.3", DataType::String);
  auto sb = encode(*s, Value(std::string("ABC")), ByteOrder::CDAB);
  EXPECT_EQ(*sb, (Bytes{'A', 'B', 'C', 0}));  // high byte first
  EXPECT_EQ(*decode(*s, *sb, ByteOrder::CDAB)->get_if<std::string>(), "ABC");
  auto bits = p("CIO0.00 [3]", DataType::Boolean);
  EXPECT_EQ(*encode(*bits, Value(sdk::Array{Value(true), Value(false), Value(true)}), ByteOrder::CDAB), (Bytes{1, 0, 1}));
  auto bcd = p("T0", DataType::BCD);
  EXPECT_EQ(*decode(*bcd, Bytes{0x12, 0x34}, ByteOrder::CDAB)->get_if<std::uint16_t>(), 1234);
}
