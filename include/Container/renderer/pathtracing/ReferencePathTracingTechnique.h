#pragma once
#include <Container/renderer/core/RenderTechnique.h>

namespace container::renderer {
class ReferencePathTracingTechnique final : public RenderTechnique {
 public:
  explicit ReferencePathTracingTechnique(bool nativeRayPipeline = false)
      : nativeRayPipeline_(nativeRayPipeline) {}

  RenderTechniqueId id() const override {
    return nativeRayPipeline_ ? RenderTechniqueId::RayTracing : RenderTechniqueId::PathTracing;
  }
  std::string_view name() const override { return renderTechniqueName(id()); }
  std::string_view displayName() const override { return renderTechniqueDisplayName(id()); }
  RenderTechniqueAvailability availability(const RenderSystemContext&) const override;
  TechniqueDebugModel debugModel() const override;
  void registerTechniqueContracts(RenderSystemContext&) override;
  void buildFrameGraph(RenderSystemContext&) override;

 private:
  bool nativeRayPipeline_{};
};
}
