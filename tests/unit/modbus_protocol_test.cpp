#include <gtest/gtest.h>

#include "protocol.h"

using namespace logict;
using namespace logict::modbus;
using sdk::DataType;
using sdk::TagType;
using sdk::Value;

namespace {
std::shared_ptr<ModbusAddress> parse(std::string_view s, DataType t = DataType::Default, AddressOptions o = {}) {
  auto a = parse_address(s, TagType{t}, o);
  EXPECT_TRUE(a) << s << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}
}  // namespace

TEST(ModbusAddress, AreasAndDigits) {
  auto a = parse("40001");
  EXPECT_EQ(a->area, Area::HoldingRegister);
  EXPECT_EQ(a->offset, 0u);
  EXPECT_EQ(a->type.scalar, DataType::Word);
  EXPECT_EQ(parse("400100")->offset, 99u);
  EXPECT_EQ(parse("465536")->offset, 65535u);
  EXPECT_EQ(parse("000001")->area, Area::Coil);
  EXPECT_EQ(parse("100010")->area, Area::DiscreteInput);
  EXPECT_EQ(parse("30005")->area, Area::InputRegister);
  EXPECT_EQ(parse("40001", DataType::Default, {.zero_based = false})->offset, 1u);
}

TEST(ModbusAddress, Rejects) {
  for (auto s : {"20001", "4001", "4000001", "40000", "465537", "000001.3", "40001.16", "abcde"})
    EXPECT_FALSE(parse_address(s, TagType{}, {})) << s;
  EXPECT_FALSE(parse_address("000001", TagType{DataType::Word}, {}));
  EXPECT_FALSE(parse_address("40001", TagType{DataType::Char}, {}));
}

TEST(ModbusAddress, BitsStringsArrays) {
  auto bit = parse("40010.15");
  EXPECT_EQ(bit->bit, 15);
  EXPECT_EQ(bit->type.scalar, DataType::Boolean);
  EXPECT_EQ(parse("40010.16", DataType::Default, {.zero_based_bits = false})->bit, 15);

  auto str = parse("40020.10H");
  EXPECT_EQ(str->type.scalar, DataType::String);
  EXPECT_EQ(str->string_len, 10u);
  EXPECT_EQ(str->units(), 5u);
  EXPECT_FALSE(parse("40020.5L")->string_high_first);
  EXPECT_EQ(parse("40020.7", DataType::String)->units(), 4u);

  auto arr = parse("40100 [10]", DataType::Float);
  EXPECT_EQ(arr->type.rows, 10u);
  EXPECT_EQ(arr->units(), 20u);
  auto arr2 = parse("40100 [2][3]", DataType::Double);
  EXPECT_EQ(arr2->units(), 24u);
  EXPECT_EQ(parse("000001 [16]")->units(), 16u);
}

TEST(ModbusEncoding, Word32Orders) {
  auto a = parse("40001", DataType::DWord);
  const std::vector<std::uint8_t> abcd{0x11, 0x22, 0x33, 0x44};
  const std::vector<std::uint8_t> cdab{0x33, 0x44, 0x11, 0x22};
  const std::vector<std::uint8_t> badc{0x22, 0x11, 0x44, 0x33};
  const std::vector<std::uint8_t> dcba{0x44, 0x33, 0x22, 0x11};
  auto check = [&](const std::vector<std::uint8_t>& wire, Encoding e) {
    auto v = decode_registers(*a, wire, e);
    ASSERT_TRUE(v);
    EXPECT_EQ(*v->get_if<std::uint32_t>(), 0x11223344u);
    auto back = encode_registers(*a, *v, e);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, wire);
  };
  check(abcd, {true, false, false});
  check(cdab, {true, true, false});  // Kepware default
  check(badc, {false, false, false});
  check(dcba, {false, true, false});
}

TEST(ModbusEncoding, Word64Orders) {
  auto a = parse("40001", DataType::QWord);
  const std::uint64_t v = 0x1122334455667788ULL;
  // Kepware default (first word low + first dword low) = all words reversed.
  auto wire = encode_registers(*a, Value(v), {});
  ASSERT_TRUE(wire);
  EXPECT_EQ(*wire, (std::vector<std::uint8_t>{0x77, 0x88, 0x55, 0x66, 0x33, 0x44, 0x11, 0x22}));
  auto hi_dword_first = encode_registers(*a, Value(v), {true, true, false});
  EXPECT_EQ(*hi_dword_first, (std::vector<std::uint8_t>{0x33, 0x44, 0x11, 0x22, 0x77, 0x88, 0x55, 0x66}));
  auto back = decode_registers(*a, *hi_dword_first, {true, true, false});
  EXPECT_EQ(*back->get_if<std::uint64_t>(), v);
}

TEST(ModbusEncoding, FloatBcdBitString) {
  auto f = parse("40001", DataType::Float);
  auto wire = encode_registers(*f, Value(1.5), {true, false, false});
  EXPECT_EQ(*wire, (std::vector<std::uint8_t>{0x3F, 0xC0, 0x00, 0x00}));

  auto bcd = parse("40001", DataType::BCD);
  EXPECT_EQ(*decode_registers(*bcd, std::vector<std::uint8_t>{0x12, 0x34}, {})->get_if<std::uint16_t>(), 1234);
  EXPECT_FALSE(decode_registers(*bcd, std::vector<std::uint8_t>{0x12, 0x3A}, {}));

  auto bit = parse("40001.9");
  EXPECT_EQ(*decode_registers(*bit, std::vector<std::uint8_t>{0x02, 0x00}, {})->get_if<bool>(), true);
  EXPECT_EQ(*decode_registers(*bit, std::vector<std::uint8_t>{0x00, 0x02}, {false, true, true})->get_if<bool>(), true);

  auto s = parse("40001.5H");
  auto sw = encode_registers(*s, Value(std::string("ABC")), {});
  EXPECT_EQ(*sw, (std::vector<std::uint8_t>{'A', 'B', 'C', 0, 0, 0}));
  EXPECT_EQ(*decode_registers(*s, *sw, {})->get_if<std::string>(), "ABC");
  auto sl = parse("40001.4L");
  auto lw = encode_registers(*sl, Value(std::string("ABCD")), {});
  EXPECT_EQ(*lw, (std::vector<std::uint8_t>{'B', 'A', 'D', 'C'}));
  EXPECT_FALSE(encode_registers(*sl, Value(std::string("ABCDE")), {}));
}

TEST(ModbusEncoding, ArraysAndBits) {
  auto a = parse("40001 [3]", DataType::Short);
  auto wire = encode_registers(*a, Value(sdk::Array{Value(1), Value(-1), Value(300)}), {});
  ASSERT_TRUE(wire);
  EXPECT_EQ(*wire, (std::vector<std::uint8_t>{0, 1, 0xFF, 0xFF, 0x01, 0x2C}));
  EXPECT_EQ(sdk::to_string(*decode_registers(*a, *wire, {})), "[1,-1,300]");

  auto coils = parse("000005 [4]");
  std::vector<std::uint8_t> packed{0b1010'0000, 0b0000'0001};  // bits 5,7,8 set
  EXPECT_EQ(sdk::to_string(decode_bits(*coils, packed, 5)), "[true,false,true,true]");
}

TEST(ModbusRtu, Crc16MatchesSpecExample) {
  // Modbus over serial line spec: read 10 holding registers from station 1 -> CRC 0xCDC5 (sent C5 CD).
  const std::vector<std::uint8_t> frame{0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  EXPECT_EQ(crc16(frame), 0xCDC5);
  EXPECT_EQ(rtu_remaining(0x03, 4), 1u + 4 + 2);
  EXPECT_EQ(rtu_remaining(0x83, 2), 3u);
  EXPECT_EQ(rtu_remaining(0x10, 0), 6u);
  EXPECT_EQ(rtu_remaining(0x2B, 0), 0u);
}
