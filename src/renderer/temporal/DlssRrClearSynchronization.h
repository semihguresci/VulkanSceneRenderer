#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace container::renderer::ngx::rr_clear_sync {

// DLSS 310.9.16 initializes two private RR images with shader-only access
// scopes, then clears them through the transfer pipeline. Match that complete
// observed packet only, within the first actual RR evaluation. Application
// command dispatch and signed vendor binaries are unchanged.
struct State {
  VkCommandBuffer commands{};
  VkImage candidate{};
  VkImageSubresourceRange range{};
  uint32_t correctedClears{};
};
inline thread_local State* activeState{};
inline std::atomic<PFN_vkCmdPipelineBarrier> originalBarrier{};
inline std::atomic<PFN_vkCmdClearColorImage> originalClear{};
static_assert(sizeof(activeState) == sizeof(void*));

class Scope {
 public:
  Scope(VkCommandBuffer commands, bool firstEvaluation) noexcept {
    if (commands && firstEvaluation && !activeState) {
      state_.commands = commands;
      activeState = &state_;
      owns_ = true;
    }
  }
  ~Scope() noexcept {
    if (!owns_) return;
    activeState = nullptr;
    if (state_.correctedClears)
      std::fprintf(stderr, "[DLSS RR] Recorded synchronization for %u private initialization clears\n",
          state_.correctedClears);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
  [[nodiscard]] uint32_t correctedClears() const noexcept { return state_.correctedClears; }

 private:
  State state_{};
  bool owns_{};
};

inline bool observedRange(const VkImageSubresourceRange& range) noexcept {
  return range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && range.baseMipLevel == 0 &&
      range.levelCount == 1 && range.baseArrayLayer == 0 && range.layerCount == 1;
}
inline bool observedInitialization(VkPipelineStageFlags source, VkPipelineStageFlags destination,
    VkDependencyFlags flags, uint32_t memories, uint32_t buffers, uint32_t images,
    const VkImageMemoryBarrier* image) noexcept {
  constexpr auto shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  return source == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && destination == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
      flags == 0 && memories == 0 && buffers == 0 && images == 1 && image && image->image &&
      image->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER && !image->pNext &&
      image->srcAccessMask == shaderAccess && image->dstAccessMask == shaderAccess &&
      image->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && image->newLayout == VK_IMAGE_LAYOUT_GENERAL &&
      image->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
      image->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && observedRange(image->subresourceRange);
}
inline VKAPI_ATTR void VKAPI_CALL pipelineBarrier(VkCommandBuffer commands,
    VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags,
    uint32_t memoryCount, const VkMemoryBarrier* memories, uint32_t bufferCount,
    const VkBufferMemoryBarrier* buffers, uint32_t imageCount, const VkImageMemoryBarrier* images) {
  if (activeState && activeState->commands == commands) {
    activeState->candidate = VK_NULL_HANDLE;
    if (observedInitialization(source, destination, flags, memoryCount, bufferCount, imageCount, images)) {
      activeState->candidate = images[0].image;
      activeState->range = images[0].subresourceRange;
    }
  }
  originalBarrier.load(std::memory_order_relaxed)(commands, source, destination, flags,
      memoryCount, memories, bufferCount, buffers, imageCount, images);
}
inline VKAPI_ATTR void VKAPI_CALL clearColorImage(VkCommandBuffer commands, VkImage image,
    VkImageLayout layout, const VkClearColorValue* color, uint32_t count,
    const VkImageSubresourceRange* ranges) {
  auto* state = activeState;
  const auto clear = originalClear.load(std::memory_order_relaxed);
  const auto barrier = originalBarrier.load(std::memory_order_relaxed);
  bool synchronize = false;
  if (state && state->commands == commands) {
    synchronize = state->candidate && state->candidate == image && layout == VK_IMAGE_LAYOUT_GENERAL &&
        color && count == 1 && ranges && observedRange(ranges[0]) && barrier &&
        std::bit_cast<std::array<uint32_t, 4>>(*color) == std::array<uint32_t, 4>{0x3f800000, 0, 0, 0};
    state->candidate = VK_NULL_HANDLE; // Every scoped clear consumes the candidate.
  }
  if (!synchronize) {
    clear(commands, image, layout, color, count, ranges);
    return;
  }

  // Layout-transition writes are automatically available. Make those writes
  // visible to this exact clear, then publish its transfer writes to later
  // vendor work. GENERAL->GENERAL preserves both layout and image contents.
  // https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions
  VkImageMemoryBarrier dependency{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dependency.oldLayout = dependency.newLayout = layout;
  dependency.srcQueueFamilyIndex = dependency.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  dependency.image = image;
  dependency.subresourceRange = state->range;
  barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      0, nullptr, 0, nullptr, 1, &dependency);
  clear(commands, image, layout, color, count, ranges);
  dependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
      0, nullptr, 0, nullptr, 1, &dependency);
  ++state->correctedClears;
}

inline PFN_vkVoidFunction intercept(const char* name, PFN_vkVoidFunction original) noexcept {
  if (!name || !original) return original;
  if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) {
    originalBarrier.store(reinterpret_cast<PFN_vkCmdPipelineBarrier>(original), std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(pipelineBarrier);
  }
  if (std::strcmp(name, "vkCmdClearColorImage") == 0) {
    originalClear.store(reinterpret_cast<PFN_vkCmdClearColorImage>(original), std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(clearColorImage);
  }
  return original;
}
inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProcAddr(VkDevice device, const char* name) {
  return intercept(name, vkGetDeviceProcAddr(device, name));
}
inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProcAddr(VkInstance instance, const char* name) {
  const auto original = vkGetInstanceProcAddr(instance, name);
  if (original && name && std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(deviceProcAddr);
  return intercept(name, original);
}

} // namespace container::renderer::ngx::rr_clear_sync
