#include <gtest/gtest.h>

#include "cansim/controller.hpp"

using namespace cansim;

TEST(Controller, StartsErrorActive) {
  Controller c("n");
  EXPECT_EQ(c.state(), ErrorState::Active);
  EXPECT_EQ(c.tec(), 0);
  EXPECT_EQ(c.rec(), 0);
}

TEST(Controller, TecReaches128GoesPassive) {
  Controller c("n");
  for (int i = 0; i < 15; ++i) c.on_tx_error(8);
  EXPECT_EQ(c.tec(), 120);
  EXPECT_EQ(c.state(), ErrorState::Active);
  c.on_tx_error(8);
  EXPECT_EQ(c.tec(), 128);
  EXPECT_EQ(c.state(), ErrorState::Passive);
}

TEST(Controller, TecReaches256GoesBusOff) {
  Controller c("n");
  for (int i = 0; i < 31; ++i) c.on_tx_error(8);
  EXPECT_EQ(c.state(), ErrorState::Passive);
  c.on_tx_error(8);
  EXPECT_EQ(c.tec(), 256);
  EXPECT_EQ(c.state(), ErrorState::BusOff);
  EXPECT_EQ(c.stats().bus_off_events, 1u);
}

TEST(Controller, BusOffDropsNewFramesUntilReset) {
  Controller c("n");
  for (int i = 0; i < 32; ++i) c.on_tx_error(8);
  c.queue_tx(CanFrame{});
  EXPECT_FALSE(c.has_pending_tx());

  c.reset_from_bus_off();
  EXPECT_EQ(c.state(), ErrorState::Active);
  EXPECT_EQ(c.tec(), 0);
  c.queue_tx(CanFrame{});
  EXPECT_TRUE(c.has_pending_tx());
}

TEST(Controller, TxSuccessDecrementsTecBackToActive) {
  Controller c("n");
  for (int i = 0; i < 16; ++i) c.on_tx_error(8);
  ASSERT_EQ(c.state(), ErrorState::Passive);
  c.on_tx_success();
  EXPECT_EQ(c.tec(), 127);
  EXPECT_EQ(c.state(), ErrorState::Active);
}

TEST(Controller, RecReaches128GoesPassiveAndRecovers) {
  Controller c("n");
  for (int i = 0; i < 127; ++i) c.on_rx_error(1);
  EXPECT_EQ(c.state(), ErrorState::Active);
  c.on_rx_error(1);
  EXPECT_EQ(c.state(), ErrorState::Passive);

  c.on_rx_success();
  EXPECT_EQ(c.rec(), 120);
  EXPECT_EQ(c.state(), ErrorState::Active);
}

TEST(Controller, RecAloneNeverCausesBusOff) {
  Controller c("n");
  for (int i = 0; i < 1000; ++i) c.on_rx_error(1);
  EXPECT_EQ(c.state(), ErrorState::Passive);
}
