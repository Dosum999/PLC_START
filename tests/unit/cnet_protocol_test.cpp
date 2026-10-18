#include <gtest/gtest.h>

#include "cnet_protocol.h"

using namespace logict::xgt::cnet;
using Bytes = std::vector<std::uint8_t>;

namespace {
Bytes frame(std::initializer_list<std::uint8_t> head, std::string_view text, std::initializer_list<std::uint8_t> tail) {
  Bytes b(head);
  b.insert(b.end(), text.begin(), text.end());
  b.insert(b.end(), tail);
  return b;
}
}  // namespace

TEST(Cnet, IndividualReadMatchesManualBccExample) {
  // Manual 7.2.1: ENQ 20 r SS 01 06 %MW100 EOT, sum 0x03A4 -> BCC "A4".
  std::vector<std::string> vars{"%MW100"};
  EXPECT_EQ(read_individual(0x20, true, vars), frame({ENQ}, "20rSS0106%MW100", {EOT, 'A', '4'}));
  EXPECT_EQ(read_individual(0x20, false, vars), frame({ENQ}, "20RSS0106%MW100", {EOT}));
}

TEST(Cnet, ContinuousReadAndWritesMatchManual) {
  // Manual 7.2.2: ENQ 10 R SB 06 %MW100 05 EOT.
  EXPECT_EQ(read_continuous(0x10, false, "%MW100", 5), frame({ENQ}, "10RSB06%MW10005", {EOT}));
  // Manual 7.2.3: write HFF to M230 of station 1.
  std::vector<std::string> vars{"%MW230"}, data{"00FF"};
  EXPECT_EQ(write_individual(0x01, false, vars, data), frame({ENQ}, "01WSS0106%MW23000FF", {EOT}));
  // Manual 7.2.4: ENQ 10 W SB 06 %MW100 02 11112222 EOT.
  EXPECT_EQ(write_continuous(0x10, false, "%MW100", 2, "11112222"), frame({ENQ}, "10WSB06%MW1000211112222", {EOT}));
}

TEST(Cnet, ParsesManualResponses) {
  // Manual 7.2.1 ACK: ACK 20 R SS 01 02 A9F3 ETX.
  auto ack = parse(frame({ACK}, "20RSS0102A9F3", {ETX}), 0x20, false, true);
  ASSERT_TRUE(ack) << ack.error().message;
  EXPECT_TRUE(ack->ack);
  ASSERT_EQ(ack->blocks.size(), 1u);
  EXPECT_EQ(ack->blocks[0], (Bytes{0xA9, 0xF3}));
  // Manual 7.2.2 example: two words 1234 5678 in one block.
  auto sb = parse(frame({ACK}, "0ARSB010412345678", {ETX}), 0x0A, false, true);
  ASSERT_TRUE(sb);
  EXPECT_EQ(sb->blocks[0], (Bytes{0x12, 0x34, 0x56, 0x78}));
  // NAK with error 1132.
  auto nak = parse(frame({NAK}, "20RSS1132", {ETX}), 0x20, false, true);
  ASSERT_TRUE(nak);
  EXPECT_FALSE(nak->ack);
  EXPECT_EQ(nak->error, 0x1132);
}

TEST(Cnet, ChecksBccStationAndFraming) {
  Bytes good = frame({ACK}, "20rSS0102A9F3", {ETX});
  const std::uint8_t sum = bcc_of(good);
  Bytes with_bcc = good;
  const std::string hex = to_hex(Bytes{sum});
  with_bcc.insert(with_bcc.end(), hex.begin(), hex.end());
  EXPECT_TRUE(parse(with_bcc, 0x20, true, true));
  with_bcc.back() = with_bcc.back() == '0' ? '1' : '0';
  EXPECT_FALSE(parse(with_bcc, 0x20, true, true));
  EXPECT_FALSE(parse(good, 0x21, false, true));                                  // wrong station
  EXPECT_FALSE(parse(frame({ACK}, "20RSS0102A9", {ETX}), 0x20, false, true));   // short data
  EXPECT_TRUE(parse(frame({ACK}, "20WSS", {ETX}), 0x20, false, false)->ack);     // write ACK
}
