#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

std::vector<uint8_t> encode_payload(const nlohmann::json& payload, const std::string& encoding);
nlohmann::json decode_payload(const std::vector<uint8_t>& data, const std::string& encoding);

/** Проверка кодека при старте ECU; false = не запускать сеть. */
bool encoding_self_test();
