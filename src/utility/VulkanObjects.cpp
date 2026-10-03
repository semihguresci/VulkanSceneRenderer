#include "Container/common/VulkanObjects.h"
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vulkan/vulkan_raii.hpp>

namespace {
// Borrowed C handles keep draw recording and VMA interoperable. Every created
// Vulkan object is owned here by a Vulkan-Hpp RAII handle until its manager
// releases it, or until the owning device is torn down after waiting idle.
struct DeviceObjects {
  const vk::raii::Device &device;
  std::unordered_map<VkSwapchainKHR, vk::raii::SwapchainKHR> ownedSwapchainKHR;
  std::unordered_map<VkCommandPool, vk::raii::CommandPool> ownedCommandPool;
  std::unordered_map<VkSemaphore, vk::raii::Semaphore> ownedSemaphore;
  std::unordered_map<VkFence, vk::raii::Fence> ownedFence;
  std::unordered_map<VkDescriptorSetLayout, vk::raii::DescriptorSetLayout>
      ownedDescriptorSetLayout;
  std::unordered_map<VkDescriptorPool, vk::raii::DescriptorPool>
      ownedDescriptorPool;
  std::unordered_map<VkPipelineLayout, vk::raii::PipelineLayout>
      ownedPipelineLayout;
  std::unordered_map<VkPipelineCache, vk::raii::PipelineCache>
      ownedPipelineCache;
  std::unordered_map<VkSampler, vk::raii::Sampler> ownedSampler;
  std::unordered_map<VkImageView, vk::raii::ImageView> ownedImageView;
  std::unordered_map<VkShaderModule, vk::raii::ShaderModule> ownedShaderModule;
  std::unordered_map<VkQueryPool, vk::raii::QueryPool> ownedQueryPool;
  std::unordered_map<VkPipeline, vk::raii::Pipeline> ownedPipeline;
};
std::mutex objectMutex;
std::unordered_map<VkDevice, std::unique_ptr<DeviceObjects>> devices;
DeviceObjects &objects(VkDevice device) {
  const auto it = devices.find(device);
  if (it == devices.end())
    throw std::logic_error("Vulkan object has no RAII device owner");
  return *it->second;
}
} // namespace
namespace container::gpu {
void registerRaiiDevice(const vk::raii::Device &device) {
  std::lock_guard lock(objectMutex);
  devices.emplace(static_cast<VkDevice>(*device),
                  std::make_unique<DeviceObjects>(device));
}
void unregisterRaiiDevice(VkDevice device) {
  std::lock_guard lock(objectMutex);
  devices.erase(device);
}
} // namespace container::gpu

VkResult createOwnedCommandPool(VkDevice device,
                                const VkCommandPoolCreateInfo *info,
                                const VkAllocationCallbacks *allocator,
                                VkCommandPool *result) {
  const VkResult status = vkCreateCommandPool(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedCommandPool.emplace(
        *result,
        vk::raii::CommandPool(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedCommandPool(VkDevice device, VkCommandPool handle,
                             const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedCommandPool.erase(handle);
}

VkResult createOwnedDescriptorPool(VkDevice device,
                                   const VkDescriptorPoolCreateInfo *info,
                                   const VkAllocationCallbacks *allocator,
                                   VkDescriptorPool *result) {
  const VkResult status =
      vkCreateDescriptorPool(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedDescriptorPool.emplace(
        *result,
        vk::raii::DescriptorPool(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedDescriptorPool(VkDevice device, VkDescriptorPool handle,
                                const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedDescriptorPool.erase(handle);
}

VkResult createOwnedDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *result) {
  const VkResult status =
      vkCreateDescriptorSetLayout(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedDescriptorSetLayout.emplace(
        *result,
        vk::raii::DescriptorSetLayout(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedDescriptorSetLayout(VkDevice device,
                                     VkDescriptorSetLayout handle,
                                     const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedDescriptorSetLayout.erase(handle);
}

VkResult createOwnedFence(VkDevice device, const VkFenceCreateInfo *info,
                          const VkAllocationCallbacks *allocator,
                          VkFence *result) {
  const VkResult status = vkCreateFence(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedFence.emplace(
        *result,
        vk::raii::Fence(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedFence(VkDevice device, VkFence handle,
                       const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedFence.erase(handle);
}

VkResult createOwnedPipelineCache(VkDevice device,
                                  const VkPipelineCacheCreateInfo *info,
                                  const VkAllocationCallbacks *allocator,
                                  VkPipelineCache *result) {
  const VkResult status =
      vkCreatePipelineCache(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedPipelineCache.emplace(
        *result,
        vk::raii::PipelineCache(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedPipelineCache(VkDevice device, VkPipelineCache handle,
                               const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedPipelineCache.erase(handle);
}

VkResult createOwnedPipelineLayout(VkDevice device,
                                   const VkPipelineLayoutCreateInfo *info,
                                   const VkAllocationCallbacks *allocator,
                                   VkPipelineLayout *result) {
  const VkResult status =
      vkCreatePipelineLayout(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedPipelineLayout.emplace(
        *result,
        vk::raii::PipelineLayout(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedPipelineLayout(VkDevice device, VkPipelineLayout handle,
                                const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedPipelineLayout.erase(handle);
}

VkResult createOwnedQueryPool(VkDevice device,
                              const VkQueryPoolCreateInfo *info,
                              const VkAllocationCallbacks *allocator,
                              VkQueryPool *result) {
  const VkResult status = vkCreateQueryPool(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedQueryPool.emplace(
        *result,
        vk::raii::QueryPool(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedQueryPool(VkDevice device, VkQueryPool handle,
                           const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedQueryPool.erase(handle);
}

VkResult createOwnedSampler(VkDevice device, const VkSamplerCreateInfo *info,
                            const VkAllocationCallbacks *allocator,
                            VkSampler *result) {
  const VkResult status = vkCreateSampler(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedSampler.emplace(
        *result,
        vk::raii::Sampler(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedSampler(VkDevice device, VkSampler handle,
                         const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedSampler.erase(handle);
}

VkResult createOwnedSemaphore(VkDevice device,
                              const VkSemaphoreCreateInfo *info,
                              const VkAllocationCallbacks *allocator,
                              VkSemaphore *result) {
  const VkResult status = vkCreateSemaphore(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedSemaphore.emplace(
        *result,
        vk::raii::Semaphore(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedSemaphore(VkDevice device, VkSemaphore handle,
                           const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedSemaphore.erase(handle);
}

VkResult createOwnedShaderModule(VkDevice device,
                                 const VkShaderModuleCreateInfo *info,
                                 const VkAllocationCallbacks *allocator,
                                 VkShaderModule *result) {
  const VkResult status = vkCreateShaderModule(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedShaderModule.emplace(
        *result,
        vk::raii::ShaderModule(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedShaderModule(VkDevice device, VkShaderModule handle,
                              const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedShaderModule.erase(handle);
}

VkResult createOwnedSwapchainKHR(VkDevice device,
                                 const VkSwapchainCreateInfoKHR *info,
                                 const VkAllocationCallbacks *allocator,
                                 VkSwapchainKHR *result) {
  const VkResult status = vkCreateSwapchainKHR(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedSwapchainKHR.emplace(
        *result,
        vk::raii::SwapchainKHR(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedSwapchainKHR(VkDevice device, VkSwapchainKHR handle,
                              const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedSwapchainKHR.erase(handle);
}

VkResult createOwnedImageView(VkDevice device,
                              const VkImageViewCreateInfo *info,
                              const VkAllocationCallbacks *allocator,
                              VkImageView *result) {
  const VkResult status = vkCreateImageView(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(objectMutex);
    auto &owner = objects(device);
    owner.ownedImageView.emplace(
        *result,
        vk::raii::ImageView(
            owner.device, *result,
            reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}
void destroyOwnedImageView(VkDevice device, VkImageView handle,
                           const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedImageView.erase(handle);
}

VkResult createOwnedGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                      uint32_t count,
                                      const VkGraphicsPipelineCreateInfo *infos,
                                      const VkAllocationCallbacks *allocator,
                                      VkPipeline *results) {
  const VkResult status = vkCreateGraphicsPipelines(device, cache, count, infos,
                                                    allocator, results);
  std::lock_guard lock(objectMutex);
  auto &owner = objects(device);
  // Vulkan may return successfully created handles even if another pipeline
  // fails.
  for (uint32_t i = 0; i < count; ++i) {
    if (results[i] != VK_NULL_HANDLE)
      owner.ownedPipeline.emplace(
          results[i],
          vk::raii::Pipeline(
              owner.device, results[i],
              reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}

VkResult createOwnedComputePipelines(VkDevice device, VkPipelineCache cache,
                                     uint32_t count,
                                     const VkComputePipelineCreateInfo *infos,
                                     const VkAllocationCallbacks *allocator,
                                     VkPipeline *results) {
  const VkResult status =
      vkCreateComputePipelines(device, cache, count, infos, allocator, results);
  std::lock_guard lock(objectMutex);
  auto &owner = objects(device);
  // Vulkan may return successfully created handles even if another pipeline
  // fails.
  for (uint32_t i = 0; i < count; ++i) {
    if (results[i] != VK_NULL_HANDLE)
      owner.ownedPipeline.emplace(
          results[i],
          vk::raii::Pipeline(
              owner.device, results[i],
              reinterpret_cast<const vk::AllocationCallbacks *>(allocator)));
  }
  return status;
}

void destroyOwnedPipeline(VkDevice device, VkPipeline handle,
                          const VkAllocationCallbacks *) {
  if (handle == VK_NULL_HANDLE)
    return;
  std::lock_guard lock(objectMutex);
  objects(device).ownedPipeline.erase(handle);
}
