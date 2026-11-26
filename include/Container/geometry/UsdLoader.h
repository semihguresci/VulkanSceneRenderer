#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <filesystem>
#include <stop_token>
#include <string_view>

namespace container::geometry::usd {

// Cancellation is checked between TinyUSDZ calls and while processing their
// results. TinyUSDZ parsing, composition, and render-scene conversion each run
// synchronously until the current call returns.
[[nodiscard]] dotbim::Model LoadFromFile(const std::filesystem::path& path,
                                         float importScale = 1.0f,
                                         std::stop_token stop = {});
[[nodiscard]] dotbim::Model LoadFromText(std::string_view usdText,
                                         float importScale = 1.0f,
                                         std::stop_token stop = {});

}  // namespace container::geometry::usd
