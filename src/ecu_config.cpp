#include "ecu_config.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

static nlohmann::json builtin_defaults(const std::string& ecu_id) {
  if (ecu_id == "engine") {
    return {
        {"tx", {{"can_id", "0x100"}, {"encoding", "float32"}, {"metric", "temperature_c"}}},
        {"rx", {{"filters",
                 nlohmann::json::array({{{"can_id", "0x200"},
                                         {"encoding", "float32"},
                                         {"label", "gateway_value"}},
                                        {{"can_id", "0x300"},
                                         {"encoding", "float32"},
                                         {"label", "dashboard_ack"}}})}}},
    };
  }
  if (ecu_id == "dashboard") {
    return {
        {"tx", {{"can_id", "0x300"}, {"encoding", "float32"}, {"metric", "percent"}}},
        {"rx", {{"filters",
                 nlohmann::json::array({{{"can_id", "0x100"},
                                         {"encoding", "float32"},
                                         {"label", "engine_value"}}})}}},
    };
  }
  if (ecu_id == "gateway") {
    return {
        {"tx", {{"can_id", "0x200"}, {"encoding", "float32"}, {"metric", "pressure_bar"}}},
        {"rx", {{"filters",
                 nlohmann::json::array({{{"can_id", "0x100"},
                                         {"encoding", "float32"},
                                         {"label", "engine_value"}},
                                        {{"can_id", "0x300"},
                                         {"encoding", "float32"},
                                         {"label", "dashboard_ack"}}})}}},
    };
  }
  return {
      {"tx", {{"can_id", "0x100"}, {"encoding", "float32"}, {"metric", "percent"}}},
      {"rx", {{"filters", nlohmann::json::array()}}},
  };
}

EcuDefaults EcuDefaults::load(const std::string& ecu_id, const std::string& config_dir) {
  nlohmann::json j;
  const std::vector<std::string> candidates = {
      config_dir + "/ecu.json",          // new layout: ecus/<name>/ecu.json
      config_dir + "/" + ecu_id + ".json" // legacy layout: config/<id>.json
  };

  bool loaded = false;
  for (const auto& path : candidates) {
    std::ifstream in(path);
    if (!in) continue;
    try {
      in >> j;
      loaded = true;
      std::cerr << "ECU " << ecu_id << ": config " << path << "\n";
      break;
    } catch (...) {
      std::cerr << "ECU " << ecu_id << ": failed to parse " << path << "\n";
    }
  }

  if (!loaded) {
    j = builtin_defaults(ecu_id);
  }

  EcuDefaults defs;
  if (j.contains("tx")) defs.tx = TxProfile::from_json(j["tx"]);
  if (j.contains("rx")) defs.rx = RxProfile::from_json(j["rx"]);
  return defs;
}

static std::string hex_id(uint32_t id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%X", id);
  return buf;
}

nlohmann::json EcuDefaults::to_json(int port, const std::string& page) const {
  nlohmann::json filters = nlohmann::json::array();
  for (const auto& f : rx.filters) {
    nlohmann::json item = {{"can_id", hex_id(f.can_id)}, {"encoding", f.encoding}};
    if (!f.label.empty()) item["label"] = f.label;
    filters.push_back(item);
  }
  nlohmann::json out = {{"tx", {{"can_id", hex_id(tx.can_id)}, {"encoding", tx.encoding}, {"metric", tx.metric}}},
                        {"rx", {{"filters", filters}}}};
  if (port > 0) out["port"] = port;
  if (!page.empty()) out["page"] = page;
  return out;
}
