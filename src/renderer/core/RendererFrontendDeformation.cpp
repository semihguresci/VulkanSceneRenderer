#include <Container/renderer/core/RendererFrontend.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/platform/VulkanContext.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/renderer/scene/GltfDeformationScene.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/utility/Platform.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>
#include <algorithm>
#include <glm/geometric.hpp>

namespace container::renderer {

void RendererFrontend::updateGltfDeformation() {
  if (!subs_.sceneController || !subs_.sceneManager) return;
  // drawFrame's serialized policy has retired graphics readers. Retire GI
  // before publishing geometry/provider revisions shared with its scene work.
  if (subs_.sceneManager->deformationData() && subs_.ddgiManager)
    subs_.ddgiManager->synchronizeAsync();
  const auto changed = subs_.sceneController->updateDeformation();
  if (changed.topologyChanged) subs_.sceneController->createGeometryBuffers();
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
  const auto* scene = subs_.sceneController->deformationScene();
  if (!scene || scene->gpuInstances().empty()) {
    subs_.gltfDeformationPass.reset();
    gltfDeformationSourceIdentity_ = 0;
    gltfDeformationSubmittedEpoch_.reset();
    gltfDeformationMaxPositionDelta_ = 0;
    gltfDeformationHistoryReset_ = true;
  } else {
    const auto images = static_cast<uint32_t>(svc_.swapChainManager.imageCount());
    const auto sourceRevision = subs_.sceneManager->geometryRevision();
    const auto vertices = subs_.sceneController->gpuBindVertices();
    if (!subs_.gltfDeformationPass || gltfDeformationImageCount_ != images) {
      subs_.gltfDeformationPass = std::make_unique<GltfDeformationPass>(
          svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
      subs_.gltfDeformationPass->createResources(container::util::executableDirectory(), images);
      gltfDeformationSourceIdentity_ = 0;
      gltfDeformationImageCount_ = images;
    }
    if (gltfDeformationSourceIdentity_ != subs_.sceneManager->animationSourceRevision() ||
        gltfDeformationSourceRevision_ != sourceRevision ||
        gltfDeformationTopologyRevision_ != scene->topologyRevision() ||
        gltfDeformationVertexCount_ != vertices.size()) {
      subs_.gltfDeformationPass->setScene(vertices, scene->gpuInstances());
      gltfDeformationSourceIdentity_ = subs_.sceneManager->animationSourceRevision();
      gltfDeformationSourceRevision_ = sourceRevision;
      gltfDeformationTopologyRevision_ = scene->topologyRevision();
      gltfDeformationVertexCount_ = vertices.size();
      gltfDeformationSubmittedEpoch_.reset();
      if (subs_.temporalManager) subs_.temporalManager->reset("deformed scene topology changed");
    }
  }
  if (changed.geometryChanged || changed.topologyChanged) {
    refreshSceneObjectData();
    syncSceneProviders();
  }
}

void RendererFrontend::prepareGltfDeformationFrame(uint32_t imageIndex) {
  if (!subs_.gltfDeformationPass || !subs_.sceneController) return;
  const auto epoch = subs_.temporalManager->state().epoch();
  gltfDeformationHistoryReset_ = !gltfDeformationSubmittedEpoch_ || *gltfDeformationSubmittedEpoch_ != epoch;
  if (gltfDeformationHistoryReset_)
    subs_.sceneController->updateDeformation(true);
  const auto* scene = subs_.sceneController->deformationScene();
  if (scene) {
    gltfDeformationMaxPositionDelta_ = 0;
    const auto current = scene->currentVertices();
    const auto previous = scene->previousVertices();
    for (const auto& instance : scene->gpuInstances()) {
      const size_t end = size_t(instance.outputVertexOffset) + instance.primitive->bindVertices.size();
      for (size_t index = instance.outputVertexOffset; index < end; ++index)
        gltfDeformationMaxPositionDelta_ = std::max(gltfDeformationMaxPositionDelta_,
            glm::distance(current[index].position, previous[index].position));
    }
    subs_.gltfDeformationPass->uploadFrame(imageIndex, scene->gpuPoses());
  }
}

void RendererFrontend::commitGltfDeformationFrame() {
  if (!subs_.sceneController) return;
  subs_.sceneController->commitDeformationFrame();
  if (subs_.temporalManager)
    gltfDeformationSubmittedEpoch_ = subs_.temporalManager->state().epoch();
}

} // namespace container::renderer
