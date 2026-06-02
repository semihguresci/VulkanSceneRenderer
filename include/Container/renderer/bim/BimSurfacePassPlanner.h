#pragma once

#include "Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h"

#include <array>
#include <cstdint>

namespace container::renderer {

enum class BimSurfacePassKind : uint32_t {
  DepthPrepass = 0,
  GBuffer = 1,
  OpaqueLighting = 2,
  TransparentPick = 3,
  TransparentLighting = 4,
};

enum class BimSurfacePassSourceKind : uint32_t {
  Mesh = 0,
  PointPlaceholders = 1,
  CurvePlaceholders = 2,
};

struct BimSurfacePassSource {
  BimSurfacePassSourceKind source{BimSurfacePassSourceKind::Mesh};
  BimSurfaceDrawLists draws{};
  bool gpuCompactionEligible{false};
  bool gpuVisibilityOwnsCpuFallback{false};
};

struct BimSurfacePassInputs {
  BimSurfacePassKind kind{BimSurfacePassKind::DepthPrepass};
  bool passReady{false};
  bool geometryReady{false};
  bool descriptorSetReady{false};
  bool bindlessPushConstantsReady{false};
  bool basePipelineReady{false};
  uint32_t semanticColorMode{0};
  std::array<BimSurfacePassSource, 3> sources{};
  uint32_t sourceCount{0};
};

struct BimSurfacePassSourcePlan {
  BimSurfacePassSourceKind source{BimSurfacePassSourceKind::Mesh};
  std::array<BimSurfaceDrawRoute, 3> routes{};
  uint32_t routeCount{0};
};

struct BimSurfacePassPlan {
  bool active{false};
  bool writesSemanticColorMode{false};
  uint32_t semanticColorMode{0};
  std::array<BimSurfacePassSourcePlan, 3> sources{};
  uint32_t sourceCount{0};
};

class BimSurfacePassPlanner {
public:
  explicit BimSurfacePassPlanner(BimSurfacePassInputs inputs);

  [[nodiscard]] BimSurfacePassPlan build() const;

private:
  BimSurfacePassInputs inputs_{};
};

[[nodiscard]] BimSurfacePassPlan
buildBimSurfacePassPlan(const BimSurfacePassInputs &inputs);

} // namespace container::renderer
