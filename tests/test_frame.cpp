#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cansim/frame.hpp"

using namespace cansim;

namespace {

BitString bytes_to_bits(const std::string& bytes) {
  BitString out;
  for (unsigned char b : bytes) {
    for (int i = 7; i >= 0; --i) out.push_back(((b >> i) & 1u) ? '1' : '0');
  }
  return out;
}

bool has_run_of_six(const BitString& bits) {
  int run = 0;
  char last = 0;
  for (char b : bits) {
    run = (b == last) ? run + 1 : 1;
    last = b;
    if (run >= 6) return true;
  }
  return false;
}

}  // namespace

// Эталон CRC-15/CAN из каталога CRC (poly 0x4599, init 0): check("123456789") = 0x059E.
TEST(Crc15, MatchesCatalogueCheckValue) {
  EXPECT_EQ(crc15(bytes_to_bits("123456789")), 0x059E);
}

TEST(Crc15, EmptyInputIsZero) {
  EXPECT_EQ(crc15(""), 0);
}

// Значение посчитано независимым делением многочленов (SOF..DATA кадра 0x123, DLC=1, 0xAB).
TEST(Crc15, StandardFrameLayout) {
  CanFrame f;
  f.id = 0x123;
  f.dlc = 1;
  f.data = {0xAB};
  const auto lf = layout_frame(f);
  EXPECT_EQ(lf.unstuffed.size(), 27u + 15u);
  EXPECT_EQ(lf.crc, 0x666F);
}

TEST(Crc15, MessageWithAppendedCrcLeavesZeroRemainder) {
  CanFrame f;
  f.id = 0x7FF;
  f.dlc = 8;
  f.data = {1, 2, 3, 4, 5, 6, 7, 8};
  const auto lf = layout_frame(f);
  EXPECT_EQ(crc15(lf.unstuffed), 0);
}

TEST(BitStuffing, NoStuffingForShortRuns) {
  std::vector<size_t> pos;
  EXPECT_EQ(stuff_bits("0000111101", &pos), "0000111101");
  EXPECT_TRUE(pos.empty());
}

TEST(BitStuffing, InsertsOppositeBitAfterFiveEqual) {
  std::vector<size_t> pos;
  EXPECT_EQ(stuff_bits("00000", &pos), "000001");
  EXPECT_EQ(pos, (std::vector<size_t>{5}));

  pos.clear();
  EXPECT_EQ(stuff_bits("11111", &pos), "111110");
  EXPECT_EQ(pos, (std::vector<size_t>{5}));
}

TEST(BitStuffing, StuffBitStartsNextRun) {
  // Вставленный stuff-бит считается первым битом следующей последовательности.
  std::vector<size_t> pos;
  EXPECT_EQ(stuff_bits("000001111", &pos), "00000111110");
  EXPECT_EQ(pos, (std::vector<size_t>{5, 10}));
}

TEST(BitStuffing, LongRunGetsMultipleStuffBits) {
  std::vector<size_t> pos;
  EXPECT_EQ(stuff_bits("0000000000", &pos), "000001000001");
  EXPECT_EQ(pos, (std::vector<size_t>{5, 11}));
}

TEST(BitStuffing, LaidFrameHasNoRunOfSix) {
  CanFrame f;
  f.id = 0;
  f.dlc = 8;
  f.data = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto lf = layout_frame(f);
  EXPECT_FALSE(lf.stuff_positions.empty());
  EXPECT_EQ(lf.stuffed.size(), lf.unstuffed.size() + lf.stuff_positions.size());
  EXPECT_FALSE(has_run_of_six(lf.stuffed));
}
