#include "NativeReferenceRayPipeline.h"

#include "Container/utility/FileLoader.h"
#include "Container/utility/Logger.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {

struct ReferenceRayStage {
  const char* file;
  const char* entryPoint;
  VkShaderStageFlagBits stage;
};

constexpr std::array rayStages{
    ReferenceRayStage{"reference_pathtrace.rgen.spv", "raygenMain", VK_SHADER_STAGE_RAYGEN_BIT_KHR},
    ReferenceRayStage{"reference_pathtrace.rmiss.spv", "missMain", VK_SHADER_STAGE_MISS_BIT_KHR},
    ReferenceRayStage{"reference_pathtrace.rchit.spv", "closestHitMain", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
    ReferenceRayStage{"reference_pathtrace.rahit.spv", "anyHitMain", VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
};
static_assert(rayStages.size() == std::tuple_size_v<NativeReferenceRayPipeline::ShaderCode>);
constexpr uint32_t rayGroupCount = 3;

VkDeviceSize alignedSize(VkDeviceSize size, VkDeviceSize alignment) {
  return ((size + alignment - 1) / alignment) * alignment;
}

struct RayShaderModules {
  VkDevice device;
  std::array<VkShaderModule, rayStages.size()> modules{};

  ~RayShaderModules() {
    for (VkShaderModule module : modules)
      if (module) destroyOwnedShaderModule(device, module, nullptr);
  }
};

} // namespace

NativeReferenceRayPipeline::NativeReferenceRayPipeline(const gpu::VulkanDevice& device,
                                                       gpu::VulkanMemoryManager& memory)
    : device_(device.device()), memory_(memory), properties_(device.rayTracingPipelineProperties()) {
  traceRays_ = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
  if (!traceRays_) throw std::runtime_error("Vulkan ray-tracing dispatch command is unavailable");
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
  const auto& limits = properties.limits;
  for (size_t axis = 0; axis < maxDispatchExtent_.size(); ++axis)
    maxDispatchExtent_[axis] = uint64_t(limits.maxComputeWorkGroupCount[axis]) * limits.maxComputeWorkGroupSize[axis];
}

NativeReferenceRayPipeline::~NativeReferenceRayPipeline() {
  memory_.destroyBuffer(bindingTable_);
}

bool NativeReferenceRayPipeline::supportsExtent(VkExtent2D extent) const {
  return extent.width <= maxDispatchExtent_[0] && extent.height <= maxDispatchExtent_[1] &&
         uint64_t(extent.width) * extent.height <= properties_.maxRayDispatchInvocationCount;
}

NativeReferenceRayPipeline::ShaderCode NativeReferenceRayPipeline::loadShaders(
    const std::filesystem::path& shaders) {
  ShaderCode code;
  for (size_t i = 0; i < rayStages.size(); ++i)
    code[i] = util::readFile(shaders / rayStages[i].file);
  return code;
}

NativeReferenceRayPipeline::Compilation NativeReferenceRayPipeline::compile(
    const gpu::VulkanDevice& device, VkPipelineLayout layout, ShaderCode shaders) {
  const auto handle = device.device();
  auto createPipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
      vkGetDeviceProcAddr(handle, "vkCreateRayTracingPipelinesKHR"));
  if (!createPipeline) throw std::runtime_error("Vulkan ray-tracing pipeline command is unavailable");

  RayShaderModules loaded{handle};
  std::array<VkPipelineShaderStageCreateInfo, rayStages.size()> stages{};
  for (size_t i = 0; i < rayStages.size(); ++i) {
    const auto& definition = rayStages[i];
    loaded.modules[i] = gpu::createShaderModule(handle, shaders[i]);
    auto& stage = stages[i];
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = definition.stage;
    stage.module = loaded.modules[i];
    stage.pName = definition.entryPoint;
  }

  std::array<VkRayTracingShaderGroupCreateInfoKHR, rayGroupCount> groups{};
  for (auto& group : groups) {
    group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    group.generalShader = group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[0].type = groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[0].generalShader = 0;
  groups[1].generalShader = 1;
  groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[2].closestHitShader = 2;
  groups[2].anyHitShader = 3;

  VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  // Favor prompt cold creation for the shared transport shader.
  info.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
  if (device.opacityMicromapsEnabled()) info.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
  info.stageCount = static_cast<uint32_t>(stages.size());
  info.pStages = stages.data();
  info.groupCount = static_cast<uint32_t>(groups.size());
  info.pGroups = groups.data();
  // The integrator loops in raygen. Hit and miss stages return to that loop.
  info.maxPipelineRayRecursionDepth = 1;
  info.layout = layout;
  const auto logger = log::ContainerLogger::instance().renderer();
  logger->debug("Creating Vulkan ray-tracing pipeline");
  const auto start = std::chrono::steady_clock::now();
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result = createPipeline(handle, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  auto owned = pipeline ? vk::raii::Pipeline(device.raii(), pipeline) : vk::raii::Pipeline(nullptr);
  if (result != VK_SUCCESS)
    throw std::runtime_error("Vulkan ray-tracing pipeline creation failed (" + std::to_string(result) + ")");
  logger->debug("Vulkan ray-tracing pipeline created in {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  return {std::move(owned), std::move(shaders)};
}

void NativeReferenceRayPipeline::adopt(Compilation compilation) {
  auto groupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
      vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
  if (!groupHandles) throw std::runtime_error("Vulkan ray-tracing shader group command is unavailable");
  const VkDeviceSize handleSize = properties_.shaderGroupHandleSize;
  const VkDeviceSize handleAlignment = properties_.shaderGroupHandleAlignment;
  const VkDeviceSize baseAlignment = properties_.shaderGroupBaseAlignment;
  if (!handleSize || !handleAlignment || !baseAlignment)
    throw std::runtime_error("Invalid Vulkan shader binding table alignment");
  const VkDeviceSize stride = alignedSize(handleSize, handleAlignment);
  if (stride > properties_.maxShaderGroupStride)
    throw std::runtime_error("Vulkan shader binding table stride exceeds the device limit");
  const VkDeviceSize spacing = alignedSize(stride, baseAlignment);
  const VkDeviceSize bytes = spacing * (regions_.size() - 1) + stride + baseAlignment - 1;

  std::vector<std::byte> handles(handleSize * regions_.size());
  if (groupHandles(device_, static_cast<VkPipeline>(*compilation.pipeline), 0,
                   rayGroupCount, handles.size(), handles.data()) != VK_SUCCESS)
    throw std::runtime_error("Cannot read Vulkan ray-tracing shader group handles");
  bindingTable_ = memory_.createBuffer(bytes,
      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr,
                                               bindingTable_.buffer};
  const auto address = vkGetBufferDeviceAddress(device_, &addressInfo);
  auto* mapped = static_cast<std::byte*>(bindingTable_.allocation_info.pMappedData);
  if (!address || !mapped) throw std::runtime_error("Cannot map the Vulkan shader binding table");
  const VkDeviceAddress alignedStart = alignedSize(address, baseAlignment);
  std::memset(mapped, 0, bytes);
  for (size_t i = 0; i < regions_.size(); ++i) {
    const auto start = alignedStart + spacing * i;
    std::memcpy(mapped + (start - address), handles.data() + i * handleSize, handleSize);
    regions_[i] = {start, stride, stride};
  }
  if (vmaFlushAllocation(memory_.allocator(), bindingTable_.allocation, 0, bytes) != VK_SUCCESS)
    throw std::runtime_error("Cannot flush the Vulkan shader binding table");
  pipeline_ = std::move(compilation.pipeline);
  shaders_ = std::move(compilation.shaders);
}

void NativeReferenceRayPipeline::record(VkCommandBuffer command, VkPipelineLayout layout,
                                        std::span<const VkDescriptorSet> sets, VkExtent2D extent) const {
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline());
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0,
                          static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  const VkStridedDeviceAddressRegionKHR callable{};
  traceRays_(command, &regions_[0], &regions_[1], &regions_[2], &callable, extent.width, extent.height, 1);
}

} // namespace container::renderer
