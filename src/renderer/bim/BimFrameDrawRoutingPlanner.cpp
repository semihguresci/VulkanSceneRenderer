#include "Container/renderer/bim/BimFrameDrawRoutingPlanner.h"

#include "Container/renderer/bim/BimManager.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasAnyGeometry(const BimGeometryDrawLists *draws) {
  return draws != nullptr &&
         (hasDrawCommands(&draws->opaqueDrawCommands) ||
          hasDrawCommands(&draws->opaqueSingleSidedDrawCommands) ||
          hasDrawCommands(&draws->opaqueWindingFlippedDrawCommands) ||
          hasDrawCommands(&draws->opaqueDoubleSidedDrawCommands) ||
          hasDrawCommands(&draws->transparentDrawCommands) ||
          hasDrawCommands(&draws->transparentSingleSidedDrawCommands) ||
          hasDrawCommands(&draws->transparentWindingFlippedDrawCommands) ||
          hasDrawCommands(&draws->transparentDoubleSidedDrawCommands));
}

[[nodiscard]] BimFrameMeshDrawLists
meshDrawsFromFilteredDraws(const BimDrawLists &draws) {
  return {.opaqueDrawCommands = &draws.opaqueDrawCommands,
          .opaqueSingleSidedDrawCommands = &draws.opaqueSingleSidedDrawCommands,
          .opaqueWindingFlippedDrawCommands =
              &draws.opaqueWindingFlippedDrawCommands,
          .opaqueDoubleSidedDrawCommands =
              &draws.opaqueDoubleSidedDrawCommands,
          .transparentDrawCommands = &draws.transparentDrawCommands,
          .transparentSingleSidedDrawCommands =
              &draws.transparentSingleSidedDrawCommands,
          .transparentWindingFlippedDrawCommands =
              &draws.transparentWindingFlippedDrawCommands,
          .transparentDoubleSidedDrawCommands =
              &draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] const BimGeometryDrawLists *
chooseNativePrimitiveDraws(const BimGeometryDrawLists *unfilteredDraws,
                           const BimGeometryDrawLists *gpuFilteredDraws,
                           const BimGeometryDrawLists *cpuFilteredDraws) {
  if (gpuFilteredDraws != nullptr) {
    return gpuFilteredDraws;
  }
  if (cpuFilteredDraws != nullptr) {
    return cpuFilteredDraws;
  }
  return unfilteredDraws;
}

[[nodiscard]] BimFrameDrawSource
nativePrimitiveDrawSource(const BimGeometryDrawLists *gpuFilteredDraws,
                          const BimGeometryDrawLists *cpuFilteredDraws) {
  if (gpuFilteredDraws != nullptr) {
    return BimFrameDrawSource::GpuFiltered;
  }
  if (cpuFilteredDraws != nullptr) {
    return BimFrameDrawSource::CpuFiltered;
  }
  return BimFrameDrawSource::Unfiltered;
}

} // namespace

bool bimFrameGpuVisibilityAvailable(
    const BimFrameGpuVisibilityInputs &inputs) {
  return inputs.filterActive && inputs.gpuResident && inputs.computeReady &&
         inputs.objectCount > 0u && inputs.visibilityMaskReady;
}

bool hasBimFrameGeometryDrawCommands(const BimGeometryDrawLists *draws) {
  return hasAnyGeometry(draws);
}

bool hasBimFrameMeshDrawCommands(const BimFrameMeshDrawLists &draws) {
  return hasDrawCommands(draws.opaqueDrawCommands) ||
         hasDrawCommands(draws.opaqueSingleSidedDrawCommands) ||
         hasDrawCommands(draws.opaqueWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.opaqueDoubleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentDrawCommands) ||
         hasDrawCommands(draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.transparentDoubleSidedDrawCommands);
}

BimFrameDrawRoutingPlanner::BimFrameDrawRoutingPlanner(
    BimFrameDrawRoutingInputs inputs)
    : inputs_(inputs) {}

BimFrameDrawRoutingPlan BimFrameDrawRoutingPlanner::build() const {
  const bool gpuVisibilityAvailable =
      bimFrameGpuVisibilityAvailable(inputs_.gpuVisibility);
  const bool nativePointDrawsUseGpuVisibility =
      gpuVisibilityAvailable &&
      hasAnyGeometry(inputs_.unfilteredNativePointDraws);
  const bool nativeCurveDrawsUseGpuVisibility =
      gpuVisibilityAvailable &&
      hasAnyGeometry(inputs_.unfilteredNativeCurveDraws);
  const bool nativeCurveStrokeDrawsUseGpuVisibility =
      gpuVisibilityAvailable && inputs_.curvesVisible &&
      hasAnyGeometry(inputs_.unfilteredNativeCurveStrokeDraws);

  BimFrameDrawRoutingPlan plan{};
  plan.meshDrawsUseGpuVisibility = gpuVisibilityAvailable;
  plan.transparentMeshDrawsUseGpuVisibility = gpuVisibilityAvailable;
  plan.nativePointDrawsUseGpuVisibility = nativePointDrawsUseGpuVisibility;
  plan.nativeCurveDrawsUseGpuVisibility = nativeCurveDrawsUseGpuVisibility;
  plan.nativeCurveStrokeDrawsUseGpuVisibility =
      nativeCurveStrokeDrawsUseGpuVisibility;
  plan.nativePrimitiveDrawsUseGpuVisibility =
      nativePointDrawsUseGpuVisibility || nativeCurveDrawsUseGpuVisibility ||
      nativeCurveStrokeDrawsUseGpuVisibility;

  if (inputs_.gpuVisibility.filterActive) {
    plan.cpuFilteredDrawsRequired =
        (!plan.meshDrawsUseGpuVisibility &&
         hasBimFrameMeshDrawCommands(inputs_.unfilteredMeshDraws)) ||
        (inputs_.pointCloudVisible &&
         hasAnyGeometry(inputs_.unfilteredPointDraws)) ||
        (inputs_.curvesVisible &&
         hasAnyGeometry(inputs_.unfilteredCurveDraws)) ||
        (inputs_.pointCloudVisible && !nativePointDrawsUseGpuVisibility &&
         hasAnyGeometry(inputs_.unfilteredNativePointDraws)) ||
        (inputs_.curvesVisible && !nativeCurveDrawsUseGpuVisibility &&
         hasAnyGeometry(inputs_.unfilteredNativeCurveDraws)) ||
        (inputs_.curvesVisible && !nativeCurveStrokeDrawsUseGpuVisibility &&
         hasAnyGeometry(inputs_.unfilteredNativeCurveStrokeDraws));
  }

  const BimDrawLists *filteredDraws = inputs_.cpuFilteredDraws;
  plan.meshDrawSource = plan.meshDrawsUseGpuVisibility || filteredDraws == nullptr
                            ? BimFrameDrawSource::Unfiltered
                            : BimFrameDrawSource::CpuFiltered;
  plan.meshDraws = plan.meshDrawSource == BimFrameDrawSource::CpuFiltered
                       ? meshDrawsFromFilteredDraws(*filteredDraws)
                       : inputs_.unfilteredMeshDraws;

  if (inputs_.pointCloudVisible) {
    plan.pointPlaceholderDrawSource =
        filteredDraws != nullptr ? BimFrameDrawSource::CpuFiltered
                                 : BimFrameDrawSource::Unfiltered;
    plan.pointPlaceholderDraws =
        filteredDraws != nullptr ? &filteredDraws->points
                                 : inputs_.unfilteredPointDraws;
    const BimGeometryDrawLists *gpuNativePointDraws =
        nativePointDrawsUseGpuVisibility ? inputs_.unfilteredNativePointDraws
                                         : nullptr;
    const BimGeometryDrawLists *cpuNativePointDraws =
        filteredDraws != nullptr ? &filteredDraws->nativePoints : nullptr;
    plan.nativePointDrawSource =
        nativePrimitiveDrawSource(gpuNativePointDraws, cpuNativePointDraws);
    plan.nativePointDraws = chooseNativePrimitiveDraws(
        inputs_.unfilteredNativePointDraws, gpuNativePointDraws,
        cpuNativePointDraws);
    plan.pointPrimitivePassEnabled = hasAnyGeometry(plan.nativePointDraws);
  }

  if (inputs_.curvesVisible) {
    plan.curvePlaceholderDrawSource =
        filteredDraws != nullptr ? BimFrameDrawSource::CpuFiltered
                                 : BimFrameDrawSource::Unfiltered;
    plan.curvePlaceholderDraws =
        filteredDraws != nullptr ? &filteredDraws->curves
                                 : inputs_.unfilteredCurveDraws;
    const BimGeometryDrawLists *gpuNativeCurveDraws =
        nativeCurveDrawsUseGpuVisibility ? inputs_.unfilteredNativeCurveDraws
                                         : nullptr;
    const BimGeometryDrawLists *cpuNativeCurveDraws =
        filteredDraws != nullptr ? &filteredDraws->nativeCurves : nullptr;
    plan.nativeCurveDrawSource =
        nativePrimitiveDrawSource(gpuNativeCurveDraws, cpuNativeCurveDraws);
    plan.nativeCurveDraws = chooseNativePrimitiveDraws(
        inputs_.unfilteredNativeCurveDraws, gpuNativeCurveDraws,
        cpuNativeCurveDraws);
    plan.curvePrimitivePassEnabled = hasAnyGeometry(plan.nativeCurveDraws);

    const BimGeometryDrawLists *gpuNativeCurveStrokeDraws =
        nativeCurveStrokeDrawsUseGpuVisibility
            ? inputs_.unfilteredNativeCurveStrokeDraws
            : nullptr;
    const BimGeometryDrawLists *cpuNativeCurveStrokeDraws =
        filteredDraws != nullptr ? &filteredDraws->nativeCurveStrokes : nullptr;
    plan.nativeCurveStrokeDrawSource = nativePrimitiveDrawSource(
        gpuNativeCurveStrokeDraws, cpuNativeCurveStrokeDraws);
    plan.nativeCurveStrokeDraws = chooseNativePrimitiveDraws(
        inputs_.unfilteredNativeCurveStrokeDraws, gpuNativeCurveStrokeDraws,
        cpuNativeCurveStrokeDraws);
    plan.curveStrokePrimitivePassEnabled =
        hasAnyGeometry(plan.nativeCurveStrokeDraws);
  }

  return plan;
}

BimFrameDrawRoutingPlan
buildBimFrameDrawRoutingPlan(const BimFrameDrawRoutingInputs &inputs) {
  return BimFrameDrawRoutingPlanner(inputs).build();
}

} // namespace container::renderer
