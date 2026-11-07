#pragma once

#include "Container/renderer/lighting/SkySettings.h"

#include <filesystem>
#include <string_view>

namespace container::renderer {

[[nodiscard]] SkyMode parseProceduralSkyMode(std::string_view name);
[[nodiscard]] SkySettings loadSkyPreset(const std::filesystem::path& path);
void saveSkyPreset(const std::filesystem::path& path, const SkySettings& settings);

} // namespace container::renderer
