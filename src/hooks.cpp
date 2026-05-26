#include "hooks.hpp"

#include <chrono>
#include <iostream>

nlohmann::json run_hooks(const std::vector<std::string>& names, nlohmann::json payload,
                         const nlohmann::json& ctx) {
  for (const auto& name : names) {
    if (name == "timestamp") {
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const double sec =
          std::chrono::duration<double>(now).count();
      payload["timestamp"] = sec;
    } else if (name == "log") {
      std::cerr << "[hook] phase=" << ctx.value("phase", "")
                << " payload=" << payload.dump() << std::endl;
    }
  }
  return payload;
}
