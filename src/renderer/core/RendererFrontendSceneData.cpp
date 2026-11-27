#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/common/VulkanObjects.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/SceneDataCapture.h"
#include "Container/renderer/core/SensorCameraDataset.h"
#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <glm/matrix.hpp>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace container::renderer {
namespace {

constexpr uint32_t kSceneDataGroupSize = 8;
using BenchmarkClock = std::chrono::steady_clock;

double milliseconds(BenchmarkClock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

struct SceneDataPushConstants {
  glm::mat4 inverseProjection;
  uint32_t width;
  uint32_t height;
  float depthScale;
  uint32_t bimSensor;
};
static_assert(offsetof(SceneDataPushConstants, width) == 64);
static_assert(offsetof(SceneDataPushConstants, height) == 68);

}  // namespace

void RendererFrontend::prepareSceneDataCapture(FrameRecordParams& frame,
                                              const glm::mat4& projection) {
  const auto extent = frame.screenshot.extent;
  const bool sensorFrame = sensorCameraCapture_.requested.has_value();
  if (frame.runtime.activeTechnique != RenderTechniqueId::DeferredRaster ||
      msaaSampleCount_ != VK_SAMPLE_COUNT_1_BIT || subs_.temporalManager->settings().enabled ||
      subs_.temporalManager->upscalingActive() ||
      extent.width != frame.runtime.sceneExtent.width || extent.height != frame.runtime.sceneExtent.height)
    throw std::runtime_error("Scene data requires Deferred raster, 1× MSAA and native resolution with TAA disabled");
  auto extension = container::util::pathToUtf8(
      container::util::pathFromUtf8(activePrimaryModelPath_).extension());
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (!sensorFrame && ((extension != ".gltf" && extension != ".glb") ||
      (subs_.bimManager && subs_.bimManager->hasScene()) || frame.debug.fullWireframe() ||
      frame.scene.diagCubeObjectIndex != std::numeric_limits<uint32_t>::max()))
    throw std::runtime_error("Scene data requires a filled primary glTF scene without auxiliary models");
  if (sensorFrame &&
      (extent.width != svc_.config.sensorCameraPlayback->settings.width ||
       extent.height != svc_.config.sensorCameraPlayback->settings.height ||
       !subs_.bimManager || subs_.bimManager->metadataRevision() != sensorCameraCapture_.bimRevision))
    throw std::runtime_error("Camera dataset resolution or BIM model changed during capture");

  auto& capture = sceneDataCapture_;
  const auto device = svc_.ctx.deviceWrapper->device();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(svc_.ctx.deviceWrapper->physicalDevice(), &properties);
  const VkDeviceSize pixelCount = VkDeviceSize{extent.width} * extent.height;
  if (pixelCount == 0 || pixelCount > properties.limits.maxStorageBufferRange / sizeof(SceneDataCapturePixel))
    throw std::runtime_error("Scene data capture exceeds the Vulkan storage buffer size limit");
  const auto byteCount = pixelCount * sizeof(SceneDataCapturePixel);

  const std::array views{
      deferredRasterImageView(frame, DeferredRasterImageId::DepthSamplingView),
      deferredRasterImageView(frame, DeferredRasterImageId::Normal),
      deferredRasterImageView(frame, DeferredRasterImageId::PickId)};
  if (views[0] == VK_NULL_HANDLE || views[1] == VK_NULL_HANDLE || views[2] == VK_NULL_HANDLE)
    throw std::runtime_error("Scene data capture could not access the Deferred G-buffer");

  if (!capture.pipeline) {
    try {
      const auto image = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      const auto compute = VK_SHADER_STAGE_COMPUTE_BIT;
      capture.descriptorLayout = svc_.pipelineManager.createDescriptorSetLayout({
          {0, image, 1, compute, nullptr}, {1, image, 1, compute, nullptr},
          {2, image, 1, compute, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute, nullptr}}, {});
      capture.pipelineLayout = svc_.pipelineManager.createPipelineLayout(
          {capture.descriptorLayout}, {{compute, 0, sizeof(SceneDataPushConstants)}});
      capture.pipeline = svc_.pipelineManager.createComputePipeline(
          container::util::executableDirectory() / "spv_shaders" / "scene_data_capture.comp.spv",
          capture.pipelineLayout, "scene-data-capture");
      const uint32_t slotCount = svc_.config.maxFramesInFlight;
      capture.descriptorPool = svc_.pipelineManager.createDescriptorPool(
          {{image, 3 * slotCount}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slotCount}}, slotCount);
      std::vector<VkDescriptorSetLayout> layouts(slotCount, capture.descriptorLayout);
      std::vector<VkDescriptorSet> sets(slotCount);
      VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocation.descriptorPool = capture.descriptorPool;
      allocation.descriptorSetCount = slotCount;
      allocation.pSetLayouts = layouts.data();
      if (vkAllocateDescriptorSets(device, &allocation, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate scene data capture descriptor sets");
      capture.slots.resize(slotCount);
      for (uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex)
        capture.slots[slotIndex].descriptorSet = sets[slotIndex];
    } catch (...) {
      destroySceneDataCapture();
      throw;
    }
  }

  const uint32_t frameSlot = frame.runtime.frameSlot;
  if (sensorFrame && sensorCameraCapture_.benchmark)
    frame.screenshot.sensorTimestamps = {sensorCameraCapture_.benchmark->queryPool,
                                       frameSlot * FrameSensorReadbackTimestamps::Count};
  auto& slot = capture.slots.at(frameSlot);
  slot.recorded = false;
  auto& readback = slot.readback;
  if (!readback.readbackBuffer.buffer || readback.readbackSize != byteCount) {
    auto replacement = svc_.allocationManager.createBuffer(
        byteCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (readback.readbackBuffer.buffer)
      svc_.allocationManager.destroyBuffer(readback.readbackBuffer);
    readback.readbackBuffer = replacement;
    readback.readbackSize = byteCount;
  }
  readback.extent = extent;

  const std::array<VkDescriptorImageInfo, 3> images{{
      {VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL},
      {VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, views[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
  const VkDescriptorBufferInfo output{readback.readbackBuffer.buffer, 0, byteCount};
  std::array<VkWriteDescriptorSet, 4> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding) {
    auto& write = writes[binding];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = slot.descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    if (binding < images.size()) {
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &images[binding];
    } else {
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &output;
    }
  }
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  const auto depthScale = sensorFrame ? static_cast<float>(sensorCameraCapture_.metersPerRendererUnit) : 1.0f;
  const SceneDataPushConstants push{glm::inverse(projection), extent.width, extent.height, depthScale,
                                    sensorFrame ? 1u : 0u};
  frame.services.sceneDataCapture = [this, push, frameSlot](VkCommandBuffer command, const FrameRecordParams&) {
    auto& state = sceneDataCapture_;
    auto& frameData = state.slots[frameSlot];
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
                            0, 1, &frameData.descriptorSet, 0, nullptr);
    vkCmdPushConstants(command, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, (push.width + kSceneDataGroupSize - 1) / kSceneDataGroupSize,
                  (push.height + kSceneDataGroupSize - 1) / kSceneDataGroupSize, 1);
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = frameData.readback.readbackBuffer.buffer;
    barrier.size = frameData.readback.readbackSize;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
    frameData.recorded = true;
  };
}

void RendererFrontend::writeSceneDataCapture(uint32_t frameSlot) {
  const auto& slot = sceneDataCapture_.slots.at(frameSlot);
  const auto& readback = slot.readback;
  if (!slot.recorded)
    throw std::runtime_error("The Deferred G-buffer did not record scene data for this frame");
  const auto& buffer = readback.readbackBuffer;
  // The screenshot fence has completed both the data pass and the RGB readback.
  const auto* pixels = static_cast<const SceneDataCapturePixel*>(buffer.allocation_info.pMappedData);
  if (!pixels || vmaInvalidateAllocation(svc_.allocationManager.memoryManager()->allocator(),
          buffer.allocation, 0, readback.readbackSize) != VK_SUCCESS)
    throw std::runtime_error("Failed to read the scene data capture buffer");
  const auto encoded = encodeSceneDataCapture(
      {pixels, size_t(readback.extent.width) * readback.extent.height},
      readback.extent.width, readback.extent.height, *subs_.sceneController, sceneGraph_);
  container::util::writeFileAtomically(sceneDataExrPath(screenshot_.outputPath), encoded.exrBytes());
  container::util::writeFileAtomically(sceneDataLookupPath(screenshot_.outputPath), encoded.lookupJson);
}

void RendererFrontend::destroySceneDataCapture() {
  auto& capture = sceneDataCapture_;
  svc_.pipelineManager.destroyDescriptorPool(capture.descriptorPool);
  svc_.pipelineManager.destroyPipeline(capture.pipeline);
  svc_.pipelineManager.destroyPipelineLayout(capture.pipelineLayout);
  svc_.pipelineManager.destroyDescriptorSetLayout(capture.descriptorLayout);
  for (auto& slot : capture.slots)
    if (slot.readback.readbackBuffer.buffer)
      svc_.allocationManager.destroyBuffer(slot.readback.readbackBuffer);
  if (auto& benchmark = sensorCameraCapture_.benchmark; benchmark && benchmark->queryPool) {
    destroyOwnedQueryPool(svc_.ctx.deviceWrapper->device(), benchmark->queryPool, nullptr);
    benchmark->queryPool = VK_NULL_HANDLE;
  }
  capture = {};
}

void RendererFrontend::initializeSensorCameraBenchmark() {
  auto& benchmark = sensorCameraCapture_.benchmark.emplace();
  benchmark.samples.reserve(svc_.config.sensorCameraPlayback->frames.size());
  const auto& device = *svc_.ctx.deviceWrapper;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
  const auto graphicsFamily = device.queueFamilyIndices().graphicsFamily.value();
  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &familyCount, families.data());
  if (graphicsFamily >= familyCount || families[graphicsFamily].timestampValidBits == 0 ||
      !std::isfinite(properties.limits.timestampPeriod) || properties.limits.timestampPeriod <= 0)
    return;
  benchmark.timestampValidBits = families[graphicsFamily].timestampValidBits;
  benchmark.timestampPeriodNs = properties.limits.timestampPeriod;
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = svc_.config.maxFramesInFlight * FrameSensorReadbackTimestamps::Count;
  if (createOwnedQueryPool(device.device(), &info, nullptr, &benchmark.queryPool) != VK_SUCCESS) {
    benchmark.queryPool = VK_NULL_HANDLE;
    benchmark.timestampStatus = "could not create camera benchmark timestamp queries";
    return;
  }
  benchmark.timestampStatus = "using camera readback timestamps";
}

void RendererFrontend::beginSensorCameraCapture(const std::filesystem::path& directory) {
  auto& capture = sensorCameraCapture_;
  if (!svc_.config.sensorCameraPlayback || !capture.directory.empty())
    throw std::logic_error("Sensor camera playback is unavailable or has already started");
  const auto* bim = subs_.bimManager.get();
  const auto* labels = bim ? bim->sensorLabels() : nullptr;
  if (!labels || !labels->metersPerRendererUnit)
    throw std::runtime_error("Sensor camera capture requires BIM length units or --sensor-meters-per-renderer-unit");
  if (!labels->sceneSource)
    throw std::runtime_error("Sensor camera capture requires BIM source fingerprints");
  if (bim->objectData().empty())
    throw std::runtime_error("Sensor camera capture requires drawable BIM objects");

  capture.labels.resize(bim->objectData().size());
  for (uint32_t index = 0; index < capture.labels.size(); ++index) {
    const auto metadata = bim->metadataForObject(index);
    if (metadata && metadata->sourceElementIndex < labels->elements.size())
      capture.labels[index] = labels->elements[metadata->sourceElementIndex];
  }
  capture.metersPerRendererUnit = *labels->metersPerRendererUnit;
  capture.sceneSource = *labels->sceneSource;
  if (const auto& appearance = svc_.config.sensorCameraPlayback->settings.appearance)
    capture.lighting = robotics::sampleSensorLighting(*appearance);
  const auto depthScale = static_cast<float>(capture.metersPerRendererUnit);
  if (!std::isfinite(depthScale) || depthScale <= 0.0f)
    throw std::runtime_error("BIM metric scale cannot be represented in the camera pass");
  capture.bimRevision = bim->metadataRevision();
  capture.inFlight.resize(svc_.config.maxFramesInFlight);
  capture.directory = directory;
  if (!svc_.config.sensorBenchmarkPath.empty())
    initializeSensorCameraBenchmark();
  container::util::writeFileAtomically(directory / "labels.json",
      container::robotics::serializeBimSensorLabelManifest(*labels),
      container::util::AtomicFilePublication::CreateNew);
}

void RendererFrontend::requestSensorCameraFrame(
    const container::robotics::SensorCameraFrame& frame) {
  auto& capture = sensorCameraCapture_;
  if (capture.directory.empty() || capture.requested || screenshot_.pending)
    throw std::logic_error("Sensor capture has no session or already has a pending frame");

  const auto& settings = svc_.config.sensorCameraPlayback->settings;
  // Undo the REP-103 world axis permutation. Scale the camera origin only:
  // its basis stays rigid, so the projection and depth shader handle units.
  const auto rendererBasis = [](glm::dvec3 axis) {
    return glm::dvec3{-axis.y, axis.z, -axis.x};
  };
  glm::dmat4 rendererFromCamera(1.0);
  for (int axis = 0; axis < 3; ++axis)
    rendererFromCamera[axis] = glm::dvec4(rendererBasis(frame.worldFromCamera[axis]), 0.0);
  rendererFromCamera[3] = glm::dvec4(rendererBasis(frame.worldFromCamera[3]) /
                                     capture.metersPerRendererUnit, 1.0);

  container::geometry::gltf::AnimatedCameraState camera;
  camera.view = glm::mat4(glm::inverse(rendererFromCamera));
  camera.projection = glm::mat4(
      container::robotics::sensorCameraProjection(settings, capture.metersPerRendererUnit));
  camera.shadowProjection = camera.projection;
  camera.nearPlane = static_cast<float>(settings.nearMeters / capture.metersPerRendererUnit);
  camera.farPlane = static_cast<float>(settings.farMeters / capture.metersPerRendererUnit);
  if (!std::isfinite(camera.farPlane) || camera.nearPlane <= 0.0f ||
      camera.farPlane <= camera.nearPlane)
    throw std::runtime_error("Sensor camera clipping range exceeds renderer precision");
  camera.data.viewProj = camera.projection * camera.view;
  camera.data.inverseViewProj = glm::mat4(glm::inverse(glm::dmat4(camera.data.viewProj)));
  camera.data.unjitteredViewProj = camera.data.viewProj;
  camera.data.cameraWorldPosition = glm::vec4(rendererFromCamera[3]);
  camera.data.cameraForward = glm::vec4(-glm::dvec3(rendererFromCamera[2]), 0.0);
  for (const auto* matrix : {&camera.projection, &camera.view, &camera.data.inverseViewProj})
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 4; ++row)
        if (!std::isfinite((*matrix)[column][row]))
          throw std::runtime_error("Sensor camera pose or projection exceeds renderer precision");
  capture.camera = camera;
  capture.requested = frame;
  screenshot_.sceneLinear = true;
  screenshot_.pending = true;
}

const container::geometry::gltf::AnimatedCameraState*
RendererFrontend::renderCameraOverride() const noexcept {
  if (sensorCameraCapture_.camera)
    return &*sensorCameraCapture_.camera;
  return gltfAnimationCamera_ ? &*gltfAnimationCamera_ : nullptr;
}

void RendererFrontend::submitSensorCameraFrame(uint32_t frameSlot) {
  auto& capture = sensorCameraCapture_;
  auto& slot = capture.inFlight.at(frameSlot);
  if (slot || !capture.requested || !sceneDataCapture_.slots.at(frameSlot).recorded)
    throw std::logic_error("Sensor camera readback slot was not ready for submission");
  slot = capture.requested;
  capture.requested.reset();
  screenshot_.pending = false;
}

void RendererFrontend::retireSensorCameraFrame(uint32_t frameSlot) {
  auto& capture = sensorCameraCapture_;
  if (frameSlot >= capture.inFlight.size() || !capture.inFlight[frameSlot])
    return;
  const auto& color = screenshot_.readbacks.at(frameSlot);
  const auto& surfaces = sceneDataCapture_.slots.at(frameSlot).readback;
  const auto allocator = svc_.allocationManager.memoryManager()->allocator();
  const auto mappedReadback = [allocator](const HostReadbackSlot& slot) -> const void* {
    const auto& buffer = slot.readbackBuffer;
    const void* pixels = buffer.allocation_info.pMappedData;
    if (!pixels || vmaInvalidateAllocation(allocator, buffer.allocation, 0, slot.readbackSize) != VK_SUCCESS)
      throw std::runtime_error("Failed to read the retired sensor camera frame");
    return pixels;
  };
  const auto& settings = svc_.config.sensorCameraPlayback->settings;
  const size_t pixels = size_t(settings.width) * settings.height;
  const auto& frame = *capture.inFlight[frameSlot];
  const auto timeNow = [&] {
    return capture.benchmark ? BenchmarkClock::now() : BenchmarkClock::time_point{};
  };
  const auto rgbStart = timeNow();
  const auto* rgb = static_cast<const float*>(mappedReadback(color));
  const auto depthStart = timeNow();
  const auto* depthLabels = static_cast<const SceneDataCapturePixel*>(mappedReadback(surfaces));
  const auto encodeStart = timeNow();
  const auto encoded = encodeSensorCameraFrame(
      {rgb, pixels * 4}, {depthLabels, pixels},
      capture.labels, settings, frame);
  const auto writeStart = timeNow();
  const auto stem = std::format("frame-{:08}", frame.sampleIndex);
  const auto publish = container::util::AtomicFilePublication::CreateNew;
  container::util::writeFileAtomically(capture.directory / (stem + ".exr"), encoded.exrBytes(), publish);
  container::util::writeFileAtomically(capture.directory / (stem + ".json"), encoded.metadataJson, publish);
  if (auto& benchmark = capture.benchmark) {
    using Timestamps = FrameSensorReadbackTimestamps;
    SensorCameraBenchmarkSample sample{
        .tick = frame.tick,
        .sampleIndex = frame.sampleIndex,
        .rgbReadbackBytes = color.readbackSize,
        .depthLabelsReadbackBytes = surfaces.readbackSize,
        .outputBytes = encoded.exrSize + encoded.metadataJson.size(),
        .cpuRgbInvalidateMs = milliseconds(depthStart - rgbStart),
        .cpuDepthLabelsInvalidateMs = milliseconds(encodeStart - depthStart),
        .cpuEncodeMs = milliseconds(writeStart - encodeStart),
        .cpuWriteMs = milliseconds(BenchmarkClock::now() - writeStart)};
    if (benchmark->queryPool) {
      // Both paths into retirement already waited the graphics fence. Read
      // this slot's completed writes before its query range can be reused.
      std::array<uint64_t, Timestamps::Count> timestamps{};
      if (vkGetQueryPoolResults(svc_.ctx.deviceWrapper->device(), benchmark->queryPool,
                               frameSlot * Timestamps::Count, Timestamps::Count,
                               sizeof(timestamps), timestamps.data(), sizeof(uint64_t),
                               VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const auto bits = benchmark->timestampValidBits;
        const auto mask = bits >= 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << bits) - 1;
        const auto duration = [&](Timestamps::Boundary start, Timestamps::Boundary end) {
          return double((timestamps[end] - timestamps[start]) & mask) * benchmark->timestampPeriodNs * 1e-6;
        };
        sample.gpuRgbReadbackMs = duration(Timestamps::RgbBegin, Timestamps::RgbEnd);
        sample.gpuDepthLabelsReadbackMs = duration(Timestamps::DepthBegin, Timestamps::DepthEnd);
      }
    }
    benchmark->samples.push_back(sample);
  }
  capture.inFlight[frameSlot].reset();
  ++capture.completed;
}

void RendererFrontend::finishSensorCameraCapture() {
  auto& capture = sensorCameraCapture_;
  const auto waitStart = capture.benchmark ? BenchmarkClock::now() : BenchmarkClock::time_point{};
  subs_.frameSyncManager->waitForAllFrames();
  const auto finalFenceWaitMs = capture.benchmark ? milliseconds(BenchmarkClock::now() - waitStart) : 0.0;
  for (uint32_t slot = 0; slot < capture.inFlight.size(); ++slot)
    retireSensorCameraFrame(slot);
  if (capture.requested || capture.completed != svc_.config.sensorCameraPlayback->frames.size())
    throw std::runtime_error("Sensor camera dataset stopped before capturing every scheduled frame");
  const auto& simulationReportSha256 = svc_.config.sensorCameraPlayback->simulationReportSha256;
  nlohmann::json manifest{
      {"schema", "container.sensor-camera-dataset"}, {"version", 1},
      {"frameCount", capture.completed}, {"labels", "labels.json"},
      {"sceneSource", robotics::bimSceneSourceJson(capture.sceneSource)},
      {"simulationReportSha256", simulationReportSha256
          ? nlohmann::json(*simulationReportSha256) : nlohmann::json(nullptr)},
      {"frameImagePattern", "frame-%08d.exr"}, {"frameMetadataPattern", "frame-%08d.json"},
      {"colorSpace", "linear-sRGB"}, {"depthUnits", "metres"},
      {"surfacePolicy", "opaque-and-alpha-mask"}};
  if (const auto& appearance = svc_.config.sensorCameraPlayback->settings.appearance) {
    manifest["appearance"] = {{"algorithm", "splitmix64-v1"}, {"seed", appearance->seed},
        {"lightingVariation", appearance->lightingVariation}, {"materialVariation", appearance->materialVariation},
        {"directionalGain", capture.lighting.directional}, {"environmentGain", capture.lighting.environment}};
  }
  container::util::writeFileAtomically(capture.directory / "dataset.json", manifest.dump(2) + '\n',
      container::util::AtomicFilePublication::CreateNew);
  if (capture.benchmark)
    writeSensorCameraBenchmark(finalFenceWaitMs);
}

void RendererFrontend::writeSensorCameraBenchmark(double finalFenceWaitMs) {
  using Json = nlohmann::json;
  auto& benchmark = *sensorCameraCapture_.benchmark;
  const auto& settings = svc_.config.sensorCameraPlayback->settings;
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 device{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  device.pNext = &driver;
  vkGetPhysicalDeviceProperties2(svc_.ctx.deviceWrapper->physicalDevice(), &device);

  const auto allocator = svc_.allocationManager.memoryManager()->allocator();
  VmaTotalStatistics statistics{};
  vmaCalculateStatistics(allocator, &statistics);
  const VkPhysicalDeviceMemoryProperties* memory = nullptr;
  vmaGetMemoryProperties(allocator, &memory);
  VkDeviceSize deviceLocalBytes = 0;
  for (uint32_t heap = 0; heap < memory->memoryHeapCount; ++heap) {
    if (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      deviceLocalBytes += statistics.memoryHeap[heap].statistics.allocationBytes;
  }

  std::ranges::sort(benchmark.samples, {}, &SensorCameraBenchmarkSample::sampleIndex);
  Json frames = Json::array();
  size_t timedFrames = 0;
  for (const auto& sample : benchmark.samples) {
    if (sample.gpuRgbReadbackMs && sample.gpuDepthLabelsReadbackMs) ++timedFrames;
    frames.push_back({{"tick", sample.tick}, {"sampleIndex", sample.sampleIndex},
        {"rgbReadbackBytes", sample.rgbReadbackBytes}, {"depthLabelsReadbackBytes", sample.depthLabelsReadbackBytes},
        {"gpuRgbReadbackMs", sample.gpuRgbReadbackMs ? Json(*sample.gpuRgbReadbackMs) : Json(nullptr)},
        {"gpuDepthLabelsReadbackMs", sample.gpuDepthLabelsReadbackMs ? Json(*sample.gpuDepthLabelsReadbackMs) : Json(nullptr)},
        {"cpuRgbInvalidateMs", sample.cpuRgbInvalidateMs},
        {"cpuDepthLabelsInvalidateMs", sample.cpuDepthLabelsInvalidateMs},
        {"cpuEncodeMs", sample.cpuEncodeMs}, {"cpuWriteMs", sample.cpuWriteMs},
        {"outputBytes", sample.outputBytes}});
  }
  const auto& gpu = device.properties;
  const auto status = benchmark.queryPool && timedFrames < benchmark.samples.size()
      ? "one or more timestamp pairs were unavailable after fence retirement" : benchmark.timestampStatus;
  const Json report{
      {"schema", "container.sensor-camera-benchmark"}, {"version", 1},
      {"build", {{"revision", CONTAINER_GIT_REVISION}, {"configuration", CONTAINER_BUILD_CONFIG},
                 {"compiler", CONTAINER_BUILD_COMPILER}}},
      {"device", {{"name", gpu.deviceName}, {"vendorId", gpu.vendorID}, {"deviceId", gpu.deviceID},
                  {"driverVersion", gpu.driverVersion}, {"apiVersion", gpu.apiVersion},
                  {"driverId", driver.driverID}, {"driverName", driver.driverName}, {"driverInfo", driver.driverInfo}}},
      {"width", settings.width}, {"height", settings.height},
      {"gpuTiming", {{"source", benchmark.queryPool ? "timestamp-query" : "none"},
                     {"available", timedFrames != 0}, {"status", status}, {"timedFrames", timedFrames},
                     {"timestampValidBits", benchmark.timestampValidBits},
                     {"timestampPeriodNs", benchmark.timestampPeriodNs},
                     {"rgbScope", "scene-linear post-process shader, swapchain draw and RGB host visibility barrier"},
                     {"depthLabelsScope", "depth layout transitions, metric depth and ID compute, host visibility barrier"}}},
      {"memory", {{"allocationBytes", statistics.total.statistics.allocationBytes},
                  {"deviceLocalBytes", deviceLocalBytes}}},
      {"finalFenceWaitMs", finalFenceWaitMs}, {"frames", std::move(frames)}};
  container::util::writeFileAtomically(svc_.config.sensorBenchmarkPath, report.dump(2) + '\n',
      container::util::AtomicFilePublication::CreateNew);
}

}  // namespace container::renderer
