#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>
#include <cstdint>
#include <optional>

namespace container::scene {
class BaseCamera;
} // namespace container::scene

namespace container::ui {
class GuiManager;
enum class GBufferViewMode : uint32_t;
} // namespace container::ui

namespace container::renderer {

class DebugOverlayRenderer;
class LightingManager;
class SceneController;
struct FrameRecordParams;

struct DeferredRasterLightingPassServices {
  VkExtent2D framebufferExtent{};
  const LightingManager *lightingManager{nullptr};
  const SceneController *sceneController{nullptr};
  const container::scene::BaseCamera *camera{nullptr};
  const container::ui::GuiManager *guiManager{nullptr};
  std::optional<container::ui::GBufferViewMode> fallbackDisplayMode{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  bool tileCullPassActive{false};
};

[[nodiscard]] bool hasDeferredRasterBimDisplayOverlays(const FrameRecordParams &p);

class DeferredRasterLightingPassRecorder {
public:
  explicit DeferredRasterLightingPassRecorder(
      DeferredRasterLightingPassServices services);

  void record(VkCommandBuffer cmd, const FrameRecordParams &p,
              VkDescriptorSet sceneSet,
              const std::array<VkDescriptorSet, 2> &lightingSets,
              const std::array<VkDescriptorSet, 4> &transparentSets,
              bool deferBimDisplayOverlays = false,
              bool oitActive = false) const;

  void recordBimDisplayOverlays(VkCommandBuffer cmd, const FrameRecordParams &p) const;

private:
  DeferredRasterLightingPassServices services_{};
};

} // namespace container::renderer
