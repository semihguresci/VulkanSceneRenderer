#include <Container/common/CommonGLFW.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanMemoryManager.h>

#include <gtest/gtest.h>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef CONTAINER_EXTENDED_PBR_SHADER_DIR
#define CONTAINER_EXTENDED_PBR_SHADER_DIR "."
#endif
#ifndef CONTAINER_EXTENDED_PBR_REFERENCE_FILE
#define CONTAINER_EXTENDED_PBR_REFERENCE_FILE "tests/fixtures/rendering/extended_pbr_reference.json"
#endif

namespace {
using namespace container::gpu;
using Json = nlohmann::json;
using ReferenceVector = std::array<double, 3>;

struct alignas(16) ContractQuery {
  glm::vec4 view{}, light{}, f0{}, radiance{}, frame{}, textureRG{};
  glm::uvec4 info{};
};

struct alignas(16) ContractResult {
  glm::vec4 direct{}, legacyDirect{}, layered{}, legacyLayered{}, coatOnly{};
  glm::vec4 anisotropyTangent{}, sourceTangent{}, sourceBitangent{};
  glm::vec4 abiFactors{}, abiColor{};
  glm::uvec4 abiIndices{};
  std::array<glm::vec4, 6> abiTransforms{};
  glm::vec4 iblInput{}, iblNormal{}, diffuseWeights{};
};

static_assert(sizeof(GpuMaterial) == 1040);
static_assert(sizeof(ContractQuery) == 112);
static_assert(sizeof(ContractResult) == 320);
static_assert(offsetof(ContractResult, abiIndices) == 160);
static_assert(offsetof(ContractResult, abiTransforms) == 176);
static_assert(offsetof(ContractResult, iblInput) == 272);

glm::vec4 vector4(const Json& value, float w = 0.0f) {
  if (!value.is_array() || (value.size() != 3 && value.size() != 4))
    throw std::runtime_error("Expected three or four fixture vector coordinates");
  return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>(),
          value.size() == 4 ? value.at(3).get<float>() : w};
}

// Own mapped buffers across exceptions as well as successful submissions. The
// descriptor/pipeline RAII locals in run() retire before these buffers do.
class HostStorage {
 public:
  HostStorage(VulkanMemoryManager& manager, size_t size, const void* contents)
      : manager_(manager) {
    buffer_ = manager_.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    try {
      if (!buffer_.allocation_info.pMappedData)
        throw std::runtime_error("Extended-PBR contract buffer was not mapped");
      if (contents) std::memcpy(buffer_.allocation_info.pMappedData, contents, size);
      else std::memset(buffer_.allocation_info.pMappedData, 0xcd, size);
      if (vmaFlushAllocation(manager_.allocator(), buffer_.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Could not flush extended-PBR storage buffer");
    } catch (...) {
      manager_.destroyBuffer(buffer_);
      throw;
    }
  }
  ~HostStorage() { manager_.destroyBuffer(buffer_); }
  HostStorage(const HostStorage&) = delete;
  HostStorage& operator=(const HostStorage&) = delete;
  [[nodiscard]] const AllocatedBuffer& buffer() const { return buffer_; }

 private:
  VulkanMemoryManager& manager_;
  AllocatedBuffer buffer_{};
};

struct Expected {
  std::string name;
  ReferenceVector direct{}, layered{}, coat{}, tangent{}, sourceTangent{}, sourceBitangent{};
  ReferenceVector iblNormal{}, diffuseWeights{};
  float sourceTangentSign{};
  bool active{};
};

class ExtendedPbrGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<ExtendedPbrGpu*>(user)->validationErrors_;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void loadReference() {
    const char* override = std::getenv("CONTAINER_EXTENDED_PBR_REFERENCE_FILE");
    std::ifstream stream(override ? override : CONTAINER_EXTENDED_PBR_REFERENCE_FILE);
    if (!stream) throw std::runtime_error("Could not open independent extended-PBR JSON reference");
    const auto document = Json::parse(stream);
    if (document.at("schemaVersion") != 1 || document.at("materialStrideBytes") != sizeof(GpuMaterial) ||
        document.at("queryStrideBytes") != sizeof(ContractQuery) ||
        document.at("resultStrideBytes") != sizeof(ContractResult))
      throw std::runtime_error("Extended-PBR fixture schema/ABI stride mismatch");
    absoluteTolerance_ = document.at("absoluteTolerance").get<double>();
    relativeTolerance_ = document.at("relativeTolerance").get<double>();
    for (const auto& entry : document.at("cases")) {
      const auto& m = entry.at("material");
      const auto& q = entry.at("query");
      const auto& abi = entry.at("abi");
      GpuMaterial material;
      material.color = vector4(m.at("baseColor"), 1.0f);
      material.metallicRoughness = {m.at("metallic").get<float>(), m.at("roughness").get<float>()};
      material.transmissionFactor = m.at("specularTransmission").get<float>();
      material.clearcoatFactor = m.at("clearcoat").get<float>();
      material.clearcoatRoughnessFactor = m.at("clearcoatRoughness").get<float>();
      material.sheenColorFactor = vector4(m.at("sheenColor"));
      material.sheenRoughnessFactor = m.at("sheenRoughness").get<float>();
      material.anisotropyDiffuseFactors = {m.at("strength").get<float>(),
          m.at("rotation").get<float>(), m.at("diffuseFactor").get<float>(), abi.at("factorsW").get<float>()};
      material.diffuseTransmissionColorFactor = vector4(m.at("transmissionColor"), abi.at("colorW").get<float>());
      material.anisotropyTextureIndex = abi.at("indices").at(0).get<uint32_t>();
      material.diffuseTransmissionTextureIndex = abi.at("indices").at(1).get<uint32_t>();
      material.diffuseTransmissionColorTextureIndex = abi.at("indices").at(2).get<uint32_t>();
      material.extendedPadding = abi.at("padding").get<uint32_t>();
      const auto& rows = abi.at("transforms");
      material.anisotropyTextureTransform = {vector4(rows.at(0)), vector4(rows.at(1))};
      material.diffuseTransmissionTextureTransform = {vector4(rows.at(2)), vector4(rows.at(3))};
      material.diffuseTransmissionColorTextureTransform = {vector4(rows.at(4)), vector4(rows.at(5))};
      ContractQuery query;
      query.view = vector4(q.at("view"));
      query.light = vector4(q.at("light"));
      query.f0 = vector4(q.at("f0"));
      query.radiance = vector4(q.at("radiance"));
      query.frame = {q.at("modelX").get<float>(), q.at("tangentSign").get<float>(),
                     q.at("faceSign").get<float>(), q.at("iblNormalScale").get<float>()};
      query.textureRG = {q.at("textureRG").at(0).get<float>(),
                         q.at("textureRG").at(1).get<float>(), 0.0f, 0.0f};
      query.info.x = static_cast<uint32_t>(materials_.size());
      const auto& e = entry.at("expected");
      Expected expected;
      expected.name = entry.at("name").get<std::string>();
      expected.direct = e.at("direct").get<ReferenceVector>();
      expected.layered = e.at("layered").get<ReferenceVector>();
      expected.coat = e.at("coat").get<ReferenceVector>();
      expected.tangent = e.at("tangent").get<ReferenceVector>();
      expected.sourceTangent = e.at("sourceTangent").get<ReferenceVector>();
      expected.sourceBitangent = e.at("sourceBitangent").get<ReferenceVector>();
      expected.iblNormal = e.at("iblNormal").get<ReferenceVector>();
      expected.diffuseWeights = e.at("diffuseWeights").get<ReferenceVector>();
      expected.sourceTangentSign = e.at("sourceTangentSign").get<float>();
      expected.active = e.at("active").get<bool>();
      materials_.push_back(material);
      queries_.push_back(query);
      expected_.push_back(std::move(expected));
    }
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_EXTENDED_PBR");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_EXTENDED_PBR=1 for extended-PBR Vulkan contracts";
    loadReference();
    ASSERT_GE(materials_.size(), 3u);
    const char* override = std::getenv("CONTAINER_EXTENDED_PBR_SHADER_DIR");
    shaderPath_ = std::filesystem::path(override ? override : CONTAINER_EXTENDED_PBR_SHADER_DIR) /
                  "extended_pbr_contract.comp.spv";
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath_));
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted_ = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(32, 32, "Extended PBR numerical and material ABI contract", nullptr, nullptr);
    ASSERT_NE(window_, nullptr);
    uint32_t count{};
    const auto* extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo create;
    create.enableValidationLayers = true;
    create.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions, extensions + count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    create.next = &validation;
    instance_ = std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_->raii(), debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance_->instance(), window_, nullptr, &handle), VK_SUCCESS);
    surface_ = vk::raii::SurfaceKHR(instance_->raii(), handle);
    DeviceCreateInfo info;
    info.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    info.next = &v12;
    device_ = std::make_shared<VulkanDevice>(*instance_, handle, info);
    memory_ = std::make_unique<VulkanMemoryManager>(instance_->instance(), device_->physicalDevice(), device_->device());
    pool_ = vk::raii::CommandPool(device_->raii(), vk::CommandPoolCreateInfo({},
        device_->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device_->raii().allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*pool_, vk::CommandBufferLevel::ePrimary, 1));
    command_ = std::move(commands.front());
  }

  void TearDown() override {
    if (device_) device_->raii().waitIdle();
    command_.clear();
    pool_.clear();
    memory_.reset();
    device_.reset();
    surface_.clear();
    messenger_.clear();
    instance_.reset();
    if (window_) glfwDestroyWindow(window_);
    if (glfwStarted_) glfwTerminate();
    EXPECT_EQ(validationErrors_.load(), 0u) << "Vulkan validation and synchronization errors";
  }

  std::vector<ContractResult> run() {
    HostStorage materialBuffer(*memory_, materials_.size() * sizeof(GpuMaterial), materials_.data());
    HostStorage queryBuffer(*memory_, queries_.size() * sizeof(ContractQuery), queries_.data());
    HostStorage resultBuffer(*memory_, queries_.size() * sizeof(ContractResult), nullptr);
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout layout(device_->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3)};
    vk::raii::DescriptorPool descriptors(device_->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    const std::array rawLayouts{*layout};
    auto sets = device_->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, rawLayouts));
    const std::array buffers{
        vk::DescriptorBufferInfo(materialBuffer.buffer().buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(queryBuffer.buffer().buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(resultBuffer.buffer().buffer, 0, VK_WHOLE_SIZE)};
    const std::array writes{
        vk::WriteDescriptorSet(*sets[0], 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[0]),
        vk::WriteDescriptorSet(*sets[0], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[1]),
        vk::WriteDescriptorSet(*sets[0], 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[2])};
    device_->raii().updateDescriptorSets(writes, {});
    vk::raii::PipelineLayout pipelineLayout(device_->raii(), vk::PipelineLayoutCreateInfo({}, rawLayouts));
    const auto code = container::util::readFile(shaderPath_);
    vk::raii::ShaderModule module(device_->raii(), vk::ShaderModuleCreateInfo({}, code.size(),
        reinterpret_cast<const uint32_t*>(code.data())));
    vk::raii::Pipeline pipeline(device_->raii(), nullptr, vk::ComputePipelineCreateInfo({},
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain"),
        *pipelineLayout));
    pool_.reset();
    command_.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    const vk::MemoryBarrier upload(vk::AccessFlagBits::eHostWrite,
        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    command_.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
                             {}, upload, {}, {});
    command_.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const std::array rawSets{*sets[0]};
    command_.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, rawSets, {});
    command_.dispatch(static_cast<uint32_t>((queries_.size() + 31) / 32), 1, 1);
    const vk::MemoryBarrier readback(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
    command_.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                             {}, readback, {}, {});
    command_.end();
    const vk::CommandBuffer raw = *command_;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    vk::raii::Queue queue(device_->raii(), device_->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, nullptr);
    queue.waitIdle();
    if (vmaInvalidateAllocation(memory_->allocator(), resultBuffer.buffer().allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Could not invalidate extended-PBR result buffer");
    std::vector<ContractResult> output(queries_.size());
    std::memcpy(output.data(), resultBuffer.buffer().allocation_info.pMappedData,
                output.size() * sizeof(ContractResult));
    return output;
  }

  void expectReference(const glm::vec4& actual, const ReferenceVector& expected,
                       const char* label) const {
    SCOPED_TRACE(label);
    for (int channel = 0; channel < 3; ++channel) {
      ASSERT_TRUE(std::isfinite(actual[channel])) << "Nonfinite channel " << channel;
      ASSERT_TRUE(std::isfinite(expected[channel]));
      const double tolerance = absoluteTolerance_ + relativeTolerance_ * std::abs(expected[channel]);
      EXPECT_NEAR(double(actual[channel]), expected[channel], tolerance) << "Channel " << channel;
    }
  }

  static void expectExact(const glm::vec4& actual, const glm::vec4& expected, int channels = 4) {
    for (int channel = 0; channel < channels; ++channel)
      EXPECT_EQ(std::bit_cast<uint32_t>(actual[channel]), std::bit_cast<uint32_t>(expected[channel]))
          << "Exact float channel " << channel;
  }

  std::vector<GpuMaterial> materials_;
  std::vector<ContractQuery> queries_;
  std::vector<Expected> expected_;
  double absoluteTolerance_{}, relativeTolerance_{};
  std::filesystem::path shaderPath_;
  bool glfwStarted_{};
  GLFWwindow* window_{};
  std::unique_ptr<VulkanInstance> instance_;
  vk::raii::DebugUtilsMessengerEXT messenger_{nullptr};
  vk::raii::SurfaceKHR surface_{nullptr};
  std::shared_ptr<VulkanDevice> device_;
  std::unique_ptr<VulkanMemoryManager> memory_;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer command_{nullptr};
  std::atomic<uint32_t> validationErrors_{};
};

TEST_F(ExtendedPbrGpu, ProductionMaterialArrayStrideAndAllTailFieldsRoundTripExactly) {
  ASSERT_GE(materials_.size(), 3u);
  EXPECT_NE(materials_[0].anisotropyTextureIndex, materials_[1].anisotropyTextureIndex);
  EXPECT_NE(materials_[1].anisotropyTextureIndex, materials_[2].anisotropyTextureIndex);
  EXPECT_NE(materials_[0].anisotropyTextureTransform.row0.x,
            materials_[2].anisotropyTextureTransform.row0.x);
  const auto output = run();
  ASSERT_EQ(output.size(), materials_.size());
  for (size_t index = 0; index < output.size(); ++index) {
    SCOPED_TRACE(expected_[index].name);
    const auto& result = output[index];
    const auto& material = materials_[index];
    expectExact(result.abiFactors, material.anisotropyDiffuseFactors);
    expectExact(result.abiColor, material.diffuseTransmissionColorFactor);
    const glm::uvec4 indices(material.anisotropyTextureIndex, material.diffuseTransmissionTextureIndex,
                             material.diffuseTransmissionColorTextureIndex, material.extendedPadding);
    for (int channel = 0; channel < 4; ++channel) EXPECT_EQ(result.abiIndices[channel], indices[channel]);
    const std::array rows{material.anisotropyTextureTransform.row0, material.anisotropyTextureTransform.row1,
        material.diffuseTransmissionTextureTransform.row0, material.diffuseTransmissionTextureTransform.row1,
        material.diffuseTransmissionColorTextureTransform.row0, material.diffuseTransmissionColorTextureTransform.row1};
    for (size_t row = 0; row < rows.size(); ++row) expectExact(result.abiTransforms[row], rows[row]);
  }
}

TEST_F(ExtendedPbrGpu, ProductionDirectLayersAndSignedFramesMatchIndependentBinary64Oracle) {
  const auto output = run();
  ASSERT_EQ(output.size(), expected_.size());
  for (size_t index = 0; index < output.size(); ++index) {
    SCOPED_TRACE(expected_[index].name);
    const auto& result = output[index];
    const auto& expected = expected_[index];
    expectReference(result.direct, expected.direct, "base direct");
    expectReference(result.layered, expected.layered, "layered direct");
    expectReference(result.coatOnly, expected.coat, "isotropic coat");
    expectReference(result.anisotropyTangent, expected.tangent, "anisotropy tangent");
    expectReference(result.sourceTangent, expected.sourceTangent, "tangent without normal texture");
    expectReference(result.sourceBitangent, expected.sourceBitangent, "bitangent without normal texture");
    expectReference(result.iblNormal, expected.iblNormal, "bent IBL normal");
    expectReference(result.diffuseWeights, expected.diffuseWeights, "diffuse energy weights");
    EXPECT_EQ(result.sourceTangent.w, expected.sourceTangentSign);
    EXPECT_EQ(result.direct.w, expected.active ? 1.0f : 0.0f);
  }
}

TEST_F(ExtendedPbrGpu, DisabledNewFeaturesPreserveExactLegacyBaseLayersAndIblInput) {
  const auto output = run();
  size_t disabledCount{};
  bool checkedExistingTransmission = false;
  bool checkedNonzeroLayers = false;
  for (size_t index = 0; index < output.size(); ++index) {
    if (expected_[index].active) continue;
    SCOPED_TRACE(expected_[index].name);
    ++disabledCount;
    checkedExistingTransmission |= materials_[index].transmissionFactor > 0.0f;
    checkedNonzeroLayers |= materials_[index].clearcoatFactor > 0.0f;
    expectExact(output[index].direct, output[index].legacyDirect, 3);
    expectExact(output[index].layered, output[index].legacyLayered, 3);
    expectExact(output[index].iblNormal, output[index].iblInput);
  }
  EXPECT_GE(disabledCount, 3u);
  EXPECT_TRUE(checkedExistingTransmission);
  EXPECT_TRUE(checkedNonzeroLayers);
}

}  // namespace
