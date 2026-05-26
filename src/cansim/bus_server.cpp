#include "bus_server.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

#include "wire_protocol.hpp"

namespace cansim {

using nlohmann::json;

BusServer::BusServer(Bus& bus, std::string socket_path)
    : bus_(bus), socket_path_(std::move(socket_path)) {}

BusServer::~BusServer() { stop(); }

void BusServer::set_vcan_mirror(const std::string& iface) {
  vcan_iface_ = iface;
  if (vcan_iface_.empty()) return;

  vcan_fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (vcan_fd_ < 0) {
    std::cerr << "bussim: vcan mirror socket failed: " << std::strerror(errno) << "\n";
    vcan_iface_.clear();
    return;
  }
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, vcan_iface_.c_str(), IFNAMSIZ - 1);
  if (ioctl(vcan_fd_, SIOCGIFINDEX, &ifr) < 0) {
    std::cerr << "bussim: vcan iface " << vcan_iface_ << " not found (run scripts/setup-vcan.sh)\n";
    ::close(vcan_fd_);
    vcan_fd_ = -1;
    vcan_iface_.clear();
    return;
  }
  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(vcan_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bussim: vcan bind failed: " << std::strerror(errno) << "\n";
    ::close(vcan_fd_);
    vcan_fd_ = -1;
    vcan_iface_.clear();
    return;
  }
  std::cerr << "bussim: mirroring delivered frames to " << vcan_iface_ << "\n";
}

bool BusServer::start() {
  ::unlink(socket_path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    std::cerr << "bussim: socket() failed: " << std::strerror(errno) << "\n";
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bussim: bind " << socket_path_ << " failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  ::chmod(socket_path_.c_str(), 0666);
  if (listen(listen_fd_, 32) < 0) {
    std::cerr << "bussim: listen failed: " << std::strerror(errno) << "\n";
    return false;
  }

  bus_.on_frame([this](const DeliveredFrame& f) {
    const std::string text = json{{"op", "frame"}, {"data", to_json(f)}}.dump();
    broadcast(text);
    if (f.ok) mirror_to_vcan(f);
  });
  bus_.on_state([this](const ControllerSnapshot& s) {
    const std::string text = json{{"op", "state"}, {"data", to_json(s)}}.dump();
    broadcast(text);
  });

  running_ = true;
  accept_thread_ = std::thread([this] { accept_loop(); });
  tick_thread_ = std::thread([this] { tick_loop(); });
  std::cerr << "bussim: listening on " << socket_path_ << "\n";
  return true;
}

void BusServer::stop() {
  running_ = false;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  ::unlink(socket_path_.c_str());
  if (accept_thread_.joinable()) accept_thread_.join();
  if (tick_thread_.joinable()) tick_thread_.join();

  std::vector<std::shared_ptr<Client>> snapshot;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    for (auto& [fd, c] : clients_) snapshot.push_back(c);
    clients_.clear();
  }
  for (auto& c : snapshot) {
    c->alive = false;
    ::shutdown(c->fd, SHUT_RDWR);
    // тред клиента detached, сам выйдет при recv()<=0
  }
  if (vcan_fd_ >= 0) {
    ::close(vcan_fd_);
    vcan_fd_ = -1;
  }
}

void BusServer::accept_loop() {
  while (running_) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_) break;
      continue;
    }
    auto client = std::make_shared<Client>();
    client->fd = fd;
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      clients_[fd] = client;
    }
    std::thread([this, client]() mutable { client_loop(std::move(client)); }).detach();
  }
}

void BusServer::client_loop(std::shared_ptr<Client> client) {
  std::string buffer;
  char chunk[4096];
  while (running_ && client->alive) {
    const ssize_t n = ::recv(client->fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buffer.append(chunk, chunk + n);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!line.empty()) handle_line(client, line);
    }
  }
  if (!client->node_id.empty()) {
    bus_.remove_controller(client->node_id);
    broadcast(json{{"op", "disconnect"}, {"node_id", client->node_id}}.dump());
  }
  const int fd = client->fd;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    clients_.erase(fd);
  }
  ::close(fd);
}

void BusServer::send_to(Client& c, const std::string& text) {
  std::lock_guard<std::mutex> lock(c.write_mu);
  std::string framed = text;
  framed.push_back('\n');
  const char* p = framed.data();
  size_t left = framed.size();
  while (left > 0) {
    const ssize_t n = ::send(c.fd, p, left, MSG_NOSIGNAL);
    if (n <= 0) break;
    p += n;
    left -= static_cast<size_t>(n);
  }
}

void BusServer::broadcast(const std::string& text) {
  std::vector<std::shared_ptr<Client>> targets;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    for (auto& [fd, c] : clients_) targets.push_back(c);
  }
  for (auto& c : targets) send_to(*c, text);
}

void BusServer::tick_loop() {
  // Тик ~100 мкс. Это достаточно, чтобы несколько ECU успели положить
  // фреймы в очередь одновременно и реальный арбитраж сработал.
  while (running_) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (!running_) break;
    try {
      bus_.step();
    } catch (const std::exception& e) {
      std::cerr << "bussim: step failed: " << e.what() << "\n";
    }
  }
}

void BusServer::mirror_to_vcan(const DeliveredFrame& f) {
  if (vcan_fd_ < 0) return;
  can_frame raw{};
  raw.can_id = f.frame.id & (f.frame.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
  if (f.frame.extended) raw.can_id |= CAN_EFF_FLAG;
  if (f.frame.rtr) raw.can_id |= CAN_RTR_FLAG;
  raw.can_dlc = static_cast<uint8_t>(std::min<size_t>(f.frame.data.size(), 8));
  for (int i = 0; i < raw.can_dlc; ++i) raw.data[i] = f.frame.data[i];
  if (::write(vcan_fd_, &raw, sizeof(raw)) != sizeof(raw)) {
    // Зеркало — best-effort; не падаем при ошибке записи в vcan.
  }
}

void BusServer::handle_line(std::shared_ptr<Client>& client, const std::string& line) {
  json msg;
  try {
    msg = json::parse(line);
  } catch (...) {
    send_to(*client, json{{"op", "error"}, {"message", "bad_json"}}.dump());
    return;
  }
  const std::string op = msg.value("op", "");

  if (op == "hello") {
    const std::string node_id = msg.value("node_id", "");
    if (node_id.empty()) {
      send_to(*client, json{{"op", "error"}, {"message", "node_id required"}}.dump());
      return;
    }
    client->node_id = node_id;
    auto& ctl = bus_.add_controller(node_id);
    ctl.set_ack_enabled(msg.value("ack", true));
    ctl.set_listen_only(msg.value("listen_only", false));
    const auto snaps = bus_.snapshots();
    ControllerSnapshot my;
    for (const auto& s : snaps) if (s.id == node_id) { my = s; break; }
    json reply = {{"op", "hello_ok"}, {"data", to_json(my)}};
    send_to(*client, reply.dump());
    broadcast(json{{"op", "join"}, {"node_id", node_id}}.dump());
    return;
  }

  if (client->node_id.empty()) {
    send_to(*client, json{{"op", "error"}, {"message", "hello first"}}.dump());
    return;
  }

  if (op == "tx") {
    try {
      const auto frame = frame_from_json(msg);
      bus_.submit(client->node_id, frame);
      // Реальная передача произойдёт на ближайшем тике шины (см. tick_loop).
    } catch (const std::exception& e) {
      send_to(*client, json{{"op", "error"}, {"message", e.what()}}.dump());
    }
  } else if (op == "set_ack") {
    if (auto* c = bus_.find(client->node_id)) c->set_ack_enabled(msg.value("enabled", true));
  } else if (op == "set_listen_only") {
    if (auto* c = bus_.find(client->node_id)) c->set_listen_only(msg.value("enabled", false));
  } else if (op == "set_app_rx_filters") {
    if (auto* c = bus_.find(client->node_id)) {
      std::vector<uint32_t> ids;
      for (const auto& item : msg.value("filters", json::array())) {
        if (item.is_number_unsigned()) {
          ids.push_back(item.get<uint32_t>());
        } else if (item.is_number_integer()) {
          ids.push_back(static_cast<uint32_t>(item.get<int>()));
        } else if (item.is_string()) {
          ids.push_back(static_cast<uint32_t>(std::strtoul(item.get<std::string>().c_str(), nullptr, 0)));
        }
      }
      c->set_app_rx_filters(std::move(ids));
      const auto snaps = bus_.snapshots();
      for (const auto& s : snaps) {
        if (s.id == client->node_id) {
          broadcast(json{{"op", "state"}, {"data", to_json(s)}}.dump());
          break;
        }
      }
    }
  } else if (op == "reset_bus_off") {
    if (auto* c = bus_.find(client->node_id)) {
      c->reset_from_bus_off();
      broadcast(json{{"op", "state"}, {"data", to_json(bus_.snapshots()[0])}}.dump());
    }
  } else if (op == "set_bit_error_rate") {
    bus_.set_bit_error_rate(msg.value("rate", 0.0));
  } else if (op == "snapshot") {
    json arr = json::array();
    for (const auto& s : bus_.snapshots()) arr.push_back(to_json(s));
    send_to(*client, json{{"op", "snapshot"}, {"data", arr}}.dump());
  } else {
    send_to(*client, json{{"op", "error"}, {"message", "unknown op: " + op}}.dump());
  }
}

}  // namespace cansim
