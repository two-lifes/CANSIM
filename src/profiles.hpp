#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

/** Один фильтр приёма: «слушай кадры с этого CAN ID». */
struct RxFilter {
  uint32_t can_id = 0;
  std::string encoding = "float32";
  std::string label;  // опционально, для UI / логики приложения

  static RxFilter from_json(const nlohmann::json& j);
};

/** Профиль приёма: список фильтров (как маски/подписки в реальном ECU). */
struct RxProfile {
  std::vector<RxFilter> filters;
  std::vector<std::string> on_receive;

  static RxProfile from_json(const nlohmann::json& j);
  const RxFilter* match(uint32_t can_id) const;
};

/** Профиль передачи: на какой ID и в каком формате слать кадр. */
struct TxProfile {
  std::string can_interface = "vcan0";
  uint32_t can_id = 0x100;
  std::string encoding = "float32";
  std::string metric = "percent";
  std::string schedule_mode = "manual";
  int interval_ms = 1000;
  nlohmann::json condition = nlohmann::json::object();
  std::vector<std::string> pre_send;
  std::vector<std::string> post_send;

  static TxProfile from_json(const nlohmann::json& j);
};

// Совместимость со старым API WebSocket
using SendProfile = TxProfile;
