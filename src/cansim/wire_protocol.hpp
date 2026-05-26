#pragma once

#include <nlohmann/json.hpp>

#include "bus.hpp"
#include "controller.hpp"
#include "frame.hpp"

namespace cansim {

/** Стандартный путь UDS-сокета bus-демона. */
inline const char* default_socket_path() { return "/tmp/linuxcan-bus.sock"; }

nlohmann::json to_json(const ControllerSnapshot& s);
nlohmann::json to_json(const DeliveredFrame& f);
nlohmann::json to_json(const CanFrame& f);
nlohmann::json to_json(const ErrorReport& e);

CanFrame frame_from_json(const nlohmann::json& j);

}  // namespace cansim
