#include <gtest/gtest.h>

#include <array>

#include "logict/sdk/codec.h"

using namespace logict::sdk::codec;

TEST(Codec, Load32AllOrders) {
  // 0x11223344 on the wire in each order.
  std::array<std::uint8_t, 4> abcd{0x11, 0x22, 0x33, 0x44};
  std::array<std::uint8_t, 4> cdab{0x33, 0x44, 0x11, 0x22};
  std::array<std::uint8_t, 4> badc{0x22, 0x11, 0x44, 0x33};
  std::array<std::uint8_t, 4> dcba{0x44, 0x33, 0x22, 0x11};
  EXPECT_EQ(load<std::uint32_t>(abcd, ByteOrder::ABCD), 0x11223344u);
  EXPECT_EQ(load<std::uint32_t>(cdab, ByteOrder::CDAB), 0x11223344u);
  EXPECT_EQ(load<std::uint32_t>(badc, ByteOrder::BADC), 0x11223344u);
  EXPECT_EQ(load<std::uint32_t>(dcba, ByteOrder::DCBA), 0x11223344u);
}

TEST(Codec, Store64WordSwapRoundTrip) {
  std::array<std::uint8_t, 8> buf{};
  for (auto order : {ByteOrder::ABCD, ByteOrder::CDAB, ByteOrder::BADC, ByteOrder::DCBA}) {
    store<std::int64_t>(-1234567890123LL, buf, order);
    EXPECT_EQ(load<std::int64_t>(buf, order), -1234567890123LL);
  }
  store<std::uint64_t>(0x0102030405060708ULL, buf, ByteOrder::CDAB);
  EXPECT_EQ(buf, (std::array<std::uint8_t, 8>{0x07, 0x08, 0x05, 0x06, 0x03, 0x04, 0x01, 0x02}));
}

TEST(Codec, FloatBigEndian) {
  std::array<std::uint8_t, 4> buf{0x3F, 0xC0, 0x00, 0x00};  // 1.5f
  EXPECT_FLOAT_EQ(load<float>(buf, ByteOrder::ABCD), 1.5f);
}

TEST(Codec, Bcd) {
  EXPECT_EQ(bcd_decode(0x1234, 4), 1234u);
  EXPECT_FALSE(bcd_decode(0x12A4, 4));
  EXPECT_EQ(bcd_encode(9876, 4), 0x9876u);
  EXPECT_FALSE(bcd_encode(12345, 4));
  EXPECT_EQ(bcd_encode(12345678, 8), 0x12345678u);
}
