#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bus.hpp"

namespace cansim {

/**
 * Демон-симулятор CAN-шины: слушает Unix-сокет, принимает подключения
 * от ECU-процессов, каждое подключение = один CAN-контроллер.
 */
class BusServer {
 public:
  BusServer(Bus& bus, std::string socket_path);
  ~BusServer();

  bool start();
  void stop();

  /** Опционально: каждый успешный кадр зеркалится в SocketCAN интерфейс. */
  void set_vcan_mirror(const std::string& iface);

 private:
  struct Client {
    int fd = -1;
    std::string node_id;
    std::atomic<bool> alive{true};
    std::mutex write_mu;
  };

  void accept_loop();
  void tick_loop();
  void client_loop(std::shared_ptr<Client> client);
  void handle_line(std::shared_ptr<Client>& client, const std::string& line);
  void send_to(Client& c, const std::string& text);
  void broadcast(const std::string& text);
  void mirror_to_vcan(const DeliveredFrame& f);

  Bus& bus_;
  std::string socket_path_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::thread tick_thread_;

  std::mutex clients_mu_;
  std::map<int, std::shared_ptr<Client>> clients_;

  std::string vcan_iface_;
  int vcan_fd_ = -1;
};

}  // namespace cansim
