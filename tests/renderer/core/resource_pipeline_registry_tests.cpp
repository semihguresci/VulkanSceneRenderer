#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/deferred/DeferredRasterPipelineBridge.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/forward/ForwardRasterPipelineBridge.h"
#include "Container/renderer/forward/ForwardRasterResourceBridge.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/pipeline/PipelineTypes.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/renderer/shadow/ShadowResourceBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using container::renderer::buildGraphicsPipelineHandleRegistry;
using container::renderer::buildGraphicsPipelineLayoutRegistry;
using container::renderer::clampMsaaSampleCount;
using container::renderer::deferredRasterBuffer;
using container::renderer::deferredRasterBufferBinding;
using container::renderer::DeferredRasterBufferId;
using container::renderer::deferredRasterBufferSize;
using container::renderer::deferredRasterDescriptorBinding;
using container::renderer::deferredRasterDescriptorSet;
using container::renderer::DeferredRasterDescriptorSetId;
using container::renderer::deferredRasterFramebuffer;
using container::renderer::deferredRasterFramebufferBinding;
using container::renderer::DeferredRasterFramebufferId;
using container::renderer::DeferredRasterFrameGraphContext;
using container::renderer::DeferredRasterFrameGraphServices;
using container::renderer::deferredRasterImage;
using container::renderer::deferredRasterImageBinding;
using container::renderer::DeferredRasterImageId;
using container::renderer::deferredRasterImageView;
using container::renderer::deferredRasterPipelineHandle;
using container::renderer::DeferredRasterPipelineId;
using container::renderer::deferredRasterPipelineLayout;
using container::renderer::DeferredRasterPipelineLayoutId;
using container::renderer::deferredRasterPipelineLayoutReady;
using container::renderer::deferredRasterPipelineReady;
using container::renderer::deferredRasterRenderPass;
using container::renderer::deferredRasterSampler;
using container::renderer::deferredRasterSamplerBinding;
using container::renderer::DeferredRasterSamplerId;
using container::renderer::DeferredRasterTechnique;
using container::renderer::forwardRasterBuffer;
using container::renderer::forwardRasterBufferBinding;
using container::renderer::ForwardRasterBufferId;
using container::renderer::forwardRasterBufferReady;
using container::renderer::forwardRasterBufferSize;
using container::renderer::forwardRasterDescriptorBinding;
using container::renderer::forwardRasterDescriptorSet;
using container::renderer::ForwardRasterDescriptorSetId;
using container::renderer::forwardRasterDescriptorSetReady;
using container::renderer::forwardRasterFramebuffer;
using container::renderer::forwardRasterFramebufferBinding;
using container::renderer::ForwardRasterFramebufferId;
using container::renderer::forwardRasterFramebufferReady;
using container::renderer::forwardRasterImage;
using container::renderer::forwardRasterImageBinding;
using container::renderer::ForwardRasterImageId;
using container::renderer::forwardRasterImageReady;
using container::renderer::forwardRasterImageView;
using container::renderer::forwardRasterImageViewReady;
using container::renderer::forwardRasterPipelineHandle;
using container::renderer::ForwardRasterPipelineId;
using container::renderer::forwardRasterPipelineLayout;
using container::renderer::ForwardRasterPipelineLayoutId;
using container::renderer::forwardRasterPipelineLayoutReady;
using container::renderer::forwardRasterPipelineReady;
using container::renderer::forwardRasterRenderPass;
using container::renderer::forwardRasterRenderPassReady;
using container::renderer::FrameBufferBinding;
using container::renderer::FrameBufferDesc;
using container::renderer::FrameDescriptorBinding;
using container::renderer::FrameFramebufferBinding;
using container::renderer::FrameFramebufferDesc;
using container::renderer::FrameImageBinding;
using container::renderer::FrameImageDesc;
using container::renderer::FrameRecorder;
using container::renderer::FrameRecordParams;
using container::renderer::FrameResourceKind;
using container::renderer::FrameResourceLifetime;
using container::renderer::FrameResourceRegistry;
using container::renderer::FrameSamplerBinding;
using container::renderer::FrameSamplerDesc;
using container::renderer::GraphicsPipelines;
using container::renderer::PipelineLayouts;
using container::renderer::PipelineRecipe;
using container::renderer::PipelineRecipeKind;
using container::renderer::PipelineRegistry;
using container::renderer::RegisteredPipelineHandle;
using container::renderer::RegisteredPipelineLayout;
using container::renderer::RenderSystemContext;
using container::renderer::RenderTechniqueId;
using container::renderer::sampleCountFromSamples;
using container::renderer::sampleCountToSamples;
using container::renderer::shadowDescriptorBinding;
using container::renderer::shadowDescriptorSet;
using container::renderer::ShadowDescriptorSetId;
using container::renderer::shadowPipelineHandle;
using container::renderer::ShadowPipelineId;
using container::renderer::shadowPipelineLayout;
using container::renderer::ShadowPipelineLayoutId;
using container::renderer::shadowPipelineLayoutReady;
using container::renderer::shadowPipelineReady;
using container::renderer::supportedMsaaSampleCounts;
using container::renderer::TechniquePipelineKey;
using container::renderer::TechniqueResourceKey;

std::string readRepoTextFile(const std::filesystem::path &relativePath) {
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

std::string definitionBlock(const std::string &text,
                            const std::string &declarationName) {
  const std::size_t start = text.find(declarationName);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end = text.find("};", start);
  if (end == std::string::npos) {
    return {};
  }
  return text.substr(start, end - start);
}

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(FrameResourceRegistryTests, RegistersTechniqueScopedResources) {
  FrameResourceRegistry registry;

  registry.registerImage(RenderTechniqueId::DeferredRaster, "scene-color",
                         FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(RenderTechniqueId::PathTracing, "scene-color",
                         FrameImageDesc{.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_STORAGE_BIT});
  registry.registerBuffer(
      RenderTechniqueId::PathTracing, "accumulation-state",
      FrameBufferDesc{.size = 4096,
                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::Persistent);
  registry.registerSampler(RenderTechniqueId::DeferredRaster,
                           "g-buffer-sampler", FrameSamplerDesc{},
                           FrameResourceLifetime::Imported);

  EXPECT_EQ(registry.size(), 4u);
  const auto *deferredSceneColor = registry.find(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "scene-color"});
  ASSERT_NE(deferredSceneColor, nullptr);
  EXPECT_EQ(deferredSceneColor->kind, FrameResourceKind::Image);
  EXPECT_EQ(deferredSceneColor->image.format, VK_FORMAT_R16G16B16A16_SFLOAT);

  const auto pathResources =
      registry.resourcesForTechnique(RenderTechniqueId::PathTracing);
  EXPECT_EQ(pathResources.size(), 2u);
  const auto *gBufferSampler = registry.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler"});
  ASSERT_NE(gBufferSampler, nullptr);
  EXPECT_EQ(gBufferSampler->kind, FrameResourceKind::Sampler);
}

TEST(FrameResourceRegistryTests,
     RegistersPathTracingAccumulationWithoutFrameResourceFields) {
  FrameResourceRegistry registry;

  registry.registerImage(RenderTechniqueId::PathTracing, "accumulation-color",
                         FrameImageDesc{.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT},
                         FrameResourceLifetime::Persistent);
  registry.registerBuffer(
      RenderTechniqueId::PathTracing, "sample-state",
      FrameBufferDesc{.size = sizeof(uint32_t),
                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::Persistent);

  const auto pathResources =
      registry.resourcesForTechnique(RenderTechniqueId::PathTracing);
  EXPECT_EQ(pathResources.size(), 2u);
  const auto *accumulation = registry.find(TechniqueResourceKey{
      RenderTechniqueId::PathTracing, "accumulation-color"});
  ASSERT_NE(accumulation, nullptr);
  EXPECT_EQ(accumulation->kind, FrameResourceKind::Image);
  EXPECT_EQ(accumulation->lifetime, FrameResourceLifetime::Persistent);

  const std::string frameResources =
      readRepoTextFile("include/Container/renderer/resources/FrameResources.h");
  EXPECT_FALSE(contains(frameResources, "PathTracing"));
  EXPECT_FALSE(contains(frameResources, "accumulation"));
}

TEST(FrameResourceRegistryTests, RejectsInvalidAndDuplicateResourceKeys) {
  FrameResourceRegistry registry;
  registry.registerExternal(RenderTechniqueId::DeferredRaster, "swapchain");

  EXPECT_THROW(
      registry.registerExternal(RenderTechniqueId::DeferredRaster, "swapchain"),
      std::invalid_argument);
  EXPECT_THROW(registry.registerExternal(RenderTechniqueId::DeferredRaster, ""),
               std::invalid_argument);
}

TEST(FrameResourceRegistryTests, ClearsOnlyRequestedTechniqueResources) {
  FrameResourceRegistry registry;
  registry.registerExternal(RenderTechniqueId::DeferredRaster, "swapchain");
  registry.registerExternal(RenderTechniqueId::RayTracing, "tlas");
  registry.bindImage(RenderTechniqueId::DeferredRaster, "scene-color", 0u,
                     FrameImageBinding{.image = fakeHandle<VkImage>(0x1),
                                       .view = fakeHandle<VkImageView>(0x2),
                                       .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                       .extent = {640, 480, 1},
                                       .usage = VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.bindBuffer(
      RenderTechniqueId::RayTracing, "accumulation-state", 0u,
      FrameBufferBinding{.buffer = fakeHandle<VkBuffer>(0x3),
                         .size = 4096,
                         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});

  registry.clearTechnique(RenderTechniqueId::RayTracing);

  EXPECT_TRUE(registry.contains(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "swapchain"}));
  EXPECT_FALSE(registry.contains(
      TechniqueResourceKey{RenderTechniqueId::RayTracing, "tlas"}));
  EXPECT_NE(registry.findBinding(
                TechniqueResourceKey{RenderTechniqueId::DeferredRaster,
                                     "scene-color"},
                0u),
            nullptr);
  EXPECT_EQ(
      registry.findBinding(TechniqueResourceKey{RenderTechniqueId::RayTracing,
                                                "accumulation-state"},
                           0u),
      nullptr);
}

TEST(FrameResourceRegistryTests, BindsProductionFrameResourcesByHandle) {
  FrameResourceRegistry registry;

  const auto sceneColor = registry.bindImage(
      RenderTechniqueId::DeferredRaster, "scene-color", 1u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x10),
                        .view = fakeHandle<VkImageView>(0x11),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .extent = {1920, 1080, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT});
  ASSERT_TRUE(sceneColor.valid());

  const auto *byHandle = registry.findBinding(sceneColor);
  ASSERT_NE(byHandle, nullptr);
  EXPECT_EQ(byHandle->key.name, "scene-color");
  EXPECT_EQ(byHandle->frameIndex, 1u);
  EXPECT_EQ(byHandle->kind, FrameResourceKind::Image);
  EXPECT_EQ(byHandle->image.view, fakeHandle<VkImageView>(0x11));

  const auto *byKey = registry.findBinding(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "scene-color"},
      1u);
  ASSERT_NE(byKey, nullptr);
  EXPECT_EQ(byKey->handle, sceneColor);

  const auto reboundSceneColor = registry.bindImage(
      RenderTechniqueId::DeferredRaster, "scene-color", 1u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x12),
                        .view = fakeHandle<VkImageView>(0x13),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .extent = {1280, 720, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
  EXPECT_EQ(reboundSceneColor, sceneColor);
  ASSERT_EQ(registry.bindingCount(), 1u);
  EXPECT_EQ(registry.findBinding(sceneColor)->image.view,
            fakeHandle<VkImageView>(0x13));

  const auto oitNodes = registry.bindBuffer(
      RenderTechniqueId::DeferredRaster, "oit-node-buffer", 1u,
      FrameBufferBinding{.buffer = fakeHandle<VkBuffer>(0x20),
                         .size = 8192,
                         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  ASSERT_TRUE(oitNodes.valid());
  EXPECT_EQ(registry.findBinding(oitNodes)->buffer.size, 8192u);

  const auto gbufferFramebuffer = registry.bindFramebuffer(
      RenderTechniqueId::DeferredRaster, "gbuffer-framebuffer", 1u,
      FrameFramebufferBinding{.framebuffer = fakeHandle<VkFramebuffer>(0x40),
                              .renderPass = fakeHandle<VkRenderPass>(0x41),
                              .extent = {1920, 1080},
                              .attachmentCount = 7u});
  ASSERT_TRUE(gbufferFramebuffer.valid());
  const auto *framebufferBinding = registry.findBinding(gbufferFramebuffer);
  ASSERT_NE(framebufferBinding, nullptr);
  EXPECT_EQ(framebufferBinding->kind, FrameResourceKind::Framebuffer);
  EXPECT_EQ(framebufferBinding->framebuffer.framebuffer,
            fakeHandle<VkFramebuffer>(0x40));
  EXPECT_EQ(framebufferBinding->framebuffer.renderPass,
            fakeHandle<VkRenderPass>(0x41));
  EXPECT_EQ(framebufferBinding->framebuffer.attachmentCount, 7u);

  const auto reboundGbufferFramebuffer = registry.bindFramebuffer(
      RenderTechniqueId::DeferredRaster, "gbuffer-framebuffer", 1u,
      FrameFramebufferBinding{.framebuffer = fakeHandle<VkFramebuffer>(0x42),
                              .renderPass = fakeHandle<VkRenderPass>(0x43),
                              .extent = {1280, 720},
                              .attachmentCount = 7u});
  EXPECT_EQ(reboundGbufferFramebuffer, gbufferFramebuffer);
  EXPECT_EQ(registry.findBinding(gbufferFramebuffer)->framebuffer.framebuffer,
            fakeHandle<VkFramebuffer>(0x42));

  const auto frameLightingDescriptor = registry.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "frame-lighting-descriptor-set", 1u,
      FrameDescriptorBinding{.descriptorSet =
                                 fakeHandle<VkDescriptorSet>(0x50)});
  ASSERT_TRUE(frameLightingDescriptor.valid());
  const auto *descriptorBinding = registry.findBinding(frameLightingDescriptor);
  ASSERT_NE(descriptorBinding, nullptr);
  EXPECT_EQ(descriptorBinding->kind, FrameResourceKind::DescriptorSet);
  EXPECT_EQ(descriptorBinding->descriptor.descriptorSet,
            fakeHandle<VkDescriptorSet>(0x50));

  const auto reboundFrameLightingDescriptor = registry.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "frame-lighting-descriptor-set", 1u,
      FrameDescriptorBinding{.descriptorSet =
                                 fakeHandle<VkDescriptorSet>(0x51)});
  EXPECT_EQ(reboundFrameLightingDescriptor, frameLightingDescriptor);
  EXPECT_EQ(
      registry.findBinding(frameLightingDescriptor)->descriptor.descriptorSet,
      fakeHandle<VkDescriptorSet>(0x51));

  const auto gBufferSampler = registry.bindSampler(
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler", 1u,
      FrameSamplerBinding{.sampler = fakeHandle<VkSampler>(0x60)});
  ASSERT_TRUE(gBufferSampler.valid());
  const auto *samplerBinding = registry.findBinding(gBufferSampler);
  ASSERT_NE(samplerBinding, nullptr);
  EXPECT_EQ(samplerBinding->kind, FrameResourceKind::Sampler);
  EXPECT_EQ(samplerBinding->sampler.sampler, fakeHandle<VkSampler>(0x60));

  EXPECT_EQ(
      registry.bindingsForFrame(RenderTechniqueId::DeferredRaster, 1u).size(),
      5u);

  std::vector<std::string> visitedBindingNames;
  registry.forEachBindingForFrame(RenderTechniqueId::DeferredRaster, 1u,
                                  [&visitedBindingNames](const auto &binding) {
                                    visitedBindingNames.push_back(
                                        binding.key.name);
                                  });
  EXPECT_EQ(visitedBindingNames.size(), 5u);
  EXPECT_NE(std::find(visitedBindingNames.begin(), visitedBindingNames.end(),
                      "scene-color"),
            visitedBindingNames.end());
  EXPECT_NE(std::find(visitedBindingNames.begin(), visitedBindingNames.end(),
                      "frame-lighting-descriptor-set"),
            visitedBindingNames.end());

  registry.clearBindings();
  EXPECT_EQ(registry.findBinding(sceneColor), nullptr);
  const auto recreatedSceneColor = registry.bindImage(
      RenderTechniqueId::DeferredRaster, "scene-color", 1u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x30),
                        .view = fakeHandle<VkImageView>(0x31),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .extent = {1920, 1080, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
  EXPECT_NE(recreatedSceneColor, sceneColor);
}

TEST(FrameResourceRegistryTests, RegistersSamplerContractsWithMetadata) {
  FrameResourceRegistry registry;

  registry.registerSampler(RenderTechniqueId::DeferredRaster,
                           "g-buffer-sampler", FrameSamplerDesc{},
                           FrameResourceLifetime::Imported);

  const auto *sampler = registry.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler"});
  ASSERT_NE(sampler, nullptr);
  EXPECT_EQ(sampler->kind, FrameResourceKind::Sampler);
  EXPECT_EQ(sampler->lifetime, FrameResourceLifetime::Imported);
}

TEST(FrameResourceRegistryTests, StoresImageSampleCountMetadata) {
  FrameResourceRegistry registry;

  registry.registerImage(
      RenderTechniqueId::DeferredRaster, "gbuffer-albedo-msaa",
      FrameImageDesc{.format = VK_FORMAT_R8G8B8A8_UNORM,
                     .extent = {1920, 1080, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     .samples = VK_SAMPLE_COUNT_4_BIT});

  const auto *contract = registry.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "gbuffer-albedo-msaa"});
  ASSERT_NE(contract, nullptr);
  EXPECT_EQ(contract->image.samples, VK_SAMPLE_COUNT_4_BIT);

  const auto handle = registry.bindImage(
      RenderTechniqueId::DeferredRaster, "gbuffer-albedo-msaa", 0u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x200),
                        .view = fakeHandle<VkImageView>(0x201),
                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                        .extent = {1920, 1080, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                        .samples = VK_SAMPLE_COUNT_4_BIT});
  ASSERT_TRUE(handle.valid());

  const auto *binding = registry.findBinding(handle);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->image.samples, VK_SAMPLE_COUNT_4_BIT);
}

TEST(FrameResourceRegistryTests, RegistersFramebufferContractsWithMetadata) {
  FrameResourceRegistry registry;

  registry.registerFramebuffer(RenderTechniqueId::DeferredRaster,
                               "gbuffer-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 7u},
                               FrameResourceLifetime::PerFrame);

  const auto *gbuffer = registry.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "gbuffer-framebuffer"});
  ASSERT_NE(gbuffer, nullptr);
  EXPECT_EQ(gbuffer->kind, FrameResourceKind::Framebuffer);
  EXPECT_EQ(gbuffer->lifetime, FrameResourceLifetime::PerFrame);
  EXPECT_EQ(gbuffer->framebuffer.attachmentCount, 7u);
}

TEST(RendererMsaaTests, ConvertsSampleCountsToUserVisibleIntegers) {
  EXPECT_EQ(sampleCountToSamples(VK_SAMPLE_COUNT_1_BIT), 1u);
  EXPECT_EQ(sampleCountToSamples(VK_SAMPLE_COUNT_2_BIT), 2u);
  EXPECT_EQ(sampleCountToSamples(VK_SAMPLE_COUNT_4_BIT), 4u);
  EXPECT_EQ(sampleCountToSamples(VK_SAMPLE_COUNT_8_BIT), 8u);
  EXPECT_EQ(sampleCountFromSamples(0u), VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(sampleCountFromSamples(3u), VK_SAMPLE_COUNT_2_BIT);
  EXPECT_EQ(sampleCountFromSamples(6u), VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(sampleCountFromSamples(16u), VK_SAMPLE_COUNT_16_BIT);
}

TEST(RendererMsaaTests, ClampsRequestedSamplesToColorAndDepthSupport) {
  const VkSampleCountFlags color =
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT |
      VK_SAMPLE_COUNT_8_BIT;
  const VkSampleCountFlags depth =
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;

  EXPECT_EQ(clampMsaaSampleCount(8u, color, depth), VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(clampMsaaSampleCount(4u, color, depth), VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(clampMsaaSampleCount(2u, color, depth), VK_SAMPLE_COUNT_2_BIT);
  EXPECT_EQ(clampMsaaSampleCount(1u, color, depth), VK_SAMPLE_COUNT_1_BIT);
}

TEST(RendererMsaaTests, ListsSupportedSamplesInAscendingUiOrder) {
  const VkSampleCountFlags color =
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT |
      VK_SAMPLE_COUNT_8_BIT;
  const VkSampleCountFlags depth =
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;

  const auto samples = supportedMsaaSampleCounts(color, depth);

  ASSERT_EQ(samples.size(), 3u);
  EXPECT_EQ(samples[0], VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(samples[1], VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(samples[2], VK_SAMPLE_COUNT_8_BIT);
}

TEST(PipelineRegistryTests, RegistersTechniqueScopedRecipes) {
  PipelineRegistry registry;

  registry.registerRecipe(
      PipelineRecipe{.key = {RenderTechniqueId::DeferredRaster, "gbuffer"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"gbuffer.slang"},
                     .layoutName = "scene"});
  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::RayTracing, "primary-rays"},
      .kind = PipelineRecipeKind::RayTracing,
      .shaderStages = {"raygen.slang", "closesthit.slang", "miss.slang"},
      .layoutName = "ray-tracing"});

  const auto *rayRecipe = registry.find(
      TechniquePipelineKey{RenderTechniqueId::RayTracing, "primary-rays"});
  ASSERT_NE(rayRecipe, nullptr);
  EXPECT_EQ(rayRecipe->kind, PipelineRecipeKind::RayTracing);
  EXPECT_EQ(rayRecipe->shaderStages.size(), 3u);
  EXPECT_EQ(
      registry.recipesForTechnique(RenderTechniqueId::DeferredRaster).size(),
      1u);
}

TEST(PipelineRegistryTests,
     RegistersForwardRasterRecipesWithoutGraphicsPipelineFields) {
  PipelineRegistry registry;

  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/forward_opaque.vert.spv",
                       "spv_shaders/forward_opaque.frag.spv"},
      .layoutName = "transparent"});
  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::ForwardRaster, "forward-transparent"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/forward_transparent.vert.spv",
                       "spv_shaders/forward_transparent.frag.spv"},
      .layoutName = "transparent"});
  registry.registerRecipe(
      PipelineRecipe{.key = {RenderTechniqueId::ForwardRaster, "post-process"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/post_process.vert.spv",
                                      "spv_shaders/post_process.frag.spv"},
                     .layoutName = "post-process"});

  const auto forwardRecipes =
      registry.recipesForTechnique(RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(forwardRecipes.size(), 3u);
  const auto *opaque = registry.find(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "forward-opaque"});
  ASSERT_NE(opaque, nullptr);
  EXPECT_EQ(opaque->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(opaque->layoutName, "transparent");
  const auto *forwardTransparent = registry.find(TechniquePipelineKey{
      RenderTechniqueId::ForwardRaster, "forward-transparent"});
  ASSERT_NE(forwardTransparent, nullptr);
  EXPECT_EQ(forwardTransparent->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(forwardTransparent->layoutName, "transparent");
  const auto *postProcess = registry.find(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "post-process"});
  ASSERT_NE(postProcess, nullptr);
  EXPECT_EQ(postProcess->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(postProcess->layoutName, "post-process");

  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  EXPECT_FALSE(contains(pipelineTypes, "ForwardRaster"));
  EXPECT_FALSE(contains(pipelineTypes, "forwardOpaque"));
  EXPECT_FALSE(contains(pipelineTypes, "forwardTransparent"));
  EXPECT_FALSE(contains(pipelineTypes, "forwardPlus"));
}

TEST(PipelineRegistryTests, RejectsInvalidAndDuplicateRecipeKeys) {
  PipelineRegistry registry;
  registry.registerRecipe(
      PipelineRecipe{.key = {RenderTechniqueId::DeferredRaster, "post-process"},
                     .kind = PipelineRecipeKind::Graphics});

  EXPECT_THROW(registry.registerRecipe(PipelineRecipe{
                   .key = {RenderTechniqueId::DeferredRaster, "post-process"},
                   .kind = PipelineRecipeKind::Graphics}),
               std::invalid_argument);
  EXPECT_THROW(registry.registerRecipe(PipelineRecipe{
                   .key = {RenderTechniqueId::DeferredRaster, ""},
                   .kind = PipelineRecipeKind::Compute}),
               std::invalid_argument);
}

TEST(PipelineRegistryTests, RegistersTechniqueScopedRuntimeHandles) {
  PipelineRegistry registry;
  const VkPipeline gbuffer = fakeHandle<VkPipeline>(0x101);

  registry.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::DeferredRaster, "gbuffer"},
      .pipeline = gbuffer});

  const auto *registered = registry.findHandle(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "gbuffer"});
  ASSERT_NE(registered, nullptr);
  EXPECT_EQ(registered->pipeline, gbuffer);
  EXPECT_EQ(registry.pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "gbuffer"}),
            gbuffer);
  EXPECT_EQ(
      registry.pipelineHandlesForTechnique(RenderTechniqueId::DeferredRaster)
          .size(),
      1u);

  EXPECT_THROW(registry.registerHandle(RegisteredPipelineHandle{
                   .key = {RenderTechniqueId::DeferredRaster, "gbuffer"},
                   .pipeline = fakeHandle<VkPipeline>(0x102)}),
               std::invalid_argument);
  EXPECT_THROW(registry.registerHandle(RegisteredPipelineHandle{
                   .key = {RenderTechniqueId::DeferredRaster, ""},
                   .pipeline = fakeHandle<VkPipeline>(0x103)}),
               std::invalid_argument);
  EXPECT_THROW(registry.registerHandle(RegisteredPipelineHandle{
                   .key = {RenderTechniqueId::DeferredRaster, "null"},
                   .pipeline = VK_NULL_HANDLE}),
               std::invalid_argument);
}

TEST(PipelineRegistryTests, RegistersTechniqueScopedRuntimeLayouts) {
  PipelineRegistry registry;
  const VkPipelineLayout sceneLayout = fakeHandle<VkPipelineLayout>(0x151);

  registry.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::DeferredRaster, "scene"},
      .layout = sceneLayout});

  const auto *registered = registry.findLayout(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "scene"});
  ASSERT_NE(registered, nullptr);
  EXPECT_EQ(registered->layout, sceneLayout);
  EXPECT_EQ(registry.pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "scene"}),
            sceneLayout);
  EXPECT_EQ(
      registry.pipelineLayoutsForTechnique(RenderTechniqueId::DeferredRaster)
          .size(),
      1u);

  EXPECT_THROW(registry.registerLayout(RegisteredPipelineLayout{
                   .key = {RenderTechniqueId::DeferredRaster, "scene"},
                   .layout = fakeHandle<VkPipelineLayout>(0x152)}),
               std::invalid_argument);
  EXPECT_THROW(registry.registerLayout(RegisteredPipelineLayout{
                   .key = {RenderTechniqueId::DeferredRaster, ""},
                   .layout = fakeHandle<VkPipelineLayout>(0x153)}),
               std::invalid_argument);
  EXPECT_THROW(registry.registerLayout(RegisteredPipelineLayout{
                   .key = {RenderTechniqueId::DeferredRaster, "null"},
                   .layout = VK_NULL_HANDLE}),
               std::invalid_argument);
}

TEST(PipelineRegistryTests,
     BuildsRegistryBackedHandlesFromProductionGraphicsPipelines) {
  GraphicsPipelines pipelines;
  pipelines.gBuffer = fakeHandle<VkPipeline>(0x201);
  pipelines.gBufferFrontCull = fakeHandle<VkPipeline>(0x202);
  pipelines.bimGBuffer = fakeHandle<VkPipeline>(0x203);
  pipelines.directionalLight = fakeHandle<VkPipeline>(0x204);

  pipelines.handleRegistry = buildGraphicsPipelineHandleRegistry(pipelines);

  ASSERT_TRUE(pipelines.handleRegistry);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "gbuffer"}),
            pipelines.gBuffer);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "gbuffer-front-cull"}),
            pipelines.gBufferFrontCull);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "bim-gbuffer"}),
            pipelines.bimGBuffer);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "lighting"}),
            pipelines.directionalLight);
  EXPECT_EQ(pipelines.handleRegistry->handleCount(), 4u);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "missing"}),
            VK_NULL_HANDLE);
}

TEST(PipelineRegistryTests,
     PublishesExtraTechniqueScopedHandlesAfterProductionGraphicsPipelines) {
  GraphicsPipelines pipelines;
  pipelines.gBuffer = fakeHandle<VkPipeline>(0x221);
  const VkPipeline forwardOpaque = fakeHandle<VkPipeline>(0x222);
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = forwardOpaque});

  const auto registry = buildGraphicsPipelineHandleRegistry(pipelines);

  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "gbuffer"}),
            pipelines.gBuffer);
  EXPECT_EQ(registry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "forward-opaque"}),
            forwardOpaque);
  EXPECT_EQ(registry->handleCount(), 2u);
}

TEST(PipelineRegistryTests,
     PublishesForwardRasterSharedPipelineAliasesFromProductionPipelines) {
  GraphicsPipelines pipelines;
  pipelines.depthPrepass = fakeHandle<VkPipeline>(0x22a);
  pipelines.depthPrepassFrontCull = fakeHandle<VkPipeline>(0x22b);
  pipelines.depthPrepassNoCull = fakeHandle<VkPipeline>(0x22c);
  pipelines.bimDepthPrepass = fakeHandle<VkPipeline>(0x22d);
  pipelines.bimDepthPrepassFrontCull = fakeHandle<VkPipeline>(0x22e);
  pipelines.bimDepthPrepassNoCull = fakeHandle<VkPipeline>(0x22f);
  pipelines.transparent = fakeHandle<VkPipeline>(0x231);
  pipelines.transparentFrontCull = fakeHandle<VkPipeline>(0x232);
  pipelines.transparentNoCull = fakeHandle<VkPipeline>(0x233);
  pipelines.postProcess = fakeHandle<VkPipeline>(0x234);
  pipelines.transformGizmo = fakeHandle<VkPipeline>(0x235);
  pipelines.transformGizmoSolid = fakeHandle<VkPipeline>(0x236);
  pipelines.transformGizmoOverlay = fakeHandle<VkPipeline>(0x237);
  pipelines.transformGizmoSolidOverlay = fakeHandle<VkPipeline>(0x238);
  pipelines.lightGizmo = fakeHandle<VkPipeline>(0x23a);
  pipelines.lightGizmoCoverage = fakeHandle<VkPipeline>(0x23b);
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = fakeHandle<VkPipeline>(0x239)});

  pipelines.handleRegistry = buildGraphicsPipelineHandleRegistry(pipelines);
  FrameRecordParams params;
  params.registries.pipelineHandles = pipelines.handleRegistry.get();

  const std::array<std::pair<ForwardRasterPipelineId, VkPipeline>, 17>
      expected = {
          {{ForwardRasterPipelineId::DepthPrepass, pipelines.depthPrepass},
           {ForwardRasterPipelineId::DepthPrepassFrontCull,
            pipelines.depthPrepassFrontCull},
           {ForwardRasterPipelineId::DepthPrepassNoCull,
            pipelines.depthPrepassNoCull},
           {ForwardRasterPipelineId::BimDepthPrepass,
            pipelines.bimDepthPrepass},
           {ForwardRasterPipelineId::BimDepthPrepassFrontCull,
            pipelines.bimDepthPrepassFrontCull},
           {ForwardRasterPipelineId::BimDepthPrepassNoCull,
            pipelines.bimDepthPrepassNoCull},
           {ForwardRasterPipelineId::ForwardOpaque,
            fakeHandle<VkPipeline>(0x239)},
           {ForwardRasterPipelineId::Transparent, pipelines.transparent},
           {ForwardRasterPipelineId::TransparentFrontCull,
            pipelines.transparentFrontCull},
           {ForwardRasterPipelineId::TransparentNoCull,
            pipelines.transparentNoCull},
           {ForwardRasterPipelineId::PostProcess, pipelines.postProcess},
           {ForwardRasterPipelineId::LightGizmo, pipelines.lightGizmo},
           {ForwardRasterPipelineId::LightGizmoCoverage,
            pipelines.lightGizmoCoverage},
           {ForwardRasterPipelineId::TransformGizmo, pipelines.transformGizmo},
           {ForwardRasterPipelineId::TransformGizmoSolid,
            pipelines.transformGizmoSolid},
           {ForwardRasterPipelineId::TransformGizmoOverlay,
            pipelines.transformGizmoOverlay},
           {ForwardRasterPipelineId::TransformGizmoSolidOverlay,
            pipelines.transformGizmoSolidOverlay}}};

  for (const auto &[id, pipeline] : expected) {
    EXPECT_EQ(forwardRasterPipelineHandle(params, id), pipeline);
    EXPECT_TRUE(forwardRasterPipelineReady(params, id));
  }
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "forward-opaque"}),
            fakeHandle<VkPipeline>(0x239));
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "forward-opaque-front-cull"}),
            VK_NULL_HANDLE);
  EXPECT_EQ(pipelines.handleRegistry->pipelineHandle(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "forward-opaque-no-cull"}),
            VK_NULL_HANDLE);
}

TEST(PipelineRegistryTests,
     BuildsRegistryBackedLayoutsFromProductionPipelineLayouts) {
  PipelineLayouts layouts;
  layouts.scene = fakeHandle<VkPipelineLayout>(0x251);
  layouts.transparent = fakeHandle<VkPipelineLayout>(0x252);
  layouts.lighting = fakeHandle<VkPipelineLayout>(0x253);
  layouts.postProcess = fakeHandle<VkPipelineLayout>(0x254);
  layouts.transformGizmo = fakeHandle<VkPipelineLayout>(0x255);

  const auto registry = buildGraphicsPipelineLayoutRegistry(layouts);

  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "scene"}),
            layouts.scene);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "scene"}),
            layouts.scene);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "transparent"}),
            layouts.transparent);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "lighting"}),
            layouts.lighting);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "post-process"}),
            layouts.postProcess);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "transform-gizmo"}),
            layouts.transformGizmo);
  EXPECT_EQ(registry->layoutCount(), 9u);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "missing"}),
            VK_NULL_HANDLE);
}

TEST(PipelineRegistryTests,
     BuildsSharedLayoutsForDeferredAndForwardRasterTechniques) {
  PipelineLayouts layouts;
  layouts.scene = fakeHandle<VkPipelineLayout>(0x255);
  layouts.transparent = fakeHandle<VkPipelineLayout>(0x256);
  layouts.postProcess = fakeHandle<VkPipelineLayout>(0x257);
  layouts.transformGizmo = fakeHandle<VkPipelineLayout>(0x258);
  layouts.lightGizmo = fakeHandle<VkPipelineLayout>(0x259);

  const auto registry = buildGraphicsPipelineLayoutRegistry(layouts);
  FrameRecordParams params;
  params.registries.pipelineLayouts = registry.get();

  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "transparent"}),
            layouts.transparent);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "transparent"}),
            layouts.transparent);
  const std::array<std::pair<ForwardRasterPipelineLayoutId, VkPipelineLayout>,
                   5>
      expected = {
          {{ForwardRasterPipelineLayoutId::Scene, layouts.scene},
           {ForwardRasterPipelineLayoutId::Transparent, layouts.transparent},
           {ForwardRasterPipelineLayoutId::PostProcess, layouts.postProcess},
           {ForwardRasterPipelineLayoutId::LightGizmo, layouts.lightGizmo},
           {ForwardRasterPipelineLayoutId::TransformGizmo,
            layouts.transformGizmo}}};
  for (const auto &[id, layout] : expected) {
    EXPECT_EQ(forwardRasterPipelineLayout(params, id), layout);
    EXPECT_TRUE(forwardRasterPipelineLayoutReady(params, id));
  }
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "scene"}),
            layouts.scene);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "light-gizmo"}),
            layouts.lightGizmo);
}

TEST(PipelineRegistryTests,
     PublishesExtraTechniqueScopedLayoutsAfterProductionPipelineLayouts) {
  PipelineLayouts layouts;
  layouts.transparent = fakeHandle<VkPipelineLayout>(0x261);
  const VkPipelineLayout forwardLayout = fakeHandle<VkPipelineLayout>(0x262);
  layouts.extraLayouts.push_back(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .layout = forwardLayout});

  const auto registry = buildGraphicsPipelineLayoutRegistry(layouts);

  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::DeferredRaster, "transparent"}),
            layouts.transparent);
  EXPECT_EQ(registry->pipelineLayout(TechniquePipelineKey{
                RenderTechniqueId::ForwardRaster, "forward-opaque"}),
            forwardLayout);
  EXPECT_EQ(registry->layoutCount(), 3u);
}

TEST(FrameRecordParamsRegistryTests,
     ResolvesRuntimeResourceBindingsAndPipelineHandles) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelineHandles;
  PipelineRegistry pipelineLayouts;
  FrameRecordParams params;
  params.runtime.imageIndex = 2u;
  params.registries.resourceBindings = &resources;
  params.registries.pipelineHandles = &pipelineHandles;
  params.registries.pipelineLayouts = &pipelineLayouts;

  resources.bindImage(
      RenderTechniqueId::DeferredRaster, "scene-color", 1u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x301),
                        .view = fakeHandle<VkImageView>(0x302),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .extent = {640, 480, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
  const auto selectedFrameSceneColor = resources.bindImage(
      RenderTechniqueId::DeferredRaster, "scene-color", 2u,
      FrameImageBinding{.image = fakeHandle<VkImage>(0x303),
                        .view = fakeHandle<VkImageView>(0x304),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .extent = {1920, 1080, 1},
                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
  const auto selectedFrameDescriptor = resources.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "frame-lighting-descriptor-set", 2u,
      FrameDescriptorBinding{.descriptorSet =
                                 fakeHandle<VkDescriptorSet>(0x305)});
  const VkPipeline gbuffer = fakeHandle<VkPipeline>(0x401);
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::DeferredRaster, "gbuffer"},
      .pipeline = gbuffer});
  const VkPipelineLayout sceneLayout = fakeHandle<VkPipelineLayout>(0x402);
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::DeferredRaster, "scene"},
      .layout = sceneLayout});

  const auto *binding =
      params.resourceBinding(RenderTechniqueId::DeferredRaster, "scene-color");
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->handle, selectedFrameSceneColor);
  EXPECT_EQ(binding->frameIndex, 2u);
  EXPECT_EQ(binding->image.view, fakeHandle<VkImageView>(0x304));
  EXPECT_EQ(params.pipelineHandle(RenderTechniqueId::DeferredRaster, "gbuffer"),
            gbuffer);
  EXPECT_EQ(params.pipelineLayout(RenderTechniqueId::DeferredRaster, "scene"),
            sceneLayout);
  EXPECT_EQ(
      params.imageBinding(RenderTechniqueId::DeferredRaster, "scene-color"),
      binding ? &binding->image : nullptr);
  EXPECT_EQ(
      params.bufferBinding(RenderTechniqueId::DeferredRaster, "scene-color"),
      nullptr);
  EXPECT_EQ(params.descriptorBinding(RenderTechniqueId::DeferredRaster,
                                     "scene-color"),
            nullptr);
  const auto *descriptorBinding = params.descriptorBinding(
      RenderTechniqueId::DeferredRaster, "frame-lighting-descriptor-set");
  ASSERT_NE(descriptorBinding, nullptr);
  EXPECT_EQ(params
                .resourceBinding(RenderTechniqueId::DeferredRaster,
                                 "frame-lighting-descriptor-set")
                ->handle,
            selectedFrameDescriptor);
  EXPECT_EQ(descriptorBinding->descriptorSet,
            fakeHandle<VkDescriptorSet>(0x305));
  EXPECT_EQ(params.descriptorSet(RenderTechniqueId::DeferredRaster,
                                 "frame-lighting-descriptor-set"),
            fakeHandle<VkDescriptorSet>(0x305));
  EXPECT_EQ(params.imageBinding(RenderTechniqueId::DeferredRaster,
                                "frame-lighting-descriptor-set"),
            nullptr);
  EXPECT_EQ(
      params.resourceBinding(RenderTechniqueId::DeferredRaster, "missing"),
      nullptr);
  EXPECT_EQ(params.pipelineHandle(RenderTechniqueId::DeferredRaster, "missing"),
            VK_NULL_HANDLE);
  EXPECT_EQ(params.pipelineLayout(RenderTechniqueId::DeferredRaster, "missing"),
            VK_NULL_HANDLE);
  EXPECT_EQ(params.descriptorSet(RenderTechniqueId::DeferredRaster, "missing"),
            VK_NULL_HANDLE);
  resources.bindSampler(
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler", 2u,
      FrameSamplerBinding{.sampler = fakeHandle<VkSampler>(0x306)});
  const auto *samplerBinding = params.samplerBinding(
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler");
  ASSERT_NE(samplerBinding, nullptr);
  EXPECT_EQ(samplerBinding->sampler, fakeHandle<VkSampler>(0x306));
  EXPECT_EQ(
      params.sampler(RenderTechniqueId::DeferredRaster, "g-buffer-sampler"),
      fakeHandle<VkSampler>(0x306));
}

TEST(FrameRecordParamsRegistryTests, ResolvesRuntimeFramebufferBindings) {
  FrameResourceRegistry resources;
  FrameRecordParams params;
  params.runtime.imageIndex = 3u;
  params.registries.resourceBindings = &resources;

  resources.bindFramebuffer(
      RenderTechniqueId::DeferredRaster, "lighting-framebuffer", 3u,
      FrameFramebufferBinding{.framebuffer = fakeHandle<VkFramebuffer>(0x501),
                              .renderPass = fakeHandle<VkRenderPass>(0x502),
                              .extent = {1280, 720},
                              .attachmentCount = 2u});

  const auto *binding = params.framebufferBinding(
      RenderTechniqueId::DeferredRaster, "lighting-framebuffer");
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->framebuffer, fakeHandle<VkFramebuffer>(0x501));
  EXPECT_EQ(binding->renderPass, fakeHandle<VkRenderPass>(0x502));
  EXPECT_EQ(binding->attachmentCount, 2u);
  EXPECT_EQ(params.framebuffer(RenderTechniqueId::DeferredRaster,
                               "lighting-framebuffer"),
            fakeHandle<VkFramebuffer>(0x501));
  EXPECT_EQ(params.imageBinding(RenderTechniqueId::DeferredRaster,
                                "lighting-framebuffer"),
            nullptr);
  EXPECT_EQ(params.framebuffer(RenderTechniqueId::DeferredRaster, "missing"),
            VK_NULL_HANDLE);
}

TEST(DeferredRasterResourceBridgeTests,
     ResolvesFramebufferHandlesFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 5u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(
      deferredRasterFramebuffer(params, DeferredRasterFramebufferId::GBuffer),
      VK_NULL_HANDLE);

  resources.bindFramebuffer(
      RenderTechniqueId::DeferredRaster, "gbuffer-framebuffer", 5u,
      FrameFramebufferBinding{.framebuffer = fakeHandle<VkFramebuffer>(0x602),
                              .renderPass = fakeHandle<VkRenderPass>(0x603),
                              .extent = {1920, 1080},
                              .attachmentCount = 7u});

  const FrameFramebufferBinding *binding = deferredRasterFramebufferBinding(
      params, DeferredRasterFramebufferId::GBuffer);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->framebuffer, fakeHandle<VkFramebuffer>(0x602));
  EXPECT_EQ(
      deferredRasterFramebuffer(params, DeferredRasterFramebufferId::GBuffer),
      fakeHandle<VkFramebuffer>(0x602));
  EXPECT_EQ(
      deferredRasterRenderPass(params, DeferredRasterFramebufferId::GBuffer),
      fakeHandle<VkRenderPass>(0x603));
}

TEST(DeferredRasterResourceBridgeTests, ResolvesImageBindingsFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 4u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(deferredRasterImage(params, DeferredRasterImageId::DepthStencil),
            VK_NULL_HANDLE);
  EXPECT_EQ(
      deferredRasterImageView(params, DeferredRasterImageId::DepthSamplingView),
      VK_NULL_HANDLE);

  resources.bindImage(RenderTechniqueId::DeferredRaster, "depth-sampling-view",
                      4u,
                      FrameImageBinding{.image = fakeHandle<VkImage>(0x704),
                                        .view = fakeHandle<VkImageView>(0x705),
                                        .format = VK_FORMAT_D32_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT});

  const FrameImageBinding *binding = deferredRasterImageBinding(
      params, DeferredRasterImageId::DepthSamplingView);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->view, fakeHandle<VkImageView>(0x705));
  EXPECT_EQ(
      deferredRasterImageView(params, DeferredRasterImageId::DepthSamplingView),
      fakeHandle<VkImageView>(0x705));
  EXPECT_EQ(
      deferredRasterImage(params, DeferredRasterImageId::DepthSamplingView),
      fakeHandle<VkImage>(0x704));
}

TEST(DeferredRasterResourceBridgeTests,
     ResolvesBufferBindingsFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 2u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(deferredRasterBuffer(params, DeferredRasterBufferId::OitNode),
            VK_NULL_HANDLE);
  EXPECT_EQ(deferredRasterBufferSize(params, DeferredRasterBufferId::OitNode),
            0u);

  resources.bindBuffer(
      RenderTechniqueId::DeferredRaster, "oit-node-buffer", 2u,
      FrameBufferBinding{.buffer = fakeHandle<VkBuffer>(0x901),
                         .size = 8192u,
                         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});

  const FrameBufferBinding *binding =
      deferredRasterBufferBinding(params, DeferredRasterBufferId::OitNode);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->buffer, fakeHandle<VkBuffer>(0x901));
  EXPECT_EQ(binding->size, 8192u);
  EXPECT_EQ(deferredRasterBuffer(params, DeferredRasterBufferId::OitNode),
            fakeHandle<VkBuffer>(0x901));
  EXPECT_EQ(deferredRasterBufferSize(params, DeferredRasterBufferId::OitNode),
            8192u);

  EXPECT_EQ(deferredRasterBuffer(params, DeferredRasterBufferId::Camera),
            VK_NULL_HANDLE);
  EXPECT_EQ(deferredRasterBufferSize(params, DeferredRasterBufferId::Camera),
            0u);

  resources.bindBuffer(
      RenderTechniqueId::DeferredRaster, "camera-buffer", 2u,
      FrameBufferBinding{.buffer = fakeHandle<VkBuffer>(0x902),
                         .size = sizeof(container::gpu::CameraData),
                         .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT});

  const FrameBufferBinding *cameraBinding =
      deferredRasterBufferBinding(params, DeferredRasterBufferId::Camera);
  ASSERT_NE(cameraBinding, nullptr);
  EXPECT_EQ(cameraBinding->buffer, fakeHandle<VkBuffer>(0x902));
  EXPECT_EQ(cameraBinding->size, sizeof(container::gpu::CameraData));
  EXPECT_EQ(deferredRasterBuffer(params, DeferredRasterBufferId::Camera),
            fakeHandle<VkBuffer>(0x902));
  EXPECT_EQ(deferredRasterBufferSize(params, DeferredRasterBufferId::Camera),
            sizeof(container::gpu::CameraData));
}

TEST(DeferredRasterResourceBridgeTests,
     ResolvesDescriptorBindingsFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 6u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(deferredRasterDescriptorSet(
                params, DeferredRasterDescriptorSetId::FrameLighting),
            VK_NULL_HANDLE);

  resources.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "frame-lighting-descriptor-set", 6u,
      FrameDescriptorBinding{.descriptorSet =
                                 fakeHandle<VkDescriptorSet>(0x802)});

  const FrameDescriptorBinding *binding = deferredRasterDescriptorBinding(
      params, DeferredRasterDescriptorSetId::FrameLighting);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->descriptorSet, fakeHandle<VkDescriptorSet>(0x802));
  EXPECT_EQ(deferredRasterDescriptorSet(
                params, DeferredRasterDescriptorSetId::FrameLighting),
            fakeHandle<VkDescriptorSet>(0x802));

  const std::array<std::pair<DeferredRasterDescriptorSetId, const char *>, 4>
      importedDescriptors = {
          {{DeferredRasterDescriptorSetId::Scene, "scene-descriptor-set"},
           {DeferredRasterDescriptorSetId::BimScene,
            "bim-scene-descriptor-set"},
           {DeferredRasterDescriptorSetId::Light, "light-descriptor-set"},
           {DeferredRasterDescriptorSetId::TiledLighting,
            "tiled-lighting-descriptor-set"}}};
  uintptr_t nextHandle = 0x810;
  for (const auto &[id, key] : importedDescriptors) {
    EXPECT_EQ(deferredRasterDescriptorSet(params, id), VK_NULL_HANDLE) << key;
    resources.bindDescriptorSet(
        RenderTechniqueId::DeferredRaster, key, 6u,
        FrameDescriptorBinding{.descriptorSet =
                                   fakeHandle<VkDescriptorSet>(nextHandle++)});
    EXPECT_NE(deferredRasterDescriptorSet(params, id), VK_NULL_HANDLE) << key;
  }
}

TEST(DeferredRasterResourceBridgeTests, ResolvesSamplersFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 7u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(deferredRasterSampler(params, DeferredRasterSamplerId::GBuffer),
            VK_NULL_HANDLE);

  resources.bindSampler(
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler", 7u,
      FrameSamplerBinding{.sampler = fakeHandle<VkSampler>(0x850)});

  const FrameSamplerBinding *binding =
      deferredRasterSamplerBinding(params, DeferredRasterSamplerId::GBuffer);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->sampler, fakeHandle<VkSampler>(0x850));
  EXPECT_EQ(deferredRasterSampler(params, DeferredRasterSamplerId::GBuffer),
            fakeHandle<VkSampler>(0x850));
}

TEST(ForwardRasterResourceBridgeTests, ResolvesBindingsFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 8u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(forwardRasterImage(params, ForwardRasterImageId::SceneColor),
            VK_NULL_HANDLE);
  EXPECT_EQ(forwardRasterImageView(params, ForwardRasterImageId::SceneColor),
            VK_NULL_HANDLE);
  EXPECT_FALSE(
      forwardRasterImageReady(params, ForwardRasterImageId::SceneColor));
  EXPECT_FALSE(
      forwardRasterImageViewReady(params, ForwardRasterImageId::SceneColor));
  EXPECT_EQ(forwardRasterBuffer(params, ForwardRasterBufferId::Camera),
            VK_NULL_HANDLE);
  EXPECT_EQ(forwardRasterBufferSize(params, ForwardRasterBufferId::Camera), 0u);
  EXPECT_FALSE(forwardRasterBufferReady(params, ForwardRasterBufferId::Camera));
  EXPECT_EQ(
      forwardRasterDescriptorSet(params, ForwardRasterDescriptorSetId::Scene),
      VK_NULL_HANDLE);
  EXPECT_FALSE(forwardRasterDescriptorSetReady(
      params, ForwardRasterDescriptorSetId::Scene));
  EXPECT_EQ(
      forwardRasterFramebuffer(params, ForwardRasterFramebufferId::Lighting),
      VK_NULL_HANDLE);
  EXPECT_EQ(
      forwardRasterRenderPass(params, ForwardRasterFramebufferId::Lighting),
      VK_NULL_HANDLE);
  EXPECT_FALSE(forwardRasterFramebufferReady(
      params, ForwardRasterFramebufferId::Lighting));
  EXPECT_FALSE(forwardRasterRenderPassReady(
      params, ForwardRasterFramebufferId::Lighting));

  const std::array<std::pair<ForwardRasterImageId, const char *>, 6> images = {
      {{ForwardRasterImageId::DepthStencil, "depth-stencil"},
       {ForwardRasterImageId::DepthSamplingView, "depth-sampling-view"},
       {ForwardRasterImageId::SceneColor, "scene-color"},
       {ForwardRasterImageId::PickDepth, "pick-depth"},
       {ForwardRasterImageId::PickId, "pick-id"},
       {ForwardRasterImageId::OitHeadPointers, "oit-head-pointers"}}};
  uintptr_t nextImage = 0xa00;
  for (const auto &[id, key] : images) {
    resources.bindImage(
        RenderTechniqueId::ForwardRaster, key, 8u,
        FrameImageBinding{.image = fakeHandle<VkImage>(nextImage++),
                          .view = fakeHandle<VkImageView>(nextImage++),
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .extent = {1920, 1080, 1},
                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT});
    const FrameImageBinding *binding = forwardRasterImageBinding(params, id);
    ASSERT_NE(binding, nullptr) << key;
    EXPECT_EQ(forwardRasterImage(params, id), binding->image) << key;
    EXPECT_EQ(forwardRasterImageView(params, id), binding->view) << key;
    EXPECT_TRUE(forwardRasterImageReady(params, id)) << key;
    EXPECT_TRUE(forwardRasterImageViewReady(params, id)) << key;
  }

  const std::array<std::pair<ForwardRasterBufferId, const char *>, 5> buffers =
      {{{ForwardRasterBufferId::Camera, "camera-buffer"},
        {ForwardRasterBufferId::SceneObject, "scene-object-buffer"},
        {ForwardRasterBufferId::OitNode, "oit-node-buffer"},
        {ForwardRasterBufferId::OitCounter, "oit-counter-buffer"},
        {ForwardRasterBufferId::OitMetadata, "oit-metadata-buffer"}}};
  uintptr_t nextBuffer = 0xb00;
  VkDeviceSize nextSize = 128u;
  for (const auto &[id, key] : buffers) {
    resources.bindBuffer(
        RenderTechniqueId::ForwardRaster, key, 8u,
        FrameBufferBinding{.buffer = fakeHandle<VkBuffer>(nextBuffer++),
                           .size = nextSize,
                           .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
    const FrameBufferBinding *binding = forwardRasterBufferBinding(params, id);
    ASSERT_NE(binding, nullptr) << key;
    EXPECT_EQ(forwardRasterBuffer(params, id), binding->buffer) << key;
    EXPECT_EQ(forwardRasterBufferSize(params, id), binding->size) << key;
    EXPECT_TRUE(forwardRasterBufferReady(params, id)) << key;
    nextSize += 64u;
  }

  const std::array<std::pair<ForwardRasterDescriptorSetId, const char *>, 8>
      descriptors = {
          {{ForwardRasterDescriptorSetId::Scene, "scene-descriptor-set"},
           {ForwardRasterDescriptorSetId::BimScene, "bim-scene-descriptor-set"},
           {ForwardRasterDescriptorSetId::Light, "light-descriptor-set"},
           {ForwardRasterDescriptorSetId::Shadow, "shadow-descriptor-set"},
           {ForwardRasterDescriptorSetId::LocalShadow,
            "local-shadow-descriptor-set"},
           {ForwardRasterDescriptorSetId::FrameLighting,
            "frame-lighting-descriptor-set"},
           {ForwardRasterDescriptorSetId::PostProcess,
            "post-process-descriptor-set"},
           {ForwardRasterDescriptorSetId::Oit, "oit-descriptor-set"}}};
  uintptr_t nextDescriptor = 0xc00;
  for (const auto &[id, key] : descriptors) {
    resources.bindDescriptorSet(
        RenderTechniqueId::ForwardRaster, key, 8u,
        FrameDescriptorBinding{
            .descriptorSet = fakeHandle<VkDescriptorSet>(nextDescriptor++)});
    const FrameDescriptorBinding *binding =
        forwardRasterDescriptorBinding(params, id);
    ASSERT_NE(binding, nullptr) << key;
    EXPECT_EQ(forwardRasterDescriptorSet(params, id), binding->descriptorSet)
        << key;
    EXPECT_TRUE(forwardRasterDescriptorSetReady(params, id)) << key;
  }

  const std::array<std::pair<ForwardRasterFramebufferId, const char *>, 5>
      framebuffers = {
          {{ForwardRasterFramebufferId::DepthPrepass,
            "depth-prepass-framebuffer"},
           {ForwardRasterFramebufferId::BimDepthPrepass,
            "bim-depth-prepass-framebuffer"},
           {ForwardRasterFramebufferId::TransparentPick,
            "transparent-pick-framebuffer"},
           {ForwardRasterFramebufferId::Lighting, "lighting-framebuffer"},
           {ForwardRasterFramebufferId::TransformGizmo,
            "transform-gizmo-framebuffer"}}};
  uintptr_t nextFramebuffer = 0xd00;
  for (const auto &[id, key] : framebuffers) {
    resources.bindFramebuffer(
        RenderTechniqueId::ForwardRaster, key, 8u,
        FrameFramebufferBinding{
            .framebuffer = fakeHandle<VkFramebuffer>(nextFramebuffer++),
            .renderPass = fakeHandle<VkRenderPass>(nextFramebuffer++),
            .extent = {1920, 1080},
            .attachmentCount = 2u});
    const FrameFramebufferBinding *binding =
        forwardRasterFramebufferBinding(params, id);
    ASSERT_NE(binding, nullptr) << key;
    EXPECT_EQ(forwardRasterFramebuffer(params, id), binding->framebuffer)
        << key;
    EXPECT_EQ(forwardRasterRenderPass(params, id), binding->renderPass) << key;
    EXPECT_TRUE(forwardRasterFramebufferReady(params, id)) << key;
    EXPECT_TRUE(forwardRasterRenderPassReady(params, id)) << key;
  }
}

TEST(ShadowResourceBridgeTests, ResolvesDescriptorBindingsFromRegistryOnly) {
  FrameResourceRegistry resources;

  FrameRecordParams params;
  params.runtime.imageIndex = 3u;
  params.registries.resourceBindings = &resources;

  EXPECT_EQ(shadowDescriptorSet(params, ShadowDescriptorSetId::Scene),
            VK_NULL_HANDLE);

  const std::array<std::pair<ShadowDescriptorSetId, const char *>, 4>
      descriptors = {
          {{ShadowDescriptorSetId::Scene, "scene-descriptor-set"},
           {ShadowDescriptorSetId::BimScene, "bim-scene-descriptor-set"},
           {ShadowDescriptorSetId::Shadow, "shadow-descriptor-set"},
           {ShadowDescriptorSetId::LocalShadow,
            "local-shadow-descriptor-set"}}};
  uintptr_t nextHandle = 0x870;
  for (const auto &[id, key] : descriptors) {
    resources.bindDescriptorSet(
        RenderTechniqueId::DeferredRaster, key, 3u,
        FrameDescriptorBinding{.descriptorSet =
                                   fakeHandle<VkDescriptorSet>(nextHandle++)});
    const FrameDescriptorBinding *binding = shadowDescriptorBinding(params, id);
    ASSERT_NE(binding, nullptr) << key;
    EXPECT_NE(binding->descriptorSet, VK_NULL_HANDLE) << key;
    EXPECT_EQ(shadowDescriptorSet(params, id), binding->descriptorSet) << key;
  }
}

TEST(ShadowPipelineBridgeTests, ResolvesProductionHandleRegistryOnly) {
  FrameRecordParams params;

  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::Depth),
            VK_NULL_HANDLE);
  EXPECT_FALSE(shadowPipelineReady(params, ShadowPipelineId::Depth));

  GraphicsPipelines registryPipelines;
  registryPipelines.shadowDepth = fakeHandle<VkPipeline>(0x911);
  registryPipelines.shadowDepthFrontCull = fakeHandle<VkPipeline>(0x912);
  registryPipelines.shadowDepthNoCull = fakeHandle<VkPipeline>(0x913);
  registryPipelines.localShadowDepth = fakeHandle<VkPipeline>(0x914);
  registryPipelines.localShadowDepthFrontCull = fakeHandle<VkPipeline>(0x915);
  registryPipelines.localShadowDepthNoCull = fakeHandle<VkPipeline>(0x916);
  registryPipelines.handleRegistry =
      buildGraphicsPipelineHandleRegistry(registryPipelines);
  params.registries.pipelineHandles = registryPipelines.handleRegistry.get();

  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::Depth),
            registryPipelines.shadowDepth);
  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::DepthFrontCull),
            registryPipelines.shadowDepthFrontCull);
  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::DepthNoCull),
            registryPipelines.shadowDepthNoCull);
  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::LocalDepth),
            registryPipelines.localShadowDepth);
  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::LocalDepthFrontCull),
            registryPipelines.localShadowDepthFrontCull);
  EXPECT_EQ(shadowPipelineHandle(params, ShadowPipelineId::LocalDepthNoCull),
            registryPipelines.localShadowDepthNoCull);
  EXPECT_TRUE(shadowPipelineReady(params, ShadowPipelineId::Depth));
  EXPECT_TRUE(shadowPipelineReady(params, ShadowPipelineId::LocalDepth));
}

TEST(ShadowPipelineBridgeTests, ResolvesProductionLayoutRegistryOnly) {
  FrameRecordParams params;

  EXPECT_EQ(shadowPipelineLayout(params, ShadowPipelineLayoutId::Shadow),
            VK_NULL_HANDLE);
  EXPECT_FALSE(
      shadowPipelineLayoutReady(params, ShadowPipelineLayoutId::Shadow));

  container::renderer::PipelineLayouts registryLayouts;
  registryLayouts.shadow = fakeHandle<VkPipelineLayout>(0x922);
  registryLayouts.layoutRegistry =
      buildGraphicsPipelineLayoutRegistry(registryLayouts);
  params.registries.pipelineLayouts = registryLayouts.layoutRegistry.get();

  EXPECT_EQ(shadowPipelineLayout(params, ShadowPipelineLayoutId::Shadow),
            registryLayouts.shadow);
  EXPECT_TRUE(
      shadowPipelineLayoutReady(params, ShadowPipelineLayoutId::Shadow));
}

TEST(DeferredRasterPipelineBridgeTests, ResolvesProductionHandleRegistryOnly) {
  FrameRecordParams params;

  EXPECT_EQ(deferredRasterPipelineHandle(params,
                                         DeferredRasterPipelineId::Transparent),
            VK_NULL_HANDLE);
  EXPECT_FALSE(deferredRasterPipelineReady(
      params, DeferredRasterPipelineId::TransparentPickFrontCull));

  GraphicsPipelines registryPipelines;
  registryPipelines.transparent = fakeHandle<VkPipeline>(0x931);
  registryPipelines.postProcess = fakeHandle<VkPipeline>(0x932);
  registryPipelines.surfaceNormalLine = fakeHandle<VkPipeline>(0x933);
  registryPipelines.transparentPickFrontCull = fakeHandle<VkPipeline>(0x934);
  registryPipelines.handleRegistry =
      buildGraphicsPipelineHandleRegistry(registryPipelines);
  params.registries.pipelineHandles = registryPipelines.handleRegistry.get();

  EXPECT_EQ(deferredRasterPipelineHandle(params,
                                         DeferredRasterPipelineId::Transparent),
            registryPipelines.transparent);
  EXPECT_EQ(deferredRasterPipelineHandle(params,
                                         DeferredRasterPipelineId::PostProcess),
            registryPipelines.postProcess);
  EXPECT_EQ(deferredRasterPipelineHandle(
                params, DeferredRasterPipelineId::SurfaceNormalLine),
            registryPipelines.surfaceNormalLine);
  EXPECT_EQ(deferredRasterPipelineHandle(
                params, DeferredRasterPipelineId::TransparentPickFrontCull),
            registryPipelines.transparentPickFrontCull);
  EXPECT_TRUE(deferredRasterPipelineReady(
      params, DeferredRasterPipelineId::TransparentPickFrontCull));
}

TEST(DeferredRasterPipelineBridgeTests, ResolvesProductionLayoutRegistryOnly) {
  FrameRecordParams params;

  EXPECT_EQ(deferredRasterPipelineLayout(
                params, DeferredRasterPipelineLayoutId::Transparent),
            VK_NULL_HANDLE);
  EXPECT_FALSE(deferredRasterPipelineLayoutReady(
      params, DeferredRasterPipelineLayoutId::NormalValidation));

  container::renderer::PipelineLayouts registryLayouts;
  registryLayouts.transparent = fakeHandle<VkPipelineLayout>(0x941);
  registryLayouts.tiledLighting = fakeHandle<VkPipelineLayout>(0x942);
  registryLayouts.normalValidation = fakeHandle<VkPipelineLayout>(0x943);
  registryLayouts.surfaceNormal = fakeHandle<VkPipelineLayout>(0x944);
  registryLayouts.layoutRegistry =
      buildGraphicsPipelineLayoutRegistry(registryLayouts);
  params.registries.pipelineLayouts = registryLayouts.layoutRegistry.get();

  EXPECT_EQ(deferredRasterPipelineLayout(
                params, DeferredRasterPipelineLayoutId::Transparent),
            registryLayouts.transparent);
  EXPECT_EQ(deferredRasterPipelineLayout(
                params, DeferredRasterPipelineLayoutId::TiledLighting),
            registryLayouts.tiledLighting);
  EXPECT_EQ(deferredRasterPipelineLayout(
                params, DeferredRasterPipelineLayoutId::NormalValidation),
            registryLayouts.normalValidation);
  EXPECT_EQ(deferredRasterPipelineLayout(
                params, DeferredRasterPipelineLayoutId::SurfaceNormal),
            registryLayouts.surfaceNormal);
  EXPECT_TRUE(deferredRasterPipelineLayoutReady(
      params, DeferredRasterPipelineLayoutId::NormalValidation));
}

TEST(ForwardRasterPipelineBridgeTests, ResolvesRegistriesOnly) {
  FrameRecordParams params;

  EXPECT_EQ(forwardRasterPipelineHandle(params,
                                        ForwardRasterPipelineId::ForwardOpaque),
            VK_NULL_HANDLE);
  EXPECT_FALSE(forwardRasterPipelineReady(
      params, ForwardRasterPipelineId::ForwardOpaque));
  EXPECT_EQ(forwardRasterPipelineLayout(
                params, ForwardRasterPipelineLayoutId::Transparent),
            VK_NULL_HANDLE);
  EXPECT_FALSE(forwardRasterPipelineLayoutReady(
      params, ForwardRasterPipelineLayoutId::Transparent));

  PipelineRegistry pipelineHandles;
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "depth-prepass"},
      .pipeline = fakeHandle<VkPipeline>(0xe06)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "bim-depth-prepass"},
      .pipeline = fakeHandle<VkPipeline>(0xe07)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = fakeHandle<VkPipeline>(0xe01)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-transparent"},
      .pipeline = fakeHandle<VkPipeline>(0xe02)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "post-process"},
      .pipeline = fakeHandle<VkPipeline>(0xe03)});
  pipelineHandles.registerHandle(
      RegisteredPipelineHandle{.key = {RenderTechniqueId::ForwardRaster,
                                       "forward-transparent-front-cull"},
                               .pipeline = fakeHandle<VkPipeline>(0xe04)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-transparent-no-cull"},
      .pipeline = fakeHandle<VkPipeline>(0xe05)});
  pipelineHandles.registerHandle(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "light-gizmo"},
      .pipeline = fakeHandle<VkPipeline>(0xe08)});
  params.registries.pipelineHandles = &pipelineHandles;

  EXPECT_EQ(forwardRasterPipelineHandle(params,
                                        ForwardRasterPipelineId::DepthPrepass),
            fakeHandle<VkPipeline>(0xe06));
  EXPECT_EQ(forwardRasterPipelineHandle(
                params, ForwardRasterPipelineId::BimDepthPrepass),
            fakeHandle<VkPipeline>(0xe07));
  EXPECT_EQ(forwardRasterPipelineHandle(params,
                                        ForwardRasterPipelineId::ForwardOpaque),
            fakeHandle<VkPipeline>(0xe01));
  EXPECT_EQ(
      forwardRasterPipelineHandle(params, ForwardRasterPipelineId::Transparent),
      fakeHandle<VkPipeline>(0xe02));
  EXPECT_EQ(
      forwardRasterPipelineHandle(params, ForwardRasterPipelineId::PostProcess),
      fakeHandle<VkPipeline>(0xe03));
  EXPECT_EQ(forwardRasterPipelineHandle(
                params, ForwardRasterPipelineId::TransparentFrontCull),
            fakeHandle<VkPipeline>(0xe04));
  EXPECT_EQ(forwardRasterPipelineHandle(
                params, ForwardRasterPipelineId::TransparentNoCull),
            fakeHandle<VkPipeline>(0xe05));
  EXPECT_EQ(
      forwardRasterPipelineHandle(params, ForwardRasterPipelineId::LightGizmo),
      fakeHandle<VkPipeline>(0xe08));
  EXPECT_TRUE(forwardRasterPipelineReady(
      params, ForwardRasterPipelineId::ForwardOpaque));
  EXPECT_TRUE(
      forwardRasterPipelineReady(params, ForwardRasterPipelineId::Transparent));
  EXPECT_TRUE(
      forwardRasterPipelineReady(params, ForwardRasterPipelineId::PostProcess));

  PipelineRegistry pipelineLayouts;
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "scene"},
      .layout = fakeHandle<VkPipelineLayout>(0xf04)});
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "transparent"},
      .layout = fakeHandle<VkPipelineLayout>(0xf01)});
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "post-process"},
      .layout = fakeHandle<VkPipelineLayout>(0xf02)});
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "transform-gizmo"},
      .layout = fakeHandle<VkPipelineLayout>(0xf03)});
  pipelineLayouts.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "light-gizmo"},
      .layout = fakeHandle<VkPipelineLayout>(0xf05)});
  params.registries.pipelineLayouts = &pipelineLayouts;

  EXPECT_EQ(
      forwardRasterPipelineLayout(params, ForwardRasterPipelineLayoutId::Scene),
      fakeHandle<VkPipelineLayout>(0xf04));
  EXPECT_EQ(forwardRasterPipelineLayout(
                params, ForwardRasterPipelineLayoutId::Transparent),
            fakeHandle<VkPipelineLayout>(0xf01));
  EXPECT_EQ(forwardRasterPipelineLayout(
                params, ForwardRasterPipelineLayoutId::PostProcess),
            fakeHandle<VkPipelineLayout>(0xf02));
  EXPECT_EQ(forwardRasterPipelineLayout(
                params, ForwardRasterPipelineLayoutId::TransformGizmo),
            fakeHandle<VkPipelineLayout>(0xf03));
  EXPECT_EQ(forwardRasterPipelineLayout(
                params, ForwardRasterPipelineLayoutId::LightGizmo),
            fakeHandle<VkPipelineLayout>(0xf05));
  EXPECT_TRUE(forwardRasterPipelineLayoutReady(
      params, ForwardRasterPipelineLayoutId::Transparent));
  EXPECT_TRUE(forwardRasterPipelineLayoutReady(
      params, ForwardRasterPipelineLayoutId::PostProcess));
  EXPECT_TRUE(forwardRasterPipelineLayoutReady(
      params, ForwardRasterPipelineLayoutId::TransformGizmo));
}

TEST(ForwardRasterPipelineBridgeTests, CoversEveryExposedPipelineAndLayoutId) {
  PipelineRegistry pipelineHandles;
  const std::array<std::pair<ForwardRasterPipelineId, const char *>, 17>
      pipelineNames = {
          {{ForwardRasterPipelineId::DepthPrepass, "depth-prepass"},
           {ForwardRasterPipelineId::DepthPrepassFrontCull,
            "depth-prepass-front-cull"},
           {ForwardRasterPipelineId::DepthPrepassNoCull,
            "depth-prepass-no-cull"},
           {ForwardRasterPipelineId::BimDepthPrepass, "bim-depth-prepass"},
           {ForwardRasterPipelineId::BimDepthPrepassFrontCull,
            "bim-depth-prepass-front-cull"},
           {ForwardRasterPipelineId::BimDepthPrepassNoCull,
            "bim-depth-prepass-no-cull"},
           {ForwardRasterPipelineId::ForwardOpaque, "forward-opaque"},
           {ForwardRasterPipelineId::Transparent, "forward-transparent"},
           {ForwardRasterPipelineId::TransparentFrontCull,
            "forward-transparent-front-cull"},
           {ForwardRasterPipelineId::TransparentNoCull,
            "forward-transparent-no-cull"},
           {ForwardRasterPipelineId::PostProcess, "post-process"},
           {ForwardRasterPipelineId::LightGizmo, "light-gizmo"},
           {ForwardRasterPipelineId::LightGizmoCoverage,
            "light-gizmo-coverage"},
           {ForwardRasterPipelineId::TransformGizmo, "transform-gizmo"},
           {ForwardRasterPipelineId::TransformGizmoSolid,
            "transform-gizmo-solid"},
           {ForwardRasterPipelineId::TransformGizmoOverlay,
            "transform-gizmo-overlay"},
           {ForwardRasterPipelineId::TransformGizmoSolidOverlay,
            "transform-gizmo-solid-overlay"}}};
  uintptr_t nextPipeline = 0xe20;
  for (const auto &[id, name] : pipelineNames) {
    pipelineHandles.registerHandle(RegisteredPipelineHandle{
        .key = {RenderTechniqueId::ForwardRaster, name},
        .pipeline = fakeHandle<VkPipeline>(nextPipeline++)});
  }

  FrameRecordParams params;
  params.registries.pipelineHandles = &pipelineHandles;

  nextPipeline = 0xe20;
  for (const auto &[id, name] : pipelineNames) {
    static_cast<void>(name);
    EXPECT_EQ(forwardRasterPipelineHandle(params, id),
              fakeHandle<VkPipeline>(nextPipeline++));
    EXPECT_TRUE(forwardRasterPipelineReady(params, id));
  }

  PipelineRegistry pipelineLayouts;
  const std::array<std::pair<ForwardRasterPipelineLayoutId, const char *>, 5>
      layoutNames = {
          {{ForwardRasterPipelineLayoutId::Scene, "scene"},
           {ForwardRasterPipelineLayoutId::Transparent, "transparent"},
           {ForwardRasterPipelineLayoutId::PostProcess, "post-process"},
           {ForwardRasterPipelineLayoutId::LightGizmo, "light-gizmo"},
           {ForwardRasterPipelineLayoutId::TransformGizmo, "transform-gizmo"}}};
  uintptr_t nextLayout = 0xf20;
  for (const auto &[id, name] : layoutNames) {
    static_cast<void>(id);
    pipelineLayouts.registerLayout(RegisteredPipelineLayout{
        .key = {RenderTechniqueId::ForwardRaster, name},
        .layout = fakeHandle<VkPipelineLayout>(nextLayout++)});
  }
  params.registries.pipelineLayouts = &pipelineLayouts;

  nextLayout = 0xf20;
  for (const auto &[id, name] : layoutNames) {
    static_cast<void>(name);
    EXPECT_EQ(forwardRasterPipelineLayout(params, id),
              fakeHandle<VkPipelineLayout>(nextLayout++));
    EXPECT_TRUE(forwardRasterPipelineLayoutReady(params, id));
  }
}

TEST(DeferredRasterTechniqueRegistryTests,
     RegistersDeferredResourceAndPipelineContracts) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  DeferredRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  const auto deferredResources =
      resources.resourcesForTechnique(RenderTechniqueId::DeferredRaster);
  const auto deferredPipelines =
      pipelines.recipesForTechnique(RenderTechniqueId::DeferredRaster);
  EXPECT_GE(deferredResources.size(), 10u);
  EXPECT_GE(deferredPipelines.size(), 8u);

  const auto *sceneColor = resources.find(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "scene-color"});
  ASSERT_NE(sceneColor, nullptr);
  EXPECT_EQ(sceneColor->kind, FrameResourceKind::Image);
  EXPECT_EQ(sceneColor->image.format, VK_FORMAT_R16G16B16A16_SFLOAT);

  const auto *localShadowAtlas = resources.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "local-shadow-atlas"});
  ASSERT_NE(localShadowAtlas, nullptr);
  EXPECT_EQ(localShadowAtlas->kind, FrameResourceKind::External);

  const auto *oitNodes = resources.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "oit-node-buffer"});
  ASSERT_NE(oitNodes, nullptr);
  EXPECT_EQ(oitNodes->kind, FrameResourceKind::Buffer);

  for (const char *descriptorName :
       {"frame-lighting-descriptor-set", "post-process-descriptor-set",
        "oit-descriptor-set"}) {
    const auto *descriptor = resources.find(TechniqueResourceKey{
        RenderTechniqueId::DeferredRaster, descriptorName});
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->kind, FrameResourceKind::DescriptorSet);
  }

  for (const char *framebufferName :
       {"depth-prepass-framebuffer", "bim-depth-prepass-framebuffer",
        "gbuffer-framebuffer", "bim-gbuffer-framebuffer",
        "transparent-pick-framebuffer", "lighting-framebuffer",
        "transform-gizmo-framebuffer"}) {
    const auto *framebuffer = resources.find(TechniqueResourceKey{
        RenderTechniqueId::DeferredRaster, framebufferName});
    ASSERT_NE(framebuffer, nullptr);
    EXPECT_EQ(framebuffer->kind, FrameResourceKind::Framebuffer);
  }

  const auto *gbuffer = pipelines.find(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "gbuffer"});
  ASSERT_NE(gbuffer, nullptr);
  EXPECT_EQ(gbuffer->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(gbuffer->layoutName, "scene");

  const auto *tileCull = pipelines.find(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "tile-cull"});
  ASSERT_NE(tileCull, nullptr);
  EXPECT_EQ(tileCull->kind, PipelineRecipeKind::Compute);

  const auto *localShadowDepth = pipelines.find(TechniquePipelineKey{
      RenderTechniqueId::DeferredRaster, "local-shadow-depth"});
  ASSERT_NE(localShadowDepth, nullptr);
  EXPECT_EQ(localShadowDepth->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(localShadowDepth->layoutName, "shadow");

  technique.registerTechniqueContracts(context);
  EXPECT_EQ(
      resources.resourcesForTechnique(RenderTechniqueId::DeferredRaster).size(),
      deferredResources.size());
  EXPECT_EQ(
      pipelines.recipesForTechnique(RenderTechniqueId::DeferredRaster).size(),
      deferredPipelines.size());
}

TEST(DeferredRasterTechniqueRegistryTests,
     RegistersImportedServiceResourceContracts) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  DeferredRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  const auto *cameraBuffer = resources.find(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "camera-buffer"});
  ASSERT_NE(cameraBuffer, nullptr);
  EXPECT_EQ(cameraBuffer->kind, FrameResourceKind::Buffer);
  EXPECT_EQ(cameraBuffer->lifetime, FrameResourceLifetime::Imported);
  EXPECT_EQ(cameraBuffer->buffer.size, sizeof(container::gpu::CameraData));
  EXPECT_EQ(cameraBuffer->buffer.usage, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

  const auto *sceneObjectBuffer = resources.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "scene-object-buffer"});
  ASSERT_NE(sceneObjectBuffer, nullptr);
  EXPECT_EQ(sceneObjectBuffer->kind, FrameResourceKind::Buffer);
  EXPECT_EQ(sceneObjectBuffer->lifetime, FrameResourceLifetime::Imported);

  for (const char *descriptorName :
       {"scene-descriptor-set", "bim-scene-descriptor-set",
        "light-descriptor-set", "tiled-lighting-descriptor-set",
        "shadow-descriptor-set", "local-shadow-descriptor-set"}) {
    const auto *descriptor = resources.find(TechniqueResourceKey{
        RenderTechniqueId::DeferredRaster, descriptorName});
    ASSERT_NE(descriptor, nullptr) << descriptorName;
    EXPECT_EQ(descriptor->kind, FrameResourceKind::DescriptorSet)
        << descriptorName;
    EXPECT_EQ(descriptor->lifetime, FrameResourceLifetime::Imported)
        << descriptorName;
  }

  const auto *sampler = resources.find(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "g-buffer-sampler"});
  ASSERT_NE(sampler, nullptr);
  EXPECT_EQ(sampler->kind, FrameResourceKind::Sampler);
  EXPECT_EQ(sampler->lifetime, FrameResourceLifetime::Imported);
}

TEST(DeferredRasterTechniqueRegistryTests,
     PickDepthContractMatchesCopyAndAttachmentUsage) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  DeferredRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  const auto *pickDepth = resources.find(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "pick-depth"});
  ASSERT_NE(pickDepth, nullptr);
  EXPECT_EQ(pickDepth->kind, FrameResourceKind::Image);
  EXPECT_TRUE((pickDepth->image.usage &
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u);
  EXPECT_TRUE((pickDepth->image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u);
  EXPECT_TRUE((pickDepth->image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u);
}

TEST(DeferredRasterTechniqueRegistryTests,
     BuildFrameGraphPublishesContractsBeforeRecorderCheck) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  DeferredRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.buildFrameGraph(context);

  EXPECT_TRUE(resources.contains(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "depth-stencil"}));
  EXPECT_TRUE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "post-process"}));
}

TEST(DeferredRasterTechniqueRegistryTests,
     DefaultRegistrySelectionPublishesDeferredAndForwardContracts) {
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  FrameRecorder recorder;
  DeferredRasterFrameGraphContext deferredContext(
      DeferredRasterFrameGraphServices{.graph = &recorder.graph()});
  auto registry = container::renderer::createDefaultRenderTechniqueRegistry();
  RenderSystemContext context{
      .frameRecorder = &recorder,
      .deferredRaster = &deferredContext,
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  const auto selection =
      registry.select(RenderTechniqueId::DeferredRaster, context);
  ASSERT_NE(selection.technique, nullptr);
  EXPECT_EQ(selection.selected, RenderTechniqueId::DeferredRaster);

  selection.technique->registerTechniqueContracts(context);

  EXPECT_TRUE(resources.contains(
      TechniqueResourceKey{RenderTechniqueId::DeferredRaster, "scene-color"}));
  EXPECT_TRUE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::DeferredRaster, "gbuffer"}));

  const auto forwardSelection =
      registry.select(RenderTechniqueId::ForwardRaster, context);
  ASSERT_NE(forwardSelection.technique, nullptr);
  EXPECT_EQ(forwardSelection.selected, RenderTechniqueId::ForwardRaster);

  forwardSelection.technique->registerTechniqueContracts(context);

  EXPECT_TRUE(resources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster, "scene-color"}));
  EXPECT_TRUE(pipelines.contains(TechniquePipelineKey{
      RenderTechniqueId::ForwardRaster, "forward-opaque"}));
  EXPECT_FALSE(resources.contains(TechniqueResourceKey{
      RenderTechniqueId::ForwardRaster, "gbuffer-framebuffer"}));
}

TEST(TechniqueRegistryGuardrails,
     DeferredCompatibilityStructsStayAlgorithmNeutral) {
  const std::string frameResources =
      readRepoTextFile("include/Container/renderer/resources/FrameResources.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");

  for (const std::string &forbidden :
       {"ForwardRaster", "ForwardPlus", "RayTracing", "PathTracing", "Splat",
        "RadianceField", "NeRF", "Gaussian", "accumulation"}) {
    EXPECT_FALSE(contains(frameResources, forbidden))
        << "FrameResources should not grow technique-specific " << forbidden
        << " fields; register them through FrameResourceRegistry.";
    EXPECT_FALSE(contains(pipelineTypes, forbidden))
        << "PipelineTypes should not grow technique-specific " << forbidden
        << " fields; register them through PipelineRegistry.";
  }
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendWiresResourceAndPipelineRegistriesIntoContext) {
  const std::string renderTechniqueHeader =
      readRepoTextFile("include/Container/renderer/core/RenderTechnique.h");
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");

  EXPECT_TRUE(
      contains(renderTechniqueHeader, "FrameResourceRegistry* frameResources"));
  EXPECT_TRUE(contains(renderTechniqueHeader, "PipelineRegistry* pipelines"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "std::unique_ptr<FrameResourceRegistry>"));
  EXPECT_TRUE(
      contains(rendererFrontendHeader, "std::unique_ptr<PipelineRegistry>"));
  EXPECT_TRUE(
      contains(rendererFrontend, "std::make_unique<FrameResourceRegistry>()"));
  EXPECT_TRUE(
      contains(rendererFrontend, "std::make_unique<PipelineRegistry>()"));
  EXPECT_TRUE(contains(rendererFrontend,
                       ".frameResources = subs_.frameResourceRegistry.get()"));
  EXPECT_TRUE(
      contains(rendererFrontend, ".pipelines = subs_.pipelineRegistry.get()"));
  EXPECT_TRUE(contains(rendererFrontend, "initializeRenderTechnique"));
  EXPECT_TRUE(
      contains(rendererFrontend, "registerTechniqueContracts(context)"));
  EXPECT_TRUE(
      contains(deferredTechnique, "registerDeferredRasterFrameResources"));
  EXPECT_TRUE(
      contains(deferredTechnique, "registerDeferredRasterPipelineRecipes"));
}

TEST(TechniqueRegistryGuardrails,
     FrameResourceManagerPublishesProductionResourcesToRegistry) {
  const std::string frameResourceManagerHeader = readRepoTextFile(
      "include/Container/renderer/resources/FrameResourceManager.h");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  EXPECT_TRUE(contains(frameResourceManagerHeader, "resourceRegistry()"));
  EXPECT_TRUE(contains(frameResourceManagerHeader,
                       "FrameResourceRegistry resourceRegistry_"));
  EXPECT_TRUE(contains(frameResourceManager, "publishFrameResourceBindings()"));
  EXPECT_TRUE(contains(frameResourceManager, "resourceRegistry_.bindImage"));
  EXPECT_TRUE(contains(frameResourceManager, "resourceRegistry_.bindBuffer"));
  EXPECT_TRUE(
      contains(frameResourceManager, "resourceRegistry_.bindFramebuffer"));
  EXPECT_TRUE(
      contains(frameResourceManager, "resourceRegistry_.bindDescriptorSet"));
  EXPECT_TRUE(contains(frameResourceManager, "\"scene-color\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"depth-stencil\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"oit-node-buffer\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"oit-metadata-buffer\""));
  EXPECT_TRUE(
      contains(frameResourceManager, "\"frame-lighting-descriptor-set\""));
  EXPECT_TRUE(
      contains(frameResourceManager, "\"post-process-descriptor-set\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"oit-descriptor-set\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"gbuffer-framebuffer\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"lighting-framebuffer\""));
  EXPECT_TRUE(
      contains(frameResourceManager, "\"transform-gizmo-framebuffer\""));
  EXPECT_TRUE(
      contains(frameResourceManager, "resourceRegistry_.clearBindings"));
}

TEST(TechniqueRegistryGuardrails,
     FrameResourceManagerSceneColorUsageMatchesTechniqueContracts) {
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  const size_t sceneColorAllocation =
      frameResourceManager.find("f.sceneColor = createAttachment");
  ASSERT_NE(sceneColorAllocation, std::string::npos);
  const std::string sceneColorBlock =
      frameResourceManager.substr(sceneColorAllocation, 320);

  EXPECT_TRUE(
      contains(sceneColorBlock, "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT"));
  EXPECT_TRUE(contains(sceneColorBlock, "VK_IMAGE_USAGE_SAMPLED_BIT"));
  EXPECT_TRUE(contains(sceneColorBlock, "VK_IMAGE_USAGE_STORAGE_BIT"));
  EXPECT_TRUE(contains(sceneColorBlock, "VK_IMAGE_USAGE_TRANSFER_SRC_BIT"));
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendConsumesPublishedResourceBindingsForReadback) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontend, "deferredRasterRuntimeImage("));
  EXPECT_TRUE(contains(rendererFrontend, "\"depth-stencil\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"pick-depth\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"pick-id\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"oit-node-buffer\""));
  EXPECT_TRUE(contains(rendererFrontend, "submitReadbackCommandBufferAndWait"));
  EXPECT_FALSE(
      contains(rendererFrontend,
               "vkQueueWaitIdle(svc_.ctx.deviceWrapper->graphicsQueue())"));
  EXPECT_FALSE(contains(rendererFrontend, "frameResourceManager->frame("));
  EXPECT_FALSE(contains(rendererFrontend, "frame->oitNodeCapacity"));
  EXPECT_FALSE(contains(rendererFrontend, "frame->depthStencil.image"));
  EXPECT_FALSE(contains(rendererFrontend, "frame->pickDepth.image"));
  EXPECT_FALSE(contains(rendererFrontend, "frame->pickId.image"));
}

TEST(TechniqueRegistryGuardrails, RendererFrontendReadbacksAreFrameSlotOwned) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontendHeader, "struct HostReadbackSlot"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "std::vector<HostReadbackSlot> readbacks"));
  EXPECT_TRUE(contains(rendererFrontendHeader, "uint32_t frameSlot"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "ensureScreenshotReadbackBuffer(frame_.currentFrame"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "writePendingScreenshotPng(frame_.currentFrame"));
  EXPECT_TRUE(contains(rendererFrontend, "ensureDepthVisibilityReadbackBuffer("
                                         "frame->frameSlot"));
  EXPECT_FALSE(
      contains(rendererFrontendHeader,
               "ScreenshotState {\n"
               "    std::filesystem::path outputPath{};\n"
               "    bool pending{false};\n"
               "    container::gpu::AllocatedBuffer readbackBuffer{}"));
  EXPECT_FALSE(
      contains(rendererFrontendHeader,
               "DepthVisibilityState {\n"
               "    container::gpu::AllocatedBuffer readbackBuffer{}"));
  EXPECT_FALSE(contains(rendererFrontend, "screenshot_.readbackBuffer"));
  EXPECT_FALSE(contains(rendererFrontend, "depthVisibility_.readbackBuffer"));
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendDepthVisibilitySnapshotsAreFrameSlotOwned) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(
      contains(rendererFrontendHeader, "struct DepthVisibilityFrameSlot"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "std::vector<DepthVisibilityFrameSlot> slots"));
  EXPECT_TRUE(contains(rendererFrontendHeader, "uint32_t latestFrameSlot"));
  EXPECT_TRUE(
      contains(rendererFrontend,
               "depthVisibilityFrameSlot(depthVisibility_.latestFrameSlot"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "DepthVisibilityFrameSlot& frame = "
                       "ensureDepthVisibilityFrameSlot(frameSlot)"));
  EXPECT_FALSE(contains(rendererFrontendHeader,
                        "DepthVisibilityState {\n"
                        "    std::vector<HostReadbackSlot> readbacks{}"));
  EXPECT_FALSE(contains(rendererFrontendHeader,
                        "DepthVisibilityState {\n"
                        "    std::vector<HostReadbackSlot> readbacks{};\n"
                        "    uint32_t frameSlot"));
  EXPECT_FALSE(contains(rendererFrontend, "depthVisibility_.renderFence"));
  EXPECT_FALSE(contains(rendererFrontend, "depthVisibility_.imageIndex"));
  EXPECT_FALSE(contains(rendererFrontend, "depthVisibility_.cameraData"));
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendResetsFrameFenceOnlyAfterCommandRecording) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  const size_t drawFrame =
      rendererFrontend.find("bool RendererFrontend::drawFrame");
  ASSERT_NE(drawFrame, std::string::npos);
  const size_t record =
      rendererFrontend.find("recordCommandBuffer(svc_.commandBufferManager."
                            "buffer(imageIndex), imageIndex",
                            drawFrame);
  const size_t reset = rendererFrontend.find(
      "subs_.frameSyncManager->resetFence(frame_.currentFrame)", drawFrame);
  const size_t assign =
      rendererFrontend.find("frame_.imagesInFlight[imageIndex] =", drawFrame);
  const size_t submit = rendererFrontend.find("vkQueueSubmit(", drawFrame);

  ASSERT_NE(record, std::string::npos);
  ASSERT_NE(reset, std::string::npos);
  ASSERT_NE(assign, std::string::npos);
  ASSERT_NE(submit, std::string::npos);
  EXPECT_LT(record, reset);
  EXPECT_LT(reset, submit);
  EXPECT_LT(submit, assign);
}

TEST(TechniqueRegistryGuardrails,
     FrameRecordParamsCarriesFrameSlotSeparatelyFromImageIndex) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(frameRecorderHeader, "uint32_t frameSlot"));
  EXPECT_TRUE(contains(frameRecorderHeader, "uint32_t imageIndex"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.runtime.frameSlot = frame_.currentFrame"));
  EXPECT_TRUE(contains(rendererFrontend, "p.runtime.imageIndex = imageIndex"));
  EXPECT_TRUE(contains(frameRecorder,
                       "beginFrame(commandBuffer, p.runtime.frameSlot)"));
  EXPECT_TRUE(contains(frameRecorder, "runtime.imageIndex"));
}

TEST(TechniqueRegistryGuardrails,
     SceneControllerTracksObjectBufferUploadsPerDestinationBuffer) {
  const std::string sceneControllerHeader =
      readRepoTextFile("include/Container/renderer/scene/SceneController.h");
  const std::string sceneController =
      readRepoTextFile("src/renderer/scene/SceneController.cpp");

  EXPECT_TRUE(contains(sceneControllerHeader,
                       "std::unordered_map<VkBuffer, uint64_t> "
                       "objectBufferUploadRevisions_"));
  EXPECT_TRUE(
      contains(sceneControllerHeader, "uint64_t objectBufferUploadRevision_"));
  EXPECT_TRUE(contains(sceneController, "++objectBufferUploadRevision_"));
  EXPECT_TRUE(
      contains(sceneController,
               "objectBufferUploadRevisions_.find(objectBuffer.buffer)"));
  EXPECT_TRUE(contains(sceneController,
                       "uploadedRevision != objectBufferUploadRevision_"));
  EXPECT_TRUE(contains(sceneController,
                       "objectBufferUploadRevisions_[objectBuffer.buffer]"));
  EXPECT_FALSE(contains(sceneControllerHeader, "objectBufferUploadDirty_"));
  EXPECT_FALSE(contains(sceneController, "objectBufferUploadDirty_ = false"));
}

TEST(TechniqueRegistryGuardrails,
     RuntimePickDepthAndCameraResourcesPublishCopyUsages) {
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(frameResourceManager,
                       "f.pickDepth = createAttachment("
                       "formats_.depthStencil,\n"
                       "                   "
                       "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |\n"
                       "                       "
                       "VK_IMAGE_USAGE_TRANSFER_DST_BIT |\n"
                       "                       "
                       "VK_IMAGE_USAGE_TRANSFER_SRC_BIT"));
  EXPECT_TRUE(contains(frameResourceManager,
                       "bindSharedImage(\"pick-depth\", f.pickDepth,\n"
                       "              "
                       "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |\n"
                       "                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |\n"
                       "                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |\n"
                       "            VK_BUFFER_USAGE_TRANSFER_SRC_BIT"));
}

TEST(TechniqueRegistryGuardrails,
     DirectionalShadowDescriptorFallbacksUseShadowResources) {
  const std::string frameResourceManagerHeader = readRepoTextFile(
      "include/Container/renderer/resources/FrameResourceManager.h");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  EXPECT_TRUE(
      contains(frameResourceManagerHeader, "fallbackShadowDataBuffer_"));
  EXPECT_TRUE(contains(frameResourceManagerHeader, "fallbackShadowAtlas_"));
  EXPECT_TRUE(contains(frameResourceManagerHeader, "fallbackShadowSampler_"));
  EXPECT_TRUE(
      contains(frameResourceManagerHeader, "ensureFallbackShadowDataBuffer"));
  EXPECT_TRUE(
      contains(frameResourceManagerHeader, "ensureFallbackShadowResources"));

  EXPECT_TRUE(contains(frameResourceManager, "allocationMgr_->destroyBuffer("
                                             "fallbackShadowDataBuffer_)"));
  EXPECT_TRUE(contains(frameResourceManager,
                       "destroyAttachment(fallbackShadowAtlas_)"));
  EXPECT_TRUE(contains(frameResourceManager,
                       "vkDestroySampler(device_->device(), "
                       "fallbackShadowSampler_"));
  EXPECT_TRUE(
      contains(frameResourceManager, "ensureFallbackShadowResources();"));
  EXPECT_TRUE(
      contains(frameResourceManager,
               "shadowUboInfo = {fallbackShadowDataBuffer_.buffer, 0,"));
  EXPECT_TRUE(
      contains(frameResourceManager,
               "postShadowUboInfo = {fallbackShadowDataBuffer_.buffer, 0,"));
  EXPECT_TRUE(contains(frameResourceManager, "shadowAtlasInfo.imageView = "
                                             "fallbackShadowAtlas_.view;"));
  EXPECT_TRUE(contains(frameResourceManager, "postShadowAtlasInfo.imageView = "
                                             "fallbackShadowAtlas_.view;"));
  EXPECT_TRUE(contains(frameResourceManager, "shadowSamplerInfo.sampler = "
                                             "fallbackShadowSampler_;"));
  EXPECT_TRUE(
      contains(frameResourceManager, "sizeof(container::gpu::ShadowData)"));

  EXPECT_FALSE(contains(frameResourceManager, "shadowUboInfo = camInfo"));
  EXPECT_FALSE(contains(frameResourceManager, "postShadowUboInfo = camInfo"));
  EXPECT_FALSE(contains(frameResourceManager, "shadowAtlasInfo = depthImg"));
  EXPECT_FALSE(
      contains(frameResourceManager, "postShadowAtlasInfo = depthPostProcess"));
  EXPECT_FALSE(contains(frameResourceManager, "shadowSamplerInfo = sampInfo"));
}

TEST(TechniqueRegistryGuardrails,
     GpuCullIndirectCountPathRequiresMultiDrawIndirect) {
  const std::string contextInitializer =
      readRepoTextFile("src/renderer/platform/VulkanContextInitializer.cpp");
  const std::string vulkanDeviceHeader =
      readRepoTextFile("include/Container/utility/VulkanDevice.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string shadowCullManager =
      readRepoTextFile("src/renderer/shadow/ShadowCullManager.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");

  EXPECT_TRUE(contains(contextInitializer,
                       "ci.optionalFeatures.multiDrawIndirect = VK_TRUE"));
  EXPECT_TRUE(contains(contextInitializer,
                       "vulkan12Features.drawIndirectCount = VK_TRUE"));
  EXPECT_TRUE(contains(vulkanDeviceHeader, "enabledVulkan12Features()"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "device_->enabledFeatures().multiDrawIndirect == "
                       "VK_TRUE"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "device_->enabledVulkan12Features().drawIndirectCount "
                       "== VK_TRUE"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "device_->enabledFeatures().multiDrawIndirect == "
                       "VK_TRUE"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "device_->enabledVulkan12Features().drawIndirectCount "
                       "== VK_TRUE"));
  EXPECT_TRUE(contains(bimManager,
                       "device_->enabledFeatures().drawIndirectFirstInstance "
                       "== VK_TRUE"));
  EXPECT_TRUE(contains(bimManager,
                       "device_->enabledFeatures().multiDrawIndirect == "
                       "VK_TRUE"));
  EXPECT_TRUE(contains(bimManager,
                       "device_->enabledVulkan12Features().drawIndirectCount "
                       "== VK_TRUE"));
}

TEST(TechniqueRegistryGuardrails,
     GpuCullUploadPreservesDrawCommandInstanceCounts) {
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string shadowCullManager =
      readRepoTextFile("src/renderer/shadow/ShadowCullManager.cpp");

  EXPECT_TRUE(contains(gpuCullManager,
                       "gpuCmds[i].instanceCount =\n"
                       "        std::max(commands[i].instanceCount, 1u)"));
  EXPECT_FALSE(contains(gpuCullManager, "gpuCmds[i].instanceCount = 1;"));
  EXPECT_TRUE(
      contains(shadowCullManager, "std::max(commands[i].instanceCount, 1u)"));
  EXPECT_FALSE(contains(shadowCullManager, "gpuCmds[i].instanceCount = 1;"));
}

TEST(TechniqueRegistryGuardrails, GpuCullUploadSkipsUnchangedDrawStreams) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string gpuCullUploadPlanner = readRepoTextFile(
      "include/Container/renderer/culling/GpuCullDrawUploadPlanner.h");
  const std::string frustumCullRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp");

  EXPECT_TRUE(contains(gpuCullHeader, "uint64_t sourceRevision"));
  EXPECT_TRUE(contains(gpuCullHeader, "lastUploadSourceData_"));
  EXPECT_TRUE(contains(gpuCullManager, "buildGpuCullDrawUploadPlan"));
  EXPECT_TRUE(contains(gpuCullUploadPlanner,
                       "cache.sourceData[inputs.imageIndex] == "
                       "inputs.sourceData"));
  EXPECT_TRUE(contains(gpuCullUploadPlanner,
                       "cache.sourceRevisions[inputs.imageIndex] =="));
  EXPECT_TRUE(contains(gpuCullUploadPlanner, "GpuCullDrawUploadAction"));
  EXPECT_TRUE(contains(frustumCullRecorder, "inputs.drawSourceRevision"));
}

TEST(TechniqueRegistryGuardrails,
     OcclusionCullPassChecksResourcesBeforePublishingDraws) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");

  EXPECT_TRUE(contains(gpuCullHeader, "occlusionCullResourcesReady"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "bool GpuCullManager::occlusionCullResourcesReady("
                       "uint32_t imageIndex) const"));
  EXPECT_TRUE(contains(deferredTechnique, "deferred->gpuCullManager()->"
                                          "occlusionCullResourcesReady("));
}

TEST(TechniqueRegistryGuardrails,
     BimGpuVisibilityPreparationUsesFrameRoutingFlags) {
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimFrameGpuVisibilityRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string frameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");

  EXPECT_TRUE(contains(recorderHeader, "prepareBimFrameGpuVisibility("
                                       "BimManager *manager,\n"
                                       "                                  "
                                       "const FrameBimResources &bim)"));
  EXPECT_TRUE(contains(recorder,
                       ".meshCompactionEnabled =\n"
                       "          bim.opaqueMeshDrawsUseGpuVisibility ||\n"
                       "          bim.transparentMeshDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(frameGraphContext,
                       "prepareBimFrameGpuVisibility(p.services.bimManager, "
                       "p.bim)"));
}

TEST(TechniqueRegistryGuardrails, SceneRasterPassesPublishReadinessCallbacks) {
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");

  EXPECT_TRUE(contains(deferredTechnique, "graph.setPassReadiness(\n"
                                          "      RenderPassId::DepthPrepass"));
  EXPECT_TRUE(contains(deferredTechnique, "graph.setPassReadiness(\n"
                                          "      RenderPassId::GBuffer"));
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendOwnsRuntimeResourceBindingRegistry) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontendHeader, "frameRuntimeResourceRegistry"));
  EXPECT_TRUE(
      contains(rendererFrontendHeader, "publishFrameRuntimeResourceBindings"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "publishFrameRuntimeResourceBindings(imageIndex)"));
  EXPECT_TRUE(
      contains(rendererFrontend, "subs_.frameRuntimeResourceRegistry.get()"));
  EXPECT_TRUE(contains(rendererFrontend, "forEachBindingForFrame"));
  EXPECT_FALSE(contains(rendererFrontend, "bindingsForFrame("));
  EXPECT_TRUE(contains(rendererFrontend, "\"scene-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"bim-scene-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"light-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"tiled-lighting-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"shadow-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"local-shadow-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"camera-buffer\""));
  EXPECT_TRUE(contains(rendererFrontend, "\"g-buffer-sampler\""));
  EXPECT_FALSE(contains(rendererFrontend,
                        "? &subs_.frameResourceManager->resourceRegistry()"));
}

TEST(TechniqueRegistryGuardrails,
     FrameResourceManagerPublishesForwardRasterSharedRuntimeBindings) {
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  EXPECT_TRUE(contains(frameResourceManager, "kForwardRasterTechnique"));
  EXPECT_TRUE(
      contains(frameResourceManager, "RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(frameResourceManager, "kForwardRasterSharedImageNames"));
  EXPECT_TRUE(
      contains(frameResourceManager, "kForwardRasterSharedBufferNames"));
  EXPECT_TRUE(
      contains(frameResourceManager, "kForwardRasterSharedDescriptorSetNames"));
  EXPECT_TRUE(
      contains(frameResourceManager, "kForwardRasterSharedFramebufferNames"));
  EXPECT_TRUE(contains(frameResourceManager, "bindSharedImage"));
  EXPECT_TRUE(contains(frameResourceManager, "bindSharedBuffer"));
  EXPECT_TRUE(contains(frameResourceManager, "bindSharedDescriptorSet"));
  EXPECT_TRUE(contains(frameResourceManager, "bindSharedFramebuffer"));

  const std::string sharedImages =
      definitionBlock(frameResourceManager, "kForwardRasterSharedImageNames");
  const std::string sharedBuffers =
      definitionBlock(frameResourceManager, "kForwardRasterSharedBufferNames");
  const std::string sharedDescriptors = definitionBlock(
      frameResourceManager, "kForwardRasterSharedDescriptorSetNames");
  const std::string sharedFramebuffers = definitionBlock(
      frameResourceManager, "kForwardRasterSharedFramebufferNames");
  ASSERT_FALSE(sharedImages.empty());
  ASSERT_FALSE(sharedBuffers.empty());
  ASSERT_FALSE(sharedDescriptors.empty());
  ASSERT_FALSE(sharedFramebuffers.empty());

  for (const char *name :
       {"depth-stencil", "depth-sampling-view", "scene-color", "pick-id",
        "pick-depth", "oit-head-pointers", "oit-node-buffer",
        "oit-counter-buffer", "oit-metadata-buffer",
        "frame-lighting-descriptor-set", "post-process-descriptor-set",
        "oit-descriptor-set", "depth-prepass-framebuffer",
        "bim-depth-prepass-framebuffer", "transparent-pick-framebuffer",
        "lighting-framebuffer", "transform-gizmo-framebuffer"}) {
    EXPECT_TRUE(contains(frameResourceManager, name)) << name;
  }

  const std::string allForwardSharedNames =
      sharedImages + sharedBuffers + sharedDescriptors + sharedFramebuffers;
  for (const char *deferredOnlyName :
       {"gbuffer-framebuffer", "bim-gbuffer-framebuffer", "g-buffer-sampler",
        "tiled-lighting-descriptor-set"}) {
    EXPECT_FALSE(contains(allForwardSharedNames, deferredOnlyName))
        << deferredOnlyName;
  }

  EXPECT_FALSE(contains(frameResourceManager,
                        "RenderTechniqueId::ForwardRaster, \"gbuffer"));
  EXPECT_FALSE(contains(frameResourceManager,
                        "RenderTechniqueId::ForwardRaster, \"bim-gbuffer"));
  EXPECT_FALSE(contains(frameResourceManager,
                        "RenderTechniqueId::ForwardRaster, "
                        "\"g-buffer-sampler\""));
}

TEST(TechniqueRegistryGuardrails,
     RendererFrontendPublishesForwardRasterRuntimeOverlayBindings) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontend, "copyManagerBinding"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "copyManagerBinding(RenderTechniqueId::DeferredRaster"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "copyManagerBinding(RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(rendererFrontend, "bindSharedDescriptorSet"));
  EXPECT_TRUE(contains(rendererFrontend, "bindSharedBuffer"));
  EXPECT_TRUE(contains(rendererFrontend, "bindDeferredDescriptorSet"));

  EXPECT_TRUE(contains(rendererFrontend,
                       "bindSharedDescriptorSet(\"scene-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "bindSharedDescriptorSet(\n"
                                         "      \"bim-scene-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bindSharedDescriptorSet(\"light-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bindSharedDescriptorSet(\"shadow-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bindSharedDescriptorSet(\n"
                       "      \"local-shadow-descriptor-set\""));

  EXPECT_TRUE(contains(rendererFrontend,
                       "bindDeferredDescriptorSet(\n"
                       "      \"tiled-lighting-descriptor-set\""));
  EXPECT_TRUE(contains(rendererFrontend, "bindSharedBuffer(\"camera-buffer\""));
  EXPECT_TRUE(contains(rendererFrontend, "bindSharedBuffer(\n"
                                         "        \"scene-object-buffer\""));
  EXPECT_TRUE(contains(rendererFrontend, "RenderTechniqueId::DeferredRaster, "
                                         "\"g-buffer-sampler\""));
  EXPECT_FALSE(contains(rendererFrontend, "RenderTechniqueId::ForwardRaster, "
                                          "\"tiled-lighting-descriptor-set\""));
  EXPECT_FALSE(contains(rendererFrontend, "RenderTechniqueId::ForwardRaster, "
                                          "\"g-buffer-sampler\""));
}

TEST(TechniqueRegistryGuardrails,
     DeferredRasterTechniqueUsesResourceBridgeForPublishedFramebuffers) {
  const std::string bridgeHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterResourceBridge.h");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(bridgeHeader, "DeferredRasterFramebufferId"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterFramebufferBinding"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterFramebuffer("));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterRenderPass("));
  EXPECT_TRUE(contains(bridgeHeader, "p.framebuffer("));
  EXPECT_FALSE(contains(bridgeHeader, "deferredRasterLegacyFramebuffer"));
  EXPECT_FALSE(contains(bridgeHeader, "p.runtime.frame"));

  EXPECT_TRUE(contains(deferredTechnique, "DeferredRasterResourceBridge.h"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterFramebufferId::DepthPrepass"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterFramebufferId::BimDepthPrepass"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterFramebufferId::GBuffer"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterFramebufferId::BimGBuffer"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterFramebufferId::TransparentPick"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterFramebufferId::TransformGizmo"));
  EXPECT_TRUE(contains(lightingPassRecorder, "DeferredRasterResourceBridge.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredRasterFramebufferId::Lighting"));
  EXPECT_FALSE(contains(deferredTechnique, "depthPrepassFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "bimDepthPrepassFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "gBufferFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "bimGBufferFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "transparentPickFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "transformGizmoFramebuffer"));
  EXPECT_FALSE(contains(lightingPassRecorder, "lightingFramebuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.depthPrepass"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.gBuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.bimDepthPrepass"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.bimGBuffer"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.transparentPick"));
  EXPECT_FALSE(contains(deferredTechnique, "p.renderPasses.transformGizmos"));
  EXPECT_FALSE(contains(lightingPassRecorder, "p.renderPasses.lighting"));
}

TEST(TechniqueRegistryGuardrails,
     ForwardRasterBridgeHeadersStayScopedToForwardContracts) {
  const std::string resourceBridge = readRepoTextFile(
      "include/Container/renderer/forward/ForwardRasterResourceBridge.h");
  const std::string pipelineBridge = readRepoTextFile(
      "include/Container/renderer/forward/ForwardRasterPipelineBridge.h");

  EXPECT_TRUE(contains(resourceBridge, "RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(pipelineBridge, "RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(resourceBridge, "ForwardRasterFramebufferId"));
  EXPECT_TRUE(contains(resourceBridge, "ForwardRasterImageId"));
  EXPECT_TRUE(contains(resourceBridge, "ForwardRasterBufferId"));
  EXPECT_TRUE(contains(resourceBridge, "ForwardRasterDescriptorSetId"));
  EXPECT_TRUE(contains(pipelineBridge, "ForwardRasterPipelineId"));
  EXPECT_TRUE(contains(pipelineBridge, "ForwardRasterPipelineLayoutId"));
  EXPECT_TRUE(contains(pipelineBridge, "\"depth-prepass\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"bim-depth-prepass\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"forward-opaque\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"forward-transparent\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"forward-transparent-front-cull\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"forward-transparent-no-cull\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"post-process\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"light-gizmo\""));
  EXPECT_TRUE(contains(pipelineBridge, "\"transform-gizmo\""));
  EXPECT_TRUE(contains(pipelineBridge, "ForwardRasterPipelineLayoutId::Scene"));
  EXPECT_TRUE(
      contains(pipelineBridge, "ForwardRasterPipelineLayoutId::LightGizmo"));

  EXPECT_FALSE(contains(resourceBridge, "g-buffer-sampler"));
  EXPECT_FALSE(contains(resourceBridge, "ForwardRasterSamplerId"));
  EXPECT_FALSE(contains(pipelineBridge, "ForwardOpaqueFrontCull"));
  EXPECT_FALSE(contains(pipelineBridge, "ForwardOpaqueNoCull"));
}

TEST(TechniqueRegistryGuardrails,
     DeferredRasterPassesUseResourceBridgeForPublishedImagesAndOitBuffers) {
  const std::string bridgeHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterResourceBridge.h");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string oitFramePassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredTransparentOitFramePassRecorder.cpp");
  const std::string oitRecorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredTransparentOitRecorder.h");
  const std::string oitManagerHeader =
      readRepoTextFile("include/Container/renderer/effects/OitManager.h");
  const std::string oitManager =
      readRepoTextFile("src/renderer/effects/OitManager.cpp");

  EXPECT_TRUE(contains(bridgeHeader, "DeferredRasterImageId"));
  EXPECT_TRUE(contains(bridgeHeader, "DeferredRasterBufferId"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterImageBinding"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterBufferBinding"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterImageView("));
  EXPECT_TRUE(contains(bridgeHeader, "p.imageBinding("));
  EXPECT_TRUE(contains(bridgeHeader, "p.bufferBinding("));
  EXPECT_TRUE(contains(frameResourceManager, "\"depth-sampling-view\""));
  EXPECT_TRUE(contains(deferredTechnique, "\"depth-sampling-view\""));

  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterImageId::DepthSamplingView"));
  EXPECT_TRUE(contains(deferredTechnique, "DeferredRasterImageId::SceneColor"));
  EXPECT_TRUE(contains(deferredTechnique, "DeferredRasterImageId::PickId"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterImageId::DepthSamplingView"));

  EXPECT_TRUE(contains(oitFramePassRecorder, "deferredRasterImage("));
  EXPECT_TRUE(contains(oitFramePassRecorder, "deferredRasterBuffer("));
  EXPECT_TRUE(
      contains(oitFramePassRecorder, "DeferredRasterImageId::OitHeadPointers"));
  EXPECT_TRUE(
      contains(oitFramePassRecorder, "DeferredRasterBufferId::OitNode"));
  EXPECT_TRUE(
      contains(oitFramePassRecorder, "DeferredRasterBufferId::OitCounter"));
  EXPECT_TRUE(contains(oitRecorderHeader, "OitFrameResources resources"));
  EXPECT_TRUE(contains(oitManagerHeader, "struct OitFrameResources"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "deferred->isPassActive(RenderPassId::OitResolve)"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "graph.setPassReadiness(RenderPassId::PostProcess"));
  EXPECT_TRUE(
      contains(deferredTechnique, "deferredRasterPostProcessReadiness"));

  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->depthSamplingView"));
  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->depthStencil.image"));
  EXPECT_FALSE(contains(deferredTechnique, "p.runtime.frame->pickDepth.image"));
  EXPECT_FALSE(contains(deferredTechnique, "p.runtime.frame->pickId.image"));
  EXPECT_FALSE(contains(deferredTechnique, "p.runtime.frame->normal.view"));
  EXPECT_FALSE(contains(deferredTechnique, "p.runtime.frame->sceneColor.view"));
  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->sceneColor.image"));
  EXPECT_FALSE(
      contains(lightingPassRecorder, "p.runtime.frame->depthSamplingView"));
  EXPECT_FALSE(contains(oitRecorderHeader, "const FrameResources *frame"));
  EXPECT_FALSE(
      contains(oitManagerHeader, "renderer/resources/FrameResources.h"));
  EXPECT_FALSE(contains(oitManagerHeader, "const FrameResources"));
  EXPECT_FALSE(contains(oitManager, "frame.oitHeadPointers"));
  EXPECT_FALSE(contains(oitManager, "frame.oitNodeBuffer"));
  EXPECT_FALSE(contains(oitManager, "frame.oitCounterBuffer"));
  EXPECT_FALSE(contains(deferredTechnique,
                        ".transparentOitActive = transparentOit.enabled(p)"));
}

TEST(TechniqueRegistryGuardrails,
     DeferredRasterPassesUseResourceBridgeForPublishedDescriptorSets) {
  const std::string frameResourceRegistryHeader = readRepoTextFile(
      "include/Container/renderer/resources/FrameResourceRegistry.h");
  const std::string frameResourceRegistry =
      readRepoTextFile("src/renderer/resources/FrameResourceRegistry.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");
  const std::string bridgeHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterResourceBridge.h");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(frameResourceRegistryHeader, "FrameDescriptorBinding"));
  EXPECT_TRUE(contains(frameResourceRegistryHeader, "FrameSamplerBinding"));
  EXPECT_TRUE(contains(frameResourceRegistryHeader, "bindDescriptorSet("));
  EXPECT_TRUE(contains(frameResourceRegistryHeader, "bindSampler("));
  EXPECT_TRUE(contains(frameResourceRegistryHeader, "descriptor{}"));
  EXPECT_TRUE(contains(frameResourceRegistryHeader, "sampler{}"));
  EXPECT_TRUE(
      contains(frameResourceRegistry, "FrameResourceKind::DescriptorSet"));
  EXPECT_TRUE(contains(frameResourceRegistry, "FrameResourceKind::Sampler"));
  EXPECT_TRUE(contains(frameRecorderHeader, "descriptorBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "descriptorSet("));
  EXPECT_TRUE(contains(frameRecorderHeader, "samplerBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "sampler("));
  EXPECT_TRUE(contains(frameRecorder, "FrameResourceKind::DescriptorSet"));
  EXPECT_TRUE(contains(frameRecorder, "FrameResourceKind::Sampler"));

  EXPECT_TRUE(
      contains(frameResourceManager, "resourceRegistry_.bindDescriptorSet"));
  EXPECT_TRUE(
      contains(frameResourceManager, "\"frame-lighting-descriptor-set\""));
  EXPECT_TRUE(
      contains(frameResourceManager, "\"post-process-descriptor-set\""));
  EXPECT_TRUE(contains(frameResourceManager, "\"oit-descriptor-set\""));

  EXPECT_TRUE(contains(bridgeHeader, "DeferredRasterDescriptorSetId"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterDescriptorBinding"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterDescriptorSet("));
  EXPECT_FALSE(contains(bridgeHeader, "deferredRasterLegacyDescriptorSet"));
  EXPECT_FALSE(contains(bridgeHeader, "p.runtime.frame"));
  EXPECT_TRUE(contains(bridgeHeader, "p.descriptorSet("));

  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterDescriptorSetId::FrameLighting"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterDescriptorSetId::Scene"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterDescriptorSetId::BimScene"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterDescriptorSetId::Light"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterDescriptorSetId::TiledLighting"));
  EXPECT_TRUE(contains(deferredTechnique,
                       "DeferredRasterDescriptorSetId::PostProcess"));
  EXPECT_TRUE(
      contains(deferredTechnique, "DeferredRasterDescriptorSetId::Oit"));
  EXPECT_TRUE(contains(lightingPassRecorder, "deferredRasterDescriptorSet("));

  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->lightingDescriptorSet"));
  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->postProcessDescriptorSet"));
  EXPECT_FALSE(
      contains(deferredTechnique, "p.runtime.frame->oitDescriptorSet"));
  EXPECT_FALSE(
      contains(lightingPassRecorder, "p.runtime.frame->lightingDescriptorSet"));
  EXPECT_FALSE(contains(deferredTechnique, "p.descriptors.sceneDescriptorSet"));
  EXPECT_FALSE(contains(deferredTechnique, "p.descriptors.lightDescriptorSet"));
  EXPECT_FALSE(contains(deferredTechnique, "p.descriptors.tiledDescriptorSet"));
  EXPECT_FALSE(contains(deferredTechnique, "p.bim.sceneDescriptorSet"));
  EXPECT_FALSE(
      contains(lightingPassRecorder, "p.descriptors.sceneDescriptorSet"));
  EXPECT_FALSE(
      contains(lightingPassRecorder, "p.descriptors.tiledDescriptorSet"));
  EXPECT_FALSE(contains(lightingPassRecorder, "p.bim.sceneDescriptorSet"));
}

TEST(TechniqueRegistryGuardrails,
     ShadowPassUsesRegistryBackedDescriptorBindings) {
  const std::string bridgeHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowResourceBridge.h");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");

  EXPECT_TRUE(contains(bridgeHeader, "ShadowDescriptorSetId"));
  EXPECT_TRUE(contains(bridgeHeader, "shadowDescriptorBinding"));
  EXPECT_TRUE(contains(bridgeHeader, "shadowDescriptorSet("));
  EXPECT_TRUE(contains(bridgeHeader, "p.descriptorSet("));
  EXPECT_TRUE(contains(shadowFramePassRecorder, "ShadowResourceBridge.h"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "ShadowDescriptorSetId::Scene"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "ShadowDescriptorSetId::BimScene"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "ShadowDescriptorSetId::Shadow"));
  EXPECT_TRUE(contains(bridgeHeader, "ShadowDescriptorSetId::LocalShadow"));
  EXPECT_TRUE(contains(bridgeHeader, "\"local-shadow-descriptor-set\""));
  EXPECT_TRUE(
      contains(deferredTechnique, "ShadowDescriptorSetId::LocalShadow"));
  EXPECT_FALSE(contains(shadowFramePassRecorder,
                        "params.descriptors.sceneDescriptorSet"));
  EXPECT_FALSE(contains(shadowFramePassRecorder,
                        "params.descriptors.shadowDescriptorSet"));
  EXPECT_FALSE(
      contains(shadowFramePassRecorder, "params.bim.sceneDescriptorSet"));
  EXPECT_FALSE(contains(shadowFramePassRecorder, "p.bim.sceneDescriptorSet"));
  EXPECT_FALSE(contains(frameRecorderHeader, "FrameDescriptorSets"));
  EXPECT_FALSE(contains(frameRecorderHeader, "sceneDescriptorSet"));
  EXPECT_FALSE(contains(frameRecorderHeader, "shadowDescriptorSet"));
  EXPECT_FALSE(contains(frameRecorderHeader, "localShadowDescriptorSet"));
}

TEST(TechniqueRegistryGuardrails,
     FrameRecordParamsExposesRegistryBackedProductionBindings) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");

  EXPECT_TRUE(contains(frameRecorderHeader, "struct FrameRegistryState"));
  EXPECT_TRUE(contains(frameRecorderHeader,
                       "const FrameResourceRegistry *resourceBindings"));
  EXPECT_TRUE(
      contains(frameRecorderHeader, "const PipelineRegistry *pipelineHandles"));
  EXPECT_TRUE(
      contains(frameRecorderHeader, "const PipelineRegistry *pipelineLayouts"));
  EXPECT_TRUE(contains(frameRecorderHeader, "resourceBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "imageBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "bufferBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "framebufferBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "descriptorBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "samplerBinding("));
  EXPECT_TRUE(contains(frameRecorderHeader, "framebuffer("));
  EXPECT_TRUE(contains(frameRecorderHeader, "descriptorSet("));
  EXPECT_TRUE(contains(frameRecorderHeader, "sampler("));
  EXPECT_TRUE(contains(frameRecorderHeader, "pipelineHandle("));
  EXPECT_TRUE(contains(frameRecorderHeader, "pipelineLayout("));
  EXPECT_FALSE(contains(frameRecorderHeader, "FrameResources"));
  EXPECT_FALSE(contains(frameRecorderHeader, "const FrameResources *frame"));
  EXPECT_FALSE(contains(frameRecorderHeader, "FramePipelineState"));
  EXPECT_FALSE(contains(frameRecorderHeader, "FrameRecordParams::pipeline"));
  EXPECT_FALSE(contains(frameRecorderHeader, "PipelineLayouts layouts{}"));
  EXPECT_FALSE(contains(frameRecorderHeader, "GraphicsPipelines pipelines{}"));
  EXPECT_FALSE(contains(frameRecorderHeader, "FrameRenderPassHandles"));
  EXPECT_FALSE(contains(frameRecorderHeader, "FrameDescriptorSets"));
  EXPECT_FALSE(contains(frameRecorderHeader, "VkSampler gBufferSampler"));
  EXPECT_FALSE(contains(frameRecorder, "runtime.frame->"));
  EXPECT_TRUE(contains(pipelineTypes, "std::shared_ptr<const PipelineRegistry> "
                                      "layoutRegistry"));
  EXPECT_TRUE(
      contains(frameRecorder, "registries.resourceBindings->findBinding"));
  EXPECT_TRUE(contains(frameRecorder, "FrameResourceKind::DescriptorSet"));
  EXPECT_TRUE(
      contains(frameRecorder, "registries.pipelineHandles->pipelineHandle"));
  EXPECT_TRUE(
      contains(frameRecorder, "registries.pipelineLayouts->pipelineLayout"));
  EXPECT_TRUE(contains(rendererFrontend, "p.registries.resourceContracts"));
  EXPECT_TRUE(contains(rendererFrontend, "p.registries.resourceBindings"));
  EXPECT_TRUE(contains(rendererFrontend, "resourceRegistry()"));
  EXPECT_TRUE(contains(rendererFrontend, "p.registries.pipelineRecipes"));
  EXPECT_TRUE(contains(rendererFrontend, "p.registries.pipelineHandles"));
  EXPECT_TRUE(contains(rendererFrontend, "p.registries.pipelineLayouts"));
  EXPECT_TRUE(contains(rendererFrontend, "handleRegistry.get()"));
  EXPECT_TRUE(contains(rendererFrontend, "layoutRegistry.get()"));
  EXPECT_FALSE(contains(rendererFrontend, "p.pipeline.layouts"));
  EXPECT_FALSE(contains(rendererFrontend, "p.pipeline.pipelines"));
  EXPECT_TRUE(
      contains(rendererFrontend, "buildGraphicsPipelineLayoutRegistry"));
}

TEST(TechniqueRegistryGuardrails,
     ForwardOpaquePipelineUsesNeutralExtraRegistryStorage) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(pipelineBuilder, "spv_shaders/forward_opaque.vert.spv"));
  EXPECT_TRUE(contains(pipelineBuilder, "spv_shaders/forward_opaque.frag.spv"));
  EXPECT_TRUE(contains(pipelineBuilder, "forwardOpaqueStages"));
  EXPECT_TRUE(contains(pipelineBuilder, "forwardOpaquePCI = meshPCI"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "forwardOpaquePCI.pDepthStencilState = &transparentDS"));
  EXPECT_FALSE(
      contains(pipelineBuilder,
               "forwardOpaquePCI.pDepthStencilState = &depthPrepassDS"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "forwardOpaquePCI.pColorBlendState = &opaqueBlend"));
  EXPECT_TRUE(contains(pipelineBuilder, "forward_opaque_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "pipelines.extraHandles.push_back"));
  EXPECT_TRUE(contains(pipelineBuilder, "RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(pipelineBuilder, "\"forward-opaque\""));
  EXPECT_TRUE(contains(rendererFrontend, "pipelines.extraHandles"));
  EXPECT_TRUE(contains(rendererFrontend, "destroyPipeline(handle.pipeline)"));
  EXPECT_TRUE(contains(rendererFrontend, "layouts.extraLayouts"));
  EXPECT_TRUE(contains(rendererFrontend, "destroyLayout(layout.layout)"));
  EXPECT_TRUE(contains(rendererFrontend, "layouts.extraLayouts.clear()"));
}
