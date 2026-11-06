#include <Container/renderer/scene/GltfDeformationPass.h>

#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/ShaderModule.h>
#include <Container/utility/VulkanDevice.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {
using geometry::Vertex;
using gpu::AllocatedBuffer;
using Primitive = geometry::gltf::DeformationPrimitive;

// ByteAddressBuffers deliberately bypass std430 float3/float4 alignment. The
// production raster and BLAS vertex stream is the same tightly packed struct.
static_assert(sizeof(Vertex) == 72);
static_assert(offsetof(Vertex, colorAlpha) == 68);
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, color) == 12);
static_assert(offsetof(Vertex, texCoord) == 24);
static_assert(offsetof(Vertex, texCoord1) == 32);
static_assert(offsetof(Vertex, normal) == 40);
static_assert(offsetof(Vertex, tangent) == 52);
static_assert(sizeof(glm::mat4) == 64);
static_assert(sizeof(geometry::gltf::JointInfluence) == 8);

struct VertexMap {
  uint32_t job{std::numeric_limits<uint32_t>::max()};
  uint32_t localVertex{0};
  uint32_t influenceOffset{0};
  uint32_t influenceCount{0};
};
struct StaticJob {
  uint32_t morphOffset{0};
  uint32_t morphCount{0};
  uint32_t vertexCount{0};
  uint32_t reserved{0};
};
struct FrameJob {
  uint32_t paletteOffset{0};
  uint32_t previousPaletteOffset{0};
  uint32_t weightsOffset{0};
  uint32_t previousWeightsOffset{0};
  uint32_t paletteCount{0};
  uint32_t previousPaletteCount{0};
  uint32_t resetHistory{0};
  uint32_t reserved{0};
  uint32_t evaluatedOffset{0};
  uint32_t previousEvaluatedOffset{0};
  uint32_t hasEvaluated{0};
  uint32_t hasPreviousEvaluated{0};
};
struct PushConstants {
  uint32_t vertexCount{0};
  uint32_t mapOffset{0};
  uint32_t jobsOffset{0};
  uint32_t baseVertex{0};
};
static_assert(sizeof(VertexMap) == 16);
static_assert(sizeof(StaticJob) == 16);
static_assert(sizeof(FrameJob) == 48);
static_assert(sizeof(PushConstants) == 16);

[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("glTF GPU deformation: ") + message);
}

uint32_t narrow(size_t value) {
  if (value > std::numeric_limits<uint32_t>::max()) fail("packed data exceeds 32-bit shader addressing");
  return static_cast<uint32_t>(value);
}

uint32_t appendBytes(std::vector<std::byte>& bytes, const void* data, size_t count) {
  const uint32_t offset = narrow(bytes.size());
  if (count > std::numeric_limits<uint32_t>::max() - bytes.size()) {
    fail("packed data exceeds 32-bit shader addressing");
  }
  if (count != 0) {
    const auto* source = static_cast<const std::byte*>(data);
    bytes.insert(bytes.end(), source, source + count);
  }
  return offset;
}

template <typename T>
uint32_t append(std::vector<std::byte>& bytes, std::span<const T> values) {
  return appendBytes(bytes, values.data(), values.size_bytes());
}

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void validateWeights(std::span<const float> weights, size_t expected) {
  if (weights.size() != expected) fail("morph weight count differs from target count");
  for (const float weight : weights) {
    if (!std::isfinite(weight)) fail("non-finite morph weight");
  }
}

void validatePalette(std::span<const glm::mat4> palette, uint32_t jointCount) {
  if (!palette.empty() && palette.size() < jointCount) fail("skin palette does not cover vertex joint indices");
  for (const auto& matrix : palette) {
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        if (!std::isfinite(matrix[c][r])) fail("non-finite skin palette matrix");
      }
    }
    if (std::abs(matrix[0][3]) > 1e-5f || std::abs(matrix[1][3]) > 1e-5f ||
        std::abs(matrix[2][3]) > 1e-5f || std::abs(matrix[3][3] - 1) > 1e-5f) {
      fail("skin palette matrix must be affine");
    }
  }
}

void validateEvaluated(std::span<const Vertex> vertices, uint32_t expected) {
  if (vertices.empty()) return;
  if (vertices.size() != expected) fail("evaluated vertex count differs from instance vertex count");
  for (const auto& vertex : vertices) {
    if (!finite(vertex.position) || !finite(vertex.normal) ||
        !finite(glm::vec3(vertex.tangent)) || !std::isfinite(vertex.tangent.w) ||
        !std::isfinite(vertex.colorAlpha)) {
      fail("non-finite evaluated vertex frame");
    }
  }
}

gpu::VulkanMemoryManager& requireMemory(gpu::AllocationManager& allocator) {
  if (allocator.memoryManager() == nullptr) fail("allocation manager is not initialized");
  return *allocator.memoryManager();
}

}  // namespace

struct GltfDeformationPass::Impl {
  struct Instance {
    uint32_t morphCount{0};
    uint32_t requiredJoints{0};
    std::vector<float> defaultWeights;
    uint32_t vertexCount{0};
    bool requiresEvaluatedFrames{false};
  };
  struct Frame {
    AllocatedBuffer pose;
    VkDeviceSize poseCapacity{0};
    AllocatedBuffer current;
    AllocatedBuffer previous;
    VkDescriptorSet descriptors{VK_NULL_HANDLE};
    bool uploaded{false};
  };

  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;
  gpu::PipelineManager& pipelines;
  VkPhysicalDeviceProperties properties{};
  VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  gpu::ComputePipeline pipeline{};
  VkDescriptorPool pool{VK_NULL_HANDLE};
  AllocatedBuffer immutable;
  VkDeviceSize immutableBytes{0};
  std::vector<Instance> instances;
  std::vector<Frame> frames;
  uint32_t vertices{0};
  uint32_t mapOffset{0};
  uint32_t jobsOffset{0};

  Impl(std::shared_ptr<gpu::VulkanDevice> d, gpu::VulkanMemoryManager& m,
       gpu::PipelineManager& p)
      : device(std::move(d)), memory(m), pipelines(p) {
    if (!device) fail("device is null");
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
  }

  ~Impl() {
    clearFrames();
    memory.destroyBuffer(immutable);
    pipelines.destroyPipeline(pipeline);
    pipelines.destroyPipelineLayout(pipelineLayout);
    pipelines.destroyDescriptorSetLayout(setLayout);
  }

  void clearFrames() {
    pipelines.destroyDescriptorPool(pool);
    for (auto& frame : frames) {
      memory.destroyBuffer(frame.pose);
      memory.destroyBuffer(frame.current);
      memory.destroyBuffer(frame.previous);
    }
    frames.clear();
  }

  void checkStorageBytes(size_t bytes) const {
    narrow(bytes);
    if (bytes > properties.limits.maxStorageBufferRange) {
      fail("packed scene exceeds device maxStorageBufferRange");
    }
  }

  AllocatedBuffer hostBuffer(size_t bytes) {
    checkStorageBytes(bytes);
    return memory.createBuffer(std::max<size_t>(bytes, 16),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  void write(const AllocatedBuffer& buffer, const std::vector<std::byte>& bytes) {
    if (buffer.allocation_info.pMappedData == nullptr) fail("upload buffer is not mapped");
    if (!bytes.empty()) std::memcpy(buffer.allocation_info.pMappedData, bytes.data(), bytes.size());
    if (vmaFlushAllocation(memory.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
      fail("upload flush failed");
    }
  }

  void allocateOutputs() {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(vertices) * sizeof(Vertex);
    checkStorageBytes(static_cast<size_t>(bytes));
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (device->enabledVulkan12Features().bufferDeviceAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (device->rayQueriesEnabled()) usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    for (auto& frame : frames) {
      memory.destroyBuffer(frame.current);
      memory.destroyBuffer(frame.previous);
      frame.uploaded = false;
      if (bytes == 0) continue;
      frame.current = memory.createBuffer(bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      frame.previous = memory.createBuffer(bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    }
  }

  void createFrames(uint32_t count) {
    if (count == 0 || count > std::numeric_limits<uint32_t>::max() / 4) fail("invalid frame count");
    clearFrames();
    frames.resize(count);
    pool = pipelines.createDescriptorPool({{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * 4}}, count);
    std::vector<VkDescriptorSetLayout> layouts(count, setLayout);
    std::vector<VkDescriptorSet> sets(count);
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool;
    info.descriptorSetCount = count;
    info.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device->device(), &info, sets.data()) != VK_SUCCESS) {
      fail("descriptor set allocation failed");
    }
    for (size_t i = 0; i < frames.size(); ++i) frames[i].descriptors = sets[i];
    allocateOutputs();
  }

  void writeDescriptors(Frame& frame) {
    const VkDeviceSize outputBytes = static_cast<VkDeviceSize>(vertices) * sizeof(Vertex);
    const std::array<VkDescriptorBufferInfo, 4> buffers{{
        {immutable.buffer, 0, immutableBytes},
        {frame.pose.buffer, 0, frame.poseCapacity},
        {frame.current.buffer, 0, outputBytes},
        {frame.previous.buffer, 0, outputBytes}}};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = frame.descriptors;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1;
      writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[binding].pBufferInfo = &buffers[binding];
    }
    vkUpdateDescriptorSets(device->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }
};

GltfDeformationPass::GltfDeformationPass(std::shared_ptr<gpu::VulkanDevice> device,
    gpu::AllocationManager& allocator, gpu::PipelineManager& pipelines)
    : GltfDeformationPass(std::move(device), requireMemory(allocator), pipelines) {}

GltfDeformationPass::GltfDeformationPass(std::shared_ptr<gpu::VulkanDevice> device,
    gpu::VulkanMemoryManager& memory, gpu::PipelineManager& pipelines)
    : impl_(std::make_unique<Impl>(std::move(device), memory, pipelines)) {}

GltfDeformationPass::~GltfDeformationPass() = default;

void GltfDeformationPass::createResources(const std::filesystem::path& shaderDir,
                                         uint32_t frameCount) {
  auto& state = *impl_;
  if (state.pipeline == VK_NULL_HANDLE) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < 4; ++i) {
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    state.setLayout = state.pipelines.createDescriptorSetLayout(bindings, {});
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    state.pipelineLayout = state.pipelines.createPipelineLayout({state.setLayout}, {range});
    state.pipeline = state.pipelines.createComputePipeline(
        shaderDir / "spv_shaders" / "gltf_deformation.comp.spv", state.pipelineLayout, "gltf_deformation");
  }
  state.createFrames(frameCount);
}

void GltfDeformationPass::setScene(std::span<const Vertex> initialVertices,
                                  std::span<const GltfDeformationInstance> instances) {
  auto& state = *impl_;
  if (state.pipeline == VK_NULL_HANDLE || state.frames.empty()) fail("createResources must precede setScene");
  const uint32_t vertexCount = narrow(initialVertices.size());
  state.checkStorageBytes(initialVertices.size_bytes());
  std::vector<std::byte> bytes;
  append(bytes, initialVertices);
  std::vector<VertexMap> vertexMaps(vertexCount);
  std::vector<StaticJob> jobs(instances.size());
  const uint32_t mapOffset = append(bytes, std::span<const VertexMap>(vertexMaps));
  const uint32_t jobsOffset = append(bytes, std::span<const StaticJob>(jobs));
  std::vector<Impl::Instance> converted;
  converted.reserve(instances.size());
  struct CachedPrimitive {
    uint32_t morphOffset{0};
    uint32_t requiredJoints{0};
    std::vector<std::pair<uint32_t, uint32_t>> influences;
  };
  std::map<const Primitive*, CachedPrimitive> cache;
  for (size_t job = 0; job < instances.size(); ++job) {
    const auto& instance = instances[job];
    if (!instance.primitive) fail("instance primitive is null");
    const auto& primitive = *instance.primitive;
    const size_t count = primitive.bindVertices.size();
    if (count == 0 || instance.outputVertexOffset > vertexCount ||
        count > vertexCount - instance.outputVertexOffset) fail("instance vertex range is outside the scene");
    if (!primitive.influences.empty() && primitive.influences.size() != count) fail("influence vertex count mismatch");
    validateWeights(primitive.meshDefaultWeights, primitive.morphTargets.size());
    auto found = cache.find(&primitive);
    if (found == cache.end()) {
      CachedPrimitive packed;
      packed.influences.resize(count);
      for (size_t i = 0; i < count; ++i) {
        const auto& vertex = primitive.bindVertices[i];
        if (!finite(vertex.position) || !finite(vertex.normal) ||
            !finite(glm::vec3(vertex.tangent)) || !std::isfinite(vertex.tangent.w) ||
            !std::isfinite(vertex.colorAlpha)) fail("non-finite bind vertex");
        if (primitive.influences.empty()) continue;
        const auto& influences = primitive.influences[i].values;
        double sum = 0.0;
        for (const auto& influence : influences) {
          if (influence.joint == std::numeric_limits<uint32_t>::max() ||
              !std::isfinite(influence.weight) || influence.weight < 0) fail("invalid skin influence");
          packed.requiredJoints = std::max(packed.requiredJoints, influence.joint + 1);
          sum += influence.weight;
        }
        if (!(sum > 0) || !std::isfinite(sum)) fail("vertex has no positive finite influence weight");
        // Normalize in double exactly as the import/reference path, including
        // directly authored primitives used by numerical regression fixtures.
        std::vector<geometry::gltf::JointInfluence> normalized = influences;
        for (auto& influence : normalized) influence.weight = static_cast<float>(influence.weight / sum);
        packed.influences[i] = {append(bytes, std::span<const geometry::gltf::JointInfluence>(normalized)), narrow(influences.size())};
      }
      packed.morphOffset = narrow(bytes.size());
      for (const auto& target : primitive.morphTargets) {
        for (const auto* values : {&target.positions, &target.normals, &target.tangents}) {
          if (!values->empty() && values->size() != count) fail("morph target vertex count mismatch");
          for (const auto delta : *values) if (!finite(delta)) fail("non-finite morph delta");
        }
        for (size_t i = 0; i < count; ++i) {
          for (const auto* values : {&target.positions, &target.normals, &target.tangents}) {
            const glm::vec3 delta = values->empty() ? glm::vec3(0) : (*values)[i];
            appendBytes(bytes, &delta, sizeof(delta));
          }
        }
      }
      found = cache.emplace(&primitive, std::move(packed)).first;
    }
    const auto& packed = found->second;
    jobs[job] = {packed.morphOffset, narrow(primitive.morphTargets.size()), narrow(count), 0};
    converted.push_back({narrow(primitive.morphTargets.size()), packed.requiredJoints,
                          primitive.meshDefaultWeights, narrow(count),
                          primitive.hasMorphing() && (!primitive.authoredBaseNormal || !primitive.authoredBaseTangent)});
    for (size_t local = 0; local < count; ++local) {
      const size_t output = instance.outputVertexOffset + local;
      if (vertexMaps[output].job != std::numeric_limits<uint32_t>::max()) fail("instance vertex ranges overlap");
      vertexMaps[output] = {narrow(job), narrow(local), packed.influences[local].first,
                             packed.influences[local].second};
    }
    std::memcpy(bytes.data() + static_cast<size_t>(instance.outputVertexOffset) * sizeof(Vertex),
                  primitive.bindVertices.data(), count * sizeof(Vertex));
  }
  if (!vertexMaps.empty()) std::memcpy(bytes.data() + mapOffset, vertexMaps.data(), vertexMaps.size() * sizeof(VertexMap));
  if (!jobs.empty()) std::memcpy(bytes.data() + jobsOffset, jobs.data(), jobs.size() * sizeof(StaticJob));
  state.checkStorageBytes(bytes.size());
  AllocatedBuffer replacement;
  if (!bytes.empty()) {
    replacement = state.hostBuffer(bytes.size());
    try {
      state.write(replacement, bytes);
    } catch (...) {
      state.memory.destroyBuffer(replacement);
      throw;
    }
  }
  state.memory.destroyBuffer(state.immutable);
  state.immutable = replacement;
  state.immutableBytes = bytes.size();
  state.instances = std::move(converted);
  state.vertices = vertexCount;
  state.mapOffset = mapOffset;
  state.jobsOffset = jobsOffset;
  state.allocateOutputs();
}

void GltfDeformationPass::uploadFrame(uint32_t frameIndex,
                                     std::span<const GltfDeformationPose> poses) {
  auto& state = *impl_;
  if (frameIndex >= state.frames.size() || poses.size() != state.instances.size()) fail("frame or instance pose count mismatch");
  auto& frame = state.frames[frameIndex];
  frame.uploaded = false;
  std::vector<FrameJob> jobs(poses.size());
  std::vector<std::byte> bytes;
  append(bytes, std::span<const FrameJob>(jobs));
  for (size_t i = 0; i < poses.size(); ++i) {
    const auto& pose = poses[i];
    const auto& instance = state.instances[i];
    if (instance.requiresEvaluatedFrames && pose.evaluatedVertices.empty()) {
      fail("missing base frames require evaluated current vertices");
    }
    const std::span<const float> currentWeights = pose.morphWeights.empty()
        ? std::span<const float>(instance.defaultWeights) : pose.morphWeights;
    const std::span<const float> previousWeights = pose.previousMorphWeights.empty() || pose.resetHistory
        ? currentWeights : pose.previousMorphWeights;
    const std::span<const glm::mat4> previousPalette = pose.previousSkinPalette.empty() || pose.resetHistory
        ? pose.skinPalette : pose.previousSkinPalette;
    validateWeights(currentWeights, instance.morphCount);
    validateWeights(previousWeights, instance.morphCount);
    validatePalette(pose.skinPalette, instance.requiredJoints);
    validatePalette(previousPalette, instance.requiredJoints);
    const auto previousEvaluated = pose.previousEvaluatedVertices.empty() || pose.resetHistory
        ? pose.evaluatedVertices : pose.previousEvaluatedVertices;
    validateEvaluated(pose.evaluatedVertices, instance.vertexCount);
    validateEvaluated(previousEvaluated, instance.vertexCount);
    auto& job = jobs[i];
    job.paletteOffset = append(bytes, pose.skinPalette);
    job.previousPaletteOffset = append(bytes, previousPalette);
    job.weightsOffset = append(bytes, currentWeights);
    job.previousWeightsOffset = append(bytes, previousWeights);
    job.paletteCount = narrow(pose.skinPalette.size());
    job.previousPaletteCount = narrow(previousPalette.size());
    job.resetHistory = pose.resetHistory ? 1u : 0u;
    job.evaluatedOffset = append(bytes, pose.evaluatedVertices);
    job.previousEvaluatedOffset = previousEvaluated.data() == pose.evaluatedVertices.data()
        ? job.evaluatedOffset : append(bytes, previousEvaluated);
    job.hasEvaluated = pose.evaluatedVertices.empty() ? 0u : 1u;
    job.hasPreviousEvaluated = previousEvaluated.empty() ? 0u : 1u;
  }
  if (!jobs.empty()) std::memcpy(bytes.data(), jobs.data(), jobs.size() * sizeof(FrameJob));
  const size_t required = std::max<size_t>(bytes.size(), 16);
  state.checkStorageBytes(required);
  if (frame.poseCapacity < required) {
    state.memory.destroyBuffer(frame.pose);
    frame.poseCapacity = 0;
    frame.pose = state.hostBuffer(required);
    frame.poseCapacity = required;
  }
  state.write(frame.pose, bytes);
  if (state.vertices != 0) state.writeDescriptors(frame);
  frame.uploaded = true;
}

void GltfDeformationPass::record(VkCommandBuffer command, uint32_t frameIndex) {
  auto& state = *impl_;
  if (frameIndex >= state.frames.size() || !state.frames[frameIndex].uploaded) fail("frame pose has not been uploaded");
  if (state.vertices == 0) return;
  if (!command || !isReady()) fail("pass or command buffer is not ready");
  // Covers immutable/pose host writes and output reuse after every possible
  // graphics, compute, transfer and AS reader on this graphics queue.
  VkMemoryBarrier input{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  input.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  input.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input, 0, nullptr, 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
      0, 1, &state.frames[frameIndex].descriptors, 0, nullptr);
  const uint64_t maxThreads = static_cast<uint64_t>(state.properties.limits.maxComputeWorkGroupCount[0]) * 64;
  for (uint64_t base = 0; base < state.vertices; base += maxThreads) {
    const uint64_t remaining = std::min<uint64_t>(state.vertices - base, maxThreads);
    const PushConstants constants{state.vertices, state.mapOffset, state.jobsOffset, static_cast<uint32_t>(base)};
    vkCmdPushConstants(command, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(command, static_cast<uint32_t>((remaining + 63) / 64), 1, 1);
  }
  VkMemoryBarrier output{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  output.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  VkPipelineStageFlags destinations = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
      VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (state.device->rayQueriesEnabled()) {
    destinations |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    output.dstAccessMask |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  }
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, destinations,
      0, 1, &output, 0, nullptr, 0, nullptr);
}

gpu::BufferSlice GltfDeformationPass::currentVertices(uint32_t frameIndex) const {
  if (frameIndex >= impl_->frames.size()) return {};
  return {impl_->frames[frameIndex].current.buffer, 0,
            static_cast<VkDeviceSize>(impl_->vertices) * sizeof(Vertex)};
}

gpu::BufferSlice GltfDeformationPass::previousVertices(uint32_t frameIndex) const {
  if (frameIndex >= impl_->frames.size()) return {};
  return {impl_->frames[frameIndex].previous.buffer, 0,
            static_cast<VkDeviceSize>(impl_->vertices) * sizeof(Vertex)};
}

uint32_t GltfDeformationPass::vertexCount() const { return impl_->vertices; }
size_t GltfDeformationPass::instanceCount() const { return impl_->instances.size(); }
bool GltfDeformationPass::isReady() const {
  return impl_->pipeline != VK_NULL_HANDLE && impl_->immutable.buffer != VK_NULL_HANDLE &&
      impl_->vertices != 0 && !impl_->frames.empty() &&
      std::all_of(impl_->frames.begin(), impl_->frames.end(), [](const Impl::Frame& frame) {
        return frame.current.buffer != VK_NULL_HANDLE && frame.previous.buffer != VK_NULL_HANDLE;
      });
}

}  // namespace container::renderer
