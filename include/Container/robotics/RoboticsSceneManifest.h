#pragma once

#include "Container/robotics/RoboticsScene.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace container::robotics {

[[nodiscard]] std::string serializeRoboticsScene(const RoboticsScene& scene);
[[nodiscard]] RoboticsScene parseRoboticsScene(std::string_view manifest);
void writeRoboticsSceneManifest(const std::filesystem::path& path, const RoboticsScene& scene);
[[nodiscard]] RoboticsScene readRoboticsSceneManifest(const std::filesystem::path& path);

}  // namespace container::robotics
