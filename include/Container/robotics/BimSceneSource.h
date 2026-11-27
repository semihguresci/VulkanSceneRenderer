#pragma once

#include "Container/utility/ContentFingerprint.h"

#include <vector>

namespace container::robotics {

struct RoboticsFrame;

struct BimSceneSource {
  // Content identity in importer consultation order. Paths do not participate.
  std::vector<util::ContentFingerprint> layers;
  double metersPerUnscaledRendererUnit{};
};

[[nodiscard]] BimSceneSource makeBimSceneSource(
    std::vector<util::ContentFingerprint> layers, const RoboticsFrame& frame);

} // namespace container::robotics
