#pragma once

#include "Container/renderer/core/TechniqueDebugModel.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::renderer {

class FrameResourceRegistry;
class FrameRecorder;
class PipelineRegistry;
class DeferredRasterFrameGraphContext;
struct RendererDeviceCapabilities;

}  // namespace container::renderer

namespace container::scene {
class SceneProviderRegistry;
}  // namespace container::scene

namespace container::renderer {

enum class RenderTechniqueId {
  DeferredRaster,
  ForwardRaster,
  RayTracing,
  PathTracing,
  GaussianSplatting,
  RadianceField,
};

[[nodiscard]] constexpr bool isReferenceRayTechnique(RenderTechniqueId id) {
  return id == RenderTechniqueId::RayTracing || id == RenderTechniqueId::PathTracing;
}

struct RenderTechniqueDescriptor {
  RenderTechniqueId id{RenderTechniqueId::DeferredRaster};
  std::string_view name{};
  std::string_view displayName{};
  bool implemented{false};
  bool requiresRayTracing{false};
  bool requiresPathTracing{false};
};

struct RenderSystemContext {
  FrameRecorder* frameRecorder{nullptr};
  DeferredRasterFrameGraphContext* deferredRaster{nullptr};
  const RendererDeviceCapabilities* deviceCapabilities{nullptr};
  const container::scene::SceneProviderRegistry* sceneProviders{nullptr};
  FrameResourceRegistry* frameResources{nullptr};
  PipelineRegistry* pipelines{nullptr};
};

struct FrameBuildContext {
  uint64_t frameIndex{0};
};

struct RenderTechniqueAvailability {
  bool available{true};
  std::string reason{};

  [[nodiscard]] static RenderTechniqueAvailability availableNow() {
    return {};
  }

  [[nodiscard]] static RenderTechniqueAvailability unavailable(
      std::string reason) {
    return {.available = false, .reason = std::move(reason)};
  }
};

class RenderTechnique {
 public:
  virtual ~RenderTechnique() = default;

  [[nodiscard]] virtual RenderTechniqueId id() const = 0;
  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual std::string_view displayName() const = 0;
  [[nodiscard]] virtual RenderTechniqueAvailability availability(
      const RenderSystemContext& context) const = 0;

  virtual void registerTechniqueContracts(RenderSystemContext& context) {
    (void)context;
  }
  virtual void buildFrameGraph(RenderSystemContext& context) = 0;
  virtual void beginFrame(const FrameBuildContext& context) { (void)context; }
  [[nodiscard]] virtual TechniqueDebugModel debugModel() const {
    return {.techniqueName = std::string(name()),
            .displayName = std::string(displayName())};
  }
};

struct RenderTechniqueSelection {
  RenderTechnique* technique{nullptr};
  RenderTechniqueId requested{RenderTechniqueId::DeferredRaster};
  RenderTechniqueId selected{RenderTechniqueId::DeferredRaster};
  bool usedFallback{false};
  std::string unavailableReason{};
};

[[nodiscard]] std::string_view renderTechniqueName(RenderTechniqueId id);
[[nodiscard]] std::string_view renderTechniqueDisplayName(RenderTechniqueId id);
[[nodiscard]] std::span<const RenderTechniqueDescriptor>
knownRenderTechniqueDescriptors();
[[nodiscard]] std::optional<RenderTechniqueId> renderTechniqueIdFromName(
    std::string_view name);

class RenderTechniqueRegistry {
 public:
  void registerTechnique(std::unique_ptr<RenderTechnique> technique);

  [[nodiscard]] RenderTechnique* find(RenderTechniqueId id);
  [[nodiscard]] const RenderTechnique* find(RenderTechniqueId id) const;
  [[nodiscard]] std::span<const std::unique_ptr<RenderTechnique>>
  techniques() const {
    return {techniques_.data(), techniques_.size()};
  }

  [[nodiscard]] RenderTechniqueSelection select(
      RenderTechniqueId requested,
      const RenderSystemContext& context);

 private:
  std::vector<std::unique_ptr<RenderTechnique>> techniques_{};
};

[[nodiscard]] RenderTechniqueRegistry createDefaultRenderTechniqueRegistry();

}  // namespace container::renderer
