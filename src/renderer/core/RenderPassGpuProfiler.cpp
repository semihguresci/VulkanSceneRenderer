#include "Container/renderer/core/RenderPassGpuProfiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace container::renderer {

namespace {

std::vector<VkExtensionProperties> enumerateDeviceExtensions(
    VkPhysicalDevice device) {
  uint32_t extensionCount = 0;
  VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr,
                                                         &extensionCount,
                                                         nullptr);
  if (result != VK_SUCCESS) {
    return {};
  }

  std::vector<VkExtensionProperties> extensions(extensionCount);
  result = vkEnumerateDeviceExtensionProperties(device, nullptr,
                                                &extensionCount,
                                                extensions.data());
  if (result != VK_SUCCESS) {
    return {};
  }
  return extensions;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
  const std::vector<VkExtensionProperties> extensions =
      enumerateDeviceExtensions(device);
  return std::ranges::any_of(extensions, [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool supportsPerformanceCounterQueryPools(VkPhysicalDevice device) {
  VkPhysicalDevicePerformanceQueryFeaturesKHR performanceFeatures{};
  performanceFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;

  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &performanceFeatures;
  vkGetPhysicalDeviceFeatures2(device, &features2);
  return performanceFeatures.performanceCounterQueryPools == VK_TRUE;
}

}  // namespace

RenderPassGpuProfiler::~RenderPassGpuProfiler() {
  shutdown();
}

void RenderPassGpuProfiler::initialize(VkDevice device,
                                       VkInstance instance,
                                       VkPhysicalDevice physicalDevice,
                                       uint32_t queueFamilyIndex,
                                       uint32_t frameSlots) {
  shutdown();

  instance_ = instance;
  device_ = device;
  physicalDevice_ = physicalDevice;
  queueFamilyIndex_ = queueFamilyIndex;
  frameSlots_ = std::max(frameSlots, 1u);
  activeWrittenPasses_.fill(0u);
  latestWrittenPasses_.fill(0u);
  latestFrameHasQueries_ = false;
  activeFrameRecording_ = false;
  backendStatus_ = "initializing GPU timing queries";

  if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE) {
    backendStatus_ = "GPU timing disabled: missing Vulkan device";
    return;
  }

  if (initializePerformanceQueryBackend()) {
    return;
  }

  const std::string performanceQueryStatus = backendStatus_;
  if (initializeTimestampBackend()) {
    if (!performanceQueryStatus.empty()) {
      backendStatus_ += "; " + performanceQueryStatus;
    }
    return;
  }

  if (!performanceQueryStatus.empty()) {
    backendStatus_ += "; " + performanceQueryStatus;
  }
}

void RenderPassGpuProfiler::shutdown() {
  if (profilingLockAcquired_ && releaseProfilingLock_ != nullptr &&
      device_ != VK_NULL_HANDLE) {
    releaseProfilingLock_(device_);
  }
  profilingLockAcquired_ = false;
  if (queryPool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    destroyOwnedQueryPool(device_, queryPool_, nullptr);
  }
  queryPool_ = VK_NULL_HANDLE;
  frameSlots_ = 0;
  backend_ = Backend::None;
  backendStatus_ = "not initialized";
  activeFrameRecording_ = false;
  latestFrameHasQueries_ = false;
  activeWrittenPasses_.fill(0u);
  latestWrittenPasses_.fill(0u);
}

void RenderPassGpuProfiler::recreate(uint32_t frameSlots) {
  const VkDevice device = device_;
  const VkInstance instance = instance_;
  const VkPhysicalDevice physicalDevice = physicalDevice_;
  const uint32_t queueFamilyIndex = queueFamilyIndex_;
  initialize(device, instance, physicalDevice, queueFamilyIndex, frameSlots);
}

void RenderPassGpuProfiler::beginFrame(VkCommandBuffer cmd,
                                       uint32_t frameSlot) {
  if (!isReady() || cmd == VK_NULL_HANDLE || frameSlots_ == 0u) {
    activeFrameRecording_ = false;
    return;
  }

  activeFrameSlot_ = frameSlot % frameSlots_;
  activeWrittenPasses_.fill(0u);
  activeFrameRecording_ = true;
  const uint32_t queryCount =
      backend_ == Backend::PerformanceQuery ? kPerformanceQueriesPerFrame
                                            : kQueriesPerFrame;
  vkResetQueryPool(device_, queryPool_, queryBase(activeFrameSlot_),
                   queryCount);
}

void RenderPassGpuProfiler::beginPass(VkCommandBuffer cmd, RenderPassId id) {
  if (!activeFrameRecording_ || !isReady() ||
      !isValidPassId(id) || cmd == VK_NULL_HANDLE) {
    return;
  }

  if (backend_ == Backend::PerformanceQuery) {
    vkCmdBeginQuery(cmd, queryPool_, performanceQuery(activeFrameSlot_, id), 0);
  } else {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_,
                        startQuery(activeFrameSlot_, id));
  }
}

void RenderPassGpuProfiler::endPass(VkCommandBuffer cmd, RenderPassId id) {
  if (!activeFrameRecording_ || !isReady() ||
      !isValidPassId(id) || cmd == VK_NULL_HANDLE) {
    return;
  }

  if (backend_ == Backend::PerformanceQuery) {
    vkCmdEndQuery(cmd, queryPool_, performanceQuery(activeFrameSlot_, id));
  } else {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_,
                        endQuery(activeFrameSlot_, id));
  }

  const auto index = static_cast<size_t>(id);
  if (index < activeWrittenPasses_.size()) {
    activeWrittenPasses_[index] = 1u;
    latestWrittenPasses_ = activeWrittenPasses_;
    latestFrameSlot_ = activeFrameSlot_;
    latestFrameHasQueries_ = true;
  }
}

std::vector<RendererPassGpuTiming> RenderPassGpuProfiler::collectLatest()
    const {
  if (backend_ == Backend::PerformanceQuery) {
    return collectPerformanceTimings();
  }
  return collectTimestampTimings();
}

std::vector<RendererPassGpuTiming> RenderPassGpuProfiler::collectTimestampTimings()
    const {
  std::vector<RendererPassGpuTiming> timings;
  if (!isReady() || !latestFrameHasQueries_ || frameSlots_ == 0u) {
    return timings;
  }

  std::array<uint64_t, kQueriesPerFrame * 2u> queryResults{};
  const VkResult result = vkGetQueryPoolResults(
      device_, queryPool_, queryBase(latestFrameSlot_), kQueriesPerFrame,
      sizeof(queryResults), queryResults.data(), sizeof(uint64_t) * 2u,
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result != VK_SUCCESS && result != VK_NOT_READY) {
    return timings;
  }

  const auto queryAvailable = [&](uint32_t queryIndex) {
    return queryResults[queryIndex * 2u + 1u] != 0u;
  };

  timings.reserve(kRenderPassIdCount);
  for (size_t passIndex = 0; passIndex < kRenderPassIdCount; ++passIndex) {
    if (latestWrittenPasses_[passIndex] == 0u) {
      continue;
    }

    const auto id = static_cast<RenderPassId>(passIndex);
    const uint32_t start = startQuery(latestFrameSlot_, id) -
                           queryBase(latestFrameSlot_);
    const uint32_t end = endQuery(latestFrameSlot_, id) -
                         queryBase(latestFrameSlot_);
    if (!queryAvailable(start) || !queryAvailable(end)) {
      continue;
    }

    const uint64_t startTick = queryResults[start * 2u];
    const uint64_t endTick = queryResults[end * 2u];
    if (endTick <= startTick) {
      continue;
    }

    const float milliseconds =
        static_cast<float>(static_cast<double>(endTick - startTick) *
                           static_cast<double>(timestampPeriodNs_) * 1.0e-6);
    timings.push_back(RendererPassGpuTiming{
        .id = id,
        .milliseconds = milliseconds,
    });
  }

  return timings;
}

std::vector<RendererPassGpuTiming>
RenderPassGpuProfiler::collectPerformanceTimings() const {
  std::vector<RendererPassGpuTiming> timings;
  if (!isReady() || !latestFrameHasQueries_ || frameSlots_ == 0u ||
      backend_ != Backend::PerformanceQuery) {
    return timings;
  }

  timings.reserve(kRenderPassIdCount);
  for (size_t passIndex = 0; passIndex < kRenderPassIdCount; ++passIndex) {
    if (latestWrittenPasses_[passIndex] == 0u) {
      continue;
    }

    VkPerformanceCounterResultKHR queryResult{};
    const VkResult result = vkGetQueryPoolResults(
        device_, queryPool_,
        performanceQuery(latestFrameSlot_, static_cast<RenderPassId>(passIndex)),
        1u, sizeof(queryResult), &queryResult,
        sizeof(VkPerformanceCounterResultKHR), 0);
    if (result != VK_SUCCESS) {
      continue;
    }

    const float milliseconds =
        performanceCounterToMilliseconds(queryResult);
    if (milliseconds <= 0.0f) {
      continue;
    }

    timings.push_back(RendererPassGpuTiming{
        .id = static_cast<RenderPassId>(passIndex),
        .milliseconds = milliseconds,
    });
  }

  return timings;
}

uint32_t RenderPassGpuProfiler::queryBase(uint32_t frameSlot) const {
  const uint32_t queriesPerFrame =
      backend_ == Backend::PerformanceQuery ? kPerformanceQueriesPerFrame
                                            : kQueriesPerFrame;
  return (frameSlot % std::max(frameSlots_, 1u)) * queriesPerFrame;
}

uint32_t RenderPassGpuProfiler::performanceQuery(uint32_t frameSlot,
                                                 RenderPassId id) const {
  return queryBase(frameSlot) + static_cast<uint32_t>(id);
}

uint32_t RenderPassGpuProfiler::startQuery(uint32_t frameSlot,
                                           RenderPassId id) const {
  return queryBase(frameSlot) +
         static_cast<uint32_t>(id) * kQueriesPerPass;
}

uint32_t RenderPassGpuProfiler::endQuery(uint32_t frameSlot,
                                         RenderPassId id) const {
  return startQuery(frameSlot, id) + 1u;
}

bool RenderPassGpuProfiler::isValidPassId(RenderPassId id) const {
  return static_cast<size_t>(id) < kRenderPassIdCount;
}

bool RenderPassGpuProfiler::initializePerformanceQueryBackend() {
  if (instance_ == VK_NULL_HANDLE) {
    backendStatus_ =
        "performance queries unavailable: missing Vulkan instance";
    return false;
  }
  if (!hasDeviceExtension(physicalDevice_,
                          VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME)) {
    backendStatus_ =
        "performance queries unavailable: device does not expose "
        VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME;
    return false;
  }
  if (!supportsPerformanceCounterQueryPools(physicalDevice_)) {
    backendStatus_ =
        "performance queries unavailable: performanceCounterQueryPools "
        "feature is not supported";
    return false;
  }

  enumeratePerformanceCounters_ =
      reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR>(
          vkGetInstanceProcAddr(
              instance_,
              "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"));
  getPerformanceQueryPasses_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR>(
          vkGetInstanceProcAddr(
              instance_,
              "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR"));
  acquireProfilingLock_ =
      reinterpret_cast<PFN_vkAcquireProfilingLockKHR>(
          vkGetDeviceProcAddr(device_, "vkAcquireProfilingLockKHR"));
  releaseProfilingLock_ =
      reinterpret_cast<PFN_vkReleaseProfilingLockKHR>(
          vkGetDeviceProcAddr(device_, "vkReleaseProfilingLockKHR"));

  if (enumeratePerformanceCounters_ == nullptr ||
      getPerformanceQueryPasses_ == nullptr ||
      acquireProfilingLock_ == nullptr ||
      releaseProfilingLock_ == nullptr) {
    backendStatus_ =
        "performance queries unavailable: extension functions not loaded";
    return false;
  }

  uint32_t counterCount = 0;
  if (enumeratePerformanceCounters_(physicalDevice_, queueFamilyIndex_,
                                    &counterCount, nullptr, nullptr) !=
          VK_SUCCESS ||
      counterCount == 0u) {
    backendStatus_ =
        "performance queries unavailable: no queue-family counters";
    return false;
  }

  std::vector<VkPerformanceCounterKHR> counters(counterCount);
  std::vector<VkPerformanceCounterDescriptionKHR> descriptions(counterCount);
  for (auto& counter : counters) {
    counter.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
  }
  for (auto& description : descriptions) {
    description.sType =
        VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
  }

  if (enumeratePerformanceCounters_(physicalDevice_, queueFamilyIndex_,
                                    &counterCount, counters.data(),
                                    descriptions.data()) != VK_SUCCESS) {
    backendStatus_ =
        "performance queries unavailable: counter enumeration failed";
    return false;
  }

  bool foundNanosecondsCounter = false;
  for (uint32_t i = 0; i < counterCount; ++i) {
    if (counters[i].unit == VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR) {
      performanceCounterIndex_ = i;
      performanceCounterStorage_ = counters[i].storage;
      foundNanosecondsCounter = true;
      break;
    }
  }
  if (!foundNanosecondsCounter) {
    backendStatus_ =
        "performance queries unavailable: no nanosecond counter";
    return false;
  }

  VkQueryPoolPerformanceCreateInfoKHR performanceCreateInfo{};
  performanceCreateInfo.sType =
      VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR;
  performanceCreateInfo.queueFamilyIndex = queueFamilyIndex_;
  performanceCreateInfo.counterIndexCount = 1u;
  performanceCreateInfo.pCounterIndices = &performanceCounterIndex_;

  uint32_t passCount = 0;
  getPerformanceQueryPasses_(physicalDevice_, &performanceCreateInfo,
                             &passCount);
  if (passCount != 1u) {
    backendStatus_ =
        "performance queries unavailable: selected counter needs " +
        std::to_string(passCount) + " counter passes";
    return false;
  }

  VkAcquireProfilingLockInfoKHR lockInfo{};
  lockInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR;
  lockInfo.timeout = 0u;
  if (acquireProfilingLock_(device_, &lockInfo) != VK_SUCCESS) {
    backendStatus_ =
        "performance queries unavailable: profiling lock not acquired";
    return false;
  }
  profilingLockAcquired_ = true;

  VkQueryPoolCreateInfo queryPoolInfo{
      VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queryPoolInfo.pNext = &performanceCreateInfo;
  queryPoolInfo.queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR;
  queryPoolInfo.queryCount = frameSlots_ * kPerformanceQueriesPerFrame;
  if (createOwnedQueryPool(device_, &queryPoolInfo, nullptr, &queryPool_) !=
      VK_SUCCESS) {
    releaseProfilingLock_(device_);
    profilingLockAcquired_ = false;
    queryPool_ = VK_NULL_HANDLE;
    backendStatus_ =
        "performance queries unavailable: query pool creation failed";
    return false;
  }

  backend_ = Backend::PerformanceQuery;
  backendStatus_ = "using VK_KHR_performance_query";
  return true;
}

bool RenderPassGpuProfiler::initializeTimestampBackend() {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
  timestampPeriodNs_ = properties.limits.timestampPeriod;

  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  if (queueFamilyCount > 0u) {
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice_, &queueFamilyCount, queueFamilies.data());
  }
  const bool queueSupportsTimestamps =
      queueFamilyIndex_ < queueFamilies.size() &&
      queueFamilies[queueFamilyIndex_].timestampValidBits > 0u;
  const bool timestampQueriesSupported =
      queueSupportsTimestamps && timestampPeriodNs_ > 0.0f;
  if (!timestampQueriesSupported) {
    backendStatus_ =
        "timestamp queries unavailable: graphics queue has no valid timestamp bits";
    return false;
  }

  VkQueryPoolCreateInfo queryPoolInfo{
      VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queryPoolInfo.queryCount = frameSlots_ * kQueriesPerFrame;
  if (createOwnedQueryPool(device_, &queryPoolInfo, nullptr, &queryPool_) !=
      VK_SUCCESS) {
    queryPool_ = VK_NULL_HANDLE;
    backendStatus_ =
        "timestamp queries unavailable: query pool creation failed";
    return false;
  }

  backend_ = Backend::Timestamp;
  backendStatus_ = "using timestamp queries";
  return true;
}

float RenderPassGpuProfiler::performanceCounterToMilliseconds(
    const VkPerformanceCounterResultKHR& value) const {
  double nanoseconds = 0.0;
  switch (performanceCounterStorage_) {
    case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR:
      nanoseconds = static_cast<double>(value.int32);
      break;
    case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR:
      nanoseconds = static_cast<double>(value.int64);
      break;
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR:
      nanoseconds = static_cast<double>(value.uint32);
      break;
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR:
      nanoseconds = static_cast<double>(value.uint64);
      break;
    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR:
      nanoseconds = static_cast<double>(value.float32);
      break;
    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR:
      nanoseconds = value.float64;
      break;
    default:
      return 0.0f;
  }

  return nanoseconds > 0.0 ? static_cast<float>(nanoseconds * 1.0e-6) : 0.0f;
}

}  // namespace container::renderer
