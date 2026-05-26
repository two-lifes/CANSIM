#include "profiles.hpp"

#include <cstdlib>

using json = nlohmann::json;

static uint32_t parse_can_id_value(const json& v, uint32_t def) {
  if (v.is_string()) {
    return static_cast<uint32_t>(std::strtoul(v.get<std::string>().c_str(), nullptr, 0));
  }
  if (v.is_number_unsigned()) return v.get<uint32_t>();
  if (v.is_number_integer()) return static_cast<uint32_t>(v.get<int>());
  return def;
}

static uint32_t parse_can_id(const json& j, const char* key, uint32_t def) {
  if (!j.contains(key)) return def;
  return parse_can_id_value(j.at(key), def);
}

static std::vector<std::string> parse_str_list(const json& j, const char* key) {
  std::vector<std::string> out;
  if (!j.contains(key) || !j.at(key).is_array()) return out;
  for (const auto& item : j.at(key)) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

RxFilter RxFilter::from_json(const json& j) {
  RxFilter f;
  f.can_id = parse_can_id(j, "can_id", 0);
  if (j.contains("encoding")) f.encoding = j["encoding"].get<std::string>();
  if (j.contains("label")) f.label = j["label"].get<std::string>();
  return f;
}

RxProfile RxProfile::from_json(const json& j) {
  RxProfile profile;
  profile.on_receive = parse_str_list(j, "on_receive");

  if (j.contains("filters") && j["filters"].is_array()) {
    for (const auto& item : j["filters"]) {
      profile.filters.push_back(RxFilter::from_json(item));
    }
    return profile;
  }

  // Legacy: один can_id + encoding
  if (j.contains("can_id")) {
    RxFilter f;
    f.can_id = parse_can_id(j, "can_id", 0x100);
    if (j.contains("encoding")) f.encoding = j["encoding"].get<std::string>();
    profile.filters.push_back(f);
  }

  return profile;
}

const RxFilter* RxProfile::match(uint32_t can_id) const {
  for (const auto& f : filters) {
    if (f.can_id == can_id) return &f;
  }
  return nullptr;
}

TxProfile TxProfile::from_json(const json& j) {
  TxProfile p;
  if (j.contains("can_interface")) p.can_interface = j["can_interface"].get<std::string>();
  p.can_id = parse_can_id(j, "can_id", 0x100);
  if (j.contains("encoding")) p.encoding = j["encoding"].get<std::string>();
  if (j.contains("schedule") && j["schedule"].is_object()) {
    const auto& s = j["schedule"];
    if (s.contains("mode")) p.schedule_mode = s["mode"].get<std::string>();
    if (s.contains("interval_ms")) p.interval_ms = s["interval_ms"].get<int>();
  } else if (j.contains("schedule_mode")) {
    p.schedule_mode = j["schedule_mode"].get<std::string>();
  }
  if (j.contains("interval_ms")) p.interval_ms = j["interval_ms"].get<int>();
  if (j.contains("condition")) p.condition = j["condition"];
  p.pre_send = parse_str_list(j, "pre_send");
  p.post_send = parse_str_list(j, "post_send");
  return p;
}
