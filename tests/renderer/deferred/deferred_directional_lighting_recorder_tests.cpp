#include "Container/renderer/deferred/DeferredDirectionalLightingRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredDirectionalLightingRecordInputs;
using container::renderer::recordDeferredDirectionalLightingCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredDirectionalLightingRecordInputs requiredInputs() {
  return {
      .pipeline = fakeHandle<VkPipeline>(0x1),
      .pipelineLayout = fakeHandle<VkPipelineLayout>(0x2),
      .descriptorSets = {fakeHandle<VkDescriptorSet>(0x3),
                         fakeHandle<VkDescriptorSet>(0x4),
                         fakeHandle<VkDescriptorSet>(0x5)},
      .ddgiDescriptorSet = fakeHandle<VkDescriptorSet>(0x6)};
}

} // namespace

TEST(DeferredDirectionalLightingRecorderTests, MissingPipelineReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.pipeline = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}

TEST(DeferredDirectionalLightingRecorderTests,
     MissingPipelineLayoutReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}

TEST(DeferredDirectionalLightingRecorderTests,
     MissingLightingDescriptorSetReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.descriptorSets[0] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}

TEST(DeferredDirectionalLightingRecorderTests,
     MissingLightDescriptorSetReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.descriptorSets[1] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}

TEST(DeferredDirectionalLightingRecorderTests,
     MissingSceneDescriptorSetReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.descriptorSets[2] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}

TEST(DeferredDirectionalLightingRecorderTests,
     MissingDdgiFallbackDescriptorSetReturnsFalse) {
  DeferredDirectionalLightingRecordInputs inputs = requiredInputs();
  inputs.ddgiDescriptorSet = VK_NULL_HANDLE;
  EXPECT_FALSE(recordDeferredDirectionalLightingCommands(VK_NULL_HANDLE,
                                                        inputs));
}
