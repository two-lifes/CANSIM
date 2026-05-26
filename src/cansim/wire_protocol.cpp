#include "wire_protocol.hpp"

#include <cstdlib>

namespace cansim {

using nlohmann::json;

static uint32_t parse_id_field(const json& v) {
  if (v.is_string()) {
    return static_cast<uint32_t>(std::strtoul(v.get<std::string>().c_str(), nullptr, 0));
  }
  if (v.is_number_unsigned()) return v.get<uint32_t>();
  if (v.is_number_integer()) return static_cast<uint32_t>(v.get<int>());
  return 0;
}

CanFrame frame_from_json(const json& j) {
  CanFrame f;
  if (j.contains("id")) f.id = parse_id_field(j.at("id"));
  else if (j.contains("can_id")) f.id = parse_id_field(j.at("can_id"));
  if (j.contains("ide")) f.extended = j["ide"].get<bool>();
  else if (j.contains("extended")) f.extended = j["extended"].get<bool>();
  if (j.contains("rtr")) f.rtr = j["rtr"].get<bool>();
  if (j.contains("dlc")) f.dlc = static_cast<uint8_t>(j["dlc"].get<int>() & 0x0F);
  if (j.contains("data") && j["data"].is_array()) {
    for (const auto& b : j["data"]) {
      f.data.push_back(static_cast<uint8_t>(b.get<int>() & 0xFF));
    }
    if (f.dlc == 0) f.dlc = static_cast<uint8_t>(f.data.size());
  }
  return f;
}

json to_json(const CanFrame& f) {
  json arr = json::array();
  for (uint8_t b : f.data) arr.push_back(static_cast<int>(b));
  return {
      {"id", f.id},
      {"ide", f.extended},
      {"rtr", f.rtr},
      {"dlc", f.dlc},
      {"data", arr},
  };
}

json to_json(const ControllerSnapshot& s) {
  return {
      {"node_id", s.id},
      {"tec", s.tec},
      {"rec", s.rec},
      {"mode", s.mode},
      {"pending", s.pending},
      {"stats", {
                    {"tx_ok", s.stats.tx_ok},
                    {"rx_ok", s.stats.rx_ok},
                    {"tx_errors", s.stats.tx_errors},
                    {"rx_errors", s.stats.rx_errors},
                    {"arbitration_lost", s.stats.arbitration_lost},
                    {"error_frames_tx", s.stats.error_frames_tx},
                    {"error_frames_rx", s.stats.error_frames_rx},
                    {"bus_off_events", s.stats.bus_off_events},
                    {"app_rx_ok", s.stats.app_rx_ok},
                }},
  };
}

json to_json(const ErrorReport& e) {
  return {
      {"kind", e.kind},
      {"at_bit", e.at_bit},
      {"detected_by", e.detected_by},
  };
}

json to_json(const DeliveredFrame& f) {
  json errors = json::array();
  for (const auto& e : f.errors) errors.push_back(to_json(e));

  json states = json::array();
  for (const auto& s : f.states) states.push_back(to_json(s));

  json arbitration = json::array();
  for (const auto& step : f.arbitration) {
    json dropped = json::array();
    for (const auto& d : step.dropped) dropped.push_back(d);
    arbitration.push_back({
        {"bit_index", step.bit_index},
        {"bus_bit", std::string(1, step.bus_bit)},
        {"dropped", dropped},
    });
  }

  json stuff_pos = json::array();
  for (size_t p : f.stuff_positions) stuff_pos.push_back(p);

  json losers = json::array();
  for (const auto& l : f.losers) losers.push_back(l);
  json acked = json::array();
  for (const auto& a : f.acked_by) acked.push_back(a);
  json app_rx = json::array();
  for (const auto& a : f.app_receivers) app_rx.push_back(a);

  return {
      {"seq", f.seq},
      {"bus_time_us", f.bus_time_us},
      {"frame", to_json(f.frame)},
      {"winner", f.winner},
      {"losers", losers},
      {"acked_by", acked},
      {"app_receivers", app_rx},
      {"unstuffed", f.unstuffed},
      {"stuffed", f.stuffed},
      {"trailer", f.trailer},
      {"stuff_positions", stuff_pos},
      {"arbitration_begin", f.arbitration_begin},
      {"arbitration_end", f.arbitration_end},
      {"crc", f.crc},
      {"ack", f.ack},
      {"errors", errors},
      {"arbitration", arbitration},
      {"states", states},
      {"retries", f.retries_for_winner},
      {"ok", f.ok},
  };
}

}  // namespace cansim
