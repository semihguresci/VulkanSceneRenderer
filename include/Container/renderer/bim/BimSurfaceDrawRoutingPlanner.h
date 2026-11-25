#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace container::renderer {

struct DrawCommand;
enum class BimDrawCompactionSlot : uint32_t;

enum class BimSurfaceDrawKind : uint32_t {
  Opaque = 0,
  Transparent = 1,
};

enum class BimSurfaceDrawRouteKind : uint32_t {
  SingleSided = 0,
  WindingFlipped = 1,
  DoubleSided = 2,
};

struct BimSurfaceDrawLists {
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
};

struct BimSurfaceDrawRoutingInputs {
  BimSurfaceDrawKind kind{BimSurfaceDrawKind::Opaque};
  BimSurfaceDrawLists draws{};
  bool gpuCompactionEligible{false};
  bool gpuVisibilityOwnsCpuFallback{false};
};

struct BimSurfaceDrawRoute {
  BimSurfaceDrawRouteKind kind{BimSurfaceDrawRouteKind::SingleSided};
  BimDrawCompactionSlot gpuSlot{};
  const std::vector<DrawCommand> *cpuCommands{nullptr};
  bool gpuCompactionAllowed{false};
  bool cpuFallbackAllowed{true};
};

struct BimSurfaceDrawRoutingPlan {
  std::array<BimSurfaceDrawRoute, 3> routes{};
  uint32_t routeCount{0};
};

class BimSurfaceDrawRoutingPlanner {
public:
  explicit BimSurfaceDrawRoutingPlanner(BimSurfaceDrawRoutingInputs inputs);

  [[nodiscard]] BimSurfaceDrawRoutingPlan build() const;

private:
  BimSurfaceDrawRoutingInputs inputs_{};
};

[[nodiscard]] std::optional<BimDrawCompactionSlot>
bimViewportCompactionSlot(BimDrawCompactionSlot slot);

[[nodiscard]] bool
hasBimSurfaceOpaqueDrawCommands(const BimSurfaceDrawLists &draws);

[[nodiscard]] bool
hasBimSurfaceTransparentDrawCommands(const BimSurfaceDrawLists &draws);

[[nodiscard]] BimSurfaceDrawRoutingPlan
buildBimSurfaceDrawRoutingPlan(const BimSurfaceDrawRoutingInputs &inputs);

} // namespace container::renderer
