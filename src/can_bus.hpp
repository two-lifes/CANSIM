#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace cansim {
class BusClient;
}

/**
 * Транспорт CAN для одного ECU. По умолчанию пытается подключиться к
 * демону linuxcan-bussim (битовый симулятор с арбитражем/CRC/ACK/TEC/REC),
 * а при его отсутствии откатывается на прямой vcan (как было раньше).
 */
class CanBus {
 public:
  using FrameHandler =
      std::function<void(uint32_t can_id, const std::vector<uint8_t>& data)>;
  using DeliveredHandler = std::function<void(const nlohmann::json& delivered)>;
  using StateHandler = std::function<void(const nlohmann::json& snapshot)>;

  CanBus(std::string node_id, std::string interface);
  ~CanBus();

  bool start();
  void stop();

  /** Отправка кадра. На bussim — через арбитраж/CRC; на vcan — прямо. */
  bool send(uint32_t can_id, const std::vector<uint8_t>& data);

  /** Колбэк на простой "пришёл кадр". */
  void on_frame(FrameHandler h) { frame_handler_ = std::move(h); }

  /** Колбэк на полную доставку (со всеми битами, ACK, ошибками, …). */
  void on_delivered(DeliveredHandler h) { delivered_handler_ = std::move(h); }

  /** Колбэк на изменение состояния нашего контроллера (TEC/REC/mode). */
  void on_state(StateHandler h) { state_handler_ = std::move(h); }

  bool is_open() const { return open_.load(); }
  const std::string& interface_name() const { return interface_; }
  const std::string& mode_name() const { return mode_; }
  nlohmann::json controller_status() const;

  /** Сбросить контроллер из bus-off в active. */
  bool reset_bus_off();

  /** Application-level RX-фильтры: для UI/диагностики, не для физического ACK. */
  bool set_app_rx_filters(const std::vector<uint32_t>& can_ids);

 private:
  bool start_bussim();
  bool start_vcan_fallback();
  void vcan_reader_loop();

  std::string node_id_;
  std::string interface_;
  std::string mode_ = "down";  // "bussim" | "vcan" | "down"

  std::unique_ptr<cansim::BusClient> bussim_;
  FrameHandler frame_handler_;
  DeliveredHandler delivered_handler_;
  StateHandler state_handler_;

  // legacy vcan fallback
  int vcan_fd_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> open_{false};
  std::thread reader_;

  mutable std::mutex state_mu_;
  nlohmann::json controller_state_;
};
