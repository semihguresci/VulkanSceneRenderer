#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/TextureImageData.h"
#include "Container/renderer/lighting/SkySettings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// Manages IBL (Image-Based Lighting) resources and SSAO (GTAO) resources.
// Generates: BRDF LUT, irradiance cubemap, pre-filtered specular cubemap,
// and per-frame GTAO compute passes.
class EnvironmentManager {
 public:
  EnvironmentManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&              pipelineManager,
      VkCommandPool                                  commandPool);

  ~EnvironmentManager();
  EnvironmentManager(const EnvironmentManager&) = delete;
  EnvironmentManager& operator=(const EnvironmentManager&) = delete;

  // Create IBL resources: BRDF LUT, placeholder cubemaps, compute pipelines.
  // Must be called once after construction.
  void createResources(const std::filesystem::path& shaderDir);

  // Load an equirectangular OpenEXR or Radiance HDR environment, convert to a cubemap,
  // and generate the diffuse-irradiance and pre-filtered specular cubemaps.
  // Replaces the placeholder cubemaps created by createResources().
  // Returns true on success. On failure the current cubemaps remain active.
  bool loadHdrEnvironment(const std::filesystem::path& hdrPath);
  // Supply already read bytes for generated environments, so the same decoder
  // and publication path can use an immutable result instead of reopening it.
  bool requestHdrEnvironment(const std::filesystem::path& path,
      std::shared_ptr<const std::vector<std::byte>> encoded = {});
  bool loadSkyEnvironment(const SkySettings& settings);
  bool requestSkyEnvironment(const SkySettings& settings);
  [[nodiscard]] SkyMode activeSkyMode() const { return activeSkyMode_; }
  [[nodiscard]] const AtmosphereSun& activeSun() const { return activeSun_; }
  [[nodiscard]] const AtmosphereSettings& activeAtmosphere() const { return activeAtmosphere_; }
  [[nodiscard]] const AtmosphereLut& activeAtmosphereLut() const { return activeAtmosphereLut_; }
  // Radiance generation identity, independent of recycled Vulkan handles.
  // BRDF-only rebakes do not change the radiometric environment.
  [[nodiscard]] uint64_t environmentRevision() const { return environmentRevision_; }
  // The worker owns only CPU data. Vulkan generation and publication occur
  // here, on the render thread, after the asynchronous decode is ready.
  bool pollEnvironment(bool wait, const std::function<void()>& beforePublish);
  [[nodiscard]] bool environmentLoading() const { return hdrDecode_.valid() || bool(environmentGeneration_); }
  [[nodiscard]] bool environmentGenerating() const { return bool(environmentGeneration_); }
  [[nodiscard]] const std::filesystem::path& activeHdrPath() const { return activeHdrPath_; }
  class PreparedShaderBake {
   public:
    ~PreparedShaderBake();
    PreparedShaderBake(const PreparedShaderBake&) = delete;
    PreparedShaderBake& operator=(const PreparedShaderBake&) = delete;
   private:
    friend class EnvironmentManager;
    struct State;
    PreparedShaderBake();
    std::unique_ptr<State> state_;
  };
  [[nodiscard]] std::unique_ptr<PreparedShaderBake> prepareShaderBake(
      const std::filesystem::path& shaders, bool brdf, bool environment);
  [[nodiscard]] bool canPublishShaderBake(const PreparedShaderBake& candidate) const noexcept;
  // The caller retires all readers, then refreshes descriptors before retiring
  // this candidate, which owns the previous textures after publication.
  void publishShaderBake(PreparedShaderBake& candidate) noexcept;

  // Create GTAO pipelines and private resources for each swapchain image.
  void createGtaoResources(const std::filesystem::path& shaderDir,
                           uint32_t fullWidth, uint32_t fullHeight,
                           uint32_t imageCount = 1);

  // Retire all graphics readers before changing dimensions or image count.
  void recreateGtaoTextures(uint32_t fullWidth, uint32_t fullHeight,
                            uint32_t imageCount);

  void destroy();

  // ---- Per-frame GTAO dispatch ----

  // The caller retires the acquired image before updating its descriptors.
  void dispatchGtao(VkCommandBuffer cmd, uint32_t imageIndex,
                    uint32_t fullWidth, uint32_t fullHeight,
                    VkBuffer cameraBuffer, VkDeviceSize cameraBufferSize,
                    VkImageView depthView, VkSampler depthSampler,
                    VkImageView normalView, VkSampler normalSampler) const;

  void dispatchGtaoBlur(VkCommandBuffer cmd, uint32_t imageIndex,
                        VkImageView depthView,
                        VkSampler depthSampler,
                        float cameraNear,
                        float cameraFar,
                        bool orthographicDepth) const;

  // ---- Accessors ----

  [[nodiscard]] bool isReady()             const { return brdfLutView_ != VK_NULL_HANDLE; }
  [[nodiscard]] bool isGtaoReady()         const { return static_cast<bool>(gtaoResources_); }
  [[nodiscard]] bool isAoEnabled()         const { return aoEnabled_; }
  [[nodiscard]] bool usingPlaceholderEnvironment() const {
    return usingPlaceholderEnvironment_;
  }
  [[nodiscard]] const std::string& environmentStatus() const {
    return environmentStatus_;
  }

  // IBL textures for lighting descriptor binding.
  [[nodiscard]] VkImageView   brdfLutView()           const { return brdfLutView_; }
  [[nodiscard]] VkSampler     brdfLutSampler()        const { return brdfLutSampler_; }
  [[nodiscard]] VkImageView   irradianceView()        const { return irradianceCubeView_; }
  [[nodiscard]] VkImageView   radianceView()          const { return radianceCubeView_; }
  [[nodiscard]] VkImageView   prefilteredView()       const { return prefilteredCubeView_; }
  [[nodiscard]] uint32_t      prefilteredMipCount()   const { return prefilteredMipCount_; }
  [[nodiscard]] VkSampler     envSampler()            const { return envSampler_; }

  // AO texture for lighting descriptor binding (blurred result).
  [[nodiscard]] std::span<const VkImageView> aoTextureViews() const;
  [[nodiscard]] VkSampler     aoSampler()             const { return gtaoSampler_; }

  // GTAO settings.
  float& aoRadius()    { return aoRadius_; }
  float& aoIntensity() { return aoIntensity_; }
  uint32_t& aoSampleCount() { return aoSampleCount_; }
  bool& aoEnabled()    { return aoEnabled_; }

 private:
  void createBrdfLut(const std::filesystem::path& shaderDir);
  void createIblPipelines(const std::filesystem::path& shaderDir);
  void createPlaceholderCubemaps();
  void createSamplers();
  void createGtaoPipelines(const std::filesystem::path& shaderDir);
  [[nodiscard]] VkFormat selectGtaoStorageFormat() const;
  void destroyEnvironmentCubemaps();
  bool submitEnvironment(const std::filesystem::path& path,
                            container::material::TextureMipLevel source,
                            const SkySettings* sky = nullptr,
                            std::shared_ptr<const std::vector<std::byte>> encoded = {});
  bool publishEnvironment(bool wait, const std::function<void()>& beforePublish);

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;
  VkCommandPool                                  commandPool_{VK_NULL_HANDLE};
  std::filesystem::path activeHdrPath_;
  SkyMode activeSkyMode_{SkyMode::Hdr};
  AtmosphereSun activeSun_{};
  AtmosphereSettings activeAtmosphere_{};
  AtmosphereLut activeAtmosphereLut_{};
  SkySettings activeSkySettings_{};
  uint64_t publicationRevision_{};
  uint64_t environmentRevision_{};
  std::shared_ptr<const std::vector<std::byte>> activeHdrBytes_;
  std::filesystem::path pendingHdrPath_;
  struct HdrSource {
    container::material::TextureMipLevel image;
    std::shared_ptr<const std::vector<std::byte>> encoded;
  };
  std::future<HdrSource> hdrDecode_;
  struct EnvironmentGeneration;
  std::unique_ptr<EnvironmentGeneration> environmentGeneration_;

  // ---- BRDF LUT ----
  VkImage       brdfLutImage_{VK_NULL_HANDLE};
  VmaAllocation brdfLutAlloc_{nullptr};
  VkImageView   brdfLutView_{VK_NULL_HANDLE};
  VkSampler     brdfLutSampler_{VK_NULL_HANDLE};

  // ---- Environment cubemaps (placeholder white for now) ----
  VkImage radianceCubeImage_{VK_NULL_HANDLE};
  VmaAllocation radianceCubeAlloc_{nullptr};
  VkImageView radianceCubeView_{VK_NULL_HANDLE};
  VkImage       irradianceCubeImage_{VK_NULL_HANDLE};
  VmaAllocation irradianceCubeAlloc_{nullptr};
  VkImageView   irradianceCubeView_{VK_NULL_HANDLE};

  VkImage       prefilteredCubeImage_{VK_NULL_HANDLE};
  VmaAllocation prefilteredCubeAlloc_{nullptr};
  VkImageView   prefilteredCubeView_{VK_NULL_HANDLE};
  uint32_t      prefilteredMipCount_{1};

  VkSampler     envSampler_{VK_NULL_HANDLE};
  VkSampler     equirectSampler_{VK_NULL_HANDLE};

  // ---- BRDF LUT compute pipeline ----
  container::gpu::ComputePipeline brdfLutPipeline_{};
  VkPipelineLayout        brdfLutPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout   brdfLutSetLayout_{VK_NULL_HANDLE};

  // ---- HDR -> IBL compute pipelines ----
  VkDescriptorSetLayout   iblSetLayout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline equirectToCubemapPipeline_{};
  VkPipelineLayout        equirectToCubemapPipelineLayout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline irradiancePipeline_{};
  VkPipelineLayout        irradiancePipelineLayout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline prefilterPipeline_{};
  VkPipelineLayout        prefilterPipelineLayout_{VK_NULL_HANDLE};

  // ---- GTAO ----
  struct GtaoResources;
  std::unique_ptr<GtaoResources> gtaoResources_;

  VkSampler     gtaoSampler_{VK_NULL_HANDLE};
  VkFormat      gtaoFormat_{VK_FORMAT_UNDEFINED};

  // GTAO compute pipelines
  container::gpu::ComputePipeline gtaoPipeline_{};
  VkPipelineLayout      gtaoPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout gtaoSetLayout_{VK_NULL_HANDLE};

  container::gpu::ComputePipeline gtaoBlurPipeline_{};
  VkPipelineLayout      gtaoBlurPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout gtaoBlurSetLayout_{VK_NULL_HANDLE};

  // GTAO settings
  float    aoRadius_{0.5f};
  float    aoIntensity_{1.5f};
  uint32_t aoSampleCount_{16};
  bool     aoEnabled_{true};

  bool        usingPlaceholderEnvironment_{true};
  std::string environmentStatus_{"Environment: not initialized"};
};

}  // namespace container::renderer
