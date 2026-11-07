#pragma once

#include "Container/renderer/scene/DrawCommand.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/utility/SceneData.h"

#include <span>

namespace container::renderer {

// Scalar projection only: no copying of object transforms/geometry and no
// resource ownership. The borrowed renderer arrays must outlive adapter use.
[[nodiscard]] inline container::scene::MeshSceneProviderBindingView MakeMeshSceneProviderBindingView(
    std::span<const container::gpu::ObjectData> objects,
    std::span<const DrawCommand> opaque, std::span<const DrawCommand> transparent,
    uint64_t indexCount, const container::scene::MeshSceneProviderBindingLimits& limits={}) noexcept {
  const auto objectAt=+[](const void* data,size_t index) noexcept {
    const auto& object=static_cast<const container::gpu::ObjectData*>(data)[index];
    return container::scene::MeshSceneProviderObjectBinding{object.temporalInfo.w,object.objectInfo.x,
        object.temporalInfo.z,bool(object.objectInfo.y & container::gpu::kObjectFlagDoubleSided)};
  };
  const auto drawAt=+[](const void* data,size_t index) noexcept {
    const auto& draw=static_cast<const DrawCommand*>(data)[index];
    return container::scene::MeshSceneProviderDrawBinding{draw.objectIndex,draw.firstIndex,draw.indexCount,draw.instanceCount};
  };
  return {{objects.data(),objects.size(),objectAt},{opaque.data(),opaque.size(),drawAt},
      {transparent.data(),transparent.size(),drawAt},indexCount,limits};
}

} // namespace container::renderer
