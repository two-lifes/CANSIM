#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "can_bus.hpp"
#include "ecu_config.hpp"
#include "profiles.hpp"

/** Узел на шине: один CAN-сокет, приём по фильтрам ID, передача на выбранный ID. */
class Ecu {
 public:
  using SendFn = std::function<void(int fd, const nlohmann::json& msg)>;

  explicit Ecu(std::string ecu_id, std::string can_interface = "vcan0");
  ~Ecu();

  const std::string& ecu_id() const { return ecu_id_; }

  void set_defaults(EcuDefaults defaults);
  void set_ui_endpoint(int port, std::string page);
  const EcuDefaults& defaults() const { return defaults_; }
  void set_network(nlohmann::json network);
  void set_bus_monitor(bool enabled);

  void start();
  void stop();
  void set_send_fn(SendFn fn);

  void attach_ui(int fd);
  void detach_ui(int fd);
  void register_ui(int fd);
  void set_tx_config(int fd, const nlohmann::json& profile);
  void set_rx_config(int fd, const nlohmann::json& profile);
  void stage_payload(int fd, const nlohmann::json& body);
  void publish(int fd, const nlohmann::json& body);
  void reset_bus_off();

  nlohmann::json status() const;

 private:
  struct UiClient {
    int fd = -1;
    std::optional<TxProfile> tx;
    std::optional<RxProfile> rx;
    nlohmann::json last_payload;
    std::atomic<bool> interval_stop{false};
    std::thread interval_thread;
  };

  UiClient* ui_by_fd(int fd);
  void send_to_fd(int fd, const nlohmann::json& msg);
  void on_can_frame(uint32_t can_id, const std::vector<uint8_t>& data);
  void on_bus_delivered(const nlohmann::json& delivered);
  void on_bus_state(const nlohmann::json& snapshot);
  bool passes_condition(UiClient& ui, const nlohmann::json& payload);
  void start_interval_locked(UiClient& ui);
  void restart_interval(UiClient& ui);
  void stop_interval(UiClient& ui);
  void apply_defaults(UiClient& ui);
  void sync_app_rx_filters_locked();

  std::string ecu_id_;
  CanBus can_;
  EcuDefaults defaults_;
  int ui_port_ = 0;
  std::string ui_page_;
  nlohmann::json network_ = nlohmann::json::array();
  SendFn send_fn_;
  mutable std::mutex mu_;
  std::map<int, std::unique_ptr<UiClient>> ui_clients_;
  bool bus_monitor_ = false;
};
