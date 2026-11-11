#pragma once

#include "Container/utility/ModelImportCancellation.h"

#include <array>
#include <istream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace container::geometry::bim_import {

inline std::string readText(std::istream &input, std::stop_token stop) {
  std::array<char, 64 * 1024> buffer;
  std::string text;
  do {
    container::util::checkModelImportCancellation(stop);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    text.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
  } while (input);
  container::util::checkModelImportCancellation(stop);
  if (input.bad() || !input.eof()) {
    throw std::runtime_error("failed to read BIM input");
  }
  return text;
}

inline nlohmann::json parseJson(std::string_view text, std::stop_token stop) {
  using Json = nlohmann::json;
  container::util::checkModelImportCancellation(stop);
  Json::parser_callback_t checkpoint;
  if (stop.stop_possible()) {
    checkpoint = [stop, events = 0u](int, Json::parse_event_t, Json &) mutable {
      if ((events++ % 512u) == 0u) {
        container::util::checkModelImportCancellation(stop);
      }
      return true;
    };
  }
  auto result = Json::parse(text.begin(), text.end(), std::move(checkpoint));
  container::util::checkModelImportCancellation(stop);
  return result;
}

} // namespace container::geometry::bim_import
