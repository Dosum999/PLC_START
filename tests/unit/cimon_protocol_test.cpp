#include <gtest/gtest.h>

#include "cimon_protocol.h"

using namespace logict;
using namespace logict::cimon;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;
using Bytes = std::vector<std::uint8_t>;

namespace {
std::shared_ptr<CimonAddress> p(std::string_view s, DataType t = DataType::Default) {
  auto a = parse_address(s, TagType{t});
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
}  // namespace

TEST(CimonAddress, Devices) {
  auto m = p("M0104");
  EXPECT_TRUE(m->bit_units);
  EXPECT_EQ(m->start, 10u * 16 + 4);
  EXPECT_EQ(wire_address(*m->device, true, m->start), "M0000104");
  EXPECT_EQ(wire_address(*p("M000F")->device, true, p("M000F")->start), "M000000F");
  auto mw = p("M0011", DataType::Word);
  EXPECT_FALSE(mw->bit_units);
  EXPECT_EQ(mw->start, 11u);
  EXPECT_EQ(wire_address(*mw->device, false, mw->start), "M0000110");  // word access: bit digit 0
  auto d = p("D40");
  EXPECT_EQ(wire_address(*d->device, false, d->start), "D0000040");
  EXPECT_EQ(wire_address(*p("TC5")->device, false, 5), "TC000005");
  auto t = p("T5");
  EXPECT_TRUE(t->bit_units);
  EXPECT_EQ(wire_address(*t->device, true, t->start), "T0000005");
  EXPECT_EQ(p("D100", DataType::Float)->points, 2u);
  EXPECT_EQ(p("D100.15")->bit, 15);
  EXPECT_EQ(p("D10.5", DataType::String)->points, 3u);
  for (auto s : {"Q1", "D", "M1", "D1.16", "T5.1", "MXYZ"}) EXPECT_FALSE(parse_address(s, TagType{})) << s;
  EXPECT_FALSE(parse_address("D1", TagType{DataType::Boolean}));
  EXPECT_FALSE(parse_address("T1", TagType{DataType::Word}));
}

TEST(CimonSerial, ManualExamples) {
  // CIMON PLC manual, "Details of Command".
  EXPECT_EQ(text(serial_read(2, *find_device("D"), false, 1, 1)), "\x05" "02R0AD000000101B9\x04");
  EXPECT_EQ(text(serial_read(2, *find_device("D"), false, 40, 1)), "\x05" "02R0AD000004001BC\x04");
  EXPECT_EQ(text(serial_read(3, *find_device("M"), true, 10 * 16 + 15, 2)), "\x05" "03r0AM000010F02F9\x04");
  EXPECT_EQ(text(serial_read(3, *find_device("M"), true, 10 * 16 + 4, 2)), "\x05" "03r0AM000010402E7\x04");
  EXPECT_EQ(text(serial_write(1, *find_device("M"), true, 10 * 16 + 4, Bytes{1, 1, 0})), "\x05" "01w0DM000010403110" "82\x04");
  EXPECT_EQ(text(serial_write(2, *find_device("D"), false, 10, Bytes{0xFA, 0x34})), "\x05" "02W0ED000001001FA34B0\x04");

  auto r = parse_serial(bytes("\x02" "02R04F4ACB4\x03"), 2, 'R', false, 1);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->data, (Bytes{0xF4, 0xAC}));
  auto e = parse_serial(bytes("\x02" "02E020209\x03"), 2, 'R', false, 1);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->error, 2);
  auto b = parse_serial(bytes("\x02" "01r020135\x03"), 1, 'r', true, 2);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->data, (Bytes{0, 1}));
  EXPECT_TRUE(parse_serial(bytes("\x02" "02W00B7\x03"), 2, 'W', false, 0));
  EXPECT_FALSE(parse_serial(bytes("\x02" "02R04F4ACB5\x03"), 2, 'R', false, 1));  // BCC
  EXPECT_FALSE(parse_serial(bytes("\x02" "03R04F4ACB5\x03"), 2, 'R', false, 1));  // station
  EXPECT_EQ(serial_error_status(6), sdk::status::BadOutOfRange);
}

TEST(CimonEthernet, Frames) {
  auto f = enet_read(5, *find_device("Y"), false, 0, 128);
  ASSERT_EQ(f.size(), kEnetHeader + 10 + 2);
  EXPECT_EQ(text(Bytes(f.begin(), f.begin() + 9)), "KDT_PLC_M");
  EXPECT_EQ(f[9], 5);
  EXPECT_EQ(f[10], 0x52);
  EXPECT_EQ(f[12], 0x00);
  EXPECT_EQ(f[13], 10);
  EXPECT_EQ(text(Bytes(f.begin() + 14, f.begin() + 22)), "Y0000000");
  EXPECT_EQ(f[22], 0x00);
  EXPECT_EQ(f[23], 0x80);  // 128, as the manual's example
  unsigned sum = 0;
  for (std::size_t i = 0; i + 2 < f.size(); ++i) sum += f[i];
  EXPECT_EQ(f[f.size() - 2] << 8 | f.back(), static_cast<int>(sum & 0xFFFF));
  auto w = enet_write(6, *find_device("Y"), true, 13, Bytes{1, 0, 0, 1});
  EXPECT_EQ(text(Bytes(w.begin() + 14, w.end() - 2)), std::string("Y000000D") + std::string("\x00\x04", 2) + "1001");

  // Slave answer: KDT_PLC_S, frame + 128, data with the 10-byte block echo.
  Bytes resp(f.begin(), f.end() - 2);
  std::copy_n("KDT_PLC_S", 9, resp.begin());
  resp[9] = 5 + 128;
  resp[12] = 0;
  resp[13] = 10 + 4;
  resp.resize(14 + 10);
  std::copy(f.begin() + 14, f.begin() + 24, resp.begin() + 14);
  resp.insert(resp.end(), {0x12, 0x34, 0xAB, 0xCD});
  sum = 0;
  for (auto c : resp) sum += c;
  resp.push_back(static_cast<std::uint8_t>(sum >> 8));
  resp.push_back(static_cast<std::uint8_t>(sum));
  auto a = parse_enet(resp, 5, false, 2, false);
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_EQ(a->data, (Bytes{0x12, 0x34, 0xAB, 0xCD}));
  EXPECT_FALSE(parse_enet(resp, 6, false, 2, false));  // another frame number
  resp.back() ^= 1;
  EXPECT_FALSE(parse_enet(resp, 5, false, 2, false));  // checksum
}

TEST(CimonValues, LowWordFirst) {
  auto dw = p("D0", DataType::DWord);
  EXPECT_EQ(*encode(*dw, Value(std::uint32_t{0x12345678})), (Bytes{0x56, 0x78, 0x12, 0x34}));
  EXPECT_EQ(*decode(*dw, Bytes{0x56, 0x78, 0x12, 0x34})->get_if<std::uint32_t>(), 0x12345678u);
  EXPECT_EQ(*decode(*p("D0.4"), Bytes{0x00, 0x10})->get_if<bool>(), true);
}
