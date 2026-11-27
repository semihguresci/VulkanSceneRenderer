#pragma once

#include "Container/robotics/BimSceneSource.h"
#include "Container/robotics/RoboticsScene.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace container::robotics {

struct BimSensorLabel {
  uint32_t instanceId{};
  uint32_t semanticId{};
  bool operator==(const BimSensorLabel&) const = default;
};

struct BimSensorClass {
  uint32_t semanticId{};
  std::string ifcClass;
};

struct BimSensorInstance {
  uint32_t instanceId{};
  uint32_t semanticId{};
  std::string entityId;
  std::string guid;
  std::string occurrenceId;
  std::vector<std::string> representationIds;
};

struct BimSensorLabelMap {
  std::string sourceNamespace;
  std::optional<double> metersPerRendererUnit;
  std::optional<BimSceneSource> sceneSource;
  // Indexed by the CPU model's element row, which BIM draw metadata retains
  // as sourceElementIndex. Object and pick IDs are assigned by the renderer.
  std::vector<BimSensorLabel> elements;
  std::vector<BimSensorInstance> instances;
  std::vector<BimSensorClass> classes;
  std::vector<RoboticsDiagnostic> diagnostics;
};

// Builds dense IDs per asset snapshot in stable logical occurrence order.
// Drawable representations of the same occurrence share an instance ID.
// Zero is reserved for background or unresolved rows. Class 1 is unclassified.
[[nodiscard]] BimSensorLabelMap buildBimSensorLabels(
    const geometry::dotbim::Model& model, const RoboticsScene& scene);

// Serializes the stable labels and provenance without any transient draw IDs.
[[nodiscard]] std::string serializeBimSensorLabelManifest(const BimSensorLabelMap& labels);

} // namespace container::robotics
