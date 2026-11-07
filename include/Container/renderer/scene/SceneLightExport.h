#pragma once
#include <Container/renderer/lighting/EditableLight.h>
#include <tiny_gltf.h>
#include <span>
#include <cstdint>
#include <vector>

namespace container::scene {
struct AuthoredSceneLights;
}

namespace container::renderer {
struct SceneLightEdits;
struct ImportedSceneLightEdits;
// Evaluate authored and manual export content from the current CPU pose. The
// input table already includes Import Scale; sceneTransform is only the outer
// editor root. Explicit imported overrides contain world-space values.
[[nodiscard]] std::vector<EditableLightEntity> evaluateEditableSceneLights(
    const container::scene::AuthoredSceneLights& authored,
    const SceneLightEdits& edits,
    const glm::mat4& sceneTransform);
// Full tables include hidden bindings; visible tables identify the active sun.
// Every authored light and global edit is retained, including inactive/hidden
// suns that are absent from the live renderer's compact ordinal table.
[[nodiscard]] std::vector<EditableLightEntity> evaluateEditableSceneLightsForExport(
    const container::scene::AuthoredSceneLights& full,
    const container::scene::AuthoredSceneLights& visible,
    const SceneLightEdits& localEdits,
    const ImportedSceneLightEdits& globalImportedEdits,
    const glm::mat4& sceneTransform);
// Overlay edited imported lights and append manual lights to a detached glTF
// snapshot. Generated diagnostic lighting is not authored scene content.
// The renderer has one active directional binding: a manual directional light
// replaces the first imported sun. Its exported root precedes preserved source
// suns so reloading selects the same active binding after hierarchy edits.
void exportEditableSceneLights(tinygltf::Model& model,
                               std::span<const EditableLightEntity> lights,
                               const tinygltf::Model* originalSource = nullptr,
                               std::span<const int32_t> sourceNodeIndices = {},
                               double importedRangeScale = 1.0);
// Scene vectors follow model.scenes order. Empty vectors retain source lights.
// A shared source binding is serialized once; conflicting per-scene edits are
// rejected without changing the snapshot. Independent bindings may still share
// one source light definition. The selected default scene is retained.
void exportEditableSceneLightsForScenes(
    tinygltf::Model& model,
    std::span<const std::vector<EditableLightEntity>> sceneLights,
    const tinygltf::Model* originalSource = nullptr,
    std::span<const int32_t> sourceNodeIndices = {},
    double importedRangeScale = 1.0,
    std::span<const container::scene::AuthoredSceneLights> sceneBindings = {},
    std::span<const container::scene::AuthoredSceneLights> visibleSceneBindings = {});
}
