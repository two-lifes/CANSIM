#include "can_bus.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "cansim/bus_client.hpp"
#include "cansim/frame.hpp"
#include "cansim/wire_protocol.hpp"

CanBus::CanBus(std::string node_id, std::string interface)
    : node_id_(std::move(node_id)), interface_(std::move(interface)) {}

CanBus::~CanBus() { stop(); }

nlohmann::json CanBus::controller_status() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return controller_state_;
}

bool CanBus::start() {
  if (open_) return true;
  if (start_bussim()) return true;
  return start_vcan_fallback();
}

bool CanBus::start_bussim() {
  std::string sock = cansim::default_socket_path();
  if (const char* env = std::getenv("LINUXCAN_BUS_SOCKET")) sock = env;
  if (std::getenv("LINUXCAN_NO_BUSSIM")) return false;

  bussim_ = std::make_unique<cansim::BusClient>(node_id_, sock);
  bussim_->on_frame([this](const nlohmann::json& d) {
    if (delivered_handler_) delivered_handler_(d);
    if (frame_handler_ && d.value("ok", false)) {
      const auto& frame = d.value("frame", nlohmann::json::object());
      const uint32_t id = frame.value("id", 0u);
      std::vector<uint8_t> data;
      for (const auto& b : frame.value("data", nlohmann::json::array())) {
        data.push_back(static_cast<uint8_t>(b.get<int>() & 0xFF));
      }
      frame_handler_(id, data);
    }
  });
  bussim_->on_state([this](const nlohmann::json& s) {
    if (s.value("node_id", std::string()) == node_id_) {
      std::lock_guard<std::mutex> lock(state_mu_);
      controller_state_ = s;
    }
    if (state_handler_) state_handler_(s);
  });
  if (!bussim_->connect()) {
    std::cerr << "CAN " << node_id_ << ": bussim not available ("
              << bussim_->last_error() << ") — fallback to vcan\n";
    bussim_.reset();
    return false;
  }
  mode_ = "bussim";
  open_ = true;
  std::cerr << "CAN " << node_id_ << ": connected to bussim ("
            << bussim_->socket_path() << ")\n";
  return true;
}

bool CanBus::start_vcan_fallback() {
  vcan_fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (vcan_fd_ < 0) {
    std::cerr << "CAN " << node_id_ << ": socket() failed: " << std::strerror(errno) << "\n";
    return false;
  }
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);
  if (ioctl(vcan_fd_, SIOCGIFINDEX, &ifr) < 0) {
    std::cerr << "CAN " << node_id_ << ": iface " << interface_ << " not found: "
              << std::strerror(errno) << "\n";
    ::close(vcan_fd_);
    vcan_fd_ = -1;
    return false;
  }
  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(vcan_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "CAN " << node_id_ << ": bind failed: " << std::strerror(errno) << "\n";
    ::close(vcan_fd_);
    vcan_fd_ = -1;
    return false;
  }
  running_ = true;
  reader_ = std::thread([this] { vcan_reader_loop(); });
  mode_ = "vcan";
  open_ = true;
  std::cerr << "CAN " << node_id_ << ": raw vcan fallback on " << interface_ << "\n";
  return true;
}

void CanBus::stop() {
  running_ = false;
  if (bussim_) {
    bussim_->disconnect();
    bussim_.reset();
  }
  if (vcan_fd_ >= 0) {
    ::shutdown(vcan_fd_, SHUT_RDWR);
  }
  if (reader_.joinable()) reader_.join();
  if (vcan_fd_ >= 0) {
    ::close(vcan_fd_);
    vcan_fd_ = -1;
  }
  open_ = false;
  mode_ = "down";
}

bool CanBus::send(uint32_t can_id, const std::vector<uint8_t>& data) {
  if (!open_) return false;
  if (bussim_) {
    cansim::CanFrame f;
    f.id = can_id & 0x1FFFFFFFu;
    f.extended = (can_id & 0x80000000u) != 0 || (can_id > 0x7FFu);
    if (f.extended) f.id &= 0x1FFFFFFFu;
    else f.id &= 0x7FFu;
    f.rtr = false;
    f.dlc = static_cast<uint8_t>(std::min<size_t>(data.size(), 8));
    f.data.assign(data.begin(),
                   data.begin() + static_cast<std::ptrdiff_t>(f.dlc));
    return bussim_->tx(f);
  }
  if (vcan_fd_ < 0) return false;
  can_frame frame{};
  frame.can_id = can_id & CAN_SFF_MASK;
  frame.can_dlc = static_cast<uint8_t>(std::min<size_t>(data.size(), 8));
  for (int i = 0; i < frame.can_dlc; ++i) frame.data[i] = data[i];
  const ssize_t n = ::write(vcan_fd_, &frame, sizeof(frame));
  if (n != sizeof(frame)) {
    std::cerr << "CAN " << node_id_ << ": vcan write failed: " << std::strerror(errno) << "\n";
    return false;
  }
  return true;
}

bool CanBus::reset_bus_off() {
  if (bussim_) return bussim_->reset_bus_off();
  return false;
}

bool CanBus::set_app_rx_filters(const std::vector<uint32_t>& can_ids) {
  if (bussim_) return bussim_->set_app_rx_filters(can_ids);
  return false;
}

void CanBus::vcan_reader_loop() {
  while (running_ && vcan_fd_ >= 0) {
    pollfd pfd{vcan_fd_, POLLIN, 0};
    if (poll(&pfd, 1, 500) <= 0) continue;
    can_frame frame{};
    const ssize_t n = ::read(vcan_fd_, &frame, sizeof(frame));
    if (n < static_cast<ssize_t>(sizeof(can_frame))) continue;
    if (!frame_handler_) continue;
    std::vector<uint8_t> data(frame.data, frame.data + frame.can_dlc);
    frame_handler_(frame.can_id & CAN_SFF_MASK, data);
  }
}
