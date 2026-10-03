#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace container::gpu {

/**
 * @brief Create a Vulkan shader module from SPIR-V bytecode.
 *
 * @param device  Logical Vulkan device
 * @param code    SPIR-V bytecode. The byte vector is copied into aligned
 *                32-bit storage before Vulkan reads it.
 * @return VkShaderModule
 *
 * @throws std::runtime_error on failure
 */
[[nodiscard]] inline VkShaderModule createShaderModule(VkDevice device,
                                         const std::vector<char>& code) {
  if (code.empty()) {
    throw std::runtime_error("Shader code is empty");
  }

  if (code.size() % 4 != 0) {
    throw std::runtime_error("Shader code size is not a multiple of 4");
  }

  std::vector<uint32_t> alignedCode(code.size() / sizeof(uint32_t));
  std::memcpy(alignedCode.data(), code.data(), code.size());

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = alignedCode.data();

  VkShaderModule shaderModule = VK_NULL_HANDLE;

  VkResult result =
      createOwnedShaderModule(device, &createInfo, nullptr, &shaderModule);

  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create shader module");
  }

  return shaderModule;
}

}  // namespace container::gpu
