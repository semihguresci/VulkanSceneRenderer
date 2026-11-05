#pragma once

#include "Container/renderer/effects/DisplayGrade.h"

#include <filesystem>
#include <string_view>

namespace container::renderer {

[[nodiscard]] ToneMapper parseToneMapper(std::string_view name);
[[nodiscard]] DisplayGrade loadDisplayGrade(const std::filesystem::path &path);
void saveDisplayGrade(const std::filesystem::path &path, const DisplayGrade &grade);

} // namespace container::renderer
