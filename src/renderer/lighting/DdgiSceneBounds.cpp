#include "Container/renderer/lighting/DdgiSceneBounds.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::renderer {
namespace {

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[noreturn]] void invalid(const RaySceneProviderSource &source,
                          std::string_view reason) {
  throw std::invalid_argument(
      "DDGI scene bounds for provider '" + source.provider.value + "': " +
      std::string(reason));
}

} // namespace

DdgiSceneBounds
computeDdgiSceneBounds(std::span<const RaySceneProviderSource> sources) {
  DdgiSceneBounds result;
  for (const auto &source : sources) {
    for (const auto &draw : source.draws) {
      if (draw.indexCount == 0 || draw.instanceCount == 0)
        continue;
      if (draw.indexCount % 3 != 0)
        invalid(source, "draw must reference an indexed triangle list");
      if (draw.firstIndex > source.indices.size() ||
          draw.indexCount > source.indices.size() - draw.firstIndex)
        invalid(source, "draw index range is out of bounds");
      if (draw.objectIndex > source.objects.size() ||
          draw.instanceCount > source.objects.size() - draw.objectIndex)
        invalid(source, "draw instance range is out of bounds");

      // Match RaySceneExtraction's absolute CPU indices. Use size_t additions
      // so even a draw near UINT32_MAX cannot wrap into a different range.
      const size_t firstIndex = draw.firstIndex;
      for (size_t offset = 0; offset < draw.indexCount; ++offset) {
        const uint32_t index = source.indices[firstIndex + offset];
        if (index >= source.vertices.size())
          invalid(source, "referenced vertex index is out of bounds");
        if (!finite(source.vertices[index].position))
          invalid(source, "referenced vertex position must be finite");
      }

      for (size_t instance = 0; instance < draw.instanceCount; ++instance) {
        const auto &model =
            source.objects[size_t(draw.objectIndex) + instance].model;
        try {
          // Reuse the shared ray scene's finite/affine/invertible contract.
          (void)rayInstanceTransform(model);
        } catch (const std::invalid_argument &error) {
          invalid(source, error.what());
        }
        for (size_t offset = 0; offset < draw.indexCount; ++offset) {
          const auto &position =
              source.vertices[source.indices[firstIndex + offset]].position;
          // Engine world matrices are column-major. Bounds remain in world
          // space; no projection, viewport flip or reverse-Z conversion.
          const glm::vec3 world(model * glm::vec4(position, 1.0f));
          if (!finite(world))
            invalid(source,
                    "transformed vertex exceeds finite world coordinates");
          if (!result.valid) {
            result.minimum = world;
            result.maximum = world;
            result.valid = true;
          } else {
            result.minimum = glm::min(result.minimum, world);
            result.maximum = glm::max(result.maximum, world);
          }
        }
      }
    }
  }
  return result;
}

} // namespace container::renderer
