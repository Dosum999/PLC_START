#include <gtest/gtest.h>

#include "mc_protocol.h"

using namespace logict;
using namespace logict::mc;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;
using Bytes = std::vector<std::uint8_t>;

namespace {
std::shared_ptr<McAddress> p(std::string_view s, DataType t = DataType::Default, AddressOptions o = {}) {
  auto a = parse_address(s, TagType{t}, o);
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(McFrame, BatchReadMatchesManualExample) {
  // MC protocol manual: batch read D100-D102 in word units, 3E binary,
  // monitoring timer 0x0010.
  auto f = batch_read(Route{}, 0x0010, *find_device("D"), 100, 3, false);
  EXPECT_EQ(f, (Bytes{0x50, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x0C, 0x00, 0x10, 0x00, 0x01, 0x04, 0x00, 0x00, 0x64,
                      0x00, 0x00, 0xA8, 0x03, 0x00}));
}

TEST(McFrame, BitUnitsAndWrites) {
  Bytes pts{1, 0, 1, 1, 0};
  EXPECT_EQ(pack_bits(pts), (Bytes{0x10, 0x11, 0x00}));
  Bytes packed{0x10, 0x11};
  EXPECT_TRUE(unpack_bit(packed, 0));
  EXPECT_FALSE(unpack_bit(packed, 1));
  EXPECT_TRUE(unpack_bit(packed, 3));
  auto w = batch_write(Route{}, 4, *find_device("M"), 100, 2, true, Bytes{0x11});
  EXPECT_EQ(Bytes(w.begin() + 11, w.end()), (Bytes{0x01, 0x14, 0x01, 0x00, 0x64, 0x00, 0x00, 0x90, 0x02, 0x00, 0x11}));
}

TEST(McFrame, ParsesResponses) {
  Bytes hdr{0xD0, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x04, 0x00};
  ASSERT_EQ(*response_length(hdr), 4);
  auto r = parse_response(Bytes{0x00, 0x00, 0x34, 0x12});
  EXPECT_EQ(r->end_code, 0);
  EXPECT_EQ(r->data, (Bytes{0x34, 0x12}));
  EXPECT_EQ(parse_response(Bytes{0x56, 0xC0})->end_code, 0xC056);
  EXPECT_EQ(end_code_status(0xC056), sdk::status::BadOutOfRange);
  EXPECT_FALSE(response_length(Bytes{0x50, 0x00, 0, 0, 0, 0, 0, 2, 0}));
}

TEST(McAddress, Devices) {
  auto d = p("D100");
  EXPECT_EQ(d->device->code, 0xA8);
  EXPECT_EQ(d->number, 100u);
  EXPECT_EQ(d->points, 1u);
  EXPECT_EQ(p("D100", DataType::Float)->points, 2u);
  EXPECT_EQ(p("D100 [10]", DataType::Double)->points, 40u);
  EXPECT_EQ(p("W1F")->number, 0x1Fu);           // hex device
  EXPECT_EQ(p("X1F")->number, 0x1Fu);
  EXPECT_EQ(p("X17", DataType::Default, {.xy_octal = true})->number, 15u);  // iQ-F octal
  EXPECT_EQ(p("SM400")->device->code, 0x91);   // two-letter devices win over one letter
  EXPECT_EQ(p("TN5")->device->code, 0xC2);
  EXPECT_EQ(p("ZR100")->device->code, 0xB0);
  auto m = p("M10");
  EXPECT_TRUE(m->bit_units);
  EXPECT_EQ(m->type.scalar, DataType::Boolean);
  auto mw = p("M16", DataType::Word);
  EXPECT_FALSE(mw->bit_units);
  EXPECT_EQ(mw->points, 1u);
  auto bit = p("D100.F");
  EXPECT_EQ(bit->bit, 15);
  auto s = p("D110.5", DataType::String);
  EXPECT_EQ(s->points, 3u);
}

TEST(McAddress, Rejects) {
  for (auto s : {"Q100", "D", "DZZ", "D100.G", "M10.1", "X1G", "D100 [961]"})
    EXPECT_FALSE(parse_address(s, TagType{}, {})) << s;
  EXPECT_FALSE(parse_address("M10", TagType{DataType::Word}, {}));    // word access needs a multiple of 16
  EXPECT_FALSE(parse_address("D100", TagType{DataType::Boolean}, {}));
  EXPECT_FALSE(parse_address("D100", TagType{DataType::Byte}, {}));
  EXPECT_FALSE(parse_address("X8", TagType{}, {.xy_octal = true}));
}

TEST(McValues, LittleEndianWordsAndStrings) {
  auto dw = p("D0", DataType::DWord);
  EXPECT_EQ(*encode(*dw, Value(std::uint32_t{0x12345678})), (Bytes{0x78, 0x56, 0x34, 0x12}));  // low word first
  auto s = p("D0.3", DataType::String);
  auto sb = encode(*s, Value(std::string("ABC")));
  EXPECT_EQ(*sb, (Bytes{'A', 'B', 'C', 0}));
  EXPECT_EQ(*decode(*s, *sb)->get_if<std::string>(), "ABC");
  auto b = p("D0.9");
  EXPECT_EQ(*decode(*b, Bytes{0x00, 0x02})->get_if<bool>(), true);
}

namespace {
std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
}  // namespace

TEST(McAscii, BatchReadMatchesManualExample) {
  // MC protocol manual: batch read D100-D102 in word units, 3E ASCII.
  EXPECT_EQ(text(batch_read_ascii(Route{}, 0x0010, *find_device("D"), 10, 100, 3, false)),
            "500000FF03FF000018001004010000D*0001000003");
  // Hex devices keep hex numbering, iQ-F X/Y octal, bit units sub-command 0001.
  EXPECT_EQ(text(batch_read_ascii(Route{}, 4, *find_device("X"), 16, 0x1F, 2, true)).substr(30), "X*00001F0002");
  EXPECT_EQ(text(batch_read_ascii(Route{}, 4, *find_device("X"), 8, 15, 2, true)).substr(30), "X*0000170002");
  EXPECT_EQ(text(batch_read_ascii(Route{}, 4, *find_device("SM"), 10, 400, 1, true)).substr(22, 14), "04010001SM0004");
}

TEST(McAscii, WritesAndResponses) {
  auto w = batch_write_ascii(Route{}, 4, *find_device("D"), 10, 0, 2, false, Bytes{0x34, 0x12, 0xFF, 0xFF});
  EXPECT_EQ(text(w).substr(14, 4), "0020");  // 24 + 8 data characters
  EXPECT_EQ(text(w).substr(42), "1234FFFF");
  auto wb = batch_write_ascii(Route{}, 4, *find_device("M"), 10, 0, 3, true, pack_bits(Bytes{1, 0, 1}));
  EXPECT_EQ(text(wb).substr(42), "101");
  EXPECT_EQ(*response_length_ascii(bytes("D00000FF03FF00000C")), 0x0C);
  EXPECT_FALSE(response_length_ascii(bytes("D00000FF03FF00")));
  EXPECT_FALSE(response_length_ascii(bytes("500000FF03FF00000C")));
  auto r = parse_response_ascii(bytes("000012340001"), false, 2);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->data, (Bytes{0x34, 0x12, 0x01, 0x00}));
  auto rb = parse_response_ascii(bytes("0000101"), true, 3);
  ASSERT_TRUE(rb);
  EXPECT_TRUE(unpack_bit(rb->data, 0));
  EXPECT_FALSE(unpack_bit(rb->data, 1));
  EXPECT_TRUE(unpack_bit(rb->data, 2));
  EXPECT_EQ(parse_response_ascii(bytes("C056"), false, 1)->end_code, 0xC056);
  EXPECT_FALSE(parse_response_ascii(bytes("0000123"), false, 1));
}

TEST(Mc1E, Frames) {
  // A-compatible 1E: word batch read D100, 3 points, PC FF, timer 0x000A.
  EXPECT_EQ(batch_read_1e(0xFF, 0x0A, device_code_1e(*find_device("D")), 100, 3, false),
            (Bytes{0x01, 0xFF, 0x0A, 0x00, 0x64, 0x00, 0x00, 0x00, 0x20, 0x44, 0x03, 0x00}));
  EXPECT_EQ(batch_read_1e(0xFF, 0x0A, device_code_1e(*find_device("X")), 8, 256, true)[10], 0x00);  // 256 points -> 0
  auto w = batch_write_1e(0xFF, 4, device_code_1e(*find_device("M")), 10, 3, true, pack_bits(Bytes{1, 0, 1}));
  EXPECT_EQ(Bytes(w.begin(), w.begin() + 2), (Bytes{0x02, 0xFF}));
  EXPECT_EQ(Bytes(w.begin() + 8, w.end()), (Bytes{0x20, 0x4D, 0x03, 0x00, 0x10, 0x10}));
  EXPECT_EQ(device_code_1e(*find_device("ZR")), 0);
  EXPECT_EQ(end_code_status_1e(0x56, 0), sdk::status::BadOutOfRange);
  EXPECT_EQ(end_code_status_1e(0x55, 0), sdk::status::BadNotWritable);
}
