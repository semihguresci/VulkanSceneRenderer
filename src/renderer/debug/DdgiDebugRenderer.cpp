#include "Container/renderer/debug/DdgiDebugRenderer.h"

#include "Container/renderer/lighting/DdgiSettings.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {
struct DebugPushConstants {
  uint32_t mode;
  float radius;
};
static_assert(sizeof(DebugPushConstants) == 8);
} // namespace

struct DdgiDebugRenderer::Impl {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  vk::raii::PipelineLayout layout{nullptr};
  vk::raii::Pipeline pipeline{nullptr};
};

DdgiDebugRenderer::DdgiDebugRenderer(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    const std::filesystem::path &root, VkDescriptorSetLayout postProcess,
    VkDescriptorSetLayout ddgi, VkFormat colorFormat)
    : impl_(std::make_unique<Impl>()) {
  auto &p = *impl_;
  p.device = std::move(device);
  if (!p.device || postProcess == VK_NULL_HANDLE || ddgi == VK_NULL_HANDLE ||
      colorFormat == VK_FORMAT_UNDEFINED)
    throw std::invalid_argument(
        "DDGI debug requires valid rendering resources");
  const auto &vk = p.device->raii();
  const std::array layouts{vk::DescriptorSetLayout(postProcess),
                           vk::DescriptorSetLayout(ddgi)};
  const vk::PushConstantRange push(vk::ShaderStageFlagBits::eVertex |
                                       vk::ShaderStageFlagBits::eFragment,
                                   0, sizeof(DebugPushConstants));
  p.layout = vk::raii::PipelineLayout(
      vk, vk::PipelineLayoutCreateInfo({}, layouts, push));

  auto module = [&](const char *name) {
    auto bytes = container::util::readFile(root / "spv_shaders" / name);
    if (bytes.empty() || bytes.size() % sizeof(uint32_t))
      throw std::runtime_error("invalid DDGI debug SPIR-V");
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return vk::raii::ShaderModule(
        vk, vk::ShaderModuleCreateInfo({}, bytes.size(), words.data()));
  };
  auto vertex = module("ddgi_debug.vert.spv");
  auto fragment = module("ddgi_debug.frag.spv");
  const std::array stages{
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
                                        *vertex, "vertMain"),
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment,
                                        *fragment, "fragMain")};
  const vk::PipelineVertexInputStateCreateInfo vertexInput{};
  const vk::PipelineInputAssemblyStateCreateInfo assembly(
      {}, vk::PrimitiveTopology::eTriangleList);
  const vk::PipelineViewportStateCreateInfo viewport({}, 1, nullptr, 1,
                                                     nullptr);
  const vk::PipelineRasterizationStateCreateInfo raster(
      {}, VK_FALSE, VK_FALSE, vk::PolygonMode::eFill,
      vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise, VK_FALSE,
      0, 0, 0, 1.f);
  const vk::PipelineMultisampleStateCreateInfo samples(
      {}, vk::SampleCountFlagBits::e1);
  const vk::PipelineColorBlendAttachmentState attachment(
      VK_FALSE, vk::BlendFactor::eOne, vk::BlendFactor::eZero,
      vk::BlendOp::eAdd, vk::BlendFactor::eOne, vk::BlendFactor::eZero,
      vk::BlendOp::eAdd,
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
  const vk::PipelineColorBlendStateCreateInfo blend(
      {}, VK_FALSE, vk::LogicOp::eCopy, attachment);
  const std::array dynamicStates{vk::DynamicState::eViewport,
                                 vk::DynamicState::eScissor};
  const vk::PipelineDynamicStateCreateInfo dynamic({}, dynamicStates);
  const auto format = static_cast<vk::Format>(colorFormat);
  const vk::PipelineRenderingCreateInfo rendering({}, format);
  vk::GraphicsPipelineCreateInfo info({}, stages, &vertexInput, &assembly,
                                      nullptr, &viewport, &raster, &samples,
                                      nullptr, &blend, &dynamic, *p.layout);
  info.pNext = &rendering;
  p.pipeline = vk::raii::Pipeline(vk, nullptr, info);
}

DdgiDebugRenderer::~DdgiDebugRenderer() = default;

void DdgiDebugRenderer::record(VkCommandBuffer cmd, VkDescriptorSet postSet,
                               VkDescriptorSet ddgiSet, uint32_t probeCount,
                               DdgiDebugView view, float radius) const {
  const auto mode = static_cast<uint32_t>(view);
  if (cmd == VK_NULL_HANDLE || postSet == VK_NULL_HANDLE ||
      ddgiSet == VK_NULL_HANDLE || probeCount == 0 || mode < 1 || mode > 4 ||
      !std::isfinite(radius) || radius <= 0.f || radius > kMaxDdgiTraceDistance)
    return;
  const auto &p = *impl_;
  const std::array sets{postSet, ddgiSet};
  const DebugPushConstants push{mode, radius};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    static_cast<VkPipeline>(*p.pipeline));
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_cast<VkPipelineLayout>(*p.layout), 0,
                          static_cast<uint32_t>(sets.size()), sets.data(), 0,
                          nullptr);
  vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(*p.layout),
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(push), &push);
  vkCmdDraw(cmd, 6, probeCount, 0, 0);
}
} // namespace container::renderer
