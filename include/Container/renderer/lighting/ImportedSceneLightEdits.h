#pragma once

#include "Container/renderer/lighting/LightingManager.h"
#include "Container/utility/SceneManager.h"

#include <map>
#include <optional>

namespace container::renderer {

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
