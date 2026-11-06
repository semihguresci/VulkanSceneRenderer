#pragma once

#include "Container/utility/SceneData.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace container::renderer {

enum class EditableLightType : uint32_t {
  Directional = 0,
  Point = 1,
  Spot = 2,
  Area = 3,
};

enum class EditableLightSource : uint32_t {
  Generated = 0,
  Imported = 1,
  Manual = 2,
};

inline constexpr uint32_t kInvalidEditableLightIndex =
    std::numeric_limits<uint32_t>::max();

struct EditableLightId {
  EditableLightType type{EditableLightType::Point};
  EditableLightSource source{EditableLightSource::Generated};
  uint32_t index{kInvalidEditableLightIndex};
};

[[nodiscard]] bool operator==(const EditableLightId &lhs,
                              const EditableLightId &rhs);
[[nodiscard]] bool operator!=(const EditableLightId &lhs,
                              const EditableLightId &rhs);
[[nodiscard]] bool isValidEditableLightId(const EditableLightId &id);

struct EditableLightEntity {
  EditableLightId id{};
  EditableLightType type{EditableLightType::Point};
  EditableLightSource source{EditableLightSource::Generated};
  std::string label{};
  glm::vec3 position{0.0f};
  glm::vec3 direction{0.0f, -1.0f, 0.0f};
  glm::vec3 tangent{1.0f, 0.0f, 0.0f};
  glm::vec3 bitangent{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f};
  float intensity{1.0f};
  float range{1.0f};
  float innerConeDegrees{0.0f};
  float outerConeDegrees{0.0f};
  glm::vec2 areaHalfSize{0.5f};
  float areaShape{container::gpu::kAreaLightTypeRectangle};
  uint64_t iesProfileAddress{};
  bool selected{false};
  bool editable{true};
};

[[nodiscard]] const char *editableLightTypeLabel(EditableLightType type);
[[nodiscard]] const char *editableLightSourceLabel(EditableLightSource source);

[[nodiscard]] EditableLightEntity editableDirectionalLight(
    EditableLightSource source, const glm::vec3 &displayPosition,
    const container::gpu::LightingData &lightingData, bool selected);

[[nodiscard]] EditableLightEntity editablePointLight(
    EditableLightSource source, uint32_t index,
    const container::gpu::PointLightData &light, bool selected);

[[nodiscard]] EditableLightEntity editableAreaLight(
    EditableLightSource source, uint32_t index,
    const container::gpu::AreaLightData &light, bool selected);

[[nodiscard]] container::gpu::LightingData directionalLightDataFromEditable(
    const EditableLightEntity &entity,
    const container::gpu::LightingData &fallback);

[[nodiscard]] container::gpu::PointLightData
pointLightDataFromEditable(const EditableLightEntity &entity);

[[nodiscard]] container::gpu::AreaLightData
areaLightDataFromEditable(const EditableLightEntity &entity);

} // namespace container::renderer
