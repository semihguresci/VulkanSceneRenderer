#pragma once

#include "Container/common/CommonVulkan.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>

namespace container::geometry {

struct Vertex {
  glm::vec3 position{};
  glm::vec3 color{1.0f, 1.0f, 1.0f};
  glm::vec2 texCoord{0.0f, 0.0f};
  glm::vec2 texCoord1{0.0f, 0.0f};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};
  glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
  float colorAlpha{1.0f};

  static VkVertexInputBindingDescription bindingDescription() {
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
  }

  static std::array<VkVertexInputAttributeDescription, 7>
  attributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 7> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, position);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, color);

    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, normal);

    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 4;
    attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[4].offset = offsetof(Vertex, tangent);

    attributeDescriptions[5].binding = 0;
    attributeDescriptions[5].location = 5;
    attributeDescriptions[5].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[5].offset = offsetof(Vertex, texCoord1);

    // Locations 6/7 belong to the temporal previous-position/normal stream.
    attributeDescriptions[6].binding = 0;
    attributeDescriptions[6].location = 8;
    attributeDescriptions[6].format = VK_FORMAT_R32_SFLOAT;
    attributeDescriptions[6].offset = offsetof(Vertex, colorAlpha);

    return attributeDescriptions;
  }
};

static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, color) == 12);
static_assert(offsetof(Vertex, texCoord) == 24);
static_assert(offsetof(Vertex, texCoord1) == 32);
static_assert(offsetof(Vertex, normal) == 40);
static_assert(offsetof(Vertex, tangent) == 52);
static_assert(offsetof(Vertex, colorAlpha) == 68);
static_assert(sizeof(Vertex) == 72);

}  // namespace container::geometry

