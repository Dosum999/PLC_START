#include <gtest/gtest.h>

#include "s7_protocol.h"

using namespace logict;
using namespace logict::s7;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;
using Bytes = std::vector<std::uint8_t>;

namespace {
std::shared_ptr<S7Address> p(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t});
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(S7Address, KepwareSyntax) {
  auto w = p("DB1.DBW10");
  EXPECT_EQ(w->area, DataBlock);
  EXPECT_EQ(w->db, 1);
  EXPECT_EQ(w->byte_offset, 10u);
  EXPECT_EQ(w->type.scalar, DataType::Word);
  EXPECT_EQ(p("db10.dbd4", DataType::Float)->byte_len, 4u);
  EXPECT_EQ(p("DB1.DBD0", DataType::Double)->byte_len, 8u);  // LREAL
  auto x = p("DB2.DBX3.7");
  EXPECT_EQ(x->bit, 7);
  EXPECT_EQ(x->type.scalar, DataType::Boolean);
  EXPECT_EQ(p("I0.1")->area, Inputs);
  EXPECT_EQ(p("E0.1")->area, Inputs);
  EXPECT_EQ(p("QB4")->area, Outputs);
  EXPECT_EQ(p("AW4")->area, Outputs);
  EXPECT_EQ(p("MD100", DataType::Long)->type.scalar, DataType::Long);
  auto s = p("DB1.STRING40.10");
  EXPECT_EQ(s->string_max, 10u);
  EXPECT_EQ(s->byte_len, 12u);
  EXPECT_EQ(p("DB1.DBW100 [5]", DataType::Short)->byte_len, 10u);
}

TEST(S7Address, Rejects) {
  for (auto s : {"DB0.DBW0", "DB1", "DB1.W0", "X0.0", "M10", "M10.8", "DB1.DBX1", "DB1.STRING4", "DB1.STRING4.300",
                 "M0.0 [8]", "DB1.DBW0.1"})
    EXPECT_FALSE(parse_address(s, TagType{})) << s;
  EXPECT_FALSE(parse_address("DB1.DBW0", TagType{DataType::Float}));
  EXPECT_FALSE(parse_address("MB0", TagType{DataType::Word}));
}

TEST(S7Frame, ConnectionRequestMatchesSnap7) {
  // snap7 ISO connection request for rack 0, slot 2, PG connection.
  EXPECT_EQ(connection_request(0x0100, 0x0102),
            (Bytes{0x03, 0x00, 0x00, 0x16, 0x11, 0xE0, 0x00, 0x00, 0x00, 0x01, 0x00, 0xC0, 0x01, 0x0A, 0xC1, 0x02, 0x01,
                   0x00, 0xC2, 0x02, 0x01, 0x02}));
}

TEST(S7Frame, SetupCommunicationMatchesSnap7) {
  EXPECT_EQ(data_frame(setup_communication(0x0400, 480)),
            (Bytes{0x03, 0x00, 0x00, 0x19, 0x02, 0xF0, 0x80, 0x32, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x08, 0x00, 0x00,
                   0xF0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0}));
}

TEST(S7Frame, ReadAndWriteVar) {
  std::vector<ReadItem> items{{DataBlock, 1, 10, 2}, {Flags, 0, 4, 1}};
  auto r = read_var(7, items);
  EXPECT_EQ(Bytes(r.begin() + 10, r.end()),
            (Bytes{0x04, 0x02, 0x12, 0x0A, 0x10, 0x02, 0x00, 0x02, 0x00, 0x01, 0x84, 0x00, 0x00, 0x50,  // DB1, byte 10
                   0x12, 0x0A, 0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x83, 0x00, 0x00, 0x20}));                // MB4
  EXPECT_EQ(r.size(), request_size(2));

  Bytes data{0x12, 0x34};
  auto w = write_var(8, DataBlock, 1, 10, -1, data);
  EXPECT_EQ(Bytes(w.end() - 6, w.end()), (Bytes{0x00, 0x04, 0x00, 0x10, 0x12, 0x34}));
  auto b = write_var(9, Flags, 0, 3, 5, Bytes{1});
  EXPECT_EQ(b[10 + 2 + 3], 0x01);  // transport BIT
  EXPECT_EQ(Bytes(b.end() - 8, b.end()), (Bytes{0x00, 0x00, 0x1D, 0x00, 0x03, 0x00, 0x01, 0x01}));  // bit address 3*8+5
}

TEST(S7Frame, ParsesMultiItemReadResponse) {
  // ack-data: ref 7, param 2 bytes, data: item1 (3 bytes + fill), item2 error 0x0A, item3 1 byte.
  Bytes data{0xFF, 0x04, 0x00, 0x18, 1, 2, 3, 0x00, 0x0A, 0x00, 0x00, 0x00, 0xFF, 0x04, 0x00, 0x08, 9};
  Bytes pdu{0x32, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x02, 0x00, static_cast<std::uint8_t>(data.size()), 0x00, 0x00, 0x04, 0x03};
  pdu.insert(pdu.end(), data.begin(), data.end());
  auto ack = parse_ack(pdu);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->pdu_ref, 7);
  auto items = parse_read_data(ack->data, 3);
  ASSERT_TRUE(items) << items.error().message;
  EXPECT_EQ((*items)[0].bytes, (Bytes{1, 2, 3}));
  EXPECT_EQ((*items)[1].code, 0x0A);
  EXPECT_EQ((*items)[2].bytes, (Bytes{9}));
  EXPECT_EQ(item_status(0x0A), sdk::status::BadOutOfRange);
}

TEST(S7Values, BigEndianAndStrings) {
  auto f = p("DB1.DBD0", DataType::Float);
  EXPECT_EQ(*encode(*f, Value(1.5)), (Bytes{0x3F, 0xC0, 0x00, 0x00}));
  auto s = p("DB1.STRING0.6");
  auto sb = encode(*s, Value(std::string("abc")));
  EXPECT_EQ(*sb, (Bytes{6, 3, 'a', 'b', 'c', 0, 0, 0}));
  EXPECT_EQ(*decode(*s, *sb)->get_if<std::string>(), "abc");
  EXPECT_FALSE(encode(*s, Value(std::string("toolong"))));
  auto bit = p("M0.3");
  EXPECT_EQ(*decode(*bit, Bytes{0x08})->get_if<bool>(), true);
}
