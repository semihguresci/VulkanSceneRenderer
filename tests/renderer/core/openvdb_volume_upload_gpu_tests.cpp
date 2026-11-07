#include "Container/common/CommonGLFW.h"
#include "Container/renderer/effects/OpenVdbVolumeRenderer.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace container::gpu;
using namespace container::renderer;
using container::geometry::OpenVdbVolume;
using namespace std::chrono_literals;

// A queue submission waits for a host timeline signal before the production
// upload. This guarantees a pending fence even on a fast GPU. The watchdog also
// releases an accidentally blocking API call, so a regression fails promptly.
class QueueGate {
public:
  explicit QueueGate(VulkanDevice& device) : device_(device.device()) {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    if (vkCreateSemaphore(device_, &info, nullptr, &semaphore_) != VK_SUCCESS)
      throw std::runtime_error("Failed to create OpenVDB upload test gate");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence, nullptr, &fence_) != VK_SUCCESS) {
      vkDestroySemaphore(device_, semaphore_, nullptr);
      throw std::runtime_error("Failed to create OpenVDB upload test gate fence");
    }
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = semaphore_;
    wait.value = 1;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    if (vkQueueSubmit2(device.graphicsQueue(), 1, &submit, fence_) != VK_SUCCESS) {
      vkDestroyFence(device_, fence_, nullptr);
      vkDestroySemaphore(device_, semaphore_, nullptr);
      throw std::runtime_error("Failed to submit OpenVDB upload test gate");
    }
    watchdog_ = std::jthread([this] {
      std::unique_lock lock(mutex_);
      if (!changed_.wait_for(lock, 5s, [&] { return released_; })) {
        watchdogFired_ = true;
        lock.unlock();
        release();
      }
    });
  }
  ~QueueGate() {
    release();
    watchdog_.join();
    EXPECT_EQ(signalResult_.load(), VK_SUCCESS);
    // No queue/device idle call precedes releasing the gate.
    EXPECT_EQ(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 10'000'000'000ull), VK_SUCCESS);
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroySemaphore(device_, semaphore_, nullptr);
  }
  void release() {
    {
      std::lock_guard lock(mutex_);
      if (released_) return;
      VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
      signal.semaphore = semaphore_;
      signal.value = 1;
      signalResult_ = vkSignalSemaphore(device_, &signal);
      released_ = true;
    }
    changed_.notify_all();
  }
  [[nodiscard]] bool watchdogFired() const { return watchdogFired_.load(); }

private:
  VkDevice device_{};
  VkSemaphore semaphore_{};
  VkFence fence_{};
  std::mutex mutex_;
  std::condition_variable changed_;
  bool released_{false};
  std::atomic_bool watchdogFired_{false};
  std::atomic<VkResult> signalResult_{VK_SUCCESS};
  std::jthread watchdog_;
};

class OpenVdbVolumeUploadGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<OpenVdbVolumeUploadGpu*>(user)->validationErrors_;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted_ = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(32, 32, "OpenVDB staged upload lifetime", nullptr, nullptr);
    ASSERT_NE(window_, nullptr);
    uint32_t count{};
    const auto* extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance_ = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_->raii(), debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance_->instance(), window_, nullptr, &handle), VK_SUCCESS);
    surface_ = vk::raii::SurfaceKHR(instance_->raii(), handle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enabledFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    // The production VMA owner enables address-capable allocations globally.
    v12.bufferDeviceAddress = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    v12.separateDepthStencilLayouts = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.dynamicRendering = VK_TRUE;
    v13.pNext = &v12;
    deviceInfo.next = &v13;
    device_ = std::make_shared<VulkanDevice>(*instance_, handle, deviceInfo);
    externalPool_ = vk::raii::CommandPool(device_->raii(), vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device_->queueFamilyIndices().graphicsFamily.value()));
    allocator_ = std::make_unique<AllocationManager>();
    allocator_->initialize(instance_->instance(), device_->physicalDevice(), device_->device(),
                           device_->graphicsQueue(), *externalPool_, {});
    pipelines_ = std::make_unique<PipelineManager>(device_->device());
    renderer_ = std::make_unique<OpenVdbVolumeRenderer>(device_, *allocator_, *pipelines_, *externalPool_);
    const std::array volumes{volume(-1.0f, 1.0f), volume(4.0f, 5.0f)};
    renderer_->initialize(CONTAINER_OPENVDB_SHADER_ROOT, volumes);
  }

  void TearDown() override {
    // Every local gate releases itself before reaching fixture cleanup.
    if (device_) EXPECT_EQ(vkDeviceWaitIdle(device_->device()), VK_SUCCESS);
    renderer_.reset();
    pipelines_.reset();
    allocator_.reset();
    externalPool_.clear();
    device_.reset();
    surface_.clear();
    messenger_.clear();
    instance_.reset();
    if (window_) glfwDestroyWindow(window_);
    if (glfwStarted_) glfwTerminate();
    EXPECT_EQ(validationErrors_.load(), 0u);
  }

  static OpenVdbVolume volume(float minimum, float maximum, bool temperature = false) {
    OpenVdbVolume result;
    result.gridName = "density";
    result.dimensions = {16, 16, 16};
    result.boundsMin = {minimum, -1, -1};
    result.boundsMax = {maximum, 1, 1};
    result.density.assign(16 * 16 * 16, 1.0f);
    result.maxDensity = 1;
    if (temperature) {
      result.temperatureGridName = "temperature";
      result.temperature.assign(result.density.size(), 4000.0f);
    }
    return result;
  }

  bool waitReady() {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      if (renderer_->volumeReplacementReady()) return true;
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return renderer_->volumeReplacementReady();
  }

  GLFWwindow* window_{};
  bool glfwStarted_{false};
  std::atomic_uint validationErrors_{};
  std::unique_ptr<VulkanInstance> instance_;
  vk::raii::DebugUtilsMessengerEXT messenger_{nullptr};
  vk::raii::SurfaceKHR surface_{nullptr};
  std::shared_ptr<VulkanDevice> device_;
  vk::raii::CommandPool externalPool_{nullptr};
  std::unique_ptr<AllocationManager> allocator_;
  std::unique_ptr<PipelineManager> pipelines_;
  std::unique_ptr<OpenVdbVolumeRenderer> renderer_;
};

TEST_F(OpenVdbVolumeUploadGpu, PendingUploadOwnsSourcesAndRejectsEarlyPublication) {
  EXPECT_FALSE(renderer_->volumeReplacementPending());
  EXPECT_FALSE(renderer_->volumeReplacementReady());
  EXPECT_FALSE(renderer_->publishVolumeReplacement());
  EXPECT_FALSE(renderer_->discardVolumeReplacement());
  auto replacement = volume(-3, -2, true);
  QueueGate gate(*device_);
  ASSERT_TRUE(renderer_->beginReplaceVolume(0, replacement));
  // No borrowed CPU vectors may survive begin; publication happens later.
  replacement = {};
  EXPECT_TRUE(renderer_->volumeReplacementPending());
  EXPECT_FALSE(renderer_->beginReplaceVolume(0, volume(-5, -4)));
  EXPECT_FALSE(renderer_->volumeReplacementReady());
  EXPECT_FALSE(renderer_->publishVolumeReplacement());
  EXPECT_FALSE(renderer_->discardVolumeReplacement());
  EXPECT_FLOAT_EQ(renderer_->minimum()[0], -1);
  EXPECT_FLOAT_EQ(renderer_->maximum()[0], 5);
  EXPECT_FALSE(gate.watchdogFired());
  gate.release();
  ASSERT_TRUE(waitReady());
  ASSERT_TRUE(renderer_->publishVolumeReplacement());
  EXPECT_EQ(renderer_->volumeCount(), 2u);
  EXPECT_FLOAT_EQ(renderer_->minimum()[0], -3);
  EXPECT_FLOAT_EQ(renderer_->maximum()[0], 5); // Other medium retained.
  EXPECT_FALSE(renderer_->volumeReplacementPending());
  EXPECT_FALSE(renderer_->publishVolumeReplacement());
}

TEST_F(OpenVdbVolumeUploadGpu, ReadyDiscardAndInvalidPayloadPreserveDisplayedMedia) {
  auto invalid = volume(-3, -2);
  invalid.density.pop_back();
  EXPECT_THROW(renderer_->beginReplaceVolume(0, invalid), std::invalid_argument);
  EXPECT_FALSE(renderer_->volumeReplacementPending());
  EXPECT_THROW(renderer_->beginReplaceVolume(2, volume(-3, -2)), std::out_of_range);
  QueueGate gate(*device_);
  ASSERT_TRUE(renderer_->beginReplaceVolume(0, volume(-3, -2)));
  EXPECT_FALSE(renderer_->discardVolumeReplacement());
  EXPECT_FALSE(gate.watchdogFired());
  gate.release();
  ASSERT_TRUE(waitReady());
  ASSERT_TRUE(renderer_->discardVolumeReplacement());
  EXPECT_FALSE(renderer_->volumeReplacementPending());
  EXPECT_FALSE(renderer_->discardVolumeReplacement());
  EXPECT_FLOAT_EQ(renderer_->minimum()[0], -1);
  EXPECT_FLOAT_EQ(renderer_->maximum()[0], 5);
  ASSERT_TRUE(renderer_->beginReplaceVolume(0, volume(-5, -4)));
  ASSERT_TRUE(waitReady());
  ASSERT_TRUE(renderer_->publishVolumeReplacement());
  EXPECT_FLOAT_EQ(renderer_->minimum()[0], -5);
}

TEST_F(OpenVdbVolumeUploadGpu, PendingUploadSurvivesExternalFramePoolReset) {
  QueueGate gate(*device_);
  ASSERT_TRUE(renderer_->beginReplaceVolume(0, volume(-3, -2)));
  EXPECT_FALSE(renderer_->volumeReplacementReady());
  // Frame-resource recreation can reset the supplied pool while the volume
  // renderer's dedicated pool retains its independent pending commands.
  ASSERT_EQ(vkResetCommandPool(device_->device(), *externalPool_, 0), VK_SUCCESS);
  EXPECT_FALSE(gate.watchdogFired());
  gate.release();
  ASSERT_TRUE(waitReady());
  ASSERT_TRUE(renderer_->publishVolumeReplacement());
  EXPECT_FLOAT_EQ(renderer_->minimum()[0], -3);
}

TEST_F(OpenVdbVolumeUploadGpu, DestructorWaitsForPendingUploadBeforeFreeingResources) {
  QueueGate gate(*device_);
  ASSERT_TRUE(renderer_->beginReplaceVolume(0, volume(-3, -2, true)));
  ASSERT_FALSE(renderer_->volumeReplacementReady());
  // The main thread destroys the renderer while the GPU is still gated. Only
  // the test's host signal runs on this helper; production Vulkan APIs remain
  // on the owning thread. The watchdog prevents a broken path from hanging.
  std::jthread release([&] {
    std::this_thread::sleep_for(30ms);
    gate.release();
  });
  renderer_.reset();
  release.join();
  EXPECT_FALSE(gate.watchdogFired());
}
} // namespace

int main(int argc, char** argv) {
  const char* enabled = std::getenv("CONTAINER_RUN_OPENVDB_UPLOAD_GPU");
  if (!enabled || std::strcmp(enabled, "1") != 0) {
    std::puts("SKIP: set CONTAINER_RUN_OPENVDB_UPLOAD_GPU=1 for live staged upload tests");
    return 77;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
