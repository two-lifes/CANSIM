#include "encoding.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

void append_float32(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  out.push_back(static_cast<uint8_t>(bits & 0xff));
  out.push_back(static_cast<uint8_t>((bits >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((bits >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((bits >> 24) & 0xff));
}

float read_float32(const std::vector<uint8_t>& data) {
  uint32_t bits = 0;
  for (int i = 0; i < 4 && i < static_cast<int>(data.size()); ++i) {
    bits |= static_cast<uint32_t>(data[i]) << (8 * i);
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::vector<uint8_t> pad8(std::vector<uint8_t> bytes) {
  bytes.resize(8, 0);
  return bytes;
}

}  // namespace

std::vector<uint8_t> encode_payload(const nlohmann::json& payload,
                                    const std::string& encoding) {
  if (encoding == "float32") {
    float value = 0.f;
    if (payload.contains("value")) {
      if (payload["value"].is_number_float()) value = payload["value"].get<float>();
      else if (payload["value"].is_number_integer())
        value = static_cast<float>(payload["value"].get<int>());
    }
    std::vector<uint8_t> out;
    append_float32(out, value);
    return pad8(std::move(out));
  }

  if (encoding == "text") {
    std::string text;
    if (payload.contains("text")) text = payload["text"].get<std::string>();
    else if (payload.contains("value")) text = payload["value"].dump();
    std::vector<uint8_t> out(text.begin(), text.end());
    return pad8(std::move(out));
  }

  const std::string raw = payload.dump();
  std::vector<uint8_t> out(raw.begin(), raw.end());
  if (out.size() > 8) out.resize(8);
  return pad8(std::move(out));
}

nlohmann::json decode_payload(const std::vector<uint8_t>& data_in,
                              const std::string& encoding) {
  std::vector<uint8_t> data = data_in;
  if (data.size() > 8) data.resize(8);
  data.resize(8, 0);

  auto hex = [&data]() {
    static const char* kHex = "0123456789abcdef";
    std::string h;
    for (uint8_t b : data) {
      h.push_back(kHex[b >> 4]);
      h.push_back(kHex[b & 0xf]);
    }
    return h;
  };

  if (encoding == "float32") {
    return {{"type", "answer"}, {"value", read_float32(data)}, {"raw_hex", hex()}};
  }

  if (encoding == "text") {
    std::string text;
    for (uint8_t b : data) {
      if (b == 0) break;
      text.push_back(static_cast<char>(b));
    }
    return {{"type", "text"}, {"text", text}, {"raw_hex", hex()}};
  }

  std::string text;
  for (uint8_t b : data) {
    if (b == 0) break;
    text.push_back(static_cast<char>(b));
  }
  try {
    auto parsed = nlohmann::json::parse(text);
    if (parsed.is_object()) {
      parsed["raw_hex"] = hex();
      return parsed;
    }
  } catch (...) {
  }
  return {{"type", "raw"}, {"text", text}, {"raw_hex", hex()}};
}

bool encoding_self_test() {
  const nlohmann::json sample = {{"value", 63}};
  const auto bytes = encode_payload(sample, "float32");
  if (bytes.size() != 8) return false;
  if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) return false;

  const auto decoded = decode_payload(bytes, "float32");
  if (!decoded.contains("value")) return false;
  const double v = decoded["value"].get<double>();
  return v > 62.9 && v < 63.1;
}
