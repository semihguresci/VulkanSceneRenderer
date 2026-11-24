#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct BimDrawLists;
struct BimGeometryDrawLists;
struct DrawCommand;

enum class BimFrameDrawSource : uint32_t {
  Unfiltered = 0,
  CpuFiltered = 1,
  GpuFiltered = 2,
};

struct BimFrameGpuVisibilityInputs {
  bool filterActive{false};
  bool gpuResident{false};
  bool computeReady{false};
  size_t objectCount{0};
  bool visibilityMaskReady{false};
};

struct BimFrameMeshDrawLists {
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
  uint64_t sourceRevision{};
};

struct BimFrameDrawRoutingInputs {
  BimFrameGpuVisibilityInputs gpuVisibility{};
  bool pointCloudVisible{true};
  bool curvesVisible{true};
  BimFrameMeshDrawLists unfilteredMeshDraws{};
  const BimGeometryDrawLists *unfilteredPointDraws{nullptr};
  const BimGeometryDrawLists *unfilteredCurveDraws{nullptr};
  const BimGeometryDrawLists *unfilteredNativePointDraws{nullptr};
  const BimGeometryDrawLists *unfilteredNativeCurveDraws{nullptr};
  const BimGeometryDrawLists *unfilteredNativeCurveStrokeDraws{nullptr};
  const BimDrawLists *cpuFilteredDraws{nullptr};
};

struct BimFrameDrawRoutingPlan {
  bool cpuFilteredDrawsRequired{false};
  bool meshDrawsUseGpuVisibility{false};
  bool transparentMeshDrawsUseGpuVisibility{false};
  bool nativePrimitiveDrawsUseGpuVisibility{false};
  bool nativePointDrawsUseGpuVisibility{false};
  bool nativeCurveDrawsUseGpuVisibility{false};
  bool nativeCurveStrokeDrawsUseGpuVisibility{false};
  BimFrameDrawSource meshDrawSource{BimFrameDrawSource::Unfiltered};
  BimFrameMeshDrawLists meshDraws{};
  BimFrameDrawSource pointPlaceholderDrawSource{
      BimFrameDrawSource::Unfiltered};
  BimFrameDrawSource curvePlaceholderDrawSource{
      BimFrameDrawSource::Unfiltered};
  BimFrameDrawSource nativePointDrawSource{BimFrameDrawSource::Unfiltered};
  BimFrameDrawSource nativeCurveDrawSource{BimFrameDrawSource::Unfiltered};
  BimFrameDrawSource nativeCurveStrokeDrawSource{
      BimFrameDrawSource::Unfiltered};
  const BimGeometryDrawLists *pointPlaceholderDraws{nullptr};
  const BimGeometryDrawLists *curvePlaceholderDraws{nullptr};
  const BimGeometryDrawLists *nativePointDraws{nullptr};
  const BimGeometryDrawLists *nativeCurveDraws{nullptr};
  const BimGeometryDrawLists *nativeCurveStrokeDraws{nullptr};
  bool pointPrimitivePassEnabled{false};
  bool curvePrimitivePassEnabled{false};
  bool curveStrokePrimitivePassEnabled{false};
};

class BimFrameDrawRoutingPlanner {
public:
  explicit BimFrameDrawRoutingPlanner(BimFrameDrawRoutingInputs inputs);

  [[nodiscard]] BimFrameDrawRoutingPlan build() const;

private:
  BimFrameDrawRoutingInputs inputs_{};
};

[[nodiscard]] bool
bimFrameGpuVisibilityAvailable(const BimFrameGpuVisibilityInputs &inputs);

[[nodiscard]] bool
hasBimFrameGeometryDrawCommands(const BimGeometryDrawLists *draws);

[[nodiscard]] bool
hasBimFrameMeshDrawCommands(const BimFrameMeshDrawLists &draws);

[[nodiscard]] BimFrameDrawRoutingPlan
buildBimFrameDrawRoutingPlan(const BimFrameDrawRoutingInputs &inputs);

} // namespace container::renderer
