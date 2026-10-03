#pragma once
#include <vulkan/vulkan.h>
VkResult createOwnedCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                                const VkAllocationCallbacks *, VkCommandPool *);
void destroyOwnedCommandPool(VkDevice, VkCommandPool,
                             const VkAllocationCallbacks *);
VkResult createOwnedDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *,
                                   const VkAllocationCallbacks *,
                                   VkDescriptorPool *);
void destroyOwnedDescriptorPool(VkDevice, VkDescriptorPool,
                                const VkAllocationCallbacks *);
VkResult createOwnedDescriptorSetLayout(VkDevice,
                                        const VkDescriptorSetLayoutCreateInfo *,
                                        const VkAllocationCallbacks *,
                                        VkDescriptorSetLayout *);
void destroyOwnedDescriptorSetLayout(VkDevice, VkDescriptorSetLayout,
                                     const VkAllocationCallbacks *);
VkResult createOwnedFence(VkDevice, const VkFenceCreateInfo *,
                          const VkAllocationCallbacks *, VkFence *);
void destroyOwnedFence(VkDevice, VkFence, const VkAllocationCallbacks *);
VkResult createOwnedPipelineCache(VkDevice, const VkPipelineCacheCreateInfo *,
                                  const VkAllocationCallbacks *,
                                  VkPipelineCache *);
void destroyOwnedPipelineCache(VkDevice, VkPipelineCache,
                               const VkAllocationCallbacks *);
VkResult createOwnedPipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *,
                                   const VkAllocationCallbacks *,
                                   VkPipelineLayout *);
void destroyOwnedPipelineLayout(VkDevice, VkPipelineLayout,
                                const VkAllocationCallbacks *);
VkResult createOwnedQueryPool(VkDevice, const VkQueryPoolCreateInfo *,
                              const VkAllocationCallbacks *, VkQueryPool *);
void destroyOwnedQueryPool(VkDevice, VkQueryPool,
                           const VkAllocationCallbacks *);
VkResult createOwnedSampler(VkDevice, const VkSamplerCreateInfo *,
                            const VkAllocationCallbacks *, VkSampler *);
void destroyOwnedSampler(VkDevice, VkSampler, const VkAllocationCallbacks *);
VkResult createOwnedSemaphore(VkDevice, const VkSemaphoreCreateInfo *,
                              const VkAllocationCallbacks *, VkSemaphore *);
void destroyOwnedSemaphore(VkDevice, VkSemaphore,
                           const VkAllocationCallbacks *);
VkResult createOwnedShaderModule(VkDevice, const VkShaderModuleCreateInfo *,
                                 const VkAllocationCallbacks *,
                                 VkShaderModule *);
void destroyOwnedShaderModule(VkDevice, VkShaderModule,
                              const VkAllocationCallbacks *);
VkResult createOwnedSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *,
                                 const VkAllocationCallbacks *,
                                 VkSwapchainKHR *);
void destroyOwnedSwapchainKHR(VkDevice, VkSwapchainKHR,
                              const VkAllocationCallbacks *);
VkResult createOwnedImageView(VkDevice, const VkImageViewCreateInfo *,
                              const VkAllocationCallbacks *, VkImageView *);
void destroyOwnedImageView(VkDevice, VkImageView,
                           const VkAllocationCallbacks *);
VkResult createOwnedGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t,
                                      const VkGraphicsPipelineCreateInfo *,
                                      const VkAllocationCallbacks *,
                                      VkPipeline *);
VkResult createOwnedComputePipelines(VkDevice, VkPipelineCache, uint32_t,
                                     const VkComputePipelineCreateInfo *,
                                     const VkAllocationCallbacks *,
                                     VkPipeline *);
void destroyOwnedPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
