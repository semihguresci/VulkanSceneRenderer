#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace container::robotics {

struct RoboticsDatasetWriteOptions {
  uint32_t ticksPerChunk{64};
};

void writeRoboticsDataset(const std::filesystem::path& cameraDirectory,
                         const std::filesystem::path& simulationReport,
                         const std::filesystem::path& outputDirectory,
                         const RoboticsDatasetWriteOptions& options = {});

struct RoboticsDatasetSummary {
  nlohmann::json manifest;
  nlohmann::json labels;
};

using RoboticsDatasetObserver = std::function<void(const nlohmann::json&)>;

// Streams one validated tick row at a time, retaining only the two manifests.
// Camera image names are portable relative names resolved against directory.
// A successful return validates the entire completed dataset. An observer can
// already have received earlier rows when a later row fails validation.
// Version 1 images use single-part, uncompressed, increasing-Y scanline EXR,
// with FLOAT R/G/B/Z and UINT InstanceID/SemanticID at full pixel sampling.
[[nodiscard]] RoboticsDatasetSummary readRoboticsDataset(
    const std::filesystem::path& directory, const RoboticsDatasetObserver& observe = {});

} // namespace container::robotics
