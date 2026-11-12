#pragma once

#include "Container/renderer/lighting/LightingManager.h"
#include "Container/utility/SceneManager.h"

#include <algorithm>
#include <map>
#include <optional>
#include <span>

namespace container::renderer {

[[nodiscard]] inline std::span<const container::scene::AuthoredLightBinding> authoredLightBindings(
    const container::scene::SceneManager& scene, EditableLightType type) noexcept {
  if (type == EditableLightType::Directional) return scene.authoredDirectionalLightBindings();
  if (type == EditableLightType::Area) return scene.authoredAreaLightBindings();
  return scene.authoredPointLightBindings();
}

[[nodiscard]] inline std::optional<container::scene::AuthoredLightBinding> selectedImportedLightBinding(
    EditableLightId light, std::span<const container::scene::AuthoredLightBinding> bindings) {
  if (light.source != EditableLightSource::Imported || !isValidEditableLightId(light)) return {};
  const auto index = light.type == EditableLightType::Directional ? 0u : light.index;
  return index < bindings.size() ? std::optional(bindings[index]) : std::nullopt;
}

[[nodiscard]] inline EditableLightId remapImportedLightSelection(EditableLightId light,
    const container::scene::AuthoredLightBinding* binding,
    std::span<const container::scene::AuthoredLightBinding> bindings) noexcept {
  if (light.source != EditableLightSource::Imported || !isValidEditableLightId(light)) return light;
  if (!binding) return {};
  // Only the first authored sun has an editable renderer row.
  if (light.type == EditableLightType::Directional) {
    if (bindings.empty() || bindings.front() != *binding) return {};
    light.index = 0;
    return light;
  }
  const auto found = std::ranges::find(bindings, *binding);
  if (found == bindings.end()) return {};
  light.index = static_cast<uint32_t>(found - bindings.begin());
  return light;
}

// Imported bindings belong to the global source graph. Scene-local ordinal
// arrays are adapters; two scenes selecting the same node share its edits.
struct ImportedSceneLightEdits {
  using Binding=container::scene::AuthoredLightBinding;
  std::map<Binding,std::optional<container::gpu::PointLightData>> points;
  std::map<Binding,std::optional<container::gpu::AreaLightData>> areas;
  std::map<Binding,std::optional<EditableLightEntity>> directionals;

  void capture(const container::scene::AuthoredSceneLights& source,const SceneLightEdits& edits) {
    for(size_t i=0;i<source.pointBindings.size();++i)
      points[source.pointBindings[i]]=i<edits.importedPointOverrides.size()?edits.importedPointOverrides[i]:std::nullopt;
    for(size_t i=0;i<source.areaBindings.size();++i)
      areas[source.areaBindings[i]]=i<edits.importedAreaOverrides.size()?edits.importedAreaOverrides[i]:std::nullopt;
    if(!source.directionalBindings.empty() &&
        (!edits.directionalOverride || edits.directionalOverride->source!=EditableLightSource::Manual))
      directionals[source.directionalBindings.front()]=edits.directionalOverride &&
          edits.directionalOverride->source==EditableLightSource::Imported ? edits.directionalOverride : std::nullopt;
  }
  [[nodiscard]] SceneLightEdits merge(const container::scene::AuthoredSceneLights& source,SceneLightEdits edits) const {
    edits.importedPointOverrides.assign(source.pointBindings.size(),{});
    edits.importedAreaOverrides.assign(source.areaBindings.size(),{});
    for(size_t i=0;i<source.pointBindings.size();++i)
      if(const auto found=points.find(source.pointBindings[i]);found!=points.end()) edits.importedPointOverrides[i]=found->second;
    for(size_t i=0;i<source.areaBindings.size();++i)
      if(const auto found=areas.find(source.areaBindings[i]);found!=areas.end()) edits.importedAreaOverrides[i]=found->second;
    if((!source.directionalBindings.empty() &&
          (!edits.directionalOverride || edits.directionalOverride->source!=EditableLightSource::Manual)) ||
        (edits.directionalOverride && edits.directionalOverride->source==EditableLightSource::Imported)) {
      edits.directionalOverride.reset();
      if(!source.directionalBindings.empty())
        if(const auto found=directionals.find(source.directionalBindings.front());found!=directionals.end())
          edits.directionalOverride=found->second;
    }
    return edits;
  }
};

} // namespace container::renderer
