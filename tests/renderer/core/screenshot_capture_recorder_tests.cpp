#include "Container/renderer/core/ScreenshotCaptureRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::ScreenshotCopyInputs;
using container::renderer::hasScreenshotCopyWork;

VkCommandBuffer fakeCommandBuffer() {
  return reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x1));
}

VkImage fakeSwapChainImage() {
  return reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x2));
}

VkBuffer fakeReadbackBuffer() {
  return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0x3));
}

TEST(ScreenshotCaptureRecorderTests, RejectsIncompleteCopyInputs) {
  EXPECT_FALSE(hasScreenshotCopyWork({}));
  EXPECT_FALSE(hasScreenshotCopyWork({.commandBuffer = fakeCommandBuffer(),
                                      .sourceImage = fakeSwapChainImage(),
                                      .readbackBuffer = fakeReadbackBuffer(),
                                      .extent = {0u, 16u}}));
  EXPECT_FALSE(hasScreenshotCopyWork({.commandBuffer = fakeCommandBuffer(),
                                      .sourceImage = fakeSwapChainImage(),
                                      .readbackBuffer = fakeReadbackBuffer(),
                                      .extent = {16u, 0u}}));
  EXPECT_FALSE(hasScreenshotCopyWork({.commandBuffer = fakeCommandBuffer(),
                                      .sourceImage = VK_NULL_HANDLE,
                                      .readbackBuffer = fakeReadbackBuffer(),
                                      .extent = {16u, 16u}}));
}

TEST(ScreenshotCaptureRecorderTests, AcceptsCompleteCopyInputs) {
  const ScreenshotCopyInputs inputs{.commandBuffer = fakeCommandBuffer(),
                                    .sourceImage = fakeSwapChainImage(),
                                    .readbackBuffer = fakeReadbackBuffer(),
                                    .extent = {1280u, 720u}};
  EXPECT_TRUE(hasScreenshotCopyWork(inputs));
}

}  // namespace
