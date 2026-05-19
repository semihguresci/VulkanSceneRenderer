#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"
#include "Container/utility/GuiManager.h"

namespace container::renderer {

namespace {

DeferredTransparentOitFrameResourceInputs deferredTransparentOitInputs(
    const FrameRecordParams &p, const OitManager *oitManager) {
  DeferredTransparentOitFrameResourceInputs inputs{};
  inputs.oitManager = oitManager;
  inputs.resources = {
      .headPointerImage =
          deferredRasterImage(p, DeferredRasterImageId::OitHeadPointers),
      .nodeBuffer = deferredRasterBuffer(p, DeferredRasterBufferId::OitNode),
      .counterBuffer =
          deferredRasterBuffer(p, DeferredRasterBufferId::OitCounter)};
  return inputs;
}

[[nodiscard]] bool hasOitFrameResources(
    const DeferredTransparentOitFrameResourceInputs &inputs) {
  return inputs.oitManager != nullptr &&
         inputs.resources.headPointerImage != VK_NULL_HANDLE &&
         inputs.resources.nodeBuffer != VK_NULL_HANDLE &&
         inputs.resources.counterBuffer != VK_NULL_HANDLE;
}

}  // namespace

DeferredTransparentOitFramePassRecorder::
    DeferredTransparentOitFramePassRecorder(
        DeferredTransparentOitFramePassServices services)
    : services_(services) {}

bool DeferredTransparentOitFramePassRecorder::enabled(
    const FrameRecordParams &p) const {
  const auto fallbackDisplayMode = services_.fallbackDisplayMode.value_or(
      container::ui::GBufferViewMode::Overview);
  return shouldRecordTransparentOit(p, services_.guiManager,
                                    fallbackDisplayMode);
}

RenderPassReadiness DeferredTransparentOitFramePassRecorder::readiness(
    const FrameRecordParams &p) const {
  if (!enabled(p)) {
    return renderPassNotNeeded();
  }
  if (!hasOitFrameResources(deferredTransparentOitInputs(
          p, services_.oitManager))) {
    return renderPassMissingResource(RenderResourceId::OitStorage);
  }
  return renderPassReady();
}

bool DeferredTransparentOitFramePassRecorder::recordClear(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!enabled(p)) {
    return false;
  }
  return recordDeferredTransparentOitClearCommands(
      cmd, deferredTransparentOitInputs(p, services_.oitManager));
}

bool DeferredTransparentOitFramePassRecorder::recordResolvePreparation(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!enabled(p)) {
    return false;
  }
  return recordDeferredTransparentOitResolvePreparationCommands(
      cmd, deferredTransparentOitInputs(p, services_.oitManager));
}

} // namespace container::renderer
