#include "ecu.hpp"

#include "encoding.hpp"
#include "hooks.hpp"

#include <chrono>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

std::string hex_id(uint32_t id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%X", id);
  return buf;
}

nlohmann::json tx_to_json(const TxProfile& tx) {
  return {{"can_id", hex_id(tx.can_id)},
          {"encoding", tx.encoding},
          {"schedule", {{"mode", tx.schedule_mode}, {"interval_ms", tx.interval_ms}}}};
}

nlohmann::json rx_to_json(const RxProfile& rx) {
  nlohmann::json filters = nlohmann::json::array();
  for (const auto& f : rx.filters) {
    nlohmann::json item = {{"can_id", hex_id(f.can_id)}, {"encoding", f.encoding}};
    if (!f.label.empty()) item["label"] = f.label;
    filters.push_back(item);
  }
  return {{"filters", filters}};
}

}  // namespace

Ecu::Ecu(std::string ecu_id, std::string can_interface)
    : ecu_id_(std::move(ecu_id)), can_(ecu_id_, std::move(can_interface)) {}

Ecu::~Ecu() { stop(); }

void Ecu::set_defaults(EcuDefaults defaults) { defaults_ = std::move(defaults); }

void Ecu::set_ui_endpoint(int port, std::string page) {
  ui_port_ = port;
  ui_page_ = std::move(page);
}

void Ecu::set_network(nlohmann::json network) { network_ = std::move(network); }

void Ecu::set_bus_monitor(bool enabled) { bus_monitor_ = enabled; }

void Ecu::start() {
  can_.on_frame([this](uint32_t id, const std::vector<uint8_t>& data) { on_can_frame(id, data); });
  can_.on_delivered([this](const nlohmann::json& d) { on_bus_delivered(d); });
  can_.on_state([this](const nlohmann::json& s) { on_bus_state(s); });
  if (!can_.start()) {
    std::cerr << "ECU " << ecu_id_ << ": CAN " << can_.interface_name()
              << " unavailable (run scripts/setup-vcan.sh)\n";
  }
  std::lock_guard lock(mu_);
  sync_app_rx_filters_locked();
}

void Ecu::stop() {
  std::vector<UiClient*> clients;
  {
    std::lock_guard lock(mu_);
    for (auto& [fd, ui] : ui_clients_) {
      (void)fd;
      ui->interval_stop = true;
      clients.push_back(ui.get());
    }
  }
  for (auto* ui : clients) stop_interval(*ui);
  {
    std::lock_guard lock(mu_);
    ui_clients_.clear();
  }
  can_.stop();
}

void Ecu::set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }

nlohmann::json Ecu::status() const {
  auto j = defaults_.to_json(ui_port_, ui_page_);
  {
    std::lock_guard lock(mu_);
    // If multiple browser tabs are open, the highest fd is the latest UI client.
    // That is the runtime state the diagnostics monitor should show.
    for (const auto& [fd, ui] : ui_clients_) {
      (void)fd;
      if (ui->tx) j["tx"] = tx_to_json(*ui->tx);
      if (ui->rx) j["rx"] = rx_to_json(*ui->rx);
    }
  }
  j["ecu_id"] = ecu_id_;
  j["can_interface"] = can_.interface_name();
  j["can_open"] = can_.is_open();
  j["can_mode"] = can_.mode_name();
  j["controller"] = can_.controller_status();
  j["network"] = network_;
  j["bus_monitor"] = bus_monitor_;
  return j;
}

void Ecu::send_to_fd(int fd, const nlohmann::json& msg) {
  if (send_fn_) send_fn_(fd, msg);
}

void Ecu::attach_ui(int fd) {
  std::lock_guard lock(mu_);
  auto ui = std::make_unique<UiClient>();
  ui->fd = fd;
  ui_clients_[fd] = std::move(ui);
}

void Ecu::detach_ui(int fd) {
  std::unique_ptr<UiClient> removed;
  {
    std::lock_guard lock(mu_);
    auto it = ui_clients_.find(fd);
    if (it == ui_clients_.end()) return;
    it->second->interval_stop = true;
    removed = std::move(it->second);
    ui_clients_.erase(it);
    sync_app_rx_filters_locked();
  }
  stop_interval(*removed);
}

void Ecu::apply_defaults(UiClient& ui) {
  ui.tx = defaults_.tx;
  ui.rx = defaults_.rx;
}

void Ecu::register_ui(int fd) {
  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (!ui) return;

    apply_defaults(*ui);
    sync_app_rx_filters_locked();
    start_interval_locked(*ui);
  }

  std::cerr << "ECU " << ecu_id_ << ": UI connected (fd=" << fd << ")\n";
  auto msg = status();
  msg["type"] = "registered";
  send_to_fd(fd, msg);
}

Ecu::UiClient* Ecu::ui_by_fd(int fd) {
  auto it = ui_clients_.find(fd);
  return it == ui_clients_.end() ? nullptr : it->second.get();
}

void Ecu::set_tx_config(int fd, const nlohmann::json& profile) {
  std::thread old_thread;
  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (!ui) return;
    ui->tx = TxProfile::from_json(profile.contains("profile") ? profile["profile"] : profile);
    ui->interval_stop = true;
    if (ui->interval_thread.joinable()) old_thread = std::move(ui->interval_thread);
  }

  if (old_thread.joinable()) old_thread.join();

  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (!ui) return;
    ui->interval_stop = false;
    start_interval_locked(*ui);
  }
}

void Ecu::set_rx_config(int fd, const nlohmann::json& profile) {
  std::lock_guard lock(mu_);
  auto* ui = ui_by_fd(fd);
  if (!ui) return;
  const auto& body = profile.contains("profile") ? profile["profile"] : profile;
  ui->rx = RxProfile::from_json(body);
  sync_app_rx_filters_locked();
}

void Ecu::sync_app_rx_filters_locked() {
  std::set<uint32_t> unique;
  if (ui_clients_.empty()) {
    for (const auto& f : defaults_.rx.filters) unique.insert(f.can_id);
  } else {
    for (const auto& [fd, ui] : ui_clients_) {
      (void)fd;
      if (!ui->rx) continue;
      for (const auto& f : ui->rx->filters) unique.insert(f.can_id);
    }
  }
  std::vector<uint32_t> ids(unique.begin(), unique.end());
  can_.set_app_rx_filters(ids);
}

void Ecu::reset_bus_off() {
  can_.reset_bus_off();
}

bool Ecu::passes_condition(UiClient& ui, const nlohmann::json& payload) {
  if (!ui.tx) return true;
  const auto& cond = ui.tx->condition;
  if (cond.contains("min_value_change") && !ui.last_payload.is_null()) {
    const double old_v = ui.last_payload.value("value", 0.0);
    const double new_v = payload.value("value", 0.0);
    const double min_ch = cond["min_value_change"].get<double>();
    if (std::abs(new_v - old_v) < min_ch) return false;
  }
  return true;
}

void Ecu::on_bus_delivered(const nlohmann::json& delivered) {
  // Транслируем полную картинку (с битами/CRC/ACK/арбитражем) только в bus-monitor UI,
  // чтобы не перегружать каждое ECU-окно.
  std::lock_guard lock(mu_);
  for (auto& [fd, ui] : ui_clients_) {
    (void)fd;
    if (!bus_monitor_) continue;
    nlohmann::json payload = {{"type", "bus_delivered"},
                              {"ecu_id", ecu_id_},
                              {"data", delivered}};
    send_to_fd(ui->fd, payload);
  }
}

void Ecu::on_bus_state(const nlohmann::json& snapshot) {
  // Обычное ECU-окно видит только своё состояние; bus-monitor — все.
  const std::string nid = snapshot.value("node_id", std::string());
  const bool is_self = (nid == ecu_id_);
  std::lock_guard lock(mu_);
  for (auto& [fd, ui] : ui_clients_) {
    (void)fd;
    if (!is_self && !bus_monitor_) continue;
    nlohmann::json payload = {{"type", "controller_state"},
                              {"ecu_id", ecu_id_},
                              {"data", snapshot}};
    send_to_fd(ui->fd, payload);
  }
}

void Ecu::on_can_frame(uint32_t can_id, const std::vector<uint8_t>& data) {
  char id_hex[16];
  std::snprintf(id_hex, sizeof(id_hex), "0x%X", can_id);

  std::lock_guard lock(mu_);
  for (auto& [fd, ui] : ui_clients_) {
    (void)fd;
    if (bus_monitor_) {
      const auto monitor_payload = decode_payload(data, "float32");
      send_to_fd(ui->fd, {{"type", "bus_frame"},
                          {"from", id_hex},
                          {"via", "can"},
                          {"can_id", can_id},
                          {"ecu_id", ecu_id_},
                          {"payload", monitor_payload}});
    }

    if (!ui->rx) continue;
    const RxFilter* filter = ui->rx->match(can_id);
    if (!filter) continue;

    auto payload = decode_payload(data, filter->encoding);
    if (!filter->label.empty()) payload["label"] = filter->label;

    payload = run_hooks(ui->rx->on_receive, payload,
                        {{"phase", "on_receive"}, {"can_id", can_id}, {"ecu_id", ecu_id_}});

    send_to_fd(ui->fd, {{"type", "message"},
                        {"from", id_hex},
                        {"via", "can"},
                        {"can_id", can_id},
                        {"filter_label", filter->label},
                        {"ecu_id", ecu_id_},
                        {"payload", payload}});
  }
}

void Ecu::stop_interval(UiClient& ui) {
  ui.interval_stop = true;
  if (ui.interval_thread.joinable()) ui.interval_thread.join();
  ui.interval_stop = false;
}

void Ecu::start_interval_locked(UiClient& ui) {
  if (!ui.tx || ui.tx->schedule_mode != "interval") return;
  if (ui.interval_thread.joinable()) return;

  const int ms = ui.tx->interval_ms;
  const int fd = ui.fd;
  ui.interval_thread = std::thread([this, fd, ms]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      nlohmann::json last;
      {
        std::lock_guard lock(mu_);
        auto* u = ui_by_fd(fd);
        if (!u || u->interval_stop) break;
        if (u->last_payload.is_null()) continue;
        last = u->last_payload;
      }
      try {
        publish(fd, last);
      } catch (...) {
      }
    }
  });
}

void Ecu::restart_interval(UiClient& ui) {
  stop_interval(ui);
  start_interval_locked(ui);
}

void Ecu::stage_payload(int fd, const nlohmann::json& body) {
  nlohmann::json payload = body.contains("payload") ? body["payload"] : body;
  std::lock_guard lock(mu_);
  auto* ui = ui_by_fd(fd);
  if (!ui) throw std::runtime_error("UI client not found");
  ui->last_payload = std::move(payload);
}

void Ecu::publish(int fd, const nlohmann::json& body) {
  nlohmann::json payload = body.contains("payload") ? body["payload"] : body;

  TxProfile tx;
  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (!ui || !ui->tx) throw std::runtime_error("TX profile not configured");
    if (!passes_condition(*ui, payload)) return;
    tx = *ui->tx;
  }

  if (body.contains("can_id")) {
    if (body["can_id"].is_string()) {
      tx.can_id = static_cast<uint32_t>(
          std::strtoul(body["can_id"].get<std::string>().c_str(), nullptr, 0));
    } else {
      tx.can_id = body["can_id"].get<uint32_t>();
    }
  }

  nlohmann::json tx_payload =
      run_hooks(tx.pre_send, payload, {{"phase", "pre_send"}, {"ecu_id", ecu_id_}});

  if (!can_.is_open()) throw std::runtime_error("CAN interface not open");

  const auto data = encode_payload(tx_payload, tx.encoding);
  if (!can_.send(tx.can_id, data)) throw std::runtime_error("CAN send failed");

  char id_buf[16];
  std::snprintf(id_buf, sizeof(id_buf), "0x%X", tx.can_id);

  tx_payload = run_hooks(tx.post_send, tx_payload,
                         {{"phase", "post_send"}, {"ecu_id", ecu_id_}, {"can_id", tx.can_id}});

  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (ui) {
      ui->last_payload = tx_payload;
      send_to_fd(fd, {{"type", "sent"},
                      {"via", nlohmann::json::array({"can"})},
                      {"can_id", tx.can_id},
                      {"payload", tx_payload}});
    }
  }

  std::cerr << "ECU " << ecu_id_ << ": TX " << id_buf << " (" << data.size() << " bytes)\n";
}
