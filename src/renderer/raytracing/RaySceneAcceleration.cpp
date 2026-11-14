#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/geometry/GltfOpacityMicromaps.h"
#include "Container/renderer/raytracing/RayInstanceTransformPass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace container::renderer {
namespace {

struct OwnedBuffer {
  container::gpu::VulkanMemoryManager &memory;
  container::gpu::AllocatedBuffer buffer{};
  VkDeviceSize size{};
  OwnedBuffer(container::gpu::VulkanMemoryManager &allocator,
              VkDeviceSize bytes, VkBufferUsageFlags usage, bool upload = false,
              std::span<const uint32_t> queueFamilies = {})
      : memory(allocator), size(bytes) {
    buffer = memory.createBuffer(
        std::max<VkDeviceSize>(bytes, 1), usage,
        upload ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
               : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT
               : 0,
        queueFamilies.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        queueFamilies);
  }
  ~OwnedBuffer() { memory.destroyBuffer(buffer); }
  OwnedBuffer(const OwnedBuffer &) = delete;
  OwnedBuffer &operator=(const OwnedBuffer &) = delete;
  void write(const void *data, size_t bytes, VkDeviceSize offset = 0) {
    if (offset > size || bytes > size - offset || buffer.allocation_info.pMappedData == nullptr)
      throw std::runtime_error("invalid ray scene upload");
    if (bytes == 0)
      return;
    std::memcpy(static_cast<std::byte*>(buffer.allocation_info.pMappedData) + offset, data, bytes);
    if (vmaFlushAllocation(memory.allocator(), buffer.allocation, offset, bytes) !=
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

using container::geometry::gltf::GltfOpacityMicromap;
using container::geometry::gltf::GltfOpacityMicromapAttachment;
using container::geometry::gltf::GltfOpacityMicromapTriangle;
using container::geometry::gltf::GltfOpacityMicromapUsage;
constexpr container::geometry::gltf::GltfOpacityMicromapLimits micromapLimits;
constexpr size_t micromapLevels = micromapLimits.maxSubdivisionLevel + 1;
using MicromapHistogram = std::array<uint32_t, 2 * micromapLevels>;

static_assert(sizeof(GltfOpacityMicromapTriangle) == sizeof(VkMicromapTriangleEXT));
static_assert(offsetof(GltfOpacityMicromapTriangle, subdivisionLevel) == offsetof(VkMicromapTriangleEXT, subdivisionLevel));
static_assert(offsetof(GltfOpacityMicromapTriangle, format) == offsetof(VkMicromapTriangleEXT, format));

std::optional<size_t> usageSlot(uint32_t format, uint32_t level,
    const VkPhysicalDeviceOpacityMicromapPropertiesEXT& props) {
  if (level >= micromapLevels) return {};
  if (format == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT && level <= props.maxOpacity2StateSubdivisionLevel)
    return level;
  if (format == VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT && level <= props.maxOpacity4StateSubdivisionLevel)
    return level + micromapLevels;
  return {};
}

bool validUsage(std::span<const GltfOpacityMicromapUsage> usage, const MicromapHistogram& expected,
                const VkPhysicalDeviceOpacityMicromapPropertiesEXT& props) {
  if (usage.size() > expected.size()) return false;
  MicromapHistogram actual{};
  for (const auto& item : usage) {
    const auto slot = usageSlot(item.format, item.subdivisionLevel, props);
    if (!slot || item.count == 0 || item.count > expected[*slot] - actual[*slot]) return false;
    actual[*slot] += item.count;
  }
  return actual == expected;
}

bool validMap(const GltfOpacityMicromap& map, const VkPhysicalDeviceOpacityMicromapPropertiesEXT& props) {
  if (map.data.empty() || map.data.size() > micromapLimits.maxPayloadBytes || map.triangles.empty() ||
      map.triangles.size() > micromapLimits.maxTriangleRecords) return false;
  MicromapHistogram expected{};
  for (const auto& triangle : map.triangles) {
    const auto slot = usageSlot(triangle.format, triangle.subdivisionLevel, props);
    if (!slot) return false;
    const uint64_t bits = uint64_t{1} << (2u * triangle.subdivisionLevel +
        (triangle.format == VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT));
    if (triangle.dataOffset > map.data.size() || (bits + 7) / 8 > map.data.size() - triangle.dataOffset)
      return false;
    ++expected[*slot];
  }
  return validUsage(map.usage, expected, props);
}

bool validAttachment(const GltfOpacityMicromapAttachment& attachment, size_t triangleCount,
    const VkPhysicalDeviceOpacityMicromapPropertiesEXT& props) {
  if (!attachment.map || attachment.indices.size() != triangleCount ||
      triangleCount > micromapLimits.maxGeometryTriangles) return false;
  MicromapHistogram expected{};
  for (const auto index : attachment.indices) {
    if (index >= static_cast<uint32_t>(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE_EXT))
      continue;
    if (index >= attachment.map->triangles.size()) return false;
    const auto& triangle = attachment.map->triangles[index];
    const auto slot = usageSlot(triangle.format, triangle.subdivisionLevel, props);
    if (!slot) return false;
    ++expected[*slot];
  }
  return validUsage(attachment.usage, expected, props);
}

std::vector<vk::MicromapUsageEXT> vulkanUsage(std::span<const GltfOpacityMicromapUsage> usage) {
  std::vector<vk::MicromapUsageEXT> result;
  result.reserve(usage.size());
  for (const auto& item : usage)
    result.emplace_back(item.count, item.subdivisionLevel, item.format);
  return result;
}

VkDeviceAddress alignedAddress(VkDeviceAddress address, VkDeviceSize alignment) {
  return (address + alignment - 1) / alignment * alignment;
}

std::unique_ptr<OwnedBuffer> micromapInput(const vk::raii::Device& device,
    container::gpu::VulkanMemoryManager& memory, const void* data, size_t bytes,
    VkDeviceSize alignment, VkDeviceAddress& address) {
  auto result = std::make_unique<OwnedBuffer>(memory, bytes + alignment - 1,
      inputUsage | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT, true);
  const auto base = result->address(device);
  address = alignedAddress(base, alignment);
  result->write(data, bytes, address - base);
  return result;
}

struct Micromap {
  std::shared_ptr<const GltfOpacityMicromap> source;
  std::vector<vk::MicromapUsageEXT> usage;
  std::unique_ptr<OwnedBuffer> data, triangles, storage, scratch;
  vk::raii::MicromapEXT handle{nullptr};
  VkDeviceAddress dataAddress{}, triangleAddress{}, scratchAddress{};
  bool recorded{};

  [[nodiscard]] vk::MicromapBuildInfoEXT buildInfo() const {
    vk::MicromapBuildInfoEXT build{};
    build.type = vk::MicromapTypeEXT::eOpacityMicromap;
    build.flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace;
    build.mode = vk::BuildMicromapModeEXT::eBuild;
    build.dstMicromap = *handle;
    build.usageCountsCount = static_cast<uint32_t>(usage.size());
    build.pUsageCounts = usage.data();
    build.data.deviceAddress = dataAddress;
    build.triangleArray.deviceAddress = triangleAddress;
    build.triangleArrayStride = sizeof(GltfOpacityMicromapTriangle);
    build.scratchData.deviceAddress = scratchAddress;
    return build;
  }
};

// Prepare every resource before recording. An optional map that fails sizing
// or allocation can then fall back without leaving dangling command inputs.
std::shared_ptr<Micromap> prepareMicromap(const vk::raii::Device& device,
    container::gpu::VulkanMemoryManager& memory, std::shared_ptr<const GltfOpacityMicromap> source,
    VkDeviceSize scratchAlignment, std::span<const uint32_t> queueFamilies) {
  auto result = std::make_shared<Micromap>();
  result->source = std::move(source);
  result->usage = vulkanUsage(result->source->usage);
  const auto sizes = device.getMicromapBuildSizesEXT(vk::AccelerationStructureBuildTypeKHR::eDevice, result->buildInfo());
  if (sizes.micromapSize == 0 || sizes.micromapSize > micromapLimits.maxPayloadBytes ||
      sizes.buildScratchSize > micromapLimits.maxPayloadBytes)
    return {};
  result->data = micromapInput(device, memory, result->source->data.data(), result->source->data.size(),
                               256, result->dataAddress);
  result->triangles = micromapInput(device, memory, result->source->triangles.data(),
      result->source->triangles.size() * sizeof(GltfOpacityMicromapTriangle), 256, result->triangleAddress);
  result->storage = std::make_unique<OwnedBuffer>(
      memory, sizes.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT,
      false, queueFamilies);
  if (sizes.buildScratchSize) {
    result->scratch = std::make_unique<OwnedBuffer>(memory, sizes.buildScratchSize + scratchAlignment - 1,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    result->scratchAddress = alignedAddress(result->scratch->address(device), scratchAlignment);
  }
  vk::MicromapCreateInfoEXT create{};
  create.type = vk::MicromapTypeEXT::eOpacityMicromap;
  create.buffer = result->storage->buffer.buffer;
  create.size = sizes.micromapSize;
  result->handle = vk::raii::MicromapEXT(device, create);
  return result;
}

void recordMicromap(const vk::raii::CommandBuffer& command, Micromap& map) {
  command.buildMicromapsEXT(map.buildInfo());
  const vk::MemoryBarrier2 barrier(vk::PipelineStageFlagBits2::eMicromapBuildEXT,
      vk::AccessFlagBits2::eMicromapWriteEXT, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
      vk::AccessFlagBits2::eMicromapReadEXT);
  vk::DependencyInfo dependency{};
  dependency.setMemoryBarriers(barrier);
  command.pipelineBarrier2(dependency);
  map.recorded = true;
}

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
  std::shared_ptr<const GltfOpacityMicromapAttachment> opacityAttachment;
  std::shared_ptr<Micromap> micromap;
  std::unique_ptr<OwnedBuffer> opacityIndices;
  // The BLAS is destroyed before the micromap it references.
  Structure structure;
};

void buildStructure(const vk::raii::Device &device,
                    container::gpu::VulkanMemoryManager &memory,
                    const vk::raii::CommandBuffer &command,
                    vk::AccelerationStructureTypeKHR type,
                    const vk::AccelerationStructureGeometryKHR &geometry,
                    uint32_t primitiveCount, uint32_t alignment,
                    Structure &result, std::span<const uint32_t> queueFamilies) {
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
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      false, queueFamilies);
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
  RayInstanceTransformPass::Publication gpuTransforms;
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
    const auto input=state.gpuTransforms.inputBuffer();
    if(input.allocation && seen.insert(input.allocation).second) bytes+=input.allocation_info.size;
    add(state.top.storage);
    add(state.top.scratch);
    for (const auto &bottom : state.bottom) {
      add(bottom->vertices);
      add(bottom->indices);
      add(bottom->opacityIndices);
      if (bottom->micromap) {
        add(bottom->micromap->data);
        add(bottom->micromap->triangles);
        add(bottom->micromap->storage);
        add(bottom->micromap->scratch);
      }
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
  if (device_.opacityMicromapsEnabled()) properties_.pNext = &opacityProperties_;
  vkGetPhysicalDeviceProperties2(device_.physicalDevice(), &properties);
}

std::shared_ptr<const RaySceneGeneration> RaySceneAcceleration::recordBuild(
    const vk::raii::CommandBuffer &command, const RaySceneInput &input,
    const std::shared_ptr<const RaySceneGeneration> &previous) const {
  if (!supported())
    throw std::runtime_error(
        "ray-query backend is disabled; use raster fallback");
  validateRayScene(input);
  if(input.gpuTransforms) {
    if(!input.gpuTransforms->producer) throw std::invalid_argument("GPU ray instance producer is missing");
    if(input.gpuTransforms->producer->device()!=device_.device())
      throw std::invalid_argument("GPU ray instance producer belongs to another device");
    input.gpuTransforms->producer->validate(*input.gpuTransforms,input.instances);
  }
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
  // DDGI can trace both AS levels and their attached micromaps from its
  // compute family. Keep sharing invariant across generations so an existing
  // BLAS is reusable when DDGI becomes active. Build inputs and scratch are
  // only accessed on the graphics family.
  const auto queueFamilies = device_.asyncComputeSharingFamilies();
  std::map<std::pair<std::string, uint64_t>, std::shared_ptr<Blas>> reusable;
  std::unordered_map<const GltfOpacityMicromap*, std::shared_ptr<Micromap>> maps;
  std::unordered_map<const GltfOpacityMicromap*, bool> mapValidity;
  std::unordered_set<const Micromap*> countedMaps;
  if (previous) {
    for (const auto &bottom : previous->impl_->bottom) {
      reusable.emplace(std::make_pair(bottom->provider, bottom->id), bottom);
      if (bottom->micromap) maps.try_emplace(bottom->micromap->source.get(), bottom->micromap);
    }
  }
  for (const auto &source : input.geometries) {
    const auto primitiveCount = static_cast<uint32_t>(source.indices.size() / 3);
    std::shared_ptr<const GltfOpacityMicromapAttachment> attachment;
    if (device_.opacityMicromapsEnabled() && !source.opaque && source.opacityMicromap &&
        validAttachment(*source.opacityMicromap, primitiveCount, opacityProperties_)) {
      const auto* map = source.opacityMicromap->map.get();
      const auto loaded = maps.find(map);
      if (loaded != maps.end()) {
        if (loaded->second) attachment = source.opacityMicromap;
      } else {
        auto [valid, inserted] = mapValidity.try_emplace(map);
        if (inserted) valid->second = validMap(*map, opacityProperties_);
        if (valid->second) attachment = source.opacityMicromap;
      }
    }
    std::shared_ptr<Blas> bottom;
    if (const auto found =
            reusable.find({source.provider.value, source.geometryId});
        found != reusable.end()) {
      const auto &candidate = found->second;
      if (candidate->revision == source.revision &&
          candidate->storageIdentity == source.storageIdentity &&
          candidate->opaque == source.opaque &&
          candidate->opacityAttachment == attachment &&
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
      vk::AccelerationStructureTrianglesOpacityMicromapEXT opacity{};
      std::vector<vk::MicromapUsageEXT> opacityUsage;
      if (attachment) {
        try {
          const auto& mapSource = attachment->map;
          auto [loaded, inserted] = maps.try_emplace(mapSource.get());
          if (inserted) loaded->second = prepareMicromap(device_.raii(), memory_, mapSource,
              properties_.minAccelerationStructureScratchOffsetAlignment, queueFamilies);
          if (loaded->second) {
            opacityUsage = vulkanUsage(attachment->usage);
            VkDeviceAddress address{};
            auto indices = micromapInput(device_.raii(), memory_, attachment->indices.data(),
                attachment->indices.size() * sizeof(uint32_t), sizeof(uint32_t), address);
            bottom->opacityAttachment = attachment;
            bottom->micromap = loaded->second;
            bottom->opacityIndices = std::move(indices);
            opacity.indexType = vk::IndexType::eUint32;
            opacity.indexBuffer.deviceAddress = address;
            opacity.indexStride = sizeof(uint32_t);
            opacity.usageCountsCount = static_cast<uint32_t>(opacityUsage.size());
            opacity.pUsageCounts = opacityUsage.empty() ? nullptr : opacityUsage.data();
            opacity.micromap = *bottom->micromap->handle;
            triangles.pNext = &opacity;
          }
        } catch (const std::exception&) {
          // No micromap commands have been recorded for this attachment yet.
          // Preserve the ordinary sampled-alpha build on optional resource failure.
        }
      }
      if (bottom->micromap && !bottom->micromap->recorded) {
        recordMicromap(command, *bottom->micromap);
        ++state.stats.opacityMicromapBuilt;
      }
      buildStructure(device_.raii(), memory_, command,
                     vk::AccelerationStructureTypeKHR::eBottomLevel, geometry,
                     primitiveCount,
                     properties_.minAccelerationStructureScratchOffsetAlignment,
                     bottom->structure, queueFamilies);
      ++state.stats.blasBuilt;
    }
    state.stats.accelerationBytes += bottom->structure.storage->size;
    state.stats.inputBytes += bottom->vertices->size + bottom->indices->size;
    state.stats.scratchBytes += bottom->structure.scratch->size;
    if (bottom->micromap) {
      ++state.stats.opacityMicromapAttachedBlas;
      state.stats.inputBytes += bottom->opacityIndices->size;
      if (countedMaps.insert(bottom->micromap.get()).second) {
        const auto& map = *bottom->micromap;
        ++state.stats.opacityMicromapCount;
        state.stats.opacityMicromapBytes += map.storage->size;
        state.stats.accelerationBytes += map.storage->size;
        state.stats.inputBytes += map.data->size + map.triangles->size;
        if (map.scratch) state.stats.scratchBytes += map.scratch->size;
      }
    } else if (source.opacityMicromap) {
      ++state.stats.opacityMicromapFallbackBlas;
    }
    state.bottom.push_back(std::move(bottom));
  }
  buildBarrier(command, false);
  std::vector<VkAccelerationStructureInstanceKHR> instances;
  instances.reserve(input.instances.size());
  for (const auto &source : input.instances) {
    VkAccelerationStructureInstanceKHR instance{};
    if(!input.gpuTransforms || source.gpuObjectIndex==UINT32_MAX)
      instance.transform = rayInstanceTransform(source.transform);
    else
      ++state.stats.gpuInstanceCount;
    instance.instanceCustomIndex = source.customIndex;
    instance.mask = source.mask;
    instance.flags = rayInstanceFlags(source.doubleSided, source.flipFacing);
    instance.accelerationStructureReference =
        state.bottom[source.geometryIndex]->structure.address;
    instances.push_back(instance);
  }
  state.instances = std::make_unique<OwnedBuffer>(
      memory_, std::max<size_t>(instances.size(), 1) * sizeof(instances[0]),
      inputUsage|(input.gpuTransforms?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:0), !input.gpuTransforms || instances.empty());
  if(input.gpuTransforms && !instances.empty()) {
    state.gpuTransforms=input.gpuTransforms->producer->record(*command,*input.gpuTransforms,input.instances,instances,
        {state.instances->buffer.buffer,0,state.instances->size});
    state.stats.gpuTransformUploadBytes=state.gpuTransforms.uploadedBytes();
  } else {
    state.instances->write(instances.data(),instances.size()*sizeof(instances[0]));
  }
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
                 state.top, queueFamilies);
  buildBarrier(command, true);
  state.stats.instanceCount = static_cast<uint32_t>(instances.size());
  state.stats.accelerationBytes += state.top.storage->size;
  state.stats.inputBytes += state.instances->size;
  state.stats.inputBytes += state.gpuTransforms.uploadedBytes();
  state.stats.scratchBytes += state.top.scratch->size;
  state.stats.accelerationAllocatedBytes =
      state.top.storage->buffer.allocation_info.size;
  state.stats.inputAllocatedBytes =
      state.instances->buffer.allocation_info.size;
  state.stats.inputAllocatedBytes += state.gpuTransforms.inputBuffer().allocation_info.size;
  state.stats.scratchAllocatedBytes =
      state.top.scratch->buffer.allocation_info.size;
  countedMaps.clear();
  for (const auto &bottom : state.bottom) {
    state.stats.accelerationAllocatedBytes +=
        bottom->structure.storage->buffer.allocation_info.size;
    state.stats.inputAllocatedBytes +=
        bottom->vertices->buffer.allocation_info.size +
        bottom->indices->buffer.allocation_info.size;
    state.stats.scratchAllocatedBytes +=
        bottom->structure.scratch->buffer.allocation_info.size;
    if (bottom->micromap) {
      state.stats.inputAllocatedBytes += bottom->opacityIndices->buffer.allocation_info.size;
      if (countedMaps.insert(bottom->micromap.get()).second) {
        const auto& map = *bottom->micromap;
        state.stats.accelerationAllocatedBytes += map.storage->buffer.allocation_info.size;
        state.stats.inputAllocatedBytes += map.data->buffer.allocation_info.size + map.triangles->buffer.allocation_info.size;
        if (map.scratch) state.stats.scratchAllocatedBytes += map.scratch->buffer.allocation_info.size;
      }
    }
  }
  return generation;
}

} // namespace container::renderer
