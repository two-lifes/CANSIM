#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

nlohmann::json run_hooks(const std::vector<std::string>& names, nlohmann::json payload,
                         const nlohmann::json& ctx);
