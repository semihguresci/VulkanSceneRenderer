#pragma once

#include <filesystem>
#include <span>

namespace container::tools {

int runTwinIngest(std::span<const std::filesystem::path> arguments);

} // namespace container::tools
