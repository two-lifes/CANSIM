#include "bus_client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include "wire_protocol.hpp"

namespace cansim {

using nlohmann::json;

BusClient::BusClient(std::string node_id, std::string socket_path)
    : node_id_(std::move(node_id)), socket_path_(std::move(socket_path)) {}

BusClient::~BusClient() { disconnect(); }

void BusClient::set_error(const std::string& msg) {
  std::lock_guard<std::mutex> lock(error_mu_);
  last_error_ = msg;
}

std::string BusClient::last_error() const {
  std::lock_guard<std::mutex> lock(error_mu_);
  return last_error_;
}

bool BusClient::connect() {
  fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) {
    set_error(std::string("socket: ") + std::strerror(errno));
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    set_error(std::string("connect ") + socket_path_ + ": " + std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  if (!send_json({{"op", "hello"}, {"node_id", node_id_}, {"ack", true}})) {
    set_error("hello send failed");
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  running_ = true;
  reader_ = std::thread([this] { reader_loop(); });
  return true;
}

void BusClient::disconnect() {
  running_ = false;
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  if (reader_.joinable()) reader_.join();
}

bool BusClient::send_json(const json& msg) {
  if (fd_ < 0) return false;
  std::string text = msg.dump();
  text.push_back('\n');
  std::lock_guard<std::mutex> lock(write_mu_);
  const char* p = text.data();
  size_t left = text.size();
  while (left > 0) {
    const ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
    if (n <= 0) return false;
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

bool BusClient::tx(const CanFrame& f) {
  json data = to_json(f);
  data["op"] = "tx";
  return send_json(data);
}

bool BusClient::set_ack(bool enabled) {
  return send_json({{"op", "set_ack"}, {"enabled", enabled}});
}

bool BusClient::set_app_rx_filters(const std::vector<uint32_t>& can_ids) {
  json filters = json::array();
  for (uint32_t id : can_ids) filters.push_back(id);
  return send_json({{"op", "set_app_rx_filters"}, {"filters", filters}});
}

bool BusClient::reset_bus_off() {
  return send_json({{"op", "reset_bus_off"}});
}

void BusClient::reader_loop() {
  std::string buffer;
  char chunk[4096];
  while (running_) {
    const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buffer.append(chunk, chunk + n);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (line.empty()) continue;
      json msg;
      try {
        msg = json::parse(line);
      } catch (...) {
        continue;
      }
      const std::string op = msg.value("op", "");
      if (op == "frame" && frame_cb_) {
        frame_cb_(msg.value("data", json::object()));
      } else if (op == "state" && state_cb_) {
        state_cb_(msg.value("data", json::object()));
      } else if (op == "hello_ok" && state_cb_) {
        state_cb_(msg.value("data", json::object()));
      } else if (op == "error") {
        set_error(msg.value("message", ""));
      }
    }
  }
  set_error("disconnected");
  running_ = false;
}

}  // namespace cansim
