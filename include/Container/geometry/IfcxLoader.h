#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <filesystem>
#include <stop_token>
#include <string_view>

namespace container::geometry::ifcx {

// IFC5 development examples are encoded as IFCX JSON with USD mesh attributes.
// The loader converts supported mesh payloads into the shared BIM model shape.
using Model = container::geometry::dotbim::Model;

[[nodiscard]] Model LoadFromFile(const std::filesystem::path& path,
                                 float importScale = 1.0f,
                                 std::stop_token stop = {});
[[nodiscard]] Model LoadFromJson(std::string_view jsonText,
                                 float importScale = 1.0f,
                                 std::stop_token stop = {});

}  // namespace container::geometry::ifcx
