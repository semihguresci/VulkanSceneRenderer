#include "Container/utility/PipelineManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/ShaderModule.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace container::gpu {

struct ComputePipeline::State {
  std::shared_ptr<const void> identity{std::make_shared<const unsigned char>(0)};
  VkDevice device{};
  VkPipeline pipeline{};
  VkPipelineLayout layout{};
  std::filesystem::path shader;
  std::string cacheKey;
  std::vector<char> code;

  void destroy() noexcept {
    if (pipeline)
      destroyOwnedPipeline(device, std::exchange(pipeline, VK_NULL_HANDLE), nullptr);
  }
  ~State() { destroy(); }
};

ComputePipeline::operator VkPipeline() const noexcept {
  return state_ ? state_->pipeline : VK_NULL_HANDLE;
}

std::shared_ptr<const void> ComputePipeline::contentIdentity() const noexcept {
  return state_ && state_->pipeline ? state_->identity : nullptr;
}

PipelineManager::PipelineManager(VkDevice device) : device_(device) {}

PipelineManager::~PipelineManager() { destroyManagedResources(); }

VkDescriptorSetLayout PipelineManager::createDescriptorSetLayout(
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    const std::vector<VkDescriptorBindingFlags>& bindingFlags,
    VkDescriptorSetLayoutCreateFlags flags, const void* next) {
  if (!bindingFlags.empty() && bindingFlags.size() != bindings.size()) {
    throw std::runtime_error(
        "Descriptor binding flags count must match layout binding count");
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
  bindingFlagsInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
  bindingFlagsInfo.pBindingFlags = bindingFlags.data();
  bindingFlagsInfo.pNext = next;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();
  layoutInfo.flags = flags;
  layoutInfo.pNext = bindingFlags.empty() ? next : &bindingFlagsInfo;

  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkResult res =
      createOwnedDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor set layout");
  }

  descriptorSetLayouts_.push_back(setLayout);
  return setLayout;
}

void PipelineManager::destroyDescriptorSetLayout(
    VkDescriptorSetLayout& layout) {
  if (layout == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(descriptorSetLayouts_, layout);
  if (it != descriptorSetLayouts_.end()) {
    descriptorSetLayouts_.erase(it);
  }

  destroyOwnedDescriptorSetLayout(device_, layout, nullptr);
  layout = VK_NULL_HANDLE;
}

VkDescriptorPool PipelineManager::createDescriptorPool(
    const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets,
    VkDescriptorPoolCreateFlags flags) {
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = maxSets;
  poolInfo.flags = flags;

  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkResult res = createOwnedDescriptorPool(device_, &poolInfo, nullptr, &pool);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor pool");
  }

  descriptorPools_.push_back(pool);
  return pool;
}

void PipelineManager::destroyDescriptorPool(VkDescriptorPool& pool) {
  if (pool == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(descriptorPools_, pool);
  if (it != descriptorPools_.end()) {
    descriptorPools_.erase(it);
  }

  destroyOwnedDescriptorPool(device_, pool, nullptr);
  pool = VK_NULL_HANDLE;
}

VkPipelineLayout PipelineManager::createPipelineLayout(
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::vector<VkPushConstantRange>& pushConstantRanges,
    const void* next) {
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
  pipelineLayoutInfo.pSetLayouts = setLayouts.data();
  pipelineLayoutInfo.pushConstantRangeCount =
      static_cast<uint32_t>(pushConstantRanges.size());
  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();
  pipelineLayoutInfo.pNext = next;

  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkResult res =
      createOwnedPipelineLayout(device_, &pipelineLayoutInfo, nullptr, &layout);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create pipeline layout");
  }

  pipelineLayouts_.push_back(layout);
  return layout;
}

void PipelineManager::destroyPipelineLayout(VkPipelineLayout& layout) {
  if (layout == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(pipelineLayouts_, layout);
  if (it != pipelineLayouts_.end()) {
    pipelineLayouts_.erase(it);
  }

  destroyOwnedPipelineLayout(device_, layout, nullptr);
  layout = VK_NULL_HANDLE;
}

VkPipelineCache PipelineManager::getOrCreatePipelineCache(
    const std::string& cacheKey, const VkPipelineCacheCreateInfo* createInfo) {
  if (auto it = pipelineCaches_.find(cacheKey); it != pipelineCaches_.end()) {
    return it->second;
  }

  VkPipelineCacheCreateInfo cacheInfo{};
  cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

  if (createInfo) {
    cacheInfo = *createInfo;
  }

  VkPipelineCache cache = VK_NULL_HANDLE;
  VkResult res = createOwnedPipelineCache(device_, &cacheInfo, nullptr, &cache);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create pipeline cache");
  }

  pipelineCaches_.emplace(cacheKey, cache);
  return cache;
}

VkPipeline PipelineManager::createGraphicsPipeline(
    const RenderingGraphicsPipelineCreateInfo& pipelineInfo,
    const std::string& cacheKey) {
  VkPipelineCache cache = getOrCreatePipelineCache(cacheKey, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  auto renderingInfo = pipelineInfo.renderPass->pipelineInfo();
  auto dynamicInfo = static_cast<const VkGraphicsPipelineCreateInfo&>(pipelineInfo);
  renderingInfo.pNext = dynamicInfo.pNext;
  dynamicInfo.pNext = &renderingInfo;
  dynamicInfo.renderPass = VK_NULL_HANDLE;
  VkResult res = createOwnedGraphicsPipelines(device_, cache, 1, &dynamicInfo,
                                           nullptr, &pipeline);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create graphics pipeline");
  }

  pipelines_.push_back(pipeline);
  return pipeline;
}

ComputePipeline PipelineManager::buildComputePipeline(
    const std::filesystem::path& shader, VkPipelineLayout layout,
    const std::string& cacheKey, std::vector<char> code) {
  ComputePipeline result;
  result.state_ = std::make_shared<ComputePipeline::State>();
  auto& state = *result.state_;
  state.device = device_;
  state.layout = layout;
  state.shader = shader;
  state.cacheKey = cacheKey;
  state.code = std::move(code);
  VkPipelineCache cache = getOrCreatePipelineCache(cacheKey, nullptr);
  VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.layout = layout;
  info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.pName = "computeMain";
  info.stage.module = createShaderModule(device_, state.code);
  const VkResult res = createOwnedComputePipelines(device_, cache, 1, &info,
                                                  nullptr, &state.pipeline);
  destroyOwnedShaderModule(device_, info.stage.module, nullptr);
  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create compute pipeline: " + cacheKey);
  }
  return result;
}

ComputePipeline PipelineManager::createComputePipeline(
    const std::filesystem::path& shader, VkPipelineLayout layout, const std::string& cacheKey) {
  const auto active = activeComputeDirectory_ / "spv_shaders" / shader.filename();
  auto result = buildComputePipeline(shader, layout, cacheKey,
      container::util::readFile(activeComputeDirectory_.empty() ? shader : active));
  std::erase_if(computePipelines_, [](const auto& slot) { return slot.expired(); });
  computePipelines_.push_back(result.state_);
  return result;
}

PipelineManager::ComputeReload PipelineManager::prepareComputeReload(
    const std::filesystem::path& directory) {
  ComputeReload candidate;
  candidate.directory_ = directory;
  for (const auto& slot : computePipelines_) {
    if (auto active = slot.lock()) {
      auto code = container::util::readFile(directory / "spv_shaders" / active->shader.filename());
      if (code != active->code) {
        auto replacement = buildComputePipeline(active->shader, active->layout, active->cacheKey,
                                                std::move(code));
        candidate.changes_.emplace_back(std::move(active), std::move(replacement));
      }
    }
  }
  return candidate;
}

bool PipelineManager::ComputeReload::changesShader(std::string_view filename) const {
  return std::ranges::any_of(changes_, [&](const auto& change) {
    return change.first->shader.filename() == filename;
  });
}

void PipelineManager::publishComputeReload(ComputeReload& candidate) noexcept {
  for (auto& [active, replacement] : candidate.changes_) {
    std::swap(active->pipeline, replacement.state_->pipeline);
    active->code.swap(replacement.state_->code);
    active->identity.swap(replacement.state_->identity);
  }
  activeComputeDirectory_.swap(candidate.directory_);
}

void PipelineManager::destroyPipeline(VkPipeline& pipeline) {
  if (pipeline == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(pipelines_, pipeline);
  if (it != pipelines_.end()) {
    pipelines_.erase(it);
  }

  destroyOwnedPipeline(device_, pipeline, nullptr);
  pipeline = VK_NULL_HANDLE;
}

void PipelineManager::destroyManagedResources() {
  for (const auto& slot : computePipelines_)
    if (auto state = slot.lock())
      state->destroy();
  computePipelines_.clear();
  activeComputeDirectory_.clear();
  for (VkPipeline pipeline : pipelines_) {
    destroyOwnedPipeline(device_, pipeline, nullptr);
  }
  pipelines_.clear();

  for (VkPipelineLayout layout : pipelineLayouts_) {
    destroyOwnedPipelineLayout(device_, layout, nullptr);
  }
  pipelineLayouts_.clear();

  for (VkDescriptorPool pool : descriptorPools_) {
    destroyOwnedDescriptorPool(device_, pool, nullptr);
  }
  descriptorPools_.clear();

  for (VkDescriptorSetLayout layout : descriptorSetLayouts_) {
    destroyOwnedDescriptorSetLayout(device_, layout, nullptr);
  }
  descriptorSetLayouts_.clear();

  for (auto& [_, cache] : pipelineCaches_) {
    destroyOwnedPipelineCache(device_, cache, nullptr);
  }
  pipelineCaches_.clear();
}

}  // namespace container::gpu
