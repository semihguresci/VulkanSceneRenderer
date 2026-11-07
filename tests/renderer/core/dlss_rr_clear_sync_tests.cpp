#include "DlssRrClearSynchronization.h"

#include <gtest/gtest.h>

#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace rrSync = container::renderer::ngx::rr_clear_sync;
namespace {
template<class Handle> Handle fake(uintptr_t value) {
  if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
  else return static_cast<Handle>(value);
}
const auto commands = fake<VkCommandBuffer>(0x1000);
const auto otherCommands = fake<VkCommandBuffer>(0x2000);
const auto imageA = fake<VkImage>(0x3000);
const auto imageB = fake<VkImage>(0x4000);

struct Call {
  bool clear{};
  VkCommandBuffer commands{};
  VkPipelineStageFlags source{}, destination{};
  VkDependencyFlags flags{};
  uint32_t memories{}, buffers{}, images{};
  const VkMemoryBarrier* memoryPointer{};
  const VkBufferMemoryBarrier* bufferPointer{};
  const VkImageMemoryBarrier* imagePointer{};
  VkImageMemoryBarrier image{};
  VkImage clearImage{};
  VkImageLayout layout{};
  const VkClearColorValue* colorPointer{};
  const VkImageSubresourceRange* rangePointer{};
  VkClearColorValue color{};
  VkImageSubresourceRange range{};
};
std::mutex callsMutex;
std::vector<Call> calls;
VKAPI_ATTR void VKAPI_CALL recordBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memoryCount,
    const VkMemoryBarrier* memories, uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
    uint32_t imageCount, const VkImageMemoryBarrier* images) {
  Call call{};
  call.commands = command; call.source = source; call.destination = destination; call.flags = flags;
  call.memories = memoryCount; call.buffers = bufferCount; call.images = imageCount;
  call.memoryPointer = memories; call.bufferPointer = buffers; call.imagePointer = images;
  if (imageCount && images) call.image = images[0];
  std::scoped_lock lock(callsMutex);
  calls.push_back(call);
}
VKAPI_ATTR void VKAPI_CALL recordClear(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    const VkClearColorValue* color, uint32_t count, const VkImageSubresourceRange* ranges) {
  Call call{};
  call.clear = true; call.commands = command; call.clearImage = image; call.layout = layout;
  call.images = count; call.colorPointer = color; call.rangePointer = ranges;
  if (color) call.color = *color;
  if (count && ranges) call.range = ranges[0];
  std::scoped_lock lock(callsMutex);
  calls.push_back(call);
}
VKAPI_ATTR void VKAPI_CALL unhandled() {}

VkImageMemoryBarrier initialization(VkImage image = imageA) {
  VkImageMemoryBarrier result{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  result.srcAccessMask = result.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  result.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; result.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  result.srcQueueFamilyIndex = result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  result.image = image;
  result.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return result;
}
struct BarrierRequest {
  VkCommandBuffer command{commands};
  VkPipelineStageFlags source{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT}, destination{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  VkDependencyFlags flags{};
  uint32_t memoryCount{}, bufferCount{}, imageCount{1};
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  std::array<VkImageMemoryBarrier, 2> images{initialization(), initialization(imageB)};
  bool missingImages{};
};
struct ClearRequest {
  VkCommandBuffer command{commands};
  VkImage image{imageA};
  VkImageLayout layout{VK_IMAGE_LAYOUT_GENERAL};
  VkClearColorValue color{{1, 0, 0, 0}};
  uint32_t count{1};
  std::array<VkImageSubresourceRange, 2> ranges{{
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
  bool missingColor{}, missingRanges{};
};
class DlssRrClearSync : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(rrSync::activeState, nullptr);
    calls.clear(); calls.reserve(32);
    barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(rrSync::deviceProcAddr({}, "vkCmdPipelineBarrier"));
    clear = reinterpret_cast<PFN_vkCmdClearColorImage>(rrSync::instanceProcAddr({}, "vkCmdClearColorImage"));
    ASSERT_NE(barrier, nullptr); ASSERT_NE(clear, nullptr);
  }
  void send(const BarrierRequest& r) {
    barrier(r.command, r.source, r.destination, r.flags, r.memoryCount, r.memoryCount ? &r.memory : nullptr,
        r.bufferCount, r.bufferCount ? &r.buffer : nullptr, r.imageCount, r.missingImages ? nullptr : r.images.data());
  }
  void send(const ClearRequest& r) {
    clear(r.command, r.image, r.layout, r.missingColor ? nullptr : &r.color,
        r.count, r.missingRanges ? nullptr : r.ranges.data());
  }
  void preserved(const BarrierRequest& r, const Call& call) {
    EXPECT_FALSE(call.clear); EXPECT_EQ(call.commands, r.command);
    EXPECT_EQ(call.source, r.source); EXPECT_EQ(call.destination, r.destination); EXPECT_EQ(call.flags, r.flags);
    EXPECT_EQ(call.memories, r.memoryCount); EXPECT_EQ(call.buffers, r.bufferCount); EXPECT_EQ(call.images, r.imageCount);
    EXPECT_EQ(call.memoryPointer, r.memoryCount ? &r.memory : nullptr);
    EXPECT_EQ(call.bufferPointer, r.bufferCount ? &r.buffer : nullptr);
    EXPECT_EQ(call.imagePointer, r.missingImages ? nullptr : r.images.data());
  }
  void preserved(const ClearRequest& r, const Call& call) {
    EXPECT_TRUE(call.clear); EXPECT_EQ(call.commands, r.command); EXPECT_EQ(call.clearImage, r.image);
    EXPECT_EQ(call.layout, r.layout); EXPECT_EQ(call.images, r.count);
    EXPECT_EQ(call.colorPointer, r.missingColor ? nullptr : &r.color);
    EXPECT_EQ(call.rangePointer, r.missingRanges ? nullptr : r.ranges.data());
  }
  PFN_vkCmdPipelineBarrier barrier{};
  PFN_vkCmdClearColorImage clear{};
};
}

// The CPU test links no Vulkan loader: the resolver seam delegates to these
// mocks, so no instance, driver or GPU work is performed.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* name) {
  if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) return reinterpret_cast<PFN_vkVoidFunction>(recordBarrier);
  if (std::strcmp(name, "vkCmdClearColorImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(recordClear);
  if (std::strcmp(name, "unhandled") == 0) return reinterpret_cast<PFN_vkVoidFunction>(unhandled);
  return nullptr;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  return vkGetDeviceProcAddr({}, name);
}

namespace {
TEST_F(DlssRrClearSync, ResolverPreservesUnknownQueriesAndChainsOnlyItsPrivateDeviceResolver) {
  EXPECT_EQ(rrSync::deviceProcAddr({}, "missing"), nullptr);
  EXPECT_EQ(rrSync::deviceProcAddr({}, "unhandled"), reinterpret_cast<PFN_vkVoidFunction>(unhandled));
  EXPECT_EQ(rrSync::instanceProcAddr({}, "unhandled"), reinterpret_cast<PFN_vkVoidFunction>(unhandled));
  EXPECT_EQ(rrSync::instanceProcAddr({}, "vkGetDeviceProcAddr"),
      reinterpret_cast<PFN_vkVoidFunction>(rrSync::deviceProcAddr));
}

TEST_F(DlssRrClearSync, AddsBothExactRangeDependenciesAndPreservesOriginalCommandArguments) {
  BarrierRequest init;
  ClearRequest reset;
  const auto original = init.images;
  {
    rrSync::Scope scope(commands, true);
    send(init); send(reset);
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_EQ(scope.correctedClears(), 1u);
  }
  EXPECT_EQ(rrSync::activeState, nullptr);
  preserved(init, calls[0]); preserved(reset, calls[2]);
  EXPECT_EQ(std::memcmp(init.images.data(), original.data(), sizeof(original)), 0);
  const auto& pre = calls[1]; const auto& post = calls[3];
  EXPECT_EQ(pre.source, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); EXPECT_EQ(pre.destination, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(pre.image.srcAccessMask, VK_ACCESS_MEMORY_WRITE_BIT); EXPECT_EQ(pre.image.dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(post.source, VK_PIPELINE_STAGE_TRANSFER_BIT); EXPECT_EQ(post.destination, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  EXPECT_EQ(post.image.srcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(post.image.dstAccessMask, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  for (const auto* dependency : {&pre, &post}) {
    EXPECT_FALSE(dependency->clear); EXPECT_EQ(dependency->commands, commands);
    EXPECT_EQ(dependency->flags, 0u); EXPECT_EQ(dependency->memories, 0u); EXPECT_EQ(dependency->buffers, 0u);
    EXPECT_EQ(dependency->images, 1u); EXPECT_EQ(dependency->image.pNext, nullptr);
    EXPECT_EQ(dependency->image.oldLayout, VK_IMAGE_LAYOUT_GENERAL); EXPECT_EQ(dependency->image.newLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(dependency->image.image, imageA);
    EXPECT_EQ(dependency->image.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(dependency->image.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(std::memcmp(&dependency->image.subresourceRange, &reset.ranges[0], sizeof(VkImageSubresourceRange)), 0);
  }
}

TEST_F(DlssRrClearSync, SuperResolutionAndSubsequentEvaluationsDelegateUnchanged) {
  BarrierRequest init; ClearRequest reset;
  send(init); send(reset); // SR never enters the RR-only scope.
  ASSERT_EQ(calls.size(), 2u); preserved(init, calls[0]); preserved(reset, calls[1]);
  calls.clear();
  rrSync::Scope later(commands, false);
  send(init); send(reset);
  ASSERT_EQ(calls.size(), 2u); EXPECT_EQ(later.correctedClears(), 0u);
  preserved(init, calls[0]); preserved(reset, calls[1]);
}

TEST_F(DlssRrClearSync, OtherCommandBuffersAndWorkerThreadsCannotBorrowTheRrCandidate) {
  BarrierRequest init; ClearRequest reset;
  rrSync::Scope owner(commands, true);
  send(init);
  auto foreign = reset; foreign.command = otherCommands;
  send(foreign);
  std::thread worker([&] { send(init); send(reset); });
  worker.join();
  ASSERT_EQ(calls.size(), 4u); EXPECT_EQ(owner.correctedClears(), 0u);
  preserved(foreign, calls[1]); preserved(init, calls[2]); preserved(reset, calls[3]);
  send(reset);
  ASSERT_EQ(calls.size(), 7u); EXPECT_EQ(owner.correctedClears(), 1u);
}

TEST_F(DlssRrClearSync, EveryUnobservedInitializationFieldAndBarrierArrayDelegatesUnchanged) {
  const std::vector<std::function<void(BarrierRequest&)>> mutations{
      [](auto& r) {r.source = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;},
      [](auto& r) {r.destination = VK_PIPELINE_STAGE_TRANSFER_BIT;},
      [](auto& r) {r.flags = VK_DEPENDENCY_BY_REGION_BIT;},
      [](auto& r) {r.memoryCount = 1;}, [](auto& r) {r.bufferCount = 1;},
      [](auto& r) {r.imageCount = 0;}, [](auto& r) {r.imageCount = 2;}, [](auto& r) {r.missingImages = true;},
      [](auto& r) {r.images[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;},
      [](auto& r) {r.images[0].pNext = &r.memory;},
      [](auto& r) {r.images[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;},
      [](auto& r) {r.images[0].dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;},
      [](auto& r) {r.images[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;},
      [](auto& r) {r.images[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;},
      [](auto& r) {r.images[0].srcQueueFamilyIndex = 0;}, [](auto& r) {r.images[0].dstQueueFamilyIndex = 0;},
      [](auto& r) {r.images[0].image = VK_NULL_HANDLE;},
      [](auto& r) {r.images[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;},
      [](auto& r) {r.images[0].subresourceRange.baseMipLevel = 1;},
      [](auto& r) {r.images[0].subresourceRange.levelCount = 2;},
      [](auto& r) {r.images[0].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;},
      [](auto& r) {r.images[0].subresourceRange.baseArrayLayer = 1;},
      [](auto& r) {r.images[0].subresourceRange.layerCount = 2;}};
  for (size_t i = 0; i < mutations.size(); ++i) {
    SCOPED_TRACE(i); calls.clear(); BarrierRequest init; ClearRequest reset; mutations[i](init);
    const auto original = init.images;
    rrSync::Scope scope(commands, true); send(init); send(reset);
    ASSERT_EQ(calls.size(), 2u); EXPECT_EQ(scope.correctedClears(), 0u);
    preserved(init, calls[0]); preserved(reset, calls[1]);
    EXPECT_EQ(std::memcmp(init.images.data(), original.data(), sizeof(original)), 0);
  }
}

TEST_F(DlssRrClearSync, UnobservedClearsDelegateAndConsumeTheCandidate) {
  const std::vector<std::function<void(ClearRequest&)>> mutations{
      [](auto& r) {r.image = imageB;}, [](auto& r) {r.image = VK_NULL_HANDLE;},
      [](auto& r) {r.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;},
      [](auto& r) {r.missingColor = true;}, [](auto& r) {r.missingRanges = true;},
      [](auto& r) {r.count = 0;}, [](auto& r) {r.count = 2;},
      [](auto& r) {r.color.float32[0] = 0;}, [](auto& r) {r.color.float32[1] = -0.0f;},
      [](auto& r) {r.color.float32[3] = 1;},
      [](auto& r) {r.ranges[0].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;},
      [](auto& r) {r.ranges[0].baseMipLevel = 1;}, [](auto& r) {r.ranges[0].levelCount = 2;},
      [](auto& r) {r.ranges[0].baseArrayLayer = 1;}, [](auto& r) {r.ranges[0].layerCount = 2;}};
  for (size_t i = 0; i < mutations.size(); ++i) {
    SCOPED_TRACE(i); calls.clear(); BarrierRequest init; ClearRequest reset, changed;
    mutations[i](changed);
    rrSync::Scope scope(commands, true); send(init); send(changed); send(reset);
    ASSERT_EQ(calls.size(), 3u); EXPECT_EQ(scope.correctedClears(), 0u);
    preserved(changed, calls[1]); preserved(reset, calls[2]);
  }
}

TEST_F(DlssRrClearSync, InterveningBarrierInvalidatesOrReplacesTheCandidate) {
  for (bool anotherImage : {false, true}) {
    calls.clear(); BarrierRequest init, next; ClearRequest reset;
    if (anotherImage) next.images[0] = initialization(imageB);
    else next.images[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    rrSync::Scope scope(commands, true); send(init); send(next); send(reset);
    ASSERT_EQ(calls.size(), 3u); EXPECT_EQ(scope.correctedClears(), 0u);
    preserved(next, calls[1]); preserved(reset, calls[2]);
  }
}

TEST_F(DlssRrClearSync, CapturedTwoImageSequenceCorrectsOnlyItsTwoInitializationClears) {
  BarrierRequest initA, initB, later; ClearRequest clearA, clearB;
  initB.images[0] = initialization(imageB); clearB.image = imageB;
  later.images[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  rrSync::Scope scope(commands, true);
  send(initA); send(clearA); send(initB); send(clearB); send(later); send(clearA);
  ASSERT_EQ(calls.size(), 10u); EXPECT_EQ(scope.correctedClears(), 2u);
  preserved(initA, calls[0]); preserved(clearA, calls[2]);
  preserved(initB, calls[4]); preserved(clearB, calls[6]);
  preserved(later, calls[8]); preserved(clearA, calls[9]);
}

TEST_F(DlssRrClearSync, ScopeOwnershipSurvivesNestedScopeAndExceptionWithoutLeakingTheCandidate) {
  BarrierRequest init; ClearRequest reset;
  EXPECT_THROW({
    rrSync::Scope owner(commands, true); send(init);
    {rrSync::Scope nested(otherCommands, true); EXPECT_EQ(nested.correctedClears(), 0u);}
    send(reset); EXPECT_EQ(owner.correctedClears(), 1u);
    throw std::runtime_error("simulated NGX failure");
  }, std::runtime_error);
  EXPECT_EQ(rrSync::activeState, nullptr);
  send(init); send(reset);
  ASSERT_EQ(calls.size(), 6u); preserved(init, calls[4]); preserved(reset, calls[5]);
}
} // namespace
