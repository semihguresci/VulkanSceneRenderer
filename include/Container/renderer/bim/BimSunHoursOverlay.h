#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace container::renderer {

inline constexpr char kBimSunHoursPipeline[] = "bim-sun-hours-display";

struct BimSunHoursVertex {
  glm::vec3 position;
  glm::vec3 color;
};

struct BimSunHoursOverlay {
  container::gpu::BufferSlice vertices{};
  uint32_t vertexCount{};
  glm::mat4 viewProjection{1.0f};
};

struct BimSunHoursOverlayPushConstants {
  glm::mat4 viewProjection;
  uint32_t encodeSrgb{};
};

struct FrameRecordParams;
void recordBimSunHoursOverlay(VkCommandBuffer, const FrameRecordParams&);

} // namespace container::renderer
