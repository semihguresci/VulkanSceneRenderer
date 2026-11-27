#pragma once

#include "Container/robotics/UrdfRuntime.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace container::robotics {

struct UrdfAssetScenario {
  UrdfPose worldFromRoot;
  std::span<const UrdfJointValue> positions;
  std::span<const UrdfJointValue> targets;
  uint64_t ticks{};
};

struct UrdfAssetExportResult {
  size_t visualCount{};
  size_t collisionCount{};
  size_t keyframeCount{};
  std::vector<std::string> diagnostics;
};

// Write a portable GLB with the URDF link hierarchy, visual and collision
// scenes, and an optional joint-motion clip sampled by UrdfRuntime. Relative
// assets resolve beside the URDF. Each package root contains named packages.
// A failed validation or write preserves the existing output.
[[nodiscard]] UrdfAssetExportResult writeUrdfAsset(const std::filesystem::path& output,
    const std::filesystem::path& urdfPath, const UrdfModel& model,
    const UrdfAssetScenario& scenario,
    std::span<const std::filesystem::path> packageRoots = {});

} // namespace container::robotics
