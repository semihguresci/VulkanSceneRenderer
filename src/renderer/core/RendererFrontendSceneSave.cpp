#include <Container/geometry/GltfSceneExport.h>
#include <Container/renderer/core/RendererFrontend.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/scene/SceneLightExport.h>
#include <Container/utility/SceneManager.h>

#include <map>
#include <stdexcept>

namespace container::renderer {
namespace {

const container::scene::AuthoredLightBinding& bindingForLight(
    const container::scene::AuthoredSceneLights& bindings, const EditableLightEntity& light) {
  if (light.type == EditableLightType::Directional)
    return bindings.directionalBindings.at(light.id.index);
  if (light.type == EditableLightType::Area)
    return bindings.areaBindings.at(light.id.index);
  return bindings.pointBindings.at(light.id.index);
}

} // namespace

void RendererFrontend::saveSceneModel(const std::filesystem::path& destination) {
  if (!subs_.sceneManager) throw std::runtime_error("No editable primary scene to save");
  auto& manager = *subs_.sceneManager;
  std::vector<int32_t> sourceNodes;
  std::vector<container::scene::AuthoredSceneLights> bindings;
  auto snapshot = manager.buildSceneExport(
      sceneGraph_, sourceNodes, bindings,
      gltfAnimation_ ? &gltfAnimation_->library() : nullptr);
  if (subs_.lightingManager) {
    sourceNodes.resize(snapshot.nodes.size(), -1);
    const auto authored = manager.publishedAuthoredSceneLights();
    const auto edits = subs_.lightingManager->snapshotSceneLightEdits();
    const auto* root = sceneGraph_.getNode(sceneState_.rootNode);
    const auto anchor = root ? root->worldTransform : glm::mat4(1);
    ImportedSceneLightEdits imported = gltfImportedLightEdits_ ? *gltfImportedLightEdits_ : ImportedSceneLightEdits{};
    imported.capture(authored, edits);
    const auto activeScene = static_cast<size_t>(snapshot.defaultScene);
    std::vector<std::vector<EditableLightEntity>> lights(snapshot.scenes.size());
    bindings.at(activeScene) = authored;
    lights[activeScene] = evaluateEditableSceneLightsForExport(authored, authored, edits, imported, anchor);
    std::map<container::scene::AuthoredLightBinding, const EditableLightEntity*> activeLights;
    if (lights.size() > 1) {
      for (const auto& light : lights[activeScene]) {
        if (light.source == EditableLightSource::Imported)
          activeLights.emplace(bindingForLight(authored, light), &light);
      }
    }
    for (size_t scene = 0; scene < lights.size(); ++scene) {
      if (scene == activeScene) continue;
      const auto cached = gltfSceneLightEdits_.find(static_cast<uint32_t>(scene));
      const SceneLightEdits empty;
      const auto& local = cached == gltfSceneLightEdits_.end() ? empty : *cached->second;
      lights[scene] = evaluateEditableSceneLightsForExport(bindings[scene], bindings[scene],
          local, imported, glm::mat4(1));
      for (auto& light : lights[scene]) {
        if (light.source != EditableLightSource::Imported) continue;
        const auto match = activeLights.find(bindingForLight(bindings[scene], light));
        if (match == activeLights.end()) continue;
        // A shared source node has one pose and edit state. Keep only the
        // scene-local ordinal from this scene's independently evaluated row.
        const auto id = light.id;
        light = *match->second;
        light.id = id;
      }
    }
    exportEditableSceneLightsForScenes(snapshot, lights, &manager.animationSource(),
        sourceNodes, manager.importScale(), bindings, bindings);
  }
  container::geometry::gltf::writeExportModel(snapshot, destination);
}

} // namespace container::renderer
