#include "ecu.hpp"

#include "encoding.hpp"
#include "hooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
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
          {"metric", tx.metric},
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

double json_number(const nlohmann::json& j, const char* key, double fallback) {
  if (!j.contains(key) || !j.at(key).is_number()) return fallback;
  return j.at(key).get<double>();
}

double clamp(double value, double lo, double hi) {
  return std::max(lo, std::min(hi, value));
}

double random_uniform(std::mt19937& rng, double lo, double hi) {
  std::uniform_real_distribution<double> dist(lo, hi);
  return dist(rng);
}

template <typename RuntimeSensorT>
double tick_runtime_sensor(RuntimeSensorT& sensor, double dt_ms, std::mt19937& rng) {
  const auto& s = sensor.settings;
  const std::string& type = sensor.type_id;

  if (type == "temperature") {
    const double min = json_number(s, "min", 20.0);
    const double max = std::max(min, json_number(s, "max", 80.0));
    const double period_ms = std::max(200.0, json_number(s, "periodMs", 8000.0));
    const double noise = std::max(0.0, json_number(s, "noise", 0.5));
    if (!sensor.init) {
      sensor.s0 = 0.0;  // phase
      sensor.init = true;
    }
    sensor.s0 += (dt_ms / period_ms) * (2.0 * M_PI);
    const double base = (min + max) * 0.5 + ((max - min) * 0.5) * std::sin(sensor.s0);
    return base + random_uniform(rng, -noise, noise);
  }

  if (type == "pressure") {
    const double mean = json_number(s, "mean", 2.0);
    const double sigma = std::max(0.0, json_number(s, "sigma", 0.15));
    const double clamp_min = json_number(s, "clampMin", 0.0);
    const double clamp_max = std::max(clamp_min, json_number(s, "clampMax", 5.0));
    if (!sensor.init) {
      sensor.s0 = mean;  // value
      sensor.init = true;
    }
    const double next = sensor.s0 + random_uniform(rng, -0.5, 0.5) * sigma * 0.4 + (mean - sensor.s0) * 0.05;
    sensor.s0 = clamp(next, clamp_min, clamp_max);
    return sensor.s0;
  }

  if (type == "speed") {
    const double idle = json_number(s, "idle", 800.0);
    const double max = std::max(idle, json_number(s, "max", 6500.0));
    const double throttle = clamp(json_number(s, "throttle", 0.5), 0.0, 1.0);
    const double accel = std::max(0.0, json_number(s, "accelRpmPerSec", 600.0));
    const double decel = std::max(0.0, json_number(s, "decelRpmPerSec", 400.0));
    const double wobble = std::max(0.0, json_number(s, "wobbleRpm", 80.0));
    const double noise = std::max(0.0, json_number(s, "noiseRpm", 15.0));
    const double dt = std::max(0.001, dt_ms / 1000.0);
    if (!sensor.init) {
      sensor.s0 = idle;  // base value
      sensor.s1 = 0.0;   // phase
      sensor.init = true;
    }
    const double target = idle + (max - idle) * throttle;
    sensor.s0 = sensor.s0 < target ? std::min(target, sensor.s0 + accel * dt)
                                   : std::max(target, sensor.s0 - decel * dt);
    sensor.s1 += dt * (0.6 + throttle * 1.8);
    const double wobble_value = std::sin(sensor.s1 * 2.0 * M_PI) * wobble * (0.25 + throttle * 0.75);
    const double noise_value = random_uniform(rng, -noise, noise);
    return clamp(sensor.s0 + wobble_value + noise_value, idle, max);
  }

  if (type == "digital") {
    return json_number(s, "value", 0.0) > 0.5 ? 1.0 : 0.0;
  }

  if (type == "constant") {
    return json_number(s, "value", 1.0);
  }

  return 0.0;
}

double aggregate_values(const std::vector<double>& values, const std::string& formula) {
  if (values.empty()) return 0.0;
  if (formula == "sum") {
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum;
  }
  if (formula == "min") {
    return *std::min_element(values.begin(), values.end());
  }
  if (formula == "max") {
    return *std::max_element(values.begin(), values.end());
  }
  if (formula == "first") return values.front();
  double sum = 0.0;
  for (double v : values) sum += v;
  return sum / static_cast<double>(values.size());
}

}  // namespace

Ecu::Ecu(std::string ecu_id, std::string can_interface)
    : ecu_id_(std::move(ecu_id)), can_(ecu_id_, std::move(can_interface)) {}

Ecu::~Ecu() { stop(); }

void Ecu::set_defaults(EcuDefaults defaults) {
  defaults_ = std::move(defaults);
  active_tx_ = defaults_.tx;
}

void Ecu::set_config_dir(std::string path) { config_dir_ = std::move(path); }

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
  {
    std::lock_guard lock(mu_);
    active_tx_ = defaults_.tx;
    sync_app_rx_filters_locked();
    load_runtime_model();
    start_process_interval_locked();
  }
}

void Ecu::stop() {
  stop_process_interval();
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
    j["tx"] = tx_to_json(active_tx_);
    for (const auto& [fd, ui] : ui_clients_) {
      (void)fd;
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
  std::lock_guard lock(mu_);
  ui_clients_.erase(fd);
  sync_app_rx_filters_locked();
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
  {
    std::lock_guard lock(mu_);
    auto* ui = ui_by_fd(fd);
    if (!ui) return;
    const auto tx = TxProfile::from_json(profile.contains("profile") ? profile["profile"] : profile);
    ui->tx = tx;
    active_tx_ = tx;
    save_runtime_model();
  }
  restart_process_interval();
}

void Ecu::set_rx_config(int fd, const nlohmann::json& profile) {
  std::lock_guard lock(mu_);
  auto* ui = ui_by_fd(fd);
  if (!ui) return;
  const auto& body = profile.contains("profile") ? profile["profile"] : profile;
  ui->rx = RxProfile::from_json(body);
  sync_app_rx_filters_locked();
}

void Ecu::apply_runtime_model(const nlohmann::json& model) {
  runtime_formula_ = model.value("formula", std::string("avg"));
  runtime_custom_expr_ = model.value("customExpr", std::string());
  runtime_scale_ = model.value("scale", 1.0);
  runtime_offset_ = model.value("offset", 0.0);
  runtime_sensors_.clear();

  for (const auto& item : model.value("sensors", nlohmann::json::array())) {
    if (!item.is_object()) continue;
    RuntimeSensor rs;
    rs.type_id = item.value("typeId", std::string());
    rs.settings = item.value("settings", nlohmann::json::object());
    if (rs.type_id.empty()) continue;
    runtime_sensors_.push_back(std::move(rs));
  }
  runtime_enabled_ = !runtime_sensors_.empty();
  runtime_last_tick_ = {};
}

void Ecu::load_runtime_model() {
  if (config_dir_.empty()) return;
  const std::string path = config_dir_ + "/runtime.json";
  std::ifstream in(path);
  if (!in) return;
  try {
    nlohmann::json j;
    in >> j;
    apply_runtime_model(j);
    if (j.contains("tx")) active_tx_ = TxProfile::from_json(j["tx"]);
  } catch (const std::exception& e) {
    std::cerr << "ECU " << ecu_id_ << ": failed to load runtime.json: " << e.what() << "\n";
  }
}

void Ecu::save_runtime_model() {
  if (config_dir_.empty()) return;
  nlohmann::json j;
  j["formula"] = runtime_formula_;
  j["customExpr"] = runtime_custom_expr_;
  j["scale"] = runtime_scale_;
  j["offset"] = runtime_offset_;
  j["sensors"] = nlohmann::json::array();
  for (const auto& rs : runtime_sensors_) {
    j["sensors"].push_back({{"typeId", rs.type_id}, {"settings", rs.settings}});
  }
  j["tx"] = tx_to_json(active_tx_);

  const std::string path = config_dir_ + "/runtime.json";
  std::ofstream out(path);
  if (!out) {
    std::cerr << "ECU " << ecu_id_ << ": failed to write runtime.json\n";
    return;
  }
  out << j.dump(2);
}

void Ecu::set_runtime_model(int fd, const nlohmann::json& model) {
  std::lock_guard lock(mu_);
  if (!ui_by_fd(fd)) return;
  apply_runtime_model(model);
  save_runtime_model();
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

bool Ecu::passes_condition(const nlohmann::json& payload) {
  const auto& cond = active_tx_.condition;
  if (cond.contains("min_value_change") && !last_payload_.is_null()) {
    const double old_v = last_payload_.value("value", 0.0);
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

void Ecu::stop_process_interval() {
  interval_stop_.store(true);
  if (interval_thread_.joinable()) interval_thread_.join();
  interval_stop_.store(false);
}

void Ecu::restart_process_interval() {
  stop_process_interval();
  std::lock_guard lock(mu_);
  start_process_interval_locked();
}

void Ecu::start_process_interval_locked() {
  if (active_tx_.schedule_mode != "interval") return;
  if (interval_thread_.joinable()) return;

  const int ms = active_tx_.interval_ms;
  interval_thread_ = std::thread([this, ms]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      nlohmann::json last;
      {
        std::lock_guard lock(mu_);
        if (interval_stop_.load()) break;
        if (runtime_enabled_) {
          last = make_runtime_payload(ms);
        } else {
          if (last_payload_.is_null()) continue;
          last = last_payload_;
        }
      }
      try {
        publish_to_bus(last, -1);
      } catch (...) {
      }
    }
  });
}

nlohmann::json Ecu::make_runtime_payload(int interval_ms) {
  const auto now = std::chrono::steady_clock::now();
  double dt_ms = static_cast<double>(interval_ms);
  if (runtime_last_tick_ != std::chrono::steady_clock::time_point{}) {
    dt_ms = std::chrono::duration<double, std::milli>(now - runtime_last_tick_).count();
  }
  runtime_last_tick_ = now;

  std::vector<double> values;
  values.reserve(runtime_sensors_.size());
  for (auto& sensor : runtime_sensors_) {
    values.push_back(tick_runtime_sensor(sensor, dt_ms, rng_));
  }

  double computed = aggregate_values(values, runtime_formula_);
  if (runtime_formula_ == "custom") {
    computed = aggregate_values(values, "avg");
  }
  const double tx = computed * runtime_scale_ + runtime_offset_;
  return nlohmann::json{{"type", "ecu"}, {"value", tx}};
}

void Ecu::stage_payload(int fd, const nlohmann::json& body) {
  nlohmann::json payload = body.contains("payload") ? body["payload"] : body;
  std::lock_guard lock(mu_);
  if (!ui_by_fd(fd)) throw std::runtime_error("UI client not found");
  last_payload_ = std::move(payload);
}

void Ecu::publish_to_bus(const nlohmann::json& body, int notify_fd) {
  nlohmann::json payload = body.contains("payload") ? body["payload"] : body;

  TxProfile tx;
  {
    std::lock_guard lock(mu_);
    if (!passes_condition(payload)) return;
    tx = active_tx_;
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
    last_payload_ = tx_payload;
    if (notify_fd >= 0 && ui_by_fd(notify_fd)) {
      send_to_fd(notify_fd, {{"type", "sent"},
                             {"via", nlohmann::json::array({"can"})},
                             {"can_id", tx.can_id},
                             {"payload", tx_payload}});
    }
  }

  std::cerr << "ECU " << ecu_id_ << ": TX " << id_buf << " (" << data.size() << " bytes)\n";
}

void Ecu::publish(int fd, const nlohmann::json& body) {
  if (!ui_by_fd(fd)) throw std::runtime_error("UI client not found");
  publish_to_bus(body, fd);
}
