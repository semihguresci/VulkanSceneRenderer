#include "Container/renderer/raytracing/RaySceneAcceleration.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace container::renderer {
namespace {

struct OwnedBuffer {
  container::gpu::VulkanMemoryManager &memory;
  container::gpu::AllocatedBuffer buffer{};
  VkDeviceSize size{};
  OwnedBuffer(container::gpu::VulkanMemoryManager &allocator,
              VkDeviceSize bytes, VkBufferUsageFlags usage, bool upload = false)
      : memory(allocator), size(bytes) {
    buffer = memory.createBuffer(
        std::max<VkDeviceSize>(bytes, 1), usage,
        upload ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
               : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT
               : 0);
  }
  ~OwnedBuffer() { memory.destroyBuffer(buffer); }
  OwnedBuffer(const OwnedBuffer &) = delete;
  OwnedBuffer &operator=(const OwnedBuffer &) = delete;
  void write(const void *data, size_t bytes) {
    if (bytes > size || buffer.allocation_info.pMappedData == nullptr)
      throw std::runtime_error("invalid ray scene upload");
    if (bytes == 0)
      return;
    std::memcpy(buffer.allocation_info.pMappedData, data, bytes);
    if (vmaFlushAllocation(memory.allocator(), buffer.allocation, 0, bytes) !=
        VK_SUCCESS)
      throw std::runtime_error("ray scene upload flush failed");
  }
  VkDeviceAddress address(const vk::raii::Device &device) const {
    return device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer.buffer));
  }
};

constexpr VkBufferUsageFlags inputUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

struct Structure {
  // Member destruction order is significant: AS before its VMA backing buffer.
  std::unique_ptr<OwnedBuffer> storage;
  std::unique_ptr<OwnedBuffer> scratch;
  vk::raii::AccelerationStructureKHR handle{nullptr};
  VkDeviceAddress address{};
};

struct Blas {
  std::string provider;
  uint64_t id{};
  uint64_t revision{};
  RaySceneStorageIdentity storageIdentity{};
  bool opaque{};
  size_t vertexCount{};
  size_t indexCount{};
  std::unique_ptr<OwnedBuffer> vertices;
  std::unique_ptr<OwnedBuffer> indices;
  Structure structure;
};

void buildStructure(const vk::raii::Device &device,
                    container::gpu::VulkanMemoryManager &memory,
                    const vk::raii::CommandBuffer &command,
                    vk::AccelerationStructureTypeKHR type,
                    const vk::AccelerationStructureGeometryKHR &geometry,
                    uint32_t primitiveCount, uint32_t alignment,
                    Structure &result) {
  vk::AccelerationStructureBuildGeometryInfoKHR build{};
  build.type = type;
  build.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
  build.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  const auto sizes = device.getAccelerationStructureBuildSizesKHR(
      vk::AccelerationStructureBuildTypeKHR::eDevice, build, primitiveCount);
  result.storage = std::make_unique<OwnedBuffer>(
      memory, sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  vk::AccelerationStructureCreateInfoKHR create{};
  create.buffer = result.storage->buffer.buffer;
  create.size = sizes.accelerationStructureSize;
  create.type = type;
  result.handle = vk::raii::AccelerationStructureKHR(device, create);
  result.scratch = std::make_unique<OwnedBuffer>(
      memory, std::max<VkDeviceSize>(sizes.buildScratchSize, 1) + alignment - 1,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  const auto scratchAddress = result.scratch->address(device);
  build.scratchData.deviceAddress =
      (scratchAddress + alignment - 1) / alignment * alignment;
  build.dstAccelerationStructure = *result.handle;
  vk::AccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = primitiveCount;
  const vk::AccelerationStructureBuildRangeInfoKHR *rangePointer = &range;
  command.buildAccelerationStructuresKHR(build, rangePointer);
  result.address = device.getAccelerationStructureAddressKHR(
      vk::AccelerationStructureDeviceAddressInfoKHR(*result.handle));
}

void buildBarrier(const vk::raii::CommandBuffer &command, bool tracing) {
  vk::MemoryBarrier2 barrier{};
  barrier.srcStageMask =
      vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
  barrier.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
  barrier.dstStageMask =
      // Both depth-derived compute visibility and forward fragment queries
      // consume the generation after this build on the graphics queue.
      tracing ? vk::PipelineStageFlagBits2::eFragmentShader |
                    vk::PipelineStageFlagBits2::eComputeShader
              : vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
  barrier.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR;
  vk::DependencyInfo dependency{};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  command.pipelineBarrier2(dependency);
}

} // namespace

struct RaySceneGeneration::Impl {
  const vk::raii::Device *device{};
  std::vector<std::shared_ptr<Blas>> bottom;
  std::unique_ptr<OwnedBuffer> instances;
  Structure top;
  RaySceneBuildStats stats;
};
RaySceneGeneration::RaySceneGeneration() : impl_(std::make_unique<Impl>()) {}
RaySceneGeneration::~RaySceneGeneration() = default;
VkAccelerationStructureKHR RaySceneGeneration::tlas() const {
  return static_cast<VkAccelerationStructureKHR>(*impl_->top.handle);
}
const RaySceneBuildStats &RaySceneGeneration::stats() const {
  return impl_->stats;
}

VkDeviceSize RaySceneGeneration::retainedAllocatedBytes(
    std::span<const std::shared_ptr<const RaySceneGeneration>> generations) {
  std::unordered_set<VmaAllocation> seen;
  VkDeviceSize bytes = 0;
  auto add = [&](const auto &buffer) {
    if (buffer && seen.insert(buffer->buffer.allocation).second)
      bytes += buffer->buffer.allocation_info.size;
  };
  for (const auto &generation : generations) {
    if (!generation)
      continue;
    const auto &state = *generation->impl_;
    add(state.instances);
    add(state.top.storage);
    add(state.top.scratch);
    for (const auto &bottom : state.bottom) {
      add(bottom->vertices);
      add(bottom->indices);
      add(bottom->structure.storage);
      add(bottom->structure.scratch);
    }
  }
  return bytes;
}

RaySceneAcceleration::RaySceneAcceleration(
    const container::gpu::VulkanDevice &device,
    container::gpu::VulkanMemoryManager &memory)
    : device_(device), memory_(memory) {
  properties_.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  if (!supported())
    return;
  VkPhysicalDeviceProperties2 properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &properties_;
  vkGetPhysicalDeviceProperties2(device_.physicalDevice(), &properties);
}

std::shared_ptr<const RaySceneGeneration> RaySceneAcceleration::recordBuild(
    const vk::raii::CommandBuffer &command, const RaySceneInput &input,
    const std::shared_ptr<const RaySceneGeneration> &previous) const {
  if (!supported())
    throw std::runtime_error(
        "ray-query backend is disabled; use raster fallback");
  validateRayScene(input);
  if (input.instances.size() > properties_.maxInstanceCount)
    throw std::invalid_argument("ray scene exceeds device instance limit");
  for (const auto &geometry : input.geometries)
    if (geometry.indices.size() / 3 > properties_.maxPrimitiveCount)
      throw std::invalid_argument("ray scene exceeds device primitive limit");
  if (previous && previous->impl_->device != &device_.raii())
    throw std::invalid_argument(
        "ray scene generation belongs to another device");

  auto generation =
      std::shared_ptr<RaySceneGeneration>(new RaySceneGeneration());
  auto &state = *generation->impl_;
  state.device = &device_.raii();
  std::map<std::pair<std::string, uint64_t>, std::shared_ptr<Blas>> reusable;
  if (previous)
    for (const auto &bottom : previous->impl_->bottom)
      reusable.emplace(std::make_pair(bottom->provider, bottom->id), bottom);
  for (const auto &source : input.geometries) {
    std::shared_ptr<Blas> bottom;
    if (const auto found =
            reusable.find({source.provider.value, source.geometryId});
        found != reusable.end()) {
      const auto &candidate = found->second;
      if (candidate->revision == source.revision &&
          candidate->storageIdentity == source.storageIdentity &&
          candidate->opaque == source.opaque &&
          candidate->vertexCount == source.vertices.size() &&
          candidate->indexCount == source.indices.size())
        bottom = candidate;
    }
    if (bottom)
      ++state.stats.blasReused;
    else {
      bottom = std::make_shared<Blas>();
      bottom->provider = source.provider.value;
      bottom->id = source.geometryId;
      bottom->revision = source.revision;
      bottom->storageIdentity = source.storageIdentity;
      bottom->opaque = source.opaque;
      bottom->vertexCount = source.vertices.size();
      bottom->indexCount = source.indices.size();
      // Compact position-only inputs; shading attributes remain in the existing
      // provider buffers and can be bound by future query consumers.
      std::vector<std::array<float, 3>> positions;
      positions.reserve(source.vertices.size());
      for (const auto &vertex : source.vertices)
        positions.push_back(
            {vertex.position.x, vertex.position.y, vertex.position.z});
      bottom->vertices = std::make_unique<OwnedBuffer>(
          memory_, positions.size() * sizeof(positions[0]), inputUsage, true);
      bottom->vertices->write(positions.data(),
                              positions.size() * sizeof(positions[0]));
      bottom->indices = std::make_unique<OwnedBuffer>(
          memory_, source.indices.size_bytes(), inputUsage, true);
      bottom->indices->write(source.indices.data(),
                             source.indices.size_bytes());
      vk::AccelerationStructureGeometryKHR geometry{};
      geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
      geometry.flags = source.opaque ? vk::GeometryFlagBitsKHR::eOpaque
                                     : vk::GeometryFlagsKHR{};
      auto &triangles = geometry.geometry.triangles;
      triangles.sType =
          vk::StructureType::eAccelerationStructureGeometryTrianglesDataKHR;
      triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
      triangles.vertexData.deviceAddress =
          bottom->vertices->address(device_.raii());
      triangles.vertexStride = sizeof(positions[0]);
      triangles.maxVertex = static_cast<uint32_t>(positions.size() - 1);
      triangles.indexType = vk::IndexType::eUint32;
      triangles.indexData.deviceAddress =
          bottom->indices->address(device_.raii());
      buildStructure(device_.raii(), memory_, command,
                     vk::AccelerationStructureTypeKHR::eBottomLevel, geometry,
                     static_cast<uint32_t>(source.indices.size() / 3),
                     properties_.minAccelerationStructureScratchOffsetAlignment,
                     bottom->structure);
      ++state.stats.blasBuilt;
    }
    state.stats.accelerationBytes += bottom->structure.storage->size;
    state.stats.inputBytes += bottom->vertices->size + bottom->indices->size;
    state.stats.scratchBytes += bottom->structure.scratch->size;
    state.bottom.push_back(std::move(bottom));
  }
  buildBarrier(command, false);
  std::vector<VkAccelerationStructureInstanceKHR> instances;
  instances.reserve(input.instances.size());
  for (const auto &source : input.instances) {
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = rayInstanceTransform(source.transform);
    instance.instanceCustomIndex = source.customIndex;
    instance.mask = source.mask;
    instance.flags = rayInstanceFlags(source.doubleSided);
    instance.accelerationStructureReference =
        state.bottom[source.geometryIndex]->structure.address;
    instances.push_back(instance);
  }
  state.instances = std::make_unique<OwnedBuffer>(
      memory_, std::max<size_t>(instances.size(), 1) * sizeof(instances[0]),
      inputUsage, true);
  state.instances->write(instances.data(),
                         instances.size() * sizeof(instances[0]));
  vk::AccelerationStructureGeometryKHR geometry{};
  geometry.geometryType = vk::GeometryTypeKHR::eInstances;
  geometry.geometry.instances.sType =
      vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
  geometry.geometry.instances.data.deviceAddress =
      state.instances->address(device_.raii());
  buildStructure(device_.raii(), memory_, command,
                 vk::AccelerationStructureTypeKHR::eTopLevel, geometry,
                 static_cast<uint32_t>(instances.size()),
                 properties_.minAccelerationStructureScratchOffsetAlignment,
                 state.top);
  buildBarrier(command, true);
  state.stats.instanceCount = static_cast<uint32_t>(instances.size());
  state.stats.accelerationBytes += state.top.storage->size;
  state.stats.inputBytes += state.instances->size;
  state.stats.scratchBytes += state.top.scratch->size;
  state.stats.accelerationAllocatedBytes =
      state.top.storage->buffer.allocation_info.size;
  state.stats.inputAllocatedBytes =
      state.instances->buffer.allocation_info.size;
  state.stats.scratchAllocatedBytes =
      state.top.scratch->buffer.allocation_info.size;
  for (const auto &bottom : state.bottom) {
    state.stats.accelerationAllocatedBytes +=
        bottom->structure.storage->buffer.allocation_info.size;
    state.stats.inputAllocatedBytes +=
        bottom->vertices->buffer.allocation_info.size +
        bottom->indices->buffer.allocation_info.size;
    state.stats.scratchAllocatedBytes +=
        bottom->structure.scratch->buffer.allocation_info.size;
  }
  return generation;
}

} // namespace container::renderer
