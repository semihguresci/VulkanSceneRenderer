#include <Container/geometry/GltfSceneExport.h>
#include <Container/renderer/core/RendererFrontend.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/scene/SceneLightExport.h>
#include <Container/utility/SceneManager.h>

#include <stdexcept>

namespace container::renderer {

void RendererFrontend::saveSceneModel(const std::filesystem::path& destination) {
  if (!subs_.sceneManager) throw std::runtime_error("No editable primary scene to save");
  auto& manager = *subs_.sceneManager;
  std::vector<int32_t> sourceNodes;
  auto snapshot = manager.buildSceneExport(
      sceneGraph_, sceneState_.rootNode, sourceNodes,
      gltfAnimation_ ? &gltfAnimation_->library() : nullptr);
  if (subs_.lightingManager) {
    sourceNodes.resize(snapshot.nodes.size(), -1);
    const auto authored = manager.publishedAuthoredSceneLights();
    const auto edits = subs_.lightingManager->snapshotSceneLightEdits();
    const auto* root = sceneGraph_.getNode(sceneState_.rootNode);
    const auto anchor = root ? root->worldTransform : glm::mat4(1);
    ImportedSceneLightEdits imported;
    imported.capture(authored, edits);
    const auto scene = static_cast<size_t>(snapshot.defaultScene);
    std::vector<std::vector<EditableLightEntity>> lights(snapshot.scenes.size());
    std::vector<container::scene::AuthoredSceneLights> bindings(snapshot.scenes.size());
    lights.at(scene) = evaluateEditableSceneLightsForExport(authored, authored, edits, imported, anchor);
    bindings.at(scene) = authored;
    exportEditableSceneLightsForScenes(snapshot, lights, &manager.animationSource(),
        sourceNodes, activePrimaryImportScale_, bindings, bindings);
  }
  container::geometry::gltf::writeExportModel(snapshot, destination);
}

} // namespace container::renderer
