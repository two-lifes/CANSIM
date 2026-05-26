#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "controller.hpp"
#include "frame.hpp"

namespace cansim {

struct ArbitrationStep {
  size_t bit_index = 0;
  char bus_bit = '1';
  std::vector<std::string> dropped;
};

struct ErrorReport {
  std::string kind;          // stuff_error | crc_error | form_error | ack_error | bit_error
  size_t at_bit = 0;
  std::string detected_by;
};

struct ControllerSnapshot {
  std::string id;
  uint16_t tec = 0;
  uint16_t rec = 0;
  std::string mode;
  ControllerStats stats;
  size_t pending = 0;
};

struct DeliveredFrame {
  uint64_t seq = 0;
  CanFrame frame;
  std::string winner;
  std::vector<std::string> losers;
  std::vector<std::string> acked_by;
  std::vector<std::string> app_receivers;
  BitString unstuffed;
  BitString stuffed;
  BitString trailer;
  std::vector<size_t> stuff_positions;
  size_t arbitration_begin = 0;
  size_t arbitration_end = 0;
  uint16_t crc = 0;
  bool ack = false;
  std::vector<ErrorReport> errors;
  std::vector<ArbitrationStep> arbitration;
  std::vector<ControllerSnapshot> states;
  uint64_t bus_time_us = 0;
  int retries_for_winner = 0;
  bool ok = false;            // true = принят шиной (ACK ok, без ошибок)
};

/**
 * Логическая модель CAN шины: bit-wise арбитраж по wired-AND,
 * генерация ACK, error detection (form/stuff/CRC/ack/bit),
 * счётчики TEC/REC, переходы active → passive → bus-off.
 *
 * Один шаг шины обрабатывает один кадр от старта до конца (или error frame).
 * Время "битового времени" моделируется счётчиком bit_time_us.
 */
class Bus {
 public:
  using FrameListener = std::function<void(const DeliveredFrame&)>;
  using StateListener = std::function<void(const ControllerSnapshot&)>;

  Bus();

  /** Скорость bit-time для отчётов в UI (по умолчанию 1 МБит/с → 1 мкс/бит). */
  void set_bit_time_us(int us) { bit_time_us_ = us > 0 ? us : 1; }
  int bit_time_us() const { return bit_time_us_; }

  /** Доля бит-ошибок при отправке кадра (0..1). Влияет на стресс-тесты. */
  void set_bit_error_rate(double r) { bit_error_rate_ = r < 0 ? 0 : (r > 1 ? 1 : r); }

  Controller& add_controller(const std::string& id);
  void remove_controller(const std::string& id);
  Controller* find(const std::string& id);
  std::vector<ControllerSnapshot> snapshots() const;

  /** Положить фрейм в очередь контроллера. */
  bool submit(const std::string& from, CanFrame f);

  /** Сделать один шаг шины. Возвращает true, если что-то было передано. */
  bool step();

  void on_frame(FrameListener l) { frame_listeners_.push_back(std::move(l)); }
  void on_state(StateListener l) { state_listeners_.push_back(std::move(l)); }

  uint64_t next_seq() { return ++seq_; }

 private:
  struct Contender {
    Controller* ctl = nullptr;
    LaidFrame laid;
    bool active = true;
  };

  ControllerSnapshot snapshot_of(const Controller& c) const;
  std::vector<ControllerSnapshot> snapshots_locked() const;
  void notify_state_locked(const Controller& c);

  std::vector<Contender> gather_contenders_locked();
  void run_arbitration(std::vector<Contender>& contenders,
                       DeliveredFrame& out);
  void simulate_transmission(Contender& winner,
                             std::vector<Contender>& receivers,
                             DeliveredFrame& out);
  void apply_outcome_locked(DeliveredFrame& out,
                            Contender& winner,
                            std::vector<Contender>& receivers);

  mutable std::mutex mu_;
  std::map<std::string, std::unique_ptr<Controller>> controllers_;
  std::vector<FrameListener> frame_listeners_;
  std::vector<StateListener> state_listeners_;

  uint64_t seq_ = 0;
  uint64_t bus_time_us_ = 0;
  int bit_time_us_ = 1;
  double bit_error_rate_ = 0.0;
};

}  // namespace cansim
