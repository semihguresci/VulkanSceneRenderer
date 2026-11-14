#include "Container/renderer/core/RenderPassGpuProfiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
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
  slots_.resize(frameSlots_);
  submittedSlots_.reserve(frameSlots_);
  retired_.reserve(frameSlots_);
  backendStatus_ = "initializing GPU timing queries";

  if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE) {
    backendStatus_ = "GPU timing disabled: missing Vulkan device";
    return;
  }

  if (initializePerformanceQueryBackend()) {
    if (initializeTimestampProperties()) {
      const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
          .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = frameSlots_ * 2u};
      if (createOwnedQueryPool(device_, &info, nullptr, &graphicsQueryPool_) != VK_SUCCESS)
        graphicsQueryPool_ = VK_NULL_HANDLE;
    }
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
  if (graphicsQueryPool_ != VK_NULL_HANDLE && graphicsQueryPool_ != queryPool_)
    destroyOwnedQueryPool(device_, graphicsQueryPool_, nullptr);
  queryPool_ = VK_NULL_HANDLE;
  graphicsQueryPool_ = VK_NULL_HANDLE;
  frameSlots_ = 0;
  backend_ = Backend::None;
  backendStatus_ = "not initialized";
  activeFrameSlot_.reset();
  slots_.clear();
  submittedSlots_.clear();
  retired_.clear();
}

void RenderPassGpuProfiler::beginFrame(VkCommandBuffer cmd,
                                       uint32_t frameSlot) {
  activeFrameSlot_.reset();
  if (!isReady() || cmd == VK_NULL_HANDLE) {
    return;
  }

  auto& slot = slots_.at(frameSlot);
  if (slot.submitted)
    throw std::logic_error("GPU frame queries must retire before slot reuse");
  slot.writtenPasses.fill(0u);
  slot.graphicsTimestampsWritten = false;
  activeFrameSlot_ = frameSlot;
  const uint32_t queryCount =
      backend_ == Backend::PerformanceQuery ? kPerformanceQueriesPerFrame
                                            : kQueriesPerFrame + 2u;
  if (backend_ == Backend::PerformanceQuery) {
    // The hardware-counter backend is serialized and requires a host reset.
    vkResetQueryPool(device_, queryPool_, queryBase(frameSlot), queryCount);
  } else {
    // Other slots can still have timestamp commands pending on this pool.
    vkCmdResetQueryPool(cmd, queryPool_, queryBase(frameSlot), queryCount);
  }
  if (graphicsQueryPool_ != VK_NULL_HANDLE) {
    const uint32_t first = backend_ == Backend::Timestamp ? queryBase(frameSlot) + kQueriesPerFrame : frameSlot * 2u;
    if (graphicsQueryPool_ != queryPool_)
      vkCmdResetQueryPool(cmd, graphicsQueryPool_, first, 2u);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, graphicsQueryPool_, first);
  }
}

void RenderPassGpuProfiler::endFrame(VkCommandBuffer cmd) {
  if (!activeFrameSlot_ || graphicsQueryPool_ == VK_NULL_HANDLE) return;
  const uint32_t slot = *activeFrameSlot_;
  const uint32_t first = backend_ == Backend::Timestamp ? queryBase(slot) + kQueriesPerFrame : slot * 2u;
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, graphicsQueryPool_, first + 1u);
  slots_[slot].graphicsTimestampsWritten = true;
}

void RenderPassGpuProfiler::beginPass(VkCommandBuffer cmd, RenderPassId id) {
  if (!activeFrameSlot_ || !isReady() ||
      !isValidPassId(id) || cmd == VK_NULL_HANDLE) {
    return;
  }

  if (backend_ == Backend::PerformanceQuery) {
    vkCmdBeginQuery(cmd, queryPool_, performanceQuery(*activeFrameSlot_, id), 0);
  } else {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_,
                        startQuery(*activeFrameSlot_, id));
  }
}

void RenderPassGpuProfiler::endPass(VkCommandBuffer cmd, RenderPassId id) {
  if (!activeFrameSlot_ || !isReady() ||
      !isValidPassId(id) || cmd == VK_NULL_HANDLE) {
    return;
  }

  if (backend_ == Backend::PerformanceQuery) {
    vkCmdEndQuery(cmd, queryPool_, performanceQuery(*activeFrameSlot_, id));
  } else {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_,
                        endQuery(*activeFrameSlot_, id));
  }

  slots_[*activeFrameSlot_].writtenPasses[static_cast<size_t>(id)] = 1u;
}

void RenderPassGpuProfiler::onSubmitted(
    VkFence fence, uint64_t frameIndex,
    std::optional<ReferenceBatch> referenceBatch) noexcept {
  if (!activeFrameSlot_) return;
  auto& slot = slots_[*activeFrameSlot_];
  if (slot.graphicsTimestampsWritten ||
      std::ranges::any_of(slot.writtenPasses, [](uint8_t written) { return written != 0u; })) {
    slot.submitted = Submission{fence, frameIndex, ++recordingSerial_, referenceBatch};
  }
  activeFrameSlot_.reset();
}

void RenderPassGpuProfiler::markFenceComplete(uint32_t frameSlot) {
  if (!isReady()) return;
  const auto& completed = slots_.at(frameSlot).submitted;
  if (!completed) return;
  // Graphics submission fences cover the prefix of the same queue. Retain
  // that proof even when a loading submission resets one of those fences.
  for (auto& slot : slots_) {
    if (slot.submitted && slot.submitted->serial <= completed->serial)
      slot.submitted->fence = VK_NULL_HANDLE;
  }
}

std::span<const RenderPassGpuProfiler::RetiredFrame> RenderPassGpuProfiler::collectRetired() {
  retired_.clear();
  if (!isReady()) return {};

  submittedSlots_.clear();
  for (uint32_t index = 0; index < slots_.size(); ++index) {
    if (slots_[index].submitted) submittedSlots_.push_back(index);
  }
  std::ranges::sort(submittedSlots_, {}, [this](uint32_t slot) { return slots_[slot].submitted->serial; });
  for (const uint32_t index : submittedSlots_) {
    auto& slot = slots_[index];
    const auto& submission = *slot.submitted;
    if (submission.fence != VK_NULL_HANDLE) {
      const VkResult status = vkGetFenceStatus(device_, submission.fence);
      // A later graphics submit can complete while the host is polling. Keep
      // the oldest pending serial so consumers always receive results in order.
      if (status == VK_NOT_READY) break;
      if (status != VK_SUCCESS)
        throw std::runtime_error("Failed to check GPU query submission fence: " +
                                 std::to_string(status));
    }
    auto timings = backend_ == Backend::PerformanceQuery
        ? collectPerformanceTimings(index) : collectTimestampTimings(index);
    // The fence permits resetting this range even if a driver supplies no
    // complete result. In that case skip this sample instead of reporting a
    // misleading partial duration or losing the ability to reuse the slot.
    const auto graphics = collectGraphicsDuration(index);
    if ((timings && !timings->empty()) || graphics) {
      retired_.push_back({submission.frameIndex, submission.serial, submission.referenceBatch,
                          timings ? std::move(*timings) : std::vector<RendererPassGpuTiming>{}, graphics});
    }
    slot.submitted.reset();
  }
  return retired_;
}

uint32_t RenderPassGpuProfiler::pendingFrames() const {
  // The latest unsignaled prefix boundary can move while the host polls.
  // This is the number of outstanding scene submissions observed now.
  return static_cast<uint32_t>(std::ranges::count_if(slots_, [this](const Slot& slot) {
    if (!slot.submitted || slot.submitted->fence == VK_NULL_HANDLE) return false;
    const auto status = vkGetFenceStatus(device_, slot.submitted->fence);
    if (status != VK_SUCCESS && status != VK_NOT_READY)
      throw std::runtime_error("Failed to check GPU query submission fence: " + std::to_string(status));
    return status == VK_NOT_READY;
  }));
}

std::optional<float> RenderPassGpuProfiler::collectGraphicsDuration(uint32_t slot) const {
  if (!slots_[slot].graphicsTimestampsWritten) return {};
  const uint32_t first = backend_ == Backend::Timestamp ? queryBase(slot) + kQueriesPerFrame : slot * 2u;
  std::array<uint64_t, 2> ticks{};
  if (vkGetQueryPoolResults(device_, graphicsQueryPool_, first, 2u, sizeof(ticks), ticks.data(),
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return {};
  return timestampMilliseconds(ticks[0], ticks[1]);
}

float RenderPassGpuProfiler::timestampMilliseconds(uint64_t start, uint64_t end) const {
  return static_cast<float>(static_cast<double>((end - start) & timestampMask_) *
                            static_cast<double>(timestampPeriodNs_) * 1.0e-6);
}

RenderPassGpuProfiler::Timings RenderPassGpuProfiler::collectTimestampTimings(
    uint32_t frameSlot) const {
  std::array<uint64_t, kQueriesPerFrame * 2u> queryResults{};
  const VkResult result = vkGetQueryPoolResults(
      device_, queryPool_, queryBase(frameSlot), kQueriesPerFrame,
      sizeof(queryResults), queryResults.data(), sizeof(uint64_t) * 2u,
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result != VK_SUCCESS && result != VK_NOT_READY) {
    return std::nullopt;
  }

  const auto queryAvailable = [&](uint32_t queryIndex) {
    return queryResults[queryIndex * 2u + 1u] != 0u;
  };

  std::vector<RendererPassGpuTiming> timings;
  timings.reserve(kRenderPassIdCount);
  for (size_t passIndex = 0; passIndex < kRenderPassIdCount; ++passIndex) {
    if (slots_[frameSlot].writtenPasses[passIndex] == 0u) {
      continue;
    }

    const auto id = static_cast<RenderPassId>(passIndex);
    const uint32_t start = startQuery(frameSlot, id) - queryBase(frameSlot);
    const uint32_t end = endQuery(frameSlot, id) - queryBase(frameSlot);
    if (!queryAvailable(start) || !queryAvailable(end)) {
      return std::nullopt;
    }

    const uint64_t startTick = queryResults[start * 2u];
    const uint64_t endTick = queryResults[end * 2u];
    const float milliseconds = timestampMilliseconds(startTick, endTick);
    if (milliseconds <= 0.0f) continue;
    timings.push_back(RendererPassGpuTiming{
        .id = id,
        .milliseconds = milliseconds,
    });
  }

  return timings;
}

RenderPassGpuProfiler::Timings RenderPassGpuProfiler::collectPerformanceTimings(
    uint32_t frameSlot) const {
  std::vector<RendererPassGpuTiming> timings;
  timings.reserve(kRenderPassIdCount);
  for (size_t passIndex = 0; passIndex < kRenderPassIdCount; ++passIndex) {
    if (slots_[frameSlot].writtenPasses[passIndex] == 0u) {
      continue;
    }

    VkPerformanceCounterResultKHR queryResult{};
    const VkResult result = vkGetQueryPoolResults(
        device_, queryPool_,
        performanceQuery(frameSlot, static_cast<RenderPassId>(passIndex)),
        1u, sizeof(queryResult), &queryResult,
        sizeof(VkPerformanceCounterResultKHR), 0);
    if (result != VK_SUCCESS) {
      return std::nullopt;
    }

    const float milliseconds =
        performanceCounterToMilliseconds(queryResult);
    if (milliseconds <= 0.0f) continue;
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
                                            : kQueriesPerFrame + 2u;
  return frameSlot * queriesPerFrame;
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

bool RenderPassGpuProfiler::initializeTimestampProperties() {
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
  if (!timestampQueriesSupported) return false;
  const uint32_t validBits = queueFamilies[queueFamilyIndex_].timestampValidBits;
  timestampMask_ = validBits == 64u ? std::numeric_limits<uint64_t>::max()
                                    : (uint64_t{1} << validBits) - 1u;
  return true;
}

bool RenderPassGpuProfiler::initializeTimestampBackend() {
  if (!initializeTimestampProperties()) {
    backendStatus_ = "timestamp queries unavailable: graphics queue has no valid timestamp bits";
    return false;
  }

  VkQueryPoolCreateInfo queryPoolInfo{
      VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queryPoolInfo.queryCount = frameSlots_ * (kQueriesPerFrame + 2u);
  if (createOwnedQueryPool(device_, &queryPoolInfo, nullptr, &queryPool_) !=
      VK_SUCCESS) {
    queryPool_ = VK_NULL_HANDLE;
    backendStatus_ =
        "timestamp queries unavailable: query pool creation failed";
    return false;
  }

  backend_ = Backend::Timestamp;
  graphicsQueryPool_ = queryPool_;
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
