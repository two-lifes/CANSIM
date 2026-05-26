#include "controller.hpp"

#include <algorithm>

namespace cansim {

const char* to_string(ErrorState s) {
  switch (s) {
    case ErrorState::Active: return "active";
    case ErrorState::Passive: return "passive";
    case ErrorState::BusOff: return "bus-off";
  }
  return "?";
}

Controller::Controller(std::string id) : id_(std::move(id)) {}

void Controller::queue_tx(CanFrame f) {
  if (state_ == ErrorState::BusOff) return;
  if (listen_only_) return;
  pending_.push_back(std::move(f));
}

void Controller::on_tx_success() {
  if (tec_ > 0) --tec_;
  ++stats_.tx_ok;
  update_state();
}

void Controller::on_rx_success() {
  if (rec_ > 0 && rec_ < 128) {
    --rec_;
  } else if (rec_ >= 128) {
    rec_ = 120;
  }
  ++stats_.rx_ok;
  update_state();
}

void Controller::on_app_rx_match() {
  ++stats_.app_rx_ok;
}

void Controller::on_tx_error(int weight) {
  tec_ = static_cast<uint16_t>(std::min<int>(tec_ + weight, 65535));
  ++stats_.tx_errors;
  ++stats_.error_frames_tx;
  update_state();
}

void Controller::on_rx_error(int weight) {
  rec_ = static_cast<uint16_t>(std::min<int>(rec_ + weight, 65535));
  ++stats_.rx_errors;
  ++stats_.error_frames_rx;
  update_state();
}

void Controller::on_arbitration_lost() {
  ++stats_.arbitration_lost;
}

void Controller::reset_from_bus_off() {
  tec_ = 0;
  rec_ = 0;
  state_ = ErrorState::Active;
}

void Controller::set_app_rx_filters(std::vector<uint32_t> can_ids) {
  app_rx_filters_ = std::move(can_ids);
}

bool Controller::accepts_app_rx(uint32_t can_id) const {
  return std::find(app_rx_filters_.begin(), app_rx_filters_.end(), can_id) != app_rx_filters_.end();
}

void Controller::update_state() {
  const ErrorState before = state_;
  if (tec_ >= 256) {
    state_ = ErrorState::BusOff;
  } else if (tec_ >= 128 || rec_ >= 128) {
    state_ = ErrorState::Passive;
  } else {
    state_ = ErrorState::Active;
  }
  if (before != ErrorState::BusOff && state_ == ErrorState::BusOff) {
    ++stats_.bus_off_events;
  }
}

}  // namespace cansim
