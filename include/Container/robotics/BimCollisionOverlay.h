#pragma once

#include <filesystem>

namespace container::robotics {

class BimCollisionWorld;
struct RoboticsFrame;

// A separate glTF debug layer in the source model's renderer coordinates.
void writeBimCollisionOverlay(const std::filesystem::path& destination,
                              const BimCollisionWorld& world, const RoboticsFrame& frame,
                              double maxSlopeDegrees);

}  // namespace container::robotics
