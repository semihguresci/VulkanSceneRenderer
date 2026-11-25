#include "Container/renderer/bim/BimFrameGpuVisibilityRecorder.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"
#include "Container/renderer/core/FrameRecorder.h"

namespace container::renderer {

void prepareBimFrameGpuVisibility(BimManager *manager) {
  prepareBimFrameGpuVisibility(manager, FrameBimResources{}, 0);
}

void prepareBimFrameGpuVisibility(BimManager *manager,
                                  const FrameBimResources &bim,
                                  uint32_t imageIndex) {
  if (manager == nullptr) {
    return;
  }
  manager->beginVisibilityFrame(imageIndex);

  BimDrawCompactionPlanInputs inputs =
      makeBimDrawCompactionPlanInputs(*manager);
  inputs.meshCompactionEnabled =
          bim.opaqueMeshDrawsUseGpuVisibility ||
          bim.transparentMeshDrawsUseGpuVisibility;
  const bool useGeometryLods = inputs.meshCompactionEnabled &&
                               manager->hasCoarseGeometryLodSelections(imageIndex);
  if (useGeometryLods) {
    inputs.opaqueSingleSided = bim.draws.opaqueSingleSidedDrawCommands;
    inputs.opaqueWindingFlipped = bim.draws.opaqueWindingFlippedDrawCommands;
    inputs.opaqueDoubleSided = bim.draws.opaqueDoubleSidedDrawCommands;
    inputs.transparentAggregate = bim.draws.transparentDrawCommands;
    inputs.transparentSingleSided = bim.draws.transparentSingleSidedDrawCommands;
    inputs.transparentWindingFlipped = bim.draws.transparentWindingFlippedDrawCommands;
    inputs.transparentDoubleSided = bim.draws.transparentDoubleSidedDrawCommands;
  }
  const auto compactionPlan = buildBimDrawCompactionPlan(inputs);
  for (const BimDrawCompactionPlanSource &source : compactionPlan) {
    const bool dynamicSource =
        useGeometryLods && source.slot < BimDrawCompactionSlot::NativePointOpaque;
    manager->prepareDrawCompaction(source.slot, *source.commands, dynamicSource);
  }
}

bool recordBimFrameGpuVisibilityCommands(
    const BimFrameGpuVisibilityRecordInputs &inputs) {
  if (inputs.manager == nullptr ||
      inputs.commandBuffer == VK_NULL_HANDLE) {
    return false;
  }

  inputs.manager->recordMeshletResidencyUpdate(
      inputs.commandBuffer, inputs.cameraBuffer, inputs.cameraBufferSize,
      inputs.objectBuffer, inputs.objectBufferSize);
  inputs.manager->recordVisibilityFilterUpdate(inputs.commandBuffer);
  inputs.manager->recordDrawCompactionUpdate(inputs.commandBuffer);
  return true;
}

} // namespace container::renderer
