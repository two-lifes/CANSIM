#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "frame.hpp"

namespace cansim {

/**
 * Клиент CAN bus-демона. Каждый ECU держит одного клиента; клиент =
 * один CAN-контроллер на стороне демона.
 *
 * - tx(): помещает запрос на отправку (демон выполнит арбитраж/CRC/ACK)
 * - on_frame(): получаем CALLBACK на каждый кадр, прошедший шину
 * - on_state(): получаем CALLBACK на изменение нашего TEC/REC/mode
 */
class BusClient {
 public:
  using FrameCb = std::function<void(const nlohmann::json& delivered_frame)>;
  using StateCb = std::function<void(const nlohmann::json& snapshot)>;

  BusClient(std::string node_id, std::string socket_path);
  ~BusClient();

  bool connect();
  void disconnect();
  bool is_open() const { return fd_ >= 0; }

  bool tx(const CanFrame& f);
  bool set_ack(bool enabled);
  bool set_app_rx_filters(const std::vector<uint32_t>& can_ids);
  bool reset_bus_off();

  void on_frame(FrameCb cb) { frame_cb_ = std::move(cb); }
  void on_state(StateCb cb) { state_cb_ = std::move(cb); }

  const std::string& node_id() const { return node_id_; }
  const std::string& socket_path() const { return socket_path_; }
  std::string last_error() const;

 private:
  void reader_loop();
  bool send_json(const nlohmann::json& msg);
  void set_error(const std::string& msg);

  std::string node_id_;
  std::string socket_path_;
  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread reader_;
  std::mutex write_mu_;
  mutable std::mutex error_mu_;
  std::string last_error_;
  FrameCb frame_cb_;
  StateCb state_cb_;
};

}  // namespace cansim
