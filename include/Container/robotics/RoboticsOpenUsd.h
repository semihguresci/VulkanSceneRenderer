#pragma once

#include "Container/robotics/RoboticsScene.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace container::robotics {

struct RoboticsOpenUsdResult {
  size_t entityCount{};
  size_t meshCount{};
  size_t colliderCount{};
  std::vector<RoboticsDiagnostic> diagnostics;
};

// Exports a standalone Z-up, metre-based USDA stage from the CPU importer model.
// The source must provide metric units or an explicit metres-per-unit override.
// Invalid input or a failed write leaves any existing destination intact.
[[nodiscard]] RoboticsOpenUsdResult writeRoboticsOpenUsd(
    const std::filesystem::path& output, const geometry::dotbim::Model& model,
    const RoboticsSceneOptions& options);

} // namespace container::robotics
