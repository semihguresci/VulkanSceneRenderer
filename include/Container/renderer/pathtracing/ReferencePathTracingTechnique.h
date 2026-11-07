#pragma once
#include <Container/renderer/core/RenderTechnique.h>

namespace container::renderer {
class ReferencePathTracingTechnique final : public RenderTechnique {
 public:
  RenderTechniqueId id() const override { return RenderTechniqueId::PathTracing; }
  std::string_view name() const override { return "path-tracing"; }
  std::string_view displayName() const override { return "Path tracing"; }
  RenderTechniqueAvailability availability(const RenderSystemContext&) const override;
  TechniqueDebugModel debugModel() const override;
  void registerTechniqueContracts(RenderSystemContext&) override;
  void buildFrameGraph(RenderSystemContext&) override;
};
}
