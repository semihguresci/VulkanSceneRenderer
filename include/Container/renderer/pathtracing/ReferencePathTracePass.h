#pragma once

#include <Container/renderer/pathtracing/ReferencePathLensSettings.h>
#include <Container/renderer/pathtracing/ReferenceGroundSettings.h>
#include <Container/renderer/pathtracing/ReferenceOidnSettings.h>
#include <Container/renderer/raytracing/RayShadowManager.h>
#include <Container/utility/PipelineManager.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace container::renderer {

struct ReferencePathTraceSettings {
  uint32_t samplesPerFrame{1}, sampleLimit{65536}, maxBounces{8};
  uint32_t rouletteDepth{3}, seed{1};
  float rayBias{0.0001f}, maxDistance{1e20f};
  float fireflyClamp{};
  ReferencePathLensSettings lens{};
  ReferenceGroundSettings ground{};
  std::optional<uint32_t> resetSamplesPerFrame;
};
struct alignas(16) ReferencePathTraceData {
  glm::mat4 inverseProjection{1}, inverseView{1};
  glm::vec4 sectionPlane{};
  gpu::SceneClipState boxClip{};
  glm::uvec4 limits{}; // instances, triangles, materials, section/ortho/cube/unsupported/volume flags
  glm::uvec4 extentSamples{}; // width, height, first sample, sample count
  glm::uvec4 sampling{}; // bounces, roulette depth, seed, emissive instance count
  glm::vec4 environmentRadiance{1};
  glm::vec4 trace{}; // bias, maximum secondary distance, aperture radius, focal plane distance
  glm::vec4 controls{}; // firefly peak RGB limit, one-shot autofocus probe, reserved
  glm::vec4 ground{}; // mode, Y height, metallic, roughness
  glm::vec4 groundColor{};
};
static_assert(sizeof(ReferencePathTraceData)==384);
struct ReferencePathTraceInput {
  ReferencePathTraceData data{};
  ReferencePathTraceSettings settings{};
  RaySurfaceGpuBindings scene{};
  VkDescriptorSet materials{}, lights{};
  VkImageView environment{};
  VkSampler environmentSampler{};
  // Includes material/texture contents, lights and environment revisions.
  // Camera, clipping, scene bindings and settings are also compared internally.
  uint64_t contentRevision{};
  std::optional<uint64_t> autofocusKey;
  bool emptyScene{false};
};
struct ReferencePathTraceDiagnostics {
  // Failure-stage bits: accumulation=1, path=2, material=4, hit=8, primary camera ray=16.
  uint32_t unsupportedSamples{}, rejectedSamples{}, acceptedSamples{}, failureStages{};
};
struct ReferenceAutofocusResult {
  uint64_t key{};
  float distance{}; // Zero means the center ray found no valid target.
};

struct ReferenceOidnStatus {
  std::string_view message;
  uint32_t displayedSamples{};
  uint64_t allocatedBytes{};
  std::optional<double> lastGpuMs;
};

// Ordered graphics-queue producer. Prepare only a fence-retired input slot;
// record, submit and commit before preparing the next frame. All readers must
// retire before recreation/destruction. Source AS/descriptors remain borrowed
// through submission. Raw moments never contain diagnostic display colors.
class ReferencePathTracePass {
 public:
  ReferencePathTracePass(std::shared_ptr<gpu::VulkanDevice>,gpu::VulkanMemoryManager&,gpu::PipelineManager&);
  ~ReferencePathTracePass();
  ReferencePathTracePass(const ReferencePathTracePass&)=delete;
  ReferencePathTracePass& operator=(const ReferencePathTracePass&)=delete;
  void createResources(const std::filesystem::path&,uint32_t imageCount,VkExtent2D,
                       VkDescriptorSetLayout materials,VkDescriptorSetLayout lights,
                       uint64_t imageBudgetBytes=512ull*1024*1024,
                       ReferenceOidnSettings oidn = {});
  // Optional host views of the same material and ray generations build the
  // mesh-light proposal. They need only survive this call.
  void prepareFrame(uint32_t image, const ReferencePathTraceInput&,
                    std::span<const gpu::GpuMaterial> materialData = {},
                    std::span<const RayInstanceData> instanceData = {});
  void record(VkCommandBuffer,uint32_t image);
  // Only successful graphics submission advances sample addresses/layout state.
  void commitFrame(uint32_t image) noexcept;
  void invalidateHistory() noexcept;
  [[nodiscard]] uint32_t attemptedSamples() const;
  [[nodiscard]] uint32_t lastSubmittedSamples() const;
  [[nodiscard]] std::optional<ReferencePathLensSettings> effectiveLens() const;
  [[nodiscard]] float effectiveFireflyClamp() const;
  [[nodiscard]] ReferenceGroundSettings effectiveGround() const;
  [[nodiscard]] ReferenceOidnStatus oidnStatus() const;
  [[nodiscard]] ReferenceOidnQuality oidnQuality() const;
  void setOidnSampleInterval(uint32_t interval);
  void collectOidnTiming(uint32_t image);
  [[nodiscard]] uint64_t historyResets() const;
  [[nodiscard]] uint64_t allocatedBytes() const;
  // Require this slot's graphics fence to retire before CPU readback.
  [[nodiscard]] ReferencePathTraceDiagnostics diagnostics(uint32_t image) const;
  [[nodiscard]] uint64_t diagnosticEpoch(uint32_t image) const;
  [[nodiscard]] uint64_t diagnosticSubmission(uint32_t image) const;
  // Consume after the probe's submitted graphics work has retired.
  [[nodiscard]] std::optional<ReferenceAutofocusResult> takeAutofocusResult();
  // Images remain GENERAL. A reader changing layout must restore GENERAL.
  [[nodiscard]] VkImage displayImage() const;
  [[nodiscard]] VkImage meanImage() const;
  [[nodiscard]] VkImage momentImage() const;
  [[nodiscard]] VkImageView displayView() const;
  [[nodiscard]] VkImageView meanView() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace container::renderer
