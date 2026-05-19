#include "Container/renderer/bim/BimFrameGpuVisibilityRecorder.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"
#include "Container/renderer/core/FrameRecorder.h"

namespace container::renderer {

void prepareBimFrameGpuVisibility(BimManager *manager) {
  prepareBimFrameGpuVisibility(manager, FrameBimResources{});
}

void prepareBimFrameGpuVisibility(BimManager *manager,
                                  const FrameBimResources &bim) {
  if (manager == nullptr) {
    return;
  }

  BimDrawCompactionPlanInputs inputs =
      makeBimDrawCompactionPlanInputs(*manager);
  inputs.meshCompactionEnabled =
          bim.opaqueMeshDrawsUseGpuVisibility ||
          bim.transparentMeshDrawsUseGpuVisibility;
  const auto compactionPlan = buildBimDrawCompactionPlan(inputs);
  for (const BimDrawCompactionPlanSource &source : compactionPlan) {
    manager->prepareDrawCompaction(source.slot, *source.commands);
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
