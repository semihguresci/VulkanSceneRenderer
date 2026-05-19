#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class SceneOpaqueDrawRouteKind : uint32_t {
  GpuIndirectSingleSided = 0,
  CpuSingleSided = 1,
  CpuWindingFlipped = 2,
  CpuDoubleSided = 3,
};

enum class SceneOpaqueDrawPipeline : uint32_t {
  Primary = 0,
  FrontCull = 1,
  NoCull = 2,
};

enum class SceneOpaqueIndirectSource : uint32_t {
  FrustumCull = 0,
  OcclusionCull = 1,
};

struct SceneOpaqueDrawLists {
  const std::vector<DrawCommand> *aggregate{nullptr};
  const std::vector<DrawCommand> *singleSided{nullptr};
  const std::vector<DrawCommand> *windingFlipped{nullptr};
  const std::vector<DrawCommand> *doubleSided{nullptr};
};

struct SceneOpaqueDrawInputs {
  bool gpuIndirectAvailable{false};
  bool occludedGpuIndirectAvailable{false};
  bool preferOccludedGpuIndirect{false};
  SceneOpaqueDrawLists draws{};
};

struct SceneOpaqueDrawRoute {
  SceneOpaqueDrawRouteKind kind{SceneOpaqueDrawRouteKind::CpuSingleSided};
  SceneOpaqueDrawPipeline pipeline{SceneOpaqueDrawPipeline::Primary};
  SceneOpaqueIndirectSource indirectSource{
      SceneOpaqueIndirectSource::FrustumCull};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct SceneOpaqueDrawPlan {
  bool useGpuIndirectSingleSided{false};
  SceneOpaqueDrawRoute gpuIndirectRoute{
      .kind = SceneOpaqueDrawRouteKind::GpuIndirectSingleSided,
      .pipeline = SceneOpaqueDrawPipeline::Primary,
      .indirectSource = SceneOpaqueIndirectSource::FrustumCull,
      .commands = nullptr};
  std::array<SceneOpaqueDrawRoute, 3> cpuRoutes{};
  uint32_t cpuRouteCount{0};
};

class SceneOpaqueDrawPlanner {
public:
  explicit SceneOpaqueDrawPlanner(SceneOpaqueDrawInputs inputs);

  [[nodiscard]] SceneOpaqueDrawPlan build() const;

private:
  SceneOpaqueDrawInputs inputs_{};
};

[[nodiscard]] SceneOpaqueDrawPlan
buildSceneOpaqueDrawPlan(const SceneOpaqueDrawInputs &inputs);

} // namespace container::renderer
