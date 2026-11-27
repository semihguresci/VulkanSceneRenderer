#pragma once

#include "Container/robotics/RoboticsDataset.h"

namespace container::robotics {

// Validates staged files against a proposed completion manifest. dataset.json
// need not exist yet. The public reader loads that marker before delegating.
[[nodiscard]] RoboticsDatasetSummary validateRoboticsDatasetFiles(
    const std::filesystem::path& directory, nlohmann::json manifest,
    const RoboticsDatasetObserver& observe = {});

} // namespace container::robotics
