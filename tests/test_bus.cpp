#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cansim/bus.hpp"

using namespace cansim;

namespace {

CanFrame data_frame(uint32_t id, std::vector<uint8_t> data = {0x11}) {
  CanFrame f;
  f.id = id;
  f.dlc = static_cast<uint8_t>(data.size());
  f.data = std::move(data);
  return f;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool has_error(const DeliveredFrame& f, const std::string& kind) {
  return std::any_of(f.errors.begin(), f.errors.end(),
                     [&](const ErrorReport& e) { return e.kind == kind; });
}

class BusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bus.on_frame([this](const DeliveredFrame& f) { frames.push_back(f); });
  }

  Bus bus;
  std::vector<DeliveredFrame> frames;
};

}  // namespace

TEST_F(BusTest, LowerIdWinsArbitration) {
  bus.add_controller("a");
  bus.add_controller("b");
  ASSERT_TRUE(bus.submit("a", data_frame(0x123)));
  ASSERT_TRUE(bus.submit("b", data_frame(0x122)));

  ASSERT_TRUE(bus.step());
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].winner, "b");
  EXPECT_TRUE(contains(frames[0].losers, "a"));
  EXPECT_TRUE(frames[0].ok);
  ASSERT_EQ(frames[0].arbitration.size(), 1u);
  // 0x123 и 0x122 расходятся в младшем бите ID: SOF + 11 бит ID → индекс 11.
  EXPECT_EQ(frames[0].arbitration[0].bit_index, 11u);
  EXPECT_EQ(frames[0].arbitration[0].bus_bit, '0');
  EXPECT_EQ(bus.find("a")->stats().arbitration_lost, 1u);

  // Проигравший остаётся в очереди и передаёт следующим.
  ASSERT_TRUE(bus.step());
  EXPECT_EQ(frames[1].winner, "a");
  EXPECT_FALSE(bus.step());
}

TEST_F(BusTest, DataFrameBeatsRemoteFrameWithSameId) {
  bus.add_controller("data");
  bus.add_controller("rtr");
  CanFrame remote;
  remote.id = 0x100;
  remote.rtr = true;
  ASSERT_TRUE(bus.submit("rtr", remote));
  ASSERT_TRUE(bus.submit("data", data_frame(0x100)));

  ASSERT_TRUE(bus.step());
  EXPECT_EQ(frames[0].winner, "data");
  EXPECT_TRUE(contains(frames[0].losers, "rtr"));
}

TEST_F(BusTest, StandardFrameBeatsExtendedWithSameBaseId) {
  bus.add_controller("std");
  bus.add_controller("ext");
  CanFrame ext = data_frame(0x100u << 18);
  ext.extended = true;
  ASSERT_TRUE(bus.submit("ext", ext));
  ASSERT_TRUE(bus.submit("std", data_frame(0x100)));

  ASSERT_TRUE(bus.step());
  EXPECT_EQ(frames[0].winner, "std");
  EXPECT_TRUE(contains(frames[0].losers, "ext"));
}

TEST_F(BusTest, HeavilyStuffedFrameIsDestuffedWithoutErrors) {
  bus.add_controller("tx");
  bus.add_controller("rx");
  ASSERT_TRUE(bus.submit("tx", data_frame(0x000, {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF})));

  ASSERT_TRUE(bus.step());
  const auto& f = frames[0];
  EXPECT_FALSE(f.stuff_positions.empty());
  EXPECT_FALSE(has_error(f, "stuff_error"));
  EXPECT_FALSE(has_error(f, "crc_error"));
  EXPECT_TRUE(f.ack);
  EXPECT_TRUE(f.ok);
  EXPECT_TRUE(contains(f.acked_by, "rx"));
}

TEST_F(BusTest, MissingAckRaisesTecAndRec) {
  bus.add_controller("tx");
  bus.add_controller("rx").set_ack_enabled(false);
  ASSERT_TRUE(bus.submit("tx", data_frame(0x200)));

  ASSERT_TRUE(bus.step());
  EXPECT_FALSE(frames[0].ok);
  EXPECT_TRUE(has_error(frames[0], "ack_error"));
  EXPECT_EQ(bus.find("tx")->tec(), 8);
  EXPECT_EQ(bus.find("rx")->rec(), 1);
  EXPECT_EQ(bus.find("tx")->pending_count(), 1u);
}

TEST_F(BusTest, UnackedRetransmissionsLeadToPassiveThenBusOff) {
  bus.add_controller("alone");
  ASSERT_TRUE(bus.submit("alone", data_frame(0x300)));

  for (int i = 0; i < 16; ++i) ASSERT_TRUE(bus.step());
  EXPECT_EQ(bus.find("alone")->state(), ErrorState::Passive);

  for (int i = 0; i < 16; ++i) ASSERT_TRUE(bus.step());
  EXPECT_EQ(bus.find("alone")->state(), ErrorState::BusOff);

  EXPECT_FALSE(bus.step());
  EXPECT_FALSE(bus.submit("alone", data_frame(0x300)));
}
