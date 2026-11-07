#include <Container/renderer/pathtracing/ReferencePathTracingTechnique.h>
#include <Container/renderer/forward/ForwardRasterTechnique.h>
#include <Container/renderer/core/FrameRecorder.h>
#include <Container/renderer/core/RendererDeviceCapabilities.h>
#include <Container/renderer/resources/FrameResourceRegistry.h>

namespace container::renderer {
RenderTechniqueAvailability ReferencePathTracingTechnique::availability(const RenderSystemContext& context) const {
  const auto shared=ForwardRasterTechnique{}.availability(context);
  if(!shared.available) return shared;
  if(!context.deviceCapabilities || !context.deviceCapabilities->supportsPathTracing())
    return RenderTechniqueAvailability::unavailable("Path tracing requires enabled Vulkan ray queries and bindless scene resources");
  return RenderTechniqueAvailability::availableNow();
}
TechniqueDebugModel ReferencePathTracingTechnique::debugModel() const {
  TechniqueDebugModel model{};model.techniqueName=std::string(name());model.displayName=std::string(displayName());
  model.displayModes={{.id="lit",.label="Lit",.value=0}};return model;
}
void ReferencePathTracingTechnique::registerTechniqueContracts(RenderSystemContext& context) {
  ForwardRasterTechnique{}.registerTechniqueContracts(context);
  if(context.frameResources) {
    context.frameResources->clearTechnique(id());
    for(const auto* name:{"reference-color","reference-mean","reference-moments"})
      context.frameResources->registerImage(id(),name,{.format=VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent={0,0,1},.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT},FrameResourceLifetime::Persistent);
  }
}
void ReferencePathTracingTechnique::buildFrameGraph(RenderSystemContext& context) {
  ForwardRasterTechnique{}.buildFrameGraph(context);
  auto& graph=context.frameRecorder->graph();
  const auto original=graph.findPass(RenderPassId::Lighting)->record;
  const auto originalReadiness=graph.findPass(RenderPassId::Lighting)->readiness;
  graph.setPassReadiness(RenderPassId::Lighting,[originalReadiness](const FrameRecordParams& frame) {
    if(frame.services.referencePathTrace) return RenderPassReadiness{};
    return originalReadiness ? originalReadiness(frame) : RenderPassReadiness{};
  });
  graph.setPassRecord(RenderPassId::Lighting,[original](VkCommandBuffer cmd,const FrameRecordParams& frame) {
    if(!frame.services.referencePathTrace || !frame.services.referencePathTrace(cmd,frame)) original(cmd,frame);
  });
  graph.setPassResourceAccess(RenderPassId::Lighting,{RenderResourceId::CameraBuffer,RenderResourceId::LightingData},
      {RenderResourceId::RayScene,RenderResourceId::SceneDepth},{RenderResourceId::SceneColor});
  for(auto pass:{RenderPassId::TemporalVelocity,RenderPassId::TemporalResolve,RenderPassId::DdgiTrace,RenderPassId::DdgiUpdate,
      RenderPassId::RayShadowTrace,RenderPassId::RayShadowFilter,RenderPassId::OitClear,RenderPassId::OitResolve,
      RenderPassId::ShadowCullCascade0,RenderPassId::ShadowCullCascade1,RenderPassId::ShadowCullCascade2,RenderPassId::ShadowCullCascade3,
      RenderPassId::ShadowCascade0,RenderPassId::ShadowCascade1,RenderPassId::ShadowCascade2,RenderPassId::ShadowCascade3,RenderPassId::LocalShadowDepth}) {
    graph.setPassEnabled(pass,false);
    // The shared UI can toggle enable flags every frame. Readiness keeps
    // raster approximations and temporal filtering out of reference mode.
    graph.setPassReadiness(pass,[](const FrameRecordParams&) {
      return RenderPassReadiness{.ready=false,.skipReason=RenderPassSkipReason::NotNeeded};
    });
  }
  graph.compile();
}
}
