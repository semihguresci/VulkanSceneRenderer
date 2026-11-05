#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::util {

// Presets use a single JSON object with scalar fields and fixed-size arrays.
inline nlohmann::json readPresetJson(const std::filesystem::path &path,
                                     std::string_view label) {
  const auto invalid = [&](std::string_view reason) {
    return std::invalid_argument(std::string(label) + ": " + std::string(reason));
  };
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw invalid("could not open the file");
  std::array<char, 4097> bytes{};
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (file.bad() || !file.eof() || file.gcount() <= 0)
    throw invalid("file must contain 1 to 4096 readable bytes");
  using Json = nlohmann::json;
  std::set<std::string> keys;
  const auto checkStructure = [&](int depth, Json::parse_event_t event, Json &value) {
    if (depth > 2 || (event == Json::parse_event_t::object_start && depth != 0))
      throw invalid("unexpected nested value");
    if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
      throw invalid("duplicate field");
    return true;
  };
  return Json::parse(bytes.data(), bytes.data() + file.gcount(), checkStructure);
}

} // namespace container::util
