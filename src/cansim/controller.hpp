#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "frame.hpp"

namespace cansim {

enum class ErrorState : uint8_t {
  Active,
  Passive,
  BusOff,
};

const char* to_string(ErrorState s);

struct ControllerStats {
  uint64_t tx_ok = 0;
  uint64_t rx_ok = 0;
  uint64_t tx_errors = 0;
  uint64_t rx_errors = 0;
  uint64_t arbitration_lost = 0;
  uint64_t error_frames_tx = 0;
  uint64_t error_frames_rx = 0;
  uint64_t bus_off_events = 0;
  uint64_t app_rx_ok = 0;
};

/**
 * Состояние одного CAN-контроллера ECU.
 * Логика TEC/REC реализована близко к ISO 11898 (упрощённо, без всех 8 веток).
 */
class Controller {
 public:
  explicit Controller(std::string id);

  const std::string& id() const { return id_; }

  void queue_tx(CanFrame f);
  bool has_pending_tx() const { return !pending_.empty(); }
  const CanFrame& peek_tx() const { return pending_.front(); }
  void pop_tx() { if (!pending_.empty()) pending_.pop_front(); }
  size_t pending_count() const { return pending_.size(); }

  ErrorState state() const { return state_; }
  uint16_t tec() const { return tec_; }
  uint16_t rec() const { return rec_; }
  const ControllerStats& stats() const { return stats_; }

  void on_tx_success();
  void on_rx_success();
  void on_app_rx_match();
  void on_tx_error(int weight = 8);
  void on_rx_error(int weight = 1);
  void on_arbitration_lost();
  void reset_from_bus_off();

  void set_ack_enabled(bool v) { ack_enabled_ = v; }
  bool ack_enabled() const { return ack_enabled_; }

  void set_listen_only(bool v) { listen_only_ = v; }
  bool listen_only() const { return listen_only_; }

  void set_app_rx_filters(std::vector<uint32_t> can_ids);
  bool accepts_app_rx(uint32_t can_id) const;

 private:
  void update_state();

  std::string id_;
  std::deque<CanFrame> pending_;
  uint16_t tec_ = 0;
  uint16_t rec_ = 0;
  ErrorState state_ = ErrorState::Active;
  bool ack_enabled_ = true;
  bool listen_only_ = false;
  std::vector<uint32_t> app_rx_filters_;
  ControllerStats stats_;
};

}  // namespace cansim
