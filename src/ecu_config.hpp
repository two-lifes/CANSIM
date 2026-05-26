#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "profiles.hpp"

/** Дефолты ECU из ecus/<name>/ecu.json (или legacy config/<id>.json). */
struct EcuDefaults {
  TxProfile tx;
  RxProfile rx;

  static EcuDefaults load(const std::string& ecu_id, const std::string& config_dir);
  nlohmann::json to_json(int port = 0, const std::string& page = "") const;
};
