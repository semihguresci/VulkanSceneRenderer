#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace {

glm::vec3 clipToNdc(const glm::vec4 &clip) { return glm::vec3(clip) / clip.w; }

glm::vec2 sceneUvToNdc(const glm::vec2 &uv) {
  return {uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
}

glm::vec2 sceneNdcToUv(const glm::vec2 &ndc) {
  return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
}

glm::vec2 shadowNdcToUv(const glm::vec2 &ndc) { return ndc * 0.5f + 0.5f; }

glm::vec2 sceneNdcToFramebuffer(const glm::vec2 &ndc) {
  return {(ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f};
}

glm::vec2 shadowNdcToFramebuffer(const glm::vec2 &ndc) {
  return (ndc + 1.0f) * 0.5f;
}

float linearizeReverseZPerspectiveDepth(float depth, float nearPlane,
                                        float farPlane) {
  return (nearPlane * farPlane) / (depth * (farPlane - nearPlane) + nearPlane);
}

glm::vec3 applyGlTfNormalScale(glm::vec3 decodedNormal, float scale) {
  decodedNormal.x *= scale;
  decodedNormal.y *= scale;
  return glm::normalize(decodedNormal);
}

float applyOcclusionStrength(float sampledOcclusion, float strength) {
  return 1.0f + (sampledOcclusion - 1.0f) * strength;
}

float signedArea(const std::array<glm::vec2, 3> &triangle) {
  const glm::vec2 ab = triangle[1] - triangle[0];
  const glm::vec2 ac = triangle[2] - triangle[0];
  return ab.x * ac.y - ab.y * ac.x;
}

float slangMatrixAt(const glm::mat4 &matrix, int row, int column) {
  return matrix[column][row];
}

glm::vec4 slangMatrixRow(const glm::mat4 &matrix, int row) {
  return {
      slangMatrixAt(matrix, row, 0),
      slangMatrixAt(matrix, row, 1),
      slangMatrixAt(matrix, row, 2),
      slangMatrixAt(matrix, row, 3),
  };
}

glm::vec4 normalizePlane(glm::vec4 plane) {
  const float lengthSq = glm::dot(glm::vec3(plane), glm::vec3(plane));
  if (lengthSq <= 1e-8f) {
    return plane;
  }
  return plane / std::sqrt(lengthSq);
}

using FrustumPlanes = std::array<glm::vec4, 6>;

FrustumPlanes extractFrustumPlanesUsingSlangRows(const glm::mat4 &matrix) {
  const glm::vec4 row0 = slangMatrixRow(matrix, 0);
  const glm::vec4 row1 = slangMatrixRow(matrix, 1);
  const glm::vec4 row2 = slangMatrixRow(matrix, 2);
  const glm::vec4 row3 = slangMatrixRow(matrix, 3);

  return {{
      normalizePlane(row3 + row0),
      normalizePlane(row3 - row0),
      normalizePlane(row3 + row1),
      normalizePlane(row3 - row1),
      normalizePlane(row2),
      normalizePlane(row3 - row2),
  }};
}

FrustumPlanes
extractFrustumPlanesUsingWrongColumnAccess(const glm::mat4 &matrix) {
  return {{
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 0),
           slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 0),
           slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 0),
           slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 0)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 0),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 0),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 0),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 0)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 1),
           slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 1),
           slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 1),
           slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 1)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 1),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 1),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 1),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 1)}),
      normalizePlane({slangMatrixAt(matrix, 0, 2), slangMatrixAt(matrix, 1, 2),
                      slangMatrixAt(matrix, 2, 2),
                      slangMatrixAt(matrix, 3, 2)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 2),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 2),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 2),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 2)}),
  }};
}

bool pointInsideFrustum(const FrustumPlanes &planes, const glm::vec3 &point) {
  for (const glm::vec4 &plane : planes) {
    if (glm::dot(glm::vec3(plane), point) + plane.w < 0.0f) {
      return false;
    }
  }
  return true;
}

bool sphereInsideFrustum(const FrustumPlanes &planes, const glm::vec3 &center,
                         float radius) {
  for (const glm::vec4 &plane : planes) {
    if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
      return false;
    }
  }
  return true;
}

bool pointInsideClipVolume(const glm::mat4 &viewProj, const glm::vec3 &point) {
  const glm::vec4 clip = viewProj * glm::vec4(point, 1.0f);
  if (clip.w <= 0.0f) {
    return false;
  }

  return clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w &&
         clip.y <= clip.w && clip.z >= 0.0f && clip.z <= clip.w;
}

float rowXyzLength(const glm::mat4 &matrix, int row) {
  const glm::vec4 r = slangMatrixRow(matrix, row);
  return glm::length(glm::vec3(r));
}

bool occlusionCullMayRejectProjectedBounds(float maxExtentPixels) {
  constexpr float kLargeProjectedExtentPixels = 96.0f;
  return maxExtentPixels <= kLargeProjectedExtentPixels;
}

uint32_t resolveObjectIndex(uint32_t pushedObjectIndex, uint32_t instanceID,
                            uint32_t startInstance) {
  return pushedObjectIndex == std::numeric_limits<uint32_t>::max()
             ? instanceID + startInstance
             : pushedObjectIndex;
}

float smoothRangeAttenuation(float distanceSq, float radius) {
  if (!std::isfinite(distanceSq) || distanceSq < 0.0f ||
      !std::isfinite(radius)) {
    return 0.0f;
  }

  if (radius <= 0.0f) {
    return 1.0f;
  }

  const float safeRadius = std::max(radius, 1e-3f);
  const float invRadiusSq = 1.0f / (safeRadius * safeRadius);
  const float normalizedSq = distanceSq * invRadiusSq;
  const float smoothFactor = std::clamp(1.0f - normalizedSq, 0.0f, 1.0f);
  return smoothFactor * smoothFactor;
}

float pointLightAttenuation(float distanceSq, float radius) {
  if (!std::isfinite(distanceSq) || distanceSq < 0.0f) {
    return 0.0f;
  }

  return smoothRangeAttenuation(distanceSq, radius) /
         std::max(distanceSq, 0.01f);
}

constexpr float reverseZShadowCompareVisibility(float compareDepth,
                                                float storedDepth) {
  return compareDepth >= storedDepth ? 1.0f : 0.0f;
}

constexpr float receiverNormalBiasTexels(float normalDotSurfaceToLight,
                                         float minBiasTexels,
                                         float maxBiasTexels) {
  const float safeMin = std::max(minBiasTexels, 0.0f);
  const float safeMax = std::max(maxBiasTexels, safeMin);
  return std::lerp(safeMin, safeMax,
                   std::clamp(1.0f - normalDotSurfaceToLight, 0.0f, 1.0f));
}

float encodeGBufferMaterialMetadata(uint32_t materialIndex, bool thinSurface) {
  return static_cast<float>(materialIndex) + (thinSurface ? 0.5f : 0.0f);
}

uint32_t decodeGBufferMaterialIndex(float encodedMetadata) {
  return static_cast<uint32_t>(std::floor(std::max(encodedMetadata, 0.0f)));
}

bool decodeGBufferThinSurface(float encodedMetadata) {
  return encodedMetadata - std::floor(std::max(encodedMetadata, 0.0f)) >= 0.25f;
}

float sceneLinearLuminance(const glm::vec3 &color) {
  return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 applyManualExposure(const glm::vec3 &color, float exposure) {
  return color * exposure;
}

float resolvePostProcessExposure(uint32_t mode, float exposure,
                                 float minExposure, float maxExposure) {
  if (mode == container::gpu::kExposureModeManual) {
    return exposure;
  }

  return std::clamp(exposure, minExposure, std::max(minExposure, maxExposure));
}

float histogramBinCenterLog2Luminance(uint32_t bin) {
  constexpr float kMinLog2Luminance = -12.0f;
  constexpr float kMaxLog2Luminance = 12.0f;
  constexpr uint32_t kBinCount = 64u;
  const float t =
      (static_cast<float>(bin) - 0.5f) / static_cast<float>(kBinCount - 1u);
  return kMinLog2Luminance + t * (kMaxLog2Luminance - kMinLog2Luminance);
}

float percentileMeteredHistogramLuminance(const std::array<uint32_t, 64> &bins,
                                          float lowPercentile,
                                          float highPercentile) {
  uint64_t nonBlackCount = 0;
  for (uint32_t bin = 1u; bin < bins.size(); ++bin) {
    nonBlackCount += bins[bin];
  }
  if (nonBlackCount == 0u) {
    return std::exp2(-12.0f);
  }

  const double lowerSample = static_cast<double>(nonBlackCount) * lowPercentile;
  const double upperSample =
      static_cast<double>(nonBlackCount) * highPercentile;
  double weighted = 0.0;
  double samples = 0.0;
  uint64_t cumulative = 0;
  for (uint32_t bin = 1u; bin < bins.size(); ++bin) {
    const uint32_t count = bins[bin];
    const double start = static_cast<double>(cumulative);
    cumulative += count;
    const double end = static_cast<double>(cumulative);
    const double included = std::max(0.0, std::min(end, upperSample) -
                                              std::max(start, lowerSample));
    weighted +=
        static_cast<double>(histogramBinCenterLog2Luminance(bin)) * included;
    samples += included;
  }
  return std::exp2(static_cast<float>(weighted / samples));
}

glm::vec3 bloomThresholdFilter(const glm::vec3 &color, float threshold,
                               float knee) {
  const float brightness = std::max({color.r, color.g, color.b});
  float soft = brightness - threshold + knee;
  soft = std::clamp(soft, 0.0f, 2.0f * knee);
  soft = soft * soft / (4.0f * knee + 1e-4f);
  float contribution = std::max(soft, brightness - threshold);
  contribution /= std::max(brightness, 1e-4f);
  return color * std::max(contribution, 0.0f);
}

uint32_t selectShadowCascade(float viewDepth,
                             const std::array<float, 4> &cascadeSplits) {
  uint32_t cascadeIndex = 3u;
  for (uint32_t i = 0u; i < 3u; ++i) {
    if (viewDepth < cascadeSplits[i]) {
      cascadeIndex = i;
      break;
    }
  }
  return cascadeIndex;
}

bool isRepositoryRoot(const std::filesystem::path &candidate) {
  return std::filesystem::exists(candidate / "CMakeLists.txt") &&
         std::filesystem::is_directory(candidate / "include") &&
         std::filesystem::is_directory(candidate / "src") &&
         std::filesystem::is_directory(candidate / "tests");
}

std::filesystem::path findRepositoryRootFrom(std::filesystem::path candidate) {
  candidate = std::filesystem::absolute(candidate);
  while (!candidate.empty()) {
    if (isRepositoryRoot(candidate)) {
      return candidate;
    }

    const std::filesystem::path parent = candidate.parent_path();
    if (parent == candidate) {
      break;
    }
    candidate = parent;
  }

  return {};
}

std::filesystem::path repositoryRoot() {
  const std::filesystem::path fileRoot =
      findRepositoryRootFrom(std::filesystem::absolute(__FILE__).parent_path());
  if (!fileRoot.empty()) {
    return fileRoot;
  }

  const std::filesystem::path workingRoot =
      findRepositoryRootFrom(std::filesystem::current_path());
  if (!workingRoot.empty()) {
    return workingRoot;
  }

  return std::filesystem::absolute(__FILE__).parent_path();
}

std::string readRepoTextFile(const std::filesystem::path &relativePath) {
  const std::filesystem::path path = repositoryRoot() / relativePath;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Unable to open " << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string stripAsciiWhitespace(std::string_view text) {
  std::string stripped;
  stripped.reserve(text.size());
  for (const char c : text) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      stripped.push_back(c);
    }
  }
  return stripped;
}

bool containsIgnoringWhitespace(std::string_view haystack,
                                std::string_view needle) {
  return contains(stripAsciiWhitespace(haystack), stripAsciiWhitespace(needle));
}

size_t countOccurrences(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return 0u;
  }

  size_t count = 0u;
  size_t offset = 0u;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

} // namespace

TEST(RenderingConventionTests, PerspectiveReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const glm::vec3 nearNdc =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests, OrthoReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 2.0f;
  constexpr float kFar = 10.0f;

  const glm::mat4 proj =
      container::math::orthoRH_ReverseZ(-4.0f, 4.0f, -3.0f, 3.0f, kNear, kFar);

  const glm::vec3 nearNdc =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests,
     ReverseZPerspectiveLinearizationRecoversViewDistance) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;
  constexpr float kMid = 12.5f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const float nearDepth =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f)).z;
  const float midDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kMid, 1.0f)).z;
  const float farDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f)).z;

  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(nearDepth, kNear, kFar), kNear,
              1e-5f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(midDepth, kNear, kFar), kMid,
              1e-4f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(farDepth, kNear, kFar), kFar,
              1e-3f);
}

TEST(RenderingConventionTests, GlTfNormalScaleOnlyScalesTangentSpaceXY) {
  const glm::vec3 decodedNormal = glm::normalize(glm::vec3(0.4f, -0.3f, 1.0f));

  const glm::vec3 flattened = applyGlTfNormalScale(decodedNormal, 0.0f);
  const glm::vec3 boosted = applyGlTfNormalScale(decodedNormal, 2.0f);

  EXPECT_NEAR(flattened.x, 0.0f, 1e-6f);
  EXPECT_NEAR(flattened.y, 0.0f, 1e-6f);
  EXPECT_NEAR(flattened.z, 1.0f, 1e-6f);
  EXPECT_GT(std::abs(boosted.x), std::abs(decodedNormal.x));
  EXPECT_GT(std::abs(boosted.y), std::abs(decodedNormal.y));
  EXPECT_LT(boosted.z, decodedNormal.z);
}

TEST(RenderingConventionTests,
     GlTfOcclusionStrengthInterpolatesFromOneToTexture) {
  constexpr float kSampledOcclusion = 0.35f;

  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 0.0f), 1.0f, 1e-6f);
  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 1.0f),
              kSampledOcclusion, 1e-6f);
  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 0.25f), 0.8375f, 1e-6f);
}

TEST(RenderingConventionTests, SceneViewportUvAndNdcRoundTripFlipsY) {
  const glm::vec2 topLeftNdc = sceneUvToNdc({0.0f, 0.0f});
  const glm::vec2 bottomLeftNdc = sceneUvToNdc({0.0f, 1.0f});

  EXPECT_NEAR(topLeftNdc.x, -1.0f, 1e-6f);
  EXPECT_NEAR(topLeftNdc.y, 1.0f, 1e-6f);
  EXPECT_NEAR(bottomLeftNdc.x, -1.0f, 1e-6f);
  EXPECT_NEAR(bottomLeftNdc.y, -1.0f, 1e-6f);

  const glm::vec2 uv = {0.27f, 0.81f};
  const glm::vec2 roundTrip = sceneNdcToUv(sceneUvToNdc(uv));

  EXPECT_NEAR(roundTrip.x, uv.x, 1e-6f);
  EXPECT_NEAR(roundTrip.y, uv.y, 1e-6f);
}

TEST(RenderingConventionTests, ShadowViewportUsesPositiveHeightMapping) {
  const glm::vec2 topLeftUv = shadowNdcToUv({-1.0f, -1.0f});
  const glm::vec2 bottomRightUv = shadowNdcToUv({1.0f, 1.0f});

  EXPECT_NEAR(topLeftUv.x, 0.0f, 1e-6f);
  EXPECT_NEAR(topLeftUv.y, 0.0f, 1e-6f);
  EXPECT_NEAR(bottomRightUv.x, 1.0f, 1e-6f);
  EXPECT_NEAR(bottomRightUv.y, 1.0f, 1e-6f);

  const glm::vec2 shadowUv = shadowNdcToUv({0.25f, 0.75f});
  const glm::vec2 sceneUv = sceneNdcToUv({0.25f, 0.75f});
  EXPECT_GT(shadowUv.y, sceneUv.y);
}

TEST(RenderingConventionTests,
     ShadowDepthPassUsesPositiveViewportAndDynamicReverseZRasterBias) {
  const std::string shadowRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(contains(shadowRecorder, "viewport.height = static_cast<float>"));
  EXPECT_FALSE(
      contains(shadowRecorder, "viewport.height = -static_cast<float>"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      shadowRecorder, "vkCmdSetDepthBias(cmd, inputs.rasterConstantBias, 0.0f, "
                      "inputs.rasterSlopeBias)"));

  EXPECT_TRUE(
      contains(pipelineBuilder,
               "shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.cullMode = VK_CULL_MODE_BACK_BIT"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowNoCullRaster.cullMode = VK_CULL_MODE_NONE"));
  EXPECT_TRUE(
      contains(pipelineBuilder,
               "sdNoCullPCI.pRasterizationState = &shadowNoCullRaster"));
  EXPECT_TRUE(
      contains(pipelineBuilder,
               "localSdNoCullPCI.pRasterizationState = &shadowNoCullRaster"));
  EXPECT_TRUE(contains(pipelineBuilder, "local_shadow_depth_no_cull_pipeline"));
}

TEST(RenderingConventionTests,
     ReverseZShadowCompareKeepsClearDepthLitAndCloserBlockersShadowed) {
  constexpr float kClearDepth = 0.0f;
  constexpr float kReceiverDepth = 0.55f;
  constexpr float kCloserBlockerDepth = 0.80f;
  constexpr float kFartherDepth = 0.30f;

  EXPECT_FLOAT_EQ(reverseZShadowCompareVisibility(kReceiverDepth, kClearDepth),
                  1.0f);
  EXPECT_FLOAT_EQ(
      reverseZShadowCompareVisibility(kReceiverDepth, kCloserBlockerDepth),
      0.0f);
  EXPECT_FLOAT_EQ(
      reverseZShadowCompareVisibility(kReceiverDepth, kFartherDepth), 1.0f);
}

TEST(RenderingConventionTests,
     ReceiverNormalBiasChangesSamplePositionButNotCasterParticipation) {
  constexpr float kTexelSizeWorld = 0.015625f;
  constexpr float kFacingBiasTexels =
      receiverNormalBiasTexels(1.0f, 1.5f, 3.5f);
  constexpr float kGrazingBiasTexels =
      receiverNormalBiasTexels(0.0f, 1.5f, 3.5f);

  EXPECT_NEAR(kFacingBiasTexels * kTexelSizeWorld, 0.0234375f, 1e-7f);
  EXPECT_NEAR(kGrazingBiasTexels * kTexelSizeWorld, 0.0546875f, 1e-7f);
  EXPECT_FLOAT_EQ(reverseZShadowCompareVisibility(0.55f, 0.80f), 0.0f)
      << "Normals may offset receiver sampling but must not remove blocker "
         "depth.";
}

TEST(RenderingConventionTests,
     ShadowShadersUseNormalsOnlyForReceiverBiasAndShading) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const size_t localOffsetStart =
      localShadow.find("float3 OffsetLocalShadowSamplePosition(");
  ASSERT_NE(localOffsetStart, std::string::npos);
  const size_t localOffsetEnd =
      localShadow.find("float SampleLocalShadowLayerPcf", localOffsetStart);
  ASSERT_NE(localOffsetEnd, std::string::npos);
  const std::string localOffsetBody =
      localShadow.substr(localOffsetStart, localOffsetEnd - localOffsetStart);

  EXPECT_TRUE(contains(shadowCommon, "OffsetShadowSamplePosition"));
  EXPECT_TRUE(contains(shadowCommon, "worldPosition + normal * normalBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowNDC.z + bias"));
  EXPECT_TRUE(contains(localShadow, "OffsetLocalShadowSamplePosition"));
  EXPECT_TRUE(contains(localOffsetBody, "return worldPosition +"));
  EXPECT_TRUE(contains(localOffsetBody, "normal *"));
  EXPECT_TRUE(contains(localOffsetBody, "normalBiasTexels"));
  EXPECT_TRUE(contains(localShadow, "shadowNdc.z + bias"));

  EXPECT_FALSE(contains(shadowCommon, "if (NdotL <= 0.0"));
  EXPECT_FALSE(contains(localShadow, "if (NdotL <= 0.0"));
}

TEST(RenderingConventionTests, ShadowPcfRadiusSpansKernelInsteadOfTapStride) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");

  EXPECT_TRUE(contains(shadowCommon, "SHADOW_PCF_KERNEL_RADIUS = 2.0"));
  EXPECT_TRUE(
      contains(shadowCommon, "filterRadiusTexels / SHADOW_PCF_KERNEL_RADIUS"));
  EXPECT_TRUE(contains(shadowCommon, "filterStepTexels * cascadeTexelSize"));

  EXPECT_TRUE(contains(localShadow, "LOCAL_SHADOW_PCF_KERNEL_RADIUS = 1.0"));
  EXPECT_TRUE(contains(localShadow,
                       "filterRadiusTexels / LOCAL_SHADOW_PCF_KERNEL_RADIUS"));
  EXPECT_TRUE(contains(localShadow, "filterStepTexels * invResolution"));
}

TEST(RenderingConventionTests,
     LocalShadowPcfUsesPoissonDiskForWideSoftShadows) {
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");

  EXPECT_TRUE(contains(localShadow, "LOCAL_SHADOW_POISSON_SAMPLE_COUNT = 32u"));
  EXPECT_TRUE(
      contains(localShadow, "LOCAL_SHADOW_PCF_FAST_SAMPLE_COUNT = 12u"));
  EXPECT_TRUE(contains(localShadow, "LocalShadowOuterSampleWeight"));
  EXPECT_TRUE(contains(localShadow, "smoothstep("));
  EXPECT_TRUE(contains(localShadow, "LocalShadowPoissonOffset"));
  EXPECT_TRUE(contains(localShadow, "LocalShadowStableRotation"));
  EXPECT_TRUE(contains(localShadow, "RotateLocalShadowPoissonOffset"));
  EXPECT_TRUE(contains(localShadow, "floor(shadowUv * atlasResolution)"));
  EXPECT_TRUE(contains(localShadow, "outerSampleWeight"));
  EXPECT_TRUE(contains(localShadow, "outerSampleCount"));
  EXPECT_TRUE(contains(localShadow, "ceil(outerSampleWeight *"));
  EXPECT_TRUE(contains(localShadow,
                       "activeSampleCount = min("));
  EXPECT_TRUE(contains(localShadow, "sampleIndex < activeSampleCount"));
  EXPECT_FALSE(contains(localShadow, "filterRadiusTexels <= 1.5"));
  EXPECT_FALSE(contains(localShadow, "for (int y = -3; y <= 3; ++y)"));
  EXPECT_FALSE(contains(localShadow, "for (int x = -3; x <= 3; ++x)"));
}

TEST(RenderingConventionTests, NegativeSceneViewportFlipsFramebufferWinding) {
  const std::array<glm::vec2, 3> ccwNdcTriangle = {{
      {-0.5f, -0.5f},
      {0.5f, -0.5f},
      {0.0f, 0.5f},
  }};

  const float ndcArea = signedArea(ccwNdcTriangle);
  const float sceneFramebufferArea = signedArea({{
      sceneNdcToFramebuffer(ccwNdcTriangle[0]),
      sceneNdcToFramebuffer(ccwNdcTriangle[1]),
      sceneNdcToFramebuffer(ccwNdcTriangle[2]),
  }});
  const float shadowFramebufferArea = signedArea({{
      shadowNdcToFramebuffer(ccwNdcTriangle[0]),
      shadowNdcToFramebuffer(ccwNdcTriangle[1]),
      shadowNdcToFramebuffer(ccwNdcTriangle[2]),
  }});

  EXPECT_GT(ndcArea, 0.0f);
  EXPECT_LT(sceneFramebufferArea, 0.0f);
  EXPECT_GT(shadowFramebufferArea, 0.0f);
}

TEST(RenderingConventionTests, SceneViewportRecordingUsesSharedHelper) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string sceneViewport =
      readRepoTextFile("src/renderer/scene/SceneViewport.cpp");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/deferred/DeferredRasterPostProcess.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string deferredTransformGizmo = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterTransformGizmo.cpp");

  EXPECT_FALSE(contains(frameRecorderHeader, "setViewportAndScissor"));
  EXPECT_FALSE(
      contains(frameRecorder, "void FrameRecorder::setViewportAndScissor"));
  EXPECT_TRUE(contains(lightingPassRecorder, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(deferredPostProcess, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(sceneViewport, "viewport.height = -static_cast<float>"));
  EXPECT_TRUE(contains(sceneViewport, "vkCmdSetViewport"));
  EXPECT_TRUE(contains(sceneViewport, "vkCmdSetScissor"));
}

TEST(RenderingConventionTests, SceneRasterFrontFaceStaysGltfCounterClockwise) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(
      contains(pipelineBuilder,
               "sceneRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
  EXPECT_FALSE(contains(pipelineBuilder,
                        "sceneRaster.frontFace = VK_FRONT_FACE_CLOCKWISE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "makes direct lighting reject visible surfaces"));
  EXPECT_TRUE(
      contains(pipelineBuilder,
               "shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
}

TEST(RenderingConventionTests,
     GraphicsPipelinesExposeRegistryBackedProductionHandles) {
  const std::string pipelineRegistryHeader = readRepoTextFile(
      "include/Container/renderer/pipeline/PipelineRegistry.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineRegistry =
      readRepoTextFile("src/renderer/pipeline/PipelineRegistry.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(
      contains(pipelineRegistryHeader, "struct RegisteredPipelineHandle"));
  EXPECT_TRUE(contains(pipelineRegistryHeader,
                       "registerHandle(RegisteredPipelineHandle"));
  EXPECT_TRUE(
      contains(pipelineTypes, "std::shared_ptr<const PipelineRegistry>"));
  EXPECT_TRUE(contains(pipelineTypes, "handleRegistry"));
  EXPECT_TRUE(contains(pipelineTypes, "buildGraphicsPipelineHandleRegistry"));
  EXPECT_TRUE(contains(pipelineRegistry, "\"gbuffer-front-cull\""));
  EXPECT_TRUE(contains(pipelineRegistry, "\"bim-section-clip-cap-fill\""));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "pipelines.handleRegistry = "
                       "buildGraphicsPipelineHandleRegistry(pipelines)"));
}

TEST(RenderingConventionTests,
     DeferredPipelineConsumersUseFrameRecordPipelineHandleBridge) {
  const std::string bridgeHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterPipelineBridge.h");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string frameState =
      readRepoTextFile("src/renderer/deferred/DeferredRasterFrameState.cpp");

  EXPECT_TRUE(contains(bridgeHeader, "enum class DeferredRasterPipelineId"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterPipelineHandle("));
  EXPECT_TRUE(contains(bridgeHeader, "p.pipelineHandle("));
  EXPECT_TRUE(contains(bridgeHeader, "RenderTechniqueId::DeferredRaster"));
  EXPECT_TRUE(
      contains(bridgeHeader, "enum class DeferredRasterPipelineLayoutId"));
  EXPECT_TRUE(contains(bridgeHeader, "deferredRasterPipelineLayout("));
  EXPECT_TRUE(contains(bridgeHeader,
                       "p.pipelineLayout(RenderTechniqueId::DeferredRaster"));
  EXPECT_FALSE(contains(bridgeHeader, "fallbackDeferredRasterPipelineHandle("));
  EXPECT_FALSE(contains(bridgeHeader, "fallbackDeferredRasterPipelineLayout("));
  EXPECT_FALSE(contains(bridgeHeader, "p.pipeline.pipelines"));
  EXPECT_FALSE(contains(bridgeHeader, "p.pipeline.layouts"));
  EXPECT_FALSE(contains(bridgeHeader, "GraphicsPipelines"));
  EXPECT_FALSE(contains(bridgeHeader, "PipelineLayouts"));
  EXPECT_TRUE(contains(bridgeHeader, "\"scene\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"transparent\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"lighting\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"tiled-lighting\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"shadow\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"post-process\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"wireframe\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"normal-validation\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"surface-normal\""));
  EXPECT_TRUE(contains(bridgeHeader, "\"transform-gizmo\""));

  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterPipelineHandle(p,"));
  EXPECT_TRUE(contains(lightingPassRecorder, "deferredRasterPipelineHandle("));
  EXPECT_TRUE(contains(frameState, "deferredRasterPipelineReady(p,"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterPipelineLayout("));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterPipelineLayoutId::Scene"));
  EXPECT_TRUE(contains(lightingPassRecorder, "deferredRasterPipelineLayout("));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineLayoutId::Lighting"));

  EXPECT_FALSE(contains(deferredRasterTechnique, "p.pipeline.pipelines."));
  EXPECT_FALSE(contains(lightingPassRecorder, "p.pipeline.pipelines."));
  EXPECT_FALSE(contains(frameState, "p.pipeline.pipelines."));
  EXPECT_FALSE(contains(deferredRasterTechnique, "p.pipeline.layouts"));
  EXPECT_FALSE(contains(deferredRasterTechnique, "params.pipeline.layouts"));
  EXPECT_FALSE(contains(lightingPassRecorder, "p.pipeline.layouts"));
  EXPECT_FALSE(contains(lightingPassRecorder, "params.pipeline.layouts"));
}

TEST(RenderingConventionTests, NormalValidationUsesSceneCullVariants) {
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string debugOverlayRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterDebugOverlayRecorder.cpp");
  const std::string debugOverlayPlanner = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterDebugOverlayPlanner.cpp");
  const std::string debugOverlay =
      readRepoTextFile("src/renderer/debug/DebugOverlayRenderer.cpp");
  const std::string normalValidation =
      readRepoTextFile("shaders/normal_validation.slang");

  EXPECT_TRUE(contains(pipelineTypes, "normalValidationFrontCull"));
  EXPECT_TRUE(contains(pipelineTypes, "normalValidationNoCull"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "normal_validation_front_cull_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "normal_validation_no_cull_pipeline"));
  EXPECT_TRUE(contains(lightingPassRecorder, "buildDeferredDebugOverlayPlan"));
  EXPECT_TRUE(
      contains(debugOverlayPlanner, "opaqueWindingFlippedDrawCommands"));
  EXPECT_TRUE(
      contains(debugOverlayPlanner, "normalValidationFrontCullPipeline"));
  EXPECT_TRUE(contains(debugOverlayPlanner, "normalValidationNoCullPipeline"));
  EXPECT_TRUE(contains(debugOverlayPlanner,
                       "kNormalValidationInvertFaceClassification"));
  EXPECT_TRUE(contains(debugOverlayPlanner, "kNormalValidationBothSidesValid"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "recordDeferredDebugOverlayNormalValidationCommands"));
  EXPECT_TRUE(contains(debugOverlayRecorder, "normalValidationFaceFlags"));
  EXPECT_TRUE(contains(debugOverlay, "pc.faceClassificationFlags"));
  EXPECT_TRUE(contains(normalValidation, "SV_IsFrontFace"));
  EXPECT_TRUE(contains(normalValidation, "pc.faceClassificationFlags"));
  EXPECT_TRUE(contains(normalValidation, "kNormalValidationBothSidesValid"));
  EXPECT_FALSE(contains(normalValidation, "dot(faceNormal, viewDir)"));
}

TEST(RenderingConventionTests, DeferredRasterDebugOverlaysUsePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterDebugOverlayPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterDebugOverlayPlanner.cpp");
  const std::string recorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterDebugOverlayRecorder.h");
  const std::string recorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterDebugOverlayRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const size_t lightingPassEnd = lightingPassRecorder.find(
      "recordBimSectionClipCapFramePassCommands", lightingPass);
  ASSERT_NE(lightingPassEnd, std::string::npos);
  const std::string lightingBlock =
      lightingPassRecorder.substr(lightingPass, lightingPassEnd - lightingPass);

  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredRasterDebugOverlayPlanner.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredRasterDebugOverlayRecorder.h"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredDebugOverlayPlan"));
  EXPECT_TRUE(contains(lightingBlock, "deferredDebugOverlayInputs"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlayWireframeFullCommands"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlayObjectNormalCommands"));
  EXPECT_TRUE(
      contains(lightingBlock, "recordDeferredDebugOverlayGeometryCommands"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlayNormalValidationCommands"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlaySurfaceNormalCommands"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlayWireframeOverlayCommands"));
  EXPECT_FALSE(contains(lightingBlock, "pipelineForDeferredDebugOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawWireframeSource"));
  EXPECT_FALSE(contains(lightingBlock, "drawSceneDebugRoute"));
  EXPECT_FALSE(contains(lightingBlock, "drawNormalValidationSource"));
  EXPECT_FALSE(contains(lightingBlock, "drawSurfaceNormalSource"));
  EXPECT_FALSE(contains(lightingBlock, "debugOverlay_.recordNormalValidation"));
  EXPECT_FALSE(contains(lightingBlock, "debugOverlay_.recordSurfaceNormals"));
  EXPECT_FALSE(contains(lightingBlock, "drawWireframeLists"));
  EXPECT_FALSE(contains(lightingBlock, "drawObjectNormalLists"));
  EXPECT_FALSE(contains(lightingBlock, "drawGeometryOverlayScene"));
  EXPECT_FALSE(contains(lightingBlock, "drawNormalValidationLists"));
  EXPECT_FALSE(contains(lightingBlock, "drawSurfaceNormalsScene"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterDebugOverlayPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredDebugOverlayDrawLists"));
  EXPECT_TRUE(contains(planner, "appendWireframeRoutes"));
  EXPECT_TRUE(contains(planner, "appendObjectNormalRoutes"));
  EXPECT_TRUE(contains(planner, "kNormalValidationInvertFaceClassification"));
  EXPECT_TRUE(contains(planner, "kNormalValidationBothSidesValid"));
  EXPECT_EQ(planner.find("vkCmd"), std::string::npos);
  EXPECT_EQ(planner.find("VkPipeline"), std::string::npos);
  EXPECT_EQ(planner.find("FrameRecordParams"), std::string::npos);
  EXPECT_EQ(planner.find("GuiManager"), std::string::npos);
  EXPECT_EQ(planner.find("BimManager"), std::string::npos);
  EXPECT_EQ(planner.find("LightingManager"), std::string::npos);
  EXPECT_EQ(planner.find("DebugOverlayRenderer"), std::string::npos);
  EXPECT_TRUE(contains(recorderHeader, "DeferredDebugOverlayRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "DeferredDebugOverlayPipelineHandles"));
  EXPECT_TRUE(contains(recorder, "pipelineForDeferredDebugOverlay"));
  EXPECT_TRUE(
      contains(recorder, "recordDeferredDebugOverlayWireframeFullCommands"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(contains(recorder, "vkCmdPushConstants"));
  EXPECT_TRUE(contains(recorder, "vkCmdDrawIndexed"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->drawWireframe"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->drawScene"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->recordNormalValidation"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->recordSurfaceNormals"));
  EXPECT_EQ(recorder.find("FrameRecordParams"), std::string::npos);
  EXPECT_EQ(recorder.find("GuiManager"), std::string::npos);
  EXPECT_EQ(recorder.find("BimManager"), std::string::npos);
  EXPECT_EQ(recorder.find("LightingManager"), std::string::npos);
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterDebugOverlayPlanner.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterDebugOverlayRecorder.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_raster_debug_overlay_planner_tests"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_raster_debug_overlay_recorder_tests"));
}

TEST(RenderingConventionTests,
     PrimitiveNoCullPropagatesToShaderDoubleSidedFlag) {
  const std::string sceneController =
      readRepoTextFile("src/renderer/scene/SceneController.cpp");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string depthPrepass =
      readRepoTextFile("shaders/depth_prepass.slang");
  const std::string shadowDepth =
      readRepoTextFile("shaders/shadow_depth.slang");

  EXPECT_TRUE(contains(sceneController, "object.objectInfo.y"));
  EXPECT_TRUE(
      contains(sceneController, "container::gpu::kObjectFlagDoubleSided"));
  EXPECT_TRUE(contains(gbuffer, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(transparent, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(depthPrepass, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(shadowDepth, "material.flags | obj.objectInfo.y"));
}

TEST(RenderingConventionTests, TransparentOitWritesAreDepthTestedEarly) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(pipelineBuilder, "transparentDS = depthPrepassDS"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "transparentDS.depthWriteEnable = VK_FALSE"));
  EXPECT_TRUE(contains(transparent, "[earlydepthstencil]"));
  EXPECT_TRUE(contains(transparent, "InterlockedAdd(nodeCounter[0]"));
  EXPECT_TRUE(contains(transparent,
                       "InterlockedExchange(headPointerImage[pixelCoord]"));
}

TEST(RenderingConventionTests, SamplePickerDiscoversIfc5Examples) {
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string sampleManifest =
      readRepoTextFile("models/sample_model_regressions.json");

  EXPECT_TRUE(
      contains(guiManager, "models/buildingSMART-IFC5-development/examples"));
  EXPECT_TRUE(contains(guiManager, "DiscoverAuxiliarySampleAssets"));
  EXPECT_TRUE(contains(guiManager, "usd::usdgeom::mesh"));
  EXPECT_TRUE(contains(guiManager, "usd::usdgeom::basiscurves"));
  EXPECT_TRUE(contains(guiManager, "points::array"));
  EXPECT_TRUE(contains(guiManager, "points::base64"));
  EXPECT_TRUE(contains(guiManager, "pcd::base64"));
  EXPECT_TRUE(contains(guiManager, "heuristicRenderableIfc5Layer"));
  EXPECT_TRUE(contains(guiManager, "models/buildingSMART-Sample-Test-Files"));
  EXPECT_FALSE(
      contains(guiManager, "extension == \".ifc\") {\n        continue;"));

  EXPECT_TRUE(
      contains(sampleManifest, "ifc5_domestic_hot_water_ifcx_renderable"));
  EXPECT_TRUE(
      contains(sampleManifest, "ifc5_pcert_infra_bridge_ifcx_renderable"));
  EXPECT_TRUE(contains(sampleManifest, "ifc5_railway_simple_ifcx_renderable"));
}

TEST(RenderingConventionTests, SamplePickerPathTooltipsWrapLongPaths) {
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManager, "ShowWrappedTooltip"));
  EXPECT_TRUE(contains(guiManager, "ShowItemTooltip"));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue"));
  EXPECT_TRUE(contains(guiManager, "WrapLongPathForImGui"));
  EXPECT_TRUE(contains(guiManager, "kPathTooltipHardBreakColumns"));
  EXPECT_TRUE(contains(guiManager, "c == '\\\\' || c == '/'"));
  EXPECT_TRUE(contains(guiManager, "viewportMaxWidth"));
  EXPECT_TRUE(contains(guiManager, "std::clamp(desiredWidth"));
  EXPECT_TRUE(contains(guiManager, "ImGui::PushTextWrapPos"));
  EXPECT_TRUE(contains(guiManager, "ImGui::TextUnformatted"));
  EXPECT_TRUE(
      contains(guiManager, "tooltipText = WrapLongPathForImGui(text)"));
  EXPECT_TRUE(contains(guiManager, "ShowItemTooltip(option.path)"));
  EXPECT_TRUE(contains(guiManager, "ShowItemTooltip(option.unavailableReason"));
  EXPECT_TRUE(contains(guiManager, "ImGuiHoveredFlags_AllowWhenDisabled"));
  EXPECT_TRUE(
      contains(guiManager, "statusText = WrapLongPathForImGui(statusMessage_)"));
  EXPECT_TRUE(contains(guiManager,
                       "environmentText = WrapLongPathForImGui("
                       "environmentStatus_)"));
  EXPECT_TRUE(contains(guiManager,
                       "TextWrappedPathValue(\"Source\", "
                       "bimInspection.modelPath)"));
  EXPECT_TRUE(contains(guiManager,
                       "TextWrappedPathValue(\"Topic path\", entry.path)"));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue(\"Snapshot source\""));
  EXPECT_TRUE(contains(guiManager, "selectedSnapshot.bimModelPath"));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue(\"Cache path\""));
  EXPECT_TRUE(contains(guiManager, "ShowItemTooltip(annotationTooltip)"));
  EXPECT_TRUE(contains(guiManager, "ShowWrappedTooltip(\"Drag to rotate."));
  EXPECT_FALSE(contains(guiManager, "ImGui::SetTooltip("));
  EXPECT_FALSE(
      contains(guiManager, "ImGui::SetTooltip(\"%s\", option.path.c_str())"));
  EXPECT_FALSE(contains(
      guiManager, "ImGui::SetTooltip(\"%s\", option.unavailableReason.c_str())"));
  EXPECT_FALSE(contains(guiManager, "ImGui::SetTooltip(\"%s to %s\""));
}

TEST(RenderingConventionTests, PerspectiveViewMatrixIgnoresCameraScale) {
  container::scene::PerspectiveCamera camera;
  camera.setPosition({1.0f, 2.0f, 3.0f});
  camera.setYawPitch(90.0f, -10.0f);

  const glm::mat4 viewAtUnitScale = camera.viewMatrix();
  camera.setScale({500.0f, 500.0f, 500.0f});
  const glm::mat4 viewAtLargeScale = camera.viewMatrix();

  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      EXPECT_FLOAT_EQ(viewAtUnitScale[column][row],
                      viewAtLargeScale[column][row]);
    }
  }
}

TEST(RenderingConventionTests, FrustumPlaneExtractionUsesSlangRowIndexing) {
  const glm::mat4 view = container::math::lookAt(glm::vec3(3.0f, 2.0f, 5.0f),
                                                 glm::vec3(0.0f, 0.0f, 0.0f),
                                                 glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(55.0f), 16.0f / 9.0f, 0.1f, 100.0f);
  const glm::mat4 viewProj = proj * view;

  const FrustumPlanes correctPlanes =
      extractFrustumPlanesUsingSlangRows(viewProj);
  const FrustumPlanes wrongPlanes =
      extractFrustumPlanesUsingWrongColumnAccess(viewProj);

  int correctMismatchCount = 0;
  int wrongMismatchCount = 0;
  for (int x = -8; x <= 8; x += 2) {
    for (int y = -8; y <= 8; y += 2) {
      for (int z = -8; z <= 8; z += 2) {
        const glm::vec3 point(static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(z));
        const bool clipInside = pointInsideClipVolume(viewProj, point);
        if (pointInsideFrustum(correctPlanes, point) != clipInside) {
          ++correctMismatchCount;
        }
        if (pointInsideFrustum(wrongPlanes, point) != clipInside) {
          ++wrongMismatchCount;
        }
      }
    }
  }

  EXPECT_EQ(correctMismatchCount, 0);
  EXPECT_GT(wrongMismatchCount, 0);
}

TEST(RenderingConventionTests,
     FrustumCullUsesDrawFirstInstanceForObjectBounds) {
  struct Bounds {
    glm::vec3 center{};
    float radius{0.0f};
  };
  struct IndirectDraw {
    uint32_t firstInstance{0};
  };

  const FrustumPlanes planes =
      extractFrustumPlanesUsingSlangRows(glm::mat4(1.0f));
  const std::array<Bounds, 2> objectBounds = {{
      {{5.0f, 0.0f, 0.5f}, 0.1f},
      {{0.0f, 0.0f, 0.5f}, 0.1f},
  }};
  const std::array<IndirectDraw, 1> compactedDrawList = {{{1u}}};

  const uint32_t drawListIndex = 0;
  const uint32_t correctObjectIndex =
      compactedDrawList[drawListIndex].firstInstance;

  EXPECT_FALSE(sphereInsideFrustum(planes, objectBounds[drawListIndex].center,
                                   objectBounds[drawListIndex].radius));
  EXPECT_TRUE(sphereInsideFrustum(planes,
                                  objectBounds[correctObjectIndex].center,
                                  objectBounds[correctObjectIndex].radius));
}

TEST(RenderingConventionTests,
     ShaderObjectIndexUsesPushConstantOrBaseInstance) {
  EXPECT_EQ(resolveObjectIndex(17u, 3u, 100u), 17u);
  EXPECT_EQ(resolveObjectIndex(std::numeric_limits<uint32_t>::max(), 42u, 100u),
            142u);
}

TEST(RenderingConventionTests,
     DeferredDirectionalLightingFetchesGpuMaterialData) {
  const container::renderer::GBufferFormats formats =
      container::renderer::GBufferFormats::defaults();
  EXPECT_EQ(formats.material, VK_FORMAT_R32G32B32A32_SFLOAT);
  EXPECT_EQ(formats.specular, VK_FORMAT_R16G16B16A16_SFLOAT);

  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string deferred =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string renderGraph =
      readRepoTextFile("src/renderer/core/RenderGraph.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string directionalRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredDirectionalLightingRecorder.cpp");

  EXPECT_TRUE(contains(materialCommon, "EncodeGBufferMaterialMetadata"));
  EXPECT_TRUE(contains(materialCommon, "DecodeGBufferMaterialIndex"));
  EXPECT_TRUE(contains(materialCommon, "IsThinSurfaceGpuMaterial"));
  EXPECT_TRUE(contains(
      gbuffer, "EncodeGBufferMaterialMetadata(materialIndex, thinSurface)"));
  EXPECT_TRUE(contains(gbuffer, "specular : SV_Target4"));
  EXPECT_TRUE(contains(
      gbuffer, "output.specular = float4(dielectricF0Color, dielectricF0)"));
  EXPECT_TRUE(contains(deferred, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(contains(
      deferred, "[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture"));
  EXPECT_TRUE(contains(
      deferred,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(
      deferred, "uint materialIndex = DecodeGBufferMaterialIndex(material.w)"));
  EXPECT_TRUE(contains(deferred,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(
      contains(deferred, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_FALSE(contains(deferred, "bool thinSurface = material.w > 0.5"));
  EXPECT_TRUE(contains(renderGraph, "\"GBufferSpecular\""));

  const size_t lightingLayoutPos = pipelineBuilder.find(
      "layouts.lighting = pipelineManager_.createPipelineLayout(");
  ASSERT_NE(lightingLayoutPos, std::string::npos);
  const size_t tiledLayoutPos =
      pipelineBuilder.find("layouts.tiledLighting", lightingLayoutPos);
  ASSERT_NE(tiledLayoutPos, std::string::npos);
  const std::string lightingLayoutBlock = pipelineBuilder.substr(
      lightingLayoutPos, tiledLayoutPos - lightingLayoutPos);
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.lighting"));
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.light"));
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.scene"));
  const size_t shadowLayoutPos =
      pipelineBuilder.find("layouts.shadow", tiledLayoutPos);
  ASSERT_NE(shadowLayoutPos, std::string::npos);
  const std::string tiledLayoutBlock =
      pipelineBuilder.substr(tiledLayoutPos, shadowLayoutPos - tiledLayoutPos);
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.lighting"));
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.tiled"));
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.scene"));

  EXPECT_TRUE(contains(directionalRecorder, "std::array<VkDescriptorSet, 3>"));
  EXPECT_TRUE(contains(directionalRecorder, "inputs.descriptorSets.data()"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "recordDeferredDirectionalLightingCommands"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredLightingDescriptorPlanner.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "buildDeferredLightingDescriptorPlan"));
  EXPECT_FALSE(contains(frameRecorder,
                        "std::array<VkDescriptorSet, 3> pointLightingSets"));
  EXPECT_FALSE(
      contains(frameRecorder, "std::array<VkDescriptorSet, 3> tiledSets"));
}

TEST(RenderingConventionTests, DeferredMaterialParityUsesSharedLayeredHelpers) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string pbrDocs = readRepoTextFile("docs/pbr-implementation.md");

  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredPbrF0"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredPbrF0FromDielectricColor"));
  EXPECT_TRUE(contains(brdfCommon, "ApproximateDeferredIridescenceTint"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredClearcoatDirectLight"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredSheenDirectLight"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredLayeredDirectLight"));

  EXPECT_TRUE(contains(directional, "gSpecularTexture"));
  EXPECT_TRUE(
      contains(directional, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(directional, "specularSample.rgb"));
  EXPECT_TRUE(contains(directional, "DecodeDeferredMaterialLayerTerms(materialData"));
  EXPECT_TRUE(contains(directional, "gMaterialLayersTexture.Load"));
  EXPECT_TRUE(contains(directional, "gMaterialSheenTexture.Load"));
  EXPECT_TRUE(contains(directional, "layerTerms.iridescenceThickness"));
  EXPECT_TRUE(contains(directional, "EvaluateDeferredLayeredDirectLight("));

  EXPECT_TRUE(contains(pointLight, "gSpecularTexture"));
  EXPECT_TRUE(
      contains(pointLight, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(pointLight, "specularSample.rgb"));
  EXPECT_TRUE(contains(pointLight, "DecodeDeferredMaterialLayerTerms(materialData"));
  EXPECT_TRUE(contains(pointLight, "gMaterialLayersTexture.Load"));
  EXPECT_TRUE(contains(pointLight, "gMaterialSheenTexture.Load"));
  EXPECT_TRUE(contains(pointLight, "layerTerms.iridescenceThickness"));
  EXPECT_TRUE(contains(pointLight, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(
      pointLight,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(pointLight,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_FALSE(contains(pointLight, "0.0, 1.0, 0.0.xxx, 1.0"));
  EXPECT_TRUE(contains(tiledLighting, "gSpecularTexture"));
  EXPECT_TRUE(
      contains(tiledLighting, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(tiledLighting, "specularSample.rgb"));
  EXPECT_TRUE(contains(tiledLighting, "DecodeDeferredMaterialLayerTerms(materialData"));
  EXPECT_TRUE(contains(tiledLighting, "gMaterialLayersTexture.Load"));
  EXPECT_TRUE(contains(tiledLighting, "gMaterialSheenTexture.Load"));
  EXPECT_TRUE(contains(tiledLighting, "layerTerms.iridescenceThickness"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(
      tiledLighting,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(tiledLighting,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_FALSE(contains(tiledLighting, "0.0, 1.0, 0.0.xxx, 1.0"));

  EXPECT_TRUE(contains(
      pbrDocs,
      "Directional, point, and tiled deferred lighting fetch `GpuMaterial`"));
  EXPECT_FALSE(
      contains(pbrDocs, "point and tiled lights use the compact fallback"));
}

TEST(RenderingConventionTests,
     ForwardRasterUsesDeferredLayeredDirectLightForEveryDirectLight) {
  const std::array<std::string_view, 2> shaderPaths = {{
      "shaders/forward_opaque.slang",
      "shaders/forward_transparent.slang",
  }};

  for (const std::string_view shaderPath : shaderPaths) {
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));

    EXPECT_GE(countOccurrences(shader, "EvaluateDeferredLayeredDirectLight("),
              3u)
        << shaderPath;
    EXPECT_TRUE(contains(shader, "clearcoat, clearcoatRoughness"))
        << shaderPath;
    EXPECT_TRUE(contains(shader, "sheenColor, sheenRoughness")) << shaderPath;
    EXPECT_FALSE(contains(shader, "lighting += EvaluatePbrDirectLight("))
        << shaderPath;
    EXPECT_FALSE(contains(shader,
                          "directionalRadiance * shadowFactor) *\n"
                          "                        clearcoat"))
        << shaderPath;
    EXPECT_FALSE(contains(shader,
                          "irradiance + directionalRadiance * shadowFactor"))
        << shaderPath;
  }
}

TEST(RenderingConventionTests,
     PointLightAttenuationUsesSmoothedInverseSquareFalloff) {
  constexpr float kLargeRadius = 1000.0f;
  const float nearAttenuation = pointLightAttenuation(4.0f, kLargeRadius);
  const float farAttenuation = pointLightAttenuation(16.0f, kLargeRadius);

  EXPECT_TRUE(std::isfinite(pointLightAttenuation(0.0f, kLargeRadius)));
  EXPECT_GT(nearAttenuation, farAttenuation);
  EXPECT_NEAR(nearAttenuation / farAttenuation, 4.0f, 1e-3f);
  EXPECT_NEAR(pointLightAttenuation(0.25f, kLargeRadius) /
                  pointLightAttenuation(1.0f, kLargeRadius),
              4.0f, 1e-3f);
  EXPECT_NEAR(
      pointLightAttenuation(4.0f, container::gpu::kUnboundedPointLightRange) /
          pointLightAttenuation(16.0f,
                                container::gpu::kUnboundedPointLightRange),
      4.0f, 1e-3f);
  EXPECT_FLOAT_EQ(
      pointLightAttenuation(4.0f, std::numeric_limits<float>::quiet_NaN()),
      0.0f);
  EXPECT_FLOAT_EQ(
      pointLightAttenuation(4.0f, std::numeric_limits<float>::infinity()),
      0.0f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(std::numeric_limits<float>::quiet_NaN(),
                                        kLargeRadius),
                  0.0f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(64.0f, 8.0f), 0.0f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(65.0f, 8.0f), 0.0f);
}

TEST(RenderingConventionTests,
     PointLightShaderKeepsSharedAttenuationConvention) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");

  EXPECT_NE(brdfCommon.find(
                "float PointLightAttenuation(float distanceSq, float radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("SmoothRangeAttenuation(distanceSq, radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("if (!isfinite(radius))"), std::string::npos);
  EXPECT_NE(brdfCommon.find("radius <= 0.0"), std::string::npos);
  EXPECT_NE(brdfCommon.find("MIN_POINT_LIGHT_DISTANCE_SQ"), std::string::npos);
  EXPECT_NE(brdfCommon.find(
                "smoothCutoff / max(distanceSq, MIN_POINT_LIGHT_DISTANCE_SQ)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightAttenuation(distanceSq, lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightHasFiniteRange(lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find(
                "pc.colorIntensity.rgb * pc.colorIntensity.a * attenuation"),
            std::string::npos);
}

TEST(RenderingConventionTests,
     PointAndTiledLightsUseScreenSpaceContactVisibilityOnlyAsFallback) {
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string screenSpaceShadow =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const std::string areaLightCommon =
      readRepoTextFile("shaders/area_light_common.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string pushConstantsCommon =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string lightPushConstants = readRepoTextFile(
      "include/Container/renderer/lighting/LightPushConstants.h");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredPointLightingDrawPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingDrawPlanner.cpp");
  const std::string recorder = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string localShadowAllocator = readRepoTextFile(
      "include/Container/renderer/lighting/LocalShadowLayerAllocator.h");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");

  EXPECT_TRUE(
      contains(screenSpaceShadow, "ScreenSpacePointLightContactVisibility"));
  EXPECT_TRUE(contains(screenSpaceShadow, "contact visibility"));
  EXPECT_TRUE(contains(screenSpaceShadow, "not a local shadow map"));
  EXPECT_TRUE(contains(screenSpaceShadow, "TryReconstructWorldPosition"));
  EXPECT_TRUE(contains(screenSpaceShadow, "mul(viewProj"));
  EXPECT_TRUE(contains(screenSpaceShadow, "depthTexture.Load"));
  EXPECT_TRUE(
      contains(screenSpaceShadow,
               "static const uint SCREEN_SPACE_LIGHT_SHADOW_STEPS = 24u"));
  EXPECT_TRUE(
      contains(screenSpaceShadow, "SCREEN_SPACE_LIGHT_SHADOW_MIN_RAY_RADIUS"));
  EXPECT_TRUE(contains(screenSpaceShadow, "all(isfinite(worldPosition))"));
  EXPECT_TRUE(contains(screenSpaceShadow, "all(isfinite(lightPosition))"));
  EXPECT_TRUE(contains(screenSpaceShadow, "TryProjectWorldToScenePixel"));
  EXPECT_TRUE(contains(screenSpaceShadow, "return false"));
  EXPECT_TRUE(contains(screenSpaceShadow, "return 1.0"));
  EXPECT_TRUE(contains(screenSpaceShadow, "return saturate(visibility)"));
  EXPECT_TRUE(contains(localShadow, "LocalPointLightShadowVisibility"));
  EXPECT_TRUE(contains(localShadow, "PointLightHasLocalShadowLayers"));
  EXPECT_TRUE(contains(localShadow, "LocalAreaLightShadowVisibility"));
  EXPECT_TRUE(contains(localShadow, "Texture2DArray<float> localShadowAtlas"));
  EXPECT_TRUE(contains(localShadow, "SamplerComparisonState"));
  EXPECT_TRUE(contains(localShadow, "SampleCmpLevelZero"));
  EXPECT_TRUE(contains(localShadow,
                       "bool LocalShadowMapsEnabled(LightingBuffer lighting"));
  const size_t lightingBufferLocalShadowStart =
      localShadow.find("bool LocalShadowMapsEnabled(LightingBuffer lighting");
  ASSERT_NE(lightingBufferLocalShadowStart, std::string::npos);
  const size_t lightingBufferLocalShadowEnd =
      localShadow.find("uint LocalShadowBaseLayerFromPointLight",
                       lightingBufferLocalShadowStart);
  ASSERT_NE(lightingBufferLocalShadowEnd, std::string::npos);
  const std::string lightingBufferLocalShadowBlock = localShadow.substr(
      lightingBufferLocalShadowStart,
      lightingBufferLocalShadowEnd - lightingBufferLocalShadowStart);
  EXPECT_TRUE(contains(lightingBufferLocalShadowBlock,
                       "LocalShadowMapsEnabled(lighting.localShadowEnabled, "
                       "localShadow)"));
  EXPECT_FALSE(contains(lightingBufferLocalShadowBlock,
                        "lighting.shadowEnabled"));
  EXPECT_TRUE(contains(localShadow, "LocalShadowSoftFilterRadiusTexels"));
  EXPECT_TRUE(contains(localShadow, "localShadow.filterSettings.y"));
  EXPECT_TRUE(contains(localShadow, "layer.params.w"));
  EXPECT_TRUE(contains(localShadow, "sourceRadiusWorld / texelSizeWorld"));
  EXPECT_FALSE(contains(localShadow, "ddx("));
  EXPECT_FALSE(contains(localShadow, "ddy("));
  EXPECT_TRUE(contains(localShadowAllocator, "hasFiniteLocalShadowRange"));
  EXPECT_TRUE(contains(lightingManager, "LocalShadowLayerAllocator.h"));
  EXPECT_TRUE(contains(shadowManager, "hasFiniteLocalShadowRange"));
  EXPECT_TRUE(
      contains(shadowManager, "kLocalShadowSoftFilterRadiusMultiplier"));
  EXPECT_TRUE(contains(shadowManager, "kLocalShadowPointSourceRadiusFraction"));
  EXPECT_TRUE(contains(shadowManager, "maxSoftFilterRadiusTexels"));
  EXPECT_TRUE(contains(shadowManager, "sourceRadius"));
  EXPECT_FALSE(contains(shadowManager, "kLocalShadowFallbackRange"));

  EXPECT_TRUE(contains(pushConstantsCommon, "uint contactVisibilityEnabled"));
  EXPECT_TRUE(contains(pushConstantsCommon, "uint localShadowEnabled"));
  EXPECT_TRUE(
      contains(lightPushConstants, "uint32_t contactVisibilityEnabled"));
  EXPECT_TRUE(contains(lightPushConstants, "uint32_t localShadowEnabled"));
  EXPECT_TRUE(
      contains(lightPushConstants,
               "offsetof(LightPushConstants, contactVisibilityEnabled) == 64"));
  EXPECT_TRUE(
      contains(lightPushConstants,
               "offsetof(LightPushConstants, localShadowEnabled) == 68"));
  EXPECT_TRUE(contains(sceneData, "uint32_t contactVisibilityEnabled"));
  EXPECT_TRUE(contains(sceneData, "uint32_t localShadowEnabled"));
  EXPECT_TRUE(contains(
      sceneData,
      "offsetof(TiledLightingPushConstants, contactVisibilityEnabled) == 20"));
  EXPECT_TRUE(contains(
      sceneData,
      "offsetof(TiledLightingPushConstants, localShadowEnabled) == 24"));
  EXPECT_TRUE(contains(plannerHeader, "uint32_t contactVisibilityEnabled{0}"));
  EXPECT_TRUE(contains(plannerHeader, "uint32_t localShadowEnabled{0}"));
  EXPECT_TRUE(
      contains(planner, "inputs.contactVisibilityEnabled != 0u ? 1u : 0u"));
  EXPECT_TRUE(contains(planner, "inputs.localShadowEnabled != 0u ? 1u : 0u"));
  EXPECT_TRUE(
      contains(planner, "plan.tiledPushConstants.contactVisibilityEnabled"));
  EXPECT_TRUE(contains(planner, "plan.tiledPushConstants.localShadowEnabled"));
  EXPECT_TRUE(contains(lightingPassRecorder, ".contactVisibilityEnabled ="));
  EXPECT_TRUE(contains(lightingPassRecorder, ".localShadowEnabled ="));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "shadowSettings.localContactVisibility"));
  const size_t cpuLocalShadowPredicateStart =
      lightingPassRecorder.find("const uint32_t localShadowEnabled =");
  ASSERT_NE(cpuLocalShadowPredicateStart, std::string::npos);
  const size_t cpuLocalShadowPredicateEnd =
      lightingPassRecorder.find("const DeferredPointLightingDrawPlan",
                                cpuLocalShadowPredicateStart);
  ASSERT_NE(cpuLocalShadowPredicateEnd, std::string::npos);
  const std::string cpuLocalShadowPredicateBlock =
      lightingPassRecorder.substr(cpuLocalShadowPredicateStart,
                                  cpuLocalShadowPredicateEnd -
                                      cpuLocalShadowPredicateStart);
  EXPECT_TRUE(contains(cpuLocalShadowPredicateBlock,
                       "lightingManager->lightingData().localShadowEnabled"));
  EXPECT_TRUE(contains(cpuLocalShadowPredicateBlock,
                       "p.shadows.localShadowData->counts.w != 0u"));
  EXPECT_FALSE(contains(cpuLocalShadowPredicateBlock,
                        "lightingManager->lightingData().shadowEnabled"));
  EXPECT_TRUE(contains(sceneData, "bool localContactVisibility{true}"));
  EXPECT_TRUE(contains(guiManager, "Local contact visibility"));
  EXPECT_TRUE(contains(recorder, "pushConstants.contactVisibilityEnabled"));
  EXPECT_TRUE(contains(recorder, "pushConstants.localShadowEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "bool localShadowAtlas"));
  EXPECT_TRUE(contains(rendererFrontend, "lightingData.localShadowEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "hasLocalShadowAtlasResources"));
  EXPECT_TRUE(
      contains(rendererFrontend, "localShadowDescriptorSet(imageIndex)"));
  EXPECT_TRUE(contains(rendererFrontend, "localShadowUbo(imageIndex)"));
  EXPECT_TRUE(contains(rendererFrontend, "localShadowFramebuffer(layerIndex)"));
  EXPECT_TRUE(contains(rendererFrontend, "RenderPassId::LocalShadowDepth"));
  EXPECT_TRUE(contains(rendererFrontend, "featureReadiness.localShadowAtlas"));
  EXPECT_TRUE(contains(rendererFrontend, "p.shadows.localShadowLayerCount"));
  EXPECT_FALSE(
      contains(rendererFrontend, "p.shadows.localShadowDescriptorSet"));

  EXPECT_TRUE(contains(pointLight,
                       "#include \"screen_space_light_shadow_common.slang\""));
  EXPECT_TRUE(contains(pointLight, "#include \"local_shadow_common.slang\""));
  EXPECT_TRUE(contains(
      pointLight, "[[vk::binding(18, 0)]] ConstantBuffer<LocalShadowBuffer>"));
  EXPECT_TRUE(
      contains(pointLight, "[[vk::binding(19, 0)]] Texture2DArray<float>"));
  EXPECT_TRUE(
      contains(pointLight, "[[vk::binding(20, 0)]] SamplerComparisonState"));
  EXPECT_TRUE(contains(pointLight, "!isfinite(depth)"));
  EXPECT_TRUE(contains(pointLight, "TryReconstructWorldPosition"));
  EXPECT_TRUE(contains(pointLight, "bool useLocalShadowMap"));
  EXPECT_TRUE(
      contains(pointLight, "if (!RayPointSelected(pc.padding2) && "
                           "pc.contactVisibilityEnabled != 0u &&"));
  EXPECT_FALSE(
      contains(pointLight, "else if (pc.contactVisibilityEnabled != 0u)"));
  EXPECT_TRUE(contains(pointLight,
                       "LocalShadowMapsEnabled(pc.localShadowEnabled, "
                       "uLocalShadow)"));
  EXPECT_TRUE(contains(pointLight, "LocalPointLightShadowVisibility("));
  EXPECT_TRUE(contains(pointLight, "ScreenSpacePointLightContactVisibility("));
  EXPECT_TRUE(contains(pointLight, "radiance *= lightVisibility"));

  EXPECT_TRUE(contains(tiledLighting,
                       "#include \"screen_space_light_shadow_common.slang\""));
  EXPECT_TRUE(
      contains(tiledLighting, "#include \"local_shadow_common.slang\""));
  EXPECT_TRUE(
      contains(tiledLighting,
               "[[vk::binding(18, 0)]] ConstantBuffer<LocalShadowBuffer>"));
  EXPECT_TRUE(
      contains(tiledLighting, "[[vk::binding(19, 0)]] Texture2DArray<float>"));
  EXPECT_TRUE(
      contains(tiledLighting, "[[vk::binding(20, 0)]] SamplerComparisonState"));
  EXPECT_TRUE(contains(tiledLighting, "!isfinite(depth)"));
  EXPECT_TRUE(contains(tiledLighting, "TryReconstructWorldPosition"));
  EXPECT_TRUE(contains(tiledLighting, "bool useLocalShadowMap"));
  EXPECT_TRUE(
      contains(tiledLighting, "if (!RayPointSelected(lightIdx) && "
                              "pc.contactVisibilityEnabled != 0u &&"));
  EXPECT_FALSE(
      contains(tiledLighting, "else if (pc.contactVisibilityEnabled != 0u)"));
  EXPECT_TRUE(contains(tiledLighting,
                       "LocalShadowMapsEnabled(pc.localShadowEnabled, "
                       "uLocalShadow)"));
  EXPECT_TRUE(contains(tiledLighting, "LocalPointLightShadowVisibility("));
  EXPECT_TRUE(
      contains(tiledLighting, "ScreenSpacePointLightContactVisibility("));
  EXPECT_TRUE(contains(tiledLighting, "radiance *= lightVisibility"));

  EXPECT_TRUE(contains(areaLightCommon, "raw direct radiance"));
  EXPECT_TRUE(contains(areaLightCommon, "local light-space"));
  EXPECT_TRUE(contains(localShadow, "transparent paths stay atlas-only"));
  EXPECT_FALSE(contains(transparent, "ScreenSpacePointLightContactVisibility"));
  EXPECT_TRUE(contains(transparent, "gSceneDepthTexture"));
  EXPECT_TRUE(contains(transparent, "#include \"local_shadow_common.slang\""));
  EXPECT_TRUE(contains(
      transparent, "[[vk::binding(18, 3)]] ConstantBuffer<LocalShadowBuffer>"));
  EXPECT_TRUE(
      contains(transparent, "[[vk::binding(19, 3)]] Texture2DArray<float>"));
  EXPECT_TRUE(
      contains(transparent, "[[vk::binding(20, 3)]] SamplerComparisonState"));
  EXPECT_TRUE(contains(transparent, "LocalPointLightShadowVisibility("));
  EXPECT_TRUE(contains(transparent, "LocalAreaLightShadowVisibility("));
}

TEST(
    RenderingConventionTests,
    ScreenSpaceLocalLightContactVisibilityRejectsCoplanarReceiverSelfOcclusion) {
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string screenSpaceShadow =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");

  EXPECT_TRUE(contains(screenSpaceShadow, "float3 receiverNormal"));
  EXPECT_TRUE(contains(screenSpaceShadow, "Texture2D<float4> normalTexture"));
  EXPECT_TRUE(contains(screenSpaceShadow, "normalTexture.Load"));
  EXPECT_TRUE(contains(screenSpaceShadow, "receiverPlaneDistance"));
  EXPECT_TRUE(contains(screenSpaceShadow, "coplanarReceiverSurface"));
  EXPECT_TRUE(contains(screenSpaceShadow, "continue"));

  EXPECT_TRUE(contains(pointLight, "geometricNormal, pc.positionRadius.xyz"));
  EXPECT_TRUE(contains(pointLight, "gDepthTexture"));
  EXPECT_TRUE(contains(pointLight, "gNormalTexture"));
  EXPECT_TRUE(
      contains(tiledLighting, "geometricNormal, light.positionRadius.xyz"));
  EXPECT_TRUE(contains(tiledLighting, "gDepthTexture"));
  EXPECT_TRUE(contains(tiledLighting, "gNormalTexture"));
}

TEST(RenderingConventionTests,
     ScreenSpacePointLightContactVisibilityUsesGradedFailOpenHits) {
  const std::string screenSpaceShadow =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");

  const size_t functionStart = screenSpaceShadow.find(
      "float ScreenSpacePointLightContactVisibility");
  ASSERT_NE(functionStart, std::string::npos);
  const size_t functionEnd = screenSpaceShadow.find(
      "float ScreenSpaceDirectionalContactVisibility", functionStart);
  ASSERT_NE(functionEnd, std::string::npos);
  const std::string pointContact =
      screenSpaceShadow.substr(functionStart, functionEnd - functionStart);

  EXPECT_TRUE(contains(screenSpaceShadow,
                       "SCREEN_SPACE_POINT_CONTACT_MIN_VISIBILITY"));
  EXPECT_TRUE(contains(pointContact, "hitStrength"));
  EXPECT_TRUE(contains(pointContact, "hitVisibility"));
  EXPECT_TRUE(contains(pointContact,
                       "SCREEN_SPACE_POINT_CONTACT_MIN_VISIBILITY"));
  EXPECT_FALSE(contains(pointContact, "visibility = 0.0"));
}

TEST(RenderingConventionTests,
     ForwardTransparentAlphaCutoutsUseSharedSmoothedAlphaMask) {
  const std::string shader =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(shader, "#include \"alpha_mask_common.slang\""));
  EXPECT_TRUE(
      contains(shader, "AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, "
                       "vertIn.pos)"));
  EXPECT_FALSE(contains(shader, "baseAlpha < vertIn.alphaCutoff"));
}

TEST(RenderingConventionTests,
     LocalPointContactTracingSkipsNegligibleRadianceAndOccludedLights) {
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");

  auto expectContactGate = [](const std::string &shader,
                              const char *shaderName) {
    EXPECT_TRUE(
        contains(shader, "LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD"))
        << shaderName;
    EXPECT_TRUE(
        contains(shader, "LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD"))
        << shaderName;

    const size_t helperStart =
        shader.find("bool ShouldTracePointLightContactShadow");
    ASSERT_NE(helperStart, std::string::npos) << shaderName;
    const size_t helperEnd = shader.find("[shader(\"fragment\")]", helperStart);
    ASSERT_NE(helperEnd, std::string::npos) << shaderName;
    const std::string helperBlock =
        shader.substr(helperStart, helperEnd - helperStart);
    EXPECT_TRUE(contains(helperBlock, "all(isfinite(radiance))"))
        << shaderName;
    EXPECT_TRUE(contains(helperBlock, "max(radiance, 0.0.xxx)"))
        << shaderName;
    EXPECT_TRUE(contains(helperBlock,
                         "radianceMagnitude > "
                         "LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD"))
        << shaderName;
    EXPECT_TRUE(contains(helperBlock,
                         "lightVisibility > "
                         "LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD"))
        << shaderName;

    const size_t contactCall =
        shader.find("ScreenSpacePointLightContactVisibility(");
    ASSERT_NE(contactCall, std::string::npos) << shaderName;
    const size_t contactGate = shader.rfind(
        "ShouldTracePointLightContactShadow(radiance, lightVisibility)",
        contactCall);
    ASSERT_NE(contactGate, std::string::npos) << shaderName;
    EXPECT_LT(contactGate, contactCall) << shaderName;

    const size_t visibilityScale =
        shader.find("radiance *= lightVisibility", contactCall);
    ASSERT_NE(visibilityScale, std::string::npos) << shaderName;
    EXPECT_LT(contactCall, visibilityScale) << shaderName;
  };

  expectContactGate(pointLight, "point_light.slang");
  expectContactGate(tiledLighting, "tiled_lighting.slang");
}

TEST(RenderingConventionTests,
     ShadowVisibilityUsesGeometricReceiverNormalsForBias) {
  const std::string deferredDirectional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(deferredDirectional, "float3 geometricNormal"));
  EXPECT_TRUE(contains(deferredDirectional,
                       "worldPosition, geometricNormal, "
                       "uLighting.directionalDirection.xyz"));
  EXPECT_TRUE(contains(deferredDirectional, "geometricNormal, normal, V"));

  EXPECT_TRUE(contains(pointLight, "float3 geometricNormal"));
  EXPECT_TRUE(
      contains(pointLight, "lightData, worldPosition, geometricNormal"));
  EXPECT_TRUE(contains(pointLight, "worldPosition, geometricNormal, "
                                   "pc.positionRadius.xyz"));

  EXPECT_TRUE(contains(tiledLighting, "float3 geometricNormal"));
  EXPECT_TRUE(contains(tiledLighting, "light, worldPosition, geometricNormal"));
  EXPECT_TRUE(contains(tiledLighting, "worldPosition, geometricNormal, "
                                      "light.positionRadius.xyz"));

  EXPECT_TRUE(contains(transparent, "surfaceNormal.geometricNormal"));
  EXPECT_TRUE(
      contains(transparent, "vertIn.worldPos, surfaceNormal.geometricNormal"));
  EXPECT_TRUE(contains(
      transparent, "i, light, vertIn.worldPos, surfaceNormal.geometricNormal"));
}

TEST(RenderingConventionTests,
     ShadowedDiffuseLightingKeepsNaturalIndirectFloor) {
  const std::string deferredDirectional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string forwardTransparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_TRUE(contains(deferredDirectional, "ComputeDiffuseAmbientFloor"));
  EXPECT_TRUE(contains(deferredDirectional, "kDiffuseAmbientFloor"));
  EXPECT_TRUE(contains(deferredDirectional,
                       "ambientLighting += ComputeDiffuseAmbientFloor"));
  EXPECT_TRUE(contains(forwardTransparent, "ComputeTransparentAmbientFloor"));
  EXPECT_TRUE(contains(forwardTransparent, "kTransparentAmbientFloor"));
  EXPECT_FALSE(contains(localShadow, "LOCAL_SHADOW_MIN_VISIBILITY"));
  EXPECT_FALSE(contains(localShadow, "ApplyLocalShadowVisibilityFloor"));
  EXPECT_FALSE(contains(shadowCommon, "SHADOW_MIN_VISIBILITY"));
  EXPECT_FALSE(contains(shadowCommon, "ApplyShadowVisibilityFloor"));
}

TEST(RenderingConventionTests,
     EnvironmentIntensityZeroAmbientFloorIsDrivenByBounceIntensity) {
  const std::string deferredDirectional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string forwardOpaque =
      readRepoTextFile("shaders/forward_opaque.slang");
  const std::string forwardTransparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(deferredDirectional, "float bounceIntensity"));
  EXPECT_TRUE(contains(deferredDirectional, "float environmentFloorScale"));
  EXPECT_TRUE(contains(deferredDirectional, "float bounceFloorScale"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      deferredDirectional,
      "float ambientFloorScale = max(environmentFloorScale, bounceFloorScale);"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      deferredDirectional,
      "ComputeDiffuseAmbientFloor("
      "albedo.rgb, occlusion, ssao, environmentIntensity, bounceIntensity);"));

  EXPECT_TRUE(contains(forwardOpaque, "float bounceIntensity"));
  EXPECT_TRUE(contains(forwardOpaque, "float environmentFloorScale"));
  EXPECT_TRUE(contains(forwardOpaque, "float bounceFloorScale"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardOpaque,
      "float ambientFloorScale = max(environmentFloorScale, bounceFloorScale);"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardOpaque,
      "ComputeForwardAmbientFloor("
      "baseColor, occlusion, environmentIntensity, uLighting.bounceIntensity);"));

  EXPECT_TRUE(contains(forwardTransparent, "float bounceIntensity"));
  EXPECT_TRUE(contains(forwardTransparent, "float environmentFloorScale"));
  EXPECT_TRUE(contains(forwardTransparent, "float bounceFloorScale"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardTransparent,
      "float ambientFloorScale = max(environmentFloorScale, bounceFloorScale);"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardTransparent,
      "ComputeTransparentAmbientFloor("
      "baseColor, occlusion, environmentIntensity, uLighting.bounceIntensity);"));
}

TEST(RenderingConventionTests,
     DiffuseBounceLightingIsSharedByAllLitRenderingPaths) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightPushConstants = readRepoTextFile(
      "include/Container/renderer/lighting/LightPushConstants.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string pushConstantsCommon =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string deferredDirectional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string forwardOpaque =
      readRepoTextFile("shaders/forward_opaque.slang");
  const std::string forwardTransparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string pointLightingRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingRecorder.cpp");
  const std::string pointLightingPlannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredPointLightingDrawPlanner.h");
  const std::string pointLightingPlanner = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingDrawPlanner.cpp");

  EXPECT_TRUE(contains(sceneData, "float bounceIntensity"));
  EXPECT_TRUE(contains(sceneData, "float bounceIntensity{1.0f}"));
  EXPECT_TRUE(
      contains(sceneData, "offsetof(LightingData, bounceIntensity) == 64"));
  EXPECT_TRUE(contains(lightPushConstants, "float bounceIntensity{1.0f}"));
  EXPECT_TRUE(contains(lightingStructs, "float bounceIntensity"));
  EXPECT_TRUE(contains(pushConstantsCommon, "float bounceIntensity"));
  EXPECT_TRUE(
      contains(pointLightingPlannerHeader, "float bounceIntensity{1.0f}"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(
      contains(brdfCommon, "kShadowedDiffuseBounceMinReceiverTurn"));
  EXPECT_TRUE(contains(brdfCommon, "kShadowedDiffuseBounceMinVisibility"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      brdfCommon,
      "float indirectReceiverTurn = lerp("
      "receiverTurnsFromLight,"
      "max(receiverTurnsFromLight, kShadowedDiffuseBounceMinReceiverTurn),"
      "1.0 - directVisibility);"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      brdfCommon,
      "float shadowWeight = kShadowedDiffuseBounceMinVisibility +"
      "(1.0 - kShadowedDiffuseBounceMinVisibility) * directVisibility;"));

  EXPECT_TRUE(contains(deferredDirectional, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(contains(deferredDirectional, "uLighting.bounceIntensity"));
  EXPECT_TRUE(contains(pointLight, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(contains(pointLight, "pc.bounceIntensity"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(contains(tiledLighting, "pc.bounceIntensity"));
  EXPECT_TRUE(contains(forwardOpaque, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(contains(forwardOpaque, "uLighting.bounceIntensity"));
  EXPECT_TRUE(contains(forwardTransparent, "EvaluateDiffuseBounceLight"));
  EXPECT_TRUE(contains(forwardTransparent, "uLighting.bounceIntensity"));

  EXPECT_TRUE(contains(guiManager, "Bounce Intensity"));
  EXPECT_TRUE(contains(lightingManager, "lightingData_.bounceIntensity"));
  EXPECT_TRUE(contains(rendererFrontend, "bounceIntensity"));
  EXPECT_TRUE(
      contains(pointLightingRecorder, "pushConstants.bounceIntensity ="));
  EXPECT_TRUE(contains(pointLightingPlanner, "bounceIntensity"));
}

TEST(RenderingConventionTests,
     DiffuseBounceUsesUnshadowedRadianceAndSeparateVisibility) {
  const std::string deferredDirectional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string forwardOpaque =
      readRepoTextFile("shaders/forward_opaque.slang");
  const std::string forwardTransparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(containsIgnoringWhitespace(
      deferredDirectional,
      "EvaluateDiffuseBounceLight("
      "albedo.rgb, metallic, normal, geometricNormal, V, L, lightColor,"
      "shadowFactor, occlusion, bounceIntensity);"));

  EXPECT_TRUE(contains(pointLight, "float3 unshadowedRadiance = radiance;"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      pointLight,
      "EvaluateDiffuseBounceLight("
      "albedo.rgb, metallic, normal, geometricNormal, V, L,"
      "unshadowedRadiance, lightVisibility, saturate(material.z),"
      "pc.bounceIntensity);"));
  EXPECT_TRUE(contains(tiledLighting, "float3 unshadowedRadiance = radiance;"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      tiledLighting,
      "EvaluateDiffuseBounceLight("
      "albedo.rgb, metallic, normal, geometricNormal, V, L,"
      "unshadowedRadiance, lightVisibility, saturate(material.z),"
      "pc.bounceIntensity);"));

  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardOpaque,
      "EvaluateDiffuseBounceLight("
      "baseColor, metallic, surfaceNormal.shadingNormal,"
      "surfaceNormal.geometricNormal, V, directionalL,"
      "directionalRadiance, shadowFactor, occlusion,"
      "uLighting.bounceIntensity);"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardTransparent,
      "EvaluateDiffuseBounceLight("
      "baseColor, metallic, surfaceNormal.shadingNormal,"
      "surfaceNormal.geometricNormal, V, directionalL,"
      "directionalRadiance, shadowFactor, occlusion,"
      "uLighting.bounceIntensity);"));
  EXPECT_FALSE(containsIgnoringWhitespace(
      forwardOpaque,
      "directionalRadiance * shadowFactor, shadowFactor, occlusion,"
      "uLighting.bounceIntensity"));
  EXPECT_FALSE(containsIgnoringWhitespace(
      forwardTransparent,
      "directionalRadiance * shadowFactor, shadowFactor, occlusion,"
      "uLighting.bounceIntensity"));

  EXPECT_TRUE(contains(deferredDirectional,
                       "float3 directRadiance = radiance * lightVisibility;"));
  EXPECT_TRUE(contains(forwardOpaque,
                       "float3 directRadiance = radiance * lightVisibility;"));
  EXPECT_TRUE(contains(
      forwardTransparent,
      "float3 directRadiance = radiance * lightVisibility;"));
}

TEST(RenderingConventionTests, PointLightShadersSupportUnboundedRangeSentinel) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string tileCull =
      readRepoTextFile("shaders/tile_light_cull.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string stencil = readRepoTextFile("shaders/light_stencil.slang");

  EXPECT_TRUE(contains(sceneData, "kUnboundedPointLightRange = 0.0f"));
  EXPECT_TRUE(contains(lightingStructs, "UNBOUNDED_POINT_LIGHT_RANGE = 0.0"));
  EXPECT_TRUE(
      contains(lightingStructs, "bool PointLightHasFiniteRange(float radius)"));
  EXPECT_TRUE(
      contains(lightingStructs, "radius > UNBOUNDED_POINT_LIGHT_RANGE"));
  EXPECT_TRUE(contains(lightingStructs,
                       "bool PointLightHasRenderableIntensity"));
  EXPECT_TRUE(contains(tileCull, "PointLightHasRenderableIntensity"));
  EXPECT_TRUE(contains(tiledLighting, "PointLightHasRenderableIntensity"));
  EXPECT_TRUE(contains(tileCull, "bool unboundedLight"));
  EXPECT_TRUE(contains(tileCull, "lightRange == UNBOUNDED_POINT_LIGHT_RANGE"));
  EXPECT_TRUE(contains(tiledLighting, "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(contains(transparent, "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(
      contains(stencil, "PointLightHasFiniteRange(pc.positionRadius.w)"));
}

TEST(RenderingConventionTests, TileLightCullRejectsInvalidPointLightMetadata) {
  const std::string tileCull =
      readRepoTextFile("shaders/tile_light_cull.slang");

  const size_t metadataGuard =
      tileCull.find("!all(isfinite(lightPos)) || !isfinite(lightRange)");
  const size_t intensityGuard =
      tileCull.find("!PointLightHasRenderableIntensity(uLights[i])");
  const size_t radiusInflation =
      tileCull.find("float lightRadius = lightRange * 1.03");
  const size_t appendLight =
      tileCull.find("InterlockedAdd(sharedLightCount, 1u, idx)");

  ASSERT_NE(intensityGuard, std::string::npos);
  ASSERT_NE(metadataGuard, std::string::npos);
  ASSERT_NE(radiusInflation, std::string::npos);
  ASSERT_NE(appendLight, std::string::npos);
  EXPECT_LT(intensityGuard, metadataGuard);
  EXPECT_LT(metadataGuard, radiusInflation);
  EXPECT_LT(radiusInflation, appendLight);
  EXPECT_TRUE(contains(tileCull, "lightRange < UNBOUNDED_POINT_LIGHT_RANGE"));
  EXPECT_TRUE(contains(tileCull, "lightRange == UNBOUNDED_POINT_LIGHT_RANGE"));
}

TEST(RenderingConventionTests,
     GtaoUsesPointLoadsFiniteProjectionGuardsAndClampedSamples) {
  const std::string gtao = readRepoTextFile("shaders/gtao.slang");

  EXPECT_TRUE(contains(gtao, "static const uint GTAO_MAX_SAMPLE_COUNT"));
  EXPECT_TRUE(
      contains(gtao, "min(max(pc.sampleCount, 1u), GTAO_MAX_SAMPLE_COUNT)"));
  EXPECT_TRUE(contains(gtao, "gDepthTexture.Load"));
  EXPECT_TRUE(contains(gtao, "gNormalTexture.Load"));
  EXPECT_FALSE(contains(gtao, "gDepthTexture.Sample"));
  EXPECT_FALSE(contains(gtao, "gNormalTexture.Sample"));
  EXPECT_TRUE(contains(gtao, "TryReconstructWorldPosition"));
  EXPECT_TRUE(contains(gtao, "!all(isfinite(projected))"));
  EXPECT_TRUE(contains(gtao, "projected.w <= 1e-6"));
}

TEST(RenderingConventionTests, GtaoKernelSamplesInsideRadius) {
  const std::string gtao = readRepoTextFile("shaders/gtao.slang");

  EXPECT_TRUE(contains(gtao, "sampleDistance"));
  EXPECT_TRUE(contains(gtao, "radius * sampleDistance"));
  EXPECT_FALSE(contains(gtao, "sampleDir * radius"));
}

TEST(RenderingConventionTests, GtaoBlurHandlesOrthographicDepth) {
  const std::string blur = readRepoTextFile("shaders/gtao_blur.slang");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string frameRecorder =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string environmentManager =
      readRepoTextFile("src/renderer/lighting/EnvironmentManager.cpp");

  EXPECT_TRUE(contains(sceneData, "uint32_t orthographicDepth"));
  EXPECT_TRUE(contains(frameRecorder, "bool orthographic"));
  EXPECT_TRUE(contains(rendererFrontend, "p.camera.orthographic"));
  EXPECT_TRUE(contains(environmentManager, "pc.orthographicDepth"));
  EXPECT_TRUE(contains(blur, "LinearizeDepthForProjection"));
  EXPECT_TRUE(contains(blur, "pc.orthographicDepth != 0u"));
  EXPECT_TRUE(contains(blur, "lerp(f, n, saturate(depth))"));
}

TEST(RenderingConventionTests, GtaoTextureFormatIsCapabilityChecked) {
  const std::string environmentHeader = readRepoTextFile(
      "include/Container/renderer/lighting/EnvironmentManager.h");
  const std::string environmentManager =
      readRepoTextFile("src/renderer/lighting/EnvironmentManager.cpp");

  EXPECT_TRUE(contains(environmentHeader, "gtaoFormat_"));
  EXPECT_TRUE(contains(environmentManager, "selectGtaoStorageFormat"));
  EXPECT_TRUE(
      contains(environmentManager, "vkGetPhysicalDeviceFormatProperties"));
  EXPECT_TRUE(
      contains(environmentManager, "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT"));
  EXPECT_TRUE(
      contains(environmentManager, "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"));
  EXPECT_TRUE(contains(environmentManager, "ii.format      = gtaoFormat_"));
  EXPECT_TRUE(contains(environmentManager, "vi.format   = gtaoFormat_"));
}

TEST(RenderingConventionTests, SpotLightsSharePointLightDataWithConeFields) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(sceneData, "kLightTypeSpot = 1.0f"));
  EXPECT_TRUE(contains(sceneData, "directionInnerCos"));
  EXPECT_TRUE(contains(sceneData, "coneOuterCosType"));
  EXPECT_TRUE(contains(lightingStructs, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(lightingStructs, "LIGHT_TYPE_SPOT = 1.0"));
  EXPECT_TRUE(contains(sceneManager, "gltfSpotConeCosines"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"spot\""));
  EXPECT_TRUE(
      contains(sceneManager, "authoredPointLights_.push_back(spotLight)"));
  EXPECT_TRUE(
      contains(lightingManager, "authoredLight.coneOuterCosType.y >= 0.5f"));
  EXPECT_TRUE(contains(pointLight, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(tiledLighting, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(transparent, "SpotLightConeAttenuation"));
}

TEST(RenderingConventionTests, AreaLightDataLayoutMatchesShaderContract) {
  using container::gpu::AreaLightData;
  using container::gpu::LightingData;
  using container::gpu::LocalShadowData;
  using container::gpu::LocalShadowLayerData;

  EXPECT_EQ(container::gpu::kMaxAreaLights, 256u);
  EXPECT_FLOAT_EQ(container::gpu::kAreaLightTypeRectangle, 0.0f);
  EXPECT_FLOAT_EQ(container::gpu::kAreaLightTypeDisk, 1.0f);

  EXPECT_EQ(sizeof(AreaLightData), 80u);
  EXPECT_EQ(alignof(AreaLightData), 16u);
  EXPECT_EQ(offsetof(AreaLightData, positionRange), 0u);
  EXPECT_EQ(offsetof(AreaLightData, colorIntensity), 16u);
  EXPECT_EQ(offsetof(AreaLightData, directionType), 32u);
  EXPECT_EQ(offsetof(AreaLightData, tangentHalfSize), 48u);
  EXPECT_EQ(offsetof(AreaLightData, bitangentHalfSize), 64u);

  EXPECT_EQ(sizeof(LightingData), 80u);
  EXPECT_EQ(offsetof(LightingData, areaLightCount), 56u);
  EXPECT_EQ(offsetof(LightingData, localShadowEnabled), 60u);
  EXPECT_EQ(offsetof(LightingData, bounceIntensity), 64u);
  EXPECT_EQ(container::gpu::kShadowMapResolution, 4096u);
  EXPECT_EQ(container::gpu::kLocalShadowMapResolution, 2048u);
  EXPECT_EQ(container::gpu::kMaxShadowedLocalLightLayers, 24u);
  EXPECT_EQ(container::gpu::kLocalShadowAreaRefPackedCount, 64u);
  EXPECT_EQ(sizeof(LocalShadowLayerData), 128u);
  EXPECT_EQ(alignof(LocalShadowLayerData), 16u);
  EXPECT_EQ(offsetof(LocalShadowLayerData, viewProj), 0u);
  EXPECT_EQ(offsetof(LocalShadowLayerData, positionRange), 64u);
  EXPECT_EQ(offsetof(LocalShadowLayerData, directionType), 80u);
  EXPECT_EQ(offsetof(LocalShadowLayerData, meta), 96u);
  EXPECT_EQ(offsetof(LocalShadowLayerData, params), 112u);
  EXPECT_EQ(sizeof(LocalShadowData),
            sizeof(LocalShadowLayerData) *
                    container::gpu::kMaxShadowedLocalLightLayers +
                sizeof(glm::uvec4) *
                    container::gpu::kLocalShadowAreaRefPackedCount +
                sizeof(glm::uvec4) + sizeof(glm::vec4) * 2);
  EXPECT_EQ(alignof(LocalShadowData), 16u);
  EXPECT_EQ(offsetof(LocalShadowData, layers), 0u);
  EXPECT_EQ(offsetof(LocalShadowData, areaLightRefs),
            sizeof(LocalShadowLayerData) *
                container::gpu::kMaxShadowedLocalLightLayers);
  EXPECT_EQ(offsetof(LocalShadowData, counts),
            sizeof(LocalShadowLayerData) *
                    container::gpu::kMaxShadowedLocalLightLayers +
                sizeof(glm::uvec4) *
                    container::gpu::kLocalShadowAreaRefPackedCount);
  EXPECT_EQ(offsetof(LocalShadowData, biasSettings),
            offsetof(LocalShadowData, counts) + sizeof(glm::uvec4));
  EXPECT_EQ(offsetof(LocalShadowData, filterSettings),
            offsetof(LocalShadowData, biasSettings) + sizeof(glm::vec4));

  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  EXPECT_TRUE(contains(lightingStructs, "MAX_AREA_LIGHTS = 256u"));
  EXPECT_TRUE(contains(lightingStructs, "struct AreaLightData"));
  EXPECT_TRUE(contains(lightingStructs, "float4 positionRange"));
  EXPECT_TRUE(contains(lightingStructs, "float4 colorIntensity"));
  EXPECT_TRUE(contains(lightingStructs, "float4 directionType"));
  EXPECT_TRUE(contains(lightingStructs, "float4 tangentHalfSize"));
  EXPECT_TRUE(contains(lightingStructs, "float4 bitangentHalfSize"));
  EXPECT_TRUE(contains(lightingStructs, "uint areaLightCount"));
  EXPECT_TRUE(contains(lightingStructs, "uint localShadowEnabled"));
  EXPECT_TRUE(
      contains(lightingStructs, "MAX_SHADOWED_LOCAL_LIGHT_LAYERS = 24u"));
  EXPECT_TRUE(
      contains(lightingStructs, "LOCAL_SHADOW_AREA_REF_PACKED_COUNT = 64u"));
  EXPECT_TRUE(contains(lightingStructs, "struct LocalShadowLayerData"));
  EXPECT_TRUE(contains(lightingStructs, "struct LocalShadowBuffer"));
  EXPECT_TRUE(contains(lightingStructs, "LocalShadowLayerData layers"));
  EXPECT_TRUE(contains(lightingStructs, "uint4 areaLightRefs"));
  EXPECT_TRUE(contains(lightingStructs, "uint4 counts"));
  EXPECT_TRUE(contains(lightingStructs, "float4 biasSettings"));
  EXPECT_TRUE(contains(lightingStructs, "float4 filterSettings"));
}

TEST(RenderingConventionTests, RectangularAndDiskAreaLightsImportToCpuBuffers) {
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManagerHeader =
      readRepoTextFile("include/Container/renderer/lighting/LightingManager.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");

  EXPECT_TRUE(contains(sceneManagerHeader, "authoredAreaLights() const"));
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<container::gpu::AreaLightData> authoredAreaLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightSpec"));
  EXPECT_TRUE(contains(sceneManager, "\"KHR_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "\"EXT_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "areaLightTypeFromString"));
  EXPECT_TRUE(contains(sceneManager, "type == \"rect\""));
  EXPECT_TRUE(contains(sceneManager, "type == \"disk\""));
  EXPECT_TRUE(
      contains(sceneManager, "authoredAreaLights_.push_back(*areaLight)"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightTangent"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightBitangent"));

  EXPECT_TRUE(contains(lightingManagerHeader, "areaLightsSsbo() const"));
  EXPECT_TRUE(contains(lightingManagerHeader, "appendAuthoredAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "publishAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "uploadAreaLightSsbo"));
  EXPECT_TRUE(contains(lightingManager, "sceneManager_->authoredAreaLights()"));
  EXPECT_TRUE(contains(lightingManager, "lightingData.areaLightCount"));
  EXPECT_TRUE(contains(lightingManager, "areaLightSsbo_"));
  EXPECT_TRUE(
      contains(lightingManager, "sizeof(AreaLightData) * kMaxAreaLights"));
  EXPECT_TRUE(contains(lightingManager, "writeLightDescriptorStorageBuffers"));
  EXPECT_TRUE(contains(lightingManager, "dstBinding = 2"));
  EXPECT_TRUE(contains(lightingManager, "uploadAreaLightSsbo()"));
}

TEST(RenderingConventionTests, AreaLightShadersUseSampledIntegration) {
  const std::string shaderCmake = readRepoTextFile("cmake/Shaders.cmake");
  const std::string areaLightCommon =
      readRepoTextFile("shaders/area_light_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(shaderCmake, "area_light_common.slang"));
  EXPECT_TRUE(contains(shaderCmake, "local_shadow_common.slang"));
  EXPECT_TRUE(
      contains(areaLightCommon, "AREA_LIGHT_INTEGRATION_SAMPLE_COUNT = 25u"));
  EXPECT_TRUE(contains(areaLightCommon, "struct AreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "BuildAreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightRectSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightDiskSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightSampleWeight"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightGauss5Node"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightGauss5Weight"));
  EXPECT_TRUE(contains(localShadow, "LocalAreaShadowSampleCount"));
  EXPECT_TRUE(contains(areaLightCommon, "EvaluateAreaLightSampleRadiance"));
  EXPECT_TRUE(contains(areaLightCommon, "frame.area * sampleWeight"));
  EXPECT_TRUE(
      contains(readRepoTextFile("shaders/lighting_structs.slang"), "AREA_LIGHT_SHADOW_VISIBILITY_SAMPLE_COUNT = 4u"));
  EXPECT_TRUE(contains(localShadow, "LocalAreaShadowSamplePosition"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightSampleContributionWeight"));
  EXPECT_TRUE(contains(localShadow, "LocalAreaLightShadowVisibility"));
  EXPECT_TRUE(contains(localShadow, "PackedLocalShadowAreaRef"));

  EXPECT_TRUE(contains(directional, "#include \"area_light_common.slang\""));
  EXPECT_TRUE(contains(directional, "#include \"local_shadow_common.slang\""));
  EXPECT_TRUE(contains(directional,
                       "sampleIndex < integrationSampleCount"));
  EXPECT_TRUE(contains(directional, "EvaluateAreaLightSampleRadiance"));
  EXPECT_TRUE(
      contains(directional, "LocalShadowMapsEnabled(uLighting, uLocalShadow)"));
  EXPECT_TRUE(contains(directional, "LocalAreaLightShadowVisibility("));
  EXPECT_FALSE(contains(directional, "ClosestPointOnAreaLight"));
  const size_t deferredAreaLightStart =
      directional.find("float3 EvaluateDeferredAreaLight");
  ASSERT_NE(deferredAreaLightStart, std::string::npos);
  const size_t deferredAreaLightSampleLoop =
      directional.find("sampleIndex < integrationSampleCount",
                       deferredAreaLightStart);
  ASSERT_NE(deferredAreaLightSampleLoop, std::string::npos);
  const size_t deferredAreaLightEnd =
      directional.find("[shader(\"fragment\")]", deferredAreaLightStart);
  ASSERT_NE(deferredAreaLightEnd, std::string::npos);
  const size_t deferredAreaLightShadowBeforeSampleLoop =
      directional.find("LocalAreaLightShadowVisibility(",
                       deferredAreaLightStart);
  ASSERT_NE(deferredAreaLightShadowBeforeSampleLoop, std::string::npos);
  EXPECT_LT(deferredAreaLightShadowBeforeSampleLoop,
            deferredAreaLightSampleLoop)
      << "Area-light shadow visibility should use the reduced shared estimate "
         "instead of paying a full PCF for every lighting sample.";
  const size_t deferredAreaLightShadowAfterSampleLoop =
      directional.find("LocalAreaLightShadowVisibility(",
                       deferredAreaLightSampleLoop);
  EXPECT_TRUE(deferredAreaLightShadowAfterSampleLoop == std::string::npos ||
              deferredAreaLightShadowAfterSampleLoop >= deferredAreaLightEnd);

  EXPECT_TRUE(contains(transparent, "#include \"area_light_common.slang\""));
  EXPECT_TRUE(contains(transparent, "#include \"local_shadow_common.slang\""));
  EXPECT_TRUE(contains(transparent,
                       "sampleIndex < integrationSampleCount"));
  EXPECT_TRUE(contains(transparent, "EvaluateAreaLightSampleRadiance"));
  EXPECT_TRUE(
      contains(transparent, "LocalShadowMapsEnabled(uLighting, uLocalShadow)"));
  EXPECT_TRUE(contains(transparent, "LocalAreaLightShadowVisibility("));
  EXPECT_FALSE(contains(transparent, "ClosestPointOnAreaLight"));
  const size_t transparentAreaLightLoop =
      transparent.find("for (uint i = 0u; i < areaLightCount; ++i)");
  ASSERT_NE(transparentAreaLightLoop, std::string::npos);
  const size_t transparentAreaLightSampleLoop =
      transparent.find("sampleIndex < integrationSampleCount",
                       transparentAreaLightLoop);
  ASSERT_NE(transparentAreaLightSampleLoop, std::string::npos);
  const size_t transparentAreaLightShadowBeforeSampleLoop =
      transparent.find("LocalAreaLightShadowVisibility(",
                       transparentAreaLightLoop);
  ASSERT_NE(transparentAreaLightShadowBeforeSampleLoop, std::string::npos);
  EXPECT_LT(transparentAreaLightShadowBeforeSampleLoop,
            transparentAreaLightSampleLoop)
      << "Transparent area-light shadow visibility should use the reduced "
         "shared estimate.";
  const size_t transparentAreaLightLoopEnd =
      transparent.find("if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)",
                       transparentAreaLightLoop);
  ASSERT_NE(transparentAreaLightLoopEnd, std::string::npos);
  const size_t transparentAreaLightShadowAfterSampleLoop =
      transparent.find("LocalAreaLightShadowVisibility(",
                       transparentAreaLightSampleLoop);
  EXPECT_TRUE(transparentAreaLightShadowAfterSampleLoop == std::string::npos ||
              transparentAreaLightShadowAfterSampleLoop >=
                  transparentAreaLightLoopEnd);
}

TEST(RenderingConventionTests,
     AreaLightShadowVisibilitySkipsUnshadowedPackedAreaRefs) {
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  const size_t deferredAreaLightStart =
      directional.find("float3 EvaluateDeferredAreaLight");
  ASSERT_NE(deferredAreaLightStart, std::string::npos);
  const size_t deferredAreaLightEnd =
      directional.find("[shader(\"fragment\")]", deferredAreaLightStart);
  ASSERT_NE(deferredAreaLightEnd, std::string::npos);
  const std::string deferredAreaLightBlock = directional.substr(
      deferredAreaLightStart, deferredAreaLightEnd - deferredAreaLightStart);
  EXPECT_TRUE(containsIgnoringWhitespace(
      deferredAreaLightBlock,
      "uint areaShadowRef = PackedLocalShadowAreaRef(uLocalShadow, "
      "areaIndex)"));
  EXPECT_TRUE(containsIgnoringWhitespace(deferredAreaLightBlock,
                       "LocalShadowMapsEnabled(uLighting, uLocalShadow) &&\n"
                       "        areaShadowRef != 0u"));
  const size_t deferredRefGate =
      deferredAreaLightBlock.find("areaShadowRef != 0u");
  ASSERT_NE(deferredRefGate, std::string::npos);
  const size_t deferredVisibilityLoop = deferredAreaLightBlock.find(
      "visibilitySample < visibilitySampleCount");
  ASSERT_NE(deferredVisibilityLoop, std::string::npos);
  EXPECT_LT(deferredRefGate, deferredVisibilityLoop);

  const size_t transparentAreaLightStart =
      transparent.find("for (uint i = 0u; i < areaLightCount; ++i)");
  ASSERT_NE(transparentAreaLightStart, std::string::npos);
  const size_t transparentAreaLightEnd =
      transparent.find("if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)",
                       transparentAreaLightStart);
  ASSERT_NE(transparentAreaLightEnd, std::string::npos);
  const std::string transparentAreaLightBlock =
      transparent.substr(transparentAreaLightStart,
                         transparentAreaLightEnd - transparentAreaLightStart);
  EXPECT_TRUE(containsIgnoringWhitespace(
      transparentAreaLightBlock,
      "uint areaShadowRef = PackedLocalShadowAreaRef(uLocalShadow, i)"));
  EXPECT_TRUE(containsIgnoringWhitespace(transparentAreaLightBlock,
                       "LocalShadowMapsEnabled(uLighting, uLocalShadow) &&\n"
                       "                areaShadowRef != 0u"));
  const size_t transparentRefGate =
      transparentAreaLightBlock.find("areaShadowRef != 0u");
  ASSERT_NE(transparentRefGate, std::string::npos);
  const size_t transparentVisibilityLoop = transparentAreaLightBlock.find(
      "visibilitySample < visibilitySampleCount");
  ASSERT_NE(transparentVisibilityLoop, std::string::npos);
  EXPECT_LT(transparentRefGate, transparentVisibilityLoop);
}

TEST(RenderingConventionTests,
     DeferredPointLightsUseMaterialFlagsForThinSurfaces) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");

  EXPECT_TRUE(contains(brdfCommon, "EvaluateThinSurfaceTransmission"));
  EXPECT_TRUE(contains(pointLight, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(
      contains(pointLight, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(pointLight, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(
      contains(tiledLighting, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(
      contains(tiledLighting, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(
      contains(directional, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(directional, "EvaluateThinSurfaceTransmission("));
}

TEST(RenderingConventionTests, MaterialOverviewLoadsMetadataWithoutFiltering) {
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(postProcess, "else if (slot == 3u) panelMode = 3u;"));
  EXPECT_TRUE(contains(
      postProcess,
      "uint2 panelPixel = TexturePixelFromUV(panelUV, width, height);"));
  EXPECT_TRUE(contains(postProcess,
                       "DebugTextureViewColor(panelMode, panelUV, panelPixel"));
  EXPECT_TRUE(
      contains(postProcess, "gMaterialTexture.Load(int3(pixelCoord, 0))"));
  EXPECT_EQ(postProcess.find(
                "gMaterialTexture.SampleLevel(gBufferSampler, panelUV, 0.0)"),
            std::string::npos);
}

TEST(RenderingConventionTests, OverviewViewIncludesAllTextureDebugPanels) {
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(postProcess, "static const uint OVERVIEW_COLUMNS = 3u"));
  EXPECT_TRUE(contains(postProcess, "static const uint OVERVIEW_ROWS = 4u"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 6u) panelMode = 6u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 7u) panelMode = 7u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 8u) panelMode = 11u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 9u) panelMode = 12u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 10u) panelMode = 13u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 11u) panelMode = 14u;"));
  EXPECT_TRUE(contains(postProcess,
                       "DebugTextureViewColor(panelMode, panelUV, panelPixel"));
}

TEST(RenderingConventionTests,
     GBufferMaterialMetadataRoundTripsWithinCapacity) {
  constexpr uint32_t kLastMaterialIndex =
      container::gpu::kMaterialTextureDescriptorCapacity - 1u;
  static_assert(kLastMaterialIndex <=
                container::gpu::kMaxExactGBufferMaterialMetadataIndex);

  const float opaqueEncoded =
      encodeGBufferMaterialMetadata(kLastMaterialIndex, false);
  const float thinEncoded =
      encodeGBufferMaterialMetadata(kLastMaterialIndex, true);

  EXPECT_EQ(decodeGBufferMaterialIndex(opaqueEncoded), kLastMaterialIndex);
  EXPECT_FALSE(decodeGBufferThinSurface(opaqueEncoded));
  EXPECT_EQ(decodeGBufferMaterialIndex(thinEncoded), kLastMaterialIndex);
  EXPECT_TRUE(decodeGBufferThinSurface(thinEncoded));

  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  EXPECT_TRUE(
      contains(materialCommon, "MAX_EXACT_GBUFFER_MATERIAL_INDEX = 8388607u"));
}

TEST(RenderingConventionTests, MaterialTextureSamplersUsePerTextureMetadata) {
  using container::gpu::GpuTextureMetadata;

  EXPECT_EQ(container::gpu::kMaterialSamplerWrapModeCount, 3u);
  EXPECT_EQ(container::gpu::kMaterialSamplerDescriptorCapacity, 9u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapRepeat, 0u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapClampToEdge, 1u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapMirroredRepeat, 2u);
  EXPECT_EQ(sizeof(GpuTextureMetadata), 16u);
  EXPECT_EQ(offsetof(GpuTextureMetadata, samplerIndex), 0u);

  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string materialX =
      readRepoTextFile("src/utility/MaterialXIntegration.cpp");
  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string pbrCommon =
      readRepoTextFile("shaders/pbr_material_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");

  EXPECT_TRUE(contains(materialX, "gltfTextureSamplerIndex"));
  EXPECT_TRUE(contains(materialX, "TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE"));
  EXPECT_TRUE(contains(materialX, "TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT"));
  EXPECT_TRUE(contains(materialX, "textureCacheKey"));
  EXPECT_TRUE(contains(materialX, "resource.samplerIndex = samplerIndex"));

  EXPECT_TRUE(contains(sceneManager, "materialSamplerAddressMode"));
  EXPECT_TRUE(contains(sceneManager, "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE"));
  EXPECT_TRUE(
      contains(sceneManager, "VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT"));
  EXPECT_TRUE(contains(sceneManager, "uploadTextureMetadataBuffer"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 4"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 5"));

  EXPECT_TRUE(
      contains(materialCommon, "MATERIAL_SAMPLER_DESCRIPTOR_COUNT = 9u"));
  EXPECT_TRUE(contains(materialCommon, "struct GpuTextureMetadata"));
  EXPECT_TRUE(contains(pbrCommon, "uTextureMetadata[texIndex].samplerIndex"));
  EXPECT_TRUE(contains(pbrCommon, "materialSamplers[samplerIndex]"));
  EXPECT_TRUE(contains(
      gbuffer,
      "SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT]"));
  EXPECT_TRUE(contains(
      gbuffer, "StructuredBuffer<GpuTextureMetadata> uTextureMetadata"));
  EXPECT_TRUE(
      contains(gbuffer, "[[vk::binding(5, 0)]] Texture2D materialTextures"));
  EXPECT_FALSE(contains(gbuffer, "SamplerState baseSampler"));
}

TEST(RenderingConventionTests,
     MaterialTexturesUseRuntimeMipSelectionToAvoidTemporalShimmer) {
  const std::string pbrCommon =
      readRepoTextFile("shaders/pbr_material_common.slang");
  const std::string allocationManager =
      readRepoTextFile("src/utility/AllocationManager.cpp");
  const std::string allocationManagerHeader =
      readRepoTextFile("include/Container/utility/AllocationManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");

  const size_t sampleStart = pbrCommon.find("float4 SamplePbrTexture(");
  ASSERT_NE(sampleStart, std::string::npos);
  const size_t sampleEnd =
      pbrCommon.find("float4 SamplePbrTextureLevel", sampleStart);
  ASSERT_NE(sampleEnd, std::string::npos);
  const std::string runtimeSample =
      pbrCommon.substr(sampleStart, sampleEnd - sampleStart);

  EXPECT_TRUE(contains(runtimeSample, ".Sample("));
  EXPECT_FALSE(contains(runtimeSample, ".SampleLevel("));

  const size_t explicitSampleStart =
      pbrCommon.find("float4 SamplePbrTextureLevel");
  ASSERT_NE(explicitSampleStart, std::string::npos);
  const std::string explicitSample = pbrCommon.substr(explicitSampleStart);
  EXPECT_TRUE(contains(explicitSample, ".SampleLevel("));

  EXPECT_TRUE(contains(allocationManager, "CalculateTextureMipLevels"));
  EXPECT_TRUE(contains(allocationManager, "textureFormatSupportsLinearBlit"));
  EXPECT_TRUE(contains(allocationManager, "VK_FORMAT_FEATURE_BLIT_SRC_BIT"));
  EXPECT_TRUE(contains(allocationManager, "VK_FORMAT_FEATURE_BLIT_DST_BIT"));
  EXPECT_TRUE(contains(allocationManager, "generateTextureMipmaps"));
  EXPECT_TRUE(contains(allocationManager, "vkCmdBlitImage"));
  EXPECT_TRUE(contains(allocationManager,
                       "VK_IMAGE_USAGE_TRANSFER_SRC_BIT"));
  EXPECT_TRUE(contains(allocationManager, "imageInfo.mipLevels = mipLevels"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      allocationManager,
      "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, 0u, mipLevels"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      allocationManager,
      "createImageView(image, imageInfo.format, VK_IMAGE_VIEW_TYPE_2D, 1u, "
      "mipLevels)"));
  EXPECT_TRUE(contains(allocationManagerHeader, "generateTextureMipmaps"));
  EXPECT_TRUE(contains(allocationManagerHeader, "uint32_t levelCount = 1u"));

  EXPECT_TRUE(contains(sceneManager,
                       "info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR"));
  EXPECT_TRUE(contains(sceneManager, "info.minLod = 0.0f"));
  EXPECT_TRUE(contains(sceneManager, "info.maxLod = VK_LOD_CLAMP_NONE"));
}

TEST(RenderingConventionTests, IndirectDrawObjectIndexUsesBaseInstance) {
  const std::string objectIndexCommon =
      readRepoTextFile("shaders/object_index_common.slang");
  EXPECT_TRUE(contains(objectIndexCommon,
                       "ResolveObjectIndex(uint pushedObjectIndex, "
                       "uint instanceID, uint startInstance)"));
  EXPECT_TRUE(contains(objectIndexCommon, "? instanceID + startInstance"));

  constexpr std::array<std::string_view, 11> kObjectIndexedVertexShaders = {{
      "shaders/depth_prepass.slang",
      "shaders/forward_opaque.slang",
      "shaders/forward_transparent.slang",
      "shaders/gbuffer.slang",
      "shaders/geometry_debug.slang",
      "shaders/normal_validation.slang",
      "shaders/object_normals.slang",
      "shaders/shadow_depth.slang",
      "shaders/surface_normals.slang",
      "shaders/wireframe_debug.slang",
      "shaders/wireframe_fallback.slang",
  }};

  for (const std::string_view shaderPath : kObjectIndexedVertexShaders) {
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));
    EXPECT_TRUE(
        contains(shader, "uint startInstance : SV_StartInstanceLocation"))
        << shaderPath;
    EXPECT_TRUE(contains(
        shader,
        "ResolveObjectIndex(pc.objectIndex, instanceID, startInstance)"))
        << shaderPath;
  }
}

TEST(RenderingConventionTests, SectionPlanePushConstantsMatchShaderContracts) {
  using container::gpu::BindlessPushConstants;
  using container::gpu::ShadowPushConstants;
  using container::renderer::NormalValidationPushConstants;
  using container::renderer::SurfaceNormalPushConstants;
  using container::renderer::WireframePushConstants;

  EXPECT_EQ(sizeof(BindlessPushConstants), 32u);
  EXPECT_EQ(offsetof(BindlessPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(BindlessPushConstants, sectionPlaneEnabled), 4u);
  EXPECT_EQ(offsetof(BindlessPushConstants, sectionPlane), 16u);

  EXPECT_EQ(sizeof(ShadowPushConstants), 32u);
  EXPECT_EQ(offsetof(ShadowPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(ShadowPushConstants, cascadeIndex), 4u);
  EXPECT_EQ(offsetof(ShadowPushConstants, sectionPlaneEnabled), 8u);
  EXPECT_EQ(offsetof(ShadowPushConstants, sectionPlane), 16u);

  EXPECT_EQ(sizeof(WireframePushConstants), 64u);
  EXPECT_EQ(offsetof(WireframePushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(WireframePushConstants, sectionPlaneEnabled), 4u);
  EXPECT_EQ(offsetof(WireframePushConstants, sectionPlane), 16u);
  EXPECT_EQ(offsetof(WireframePushConstants, colorIntensity), 32u);
  EXPECT_EQ(offsetof(WireframePushConstants, lineWidth), 48u);

  EXPECT_EQ(sizeof(NormalValidationPushConstants), 48u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, showFaceFill), 4u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, faceAlpha), 8u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, faceClassificationFlags),
            12u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, sectionPlaneEnabled), 16u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, sectionPlane), 32u);

  EXPECT_EQ(sizeof(SurfaceNormalPushConstants), 32u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, lineLength), 4u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, lineOffset), 8u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, sectionPlaneEnabled), 12u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, sectionPlane), 16u);

  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  EXPECT_TRUE(contains(pushConstants, "struct BindlessPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct ShadowPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct WireframePushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct NormalValidationPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct SurfaceNormalPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "uint sectionPlaneEnabled;"));
  EXPECT_TRUE(contains(pushConstants, "float4 sectionPlane;"));
  EXPECT_TRUE(contains(pushConstants, "bool SectionPlaneClips"));

  constexpr std::array<std::string_view, 12> kSectionClippedShaders = {{
      "shaders/depth_prepass.slang",
      "shaders/forward_opaque.slang",
      "shaders/forward_transparent.slang",
      "shaders/gbuffer.slang",
      "shaders/geometry_debug.slang",
      "shaders/normal_validation.slang",
      "shaders/object_normals.slang",
      "shaders/shadow_depth.slang",
      "shaders/surface_normals.slang",
      "shaders/transparent_pick.slang",
      "shaders/wireframe_debug.slang",
      "shaders/wireframe_fallback.slang",
  }};

  for (const std::string_view shaderPath : kSectionClippedShaders) {
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));
    EXPECT_TRUE(contains(shader, "scene_clip_common.slang")) << shaderPath;
    EXPECT_TRUE(contains(shader, "SceneClipPlanesClip")) << shaderPath;
  }

  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string sectionClipCapRecorder =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string sceneController =
      readRepoTextFile("src/renderer/scene/SceneController.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  EXPECT_TRUE(
      contains(lightingPassRecorder, "syncOverlaySectionPlanePushConstants"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "pushConstants.wireframe->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "pushConstants.normalValidation->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "pushConstants.surfaceNormal->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "pc.sectionPlaneEnabled = 0u"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(WireframePushConstants)"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "sizeof(NormalValidationPushConstants)"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(SurfaceNormalPushConstants)"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(ShadowPushConstants)"));
  EXPECT_TRUE(contains(sceneController, "sectionPlaneClips"));
  EXPECT_TRUE(contains(bimManager, "sectionPlaneClips"));
  EXPECT_TRUE(contains(bimManager, "accumulateSectionCapCrossing"));
  EXPECT_TRUE(contains(bimManager, "intersectRaySectionPlane"));
  EXPECT_TRUE(contains(bimManager, "insideSectionCapBounds"));
  EXPECT_TRUE(contains(bimManager, "sectionCapCrossings & 1u"));
  EXPECT_TRUE(contains(bimManager, "sectionCapCrossesObject"));
  EXPECT_TRUE(
      contains(rendererFrontend, "hoverPickCache_.sectionPlaneEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "frame.sectionPlaneEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "frame->sectionPlaneEnabled"));
}

TEST(RenderingConventionTests, SectionedSelectionUsesVisibleGpuPickFirst) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const size_t selectStart =
      rendererFrontend.find("void RendererFrontend::selectMeshNodeAtCursor");
  ASSERT_NE(selectStart, std::string::npos);
  const size_t selectEnd = rendererFrontend.find(
      "void RendererFrontend::hoverMeshNodeAtCursor", selectStart);
  ASSERT_NE(selectEnd, std::string::npos);
  const std::string selectBlock =
      rendererFrontend.substr(selectStart, selectEnd - selectStart);

  const size_t sectionPlane =
      selectBlock.find("const bool sectionPlaneEnabled");
  const size_t gpuPick = selectBlock.find("samplePickIdAtCursor");
  ASSERT_NE(sectionPlane, std::string::npos);
  ASSERT_NE(gpuPick, std::string::npos);
  EXPECT_LT(sectionPlane, gpuPick);
  EXPECT_FALSE(contains(selectBlock, "!sectionPlaneEnabled &&"));
  EXPECT_TRUE(
      contains(selectBlock, "const bool hasGpuPick = samplePickIdAtCursor"));
}

TEST(RenderingConventionTests, HoverPickingUsesGpuPickBeforeCpuTraversal) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const size_t hoverStart =
      rendererFrontend.find("void RendererFrontend::hoverMeshNodeAtCursor");
  ASSERT_NE(hoverStart, std::string::npos);
  const size_t hoverEnd = rendererFrontend.find(
      "void RendererFrontend::transformSelectedNodeByDrag", hoverStart);
  ASSERT_NE(hoverEnd, std::string::npos);
  const std::string hoverBlock =
      rendererFrontend.substr(hoverStart, hoverEnd - hoverStart);

  const size_t gpuPick = hoverBlock.find("samplePickIdAtCursor");
  const size_t cpuScenePick = hoverBlock.find("pickRenderableNodeHit");
  const size_t cpuBimPick = hoverBlock.find("pickRenderableObject");
  ASSERT_NE(gpuPick, std::string::npos);
  ASSERT_NE(cpuScenePick, std::string::npos);
  ASSERT_NE(cpuBimPick, std::string::npos);
  EXPECT_LT(gpuPick, cpuScenePick);
  EXPECT_LT(gpuPick, cpuBimPick);
  EXPECT_TRUE(contains(hoverBlock, "GpuPickTarget target"));
  EXPECT_TRUE(contains(hoverBlock, "} else {"));
}

TEST(RenderingConventionTests, TransformGizmoOverlaysAfterOitResolve) {
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/deferred/DeferredRasterPostProcess.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string guiPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterGuiPassRecorder.cpp");
  const std::string guiPassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterGuiPassRecorder.h");
  const std::string deferredTransformGizmo = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterTransformGizmo.cpp");

  EXPECT_TRUE(contains(pipelineTypes, "transformGizmoOverlay"));
  EXPECT_TRUE(contains(pipelineBuilder, "transform_gizmo_overlay_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "tgOverlayPCI.renderPass = renderPasses.postProcess"));
  EXPECT_FALSE(contains(frameRecorder, "recordTransformGizmoOverlay"));
  EXPECT_FALSE(
      contains(frameRecorder, "p.pipeline.pipelines.transformGizmoOverlay"));
  EXPECT_FALSE(contains(frameRecorder, "recordDeferredTransformGizmoOverlay"));
  EXPECT_FALSE(contains(frameRecorder, "recordDeferredTransformGizmoPass"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "recordDeferredRasterTransformGizmoFramePass"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterPipelineId::TransformGizmoOverlay"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "recordDeferredTransformGizmoOverlay"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "recordDeferredTransformGizmoPass"));
  EXPECT_FALSE(contains(frameRecorder, "transformGizmoVertexCount"));
  EXPECT_FALSE(contains(frameRecorder, "updateTransformGizmoPushConstants"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "deferredTransformGizmoVertexCount"));
  EXPECT_TRUE(contains(deferredTransformGizmo,
                       "updateDeferredTransformGizmoPushConstants"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordDeferredTransformGizmoOverlay"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordDeferredTransformGizmoPass"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "DeferredPostProcessPassScope::recordFullscreenDraw"));
  EXPECT_TRUE(
      contains(deferredPostProcess, "vkCmdDraw(commandBuffer_, 3, 1, 0, 0);"));

  const size_t postProcessPass =
      deferredPostProcess.find("bool recordDeferredPostProcessPassCommands");
  ASSERT_NE(postProcessPass, std::string::npos);
  const size_t passScope =
      deferredPostProcess.find("DeferredPostProcessPassScope::"
                               "DeferredPostProcessPassScope",
                               postProcessPass);
  ASSERT_NE(passScope, std::string::npos);
  const std::string postProcessBlock =
      deferredPostProcess.substr(postProcessPass, passScope - postProcessPass);
  const size_t postDraw =
      postProcessBlock.find("postProcessPass.recordFullscreenDraw");
  ASSERT_NE(postDraw, std::string::npos);
  const size_t afterDraw =
      postProcessBlock.find("inputs.recordAfterFullscreenDraw", postDraw);
  ASSERT_NE(afterDraw, std::string::npos);

  const size_t overlayCallback =
      deferredRasterTechnique.find(".recordAfterFullscreenDraw");
  ASSERT_NE(overlayCallback, std::string::npos);
  const size_t overlayCallbackEnd =
      deferredRasterTechnique.find("graph.setPassReadiness", overlayCallback);
  ASSERT_NE(overlayCallbackEnd, std::string::npos);
  const std::string overlayBlock = deferredRasterTechnique.substr(
      overlayCallback, overlayCallbackEnd - overlayCallback);
  const size_t gizmoOverlay =
      overlayBlock.find("recordDeferredRasterTransformGizmoOverlay(");
  ASSERT_NE(gizmoOverlay, std::string::npos);
  const size_t imguiRender =
      overlayBlock.find("deferred->renderGui(passCmd);", gizmoOverlay);
  ASSERT_NE(imguiRender, std::string::npos);
  EXPECT_LT(postDraw, afterDraw);
  EXPECT_LT(gizmoOverlay, imguiRender);
  EXPECT_FALSE(contains(deferredRasterTechnique, "recordPostProcessOverlays"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordPostProcessPass"));
  EXPECT_FALSE(contains(frameRecorder, "guiManager_->render"));
  EXPECT_FALSE(contains(frameRecorder, "recordDeferredRasterGuiPass"));
  EXPECT_TRUE(
      contains(deferredFrameGraphContext, "recordDeferredRasterGuiPass"));
  EXPECT_TRUE(
      contains(guiPassRecorderHeader, "DeferredRasterGuiPassRecordInputs"));
  EXPECT_TRUE(contains(guiPassRecorder, "inputs.guiManager->render"));
}

TEST(RenderingConventionTests, BimFloorPlanOverlayCanBeEnabledInViewer) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string bimLightingOverlayRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimFloorPlanOverlayState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimFloorPlanElevationMode"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimFloorPlanOverlayState()"));
  EXPECT_TRUE(contains(guiManager, "Floor plan overlay"));
  EXPECT_TRUE(contains(guiManager, "Floor plan elevation"));
  EXPECT_TRUE(contains(guiManager, "Projected on ground"));
  EXPECT_TRUE(contains(guiManager, "Source elevation"));
  EXPECT_TRUE(contains(guiManager, "Floor plan depth test"));

  EXPECT_TRUE(contains(bimManagerHeader, "floorPlanDrawCommands()"));
  EXPECT_TRUE(contains(bimManagerHeader, "floorPlanGroundDrawCommands()"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "floorPlanSourceElevationDrawCommands()"));
  EXPECT_TRUE(contains(bimManagerHeader, "hasFloorPlanOverlay()"));
  EXPECT_TRUE(contains(bimManager, "appendFloorPlanOverlayGeometry"));
  EXPECT_TRUE(contains(bimManager, "floorPlanGround_.firstIndex"));
  EXPECT_TRUE(contains(bimManager, "floorPlanSourceElevation_.firstIndex"));
  EXPECT_TRUE(contains(bimManager, "appendFloorPlanOverlay(floorPlanGround_)"));
  EXPECT_TRUE(contains(bimManager,
                       "appendFloorPlanOverlay(floorPlanSourceElevation_)"));

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimFloorPlanOverlayState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "floorPlanDrawCommands"));
  EXPECT_TRUE(contains(pipelineTypes, "bimFloorPlanDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimFloorPlanNoDepth"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_floor_plan_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_floor_plan_no_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "floorPlanPCI.pInputAssemblyState = &lineAssembly"));

  EXPECT_TRUE(contains(rendererFrontend, "bimFloorPlanOverlayState()"));
  EXPECT_TRUE(contains(rendererFrontend, "floorPlan.elevationMode"));
  EXPECT_TRUE(
      contains(rendererFrontend, "BimFloorPlanElevationMode::SourceElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "floorPlanGroundDrawCommands()"));
  EXPECT_TRUE(
      contains(rendererFrontend, "floorPlanSourceElevationDrawCommands()"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.floorPlan.enabled"));
  EXPECT_FALSE(contains(frameRecorder, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "recordBimLightingOverlayFrameCommands"));
  EXPECT_TRUE(contains(bimLightingOverlayRecorder, "plan.floorPlan"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimFloorPlanDepth"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimFloorPlanNoDepth"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.floorPlan.opacity"));
  EXPECT_TRUE(
      contains(bimLightingOverlayHeader, "BimLightingFloorPlanOverlayInputs"));
  EXPECT_TRUE(
      contains(bimLightingOverlayPlanner, "BimLightingOverlayKind::FloorPlan"));
}

TEST(RenderingConventionTests,
     BimElevationViewsComposeExistingTechnicalPasses) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string cameraController =
      readRepoTextFile("src/renderer/scene/CameraController.cpp");
  const std::string bimLightingOverlayRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string sectionClipCapRecorder =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassRecorder.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimElevationTechnicalStyle"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimElevationViewState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimElevationViewRequest"));
  EXPECT_TRUE(contains(guiManagerHeader, "consumeBimElevationViewRequest"));

  EXPECT_TRUE(contains(guiManager, "BIM Elevation View"));
  EXPECT_TRUE(contains(guiManager, "Apply elevation view"));
  EXPECT_TRUE(contains(guiManager, "Hidden line"));
  EXPECT_TRUE(contains(guiManager, "Technical"));
  EXPECT_TRUE(contains(guiManager, "Depth-tested lines"));
  EXPECT_TRUE(contains(guiManager, "Align section plane to elevation"));
  EXPECT_TRUE(contains(guiManager, "requestElevationView"));
  EXPECT_TRUE(
      contains(guiManager, "gBufferViewMode_ = GBufferViewMode::Albedo"));
  EXPECT_TRUE(contains(guiManager, "wireframeSettings_.depthTest = true"));
  EXPECT_TRUE(
      contains(guiManager, "wireframeSettings_.mode = WireframeMode::Full"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneAxis_ = -1"));
  EXPECT_TRUE(contains(guiManager, "Section normal"));

  EXPECT_TRUE(contains(rendererFrontend, "consumeBimElevationViewRequest"));
  EXPECT_TRUE(contains(rendererFrontend, "setBimElevationView"));
  // Capture scripts may change projection explicitly. The elevation control
  // itself must continue to use the dedicated camera-view preset path.
  const auto captureStart = rendererFrontend.find("void RendererFrontend::applyTemporalCapture");
  const auto captureEnd = rendererFrontend.find("void RendererFrontend::writeCaptureTelemetry", captureStart);
  ASSERT_NE(captureStart, std::string::npos);
  ASSERT_NE(captureEnd, std::string::npos);
  const auto controls = rendererFrontend.substr(0, captureStart) + rendererFrontend.substr(captureEnd);
  EXPECT_FALSE(contains(controls, "setOrthographic(sceneState_.selectedMeshNode"));
  EXPECT_TRUE(contains(cameraController, "CameraViewPreset::Front"));
  EXPECT_TRUE(contains(cameraController, "CameraViewPreset::Back"));
  EXPECT_TRUE(contains(cameraController, "CameraViewPreset::Right"));
  EXPECT_TRUE(contains(cameraController, "CameraViewPreset::Left"));
  EXPECT_TRUE(contains(cameraController, "setOrthographic(nodeIndex, true)"));
  EXPECT_TRUE(contains(cameraController, "setViewPreset(nodeIndex, preset)"));

  EXPECT_TRUE(
      contains(bimLightingOverlayRecorder, "debugOverlay->drawWireframe"));
  EXPECT_TRUE(contains(sectionClipCapRecorder,
                       "recordBimSectionClipCapFramePassCommands"));
}

TEST(RenderingConventionTests,
     DepthTestedWireframeDebugOverlaysOffsetTowardCamera) {
  const std::string wireframeDebug =
      readRepoTextFile("shaders/wireframe_debug.slang");
  const std::string wireframeFallback =
      readRepoTextFile("shaders/wireframe_fallback.slang");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");

  for (const std::string *shader : {&wireframeDebug, &wireframeFallback}) {
    EXPECT_FALSE(contains(*shader, "ApplyReverseZWireframeDepthOffset"));
    EXPECT_FALSE(contains(*shader, "kWireframeDepthOffsetNdc"));
  }

  EXPECT_TRUE(contains(pipelineBuilder, "wfDepthRaster.depthBiasEnable"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "wfDepthRaster.depthBiasConstantFactor"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "wfFbDepthRaster.depthBiasConstantFactor"));
}

TEST(RenderingConventionTests, GltfPunctualPointLightsImportAsAuthoredLights) {
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManagerHeader =
      readRepoTextFile("include/Container/renderer/lighting/LightingManager.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");

  EXPECT_TRUE(contains(sceneManagerHeader, "authoredPointLights() const"));
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<container::gpu::PointLightData> authoredPointLights_{}"));
  EXPECT_TRUE(contains(sceneManagerHeader, "struct AuthoredDirectionalLight"));
  EXPECT_TRUE(
      contains(sceneManagerHeader, "authoredDirectionalLights() const"));
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "collectAuthoredPunctualLights()"));
  EXPECT_TRUE(contains(sceneManager, "activeSceneRootNodes(gltfModel_)"));
  EXPECT_TRUE(contains(sceneManager, "node.light"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"point\""));
  EXPECT_TRUE(
      contains(sceneManager, "lightDefinition.type == \"directional\""));
  EXPECT_TRUE(contains(sceneManager, "gltfDirectionalLightDirection"));
  EXPECT_TRUE(contains(sceneManager, "glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"spot\""));
  EXPECT_TRUE(contains(sceneManager, "gltfSpotConeCosines"));
  EXPECT_TRUE(contains(sceneManager, "gltfPointLightRange"));
  EXPECT_TRUE(contains(sceneManager, "pointLightColorOrDefault"));
  EXPECT_TRUE(contains(sceneManager, "pointLightIntensityOrDefault"));
  EXPECT_TRUE(contains(sceneManager, "static_cast<float>(light.range) *"));
  EXPECT_TRUE(contains(sceneManager, "sanitizeImportScale(importScale)"));
  EXPECT_TRUE(contains(sceneManager,
                       "return container::gpu::kUnboundedPointLightRange"));
  EXPECT_EQ(sceneManager.find("transformDistanceScale(sceneLocalTransform)"),
            std::string::npos);

  EXPECT_TRUE(contains(lightingManagerHeader, "appendAuthoredPointLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "applyAuthoredDirectionalLight"));
  EXPECT_TRUE(
      contains(lightingManager, "sceneManager_->authoredPointLights()"));
  EXPECT_TRUE(
      contains(lightingManager, "sceneManager_->authoredDirectionalLights()"));
  EXPECT_TRUE(contains(lightingManager, "authoredDirectionalLights().front()"));
  EXPECT_TRUE(contains(lightingManager,
                       "glm::vec4(glm::vec3(authoredLight.direction), 0.0f)"));
  EXPECT_TRUE(contains(
      lightingManager,
      "lightingData_.directionalDirection = glm::vec4(worldDirection, 0.0f)"));
  EXPECT_TRUE(contains(lightingManager,
                       "lightingData_.directionalColorIntensity = "
                       "authoredLight.colorIntensity"));
  EXPECT_TRUE(
      contains(lightingManager, "applyAuthoredDirectionalLight(anchor)"));
  EXPECT_TRUE(contains(lightingManager, "appendAuthoredPointLights(anchor)"));
  EXPECT_TRUE(contains(lightingManager,
                       "sourcePointCount > 0u ? EditableLightSource::Imported"));
  EXPECT_TRUE(
      contains(lightingManager, "world_.replacePointLights(pointLightsSsbo_)"));
}

TEST(RenderingConventionTests, LightingManagerOwnsEditableLightState) {
  const std::string header =
      readRepoTextFile("include/Container/renderer/lighting/LightingManager.h");
  const std::string source =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");

  EXPECT_TRUE(contains(header, "EditableLightEntity"));
  EXPECT_TRUE(contains(header, "editableLights() const"));
  EXPECT_TRUE(contains(header, "selectedEditableLight() const"));
  EXPECT_TRUE(contains(header, "selectEditableLight"));
  EXPECT_TRUE(contains(header, "updateEditableLight"));
  EXPECT_TRUE(contains(header, "addManualEditableLight"));
  EXPECT_TRUE(contains(header, "translateSelectedEditableLight"));
  EXPECT_TRUE(contains(header, "rotateSelectedEditableLight"));
  EXPECT_TRUE(contains(header, "scaleSelectedEditableLight"));
  EXPECT_TRUE(contains(source, "applyEditableLightOverrides"));
  EXPECT_TRUE(contains(source, "appendManualEditableLights"));
  EXPECT_EQ(source.find("GuiManager"), std::string::npos);
}

TEST(RenderingConventionTests, SceneControlsExposeEditableLightInspector) {
  const std::string header =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(header, "EditableLightEntity"));
  EXPECT_TRUE(contains(header, "addManualEditableLight"));
  EXPECT_TRUE(contains(gui, "Editable lights"));
  EXPECT_TRUE(contains(gui, "Selected Light Properties"));
  EXPECT_TRUE(contains(gui, "Light Transform"));
  EXPECT_TRUE(contains(gui, "Light Radiance"));
  EXPECT_TRUE(contains(gui, "Spot Beam"));
  EXPECT_TRUE(contains(gui, "Area Emitter"));
  EXPECT_TRUE(contains(gui, "Area Width"));
  EXPECT_TRUE(contains(gui, "Area Height"));
  EXPECT_TRUE(contains(gui, "Shadow Eligibility"));
  EXPECT_TRUE(
      contains(gui, "Local shadow budget assigns the first eligible point/spot "
                    "lights"));
  EXPECT_TRUE(contains(gui, "Directional direction"));
  EXPECT_TRUE(contains(gui, "Light Runtime Stats"));
  EXPECT_TRUE(contains(gui, "Lighting Settings"));
  EXPECT_EQ(gui.find("Lighting Preset"), std::string::npos);
  EXPECT_EQ(gui.find("LightingPreset("), std::string::npos);
  EXPECT_LT(gui.find("Editable lights"), gui.find("Light Runtime Stats"));
  EXPECT_TRUE(contains(gui, "Add point"));
  EXPECT_TRUE(contains(gui, "Add spot"));
  EXPECT_TRUE(contains(gui, "Add area"));
  EXPECT_TRUE(contains(frontend, "editableLights()"));
  EXPECT_TRUE(contains(frontend, "updateEditableLight"));
}

TEST(RenderingConventionTests, LocalPointShadowsHaveRuntimeBudget) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string localShadowAllocator = readRepoTextFile(
      "include/Container/renderer/lighting/LocalShadowLayerAllocator.h");
  const std::string shadowHeader =
      readRepoTextFile("include/Container/renderer/shadow/ShadowManager.h");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(containsIgnoringWhitespace(sceneData, "uint32_t localShadowPointBudget{1}"));
  EXPECT_TRUE(containsIgnoringWhitespace(sceneData, "uint32_t localShadowLayerBudget{8}"));
  EXPECT_TRUE(containsIgnoringWhitespace(lightingManager, "LocalShadowLayerAllocator.h"));
  EXPECT_TRUE(containsIgnoringWhitespace(lightingManager, "generatedLightingSettingsDiffer"));
  EXPECT_FALSE(containsIgnoringWhitespace(lightingManager, "lightingSettingsDiffer"));
  EXPECT_TRUE(containsIgnoringWhitespace(lightingManager, "kMaxLocalShadowOmniPointBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(lightingManager, "localShadowPointBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(lightingManager, "localShadowLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      localShadowAllocator,
      "inline constexpr uint32_t kMaxLocalShadowOmniPointBudget =\n"
      "    container::gpu::kMaxShadowedLocalLightLayers /\n"
      "    container::gpu::kLocalShadowPointFaceCount;"));
  EXPECT_FALSE(containsIgnoringWhitespace(
      localShadowAllocator,
      "kMaxShadowedLocalLightLayers /\n"
      "    container::gpu::kLocalShadowSpotLayerCount;"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAllocator, "assignedOmniPointCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAllocator, "assignedSpotCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAllocator, "usedLayerCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAllocator,
                       "!isSpot && result.assignedOmniPointCount >= "
                       "settings.omniPointBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAllocator,
                       "isSpot ? container::gpu::kLocalShadowSpotLayerCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      lightingManager,
      "lightingSettings_.localShadowLayerBudget =\n"
      "      std::min(settings.localShadowLayerBudget, "
      "kMaxShadowedLocalLightLayers)"));
  const size_t localShadowAssignStart =
      lightingManager.find("void LightingManager::assignLocalShadowLayerMetadata");
  ASSERT_NE(localShadowAssignStart, std::string::npos);
  const size_t localShadowAssignEnd =
      lightingManager.find("void LightingManager::publishPointLights",
                           localShadowAssignStart);
  ASSERT_NE(localShadowAssignEnd, std::string::npos);
  const std::string localShadowAssignBlock =
      lightingManager.substr(localShadowAssignStart,
                             localShadowAssignEnd - localShadowAssignStart);
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAssignBlock,
                       "container::renderer::assignLocalShadowLayerMetadata"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAssignBlock,
                       ".omniPointBudget = "
                       "lightingSettings_.localShadowPointBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(localShadowAssignBlock,
                       ".layerBudget = lightingSettings_.localShadowLayerBudget"));
  EXPECT_FALSE(containsIgnoringWhitespace(localShadowAssignBlock, "shadowedPointLightCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(shadowHeader, "uint32_t localShadowLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(shadowManager, "uint32_t localShadowLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      shadowManager,
      "const uint32_t activeLayerBudget =\n"
      "      std::min(localShadowLayerBudget, kMaxShadowedLocalLightLayers)"));
  EXPECT_TRUE(containsIgnoringWhitespace(shadowManager,
                       "baseLayer + layerCount > activeLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      shadowManager,
      "nextLayer + kLocalShadowAreaLayerCount > activeLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(testsCmake, "local_shadow_layer_allocator_tests"));
  EXPECT_TRUE(containsIgnoringWhitespace(gui, "Shadowed Point Light Budget"));
  EXPECT_TRUE(containsIgnoringWhitespace(gui, "Local Shadow Layer Budget"));
  EXPECT_TRUE(containsIgnoringWhitespace(gui, "container::gpu::kMaxShadowedLocalLightLayers"));
  EXPECT_TRUE(containsIgnoringWhitespace(frontend, "guiLightingSettings.localShadowPointBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(frontend, "guiLightingSettings.localShadowLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(frontend,
                       "currentLightingSettings.localShadowLayerBudget"));
  EXPECT_TRUE(containsIgnoringWhitespace(frontend, "updateLocalShadows("));
}

TEST(RenderingConventionTests, LightingManagerDoesNotAutoGeneratePointLights) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(lightingManager, "appendAuthoredPointLights(anchor)"));
  EXPECT_TRUE(contains(lightingManager, "appendManualEditableLights(anchor)"));
  EXPECT_TRUE(contains(lightingManager, "EditableLightSource::Generated"));
  EXPECT_EQ(lightingManager.find("kHallLightRows"), std::string::npos);
  EXPECT_EQ(lightingManager.find("kFallbackGridX"), std::string::npos);
  EXPECT_EQ(lightingManager.find("appendPointLight"), std::string::npos);
  EXPECT_EQ(sceneData.find("maxGeneratedPointLights"), std::string::npos);
  EXPECT_EQ(gui.find("Generated Point Budget"), std::string::npos);
  EXPECT_EQ(gui.find("Local Shadowed Point Lights"), std::string::npos);
}

TEST(RenderingConventionTests, SceneControlsExposePrimitiveAddMenu) {
  const std::string primitivesHeader =
      readRepoTextFile("include/Container/renderer/scene/ScenePrimitives.h");
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneControllerHeader =
      readRepoTextFile("include/Container/renderer/scene/SceneController.h");
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(primitivesHeader, "enum class ScenePrimitiveKind"));
  EXPECT_TRUE(contains(primitivesHeader, "TriangularPrism"));
  EXPECT_TRUE(contains(sceneManagerHeader, "appendRuntimeMesh"));
  EXPECT_TRUE(contains(sceneControllerHeader, "addScenePrimitive"));
  EXPECT_TRUE(contains(guiHeader, "addScenePrimitive"));
  EXPECT_TRUE(contains(gui, "Add Primitive"));
  EXPECT_TRUE(contains(gui, "Plane (double-sided wall)"));
  EXPECT_TRUE(contains(gui, "Triangular Prism"));
  EXPECT_TRUE(contains(gui, "Pyramid"));
  EXPECT_TRUE(contains(gui, "Torus"));
  EXPECT_TRUE(contains(frontend, "addScenePrimitive"));
  EXPECT_TRUE(contains(frontend, "ScenePrimitiveKind"));

  const size_t addPrimitiveStart = frontend.find("addScenePrimitive =");
  ASSERT_NE(addPrimitiveStart, std::string::npos);
  const size_t addPrimitiveEnd =
      frontend.find("subs_.guiManager->drawSceneControls", addPrimitiveStart);
  ASSERT_NE(addPrimitiveEnd, std::string::npos);
  const std::string addPrimitiveBlock =
      frontend.substr(addPrimitiveStart, addPrimitiveEnd - addPrimitiveStart);
  EXPECT_TRUE(contains(addPrimitiveBlock, "syncSceneStateFromController()"));
  EXPECT_TRUE(contains(addPrimitiveBlock, "refreshSceneObjectData()"));
  EXPECT_TRUE(contains(addPrimitiveBlock, "syncSceneProviders()"));
}

TEST(RenderingConventionTests,
     TransformGizmoCanOperateOnSelectedEditableLights) {
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(frontend, "selectedEditableLight()"));
  EXPECT_TRUE(contains(frontend, "translateSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "rotateSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "scaleSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "Selected editable light"));
}

TEST(RenderingConventionTests, ViewportSelectionChecksEditableLightGizmos) {
  const std::string header =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_FALSE(contains(header, "pickEditableLightAtCursor"));
  EXPECT_FALSE(contains(header, "hitRadiusPixels"));
  EXPECT_FALSE(contains(frontend, "pickDeferredLightGizmoAtCursor"));
  EXPECT_FALSE(
      contains(frontend, "RendererFrontend::pickEditableLightAtCursor"));
  EXPECT_TRUE(contains(frontend, "GpuPickTargetKind::Light"));
  EXPECT_TRUE(contains(frontend, "decodeEditableLightPickId"));
  EXPECT_TRUE(
      contains(frontend, "selectEditableLight(target.editableLightId)"));

  const size_t selectStart =
      frontend.find("void RendererFrontend::selectMeshNodeAtCursor");
  ASSERT_NE(selectStart, std::string::npos);
  const size_t selectEnd = frontend.find(
      "void RendererFrontend::hoverMeshNodeAtCursor", selectStart);
  ASSERT_NE(selectEnd, std::string::npos);
  const std::string selectBlock =
      frontend.substr(selectStart, selectEnd - selectStart);

  const size_t gpuPick = selectBlock.find("samplePickIdAtCursor");
  const size_t lightGpuPick =
      selectBlock.find("GpuPickTargetKind::Light", gpuPick);
  const size_t sceneCpuPick = selectBlock.find("pickRenderableNodeHit");
  const size_t bimCpuPick = selectBlock.find("pickRenderableObject");
  const size_t noGeometryBranch =
      selectBlock.find("if (!sceneHit.hit && !bimHit.hit)");
  const size_t clearSelection =
      selectBlock.find("clearSelectedMeshNode", noGeometryBranch);
  ASSERT_NE(gpuPick, std::string::npos);
  ASSERT_NE(lightGpuPick, std::string::npos);
  ASSERT_NE(sceneCpuPick, std::string::npos);
  ASSERT_NE(bimCpuPick, std::string::npos);
  ASSERT_NE(noGeometryBranch, std::string::npos);
  ASSERT_NE(clearSelection, std::string::npos);
  EXPECT_LT(gpuPick, lightGpuPick);
  EXPECT_LT(lightGpuPick, sceneCpuPick);
  EXPECT_LT(lightGpuPick, bimCpuPick);
  EXPECT_LT(sceneCpuPick, noGeometryBranch);
  EXPECT_LT(noGeometryBranch, clearSelection);
  EXPECT_FALSE(contains(selectBlock, "pickEditableLightAtCursor"));
  EXPECT_FALSE(contains(selectBlock, "kLightGizmoDirectPickRadiusPixels"));
  EXPECT_FALSE(contains(selectBlock, "kLightGizmoDefaultPickRadiusPixels"));

  const size_t clearStart =
      frontend.find("void RendererFrontend::clearSelectedMeshNode");
  ASSERT_NE(clearStart, std::string::npos);
  const size_t clearEnd = frontend.find(
      "void RendererFrontend::transformSelectedNodeByDrag", clearStart);
  ASSERT_NE(clearEnd, std::string::npos);
  const std::string clearBlock =
      frontend.substr(clearStart, clearEnd - clearStart);
  EXPECT_TRUE(contains(clearBlock, "selectEditableLight({})"));
}

TEST(RenderingConventionTests,
     PostProcessPushConstantsExposeExposureAndCascadeMetadata) {
  using container::gpu::ExposureHistogramPushConstants;
  using container::gpu::ExposureSettings;
  using container::gpu::ExposureStateData;
  using container::gpu::PostProcessPushConstants;

  EXPECT_EQ(sizeof(ExposureSettings), 32u);
  EXPECT_EQ(offsetof(ExposureSettings, mode), 0u);
  EXPECT_EQ(offsetof(ExposureSettings, manualExposure), 4u);
  EXPECT_EQ(offsetof(ExposureSettings, targetLuminance), 8u);
  EXPECT_EQ(offsetof(ExposureSettings, minExposure), 12u);
  EXPECT_EQ(offsetof(ExposureSettings, maxExposure), 16u);
  EXPECT_EQ(offsetof(ExposureSettings, adaptationRate), 20u);
  EXPECT_EQ(offsetof(ExposureSettings, meteringLowPercentile), 24u);
  EXPECT_EQ(offsetof(ExposureSettings, meteringHighPercentile), 28u);

  EXPECT_EQ(sizeof(PostProcessPushConstants), 76u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, outputMode), 0u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, bloomEnabled), 4u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, bloomIntensity), 8u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, exposure), 12u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cameraNear), 16u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cameraFar), 20u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cascadeSplits), 24u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, tileCountX), 40u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, totalLights), 44u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, depthSliceCount), 48u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, oitEnabled), 52u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, exposureMode), 56u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, targetLuminance), 60u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, minExposure), 64u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, maxExposure), 68u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, adaptationRate), 72u);

  const ExposureSettings exposureSettings{};
  EXPECT_EQ(exposureSettings.mode, container::gpu::kExposureModeManual);
  EXPECT_FLOAT_EQ(exposureSettings.manualExposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureSettings.targetLuminance, 0.18f);
  EXPECT_FLOAT_EQ(exposureSettings.minExposure, 0.03125f);
  EXPECT_FLOAT_EQ(exposureSettings.maxExposure, 8.0f);
  EXPECT_FLOAT_EQ(exposureSettings.adaptationRate, 1.5f);
  EXPECT_FLOAT_EQ(exposureSettings.meteringLowPercentile, 0.50f);
  EXPECT_FLOAT_EQ(exposureSettings.meteringHighPercentile, 0.95f);

  const PostProcessPushConstants pc{};
  EXPECT_FLOAT_EQ(pc.exposure, 0.25f);
  EXPECT_FLOAT_EQ(pc.cameraNear, 0.1f);
  EXPECT_FLOAT_EQ(pc.cameraFar, 100.0f);
  EXPECT_EQ(pc.depthSliceCount, container::gpu::kClusterDepthSlices);
  EXPECT_EQ(std::size(pc.cascadeSplits), container::gpu::kShadowCascadeCount);
  EXPECT_EQ(pc.exposureMode, container::gpu::kExposureModeManual);
  EXPECT_FLOAT_EQ(pc.targetLuminance, 0.18f);
  EXPECT_FLOAT_EQ(pc.minExposure, 0.03125f);
  EXPECT_FLOAT_EQ(pc.maxExposure, 8.0f);
  EXPECT_FLOAT_EQ(pc.adaptationRate, 1.5f);

  EXPECT_EQ(sizeof(ExposureHistogramPushConstants), 32u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, width), 0u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, height), 4u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, binCount), 8u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, minLogLuminance), 12u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, maxLogLuminance), 16u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad0), 20u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad1), 24u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad2), 28u);

  EXPECT_EQ(sizeof(ExposureStateData), 16u);
  EXPECT_EQ(offsetof(ExposureStateData, exposure), 0u);
  EXPECT_EQ(offsetof(ExposureStateData, averageLuminance), 4u);
  EXPECT_EQ(offsetof(ExposureStateData, targetExposure), 8u);
  EXPECT_EQ(offsetof(ExposureStateData, initialized), 12u);

  const ExposureStateData exposureState{};
  EXPECT_FLOAT_EQ(exposureState.exposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureState.averageLuminance, 0.18f);
  EXPECT_FLOAT_EQ(exposureState.targetExposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureState.initialized, 0.0f);
}

TEST(RenderingConventionTests, PostProcessShaderUsesExposurePushConstant) {
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_NE(pushConstants.find("struct PostProcessPushConstants"),
            std::string::npos);
  EXPECT_NE(pushConstants.find("float exposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float cascadeSplits[4];"), std::string::npos);
  EXPECT_NE(pushConstants.find("uint exposureMode;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float targetLuminance;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float minExposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float maxExposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float adaptationRate;"), std::string::npos);
  EXPECT_NE(pushConstants.find("struct ExposureHistogramPushConstants"),
            std::string::npos);
  EXPECT_NE(pushConstants.find("float minLogLuminance;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float maxLogLuminance;"), std::string::npos);
  EXPECT_NE(postProcess.find("ConstantBuffer<PostProcessPushConstants> pc"),
            std::string::npos);
  EXPECT_NE(postProcess.find("ResolveExposureForToneMapping"),
            std::string::npos);
  EXPECT_NE(postProcess.find("pc.exposureMode"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.minExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.maxExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("FiniteColorOr(finalHdr, 0.0.xxx) *"),
            std::string::npos);
  EXPECT_NE(postProcess.find("compositedColor * resolvedExposure"),
            std::string::npos);
  EXPECT_NE(postProcess.find("overviewCompositedColor * resolvedExposure"),
            std::string::npos);
  EXPECT_EQ(postProcess.find("finalHdr * 0.25"), std::string::npos);
  EXPECT_EQ(postProcess.find("litColor *= 0.25"), std::string::npos);
}

TEST(RenderingConventionTests,
     ExposureSettingsFlowFromUiToPostProcessPushConstants) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredPostProcessHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterPostProcess.h");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/deferred/DeferredRasterPostProcess.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string exposureHeader =
      readRepoTextFile("include/Container/renderer/effects/ExposureManager.h");
  const std::string exposureManager =
      readRepoTextFile("src/renderer/effects/ExposureManager.cpp");
  const std::string exposureHistogram =
      readRepoTextFile("shaders/exposure_histogram.slang");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");
  const std::string renderGraph =
      readRepoTextFile("src/renderer/core/RenderGraph.cpp");

  EXPECT_TRUE(contains(
      guiHeader, "const container::gpu::ExposureSettings& exposureSettings()"));
  EXPECT_TRUE(contains(guiHeader,
                       "container::gpu::ExposureSettings exposureSettings_{}"));
  EXPECT_TRUE(contains(guiManager, "\"Exposure Mode\""));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.manualExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.targetLuminance"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.minExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.maxExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.adaptationRate"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.meteringLowPercentile"));
  EXPECT_TRUE(
      contains(guiManager, "&exposureSettings_.meteringHighPercentile"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "sanitizeExposureSettings"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "resolvePostProcessExposure"));
  EXPECT_FALSE(contains(frameRecorder, "float resolvePostProcessExposure"));
  EXPECT_FALSE(contains(frameRecorder, "buildDeferredPostProcessFrameState"));
  EXPECT_FALSE(contains(frameRecorder, "DeferredPostProcessPassScope"));
  EXPECT_FALSE(contains(frameRecorder, "displayModeRecordsBloom(displayMode)"));
  EXPECT_FALSE(
      contains(frameRecorder, "displayModeRecordsTileCull(displayMode)"));
  EXPECT_TRUE(
      contains(deferredPostProcess, "recordDeferredPostProcessPassCommands"));
  EXPECT_TRUE(
      contains(deferredPostProcess, "buildDeferredPostProcessFrameState"));
  EXPECT_TRUE(contains(deferredPostProcess, "DeferredPostProcessPassScope"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "recordDeferredPostProcessPassCommands"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "RenderPassId::ExposureAdaptation"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "deferred->exposureManager()->dispatch"));
  EXPECT_TRUE(
      contains(deferredPostProcessHeader, "resolvePostProcessExposure"));
  EXPECT_TRUE(contains(deferredPostProcess, "resolvePostProcessExposure"));
  EXPECT_FALSE(contains(frameRecorder, "exposureManager_->resolvedExposure"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "exposureManager->resolvedExposure"));
  EXPECT_TRUE(contains(exposureHeader, "kHistogramBinCount = 64"));
  EXPECT_TRUE(contains(exposureManager, "collectReadback"));
  EXPECT_TRUE(contains(exposureManager, "settings.targetLuminance"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringLowPercentile"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringHighPercentile"));
  EXPECT_TRUE(contains(exposureHistogram,
                       "RWStructuredBuffer<uint> luminanceHistogram"));
  EXPECT_TRUE(
      contains(exposureHistogram, "InterlockedAdd(luminanceHistogram[bin]"));
  EXPECT_TRUE(
      contains(frameResourceManager, "fallbackExposure.exposure = 0.25f"));
  EXPECT_TRUE(
      contains(frameResourceManager, "fallbackExposure.initialized = 0.0f"));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureAdaptation\""));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureState\""));
  EXPECT_TRUE(contains(deferredPostProcessHeader,
                       "class DeferredPostProcessPassScope"));
  EXPECT_TRUE(contains(deferredPostProcessHeader,
                       "DeferredPostProcessPassRecordInputs"));
  EXPECT_TRUE(contains(deferredPostProcessHeader,
                       "recordDeferredPostProcessPassCommands"));
  EXPECT_TRUE(contains(deferredPostProcess, "beginDynamicRendering"));
  EXPECT_TRUE(contains(deferredPostProcess, "endDynamicRendering"));
  EXPECT_TRUE(contains(deferredPostProcess, "vkCmdPushConstants"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "displayModeRecordsBloom(inputs_.displayMode)"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "displayModeRecordsTileCull(inputs_.displayMode)"));
  EXPECT_TRUE(
      contains(deferredPostProcess,
               "pushConstants.exposureMode = inputs.exposureSettings.mode"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "pushConstants.targetLuminance = "
                       "inputs.exposureSettings.targetLuminance"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.minExposure = inputs.exposureSettings.minExposure"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.maxExposure = inputs.exposureSettings.maxExposure"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.adaptationRate = inputs.exposureSettings.adaptationRate"));
}

TEST(RenderingConventionTests,
     ExposureManagerSharesAdaptationStateAcrossSwapchainImages) {
  const std::string exposureHeader =
      readRepoTextFile("include/Container/renderer/effects/ExposureManager.h");
  const std::string exposureManager =
      readRepoTextFile("src/renderer/effects/ExposureManager.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string forwardRasterTechnique =
      readRepoTextFile("src/renderer/forward/ForwardRasterTechnique.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string frameResourceHeader =
      readRepoTextFile("include/Container/renderer/resources/"
                       "FrameResourceManager.h");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "void createResources(const std::filesystem::path& "
                       "shaderDir,\n                       uint32_t "
                       "descriptorSetCount = 1)"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader, "void dispatch(uint32_t imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "void collectReadback(uint32_t imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "VkBuffer exposureStateBuffer(uint32_t imageIndex) const"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "std::span<const container::gpu::AllocatedBuffer>"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader, "exposureStateBuffers() const"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "histogramBuffers_"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "container::gpu::AllocatedBuffer "
                       "exposureStateBuffer_{}"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureHeader,
                       "std::vector<VkDescriptorSet> descriptorSets_"));

  EXPECT_TRUE(containsIgnoringWhitespace(exposureManager, "resizeFrameResources(imageCount)"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureManager, "histogramBuffers_[imageIndex]"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureManager, "exposureStateBuffer_"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureManager, "descriptorSets_[imageIndex]"));
  EXPECT_TRUE(containsIgnoringWhitespace(exposureManager,
                       "updateDescriptorSet(imageIndex, sceneColorView, sourceLayout)"));
  EXPECT_FALSE(containsIgnoringWhitespace(exposureManager, "exposureStateBuffers_[imageIndex]"));

  EXPECT_TRUE(containsIgnoringWhitespace(deferredRasterTechnique,
                       "dispatch(p.runtime.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(rendererFrontend,
                       "exposureManager->createResources(\n"
                       "      container::util::executableDirectory(),\n"
                       "      static_cast<uint32_t>("));
  EXPECT_TRUE(containsIgnoringWhitespace(rendererFrontend,
                       "exposureStateBuffers =\n"
                       "          subs_.exposureManager->exposureStateBuffers()"));
  EXPECT_TRUE(containsIgnoringWhitespace(frameResourceHeader,
                       "std::span<const container::gpu::AllocatedBuffer> "
                       "exposureStateBuffers"));
  EXPECT_TRUE(containsIgnoringWhitespace(frameResourceManager,
                       "const auto& exposureStateBuffer = "
                       "exposureStateBuffers[std::min("));

  EXPECT_FALSE(containsIgnoringWhitespace(exposureHeader,
                        "container::gpu::AllocatedBuffer histogramBuffer_{}"));
  EXPECT_FALSE(containsIgnoringWhitespace(exposureHeader,
                        "std::vector<container::gpu::AllocatedBuffer> "
                        "exposureStateBuffers_"));
  EXPECT_FALSE(containsIgnoringWhitespace(exposureHeader,
                        "VkDescriptorSet descriptorSet_{VK_NULL_HANDLE}"));
}

TEST(RenderingConventionTests, DeferredDepthReadOnlyTransitionUsesRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string transitionHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterDepthReadOnlyTransitionRecorder.h");
  const std::string transitionRecorder =
      readRepoTextFile("src/renderer/deferred/"
                       "DeferredRasterDepthReadOnlyTransitionRecorder.cpp");
  const std::string imageBarrierRecorder =
      readRepoTextFile("src/renderer/deferred/DeferredRasterImageBarrier.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t transitionStart =
      deferredRasterTechnique.find("RenderPassId::DepthToReadOnly");
  ASSERT_NE(transitionStart, std::string::npos);
  const size_t transitionEnd =
      deferredRasterTechnique.find("RenderPassId::TileCull", transitionStart);
  ASSERT_NE(transitionEnd, std::string::npos);
  const std::string transitionBlock = deferredRasterTechnique.substr(
      transitionStart, transitionEnd - transitionStart);

  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterDepthReadOnlyTransitionRecorder.h"));
  EXPECT_TRUE(contains(transitionBlock,
                       "buildDeferredRasterDepthReadOnlyTransitionPlan"));
  EXPECT_TRUE(contains(transitionBlock,
                       "recordDeferredRasterDepthReadOnlyTransitionCommands"));
  EXPECT_TRUE(contains(transitionBlock, ".depthStencilImage"));
  EXPECT_TRUE(contains(transitionBlock, ".shadowAtlasImage"));
  EXPECT_TRUE(contains(transitionBlock, ".shadowAtlasVisible"));
  EXPECT_TRUE(contains(transitionBlock, ".shadowCascadeCount"));
  EXPECT_TRUE(contains(transitionBlock, ".localShadowAtlasImage"));
  EXPECT_TRUE(contains(transitionBlock, ".localShadowAtlasVisible"));
  EXPECT_TRUE(contains(transitionBlock, ".localShadowLayerCount"));
  EXPECT_FALSE(contains(transitionBlock, "VkImageMemoryBarrier"));
  EXPECT_FALSE(contains(transitionBlock, "vkCmdPipelineBarrier"));
  EXPECT_FALSE(contains(transitionBlock,
                        "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_FALSE(
      contains(transitionBlock, "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL"));
  EXPECT_FALSE(contains(transitionBlock, "VK_ACCESS_"));
  EXPECT_FALSE(contains(transitionBlock, "VK_PIPELINE_STAGE_"));

  EXPECT_TRUE(contains(transitionHeader, "DeferredRasterImageBarrierStep"));
  EXPECT_TRUE(contains(transitionHeader,
                       "DeferredRasterDepthReadOnlyTransitionInputs"));
  EXPECT_TRUE(
      contains(transitionHeader, "DeferredRasterDepthReadOnlyTransitionPlan"));
  EXPECT_TRUE(contains(transitionHeader, "localShadowAtlasImage"));
  EXPECT_TRUE(contains(transitionHeader, "localShadowAtlasVisible"));
  EXPECT_TRUE(contains(transitionHeader, "localShadowLayerCount"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "buildDeferredRasterDepthReadOnlyTransitionPlan"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "recordDeferredRasterDepthReadOnlyTransitionCommands"));
  EXPECT_TRUE(
      contains(transitionRecorder, "recordDeferredRasterImageBarrierSteps"));
  EXPECT_TRUE(contains(imageBarrierRecorder, "vkCmdPipelineBarrier"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_TRUE(
      contains(transitionRecorder,
               "VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_TRUE(
      contains(transitionRecorder, "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL"));
  EXPECT_TRUE(contains(transitionRecorder, "VK_IMAGE_ASPECT_DEPTH_BIT | "
                                           "VK_IMAGE_ASPECT_STENCIL_BIT"));
  EXPECT_FALSE(contains(transitionRecorder, "FrameRecorder"));
  EXPECT_FALSE(contains(transitionRecorder, "DeferredRasterTechnique"));
  EXPECT_FALSE(contains(transitionRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transitionRecorder, "RenderGraph"));
  EXPECT_FALSE(contains(transitionRecorder, "GuiManager"));
  EXPECT_FALSE(contains(transitionRecorder, "ShadowManager"));
  EXPECT_FALSE(contains(transitionRecorder, "GpuCullManager"));
  EXPECT_FALSE(contains(transitionRecorder, "LightingManager"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredRasterImageBarrier.cpp"));
  EXPECT_TRUE(contains(
      srcCmake,
      "renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.cpp"));
  EXPECT_TRUE(contains(
      testsCmake, "deferred_raster_depth_read_only_transition_recorder_tests"));
}

TEST(RenderingConventionTests, DeferredFrustumCullUsesPlannerAndRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string plannerHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterFrustumCullPassPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassPlanner.cpp");
  const std::string recorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterFrustumCullPassRecorder.h");
  const std::string recorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t passStart =
      deferredRasterTechnique.find("RenderPassId::FrustumCull");
  ASSERT_NE(passStart, std::string::npos);
  const size_t passEnd =
      deferredRasterTechnique.find("RenderPassId::DepthPrepass", passStart);
  ASSERT_NE(passEnd, std::string::npos);
  const std::string frustumBlock =
      deferredRasterTechnique.substr(passStart, passEnd - passStart);

  const size_t readinessStart =
      deferredRasterTechnique.find("RenderPassId::FrustumCull", passEnd);
  ASSERT_NE(readinessStart, std::string::npos);
  const size_t readinessEnd = deferredRasterTechnique.find(
      "RenderPassId::BimDepthPrepass", readinessStart);
  ASSERT_NE(readinessEnd, std::string::npos);
  const std::string readinessBlock = deferredRasterTechnique.substr(
      readinessStart, readinessEnd - readinessStart);

  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterFrustumCullPassPlanner.h"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterFrustumCullPassRecorder.h"));
  EXPECT_TRUE(contains(frustumBlock, "buildDeferredRasterFrustumCullPassPlan"));
  EXPECT_TRUE(
      contains(frustumBlock, "recordDeferredRasterFrustumCullPassCommands"));
  EXPECT_TRUE(contains(frustumBlock, ".gpuCullManagerReady"));
  EXPECT_TRUE(contains(frustumBlock, ".sceneSingleSidedDrawsAvailable"));
  EXPECT_TRUE(contains(frustumBlock, ".cameraBufferReady"));
  EXPECT_TRUE(contains(frustumBlock, ".objectBufferReady"));
  EXPECT_TRUE(contains(frustumBlock, ".debugFreezeCulling"));
  EXPECT_TRUE(contains(frustumBlock, ".cullingFrozen"));
  EXPECT_TRUE(contains(frustumBlock, ".sourceDrawCount"));
  EXPECT_FALSE(contains(frustumBlock, "ensureBufferCapacity"));
  EXPECT_FALSE(contains(frustumBlock, "updateObjectSsboDescriptor"));
  EXPECT_FALSE(contains(frustumBlock, "uploadDrawCommands"));
  EXPECT_FALSE(contains(frustumBlock, "freezeCulling("));
  EXPECT_FALSE(contains(frustumBlock, "unfreezeCulling"));
  EXPECT_FALSE(contains(frustumBlock, "dispatchFrustumCull"));
  EXPECT_TRUE(
      contains(readinessBlock, "buildDeferredRasterFrustumCullPassPlan"));
  EXPECT_TRUE(contains(readinessBlock, ".readiness"));
  EXPECT_FALSE(contains(readinessBlock, "renderPassNotNeeded()"));
  EXPECT_FALSE(
      contains(readinessBlock,
               "renderPassMissingResource(RenderResourceId::CameraBuffer)"));
  EXPECT_FALSE(
      contains(readinessBlock,
               "renderPassMissingResource(RenderResourceId::ObjectBuffer)"));

  const size_t ensureCapacity = recorder.find("ensureBufferCapacity");
  const size_t updateDescriptor = recorder.find("updateObjectSsboDescriptor");
  const size_t uploadDraws = recorder.find("uploadDrawCommands");
  const size_t freeze = recorder.find("freezeCulling");
  const size_t unfreeze = recorder.find("unfreezeCulling");
  const size_t dispatch = recorder.find("dispatchFrustumCull");
  ASSERT_NE(ensureCapacity, std::string::npos);
  ASSERT_NE(updateDescriptor, std::string::npos);
  ASSERT_NE(uploadDraws, std::string::npos);
  ASSERT_NE(freeze, std::string::npos);
  ASSERT_NE(unfreeze, std::string::npos);
  ASSERT_NE(dispatch, std::string::npos);
  EXPECT_LT(ensureCapacity, updateDescriptor);
  EXPECT_LT(updateDescriptor, uploadDraws);
  EXPECT_LT(uploadDraws, freeze);
  EXPECT_LT(uploadDraws, unfreeze);
  EXPECT_LT(freeze, dispatch);
  EXPECT_LT(unfreeze, dispatch);

  EXPECT_TRUE(
      contains(plannerHeader, "DeferredRasterFrustumCullPassPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterFrustumCullPassPlan"));
  EXPECT_TRUE(contains(planner, "buildDeferredRasterFrustumCullPassPlan"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::NotNeeded"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::MissingResource"));
  EXPECT_TRUE(contains(planner, "RenderResourceId::CameraBuffer"));
  EXPECT_TRUE(contains(planner, "RenderResourceId::ObjectBuffer"));
  EXPECT_FALSE(contains(planner, "GpuCullManager"));
  EXPECT_FALSE(contains(planner, "FrameRecorder"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "SwapChainManager"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(
      contains(recorderHeader, "DeferredRasterFrustumCullPassRecordInputs"));
  EXPECT_TRUE(
      contains(recorder, "recordDeferredRasterFrustumCullPassCommands"));
  EXPECT_FALSE(contains(recorder, "FrameRecorder"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "RenderGraph"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "SwapChainManager"));
  EXPECT_FALSE(contains(recorder, "DeferredRasterTechnique"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterFrustumCullPassPlanner.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_raster_frustum_cull_pass_planner_tests"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_raster_frustum_cull_pass_recorder_tests"));
}

TEST(RenderingConventionTests,
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
  EXPECT_TRUE(contains(sceneController,
                       "objectBufferUploadRevisions_.find(objectBuffer.buffer)"));
  EXPECT_TRUE(contains(sceneController,
                       "uploadedRevision != objectBufferUploadRevision_"));
  EXPECT_TRUE(contains(sceneController,
                       "objectBufferUploadRevisions_[objectBuffer.buffer]"));
  EXPECT_FALSE(contains(sceneControllerHeader, "objectBufferUploadDirty_"));
  EXPECT_FALSE(contains(sceneController, "objectBufferUploadDirty_ = false"));
}

TEST(RenderingConventionTests,
     SceneManagerCanWritePerImageObjectBufferDescriptors) {
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");

  EXPECT_TRUE(contains(sceneManagerHeader,
                       "std::span<const container::gpu::AllocatedBuffer> "
                       "objectBuffers"));
  EXPECT_TRUE(contains(sceneManager,
                       "const auto& objectBuffer = objectBuffers[std::min("));
  EXPECT_TRUE(contains(sceneManager,
                       "writeDescriptorSetContents(descriptorSets_[i], "
                       "cameraBuffers[i], objectBuffer)"));
  EXPECT_TRUE(contains(sceneManager,
                       "writeDescriptorSetContents(auxiliaryDescriptorSets_[i], "
                       "cameraBuffers[i],"));
  EXPECT_TRUE(contains(sceneManager,
                       "std::span<const container::gpu::AllocatedBuffer>("
                       "&objectBuffer, 1u)"));
}

TEST(RenderingConventionTests,
     ShadowCullManagerTracksObjectSsboDescriptorsPerImage) {
  const std::string shadowCullHeader =
      readRepoTextFile("include/Container/renderer/shadow/ShadowCullManager.h");
  const std::string shadowCullManager =
      readRepoTextFile("src/renderer/shadow/ShadowCullManager.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");

  EXPECT_TRUE(contains(shadowCullHeader,
                       "void updateObjectSsboDescriptor(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "void uploadDrawCommands(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "indirectDrawBuffer(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "drawCountBuffer(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullHeader, "std::vector<VkBuffer> objectSsboBuffers_"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "std::vector<VkDeviceSize> objectSsboSizes_"));
  EXPECT_TRUE(contains(shadowCullHeader, "std::vector<uint32_t> objectCounts_"));
  EXPECT_TRUE(contains(shadowCullHeader, "inputDrawBuffers_"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "std::vector<std::array<container::gpu::AllocatedBuffer"));
  EXPECT_TRUE(contains(shadowCullHeader, "lastUploadSourceRevision_"));
  EXPECT_TRUE(contains(shadowCullHeader, "std::vector<uint32_t> drawCapacities_"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "bool ensureBufferCapacity(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullHeader,
                       "bool canDispatchCascadeCull(uint32_t imageIndex"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "updateObjectSsboDescriptor(i, objectBuffer, "
                       "objectBufferSize)"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "const VkBuffer objectSsboBuffer = "
                       "objectSsboBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "objectSsboBuffers_.resize(setCount, "
                       "fallbackObjectBuffer)"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "inputDrawBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "indirectDrawBuffers_[imageIndex][cascadeIndex]"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "drawCountBuffers_[imageIndex][cascadeIndex]"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "buildGpuCullDrawUploadPlan"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "maxDrawCount(imageIndex)"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "resizePerImageBufferState(imageCount)"));
  EXPECT_TRUE(contains(shadowCullManager,
                       "pc.objectCount  = objectCounts_[imageIndex]"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "ensureBufferCapacity(\n"
                       "        params.runtime.imageIndex"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "uploadDrawCommands(\n"
                       "        params.runtime.imageIndex"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "indirectDrawBuffer(\n"
                       "                  p.runtime.imageIndex"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "drawCountBuffer(\n"
                       "                  p.runtime.imageIndex"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "canDispatchCascadeCull(\n"
                       "          params.runtime.imageIndex"));
  EXPECT_FALSE(contains(shadowCullHeader, "VkBuffer objectSsboBuffer_"));
  EXPECT_FALSE(contains(shadowCullHeader,
                        "container::gpu::AllocatedBuffer inputDrawBuffer_{}"));
  EXPECT_FALSE(contains(shadowCullManager,
                        "objectSsboBuffers_.assign(setCount"));
}

TEST(RenderingConventionTests,
     RendererFrontendRoutesSceneObjectBuffersPerImage) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "std::vector<container::gpu::AllocatedBuffer> objects"));
  EXPECT_TRUE(
      contains(rendererFrontendHeader, "std::vector<size_t> objectCapacities"));
  EXPECT_TRUE(
      contains(rendererFrontend, "for (auto &objectBuffer : buffers_.objects)"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "buffers_.objects.assign(imageCount, {})"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "buffers_.objectCapacities.assign(imageCount, 0)"));
  EXPECT_TRUE(contains(rendererFrontend, "buffers_.objects[imageIndex]"));
  EXPECT_TRUE(
      contains(rendererFrontend, "buffers_.objectCapacities[imageIndex]"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "updateDescriptorSets(buffers_.cameras, "
                       "buffers_.objects)"));
  EXPECT_TRUE(contains(rendererFrontend, "updateObjectSsboDescriptor("));
  EXPECT_TRUE(contains(rendererFrontend,
                       "imageIndex, buffers_.objects[imageIndex].buffer"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "std::fill(buffers_.shadowObjectDescriptorReady.begin(),"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "buffers_.shadowObjectDescriptorReady.end(), false);"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.scene.objectBuffer = objectBuffer.buffer"));
  EXPECT_FALSE(contains(rendererFrontendHeader,
                        "container::gpu::AllocatedBuffer object{}"));
  EXPECT_FALSE(contains(rendererFrontend, "buffers_.object.buffer"));
}

TEST(RenderingConventionTests,
     RendererFrontendUpdatesOnlyAcquiredObjectBufferDuringFrame) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(
      contains(rendererFrontendHeader, "void updateObjectBuffer(uint32_t imageIndex)"));
  EXPECT_TRUE(contains(rendererFrontendHeader, "void updateAllObjectBuffers()"));
  EXPECT_TRUE(contains(rendererFrontendHeader, "void refreshSceneObjectData()"));
  EXPECT_TRUE(contains(rendererFrontend, "updateObjectBuffer(imageIndex);"));
  EXPECT_TRUE(contains(rendererFrontend, "updateAllObjectBuffers();"));
  EXPECT_TRUE(contains(rendererFrontend, "refreshSceneObjectData();"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "void RendererFrontend::updateObjectBuffer("
                       "uint32_t imageIndex)"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "void RendererFrontend::updateAllObjectBuffers()"));
  EXPECT_FALSE(contains(rendererFrontend, "void RendererFrontend::updateObjectBuffer()"));
}

TEST(RenderingConventionTests,
     GpuCullManagerRoutesDescriptorsPerImage) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string frustumRecorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/"
      "DeferredRasterFrustumCullPassRecorder.h");
  const std::string frustumRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullHeader, "uint32_t descriptorSetCount"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void recreatePerFrameResources(uint32_t"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void dispatchFrustumCull(VkCommandBuffer cmd,\n"
                              "                           uint32_t imageIndex"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void dispatchOcclusionCull(VkCommandBuffer cmd,\n"
                              "                             uint32_t imageIndex"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void updateObjectSsboDescriptor(uint32_t imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullHeader, "std::vector<VkDescriptorSet> frustumCullSets_"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "std::vector<VkDescriptorSet> occlusionCullSets_"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullHeader, "std::vector<VkBuffer> objectSsboBuffers_"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "frustumCullSets_[imageIndex]"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "occlusionCullSets_[imageIndex]"));
  EXPECT_TRUE(containsIgnoringWhitespace(frustumRecorderHeader, "uint32_t imageIndex{0}"));
  EXPECT_TRUE(containsIgnoringWhitespace(frustumRecorder,
                       "updateObjectSsboDescriptor(inputs.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(frustumRecorder,
                       "dispatchFrustumCull(cmd, inputs.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(deferredTechnique,
                       ".imageIndex = p.runtime.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(deferredTechnique,
                       "dispatchOcclusionCull(\n"
                       "          cmd, p.runtime.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(rendererFrontend,
                       "gpuCullManager->recreatePerFrameResources("));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "VkDescriptorSet      frustumCullSet_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "VkBuffer                        objectSsboBuffer_"));
}

TEST(RenderingConventionTests,
     GpuCullManagerOwnsCullDataBuffersPerImage) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string frustumRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string deferredScenePassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterScenePassRecorder.h");
  const std::string deferredScenePassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterScenePassRecorder.cpp");
  const std::string deferredSceneGpuCullRoutePlanner = readRepoTextFile(
      "include/Container/renderer/deferred/"
      "DeferredRasterSceneGpuCullRoutePlanner.h");
  const std::string sceneRasterPassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneRasterPassRecorder.h");
  const std::string sceneRasterPassRecorder =
      readRepoTextFile("src/renderer/scene/SceneRasterPassRecorder.cpp");
  const std::string sceneOpaqueDrawRecorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneOpaqueDrawRecorder.h");
  const std::string sceneOpaqueDrawRecorder =
      readRepoTextFile("src/renderer/scene/SceneOpaqueDrawRecorder.cpp");

  EXPECT_TRUE(contains(gpuCullHeader,
                       "void uploadDrawCommands(uint32_t imageIndex"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "void drawIndirect(VkCommandBuffer cmd, "
                       "uint32_t imageIndex) const"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "void drawIndirectOccluded(VkCommandBuffer cmd, "
                       "uint32_t imageIndex) const"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "void scheduleStatsReadback(VkCommandBuffer cmd, "
                       "uint32_t imageIndex)"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "inputDrawBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "indirectDrawBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "drawCountBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "occlusionIndirectBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "occlusionCountBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "statsReadbackBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<const DrawCommand *> "
                       "lastUploadSourceData_"));
  EXPECT_TRUE(
      contains(gpuCullHeader, "std::vector<bool> frustumDrawsValid_"));
  EXPECT_TRUE(
      contains(gpuCullHeader, "std::vector<bool> occlusionDrawsValid_"));

  EXPECT_TRUE(contains(gpuCullManager, "inputDrawBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullManager, "indirectDrawBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullManager, "drawCountBuffers_[imageIndex]"));
  EXPECT_TRUE(
      contains(gpuCullManager, "occlusionIndirectBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullManager, "occlusionCountBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullManager, "statsReadbackBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(frustumRecorder,
                       "uploadDrawCommands(inputs.imageIndex"));
  EXPECT_TRUE(
      contains(deferredTechnique,
               "scheduleStatsReadback(cmd, p.runtime.imageIndex)"));

  EXPECT_TRUE(
      contains(deferredScenePassRecorderHeader, "uint32_t imageIndex{0}"));
  EXPECT_TRUE(contains(deferredScenePassRecorder,
                       "gpuCullManager->frustumDrawsValid(inputs.imageIndex)"));
  EXPECT_TRUE(contains(deferredScenePassRecorder,
                       "gpuCullManager->occlusionDrawsValid(inputs.imageIndex)"));
  EXPECT_TRUE(contains(deferredScenePassRecorder,
                       "buildDeferredRasterSceneGpuCullRoutePlan"));
  EXPECT_TRUE(contains(deferredSceneGpuCullRoutePlanner,
                       "frustumCullActive"));
  EXPECT_TRUE(contains(deferredSceneGpuCullRoutePlanner,
                       "frustumDrawsValid"));
  EXPECT_TRUE(contains(deferredSceneGpuCullRoutePlanner,
                       "occlusionCullActive"));
  EXPECT_TRUE(contains(deferredSceneGpuCullRoutePlanner,
                       "occlusionDrawsValid"));
  EXPECT_TRUE(contains(deferredScenePassRecorder,
                       "const GpuCullManager *gpuCullManager = "
                       "inputs.gpuCullManager"));
  EXPECT_TRUE(contains(deferredScenePassRecorder,
                       ".imageIndex = inputs.imageIndex"));
  EXPECT_TRUE(
      contains(sceneRasterPassRecorderHeader, "uint32_t imageIndex{0}"));
  EXPECT_TRUE(contains(sceneRasterPassRecorder,
                       ".imageIndex = inputs.imageIndex"));
  EXPECT_TRUE(
      contains(sceneOpaqueDrawRecorderHeader, "uint32_t imageIndex{0}"));
  EXPECT_TRUE(contains(sceneOpaqueDrawRecorder,
                       "gpuCullManager.drawIndirect(cmd, imageIndex)"));
  EXPECT_TRUE(contains(sceneOpaqueDrawRecorder,
                       "gpuCullManager.drawIndirectOccluded(cmd, imageIndex)"));

  EXPECT_FALSE(contains(gpuCullHeader,
                        "container::gpu::AllocatedBuffer inputDrawBuffer_{}"));
  EXPECT_FALSE(contains(
      gpuCullHeader,
      "container::gpu::AllocatedBuffer indirectDrawBuffer_{}"));
  EXPECT_FALSE(contains(gpuCullHeader,
                        "container::gpu::AllocatedBuffer drawCountBuffer_{}"));
  EXPECT_FALSE(contains(
      gpuCullHeader,
      "container::gpu::AllocatedBuffer occlusionIndirectBuffer_{}"));
  EXPECT_FALSE(contains(
      gpuCullHeader,
      "container::gpu::AllocatedBuffer occlusionCountBuffer_{}"));
}

TEST(RenderingConventionTests,
     GpuCullManagerOwnsFrozenCullingCameraBuffersPerImage) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string frustumRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp");

  EXPECT_TRUE(contains(gpuCullHeader,
                       "void freezeCulling(uint32_t imageIndex"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "VkBuffer frozenCameraBuffer(uint32_t imageIndex) const"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "std::vector<container::gpu::AllocatedBuffer> "
                       "frozenCameraBuffers_"));
  EXPECT_TRUE(contains(gpuCullHeader, "void destroyFrozenCameraBuffers()"));

  EXPECT_TRUE(
      contains(gpuCullManager, "void GpuCullManager::destroyFrozenCameraBuffers()"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "destroyBuffers(allocationManager_, "
                       "frozenCameraBuffers_)"));
  EXPECT_TRUE(contains(gpuCullHeader, "frozenCameraBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullHeader,
                       "const auto& frozenCameraBuffer = "
                       "frozenCameraBuffers_[imageIndex]"));
  EXPECT_TRUE(contains(gpuCullManager, "frozenCameraBuffer(imageIndex)"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "auto& frozenCameraBuffer = "
                       "frozenCameraBuffers_[bufferIndex]"));
  EXPECT_TRUE(contains(gpuCullManager,
                       "for (uint32_t bufferIndex = 0; "
                       "bufferIndex < imageCount; ++bufferIndex)"));
  EXPECT_TRUE(contains(frustumRecorder,
                       "freezeCulling(inputs.imageIndex"));

  EXPECT_FALSE(contains(gpuCullHeader,
                        "container::gpu::AllocatedBuffer frozenCameraBuffer_"));
  EXPECT_FALSE(contains(gpuCullHeader, "frozenCameraBuffer_.buffer"));
  EXPECT_FALSE(contains(gpuCullManager, "frozenCameraBuffer_.buffer"));
}

TEST(RenderingConventionTests, GpuCullManagerOwnsHiZResourcesPerImage) {
  const std::string gpuCullHeader =
      readRepoTextFile("include/Container/renderer/culling/GpuCullManager.h");
  const std::string gpuCullManager =
      readRepoTextFile("src/renderer/culling/GpuCullManager.cpp");
  const std::string deferredTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");

  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void ensureHiZImage(uint32_t imageIndex"));
  EXPECT_TRUE(
      containsIgnoringWhitespace(gpuCullHeader, "void dispatchHiZGenerate(VkCommandBuffer cmd,\n"
                              "                           uint32_t imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullHeader, "struct HiZFrameResources"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullHeader, "std::vector<HiZFrameResources> hizFrames_"));

  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrames_[imageIndex]"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrame.image"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrame.fullView"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrame.mipViews"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrame.descriptorSets"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizFrame.generatedThisFrame"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "hizGeneratedThisFrame(imageIndex)"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager, "const uint32_t totalSetCount =\n"
                                      "      hizMipLevels_ * imageCount"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager,
                       "hizFrame.descriptorSets[mip]"));
  EXPECT_TRUE(containsIgnoringWhitespace(gpuCullManager,
                       "hizFrames_[imageIndex].fullView"));

  EXPECT_TRUE(containsIgnoringWhitespace(deferredTechnique,
                       "ensureHiZImage(p.runtime.imageIndex"));
  EXPECT_TRUE(containsIgnoringWhitespace(deferredTechnique,
                       "dispatchHiZGenerate(\n"
                       "        cmd, p.runtime.imageIndex"));

  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "std::vector<VkDescriptorSet> hizSets_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "VkImage              hizImage_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "VmaAllocation        hizAllocation_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "VkImageView          hizFullView_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "std::vector<VkImageView> hizMipViews_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "bool                 hizInitialized_"));
  EXPECT_FALSE(containsIgnoringWhitespace(gpuCullHeader, "bool hizGeneratedThisFrame_{false}"));
}

TEST(RenderingConventionTests, DeferredHiZDepthTransitionsUseRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterHiZPassPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterHiZPassPlanner.cpp");
  const std::string transitionHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterHiZDepthTransitionRecorder.h");
  const std::string transitionRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t hizStart =
      deferredRasterTechnique.find("RenderPassId::HiZGenerate");
  ASSERT_NE(hizStart, std::string::npos);
  const size_t hizEnd =
      deferredRasterTechnique.find("RenderPassId::OcclusionCull", hizStart);
  ASSERT_NE(hizEnd, std::string::npos);
  const std::string hizBlock =
      deferredRasterTechnique.substr(hizStart, hizEnd - hizStart);

  const size_t readinessStart =
      deferredRasterTechnique.find("RenderPassId::HiZGenerate", hizEnd);
  ASSERT_NE(readinessStart, std::string::npos);
  const size_t readinessEnd = deferredRasterTechnique.find(
      "RenderPassId::OcclusionCull", readinessStart);
  ASSERT_NE(readinessEnd, std::string::npos);
  const std::string readinessBlock = deferredRasterTechnique.substr(
      readinessStart, readinessEnd - readinessStart);

  const size_t depthToSampling =
      hizBlock.find("recordDeferredRasterHiZDepthToSamplingTransitionCommands");
  const size_t dispatch = hizBlock.find("dispatchHiZGenerate");
  const size_t depthToAttachment = hizBlock.find(
      "recordDeferredRasterHiZDepthToAttachmentTransitionCommands");
  ASSERT_NE(depthToSampling, std::string::npos);
  ASSERT_NE(dispatch, std::string::npos);
  ASSERT_NE(depthToAttachment, std::string::npos);

  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterHiZDepthTransitionRecorder.h"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "DeferredRasterHiZPassPlanner.h"));
  EXPECT_TRUE(contains(hizBlock, "buildDeferredRasterHiZPassPlan"));
  EXPECT_TRUE(contains(hizBlock, ".gpuCullManagerReady"));
  EXPECT_TRUE(contains(hizBlock, ".frameReady"));
  EXPECT_TRUE(contains(hizBlock, ".depthSamplingViewReady"));
  EXPECT_TRUE(contains(hizBlock, ".depthSamplerReady"));
  EXPECT_TRUE(contains(hizBlock, ".depthStencilImageReady"));
  EXPECT_TRUE(contains(hizBlock, "buildDeferredRasterHiZDepthTransitionPlan"));
  EXPECT_TRUE(contains(
      hizBlock, "recordDeferredRasterHiZDepthToSamplingTransitionCommands"));
  EXPECT_TRUE(contains(
      hizBlock, "recordDeferredRasterHiZDepthToAttachmentTransitionCommands"));
  EXPECT_LT(depthToSampling, dispatch);
  EXPECT_LT(dispatch, depthToAttachment);
  EXPECT_FALSE(contains(hizBlock, "VkImageMemoryBarrier"));
  EXPECT_FALSE(contains(hizBlock, "vkCmdPipelineBarrier"));
  EXPECT_FALSE(contains(hizBlock, "VK_ACCESS_"));
  EXPECT_FALSE(contains(hizBlock, "VK_PIPELINE_STAGE_"));
  EXPECT_FALSE(contains(hizBlock, "VK_IMAGE_LAYOUT_"));
  EXPECT_TRUE(contains(readinessBlock, "buildDeferredRasterHiZPassPlan"));
  EXPECT_TRUE(contains(readinessBlock, ".readiness"));
  EXPECT_FALSE(contains(readinessBlock, "renderPassNotNeeded()"));
  EXPECT_FALSE(
      contains(readinessBlock,
               "renderPassMissingResource(RenderResourceId::SceneDepth)"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterHiZPassPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterHiZPassPlan"));
  EXPECT_TRUE(contains(planner, "buildDeferredRasterHiZPassPlan"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::NotNeeded"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::MissingResource"));
  EXPECT_TRUE(contains(planner, "RenderResourceId::SceneDepth"));
  EXPECT_FALSE(contains(planner, "FrameRecorder"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "DeferredRasterTechnique"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "SwapChainManager"));
  EXPECT_FALSE(contains(planner, "GpuCullManager"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(
      contains(transitionHeader, "DeferredRasterHiZDepthTransitionInputs"));
  EXPECT_TRUE(
      contains(transitionHeader, "DeferredRasterHiZDepthTransitionPlan"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "buildDeferredRasterHiZDepthTransitionPlan"));
  EXPECT_TRUE(
      contains(transitionRecorder,
               "recordDeferredRasterHiZDepthToSamplingTransitionCommands"));
  EXPECT_TRUE(
      contains(transitionRecorder,
               "recordDeferredRasterHiZDepthToAttachmentTransitionCommands"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT"));
  EXPECT_TRUE(
      contains(transitionRecorder, "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT"));
  EXPECT_TRUE(contains(transitionRecorder, "VK_ACCESS_SHADER_READ_BIT"));
  EXPECT_TRUE(contains(transitionRecorder,
                       "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_TRUE(
      contains(transitionRecorder,
               "VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_TRUE(contains(transitionRecorder, "VK_IMAGE_ASPECT_DEPTH_BIT | "
                                           "VK_IMAGE_ASPECT_STENCIL_BIT"));
  EXPECT_FALSE(contains(transitionRecorder, "DeferredRasterTechnique"));
  EXPECT_FALSE(contains(transitionRecorder, "FrameRecorder"));
  EXPECT_FALSE(contains(transitionRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transitionRecorder, "RenderGraph"));
  EXPECT_FALSE(contains(transitionRecorder, "GpuCullManager"));
  EXPECT_FALSE(contains(transitionRecorder, "LightingManager"));
  EXPECT_FALSE(contains(transitionRecorder, "EnvironmentManager"));
  EXPECT_FALSE(contains(transitionRecorder, "OitManager"));
  EXPECT_FALSE(contains(transitionRecorder, "GuiManager"));
  EXPECT_FALSE(contains(transitionRecorder, "SwapChainManager"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredRasterHiZPassPlanner.cpp"));
  EXPECT_TRUE(contains(
      srcCmake,
      "renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_raster_hiz_pass_planner_tests"));
  EXPECT_TRUE(contains(testsCmake,
                       "deferred_raster_hiz_depth_transition_recorder_tests"));
}

TEST(RenderingConventionTests, DeferredTileCullUsesPlannerAndRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterTileCullPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterTileCullPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterTileCullRecorder.h");
  const std::string recorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterTileCullRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t passStart =
      deferredRasterTechnique.find("RenderPassId::TileCull");
  ASSERT_NE(passStart, std::string::npos);
  const size_t passEnd =
      deferredRasterTechnique.find("RenderPassId::GTAO", passStart);
  ASSERT_NE(passEnd, std::string::npos);
  const std::string tileCullBlock =
      deferredRasterTechnique.substr(passStart, passEnd - passStart);

  const size_t readinessStart = deferredRasterTechnique.find(
      "graph.setPassReadiness(RenderPassId::TileCull", passEnd);
  ASSERT_NE(readinessStart, std::string::npos);
  const size_t readinessEnd =
      deferredRasterTechnique.find("RenderPassId::GTAO", readinessStart);
  ASSERT_NE(readinessEnd, std::string::npos);
  const std::string readinessBlock = deferredRasterTechnique.substr(
      readinessStart, readinessEnd - readinessStart);

  EXPECT_TRUE(
      contains(deferredRasterTechnique, "DeferredRasterTileCullPlanner.h"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "DeferredRasterTileCullRecorder.h"));
  EXPECT_TRUE(contains(tileCullBlock, "buildDeferredRasterTileCullPlan"));
  EXPECT_TRUE(contains(tileCullBlock, "recordDeferredRasterTileCullCommands"));
  EXPECT_TRUE(contains(tileCullBlock, ".tileCullDisplayMode"));
  EXPECT_TRUE(contains(tileCullBlock, ".tiledLightingReady"));
  EXPECT_TRUE(contains(tileCullBlock, ".frameAvailable"));
  EXPECT_TRUE(contains(tileCullBlock, ".depthSamplingView"));
  EXPECT_TRUE(contains(tileCullBlock, ".cameraBuffer"));
  EXPECT_TRUE(contains(tileCullBlock, ".cameraBufferSize"));
  EXPECT_TRUE(contains(tileCullBlock, ".screenExtent"));
  EXPECT_TRUE(contains(tileCullBlock, ".cameraNear"));
  EXPECT_TRUE(contains(tileCullBlock, ".cameraFar"));
  EXPECT_FALSE(contains(tileCullBlock, "beginClusterCullTimer"));
  EXPECT_FALSE(contains(tileCullBlock, "dispatchTileCull("));
  EXPECT_FALSE(contains(tileCullBlock, "endClusterCullTimer"));
  EXPECT_TRUE(contains(readinessBlock, "buildDeferredRasterTileCullPlan"));
  EXPECT_TRUE(contains(readinessBlock, ".readiness"));
  EXPECT_FALSE(contains(readinessBlock, "renderPassNotNeeded()"));
  EXPECT_FALSE(
      contains(readinessBlock,
               "renderPassMissingResource(RenderResourceId::SceneDepth)"));

  const size_t beginTimer = recorder.find("beginClusterCullTimer");
  const size_t dispatch = recorder.find("dispatchTileCull");
  const size_t endTimer = recorder.find("endClusterCullTimer");
  ASSERT_NE(beginTimer, std::string::npos);
  ASSERT_NE(dispatch, std::string::npos);
  ASSERT_NE(endTimer, std::string::npos);
  EXPECT_LT(beginTimer, dispatch);
  EXPECT_LT(dispatch, endTimer);

  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterTileCullPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredRasterTileCullPlan"));
  EXPECT_TRUE(contains(planner, "buildDeferredRasterTileCullPlan"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::NotNeeded"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::MissingResource"));
  EXPECT_TRUE(contains(planner, "RenderResourceId::SceneDepth"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "FrameRecorder"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "SwapChainManager"));
  EXPECT_FALSE(contains(planner, "DeferredRasterTechnique"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(contains(recorderHeader, "DeferredRasterTileCullRecordInputs"));
  EXPECT_TRUE(contains(recorder, "recordDeferredRasterTileCullCommands"));
  EXPECT_FALSE(contains(recorder, "FrameRecorder"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "RenderGraph"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "SwapChainManager"));
  EXPECT_FALSE(contains(recorder, "DeferredRasterTechnique"));
  EXPECT_TRUE(contains(srcCmake,
                       "renderer/deferred/DeferredRasterTileCullPlanner.cpp"));
  EXPECT_TRUE(contains(srcCmake,
                       "renderer/deferred/DeferredRasterTileCullRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_raster_tile_cull_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "deferred_raster_tile_cull_recorder_tests"));
}

TEST(RenderingConventionTests, DeferredSceneColorReadBarriersUseRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string barrierHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterSceneColorReadBarrierRecorder.h");
  const std::string barrierRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t exposureStart =
      deferredRasterTechnique.find("RenderPassId::ExposureAdaptation");
  ASSERT_NE(exposureStart, std::string::npos);
  const size_t exposureEnd =
      deferredRasterTechnique.find("RenderPassId::OitResolve", exposureStart);
  ASSERT_NE(exposureEnd, std::string::npos);
  const std::string exposureBlock = deferredRasterTechnique.substr(
      exposureStart, exposureEnd - exposureStart);

  const size_t bloomStart = deferredRasterTechnique.find("RenderPassId::Bloom");
  ASSERT_NE(bloomStart, std::string::npos);
  const size_t bloomEnd =
      deferredRasterTechnique.find("RenderPassId::PostProcess", bloomStart);
  ASSERT_NE(bloomEnd, std::string::npos);
  const std::string bloomBlock =
      deferredRasterTechnique.substr(bloomStart, bloomEnd - bloomStart);

  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterSceneColorReadBarrierRecorder.h"));
  EXPECT_TRUE(
      contains(exposureBlock, "buildDeferredRasterSceneColorReadBarrierPlan"));
  EXPECT_TRUE(contains(exposureBlock,
                       "recordDeferredRasterSceneColorReadBarrierCommands"));
  EXPECT_TRUE(contains(exposureBlock, ".sceneColorImage"));
  EXPECT_TRUE(
      contains(bloomBlock, "buildDeferredRasterSceneColorReadBarrierPlan"));
  EXPECT_TRUE(contains(bloomBlock,
                       "recordDeferredRasterSceneColorReadBarrierCommands"));
  EXPECT_TRUE(contains(bloomBlock, ".sceneColorImage"));
  EXPECT_FALSE(contains(exposureBlock, "VkImageMemoryBarrier"));
  EXPECT_FALSE(contains(bloomBlock, "VkImageMemoryBarrier"));
  EXPECT_FALSE(contains(exposureBlock, "vkCmdPipelineBarrier"));
  EXPECT_FALSE(contains(bloomBlock, "vkCmdPipelineBarrier"));
  EXPECT_FALSE(contains(exposureBlock, "VK_ACCESS_"));
  EXPECT_FALSE(contains(bloomBlock, "VK_ACCESS_"));
  EXPECT_FALSE(contains(exposureBlock, "VK_PIPELINE_STAGE_"));
  EXPECT_FALSE(contains(bloomBlock, "VK_PIPELINE_STAGE_"));

  EXPECT_TRUE(
      contains(barrierHeader, "DeferredRasterSceneColorReadBarrierInputs"));
  EXPECT_TRUE(
      contains(barrierHeader, "DeferredRasterSceneColorReadBarrierPlan"));
  EXPECT_TRUE(contains(barrierRecorder,
                       "buildDeferredRasterSceneColorReadBarrierPlan"));
  EXPECT_TRUE(contains(barrierRecorder,
                       "recordDeferredRasterSceneColorReadBarrierCommands"));
  EXPECT_TRUE(contains(barrierRecorder, "vkCmdPipelineBarrier"));
  EXPECT_TRUE(contains(barrierRecorder,
                       "VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT"));
  EXPECT_TRUE(
      contains(barrierRecorder, "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT"));
  EXPECT_TRUE(
      contains(barrierRecorder, "VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT"));
  EXPECT_TRUE(contains(barrierRecorder, "VK_ACCESS_SHADER_READ_BIT"));
  EXPECT_TRUE(
      contains(barrierRecorder, "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL"));
  EXPECT_TRUE(contains(barrierRecorder, "VK_IMAGE_ASPECT_COLOR_BIT"));
  EXPECT_FALSE(contains(barrierRecorder, "DeferredRasterTechnique"));
  EXPECT_FALSE(contains(barrierRecorder, "FrameRecorder"));
  EXPECT_FALSE(contains(barrierRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(barrierRecorder, "RenderGraph"));
  EXPECT_FALSE(contains(barrierRecorder, "BloomManager"));
  EXPECT_FALSE(contains(barrierRecorder, "ExposureManager"));
  EXPECT_FALSE(contains(barrierRecorder, "GuiManager"));
  EXPECT_FALSE(contains(barrierRecorder, "SwapChainManager"));
  EXPECT_TRUE(contains(
      srcCmake,
      "renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.cpp"));
  EXPECT_TRUE(contains(
      testsCmake, "deferred_raster_scene_color_read_barrier_recorder_tests"));
}

TEST(RenderingConventionTests, DeferredRasterLightingUsesFrameStatePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string directionalRecorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredDirectionalLightingRecorder.h");
  const std::string directionalRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredDirectionalLightingRecorder.cpp");
  const std::string deferredLightingHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterLighting.h");
  const std::string deferredLighting =
      readRepoTextFile("src/renderer/deferred/DeferredRasterLighting.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingPass);
  const size_t directionalBranch =
      lightingBlock.find("lightingState.directionalLightingEnabled");
  ASSERT_NE(directionalBranch, std::string::npos);
  const size_t directionalBranchEnd = lightingBlock.find(
      "const std::vector<container::gpu::PointLightData>", directionalBranch);
  ASSERT_NE(directionalBranchEnd, std::string::npos);
  const std::string directionalBlock = lightingBlock.substr(
      directionalBranch, directionalBranchEnd - directionalBranch);

  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));
  EXPECT_TRUE(contains(lightingPassRecorder, "DeferredRasterLighting.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredDirectionalLightingRecorder.h"));
  EXPECT_TRUE(contains(lightingBlock, "deferredLightingFrameInputs"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredLightingFrameState"));
  EXPECT_TRUE(contains(lightingBlock, "lightingState.transparentOitEnabled"));
  EXPECT_TRUE(contains(lightingBlock, "lightingState.pointLighting"));
  EXPECT_TRUE(
      contains(lightingBlock, "recordDeferredDirectionalLightingCommands"));
  EXPECT_TRUE(contains(lightingBlock, "recordDeferredPointLightingCommands"));
  EXPECT_TRUE(contains(lightingBlock,
                       "recordDeferredDebugOverlaySurfaceNormalCommands"));
  EXPECT_FALSE(contains(lightingBlock, "lightingState.lightGizmosEnabled"));
  EXPECT_FALSE(contains(lightingBlock, "drawLightGizmos("));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->wireframeSettings()"));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->showNormalValidation()"));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->showGeometryOverlay()"));
  EXPECT_FALSE(
      contains(lightingBlock, "guiManager_->normalValidationSettings()"));
  EXPECT_FALSE(contains(lightingBlock, "const bool useTiled"));
  EXPECT_FALSE(contains(lightingBlock, "kMaxDeferredPointLights"));
  EXPECT_FALSE(contains(directionalBlock, "vkCmdBindPipeline"));
  EXPECT_FALSE(contains(directionalBlock, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(directionalBlock, "vkCmdDraw(cmd, 3, 1, 0, 0)"));

  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredPointLightingPath"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingDisplayMode"));
  EXPECT_TRUE(
      contains(deferredLightingHeader, "DeferredLightingWireframeSettings"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingFrameInputs"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingFrameState"));
  EXPECT_TRUE(contains(deferredLighting, "buildDeferredLightingFrameState"));
  EXPECT_TRUE(contains(deferredLighting, "wireframeFullMode"));
  EXPECT_TRUE(contains(deferredLighting, "objectSpaceNormalsEnabled"));
  EXPECT_TRUE(contains(deferredLighting, "pointLighting.path"));
  EXPECT_TRUE(contains(deferredLighting, "transparentOitEnabled"));
  EXPECT_TRUE(
      contains(deferredLighting, "container::gpu::kMaxDeferredPointLights"));
  EXPECT_EQ(deferredLighting.find("vkCmd"), std::string::npos);
  EXPECT_EQ(deferredLighting.find("GuiManager"), std::string::npos);
  EXPECT_EQ(deferredLighting.find("FrameRecordParams"), std::string::npos);
  EXPECT_TRUE(contains(directionalRecorderHeader,
                       "DeferredDirectionalLightingRecordInputs"));
  EXPECT_TRUE(contains(directionalRecorder,
                       "recordDeferredDirectionalLightingCommands"));
  EXPECT_TRUE(contains(directionalRecorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(directionalRecorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(directionalRecorder, "vkCmdDraw(cmd, 3, 1, 0, 0)"));
  EXPECT_FALSE(contains(directionalRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(directionalRecorder, "GuiManager"));
  EXPECT_FALSE(contains(directionalRecorder, "LightingManager"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredRasterLighting.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterLightingPassRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredDirectionalLightingRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_raster_lighting_tests"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_directional_lighting_recorder_tests"));
}

TEST(RenderingConventionTests, DeferredLightingDescriptorsUsePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredLightingDescriptorPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredLightingDescriptorPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/resources/FrameResourceManager.cpp");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingPass);

  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredLightingDescriptorPlanner.h"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredLightingDescriptorPlan"));
  EXPECT_TRUE(
      contains(lightingBlock,
               "lightingDescriptorPlan.directionalLightingDescriptorSets"));
  EXPECT_TRUE(contains(lightingBlock,
                       "lightingDescriptorPlan.pointLightingDescriptorSets"));
  EXPECT_TRUE(contains(lightingBlock,
                       "lightingDescriptorPlan.tiledLightingDescriptorSets"));
  EXPECT_FALSE(contains(lightingBlock,
                        "lightingDescriptorPlan.lightGizmoDescriptorSets"));
  EXPECT_FALSE(contains(lightingBlock,
                        "std::array<VkDescriptorSet, 3> pointLightingSets"));
  EXPECT_FALSE(
      contains(lightingBlock, "std::array<VkDescriptorSet, 3> tiledSets"));
  EXPECT_FALSE(contains(
      lightingBlock,
      "lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredLightingDescriptorPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredLightingDescriptorPlan"));
  EXPECT_TRUE(contains(planner, "directionalLightingDescriptorSets"));
  EXPECT_TRUE(contains(planner, "pointLightingDescriptorSets"));
  EXPECT_TRUE(contains(planner, "tiledLightingDescriptorSets"));
  EXPECT_FALSE(contains(plannerHeader, "lightGizmoDescriptorSets"));
  EXPECT_FALSE(contains(planner, "lightGizmoDescriptorSets"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "FrameResources"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(
      contains(frameResourceManager, "{18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"));
  EXPECT_TRUE(
      contains(frameResourceManager, "{19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE"));
  EXPECT_TRUE(
      contains(frameResourceManager, "{20, VK_DESCRIPTOR_TYPE_SAMPLER"));
  EXPECT_TRUE(contains(frameResourceManager,
                       "sizeof(container::gpu::LocalShadowData)"));
  EXPECT_TRUE(contains(frameResourceManager, "fallbackLocalShadowDataBuffer_"));
  EXPECT_TRUE(contains(frameResourceManager, "fallbackLocalShadowAtlas_"));
  EXPECT_TRUE(contains(frameResourceManager, "fallbackLocalShadowSampler_"));
  EXPECT_TRUE(contains(frameResourceManager, "VK_IMAGE_VIEW_TYPE_2D_ARRAY"));
  EXPECT_TRUE(contains(frameResourceManager, "VK_COMPARE_OP_GREATER"));
  EXPECT_TRUE(
      contains(frameResourceManager, "ensureFallbackLocalShadowDataBuffer"));
  EXPECT_TRUE(
      contains(frameResourceManager, "ensureFallbackLocalShadowResources"));
  EXPECT_FALSE(
      contains(frameResourceManager, "localShadowAtlasInfo = shadowAtlasInfo"));
  EXPECT_FALSE(contains(frameResourceManager,
                        "localShadowSamplerInfo = shadowSamplerInfo"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredLightingDescriptorPlanner.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_lighting_descriptor_planner_tests"));
}

TEST(RenderingConventionTests, DeferredLightingPassUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredLightingPassPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/deferred/DeferredLightingPassPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingPass);

  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));
  EXPECT_TRUE(contains(lightingPassRecorder, "DeferredLightingPassPlanner.h"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredLightingPassPlan"));
  EXPECT_TRUE(contains(lightingBlock, "lightingPassPlan.renderArea"));
  EXPECT_TRUE(contains(lightingBlock, "lightingPassPlan.clearValues"));
  EXPECT_TRUE(contains(lightingBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(lightingBlock, "VkClearAttachment"));
  EXPECT_FALSE(contains(lightingBlock, "VkClearRect"));
  EXPECT_FALSE(contains(lightingBlock, "VK_IMAGE_ASPECT_STENCIL_BIT"));
  EXPECT_FALSE(contains(lightingBlock, "std::array<VkClearValue, 2>"));
  EXPECT_FALSE(contains(lightingBlock, "lightingClearValues"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredLightingPassPlan"));
  EXPECT_TRUE(contains(plannerHeader, "buildDeferredLightingPassPlan"));
  EXPECT_TRUE(contains(plannerHeader, "VkClearAttachment"));
  EXPECT_TRUE(contains(plannerHeader, "VkClearRect"));
  EXPECT_TRUE(contains(planner, "renderArea.offset = {0, 0}"));
  EXPECT_TRUE(contains(planner, "renderArea.extent = framebufferExtent"));
  EXPECT_TRUE(contains(planner, "clearValues[0].color"));
  EXPECT_TRUE(contains(planner, "clearValues[1].depthStencil"));
  EXPECT_TRUE(contains(planner, "selectionStencilClearAttachment"));
  EXPECT_TRUE(contains(planner, "selectionStencilClearRect"));
  EXPECT_TRUE(contains(planner, "VK_IMAGE_ASPECT_STENCIL_BIT"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredLightingPassPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_lighting_pass_planner_tests"));
}

TEST(RenderingConventionTests, DeferredLightGizmosUseRecorder) {
  const std::string lightingManagerHeader =
      readRepoTextFile("include/Container/renderer/lighting/LightingManager.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");
  const std::string pushConstantsHeader = readRepoTextFile(
      "include/Container/renderer/lighting/LightPushConstants.h");
  const std::string pushConstantBlock =
      readRepoTextFile("include/Container/renderer/core/PushConstantBlock.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredLightGizmoPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/deferred/DeferredLightGizmoPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredLightGizmoRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/deferred/DeferredLightGizmoRecorder.cpp");
  const std::string shader = readRepoTextFile("shaders/light_gizmo.slang");
  const std::string pickShader =
      readRepoTextFile("shaders/light_gizmo_pick.slang");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineRegistry =
      readRepoTextFile("src/renderer/pipeline/PipelineRegistry.cpp");
  const std::string rasterPipelineBridge = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterPipelineBridge.h");
  const std::string transparentPickRasterHeader = readRepoTextFile(
      "include/Container/renderer/picking/TransparentPickRasterPassRecorder.h");
  const std::string transparentPickRaster = readRepoTextFile(
      "src/renderer/picking/TransparentPickRasterPassRecorder.cpp");
  const std::string rasterLightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string allocationManagerHeader =
      readRepoTextFile("include/Container/utility/AllocationManager.h");
  const std::string allocationManager =
      readRepoTextFile("src/utility/AllocationManager.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t drawGizmosStart =
      lightingManager.find("void LightingManager::drawLightGizmos");
  ASSERT_NE(drawGizmosStart, std::string::npos);
  const size_t drawGizmosEnd =
      lightingManager.find("// "
                           "---------------------------------------------------"
                           "------------------------",
                           drawGizmosStart);
  ASSERT_NE(drawGizmosEnd, std::string::npos);
  const std::string drawGizmosBlock =
      lightingManager.substr(drawGizmosStart, drawGizmosEnd - drawGizmosStart);

  EXPECT_TRUE(contains(lightingManager, "DeferredLightGizmoPlanner.h"));
  EXPECT_TRUE(contains(lightingManager, "DeferredLightGizmoRecorder.h"));
  EXPECT_TRUE(contains(drawGizmosBlock, "buildDeferredLightGizmoPlan"));
  EXPECT_TRUE(contains(drawGizmosBlock, "editableLights_"));
  EXPECT_FALSE(contains(drawGizmosBlock, "pointLightsSsbo_.data()"));
  EXPECT_TRUE(contains(drawGizmosBlock, "recordDeferredLightGizmoCommands"));
  EXPECT_FALSE(contains(drawGizmosBlock, "gizmoPushConstants"));
  EXPECT_FALSE(contains(drawGizmosBlock, "kMaxVisibleLightGizmos + 1u"));
  EXPECT_FALSE(contains(drawGizmosBlock, "computeGizmoExtent"));
  EXPECT_FALSE(contains(drawGizmosBlock, "dirMax"));
  EXPECT_FALSE(contains(drawGizmosBlock, "maxCh"));
  EXPECT_FALSE(contains(drawGizmosBlock, "vkCmdBindPipeline"));
  EXPECT_FALSE(contains(drawGizmosBlock, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(drawGizmosBlock, "vkCmdPushConstants"));
  EXPECT_FALSE(contains(drawGizmosBlock, "vkCmdDraw"));

  EXPECT_TRUE(contains(lightingManagerHeader, "LightPushConstants.h"));
  EXPECT_FALSE(contains(lightingManagerHeader, "struct LightPushConstants"));
  EXPECT_TRUE(contains(pushConstantsHeader, "struct LightPushConstants"));
  EXPECT_TRUE(contains(pushConstantsHeader, "offsetof(LightPushConstants"));
  EXPECT_TRUE(contains(pushConstantBlock, "LightPushConstants.h"));
  EXPECT_FALSE(contains(pushConstantBlock, "LightingManager.h"));
  EXPECT_TRUE(contains(pipelineBuilder, "LightPushConstants.h"));
  EXPECT_FALSE(contains(pipelineBuilder, "LightingManager.h"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredLightGizmoPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredLightGizmoPlan"));
  EXPECT_FALSE(contains(plannerHeader, "DeferredLightGizmoVisual"));
  EXPECT_FALSE(contains(plannerHeader, "pickDeferredLightGizmoAtCursor"));
  EXPECT_TRUE(contains(plannerHeader, "encodeEditableLightPickId"));
  EXPECT_TRUE(contains(plannerHeader, "decodeEditableLightPickId"));
  EXPECT_TRUE(contains(plannerHeader, "kMaxDeferredLightGizmos"));
  EXPECT_TRUE(contains(planner, "buildDeferredLightGizmoPlan"));
  EXPECT_TRUE(contains(planner, "normalizedDisplayColor"));
  EXPECT_TRUE(contains(planner, "gizmoExtentForPosition"));
  EXPECT_TRUE(contains(planner, "directionalDirection"));
  EXPECT_TRUE(contains(planner, "kMaxDeferredLightGizmos"));
  EXPECT_TRUE(contains(planner, "padding2"));
  EXPECT_TRUE(contains(planner, "encodeEditableLightPickId(light.id)"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "VkPipeline"));
  EXPECT_FALSE(contains(planner, "VkDescriptorSet"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "World::"));
  EXPECT_TRUE(contains(recorderHeader, "DeferredLightGizmoRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "std::span<const LightPushConstants>"));
  EXPECT_TRUE(contains(recorder, "recordDeferredLightGizmoCommands"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(recorder, "vkCmdPushConstants"));
  EXPECT_TRUE(contains(recorder, "vkCmdDraw"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "LightingManager"));
  EXPECT_TRUE(contains(shader, "Texture2DArray"));
  EXPECT_TRUE(contains(shader, "uLightGizmoIcons"));
  EXPECT_TRUE(contains(shader, "uLightGizmoSampler"));
  EXPECT_TRUE(contains(shader, "unlitGizmoColor"));
  EXPECT_FALSE(contains(shader, "ResolveExposureForToneMapping"));
  EXPECT_FALSE(contains(shader, "uIrradiance"));
  EXPECT_FALSE(contains(shader, "uPrefiltered"));
  EXPECT_TRUE(contains(shader, "float2 uv"));
  EXPECT_TRUE(contains(shader, "kCoveragePoint"));
  EXPECT_TRUE(contains(shader, "kCoverageSpot"));
  EXPECT_TRUE(contains(shader, "kCoverageArea"));
  EXPECT_TRUE(contains(shader, "kCoverageDirectional"));
  EXPECT_TRUE(contains(shader, "directionalCoveragePosition"));
  EXPECT_TRUE(contains(shader, "coverageLinePosition"));
  EXPECT_FALSE(contains(shader, "kAxisVertices"));
  EXPECT_TRUE(contains(pipelineTypes, "lightGizmo"));
  EXPECT_TRUE(contains(pipelineTypes, "lightGizmoCoverage"));
  EXPECT_TRUE(contains(pipelineTypes, "lightGizmoPick"));
  EXPECT_TRUE(contains(pipelineTypes, "VkDescriptorSetLayout lightGizmo"));
  EXPECT_TRUE(contains(pipelineBuilder, "layouts.lightGizmo"));
  EXPECT_TRUE(contains(pipelineBuilder, "descriptorLayouts.lightGizmo"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "lgPCI.pInputAssemblyState = &triAssembly"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "lgPCI.renderPass = renderPasses.postProcess"));
  EXPECT_TRUE(contains(pipelineBuilder, "lgCoveragePCI"));
  EXPECT_TRUE(contains(pipelineBuilder, "lgPickPCI"));
  EXPECT_TRUE(contains(pipelineBuilder, "renderPasses.transparentPick"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "lgCoveragePCI.pInputAssemblyState = &lineAssembly"));
  EXPECT_TRUE(contains(pipelineRegistry, "\"light-gizmo-pick\""));
  EXPECT_TRUE(contains(pipelineRegistry,
                       "registerLayoutIfPresent(*registry, \"light-gizmo\""));
  EXPECT_TRUE(contains(rasterPipelineBridge, "DeferredRasterPipelineLayoutId"));
  EXPECT_TRUE(contains(rasterPipelineBridge, "LightGizmo"));
  EXPECT_TRUE(contains(rasterPipelineBridge, "LightGizmoPick"));
  EXPECT_TRUE(contains(rasterPipelineBridge, "return \"light-gizmo\""));
  EXPECT_TRUE(contains(rasterPipelineBridge, "return \"light-gizmo-pick\""));
  EXPECT_FALSE(contains(rasterLightingPassRecorder,
                        "DeferredRasterPipelineLayoutId::LightGizmo"));
  EXPECT_FALSE(contains(rasterLightingPassRecorder, "drawLightGizmos("));
  EXPECT_TRUE(contains(pickShader, "Texture2DArray"));
  EXPECT_TRUE(contains(pickShader, "uint fragMain"));
  EXPECT_TRUE(contains(pickShader, "discard"));
  EXPECT_TRUE(contains(pickShader, "pc.padding2"));
  EXPECT_TRUE(contains(transparentPickRasterHeader, "recordAfterGeometry"));
  EXPECT_TRUE(contains(transparentPickRasterHeader, "extraPassWorkActive"));
  EXPECT_TRUE(contains(transparentPickRaster, "recordAfterGeometry"));
  EXPECT_TRUE(contains(transparentPickRaster, "extraPassWorkActive"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "LightGizmoPick"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "drawLightGizmoPickIds"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "deferredRasterLightGizmoOverlayReady"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "recordDeferredRasterLightGizmoOverlay"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredRasterDescriptorSetId::FrameLighting"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "showLightGizmos()"));
  EXPECT_TRUE(
      contains(lightingManagerHeader, "lightGizmoIconDescriptorSetLayout"));
  EXPECT_TRUE(contains(lightingManagerHeader, "lightGizmoIconsReady"));
  EXPECT_TRUE(contains(lightingManager, "LightGizmoIconAtlas.h"));
  EXPECT_TRUE(contains(lightingManager, "loadLightGizmoIconAtlasRgba"));
  EXPECT_TRUE(contains(lightingManager, "lightGizmoIconDescriptorSet_"));
  EXPECT_TRUE(contains(lightingManager, "lightGizmoCoveragePipeline"));
  EXPECT_TRUE(
      contains(allocationManagerHeader, "createTexture2DArrayFromRgbaPixels"));
  EXPECT_TRUE(contains(allocationManagerHeader, "TextureAllocationLifetime"));
  EXPECT_TRUE(contains(allocationManager, "TextureAllocationResetScope::All"));
  EXPECT_TRUE(contains(allocationManager,
                       "texture.lifetime == TextureAllocationLifetime::Scene"));
  EXPECT_TRUE(contains(allocationManager,
                       "std::erase_if(textureAllocations_, shouldReset)"));
  EXPECT_TRUE(
      contains(lightingManager, "TextureAllocationLifetime::Persistent"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredLightGizmoPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/deferred/DeferredLightGizmoRecorder.cpp"));
  EXPECT_TRUE(contains(srcCmake, "renderer/lighting/LightGizmoIconAtlas.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_light_gizmo_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "light_gizmo_icon_atlas_tests"));
  EXPECT_TRUE(contains(testsCmake, "deferred_light_gizmo_recorder_tests"));

  const std::array<std::filesystem::path, 4> lightSvgAssets = {
      "materials/gizmos/lights/directional.svg",
      "materials/gizmos/lights/point.svg",
      "materials/gizmos/lights/spot.svg",
      "materials/gizmos/lights/area.svg",
  };
  for (const std::filesystem::path &asset : lightSvgAssets) {
    EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / asset))
        << asset.generic_string();
  }
}

TEST(RenderingConventionTests,
     RenderPreviewModeSuppressesEditorDebugViewportOverlays) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string forwardRasterTechnique =
      readRepoTextFile("src/renderer/forward/ForwardRasterTechnique.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "enum class SceneViewportMode"));
  EXPECT_TRUE(contains(guiManagerHeader, "SceneViewportMode::Editor"));
  EXPECT_TRUE(contains(guiManagerHeader, "SceneViewportMode::RenderPreview"));
  EXPECT_TRUE(contains(guiManagerHeader, "editorOverlaysEnabled()"));
  EXPECT_TRUE(contains(guiManagerHeader, "setSceneViewportMode"));
  EXPECT_TRUE(contains(guiManager, "Viewport Mode"));
  EXPECT_TRUE(contains(guiManager, "Render Preview"));
  EXPECT_TRUE(
      contains(guiManager, "ImGui::BeginDisabled(!editorOverlaysEnabled())"));

  EXPECT_TRUE(
      contains(lightingPassRecorder, "guiManager->editorOverlaysEnabled()"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "inputs.wireframeSettings.enabled = false"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "inputs.normalValidationSettings.enabled = false"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "inputs.geometryOverlayRequested = false"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "inputs.debugVisualizePointLightStencil = false"));

  EXPECT_TRUE(contains(deferredRasterTechnique, "showLightGizmos()"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      deferredRasterTechnique,
      "if (guiManager == nullptr || !guiManager->showLightGizmos())"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      forwardRasterTechnique,
      "if (lightingManager == nullptr || guiManager == nullptr || "
      "!guiManager->showLightGizmos())"));
  EXPECT_TRUE(
      contains(rendererFrontend, "subs_.guiManager->editorOverlaysEnabled()"));
  EXPECT_TRUE(contains(rendererFrontend, "pickTransformGizmoAxisAtCursor"));
}

TEST(RenderingConventionTests,
     ViewportInteractionControlsUseCompactUnsavedWindowState) {
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  const size_t controlsStart =
      guiManager.find("void GuiManager::drawViewportInteractionControls");
  ASSERT_NE(controlsStart, std::string::npos);
  const size_t controlsEnd =
      guiManager.find("void GuiManager::drawViewportNavigationOverlay",
                      controlsStart);
  ASSERT_NE(controlsEnd, std::string::npos);
  const std::string controlsBlock =
      guiManager.substr(controlsStart, controlsEnd - controlsStart);

  EXPECT_TRUE(contains(controlsBlock, "kViewportControlsMinWidth"));
  EXPECT_TRUE(contains(controlsBlock, "kViewportControlsMaxWidth"));
  EXPECT_TRUE(contains(controlsBlock, "ImGuiWindowFlags_AlwaysAutoResize"));
  EXPECT_TRUE(contains(controlsBlock, "ImGuiWindowFlags_NoSavedSettings"));
  EXPECT_TRUE(contains(controlsBlock, "ImGui::SetNextWindowSizeConstraints"));
  EXPECT_TRUE(contains(controlsBlock, "ImGui::Begin(\"Viewport\""));
}

TEST(RenderingConventionTests,
     ViewportNavigationOverlayUsesCompactOptionsControl) {
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  const size_t navigationStart =
      guiManager.find("void GuiManager::drawViewportNavigationOverlay");
  ASSERT_NE(navigationStart, std::string::npos);
  const size_t navigationEnd =
      guiManager.find("void GuiManager::drawSceneControls", navigationStart);
  ASSERT_NE(navigationEnd, std::string::npos);
  const std::string navigationBlock =
      guiManager.substr(navigationStart, navigationEnd - navigationStart);

  EXPECT_TRUE(contains(navigationBlock, "kViewportNavOptionsButtonSize"));
  EXPECT_TRUE(contains(navigationBlock, "##viewport-nav-options-button"));
  EXPECT_TRUE(contains(navigationBlock, "ShowItemTooltip(\"Navigation options\")"));
  EXPECT_FALSE(contains(navigationBlock, "\"Options v\""));
}

TEST(RenderingConventionTests, DeferredPointLightingDrawPlanningUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredPointLightingDrawPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingDrawPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredPointLightingRecorder.h");
  const std::string recorder = readRepoTextFile(
      "src/renderer/deferred/DeferredPointLightingRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingPass);

  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredPointLightingDrawPlanner.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredPointLightingRecorder.h"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredPointLightingDrawPlan"));
  EXPECT_TRUE(contains(lightingBlock, "recordDeferredPointLightingCommands"));
  EXPECT_TRUE(contains(recorder, "plan.tiledPushConstants"));
  EXPECT_TRUE(contains(recorder, "plan.stencilRoutes"));
  EXPECT_TRUE(contains(recorder, "plan.stencilRouteCount"));
  EXPECT_TRUE(contains(recorder, "pipelineForDeferredPointLighting"));
  EXPECT_TRUE(contains(recorder, "beginClusteredLightingTimer"));
  EXPECT_TRUE(contains(recorder, "endClusteredLightingTimer"));
  EXPECT_TRUE(contains(recorder, "vkCmdClearAttachments"));
  EXPECT_FALSE(contains(lightingBlock, "beginClusteredLightingTimer"));
  EXPECT_FALSE(contains(lightingBlock, "pointLightingPlan.stencilRoutes"));
  EXPECT_FALSE(contains(lightingBlock, "TiledLightingPushConstants tlpc"));
  EXPECT_FALSE(contains(lightingBlock, "kTileSize"));
  EXPECT_FALSE(contains(lightingBlock, "kClusterDepthSlices"));
  EXPECT_FALSE(
      contains(lightingBlock, "lightingState.pointLighting.stencilLightCount"));

  EXPECT_TRUE(contains(plannerHeader, "DeferredPointLightingDrawInputs"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredPointLightingDrawPlan"));
  EXPECT_TRUE(contains(plannerHeader, "DeferredPointLightingStencilRoute"));
  EXPECT_TRUE(contains(planner, "tileCount"));
  EXPECT_TRUE(contains(planner, "container::gpu::kTileSize"));
  EXPECT_TRUE(contains(planner, "container::gpu::kClusterDepthSlices"));
  EXPECT_TRUE(contains(planner, "container::gpu::kMaxDeferredPointLights"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "VkPipeline"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "DebugOverlayRenderer"));
  EXPECT_TRUE(contains(recorderHeader, "DeferredPointLightingRecordInputs"));
  EXPECT_TRUE(contains(recorder, "recordDeferredPointLightingCommands"));
  EXPECT_TRUE(contains(srcCmake,
                       "renderer/deferred/DeferredPointLightingRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredPointLightingDrawPlanner.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "deferred_point_lighting_draw_planner_tests"));
}

TEST(RenderingConventionTests, SceneOpaqueDrawPlanningUsesPlanner) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string deferredScenePassRecorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/DeferredRasterScenePassRecorder.h");
  const std::string deferredScenePassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterScenePassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneOpaqueDrawPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/scene/SceneOpaqueDrawPlanner.cpp");
  const std::string rasterPassPlannerHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneRasterPassPlanner.h");
  const std::string rasterPassPlanner =
      readRepoTextFile("src/renderer/scene/SceneRasterPassPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneOpaqueDrawRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/scene/SceneOpaqueDrawRecorder.cpp");
  const std::string diagnosticRecorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneDiagnosticCubeRecorder.h");
  const std::string diagnosticRecorder =
      readRepoTextFile("src/renderer/scene/SceneDiagnosticCubeRecorder.cpp");
  const std::string rasterPassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneRasterPassRecorder.h");
  const std::string rasterPassRecorder =
      readRepoTextFile("src/renderer/scene/SceneRasterPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t scenePassStart = deferredScenePassRecorder.find(
      "bool recordDeferredRasterScenePassCommands");
  ASSERT_NE(scenePassStart, std::string::npos);
  const std::string scenePassBlock =
      deferredScenePassRecorder.substr(scenePassStart);

  EXPECT_FALSE(contains(frameRecorder, "SceneOpaqueDrawPlanner.h"));
  EXPECT_FALSE(contains(frameRecorder, "SceneRasterPassPlanner.h"));
  EXPECT_FALSE(contains(frameRecorder, "SceneRasterPassRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "DeferredRasterScenePassRecorder.h"));
  EXPECT_FALSE(
      contains(frameRecorderHeader, "DeferredRasterScenePassRecordInputs"));
  EXPECT_FALSE(
      contains(frameRecorder, "recordDeferredRasterScenePassCommands"));
  EXPECT_FALSE(contains(frameRecorder, "sceneOpaqueDrawLists"));
  EXPECT_FALSE(contains(frameRecorder, "sceneOpaqueGpuIndirectAvailable"));
  EXPECT_FALSE(contains(frameRecorderHeader, "recordDepthPrepass"));
  EXPECT_FALSE(contains(frameRecorderHeader, "recordGBufferPass"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordDepthPrepass"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordGBufferPass"));
  EXPECT_FALSE(contains(frameRecorder, "pipelineForSceneOpaqueRoute"));
  EXPECT_FALSE(contains(frameRecorderHeader, "drawDiagnosticCube"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::drawDiagnosticCube"));
  EXPECT_TRUE(contains(deferredScenePassRecorderHeader,
                       "DeferredRasterScenePassRecordInputs"));
  EXPECT_TRUE(contains(deferredScenePassRecorderHeader,
                       "SceneRasterPassPipelineInputs"));
  EXPECT_TRUE(
      contains(deferredScenePassRecorder, "sceneOpaqueGpuIndirectAvailable"));
  EXPECT_TRUE(contains(scenePassBlock, "buildSceneRasterPassPlan"));
  EXPECT_TRUE(contains(scenePassBlock, "recordSceneRasterPassCommands"));
  EXPECT_FALSE(contains(deferredScenePassRecorderHeader, "FrameRecordParams"));
  EXPECT_FALSE(contains(deferredScenePassRecorderHeader, "FrameRecorder.h"));
  EXPECT_FALSE(contains(deferredScenePassRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(deferredScenePassRecorder, "FrameRecorder.h"));
  EXPECT_FALSE(contains(deferredScenePassRecorder, "SceneController"));
  EXPECT_FALSE(contains(scenePassBlock, "buildSceneOpaqueDrawPlan"));
  EXPECT_FALSE(contains(scenePassBlock, "sceneRasterPassClearValues"));
  EXPECT_FALSE(contains(scenePassBlock, "RenderingBeginInfo"));
  EXPECT_FALSE(contains(scenePassBlock, "beginDynamicRendering"));
  EXPECT_FALSE(contains(scenePassBlock, "endDynamicRendering"));
  EXPECT_FALSE(contains(scenePassBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(scenePassBlock, "recordSceneOpaqueDrawCommands"));
  EXPECT_FALSE(contains(scenePassBlock, "recordSceneDiagnosticCubeCommands"));
  EXPECT_FALSE(contains(scenePassBlock, "gpuCullManager_->isReady()"));
  EXPECT_FALSE(
      contains(scenePassBlock, "gpuCullManager_->frustumDrawsValid()"));
  EXPECT_FALSE(contains(scenePassBlock, "gpuCullManager_->drawIndirect"));
  EXPECT_FALSE(contains(scenePassBlock, "debugOverlay_.drawScene"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdBindPipeline"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdBindVertexBuffers"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdBindIndexBuffer"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdPushConstants"));
  EXPECT_FALSE(contains(scenePassBlock, "vkCmdDrawIndexed"));
  EXPECT_FALSE(contains(scenePassBlock, "pipelineForSceneOpaqueRoute"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterScenePassInputs"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "SceneRasterPassKind::DepthPrepass"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "SceneRasterPassKind::GBuffer"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterSceneOpaqueDrawLists"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterScenePipelineInputs"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "deferredRasterSceneDiagnosticCubeInputs"));

  EXPECT_TRUE(contains(plannerHeader, "SceneOpaqueDrawPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "SceneOpaqueDrawRouteKind"));
  EXPECT_TRUE(contains(planner, "GpuIndirectSingleSided"));
  EXPECT_TRUE(contains(planner, "CpuWindingFlipped"));
  EXPECT_TRUE(contains(planner, "CpuDoubleSided"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "VkPipeline"));
  EXPECT_FALSE(contains(planner, "GpuCullManager"));
  EXPECT_FALSE(contains(planner, "FrameRecorder"));
  EXPECT_FALSE(contains(planner, "RenderPass"));
  EXPECT_TRUE(contains(srcCmake, "renderer/scene/SceneOpaqueDrawPlanner.cpp"));
  EXPECT_TRUE(contains(rasterPassPlannerHeader, "SceneRasterPassPlanInputs"));
  EXPECT_TRUE(
      contains(rasterPassPlannerHeader, "SceneRasterPassPipelineInputs"));
  EXPECT_TRUE(contains(rasterPassPlanner, "buildSceneRasterPassPlan"));
  EXPECT_TRUE(contains(rasterPassPlanner, "buildSceneOpaqueDrawPlan"));
  EXPECT_TRUE(contains(rasterPassPlanner, "sceneRasterPassClearValues"));
  EXPECT_TRUE(contains(rasterPassPlanner, "choosePipeline"));
  EXPECT_FALSE(contains(rasterPassPlanner, "vkCmd"));
  EXPECT_FALSE(contains(rasterPassPlanner, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterPassPlanner, "FrameRecorder"));
  EXPECT_FALSE(contains(rasterPassPlanner, "GpuCullManager"));
  EXPECT_FALSE(contains(rasterPassPlanner, "DebugOverlayRenderer"));
  EXPECT_FALSE(contains(rasterPassPlanner, "GuiManager"));
  EXPECT_TRUE(contains(srcCmake, "renderer/scene/SceneRasterPassPlanner.cpp"));
  EXPECT_TRUE(contains(recorderHeader, "SceneOpaqueDrawRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "SceneOpaqueDrawPipelineHandles"));
  EXPECT_TRUE(contains(recorder, "pipelineForSceneOpaqueRoute"));
  EXPECT_TRUE(contains(recorder, "recordSceneOpaqueDrawCommands"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(contains(recorder, "drawGpuIndirectRoute"));
  EXPECT_TRUE(contains(recorder, "gpuCullManager.drawIndirect"));
  EXPECT_TRUE(contains(recorder, "gpuCullManager.drawIndirectOccluded"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->drawScene"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "BimSurface"));
  EXPECT_FALSE(contains(recorder, "BimManager"));
  EXPECT_FALSE(contains(recorder, "LightingManager"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "RenderPass"));
  EXPECT_FALSE(contains(recorder, "beginDynamicRendering"));
  EXPECT_TRUE(
      contains(diagnosticRecorderHeader, "SceneDiagnosticCubeRecordInputs"));
  EXPECT_TRUE(
      contains(diagnosticRecorder, "recordSceneDiagnosticCubeCommands"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdPushConstants"));
  EXPECT_TRUE(contains(diagnosticRecorder, "vkCmdDrawIndexed"));
  EXPECT_FALSE(contains(diagnosticRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(diagnosticRecorder, "SceneController"));
  EXPECT_FALSE(contains(diagnosticRecorder, "DebugOverlayRenderer"));
  EXPECT_FALSE(contains(diagnosticRecorder, "BimManager"));
  EXPECT_FALSE(contains(diagnosticRecorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(diagnosticRecorder, "endDynamicRendering"));
  EXPECT_TRUE(contains(rasterPassRecorderHeader, "SceneRasterPassKind"));
  EXPECT_TRUE(contains(rasterPassRecorderHeader, "SceneRasterPassClearValues"));
  EXPECT_TRUE(
      contains(rasterPassRecorderHeader, "SceneRasterPassRecordInputs"));
  EXPECT_TRUE(contains(rasterPassRecorder, "recordSceneRasterPassCommands"));
  EXPECT_TRUE(contains(rasterPassRecorder, "sceneRasterPassClearValues"));
  EXPECT_TRUE(contains(rasterPassRecorder, "beginDynamicRendering"));
  EXPECT_TRUE(contains(rasterPassRecorder, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(rasterPassRecorder, "recordSceneOpaqueDrawCommands"));
  EXPECT_TRUE(
      contains(rasterPassRecorder, "recordSceneDiagnosticCubeCommands"));
  EXPECT_TRUE(contains(rasterPassRecorder, "endDynamicRendering"));
  EXPECT_FALSE(contains(rasterPassRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterPassRecorder, "FrameRecorder.h"));
  EXPECT_FALSE(contains(rasterPassRecorder, "GuiManager"));
  EXPECT_FALSE(contains(rasterPassRecorder, "LightingManager"));
  EXPECT_FALSE(contains(rasterPassRecorder, "BimManager"));
  EXPECT_FALSE(contains(rasterPassRecorder, "Shadow"));
  EXPECT_FALSE(contains(rasterPassRecorder, "RenderPassId"));
  EXPECT_FALSE(contains(rasterPassRecorder, "buildSceneOpaqueDrawPlan"));
  EXPECT_TRUE(contains(srcCmake, "renderer/scene/SceneOpaqueDrawRecorder.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/scene/SceneDiagnosticCubeRecorder.cpp"));
  EXPECT_TRUE(contains(srcCmake, "renderer/scene/SceneRasterPassRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/deferred/DeferredRasterScenePassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "scene_opaque_draw_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "scene_opaque_draw_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake, "scene_raster_pass_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "scene_raster_pass_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake, "scene_diagnostic_cube_recorder_tests"));
}

TEST(RenderingConventionTests, SceneTransparentDrawPlanningUsesPlanner) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneTransparentDrawPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/scene/SceneTransparentDrawPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/scene/SceneTransparentDrawRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/scene/SceneTransparentDrawRecorder.cpp");
  const std::string transparentOitRecorderHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredTransparentOitRecorder.h");
  const std::string transparentOitRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredTransparentOitRecorder.cpp");
  const std::string transparentOitFramePassRecorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredTransparentOitFramePassRecorder.h");
  const std::string transparentOitFramePassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredTransparentOitFramePassRecorder.cpp");
  const std::string transparentPickRecorderHeader = readRepoTextFile(
      "include/Container/renderer/picking/TransparentPickPassRecorder.h");
  const std::string transparentPickRecorder =
      readRepoTextFile("src/renderer/picking/TransparentPickPassRecorder.cpp");
  const std::string transparentPickRasterRecorderHeader = readRepoTextFile(
      "include/Container/renderer/picking/TransparentPickRasterPassRecorder.h");
  const std::string transparentPickRasterRecorder = readRepoTextFile(
      "src/renderer/picking/TransparentPickRasterPassRecorder.cpp");
  const std::string transparentPickDepthCopyRecorderHeader = readRepoTextFile(
      "include/Container/renderer/picking/TransparentPickDepthCopyRecorder.h");
  const std::string transparentPickDepthCopyRecorder = readRepoTextFile(
      "src/renderer/picking/TransparentPickDepthCopyRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t pickStart = transparentPickRasterRecorder.find(
      "bool recordTransparentPickFramePassCommands");
  ASSERT_NE(pickStart, std::string::npos);
  const size_t pickEnd =
      transparentPickRasterRecorder.find("} // namespace", pickStart);
  ASSERT_NE(pickEnd, std::string::npos);
  const size_t lightingStart = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingStart, std::string::npos);
  const std::string pickBlock =
      transparentPickRasterRecorder.substr(pickStart, pickEnd - pickStart);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingStart);
  const size_t transparentDrawStart =
      lightingBlock.find("if (transparentOitEnabled)");
  ASSERT_NE(transparentDrawStart, std::string::npos);
  const size_t transparentDrawEnd = lightingBlock.find(
      "recordDeferredDebugOverlayGeometryCommands", transparentDrawStart);
  ASSERT_NE(transparentDrawEnd, std::string::npos);
  const std::string transparentDrawBlock = lightingBlock.substr(
      transparentDrawStart, transparentDrawEnd - transparentDrawStart);

  EXPECT_FALSE(contains(frameRecorder, "SceneTransparentDrawPlanner.h"));
  EXPECT_FALSE(contains(frameRecorder, "SceneTransparentDrawRecorder.h"));
  EXPECT_FALSE(contains(frameRecorderHeader, "clearOitResources"));
  EXPECT_FALSE(contains(frameRecorderHeader, "prepareOitResolve"));
  EXPECT_FALSE(contains(frameRecorderHeader, "transparentOitEnabled"));
  EXPECT_FALSE(contains(frameRecorderHeader, "const OitManager *oitManager"));
  EXPECT_FALSE(contains(frameRecorder, "clearOitResources"));
  EXPECT_FALSE(contains(frameRecorder, "prepareOitResolve"));
  EXPECT_FALSE(contains(frameRecorder, "oitManager_.clearResources"));
  EXPECT_FALSE(contains(frameRecorder, "oitManager_.prepareResolve"));
  EXPECT_FALSE(contains(frameRecorder, "shouldRecordTransparentOit"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "DeferredTransparentOitRecorder.h"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "DeferredTransparentOitFramePassRecorder.h"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "transparentOit.recordClear"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "transparentOit.recordResolvePreparation"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "transparentOit.readiness"));
  EXPECT_FALSE(contains(deferredRasterTechnique,
                        "recordDeferredTransparentOitClearCommands"));
  EXPECT_FALSE(
      contains(deferredRasterTechnique,
               "recordDeferredTransparentOitResolvePreparationCommands"));
  EXPECT_FALSE(contains(frameRecorder, "TransparentPickDepthCopyRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "TransparentPickRasterPassRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "sceneTransparentDrawLists"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "recordTransparentPickFramePassCommands"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "deferredRasterSceneTransparentDrawLists"));
  EXPECT_FALSE(contains(frameRecorder, "pipelineForSceneTransparentRoute"));
  EXPECT_TRUE(contains(pickBlock, "buildSceneTransparentDrawPlan"));
  EXPECT_TRUE(contains(pickBlock, "buildTransparentPickDepthCopyPlan"));
  EXPECT_TRUE(contains(lightingBlock, "buildSceneTransparentDrawPlan"));
  EXPECT_TRUE(contains(pickBlock, "recordTransparentPickDepthCopyCommands"));
  EXPECT_TRUE(contains(pickBlock, "recordTransparentPickRasterPassCommands"));
  const size_t depthCopyCall =
      pickBlock.find("recordTransparentPickDepthCopyCommands");
  const size_t rasterPickCall =
      pickBlock.find("recordTransparentPickRasterPassCommands");
  ASSERT_NE(depthCopyCall, std::string::npos);
  ASSERT_NE(rasterPickCall, std::string::npos);
  EXPECT_LT(depthCopyCall, rasterPickCall);
  EXPECT_FALSE(contains(pickBlock, "RenderingBeginInfo"));
  EXPECT_FALSE(contains(pickBlock, "beginDynamicRendering"));
  EXPECT_FALSE(contains(pickBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(pickBlock, "recordTransparentPickPassCommands"));
  EXPECT_FALSE(contains(pickBlock, "endDynamicRendering"));
  EXPECT_FALSE(contains(pickBlock, "recordSceneTransparentDrawCommands"));
  EXPECT_TRUE(contains(lightingBlock, "recordDeferredTransparentOitCommands"));
  EXPECT_FALSE(
      contains(transparentDrawBlock, "recordSceneTransparentDrawCommands"));
  EXPECT_FALSE(contains(pickBlock, "drawTransparentPickLists"));
  EXPECT_FALSE(contains(lightingBlock, "drawTransparentLists"));
  EXPECT_FALSE(contains(pickBlock, "pipelineForSceneTransparentRoute"));
  EXPECT_FALSE(
      contains(transparentDrawBlock, "pipelineForSceneTransparentRoute"));
  EXPECT_FALSE(contains(pickBlock, "debugOverlay_.drawScene"));
  EXPECT_FALSE(contains(transparentDrawBlock, "debugOverlay_.drawScene"));
  EXPECT_FALSE(contains(pickBlock, "makeDepthBarrier"));
  EXPECT_FALSE(contains(pickBlock, "vkCmdPipelineBarrier"));
  EXPECT_FALSE(contains(pickBlock, "vkCmdCopyImage"));
  EXPECT_FALSE(contains(pickBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(lightingBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(pickBlock, "BimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(lightingBlock, "BimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(pickBlock, "surfaceDrawLists(p.draws)"));
  EXPECT_FALSE(contains(lightingBlock, "surfaceDrawLists(p.draws)"));
  EXPECT_FALSE(contains(pickBlock, "drawGpuCompacted"));
  EXPECT_FALSE(contains(lightingBlock, "drawGpuCompacted"));
  EXPECT_FALSE(contains(pickBlock, "drawCompactionReady"));
  EXPECT_FALSE(contains(lightingBlock, "drawCompactionReady"));
  EXPECT_FALSE(contains(pickBlock, "recordBimSurfacePassCommands"));
  EXPECT_FALSE(contains(transparentDrawBlock, "recordBimSurfacePassCommands"));
  EXPECT_TRUE(
      contains(transparentPickRecorder, "recordBimSurfacePassCommands"));
  EXPECT_TRUE(contains(transparentOitRecorder, "recordBimSurfacePassCommands"));
  EXPECT_TRUE(contains(pickBlock, "BimSurfacePassKind::TransparentPick"));
  EXPECT_TRUE(
      contains(lightingBlock, "BimSurfacePassKind::TransparentLighting"));
  EXPECT_FALSE(contains(pickBlock, "BimDrawCompactionSlot::"));
  EXPECT_FALSE(contains(lightingBlock, "BimDrawCompactionSlot::"));
  EXPECT_TRUE(contains(pickBlock, "inputs.bimSemanticColorMode"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "p.bim.semanticColorMode"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "inputs.bimPlan->semanticColorMode"));

  EXPECT_TRUE(contains(plannerHeader, "SceneTransparentDrawPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "SceneTransparentDrawPipeline"));
  EXPECT_TRUE(contains(planner, "primaryTransparentDrawCommands"));
  EXPECT_TRUE(contains(planner, "hasSplitTransparentDrawCommands"));
  EXPECT_TRUE(contains(planner, "SceneTransparentDrawPipeline::FrontCull"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "VkPipeline"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "BimSurface"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "LightingManager"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "DebugOverlayRenderer"));
  EXPECT_TRUE(contains(recorderHeader, "SceneTransparentDrawRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "SceneTransparentDrawPipelineHandles"));
  EXPECT_TRUE(contains(recorder, "pipelineForSceneTransparentRoute"));
  EXPECT_TRUE(contains(recorder, "recordSceneTransparentDrawCommands"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(contains(recorder, "debugOverlay->drawScene"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "BimSurface"));
  EXPECT_FALSE(contains(recorder, "BimManager"));
  EXPECT_FALSE(contains(recorder, "LightingManager"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "RenderPass"));
  EXPECT_FALSE(contains(recorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(recorder, "vkCmdPipelineBarrier"));
  EXPECT_TRUE(contains(transparentPickRecorderHeader,
                       "TransparentPickPassRecordInputs"));
  EXPECT_TRUE(
      contains(transparentPickRecorder, "recordTransparentPickPassCommands"));
  EXPECT_TRUE(
      contains(transparentPickRecorder, "recordSceneTransparentDrawCommands"));
  EXPECT_TRUE(
      contains(transparentPickRecorder, "recordBimSurfacePassCommands"));
  EXPECT_TRUE(contains(transparentPickRecorder, ".descriptorSets"));
  EXPECT_FALSE(
      contains(transparentPickRecorder, "bindTransparentPickGeometry"));
  EXPECT_FALSE(contains(transparentPickRecorder, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(transparentPickRecorder, "vkCmdBindVertexBuffers"));
  EXPECT_FALSE(contains(transparentPickRecorder, "vkCmdBindIndexBuffer"));
  EXPECT_FALSE(contains(transparentPickRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transparentPickRecorder, "LightingManager"));
  EXPECT_FALSE(contains(transparentPickRecorder, "GuiManager"));
  EXPECT_FALSE(contains(transparentPickRecorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(transparentPickRecorder, "endDynamicRendering"));
  EXPECT_FALSE(contains(transparentPickRecorder, "vkCmdPipelineBarrier"));
  EXPECT_TRUE(contains(transparentPickRasterRecorderHeader,
                       "TransparentPickRasterPassRecordInputs"));
  EXPECT_TRUE(contains(transparentPickRasterRecorderHeader,
                       "TransparentPickFramePassRecordInputs"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder,
                       "recordTransparentPickRasterPassCommands"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder,
                       "recordTransparentPickFramePassCommands"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder, "beginDynamicRendering"));
  EXPECT_TRUE(
      contains(transparentPickRasterRecorder, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder,
                       "recordTransparentPickPassCommands"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder, "endDynamicRendering"));
  EXPECT_FALSE(contains(transparentPickRasterRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transparentPickRasterRecorder, "GuiManager"));
  EXPECT_FALSE(contains(transparentPickRasterRecorder, "LightingManager"));
  EXPECT_TRUE(
      contains(transparentPickRasterRecorder, "TransparentPickDepthCopy"));
  EXPECT_TRUE(
      contains(transparentPickRasterRecorder, "buildSceneTransparentDrawPlan"));
  EXPECT_TRUE(
      contains(transparentPickRasterRecorder, "buildBimSurfaceFramePassPlan"));
  EXPECT_FALSE(
      contains(transparentPickRasterRecorder, "buildBimSurfacePassPlan"));
  EXPECT_TRUE(contains(transparentPickRasterRecorder, "BimSurfacePassKind"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorderHeader,
                       "TransparentPickDepthCopyPlan"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorder,
                       "buildTransparentPickDepthCopyPlan"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorder,
                       "recordTransparentPickDepthCopyCommands"));
  EXPECT_TRUE(
      contains(transparentPickDepthCopyRecorder, "vkCmdPipelineBarrier"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorder, "vkCmdCopyImage"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorder,
                       "VK_IMAGE_ASPECT_DEPTH_BIT | "
                       "VK_IMAGE_ASPECT_STENCIL_BIT"));
  EXPECT_TRUE(contains(transparentPickDepthCopyRecorder,
                       "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"));
  EXPECT_FALSE(contains(transparentPickDepthCopyRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transparentPickDepthCopyRecorder, "BimManager"));
  EXPECT_FALSE(
      contains(transparentPickDepthCopyRecorder, "SceneTransparentDrawPlan"));
  EXPECT_FALSE(
      contains(transparentPickDepthCopyRecorder, "beginDynamicRendering"));
  EXPECT_FALSE(
      contains(transparentPickDepthCopyRecorder, "endDynamicRendering"));
  EXPECT_TRUE(contains(transparentOitRecorderHeader,
                       "DeferredTransparentOitRecordInputs"));
  EXPECT_TRUE(contains(transparentOitRecorderHeader,
                       "DeferredTransparentOitFrameResourceInputs"));
  EXPECT_TRUE(contains(transparentOitFramePassRecorderHeader,
                       "DeferredTransparentOitFramePassServices"));
  EXPECT_TRUE(contains(transparentOitFramePassRecorderHeader,
                       "DeferredTransparentOitFramePassRecorder"));
  EXPECT_TRUE(
      contains(transparentOitFramePassRecorder, "shouldRecordTransparentOit"));
  EXPECT_TRUE(contains(transparentOitFramePassRecorder,
                       "recordDeferredTransparentOitClearCommands"));
  EXPECT_TRUE(
      contains(transparentOitFramePassRecorder,
               "recordDeferredTransparentOitResolvePreparationCommands"));
  EXPECT_TRUE(contains(transparentOitFramePassRecorder, "FrameRecordParams"));
  EXPECT_TRUE(contains(transparentOitFramePassRecorderHeader, "GuiManager"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "recordDeferredTransparentOitCommands"));
  EXPECT_TRUE(contains(transparentOitRecorder,
                       "recordDeferredTransparentOitClearCommands"));
  EXPECT_TRUE(
      contains(transparentOitRecorder,
               "recordDeferredTransparentOitResolvePreparationCommands"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "recordSceneTransparentDrawCommands"));
  EXPECT_TRUE(contains(transparentOitRecorder, "bindTransparentGeometry"));
  EXPECT_TRUE(contains(transparentOitRecorder, "semanticColorMode = 0u"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "inputs.bimPlan->semanticColorMode"));
  EXPECT_FALSE(contains(transparentOitRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(transparentOitRecorder, "GuiManager"));
  EXPECT_FALSE(contains(transparentOitRecorder, "LightingManager"));
  EXPECT_FALSE(contains(transparentOitRecorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(transparentOitRecorder, "endDynamicRendering"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/scene/SceneTransparentDrawPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/scene/SceneTransparentDrawRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/picking/TransparentPickDepthCopyRecorder.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/picking/TransparentPickPassRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/picking/TransparentPickRasterPassRecorder.cpp"));
  EXPECT_TRUE(contains(srcCmake,
                       "renderer/deferred/DeferredTransparentOitRecorder.cpp"));
  EXPECT_TRUE(contains(
      srcCmake,
      "renderer/deferred/DeferredTransparentOitFramePassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "scene_transparent_draw_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "scene_transparent_draw_recorder_tests"));
  EXPECT_TRUE(
      contains(testsCmake, "transparent_pick_depth_copy_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake, "transparent_pick_pass_recorder_tests"));
  EXPECT_TRUE(
      contains(testsCmake, "transparent_pick_raster_pass_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake, "deferred_transparent_oit_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake,
                       "deferred_transparent_oit_frame_pass_recorder_tests"));
}

TEST(RenderingConventionTests, FrameRecorderUsesSharedRenderPassScopeRecorder) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string scopeRecorderHeader = readRepoTextFile(
      "include/Container/renderer/core/RenderPassScopeRecorder.h");
  const std::string scopeRecorder =
      readRepoTextFile("src/renderer/core/RenderPassScopeRecorder.cpp");
  const std::string shadowRasterRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRasterRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t shadowStart = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::cascadePassRecordInputs");
  ASSERT_NE(shadowStart, std::string::npos);
  const size_t shadowEnd = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::secondaryPassRecordInputs", shadowStart);
  ASSERT_NE(shadowEnd, std::string::npos);
  const std::string shadowBlock =
      shadowFramePassRecorder.substr(shadowStart, shadowEnd - shadowStart);

  const size_t lightingStart = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingStart, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingStart);

  EXPECT_TRUE(contains(lightingPassRecorder, "RenderPassScopeRecorder.h"));
  EXPECT_FALSE(contains(shadowBlock, "recordRenderPassBeginCommands"));
  EXPECT_FALSE(
      contains(shadowBlock, "recordRenderPassExecuteSecondaryCommands"));
  EXPECT_FALSE(contains(shadowBlock, "recordRenderPassEndCommands"));
  EXPECT_TRUE(contains(lightingBlock, "recordRenderPassBeginCommands"));
  EXPECT_TRUE(contains(lightingBlock, "recordRenderPassEndCommands"));
  EXPECT_TRUE(contains(lightingBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(frameRecorder, "RenderingBeginInfo"));
  EXPECT_FALSE(contains(frameRecorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(frameRecorder, "vkCmdExecuteCommands"));
  EXPECT_FALSE(contains(frameRecorder, "endDynamicRendering"));
  EXPECT_FALSE(contains(shadowBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordLightingPass"));

  EXPECT_TRUE(contains(scopeRecorderHeader, "RenderPassScopeRecordInputs"));
  EXPECT_TRUE(contains(scopeRecorderHeader, "recordRenderPassBeginCommands"));
  EXPECT_TRUE(contains(scopeRecorderHeader,
                       "recordRenderPassExecuteSecondaryCommands"));
  EXPECT_TRUE(contains(scopeRecorderHeader, "recordRenderPassEndCommands"));
  EXPECT_TRUE(contains(scopeRecorder, "RenderingBeginInfo"));
  EXPECT_TRUE(contains(scopeRecorder, "beginDynamicRendering"));
  EXPECT_TRUE(contains(scopeRecorder, "vkCmdExecuteCommands"));
  EXPECT_TRUE(contains(scopeRecorder, "endDynamicRendering"));
  EXPECT_TRUE(contains(shadowRasterRecorder, "recordRenderPassBeginCommands"));
  EXPECT_TRUE(contains(shadowRasterRecorder,
                       "recordRenderPassExecuteSecondaryCommands"));
  EXPECT_TRUE(contains(shadowRasterRecorder, "recordRenderPassEndCommands"));
  EXPECT_FALSE(contains(scopeRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(scopeRecorder, "GuiManager"));
  EXPECT_FALSE(contains(scopeRecorder, "LightingManager"));
  EXPECT_FALSE(contains(scopeRecorder, "BimManager"));
  EXPECT_FALSE(contains(scopeRecorder, "ShadowPassDrawPlanner"));
  EXPECT_TRUE(contains(srcCmake, "renderer/core/RenderPassScopeRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "render_pass_scope_recorder_tests"));
}

TEST(RenderingConventionTests, ShadowPassScopeUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassScopePlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowPassScopePlanner.cpp");
  const std::string rasterPlannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassRasterPlanner.h");
  const std::string rasterPlanner =
      readRepoTextFile("src/renderer/shadow/ShadowPassRasterPlanner.cpp");
  const std::string rasterRecorderHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassRasterRecorder.h");
  const std::string rasterRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRasterRecorder.cpp");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t shadowStart = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::cascadePassRecordInputs");
  ASSERT_NE(shadowStart, std::string::npos);
  const size_t shadowBodyStart = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::secondaryPassRecordInputs", shadowStart);
  ASSERT_NE(shadowBodyStart, std::string::npos);
  const std::string shadowBlock = shadowFramePassRecorder.substr(
      shadowStart, shadowBodyStart - shadowStart);

  EXPECT_FALSE(contains(frameRecorder, "ShadowPassRasterPlanner.h"));
  EXPECT_FALSE(contains(frameRecorder, "ShadowPassRasterRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "ShadowPassScopePlanner.h"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "recordShadowCascadePassCommands"));
  EXPECT_TRUE(contains(shadowPassRecorder, "buildShadowPassRasterPlan"));
  EXPECT_TRUE(contains(shadowPassRecorder, "recordShadowPassRasterCommands"));
  EXPECT_TRUE(contains(shadowBlock, ".shadowAtlasVisible"));
  EXPECT_TRUE(contains(shadowBlock, ".shadowPassRecordable"));
  EXPECT_TRUE(contains(shadowBlock, ".useSecondaryCommandBuffer"));
  EXPECT_TRUE(contains(shadowBlock, ".secondaryCommandBuffer"));
  EXPECT_FALSE(contains(shadowBlock, "buildShadowPassScopePlan"));
  EXPECT_FALSE(contains(shadowBlock, "shadowPassPlan.renderArea"));
  EXPECT_FALSE(contains(shadowBlock, "shadowPassPlan.clearValues"));
  EXPECT_FALSE(contains(shadowBlock, "shadowPassPlan.contents"));
  EXPECT_FALSE(contains(shadowBlock, "shadowPassPlan.executeSecondary"));
  EXPECT_FALSE(contains(shadowBlock, "recordRenderPassBeginCommands"));
  EXPECT_FALSE(
      contains(shadowBlock, "recordRenderPassExecuteSecondaryCommands"));
  EXPECT_FALSE(contains(shadowBlock, "recordRenderPassEndCommands"));
  EXPECT_FALSE(contains(shadowBlock, "VkClearValue"));
  EXPECT_FALSE(contains(shadowBlock, "VkRect2D"));
  EXPECT_FALSE(contains(shadowBlock, "kShadowMapResolution"));
  EXPECT_FALSE(
      contains(shadowBlock, "VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS"));

  EXPECT_TRUE(contains(plannerHeader, "ShadowPassScopePlan"));
  EXPECT_TRUE(contains(plannerHeader, "buildShadowPassScopePlan"));
  EXPECT_TRUE(contains(planner, "kShadowMapResolution"));
  EXPECT_TRUE(contains(planner, "renderArea.offset = {0, 0}"));
  EXPECT_TRUE(contains(planner, "clearValues[0].depthStencil"));
  EXPECT_TRUE(contains(planner, "VK_SUBPASS_CONTENTS_INLINE"));
  EXPECT_TRUE(
      contains(planner, "VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS"));
  EXPECT_TRUE(contains(planner, "executeSecondary"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "ShadowPassDrawPlanner"));
  EXPECT_FALSE(contains(planner, "ShadowPassRecorder"));
  EXPECT_TRUE(contains(rasterPlannerHeader, "ShadowPassRasterPlanInputs"));
  EXPECT_TRUE(contains(rasterPlannerHeader, "ShadowPassRasterPlan"));
  EXPECT_TRUE(contains(rasterPlanner, "buildShadowPassRasterPlan"));
  EXPECT_TRUE(contains(rasterPlanner, "buildShadowPassScopePlan"));
  EXPECT_TRUE(contains(rasterPlanner, "secondaryCommandBuffer"));
  EXPECT_FALSE(contains(rasterPlanner, "vkCmd"));
  EXPECT_FALSE(contains(rasterPlanner, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterPlanner, "GuiManager"));
  EXPECT_FALSE(contains(rasterPlanner, "BimManager"));
  EXPECT_FALSE(contains(rasterPlanner, "ShadowCullManager"));
  EXPECT_TRUE(contains(rasterRecorderHeader, "ShadowPassRasterRecordInputs"));
  EXPECT_TRUE(contains(rasterRecorderHeader, "ShadowPassRasterRecordBody"));
  EXPECT_TRUE(contains(rasterRecorder, "recordShadowPassRasterCommands"));
  EXPECT_TRUE(contains(rasterRecorder, "recordRenderPassBeginCommands"));
  EXPECT_TRUE(
      contains(rasterRecorder, "recordRenderPassExecuteSecondaryCommands"));
  EXPECT_TRUE(contains(rasterRecorder, "recordRenderPassEndCommands"));
  EXPECT_FALSE(contains(rasterRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterRecorder, "GuiManager"));
  EXPECT_FALSE(contains(rasterRecorder, "BimManager"));
  EXPECT_FALSE(contains(rasterRecorder, "ShadowCullManager"));
  EXPECT_TRUE(contains(srcCmake, "renderer/shadow/ShadowPassScopePlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowPassRasterPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowPassRasterRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_pass_scope_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "shadow_pass_raster_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "shadow_pass_raster_recorder_tests"));
}

TEST(RenderingConventionTests,
     FrameRecorderUsesSharedCommandBufferScopeRecorder) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string scopeRecorderHeader = readRepoTextFile(
      "include/Container/renderer/core/CommandBufferScopeRecorder.h");
  const std::string scopeRecorder =
      readRepoTextFile("src/renderer/core/CommandBufferScopeRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  EXPECT_TRUE(contains(frameRecorder, "CommandBufferScopeRecorder.h"));
  EXPECT_TRUE(contains(frameRecorder, "recordCommandBufferBeginCommands"));
  EXPECT_TRUE(contains(frameRecorder, "recordCommandBufferEndCommands"));
  EXPECT_FALSE(contains(frameRecorder, "recordCommandBufferResetCommands"));
  EXPECT_FALSE(
      contains(frameRecorder, "recordSecondaryCommandBufferBeginCommands"));
  EXPECT_FALSE(contains(frameRecorder, "VkCommandBufferBeginInfo"));
  EXPECT_FALSE(contains(frameRecorder, "VkCommandBufferInheritanceInfo"));
  EXPECT_FALSE(contains(frameRecorder, "vkResetCommandBuffer"));
  EXPECT_FALSE(contains(frameRecorder, "vkBeginCommandBuffer"));
  EXPECT_FALSE(contains(frameRecorder, "vkEndCommandBuffer"));
  EXPECT_TRUE(
      contains(frameRecorder, "failed to begin recording command buffer!"));
  EXPECT_FALSE(contains(frameRecorder,
                        "failed to record shadow secondary command buffer!"));

  EXPECT_TRUE(contains(scopeRecorderHeader, "CommandBufferBeginRecordInputs"));
  EXPECT_TRUE(
      contains(scopeRecorderHeader, "SecondaryCommandBufferBeginRecordInputs"));
  EXPECT_TRUE(
      contains(scopeRecorderHeader, "recordCommandBufferResetCommands"));
  EXPECT_TRUE(
      contains(scopeRecorderHeader, "recordCommandBufferBeginCommands"));
  EXPECT_TRUE(contains(scopeRecorderHeader,
                       "recordSecondaryCommandBufferBeginCommands"));
  EXPECT_TRUE(contains(scopeRecorderHeader, "recordCommandBufferEndCommands"));
  EXPECT_TRUE(contains(scopeRecorder, "VkCommandBufferBeginInfo"));
  EXPECT_TRUE(contains(scopeRecorder, "VkCommandBufferInheritanceInfo"));
  EXPECT_TRUE(contains(scopeRecorder, "vkResetCommandBuffer"));
  EXPECT_TRUE(contains(scopeRecorder, "vkBeginCommandBuffer"));
  EXPECT_TRUE(contains(scopeRecorder, "vkEndCommandBuffer"));
  EXPECT_FALSE(contains(scopeRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(scopeRecorder, "GuiManager"));
  EXPECT_FALSE(contains(scopeRecorder, "BimManager"));
  EXPECT_FALSE(contains(scopeRecorder, "ShadowPassDrawPlanner"));
  EXPECT_FALSE(contains(scopeRecorder, "RenderGraph"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/core/CommandBufferScopeRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "command_buffer_scope_recorder_tests"));
}

TEST(RenderingConventionTests,
     ShadowCascadeSecondaryCommandBufferRecordingUsesRecorder) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string recorderHeader =
      readRepoTextFile("include/Container/renderer/shadow/"
                       "ShadowCascadeSecondaryCommandBufferRecorder.h");
  const std::string recorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.cpp");
  const std::string shadowPassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassRecorder.h");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t secondaryStart = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::secondaryPassRecordInputs");
  ASSERT_NE(secondaryStart, std::string::npos);
  const size_t secondaryEnd = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::prepareDrawCommands",
      secondaryStart);
  ASSERT_NE(secondaryEnd, std::string::npos);
  const std::string secondaryBlock = shadowFramePassRecorder.substr(
      secondaryStart, secondaryEnd - secondaryStart);

  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "recordShadowCascadeSecondaryPassCommands"));
  EXPECT_TRUE(contains(shadowPassRecorderHeader,
                       "ShadowCascadeSecondaryPassRecordInputs"));
  EXPECT_TRUE(
      contains(shadowPassRecorder, "recordShadowCascadeSecondaryPassCommands"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "ShadowCascadeSecondaryCommandBufferPlanInputs"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "buildShadowCascadeSecondaryCommandBufferRecordPlan"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "recordShadowCascadeSecondaryCommandBufferPlan"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "recordShadowCascadeSecondaryCommandBufferCommands"));
  EXPECT_FALSE(contains(secondaryBlock, "std::async"));
  EXPECT_FALSE(contains(secondaryBlock, "std::future"));
  EXPECT_FALSE(contains(secondaryBlock, "recordCommandBufferResetCommands"));
  EXPECT_FALSE(
      contains(secondaryBlock, "recordSecondaryCommandBufferBeginCommands"));
  EXPECT_FALSE(contains(secondaryBlock, "recordCommandBufferEndCommands"));
  EXPECT_FALSE(contains(frameRecorderHeader,
                        "recordShadowCascadeSecondaryCommandBuffer("));
  EXPECT_FALSE(
      contains(frameRecorder, "recordShadowCascadeSecondaryCommandBuffers"));

  EXPECT_TRUE(contains(recorderHeader,
                       "ShadowCascadeSecondaryCommandBufferPlanInputs"));
  EXPECT_TRUE(contains(recorderHeader,
                       "ShadowCascadeSecondaryCommandBufferRecordPlan"));
  EXPECT_TRUE(
      contains(recorderHeader, "ShadowCascadeSecondaryCommandBufferCommands"));
  EXPECT_TRUE(contains(recorderHeader,
                       "ShadowCascadeSecondaryCommandBufferRecordCallback"));
  EXPECT_TRUE(contains(recorder, "std::async"));
  EXPECT_TRUE(contains(recorder, "std::launch::async"));
  EXPECT_TRUE(contains(recorder, "recordCommandBufferResetCommands"));
  EXPECT_TRUE(contains(recorder, "recordSecondaryCommandBufferBeginCommands"));
  EXPECT_TRUE(contains(recorder, "recordCommandBufferEndCommands"));
  EXPECT_TRUE(
      contains(recorder, "failed to reset shadow secondary command buffer!"));
  EXPECT_TRUE(
      contains(recorder, "failed to begin shadow secondary command buffer!"));
  EXPECT_TRUE(
      contains(recorder, "failed to record shadow secondary command buffer!"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "GuiManager"));
  EXPECT_FALSE(contains(recorder, "BimManager"));
  EXPECT_FALSE(contains(recorder, "ShadowCullManager"));
  EXPECT_FALSE(contains(recorder, "RenderGraph"));
  EXPECT_TRUE(contains(
      srcCmake,
      "renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.cpp"));
  EXPECT_TRUE(contains(
      testsCmake, "shadow_cascade_secondary_command_buffer_recorder_tests"));
}

TEST(RenderingConventionTests, ScreenshotCopyRecordingUsesCaptureHelper) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string screenshotRecorder =
      readRepoTextFile("src/renderer/core/ScreenshotCaptureRecorder.cpp");

  const size_t screenshotPass = deferredFrameGraphContext.find(
      "void DeferredRasterFrameGraphContext::afterGraphExecution");
  ASSERT_NE(screenshotPass, std::string::npos);
  const size_t namespaceEnd = deferredFrameGraphContext.find(
      "} // namespace container::renderer", screenshotPass);
  ASSERT_NE(namespaceEnd, std::string::npos);
  const std::string screenshotBlock = deferredFrameGraphContext.substr(
      screenshotPass, namespaceEnd - screenshotPass);

  EXPECT_FALSE(contains(frameRecorder, "recordScreenshotCaptureCopy"));
  EXPECT_TRUE(contains(screenshotBlock, "recordScreenshotCaptureCopy"));
  EXPECT_FALSE(contains(screenshotBlock, "vkCmdCopyImageToBuffer"));
  EXPECT_FALSE(contains(screenshotBlock, "VK_PIPELINE_STAGE_HOST_BIT"));
  EXPECT_TRUE(contains(screenshotRecorder, "hasScreenshotCopyWork"));
  EXPECT_TRUE(contains(screenshotRecorder, "vkCmdCopyImageToBuffer"));
  EXPECT_TRUE(contains(screenshotRecorder, "VK_PIPELINE_STAGE_HOST_BIT"));
  EXPECT_TRUE(contains(screenshotRecorder, "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"));
}

TEST(RenderingConventionTests, AutoExposurePercentileMeteringRejectsOutliers) {
  std::array<uint32_t, 64> bins{};
  bins[20] = 900;
  bins[50] = 100;
  bins[63] = 1;

  const float fullRange = percentileMeteredHistogramLuminance(bins, 0.0f, 1.0f);
  const float percentileWindow =
      percentileMeteredHistogramLuminance(bins, 0.0f, 0.899f);

  EXPECT_LT(percentileWindow, fullRange);
  EXPECT_NEAR(percentileWindow, std::exp2(histogramBinCenterLog2Luminance(20)),
              1e-4f);

  const std::string exposureManager =
      readRepoTextFile("src/renderer/effects/ExposureManager.cpp");
  EXPECT_TRUE(contains(exposureManager, "lowerSample"));
  EXPECT_TRUE(contains(exposureManager, "upperSample"));
  EXPECT_TRUE(contains(exposureManager, "includedSamples"));
}

TEST(RenderingConventionTests, ManualExposureScalesSceneLinearLuminance) {
  const glm::vec3 hdrColor{6.0f, 2.0f, 0.5f};
  const glm::vec3 halfExposure = applyManualExposure(hdrColor, 0.5f);
  const glm::vec3 doubleExposure = applyManualExposure(hdrColor, 2.0f);

  EXPECT_NEAR(sceneLinearLuminance(halfExposure),
              sceneLinearLuminance(hdrColor) * 0.5f, 1e-6f);
  EXPECT_NEAR(sceneLinearLuminance(doubleExposure),
              sceneLinearLuminance(hdrColor) * 2.0f, 1e-6f);
  EXPECT_GT(sceneLinearLuminance(doubleExposure),
            sceneLinearLuminance(halfExposure));

  EXPECT_FLOAT_EQ(resolvePostProcessExposure(
                      container::gpu::kExposureModeManual, 0.25f, 1.0f, 2.0f),
                  0.25f);
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(container::gpu::kExposureModeAuto,
                                             0.25f, 1.0f, 2.0f),
                  1.0f);
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(container::gpu::kExposureModeAuto,
                                             3.0f, 1.0f, 2.0f),
                  2.0f);
}

TEST(RenderingConventionTests, BloomThresholdFilterIsSceneLinear) {
  const glm::vec3 hdrColor{5.0f, 1.25f, 0.5f};
  constexpr float kThreshold = 2.0f;
  constexpr float kKnee = 0.5f;
  constexpr float kScale = 3.0f;

  const glm::vec3 filtered = bloomThresholdFilter(hdrColor, kThreshold, kKnee);
  const glm::vec3 scaledFiltered = bloomThresholdFilter(
      hdrColor * kScale, kThreshold * kScale, kKnee * kScale);

  EXPECT_NEAR(scaledFiltered.r, filtered.r * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.g, filtered.g * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.b, filtered.b * kScale, 1e-4f);

  const std::string bloomDownsample =
      readRepoTextFile("shaders/bloom_downsample.slang");
  EXPECT_TRUE(
      contains(bloomDownsample,
               "color = ThresholdFilter(color, pc.threshold, pc.knee)"));
  EXPECT_FALSE(contains(bloomDownsample, "pc.exposure"));
}

TEST(RenderingConventionTests, BloomComputeSanitizesNonFiniteHdrSamples) {
  const std::string bloomDownsample =
      readRepoTextFile("shaders/bloom_downsample.slang");
  const std::string bloomUpsample =
      readRepoTextFile("shaders/bloom_upsample.slang");

  EXPECT_TRUE(contains(bloomDownsample, "BloomFinite3Or"));
  EXPECT_TRUE(contains(bloomDownsample, "SampleBloomFinite"));
  EXPECT_TRUE(contains(bloomDownsample, "ThresholdFilter("));
  EXPECT_TRUE(contains(bloomDownsample, "BloomFinite3Or(color, 0.0.xxx)"));
  EXPECT_TRUE(contains(bloomDownsample, "dstTexture[dtid.xy] = float4(color"));

  EXPECT_TRUE(contains(bloomUpsample, "BloomFinite3Or"));
  EXPECT_TRUE(contains(bloomUpsample,
                       "radius = max(BloomFiniteOr(radius, 1.0), 0.0)"));
  EXPECT_TRUE(contains(bloomUpsample, "SampleBloomFinite"));
  EXPECT_TRUE(contains(bloomUpsample, "BloomFinite3Or(existing + upsampled"));
  EXPECT_TRUE(contains(bloomUpsample, "dstTexture[dtid.xy] = float4(result"));
}

TEST(RenderingConventionTests,
     ShadowCascadeMetadataLayoutMatchesShaderContract) {
  using container::gpu::ShadowCascadeCullData;
  using container::gpu::ShadowCascadeData;
  using container::gpu::ShadowCullData;
  using container::gpu::ShadowData;

  EXPECT_EQ(container::gpu::kShadowCascadeCount, 4u);
  EXPECT_EQ(sizeof(ShadowCascadeData), 80u);
  EXPECT_EQ(alignof(ShadowCascadeData), 16u);
  EXPECT_EQ(offsetof(ShadowCascadeData, viewProj), 0u);
  EXPECT_EQ(offsetof(ShadowCascadeData, splitDepth), 64u);
  EXPECT_EQ(offsetof(ShadowCascadeData, texelSize), 68u);
  EXPECT_EQ(offsetof(ShadowCascadeData, worldRadius), 72u);
  EXPECT_EQ(offsetof(ShadowCascadeData, depthRange), 76u);
  EXPECT_EQ(sizeof(ShadowData),
            sizeof(ShadowCascadeData) * container::gpu::kShadowCascadeCount +
                64u);
  EXPECT_EQ(offsetof(ShadowData, cascades), 0u);
  EXPECT_EQ(offsetof(ShadowData, biasSettings), 320u);
  EXPECT_EQ(offsetof(ShadowData, filterSettings), 336u);
  EXPECT_EQ(offsetof(ShadowData, softShadowSettings), 352u);
  EXPECT_EQ(offsetof(ShadowData, contactShadowSettings), 368u);

  EXPECT_EQ(sizeof(ShadowCascadeCullData), 192u);
  EXPECT_EQ(alignof(ShadowCascadeCullData), 16u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, viewProj), 0u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, lightView), 64u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, receiverMinBounds), 128u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, receiverMaxBounds), 144u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, casterMinBounds), 160u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, casterMaxBounds), 176u);
  EXPECT_EQ(sizeof(ShadowCullData), sizeof(ShadowCascadeCullData) *
                                        container::gpu::kShadowCascadeCount);
}

TEST(RenderingConventionTests, ShadowSettingsMapToShadowBufferVectors) {
  using container::gpu::ShadowData;
  using container::gpu::ShadowSettings;

  EXPECT_EQ(sizeof(ShadowSettings), 72u);
  EXPECT_EQ(offsetof(ShadowSettings, areaShadowQuality), 68u);
  EXPECT_EQ(offsetof(ShadowSettings, normalBiasMinTexels), 0u);
  EXPECT_EQ(offsetof(ShadowSettings, normalBiasMaxTexels), 4u);
  EXPECT_EQ(offsetof(ShadowSettings, slopeBiasScale), 8u);
  EXPECT_EQ(offsetof(ShadowSettings, receiverPlaneBiasScale), 12u);
  EXPECT_EQ(offsetof(ShadowSettings, filterRadiusTexels), 16u);
  EXPECT_EQ(offsetof(ShadowSettings, cascadeBlendFraction), 20u);
  EXPECT_EQ(offsetof(ShadowSettings, constantDepthBias), 24u);
  EXPECT_EQ(offsetof(ShadowSettings, maxDepthBias), 28u);
  EXPECT_EQ(offsetof(ShadowSettings, rasterConstantBias), 32u);
  EXPECT_EQ(offsetof(ShadowSettings, rasterSlopeBias), 36u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalPcssLightRadiusDegrees), 40u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalPcssBlockerSearchRadiusTexels),
            44u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalPcssMaxFilterRadiusTexels),
            48u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalContactMaxDistance), 52u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalContactThickness), 56u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalContactFadeDistance), 60u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalPcssEnabled), 64u);
  EXPECT_EQ(offsetof(ShadowSettings, directionalContactVisibility), 65u);
  EXPECT_EQ(offsetof(ShadowSettings, localContactVisibility), 66u);

  const ShadowSettings settings{};
  const ShadowData shadowData{};
  EXPECT_FLOAT_EQ(shadowData.biasSettings.x, settings.normalBiasMinTexels);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.y, settings.normalBiasMaxTexels);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.z, settings.slopeBiasScale);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.w, settings.receiverPlaneBiasScale);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.x, settings.filterRadiusTexels);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.y, settings.cascadeBlendFraction);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.z, settings.constantDepthBias);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.w, settings.maxDepthBias);
  EXPECT_FLOAT_EQ(shadowData.softShadowSettings.x, 1.0f);
  EXPECT_NEAR(shadowData.softShadowSettings.y,
              container::gpu::kDefaultDirectionalPcssTanLightRadius, 1e-7f);
  EXPECT_FLOAT_EQ(shadowData.softShadowSettings.z,
                  settings.directionalPcssBlockerSearchRadiusTexels);
  EXPECT_FLOAT_EQ(shadowData.softShadowSettings.w,
                  settings.directionalPcssMaxFilterRadiusTexels);
  EXPECT_FLOAT_EQ(shadowData.contactShadowSettings.x, 0.0f);
  EXPECT_FLOAT_EQ(shadowData.contactShadowSettings.y,
                  settings.directionalContactMaxDistance);
  EXPECT_FLOAT_EQ(shadowData.contactShadowSettings.z,
                  settings.directionalContactThickness);
  EXPECT_FLOAT_EQ(shadowData.contactShadowSettings.w,
                  settings.directionalContactFadeDistance);
  EXPECT_FLOAT_EQ(settings.rasterConstantBias, -4.0f);
  EXPECT_FLOAT_EQ(settings.rasterSlopeBias, -1.5f);
  EXPECT_FLOAT_EQ(settings.directionalContactMaxDistance, 0.35f);
  EXPECT_FLOAT_EQ(settings.directionalContactThickness, 0.04f);
  EXPECT_FLOAT_EQ(settings.directionalContactFadeDistance, 0.30f);
  EXPECT_TRUE(settings.directionalPcssEnabled);
  EXPECT_FALSE(settings.directionalContactVisibility);
  EXPECT_TRUE(settings.localContactVisibility);
}

TEST(RenderingConventionTests, ShadowSettingsFlowFromUiThroughShadowUpload) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");

  EXPECT_TRUE(
      contains(guiHeader,
               "const container::gpu::ShadowSettings& shadowSettings() const"));
  EXPECT_TRUE(
      contains(guiHeader, "container::gpu::ShadowSettings shadowSettings_{}"));
  EXPECT_TRUE(contains(rendererFrontend, "subs_.guiManager->shadowSettings()"));
  EXPECT_TRUE(contains(shadowManager,
                       "const container::gpu::ShadowSettings& shadowSettings"));

  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.normalBiasMinTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.normalBiasMaxTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.slopeBiasScale"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.receiverPlaneBiasScale"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.filterRadiusTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.cascadeBlendFraction"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.constantDepthBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.maxDepthBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterConstantBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterSlopeBias"));
  EXPECT_TRUE(
      contains(guiManager, "&shadowSettings_.directionalPcssEnabled"));
  EXPECT_TRUE(contains(
      guiManager, "&shadowSettings_.directionalPcssLightRadiusDegrees"));
  EXPECT_TRUE(contains(
      guiManager,
      "&shadowSettings_.directionalPcssBlockerSearchRadiusTexels"));
  EXPECT_TRUE(contains(
      guiManager, "&shadowSettings_.directionalPcssMaxFilterRadiusTexels"));
  EXPECT_TRUE(
      contains(guiManager, "&shadowSettings_.directionalContactVisibility"));
  EXPECT_TRUE(
      contains(guiManager, "&shadowSettings_.directionalContactMaxDistance"));
  EXPECT_TRUE(
      contains(guiManager, "&shadowSettings_.directionalContactThickness"));
  EXPECT_TRUE(
      contains(guiManager, "&shadowSettings_.directionalContactFadeDistance"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.localContactVisibility"));

  EXPECT_TRUE(contains(shadowManager, "shadowData_.biasSettings = glm::vec4("));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.normalBiasMinTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.normalBiasMaxTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.slopeBiasScale"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.receiverPlaneBiasScale"));
  EXPECT_TRUE(
      contains(shadowManager, "shadowData_.filterSettings = glm::vec4("));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.filterRadiusTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.cascadeBlendFraction"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.constantDepthBias"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.maxDepthBias"));
  EXPECT_TRUE(contains(shadowManager,
                       "shadowData_.softShadowSettings = glm::vec4("));
  EXPECT_TRUE(
      contains(shadowManager, "shadowSettings.directionalPcssEnabled"));
  EXPECT_TRUE(contains(
      shadowManager, "shadowSettings.directionalPcssLightRadiusDegrees"));
  EXPECT_TRUE(contains(
      shadowManager, "shadowSettings.directionalPcssBlockerSearchRadiusTexels"));
  EXPECT_TRUE(contains(
      shadowManager, "shadowSettings.directionalPcssMaxFilterRadiusTexels"));
  EXPECT_TRUE(contains(shadowManager,
                       "shadowData_.contactShadowSettings = glm::vec4("));
  EXPECT_TRUE(
      contains(shadowManager, "shadowSettings.directionalContactVisibility"));
  EXPECT_TRUE(
      contains(shadowManager, "shadowSettings.directionalContactMaxDistance"));
  EXPECT_TRUE(
      contains(shadowManager, "shadowSettings.directionalContactThickness"));
  EXPECT_TRUE(
      contains(shadowManager, "shadowSettings.directionalContactFadeDistance"));
}

TEST(RenderingConventionTests,
     DirectionalShadowsUsePcssBlockerSearchAndAdaptivePenumbra) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(sceneData, "directionalPcssEnabled"));
  EXPECT_TRUE(contains(sceneData, "directionalPcssLightRadiusDegrees"));
  EXPECT_TRUE(
      contains(sceneData, "directionalPcssBlockerSearchRadiusTexels"));
  EXPECT_TRUE(contains(sceneData, "directionalPcssMaxFilterRadiusTexels"));
  EXPECT_TRUE(contains(sceneData, "kDefaultDirectionalPcssTanLightRadius"));

  EXPECT_TRUE(contains(lightingStructs, "float4 softShadowSettings"));
  EXPECT_TRUE(contains(lightingStructs, "float4 contactShadowSettings"));
  EXPECT_TRUE(contains(shadowCommon, "SHADOW_PCSS_BLOCKER_SAMPLE_COUNT"));
  EXPECT_TRUE(contains(shadowCommon, "DirectionalShadowPoissonOffset"));
  EXPECT_TRUE(contains(shadowCommon, "DirectionalPcssAverageBlockerDepth"));
  EXPECT_TRUE(contains(shadowCommon, "DirectionalPcssSearchRadiusTexels"));
  EXPECT_TRUE(contains(shadowCommon, "DirectionalPcssFilterRadiusTexels"));
  EXPECT_TRUE(contains(shadowCommon, "SampleWeightedPcfWithRadius"));
  EXPECT_TRUE(contains(shadowCommon, "shadowAtlas.Load"));
  EXPECT_TRUE(contains(shadowCommon, "1.0 - receiverDepth"));
  EXPECT_TRUE(contains(shadowCommon,
                       "receiverDistanceFromNearWorld * lightTanRadius"));
  EXPECT_TRUE(contains(shadowCommon, "avgBlockerDepth - receiverDepth"));
  EXPECT_TRUE(contains(shadowCommon,
                       "shadowData.cascades[cascadeIndex].depthRange"));
  EXPECT_TRUE(contains(shadowCommon,
                       "shadowData.cascades[cascadeIndex].texelSize"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.softShadowSettings.x"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.softShadowSettings.y"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.softShadowSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.softShadowSettings.w"));

  EXPECT_TRUE(contains(shadowManager, "std::tan"));
  EXPECT_TRUE(contains(shadowManager, "directionalPcssLightRadiusDegrees"));
  EXPECT_TRUE(contains(guiManager, "\"Directional PCSS\""));
  EXPECT_TRUE(contains(guiManager, "\"Sun Radius Degrees\""));
  EXPECT_TRUE(contains(guiManager, "\"Blocker Search Texels\""));
  EXPECT_TRUE(contains(guiManager, "\"Max PCSS Filter Texels\""));
}

TEST(RenderingConventionTests,
     DirectionalPcssBlockerSearchUsesUnbiasedDepthAndRejectsOutOfBoundsTaps) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  const size_t blockerStart =
      shadowCommon.find("float DirectionalPcssAverageBlockerDepth");
  ASSERT_NE(blockerStart, std::string::npos);
  const size_t blockerEnd =
      shadowCommon.find("float DirectionalPcssSearchRadiusTexels",
                        blockerStart);
  ASSERT_NE(blockerEnd, std::string::npos);
  const std::string blockerSearch =
      shadowCommon.substr(blockerStart, blockerEnd - blockerStart);

  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch, "float receiverDepth"));
  EXPECT_FALSE(containsIgnoringWhitespace(blockerSearch, "float compareDepth"));
  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch,
                       "float receiverCompareDepth = saturate(receiverDepth)"));
  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch,
                       "int2 baseTexel = clamp(int2(floor(shadowUV * "
                       "atlasSize))"));
  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch, "if (any(sampleTexel < minTexel) ||"));
  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch, "any(sampleTexel > maxTexel)"));
  EXPECT_TRUE(containsIgnoringWhitespace(blockerSearch, "continue;"));

  const size_t filterStart =
      shadowCommon.find("float DirectionalPcssFilterRadiusTexels");
  ASSERT_NE(filterStart, std::string::npos);
  const size_t filterEnd = shadowCommon.find("uint SelectCascade", filterStart);
  ASSERT_NE(filterEnd, std::string::npos);
  const std::string filterBlock =
      shadowCommon.substr(filterStart, filterEnd - filterStart);
  EXPECT_TRUE(containsIgnoringWhitespace(filterBlock, "float receiverDepth"));
  EXPECT_FALSE(containsIgnoringWhitespace(filterBlock, "float compareDepth"));
  EXPECT_TRUE(containsIgnoringWhitespace(
      filterBlock,
      "DirectionalPcssAverageBlockerDepth(\n"
      "        shadowUV, cascadeIndex, receiverDepth, blockerSearchRadiusTexels"));

  const size_t sampleCascadeStart =
      shadowCommon.find("float SampleCascadeShadow");
  ASSERT_NE(sampleCascadeStart, std::string::npos);
  const size_t sampleCascadeEnd =
      shadowCommon.find("// Compute the shadow factor", sampleCascadeStart);
  ASSERT_NE(sampleCascadeEnd, std::string::npos);
  const std::string sampleCascade =
      shadowCommon.substr(sampleCascadeStart,
                          sampleCascadeEnd - sampleCascadeStart);
  EXPECT_TRUE(containsIgnoringWhitespace(sampleCascade,
                       "float receiverDepth = saturate(shadowNDC.z)"));
  EXPECT_TRUE(containsIgnoringWhitespace(sampleCascade, "DirectionalPcssFilterRadiusTexels("));
  EXPECT_TRUE(containsIgnoringWhitespace(sampleCascade, "shadowUV, cascadeIndex, receiverDepth"));
  EXPECT_TRUE(containsIgnoringWhitespace(sampleCascade, "shadowData, shadowAtlas"));
  EXPECT_TRUE(containsIgnoringWhitespace(sampleCascade, "float compareDepth = shadowNDC.z + bias"));
}

TEST(RenderingConventionTests,
     DirectionalPcssBlockerSearchUsesCascadeStableSampleRotation) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_TRUE(contains(shadowCommon, "ShadowCascadeStableRotation"));
  EXPECT_FALSE(contains(shadowCommon, "ShadowStableTexelRotation"));
  EXPECT_FALSE(contains(shadowCommon, "float2 texel = float2(shadowTexel)"));

  const size_t blockerStart =
      shadowCommon.find("float DirectionalPcssAverageBlockerDepth");
  ASSERT_NE(blockerStart, std::string::npos);
  const size_t blockerEnd =
      shadowCommon.find("float DirectionalPcssSearchRadiusTexels",
                        blockerStart);
  ASSERT_NE(blockerEnd, std::string::npos);
  const std::string blockerSearch =
      shadowCommon.substr(blockerStart, blockerEnd - blockerStart);

  EXPECT_TRUE(contains(blockerSearch, "int2 baseTexel = clamp"));
  EXPECT_TRUE(contains(
      blockerSearch,
      "float rotation = ShadowCascadeStableRotation(cascadeIndex)"));
  EXPECT_FALSE(
      contains(blockerSearch, "ShadowStableRotation(shadowUV, cascadeIndex)"));
}

TEST(RenderingConventionTests,
     DirectionalPcssRequiresStableBlockerSupportBeforeExpandingPenumbra) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_TRUE(contains(shadowCommon, "SHADOW_PCSS_MIN_BLOCKER_SAMPLE_COUNT"));
  const size_t filterStart =
      shadowCommon.find("float DirectionalPcssFilterRadiusTexels");
  ASSERT_NE(filterStart, std::string::npos);
  const size_t filterEnd =
      shadowCommon.find("// Select the cascade index", filterStart);
  ASSERT_NE(filterEnd, std::string::npos);
  const std::string filterBlock =
      shadowCommon.substr(filterStart, filterEnd - filterStart);

  EXPECT_TRUE(contains(filterBlock,
                       "blockerCount < SHADOW_PCSS_MIN_BLOCKER_SAMPLE_COUNT"));
  EXPECT_TRUE(contains(filterBlock, "blockerConfidence"));
  EXPECT_TRUE(contains(filterBlock, "smoothstep("));
  EXPECT_TRUE(contains(filterBlock,
                       "float(blockerCount - "
                       "SHADOW_PCSS_MIN_BLOCKER_SAMPLE_COUNT + 1u)"));
  EXPECT_TRUE(contains(filterBlock,
                       "lerp(baseFilterRadiusTexels, expandedFilterRadiusTexels"));
}

TEST(RenderingConventionTests,
     DirectionalPcssSkipsRadiusWorkWhenPenumbraCannotExpand) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_EQ(shadowCommon.find("ClassifyDirectionalLitShadowRegion"),
            std::string::npos);
  EXPECT_EQ(shadowCommon.find("SHADOW_DIRECTIONAL_CLASSIFICATION"),
            std::string::npos);

  const size_t sampleCascadeStart =
      shadowCommon.find("float SampleCascadeShadow");
  ASSERT_NE(sampleCascadeStart, std::string::npos);
  const size_t sampleCascadeEnd =
      shadowCommon.find("// Compute the shadow factor", sampleCascadeStart);
  ASSERT_NE(sampleCascadeEnd, std::string::npos);
  const std::string sampleCascade =
      shadowCommon.substr(sampleCascadeStart,
                          sampleCascadeEnd - sampleCascadeStart);
  const size_t pcssGate =
      sampleCascade.find("bool pcssCanExpandPenumbra =");
  const size_t pcssCall =
      sampleCascade.find("DirectionalPcssFilterRadiusTexels(");
  ASSERT_NE(pcssGate, std::string::npos);
  ASSERT_NE(pcssCall, std::string::npos);
  EXPECT_LT(pcssGate, pcssCall);
  EXPECT_TRUE(contains(sampleCascade,
                       "float filterRadiusTexels = baseFilterRadiusTexels"));
  EXPECT_TRUE(contains(
      sampleCascade,
      "ShadowFiniteOr(shadowData.softShadowSettings.x, 0.0) > 0.0"));
  EXPECT_TRUE(contains(
      sampleCascade,
      "ShadowFiniteOr(shadowData.softShadowSettings.y, 0.0) > 0.0"));
  EXPECT_TRUE(contains(sampleCascade, "if (pcssCanExpandPenumbra)"));
  EXPECT_TRUE(contains(
      sampleCascade,
      "filterRadiusTexels = DirectionalPcssFilterRadiusTexels("));
  EXPECT_EQ(sampleCascade.find("return 1.0;\n"
                               "    }\n"
                               "\n"
                               "    float filterRadiusTexels"),
            std::string::npos);
}

TEST(RenderingConventionTests,
     DirectionalCascadeProjectionUsesTexelGuardBand) {
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");

  EXPECT_TRUE(contains(shadowManager, "kDirectionalCascadeGuardTexels"));
  EXPECT_TRUE(contains(shadowManager,
                       "expandShadowCascadeRadiusForFilterGuard("));
  EXPECT_TRUE(contains(shadowManager, "const float projectionRadius"));
  EXPECT_TRUE(contains(shadowManager,
                       "const float cascadeExtent = projectionRadius * 2.0f"));
  EXPECT_TRUE(contains(shadowManager,
                       "bounds.minBounds = {-projectionRadius, "
                       "-projectionRadius"));
  EXPECT_TRUE(contains(shadowManager,
                       "outBounds.minBounds = {-projectionRadius, "
                       "-projectionRadius"));
  EXPECT_TRUE(contains(
      shadowManager,
      "-projectionRadius, projectionRadius, -projectionRadius, "
      "projectionRadius"));
  EXPECT_TRUE(contains(shadowManager,
                       "result.worldRadius = projectionRadius"));
  EXPECT_FALSE(contains(
      shadowManager,
      "const float cascadeExtent = cascadeRadius * 2.0f"));
}

TEST(RenderingConventionTests,
     DirectionalShadowsUseScreenSpaceContactVisibility) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string screenSpaceShadow =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(sceneData, "directionalContactVisibility"));
  EXPECT_TRUE(contains(sceneData, "directionalContactMaxDistance"));
  EXPECT_TRUE(contains(sceneData, "directionalContactThickness"));
  EXPECT_TRUE(contains(sceneData, "directionalContactFadeDistance"));
  EXPECT_TRUE(
      contains(sceneData, "kDefaultDirectionalContactMaxDistance = 0.35f"));
  EXPECT_TRUE(
      contains(sceneData, "kDefaultDirectionalContactThickness = 0.04f"));
  EXPECT_TRUE(
      contains(sceneData, "kDefaultDirectionalContactFadeDistance = 0.30f"));
  EXPECT_TRUE(contains(lightingStructs, "float4 contactShadowSettings"));
  EXPECT_TRUE(contains(shadowManager, "shadowData_.contactShadowSettings"));
  EXPECT_TRUE(contains(
      shadowManager,
      "shadowSettings.directionalContactMaxDistance, 0.0f, 0.75f"));

  EXPECT_TRUE(contains(
      screenSpaceShadow, "ScreenSpaceDirectionalContactVisibility"));
  EXPECT_TRUE(contains(screenSpaceShadow,
                       "SCREEN_SPACE_DIRECTIONAL_CONTACT_SHADOW_STEPS"));
  EXPECT_TRUE(contains(screenSpaceShadow,
                       "ScreenSpaceDirectionalContactRayJitter("));
  EXPECT_TRUE(contains(screenSpaceShadow, "float pixelSeed = dot(float2(pixel)"));
  EXPECT_TRUE(contains(screenSpaceShadow, "52.9829189"));
  EXPECT_TRUE(contains(screenSpaceShadow, "float rayJitter ="));
  EXPECT_TRUE(contains(screenSpaceShadow, "* 0.25"));
  EXPECT_TRUE(contains(screenSpaceShadow,
                       "if (!TryProjectWorldToScenePixel(worldPosition"));
  EXPECT_TRUE(contains(screenSpaceShadow, "return 1.0"));
  EXPECT_TRUE(contains(screenSpaceShadow, "clamp("));
  EXPECT_TRUE(contains(screenSpaceShadow, "sampleIndex) + 0.5 + rayJitter"));
  EXPECT_TRUE(contains(screenSpaceShadow, "sampleT * sampleT"));
  EXPECT_TRUE(contains(screenSpaceShadow, "TryProjectWorldToScenePixel"));
  EXPECT_TRUE(contains(screenSpaceShadow, "TryReconstructWorldPosition"));
  EXPECT_TRUE(contains(screenSpaceShadow, "grazingNormalFade"));
  EXPECT_TRUE(contains(screenSpaceShadow, "coplanarReceiverSurface"));
  EXPECT_TRUE(contains(screenSpaceShadow, "distanceFromLightRay"));
  EXPECT_TRUE(contains(screenSpaceShadow, "return saturate(visibility)"));

  EXPECT_TRUE(contains(directional,
                       "#include \"screen_space_light_shadow_common.slang\""));
  EXPECT_TRUE(contains(directional, "uShadow.contactShadowSettings.x"));
  EXPECT_TRUE(
      contains(directional, "ScreenSpaceDirectionalContactVisibility("));
  EXPECT_TRUE(contains(directional, "float contactVisibility = 1.0"));
  EXPECT_TRUE(contains(
      directional, "DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD = 0.02"));
  EXPECT_TRUE(contains(
      directional,
      "DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD = 1.0e-4"));
  EXPECT_TRUE(contains(
      directional,
      "shadowFactor > DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD"));
  EXPECT_TRUE(contains(
      directional,
      "float directionalRadiance = max(max(lightColor.r, lightColor.g), "
      "lightColor.b)"));
  EXPECT_TRUE(contains(
      directional,
      "directionalRadiance > DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD"));
  EXPECT_TRUE(contains(directional,
                       "float directShadowVisibility = shadowFactor * "
                       "contactVisibility"));
  EXPECT_TRUE(contains(directional, ") * directShadowVisibility"));
  EXPECT_TRUE(contains(directional, "shadowFactor, occlusion"));
  EXPECT_FALSE(contains(directional, "shadowFactor *= contactVisibility"));

  EXPECT_TRUE(contains(guiManager, "\"Directional contact shadows\""));
  EXPECT_TRUE(contains(guiManager, "\"Contact Max Distance\""));
  EXPECT_TRUE(contains(guiManager, "0.0f, 0.75f, \"%.2f\""));
  EXPECT_TRUE(contains(guiManager, "\"Contact Thickness\""));
  EXPECT_TRUE(contains(guiManager, "\"Contact Fade Distance\""));
}

TEST(RenderingConventionTests,
     ForwardRasterUsesDirectionalContactShadowVisibility) {
  const std::string forwardTechnique =
      stripAsciiWhitespace(readRepoTextFile("src/renderer/forward/ForwardRasterTechnique.cpp"));
  const std::array<std::string_view, 2> shaderPaths = {{
      "shaders/forward_opaque.slang",
      "shaders/forward_transparent.slang",
  }};

  for (const std::string_view shaderPath : shaderPaths) {
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));

    EXPECT_TRUE(contains(shader,
                         "#include \"screen_space_light_shadow_common.slang\""))
        << shaderPath;
    EXPECT_TRUE(contains(shader, "DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD"))
        << shaderPath;
    EXPECT_TRUE(
        contains(shader, "DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD"))
        << shaderPath;
    EXPECT_TRUE(contains(shader, "float contactVisibility = 1.0"))
        << shaderPath;
    EXPECT_TRUE(
        contains(shader, "ScreenSpaceDirectionalContactVisibilityDepthOnly("))
        << shaderPath;
    EXPECT_TRUE(contains(shader, "uShadow.contactShadowSettings.x"))
        << shaderPath;
    EXPECT_TRUE(contains(shader, "gSceneDepthTexture")) << shaderPath;
    EXPECT_FALSE(contains(shader, "gSceneNormalTexture")) << shaderPath;
    EXPECT_TRUE(contains(shader,
                         "float directShadowVisibility = shadowFactor * "
                         "contactVisibility"))
        << shaderPath;
  }

  const size_t lightingPass =
      forwardTechnique.find("graph.setPassResourceAccess(RenderPassId::Lighting");
  ASSERT_NE(lightingPass, std::string::npos);
  const size_t lightingEnd =
      forwardTechnique.find("graph.setPassResourceTransitions", lightingPass);
  ASSERT_NE(lightingEnd, std::string::npos);
  const std::string lightingAccess =
      forwardTechnique.substr(lightingPass, lightingEnd - lightingPass);
  EXPECT_TRUE(containsIgnoringWhitespace(lightingAccess, "RenderResourceId::SceneDepth"));
  EXPECT_FALSE(containsIgnoringWhitespace(lightingAccess, "RenderResourceId::GBufferNormal"));
}

TEST(RenderingConventionTests,
     ForwardRasterOpaqueUsesDepthOnlyPointContactVisibility) {
  const std::string opaque = readRepoTextFile("shaders/forward_opaque.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(lightingStructs, "uint localContactVisibility"));
  EXPECT_TRUE(contains(sceneData, "uint32_t localContactVisibility"));
  EXPECT_TRUE(contains(sceneData,
                       "offsetof(LightingData, localContactVisibility) == 68"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "lightingData.localContactVisibility ="));
  EXPECT_TRUE(contains(rendererFrontend,
                       "shadowSettings.localContactVisibility ? 1u : 0u"));

  EXPECT_TRUE(contains(opaque, "uLighting.localContactVisibility != 0u"));
  EXPECT_TRUE(contains(opaque,
                       "ShouldTracePointLightContactShadow(radiance, lightVisibility)"));
  EXPECT_TRUE(
      contains(opaque, "ScreenSpacePointLightContactVisibilityDepthOnly("));
  EXPECT_TRUE(contains(opaque, "radiance *= lightVisibility"));
  EXPECT_FALSE(contains(opaque, "gSceneNormalTexture"));

  EXPECT_FALSE(
      contains(transparent, "ScreenSpacePointLightContactVisibility"));
}

TEST(RenderingConventionTests,
     DirectionalContactShadowsRejectSameSurfaceEdgeHits) {
  const std::string screenSpaceShadow =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");

  const size_t functionStart = screenSpaceShadow.find(
      "float ScreenSpaceDirectionalContactVisibility");
  ASSERT_NE(functionStart, std::string::npos);
  const size_t functionEnd =
      screenSpaceShadow.find("return lerp(1.0, saturate(visibility)",
                             functionStart);
  ASSERT_NE(functionEnd, std::string::npos);
  const std::string directionalContact =
      screenSpaceShadow.substr(functionStart, functionEnd - functionStart);

  EXPECT_TRUE(contains(directionalContact,
                       "SCREEN_SPACE_DIRECTIONAL_CONTACT_MIN_VISIBILITY"));
  EXPECT_TRUE(contains(directionalContact, "receiverSurfaceSeparation"));
  EXPECT_TRUE(contains(directionalContact,
                       "receiverSurfaceSeparation <= rayThickness * 0.5"));
  EXPECT_TRUE(contains(directionalContact, "contactSeparationFade"));
  EXPECT_TRUE(contains(directionalContact, "hitVisibility"));
  EXPECT_TRUE(contains(directionalContact, "max(hitVisibility,"));
  EXPECT_TRUE(contains(directionalContact,
                       "SCREEN_SPACE_DIRECTIONAL_CONTACT_MIN_VISIBILITY)"));
}

TEST(RenderingConventionTests, ShadowRasterDepthBiasIsDynamicFrameSetting) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string gpuVisibilityRecorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(sceneData, "float rasterConstantBias{-4.0f}"));
  EXPECT_TRUE(contains(sceneData, "float rasterSlopeBias{-1.5f}"));
  EXPECT_TRUE(contains(sceneData, "bool localContactVisibility{true}"));
  EXPECT_TRUE(contains(frameRecorderHeader, "container::gpu::ShadowSettings"));
  EXPECT_TRUE(contains(rendererFrontend, "p.shadows.shadowSettings"));
  EXPECT_TRUE(
      contains(rendererFrontend,
               "subs_.guiManager ? subs_.guiManager->shadowSettings()"));
  EXPECT_TRUE(contains(rendererFrontend, ": container::gpu::ShadowSettings{}"));

  EXPECT_TRUE(contains(pipelineBuilder, "VK_DYNAMIC_STATE_DEPTH_BIAS"));
  EXPECT_TRUE(contains(pipelineBuilder, "shadowDynState"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "shadowRaster.depthBiasConstantFactor = 0.0f"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "shadowRaster.depthBiasSlopeFactor = 0.0f"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "sdPCI.pDynamicState = &shadowDynState"));

  EXPECT_TRUE(contains(shadowPassRecorder, "vkCmdSetDepthBias(cmd"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "params.shadows.shadowSettings.rasterConstantBias"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "params.shadows.shadowSettings.rasterSlopeBias"));

  EXPECT_TRUE(contains(guiManager, "\"Raster Constant Bias\""));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterConstantBias"));
  EXPECT_TRUE(contains(guiManager, "\"Raster Slope Bias\""));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterSlopeBias"));
}

TEST(RenderingConventionTests, ShadowUploadSanitizesBiasAndFilterSettings) {
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");

  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.normalBiasMinTexels, 0.0f)"));
  EXPECT_TRUE(contains(
      shadowManager,
      "std::max(shadowSettings.normalBiasMaxTexels, normalBiasMinTexels)"));
  EXPECT_TRUE(
      contains(shadowManager, "std::max(shadowSettings.slopeBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(
      shadowManager, "std::max(shadowSettings.receiverPlaneBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.filterRadiusTexels, 0.25f)"));
  EXPECT_TRUE(
      contains(shadowManager,
               "std::clamp(shadowSettings.cascadeBlendFraction, 0.0f, 0.45f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.constantDepthBias, 0.0f)"));
  EXPECT_TRUE(
      contains(shadowManager, "std::max(shadowSettings.maxDepthBias, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager, "kLocalShadowNormalBiasMinTexels"));
  EXPECT_TRUE(contains(shadowManager, "kLocalShadowNormalBiasMaxTexels"));
  EXPECT_TRUE(contains(shadowManager, "localNormalBiasMinTexels"));
  EXPECT_TRUE(contains(shadowManager, "localNormalBiasMaxTexels"));
}

TEST(RenderingConventionTests,
     ShadowCasterSceneBoundsSkipInvalidCasterSpheresWithoutDiscardingScene) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(rendererFrontend,
                       "bool isValidShadowCasterBoundingSphere"));
  EXPECT_TRUE(contains(rendererFrontend, "sphere.w > 0.0f"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bool accumulateShadowCasterDrawCommandBounds"));

  const size_t shadowDrawBoundsStart =
      rendererFrontend.find("accumulateShadowCasterDrawCommandBounds");
  ASSERT_NE(shadowDrawBoundsStart, std::string::npos);
  const size_t shadowDrawBoundsEnd =
      rendererFrontend.find("void includeBounds", shadowDrawBoundsStart);
  ASSERT_NE(shadowDrawBoundsEnd, std::string::npos);
  const std::string shadowDrawBoundsBlock = rendererFrontend.substr(
      shadowDrawBoundsStart, shadowDrawBoundsEnd - shadowDrawBoundsStart);
  EXPECT_TRUE(contains(shadowDrawBoundsBlock,
                       "isValidShadowCasterBoundingSphere"));
  EXPECT_TRUE(contains(shadowDrawBoundsBlock, "continue;"));
  EXPECT_FALSE(contains(shadowDrawBoundsBlock, "return std::nullopt"));

  const size_t shadowCasterBoundsStart =
      rendererFrontend.find("accumulateShadowCasterSceneBounds");
  ASSERT_NE(shadowCasterBoundsStart, std::string::npos);
  const size_t shadowCasterBoundsEnd =
      rendererFrontend.find("float transformGizmoScale", shadowCasterBoundsStart);
  ASSERT_NE(shadowCasterBoundsEnd, std::string::npos);
  const std::string shadowCasterBoundsBlock = rendererFrontend.substr(
      shadowCasterBoundsStart, shadowCasterBoundsEnd - shadowCasterBoundsStart);
  EXPECT_TRUE(contains(shadowCasterBoundsBlock,
                       "accumulateShadowCasterDrawCommandBounds"));
  EXPECT_FALSE(contains(shadowCasterBoundsBlock, "std::optional<bool>"));
  EXPECT_FALSE(contains(shadowCasterBoundsBlock,
                        "accumulateDrawCommandBounds("));
}

TEST(RenderingConventionTests,
     ShadowCascadeLightDistanceRebuildUsesBoundedRetryLoop) {
  const std::string shadowManager =
      readRepoTextFile("src/renderer/shadow/ShadowManager.cpp");

  const size_t computeStart =
      shadowManager.find("ShadowManager::CascadeViewProjData "
                         "ShadowManager::computeCascadeViewProj");
  ASSERT_NE(computeStart, std::string::npos);
  const size_t projectionStart =
      shadowManager.find("const glm::mat4 lightProj", computeStart);
  ASSERT_NE(projectionStart, std::string::npos);
  const std::string rebuildBlock =
      shadowManager.substr(computeStart, projectionStart - computeStart);

  EXPECT_TRUE(contains(rebuildBlock, "kMaxLightDistanceRebuilds"));
  EXPECT_TRUE(contains(rebuildBlock, "for (uint32_t rebuild = 0"));
  EXPECT_TRUE(
      contains(rebuildBlock, "depthPlan.lightDistanceIncrease > 1.0e-4f"));
  EXPECT_FALSE(contains(rebuildBlock,
                        "depthPlan.lightDistanceIncrease = 0.0f"));
  const size_t finalFallbackStart =
      rebuildBlock.find("if (depthPlan.lightDistanceIncrease > 1.0e-4f)");
  if (finalFallbackStart != std::string::npos) {
    const std::string finalFallbackBlock =
        rebuildBlock.substr(finalFallbackStart);
    EXPECT_TRUE(contains(finalFallbackBlock,
                         "depthPlan = depthPlanForView(lightView, "
                         "lightDistance)"));
  }
  EXPECT_FALSE(containsIgnoringWhitespace(
      rebuildBlock,
      "if (depthPlan.lightDistanceIncrease > 0.0f) {"
      "lightDistance = depthPlan.lightDistance;"
      "lightView = makeSnappedLightView(lightDistance);"
      "depthPlan = depthPlanForView(lightView, lightDistance);"
      "if (depthPlan.lightDistanceIncrease > 1.0e-4f)"));
}

TEST(RenderingConventionTests, ShadowCascadeSelectionUsesOrderedSplitDepths) {
  const std::array<float, 4> splits = {5.0f, 15.0f, 45.0f, 100.0f};

  EXPECT_LT(splits[0], splits[1]);
  EXPECT_LT(splits[1], splits[2]);
  EXPECT_LT(splits[2], splits[3]);

  EXPECT_EQ(selectShadowCascade(0.1f, splits), 0u);
  EXPECT_EQ(selectShadowCascade(4.999f, splits), 0u);
  EXPECT_EQ(selectShadowCascade(5.0f, splits), 1u);
  EXPECT_EQ(selectShadowCascade(44.999f, splits), 2u);
  EXPECT_EQ(selectShadowCascade(45.0f, splits), 3u);
  EXPECT_EQ(selectShadowCascade(120.0f, splits), 3u);
}

TEST(RenderingConventionTests, ShadowShaderStructsExposeCascadeSplitMetadata) {
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_NE(lightingStructs.find("static const uint SHADOW_CASCADE_COUNT = 4u"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("struct ShadowCascadeData"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("float splitDepth;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float texelSize;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float worldRadius;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float depthRange;"), std::string::npos);
  EXPECT_NE(
      lightingStructs.find("ShadowCascadeData cascades[SHADOW_CASCADE_COUNT]"),
      std::string::npos);
  EXPECT_NE(lightingStructs.find("float4 biasSettings;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float4 filterSettings;"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.cascadeSplits[i]"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.cascadeSplits[cascadeIndex]"),
            std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.biasSettings"), std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.filterSettings"), std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.cascades[cascadeIndex].texelSize"),
            std::string::npos);
  EXPECT_NE(postProcess.find("uShadow.filterSettings.y"), std::string::npos);
  EXPECT_NE(postProcess.find("uShadow.cascades[cascadeIndex].texelSize"),
            std::string::npos);
}

TEST(RenderingConventionTests, ShadowDebugViewsUseDataDrivenCascadeMetadata) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(guiHeader, "ShadowCascades = 11"));
  EXPECT_TRUE(contains(guiHeader, "ShadowTexelDensity = 13"));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Cascades\""));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Texel Density\""));

  EXPECT_TRUE(contains(postProcess, "DebugTextureViewColor(outputMode"));
  EXPECT_TRUE(contains(postProcess, "ValidSceneDepth(depthSample)"));
  EXPECT_TRUE(contains(postProcess, "outputMode >= 1u && outputMode <= 7u"));
  EXPECT_FALSE(contains(postProcess, "if (outputMode == 1u)"));
  EXPECT_FALSE(
      contains(postProcess, "if (outputMode == 6u || outputMode == 7u)"));
  EXPECT_TRUE(contains(postProcess, "if (mode == 11u)"));
  EXPECT_TRUE(contains(postProcess, "pc.cascadeSplits[cascadeIndex]"));
  EXPECT_TRUE(
      contains(postProcess,
               "clamp(FiniteOr(uShadow.filterSettings.y, 0.0), 0.0, 0.45)"));
  EXPECT_TRUE(contains(postProcess, "isfinite(cascadeRange)"));
  EXPECT_TRUE(contains(
      postProcess,
      "lerp(color, cascadeColors[cascadeIndex + 1u], blendIndicator)"));
  EXPECT_FALSE(contains(postProcess, "float blendFraction = 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.10"));

  EXPECT_TRUE(contains(postProcess, "if (mode == 13u)"));
  EXPECT_TRUE(contains(postProcess,
                       "FiniteOr(uShadow.cascades[cascadeIndex].texelSize"));
  EXPECT_TRUE(contains(postProcess, "float texelsPerMeter = 1.0 / texelSize"));
  EXPECT_TRUE(contains(postProcess,
                       "float density = saturate(log2(max(texelsPerMeter"));
  EXPECT_FALSE(contains(postProcess, "float texelSize = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 0.0"));
}

TEST(RenderingConventionTests, GuiDebugStateAvoidsHeavyRendererDebugIncludes) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiDebugState =
      readRepoTextFile("include/Container/utility/GuiDebugState.h");
  const std::string debugPresenterHeader =
      readRepoTextFile("include/Container/renderer/debug/DebugUiPresenter.h");
  const std::string debugPresenter =
      readRepoTextFile("src/renderer/debug/DebugUiPresenter.cpp");

  EXPECT_TRUE(contains(guiHeader, "GuiDebugState.h"));
  EXPECT_TRUE(contains(guiHeader, "enum class BimSemanticColorMode"));
  EXPECT_TRUE(contains(guiHeader, "struct RendererTelemetryView"));
  EXPECT_FALSE(contains(guiHeader, "BimSemanticColorMode.h"));
  EXPECT_FALSE(contains(guiHeader, "RendererTelemetry.h"));
  EXPECT_FALSE(contains(guiHeader, "container::renderer::RendererTelemetryView "
                                   "rendererTelemetry_"));

  EXPECT_TRUE(contains(guiDebugState, "struct RenderPassToggle"));
  EXPECT_TRUE(contains(guiDebugState, "GuiRendererTelemetryView"));
  EXPECT_TRUE(contains(debugPresenterHeader, "TechniqueDebugModel.h"));
  EXPECT_TRUE(contains(debugPresenterHeader, "publishRenderGraphDebugModel"));
  EXPECT_FALSE(contains(debugPresenterHeader, "RenderGraph.h"));
  EXPECT_FALSE(contains(debugPresenterHeader, "GuiManager.h"));
  EXPECT_TRUE(contains(debugPresenter, "graph.debugModel()"));
}

TEST(RenderingConventionTests, ShadowShadersUseDataDrivenDepthBiasSettings) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string shadowDepth =
      readRepoTextFile("shaders/shadow_depth.slang");

  EXPECT_TRUE(contains(shadowCommon, "float ComputeReceiverPlaneDepthBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "float ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "float bias = ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon,
                       "ComputeReceiverPlaneDepthBias(shadowNDC, shadowData)"));
  EXPECT_TRUE(
      contains(shadowCommon, "float compareDepth = shadowNDC.z + bias"));

  EXPECT_TRUE(contains(shadowDepth, "ConstantBuffer<ShadowBuffer> uShadow"));
  EXPECT_TRUE(
      contains(shadowDepth, "uShadow.cascades[pc.cascadeIndex].viewProj"));
  EXPECT_TRUE(contains(shadowDepth, "[[vk::location(5)]] float2 inTexCoord1"));
  EXPECT_TRUE(contains(shadowDepth, "material.baseColorTextureTransform"));
  EXPECT_TRUE(contains(shadowDepth, "ApplyGpuTextureTransform"));
}

TEST(RenderingConventionTests,
     DirectionalReceiverPlaneBiasAvoidsScreenDerivatives) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  const size_t biasStart =
      shadowCommon.find("float ComputeReceiverPlaneDepthBias");
  ASSERT_NE(biasStart, std::string::npos);
  const size_t biasEnd =
      shadowCommon.find("float ComputeSlopeScaledBias", biasStart);
  ASSERT_NE(biasEnd, std::string::npos);
  const std::string receiverPlaneBias =
      shadowCommon.substr(biasStart, biasEnd - biasStart);

  EXPECT_FALSE(contains(receiverPlaneBias, "ddx("));
  EXPECT_FALSE(contains(receiverPlaneBias, "ddy("));
}

TEST(RenderingConventionTests, OcclusionCullSphereBoundsNeedProjectionScale) {
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

  constexpr float kSphereRadius = 1.0f;
  constexpr float kSphereViewDepth = 5.0f;
  const glm::vec4 topPointClip =
      proj * glm::vec4(0.0f, kSphereRadius, -kSphereViewDepth, 1.0f);
  const float actualNdcYExtent = std::abs(clipToNdc(topPointClip).y);

  const float oldProjectedRadius = kSphereRadius / kSphereViewDepth;
  const float conservativeProjectedRadius = rowXyzLength(proj, 1) *
                                            kSphereRadius /
                                            (kSphereViewDepth - kSphereRadius);

  EXPECT_LT(oldProjectedRadius, actualNdcYExtent);
  EXPECT_GE(conservativeProjectedRadius, actualNdcYExtent);
}

TEST(RenderingConventionTests,
     OcclusionCullSphereDepthUsesNearestPointProjection) {
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

  constexpr float kSphereRadius = 2.0f;
  constexpr float kSphereCenterDepth = 10.0f;
  const glm::vec4 centerClip =
      proj * glm::vec4(0.0f, 0.0f, -kSphereCenterDepth, 1.0f);
  const float oldMixedDepth =
      clipToNdc(centerClip).z + (kSphereRadius / centerClip.w);

  const glm::vec4 nearestPointClip =
      proj * glm::vec4(0.0f, 0.0f, -(kSphereCenterDepth - kSphereRadius), 1.0f);
  const float exactClosestDepth = clipToNdc(nearestPointClip).z;

  EXPECT_GT(oldMixedDepth, exactClosestDepth);
  EXPECT_NEAR(exactClosestDepth, clipToNdc(nearestPointClip).z, 1e-6f);
}

TEST(RenderingConventionTests, OcclusionCullKeepsLargeProjectedBoundsVisible) {
  EXPECT_TRUE(occlusionCullMayRejectProjectedBounds(32.0f));
  EXPECT_TRUE(occlusionCullMayRejectProjectedBounds(96.0f));
  EXPECT_FALSE(occlusionCullMayRejectProjectedBounds(96.01f));
  EXPECT_FALSE(occlusionCullMayRejectProjectedBounds(256.0f));
}

TEST(RenderingConventionTests,
     ShadowAndOcclusionCullTreatInvalidProjectionDataAsVisible) {
  const std::string shadowCull = readRepoTextFile("shaders/shadow_cull.slang");
  const std::string occlusionCull =
      readRepoTextFile("shaders/occlusion_cull.slang");

  EXPECT_TRUE(contains(shadowCull, "!all(isfinite(boundingSphere))"));
  EXPECT_TRUE(contains(shadowCull, "!all(isfinite(sphere))"));
  EXPECT_TRUE(contains(shadowCull, "!all(isfinite(center))"));
  EXPECT_TRUE(contains(shadowCull, "!all(isfinite(cascade.casterMinBounds))"));
  EXPECT_TRUE(contains(occlusionCull, "!all(isfinite(worldPoint))"));
  EXPECT_TRUE(contains(occlusionCull, "!all(isfinite(clip))"));
  EXPECT_TRUE(contains(occlusionCull, "!all(isfinite(clipCenter))"));
  EXPECT_TRUE(contains(occlusionCull, "!all(isfinite(ndc))"));
}

TEST(RenderingConventionTests, DefaultConfigUsesCornellBoxScene) {
  const auto config = container::app::DefaultAppConfig();
  constexpr std::string_view kCornellBoxScene =
      "models/validation/cornell_box_local_light.gltf";

  EXPECT_EQ(config.modelPath, container::app::kDefaultModelRelativePath);
  EXPECT_EQ(container::app::kDefaultModelRelativePath, kCornellBoxScene);
  EXPECT_NE(config.modelPath, container::app::kDefaultSceneModelToken);
  EXPECT_FALSE(config.enableValidationLayers);
}

TEST(RenderingConventionTests, DefaultCornellSceneIsCopiedToBuildModels) {
  const std::string assetsCmake = readRepoTextFile("cmake/Assets.cmake");

  EXPECT_TRUE(contains(assetsCmake, "copy_validation_models"));
  EXPECT_TRUE(contains(assetsCmake, "${MODELS_DIR}/validation"));
  EXPECT_TRUE(contains(assetsCmake, "${MODELS_OUTPUT_DIR}/validation"));
  EXPECT_TRUE(contains(
      assetsCmake, "add_dependencies(generate_models copy_validation_models)"));
}

TEST(RenderingConventionTests,
     VisualRegressionHarnessPassesFixtureRenderModeToHeadlessApp) {
  const std::string visualRegression =
      readRepoTextFile("tests/validation/visual_regression_gpu_tests.cpp");

  EXPECT_TRUE(contains(visualRegression, "scene.at(\"renderMode\")"));
  EXPECT_TRUE(
      contains(visualRegression, "args.emplace_back(\"--display-mode\")"));
  EXPECT_TRUE(contains(visualRegression, "CONTAINER_VISUAL_REGRESSION_SCENE"));
  EXPECT_TRUE(contains(visualRegression, "\"skipReason\", \"scene filter\""));
}

TEST(RenderingConventionTests,
     VisualRegressionHarnessPassesDirectionalLightOverridesToHeadlessApp) {
  const std::string appConfig =
      readRepoTextFile("include/Container/app/AppConfig.h");
  const std::string main = readRepoTextFile("main.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string visualRegression =
      readRepoTextFile("tests/validation/visual_regression_gpu_tests.cpp");
  const std::string fixtures = readRepoTextFile(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const std::string yellowAreaLight =
      readRepoTextFile("models/validation/cornell_box_yellow_area_light.gltf");

  EXPECT_TRUE(contains(appConfig, "hasDirectionalDirectionOverride"));
  EXPECT_TRUE(contains(appConfig, "hasDirectionalColorOverride"));
  EXPECT_TRUE(contains(main, "arg == \"--directional-direction\""));
  EXPECT_TRUE(contains(main, "arg == \"--directional-color\""));
  EXPECT_TRUE(
      contains(rendererFrontend, "applyConfiguredDirectionalLightOverrides"));
  EXPECT_TRUE(contains(visualRegression,
                       "appendVec3(args, \"--directional-direction\""));
  EXPECT_TRUE(contains(visualRegression,
                       "appendVec3(args, \"--directional-color\""));
  EXPECT_TRUE(contains(
      fixtures, "cornell_yellow_area_purple_directional_shadow"));
  EXPECT_TRUE(contains(
      fixtures, "models/validation/cornell_box_yellow_area_light.gltf"));
  EXPECT_TRUE(contains(fixtures, "\"color\": [0.62, 0.35, 1.0]"));
  EXPECT_TRUE(contains(fixtures, "\"targetColor\": [1.0, 0.86, 0.10]"));
  EXPECT_TRUE(contains(yellowAreaLight, "Ceiling yellow soft area light"));
  EXPECT_TRUE(contains(yellowAreaLight,
                       "Cornell box yellow area purple directional shadow"));
}

TEST(RenderingConventionTests,
     VisualRegressionHarnessDisablesBloomForLightingShadowCaptures) {
  const std::string appConfig =
      readRepoTextFile("include/Container/app/AppConfig.h");
  const std::string main = readRepoTextFile("main.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string visualRegression =
      readRepoTextFile("tests/validation/visual_regression_gpu_tests.cpp");

  EXPECT_TRUE(contains(appConfig, "hasBloomEnabledOverride"));
  EXPECT_TRUE(contains(appConfig, "bool bloomEnabled"));
  EXPECT_TRUE(contains(main, "arg == \"--no-bloom\""));
  EXPECT_TRUE(
      contains(rendererFrontend, "svc_.config.hasBloomEnabledOverride"));
  EXPECT_TRUE(
      contains(rendererFrontend,
               "current.bloomEnabled = svc_.config.bloomEnabled"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "subs_.bloomManager->enabled() = current.bloomEnabled"));
  EXPECT_TRUE(contains(visualRegression, "args.emplace_back(\"--no-bloom\")"));
}

TEST(RenderingConventionTests,
     VisualRegressionHarnessBlocksPlannedFixtureGoldenPromotion) {
  const std::string visualRegression =
      readRepoTextFile("tests/validation/visual_regression_gpu_tests.cpp");

  EXPECT_TRUE(contains(visualRegression, "status != \"active\""));
  EXPECT_TRUE(contains(visualRegression, "goldenPromotionBlocked"));
  EXPECT_TRUE(contains(visualRegression,
                       "refusing to promote golden for non-active fixture"));
  EXPECT_TRUE(contains(
      visualRegression,
      "copyCandidateToGolden(candidatePath, goldenPath, overwriteGoldens)"));
}

TEST(RenderingConventionTests, CornellLightingProfileMatchesOnlyBundledSamples) {
  using container::app::IsDefaultAuthoredLocalLightScene;
  EXPECT_TRUE(IsDefaultAuthoredLocalLightScene(
      "models/validation/cornell_box_local_light.gltf"));
  EXPECT_TRUE(IsDefaultAuthoredLocalLightScene(
      "F:\\Assets\\MODELS\\VALIDATION\\CORNELL_BOX_YELLOW_AREA_LIGHT.GLTF"));
  EXPECT_FALSE(IsDefaultAuthoredLocalLightScene(
      "models/validation/taa_equivalent.bim"));
  EXPECT_FALSE(IsDefaultAuthoredLocalLightScene(
      "othermodels/validation/cornell_box_local_light.gltf"));
}

TEST(RenderingConventionTests,
     DefaultCornellStartupDisablesBloomUnlessExplicitlyOverridden) {
  const std::string appConfig =
      readRepoTextFile("include/Container/app/AppConfig.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(appConfig, "kDefaultAuthoredLocalLightBloomEnabled"));
  EXPECT_TRUE(
      contains(rendererFrontend, "kDefaultAuthoredLocalLightBloomEnabled"));
  EXPECT_TRUE(
      contains(rendererFrontend, "svc_.config.hasBloomEnabledOverride"));
  EXPECT_TRUE(
      contains(rendererFrontend,
               "IsDefaultAuthoredLocalLightScene(activePrimaryModelPath_)"));
}

TEST(RenderingConventionTests, HeadlessDisplayModeOverrideIsConfigDriven) {
  const std::string appConfig =
      readRepoTextFile("include/Container/app/AppConfig.h");
  const std::string main = readRepoTextFile("main.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string frameGraphContextHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterFrameGraphContext.h");
  const std::string lightingPassHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterLightingPassRecorder.h");
  const std::string lightingPass = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(appConfig, "std::string displayModeOverride"));
  EXPECT_TRUE(contains(main, "arg == \"--display-mode\""));
  EXPECT_TRUE(contains(rendererFrontend, "configuredDisplayMode(svc_.config)"));
  EXPECT_TRUE(contains(frameGraphContextHeader, "fallbackDisplayMode"));
  EXPECT_TRUE(contains(lightingPassHeader, "fallbackDisplayMode"));
  EXPECT_TRUE(contains(lightingPass, "currentDisplayMode(guiManager,"));
}

TEST(RenderingConventionTests, HeadlessRenderTechniqueOverrideIsConfigDriven) {
  const std::string appConfig =
      readRepoTextFile("include/Container/app/AppConfig.h");
  const std::string main = readRepoTextFile("main.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(appConfig, "std::string renderTechnique"));
  EXPECT_TRUE(contains(main, "arg == \"--render-technique\""));
  EXPECT_TRUE(
      contains(main, "config.renderTechnique ="));
  EXPECT_TRUE(contains(rendererFrontend, "svc_.config.renderTechnique"));
  EXPECT_TRUE(contains(rendererFrontend, "initializeRenderTechnique"));
}

TEST(RenderingConventionTests,
     DefaultSceneModelListContainsTriangleCubeAndSphere) {
  const auto &modelPaths = container::app::kDefaultSceneModelRelativePaths;

  ASSERT_EQ(modelPaths.size(), 3u);
  EXPECT_NE(modelPaths[0].find("Triangle"), std::string_view::npos);
  EXPECT_NE(modelPaths[1].find("Cube"), std::string_view::npos);
  EXPECT_EQ(modelPaths[2], std::string_view("__procedural_uv_sphere__"));
}

TEST(RenderingConventionTests,
     BimIdentityLookupSupportsDuplicateStringViewKeys) {
  container::renderer::BimObjectIndexLookup lookup;
  lookup["wall-guid"].push_back(4u);
  lookup["wall-guid"].push_back(9u);
  lookup["storey/source/path"].push_back(12u);

  const std::string_view guid = "wall-guid";
  const auto guidIt = lookup.find(guid);
  ASSERT_NE(guidIt, lookup.end());
  ASSERT_EQ(guidIt->second.size(), 2u);
  EXPECT_EQ(guidIt->second[0], 4u);
  EXPECT_EQ(guidIt->second[1], 9u);

  const auto literalGuidIt = lookup.find("wall-guid");
  ASSERT_NE(literalGuidIt, lookup.end());
  EXPECT_EQ(literalGuidIt->second.size(), 2u);

  const auto sourceIt = lookup.find(std::string_view("storey/source/path"));
  ASSERT_NE(sourceIt, lookup.end());
  ASSERT_EQ(sourceIt->second.size(), 1u);
  EXPECT_EQ(sourceIt->second[0], 12u);

  EXPECT_EQ(lookup.find(std::string_view("missing-guid")), lookup.end());
}

TEST(RenderingConventionTests, BimDrawFilterActivationTracksTypeAndSelection) {
  container::renderer::BimDrawFilter filter;

  EXPECT_FALSE(filter.active());

  filter.typeFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.type = "IfcWall";
  EXPECT_TRUE(filter.active());

  filter.typeFilterEnabled = false;
  filter.type.clear();
  EXPECT_FALSE(filter.active());

  filter.storeyFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.storey = "Level 01";
  EXPECT_TRUE(filter.active());

  filter.storeyFilterEnabled = false;
  filter.storey.clear();
  EXPECT_FALSE(filter.active());

  filter.materialFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.material = "Concrete";
  EXPECT_TRUE(filter.active());

  filter.materialFilterEnabled = false;
  filter.material.clear();
  EXPECT_FALSE(filter.active());

  filter.phaseFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.phase = "New construction";
  EXPECT_TRUE(filter.active());

  filter.phaseFilterEnabled = false;
  filter.phase.clear();
  EXPECT_FALSE(filter.active());

  filter.statusFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.status = "Existing";
  EXPECT_TRUE(filter.active());

  filter.statusFilterEnabled = false;
  filter.status.clear();
  EXPECT_FALSE(filter.active());

  filter.drawBudgetEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.drawBudgetMaxObjects = 1000u;
  EXPECT_TRUE(filter.active());

  filter.drawBudgetEnabled = false;
  filter.drawBudgetMaxObjects = 0u;
  EXPECT_FALSE(filter.active());

  filter.isolateSelection = true;
  EXPECT_FALSE(filter.active());

  filter.selectedObjectIndex = 7u;
  EXPECT_TRUE(filter.active());

  filter.isolateSelection = false;
  filter.hideSelection = true;
  EXPECT_TRUE(filter.active());

  filter.selectedObjectIndex = std::numeric_limits<uint32_t>::max();
  EXPECT_FALSE(filter.active());
}

TEST(RenderingConventionTests, BimStoreyMaterialAndHideFiltersArePlumbed) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string metadataCatalog =
      readRepoTextFile("src/renderer/bim/BimMetadataCatalog.cpp");
  const std::string bimDrawFilterState =
      readRepoTextFile("src/renderer/bim/BimDrawFilterState.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "storeyFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "materialFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "disciplineFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "phaseFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "statusFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(bimManagerHeader, "hideSelection"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStoreys()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementMaterials()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementDisciplines()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementPhases()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementFireRatings()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementLoadBearingValues()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStatuses()"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStoreys"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementMaterials"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementDisciplines"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementPhases"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementFireRatings"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementLoadBearingValues"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStatuses"));

  EXPECT_TRUE(contains(metadataCatalog, "bimMetadataStoreyLabel"));
  EXPECT_TRUE(contains(metadataCatalog, "bimMetadataMaterialLabel"));
  EXPECT_TRUE(contains(bimManager, "drawFilterState_->objectMatchesFilter"));
  EXPECT_TRUE(contains(bimManager, "drawFilterState_->filteredDrawLists"));
  EXPECT_TRUE(
      contains(bimDrawFilterState, "sameBimProductIdentity(*selectedMetadata"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.storeyFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.materialFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.disciplineFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.phaseFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.statusFilterEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.drawBudgetEnabled"));
  EXPECT_TRUE(contains(bimDrawFilterState, "filter.drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerStorey"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerMaterial"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerDiscipline"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerPhase"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerFireRating"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerLoadBearing"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerStatus"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(storeyIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(materialIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(disciplineIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(phaseIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(fireRatingIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(loadBearingIds_"));
  EXPECT_TRUE(contains(metadataCatalog, "registerUniqueLabel(statusIds_"));

  EXPECT_TRUE(contains(rendererFrontend, "filter.storeyFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.materialFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.disciplineFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.phaseFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.statusFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.drawBudgetEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.hideSelection"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStoreys"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementMaterials"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementDisciplines"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementPhases"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementFireRatings"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.elementLoadBearingValues"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStatuses"));

  EXPECT_TRUE(contains(guiManager, "Storey filter"));
  EXPECT_TRUE(contains(guiManager, "Material filter"));
  EXPECT_TRUE(contains(guiManager, "Discipline filter"));
  EXPECT_TRUE(contains(guiManager, "Phase filter"));
  EXPECT_TRUE(contains(guiManager, "Fire rating filter"));
  EXPECT_TRUE(contains(guiManager, "Load-bearing filter"));
  EXPECT_TRUE(contains(guiManager, "Status filter"));
  EXPECT_TRUE(contains(guiManager, "Draw budget enabled"));
  EXPECT_TRUE(contains(guiManager, "Hide selected"));
}

TEST(RenderingConventionTests, BimSemanticColorModesAreBimScoped) {
  const std::string semanticModeHeader =
      readRepoTextFile("include/Container/renderer/bim/BimSemanticColorMode.h");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string metadataCatalogHeader =
      readRepoTextFile("include/Container/renderer/bim/BimMetadataCatalog.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string metadataCatalog =
      readRepoTextFile("src/renderer/bim/BimMetadataCatalog.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string transparentOitRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredTransparentOitRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string surfaceRasterPassRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfaceRasterPassRecorder.cpp");
  const std::string surfacePassPlanner =
      readRepoTextFile("src/renderer/bim/BimSurfacePassPlanner.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(semanticModeHeader, "BimSemanticColorMode"));
  EXPECT_TRUE(contains(semanticModeHeader, "Type"));
  EXPECT_TRUE(contains(semanticModeHeader, "Storey"));
  EXPECT_TRUE(contains(semanticModeHeader, "Material"));
  EXPECT_TRUE(contains(semanticModeHeader, "FireRating"));
  EXPECT_TRUE(contains(semanticModeHeader, "LoadBearing"));
  EXPECT_TRUE(contains(semanticModeHeader, "Status"));
  EXPECT_TRUE(contains(semanticModeHeader, "Original materials"));

  EXPECT_TRUE(contains(bimManagerHeader, "setSemanticColorMode"));
  EXPECT_TRUE(contains(bimManagerHeader, "semanticColorMode()"));
  EXPECT_TRUE(contains(bimManagerHeader, "semanticColorIdsDirty_"));
  EXPECT_TRUE(contains(metadataCatalogHeader, "semanticIdForMetadata"));
  EXPECT_TRUE(contains(bimManager, "BimManager::setSemanticColorMode"));
  EXPECT_TRUE(contains(bimManager, "mode == semanticColorMode_"));
  EXPECT_TRUE(contains(bimManager, "!semanticColorIdsDirty_"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->semanticIdForMetadata"));
  EXPECT_TRUE(
      contains(metadataCatalog, "BimMetadataCatalog::semanticIdForMetadata"));
  EXPECT_TRUE(contains(bimManager, "object.objectInfo.w = semanticId"));
  EXPECT_TRUE(contains(metadataCatalog, "bimMetadataStoreyLabel(metadata)"));
  EXPECT_TRUE(contains(metadataCatalog, "bimMetadataMaterialLabel(metadata)"));

  EXPECT_TRUE(contains(guiManagerHeader, "bimSemanticColorMode()"));
  EXPECT_TRUE(contains(guiManager, "Semantic color"));
  EXPECT_TRUE(contains(guiManager, "kBimSemanticColorModes"));
  EXPECT_TRUE(contains(guiManager, "Semantic color legend"));
  EXPECT_TRUE(contains(guiManager, "BimSemanticLegendValues"));
  EXPECT_TRUE(contains(guiManager, "DrawSemanticLegendEntry"));

  EXPECT_TRUE(contains(rendererFrontend, "applyBimSemanticColorMode"));
  EXPECT_TRUE(contains(rendererFrontend, "bimSemanticColorMode()"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.semanticColorMode"));
  EXPECT_TRUE(contains(frameRecorderHeader, "semanticColorMode"));
  EXPECT_TRUE(
      contains(surfaceRasterPassRecorder,
               "planInputs.semanticColorMode = inputs.semanticColorMode"));
  EXPECT_TRUE(
      contains(surfaceRasterPassRecorder, "plan.writesSemanticColorMode"));
  EXPECT_TRUE(contains(surfaceRasterPassRecorder,
                       "base.semanticColorMode = plan.semanticColorMode"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "bimTransparentPc.semanticColorMode"));
  EXPECT_TRUE(contains(surfacePassPlanner, "plan.semanticColorMode"));
  EXPECT_TRUE(contains(surfacePassPlanner, "inputs_.semanticColorMode"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "transparentPc.semanticColorMode = 0u"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "BimSurfacePassKind::TransparentLighting"));
  EXPECT_TRUE(
      contains(transparentOitRecorder, "inputs.bimPlan->semanticColorMode"));

  EXPECT_TRUE(contains(sceneData, "semanticColorMode"));
  EXPECT_TRUE(contains(pushConstants, "uint semanticColorMode"));
  EXPECT_TRUE(contains(materialCommon, "EvaluateBimSemanticColor"));
  EXPECT_TRUE(contains(gbuffer, "pc.semanticColorMode"));
  EXPECT_TRUE(contains(gbuffer, "EvaluateBimSemanticColor"));
  EXPECT_TRUE(contains(transparent, "pc.semanticColorMode"));
  EXPECT_TRUE(contains(transparent, "EvaluateBimSemanticColor"));
}

TEST(RenderingConventionTests,
     BimInspectionExposesElementBoundsAndFloorElevation) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string metadataCatalog =
      readRepoTextFile("src/renderer/bim/BimMetadataCatalog.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimElementBounds"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementBoundsForObject"));
  EXPECT_TRUE(contains(bimManager, "BimManager::elementBoundsForObject"));
  EXPECT_TRUE(contains(bimManager, "includeBoundsPoint"));
  EXPECT_TRUE(contains(bimManager, "includeBoundsBox"));
  EXPECT_TRUE(contains(bimManager, "includeIndexedBounds"));
  EXPECT_TRUE(contains(bimManager, "objectIndicesForGuid(metadata->guid)"));
  EXPECT_TRUE(
      contains(bimManager, "objectIndicesForSourceId(metadata->sourceId)"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectDrawCommands_"));
  EXPECT_TRUE(contains(bimManager, "appendCommandsForObject"));
  EXPECT_TRUE(contains(guiManagerHeader, "hasSelectionBounds"));
  EXPECT_TRUE(contains(guiManagerHeader, "geometryKind"));
  EXPECT_TRUE(contains(rendererFrontend, "elementBoundsForObject"));
  EXPECT_TRUE(contains(rendererFrontend, "selectionFloorElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "bimGeometryKindLabel"));
  EXPECT_TRUE(contains(guiManager, "Floor elevation"));
  EXPECT_TRUE(contains(guiManager, "Bounds"));
}

TEST(RenderingConventionTests, BimCameraPivotUsesElementBoundsCenter) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const size_t syncStart =
      rendererFrontend.find("void RendererFrontend::syncCameraSelectionPivotOverride()");
  ASSERT_NE(syncStart, std::string::npos);
  const size_t syncEnd =
      rendererFrontend.find("void RendererFrontend::initializeScene", syncStart);
  ASSERT_NE(syncEnd, std::string::npos);
  const std::string syncBlock =
      rendererFrontend.substr(syncStart, syncEnd - syncStart);

  EXPECT_TRUE(contains(syncBlock, "if (anchorMatchesScene)"));
  EXPECT_FALSE(contains(syncBlock, "anchorMatchesScene || anchorMatchesBim"));

  const size_t bimAnchor = syncBlock.find("anchorMatchesBim");
  const size_t bimGuard = syncBlock.find("if (selectedBimObjectIndex_");
  const size_t boundsLookup =
      syncBlock.find("elementBoundsForObject(selectedBimObjectIndex_)");
  ASSERT_NE(bimAnchor, std::string::npos);
  ASSERT_NE(bimGuard, std::string::npos);
  ASSERT_NE(boundsLookup, std::string::npos);
  EXPECT_LT(bimAnchor, boundsLookup);
  EXPECT_LT(bimGuard, boundsLookup);
  EXPECT_EQ(syncBlock.substr(bimGuard, boundsLookup - bimGuard)
                .find("selectionNavigationAnchor_.point"),
            std::string::npos);

  const size_t boundsPivot =
      syncBlock.find(".center = bounds.center", boundsLookup);
  ASSERT_NE(boundsPivot, std::string::npos);
}

TEST(RenderingConventionTests, SceneHierarchyPanelExposesNodeActions) {
  const std::string sceneGraphHeader =
      readRepoTextFile("include/Container/utility/SceneGraph.h");
  const std::string sceneGraph =
      readRepoTextFile("src/utility/SceneGraph.cpp");
  const std::string world = readRepoTextFile("src/ecs/World.cpp");
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(sceneGraphHeader, "rootNodes()"));
  EXPECT_TRUE(contains(sceneGraphHeader, "visible = true"));
  EXPECT_TRUE(contains(sceneGraphHeader, "setParentPreserveWorldTransform"));
  EXPECT_TRUE(contains(sceneGraph, "isNodeEffectivelyVisible"));
  EXPECT_TRUE(contains(world, "graph.isNodeEffectivelyVisible(nodeIndex)"));

  EXPECT_TRUE(contains(guiHeader, "focusSceneNode"));
  EXPECT_TRUE(contains(guiHeader, "setSceneNodeVisible"));
  EXPECT_TRUE(contains(guiHeader, "reparentSceneNode"));
  EXPECT_TRUE(contains(gui, "Scene Hierarchy"));
  EXPECT_TRUE(contains(gui, "DrawSceneHierarchyPanel"));
  EXPECT_TRUE(contains(gui, "DrawSceneHierarchyParentCombo"));
  EXPECT_TRUE(contains(gui, "SceneHierarchyDescendants"));
  EXPECT_TRUE(contains(gui, "SceneHierarchyVisitedNodes"));
  EXPECT_TRUE(contains(gui, "SceneHierarchyParentCandidateCache"));
  EXPECT_TRUE(contains(gui, "ImGuiListClipper"));
  EXPECT_TRUE(contains(gui, "SceneHierarchyParentChange"));
  EXPECT_TRUE(contains(gui, "Focus"));
  EXPECT_TRUE(contains(gui, "Visible"));
  EXPECT_TRUE(contains(gui, "Parent"));
  EXPECT_TRUE(contains(frontend, "frameNodeOrScene(nodeIndex)"));
  EXPECT_TRUE(contains(frontend, "setVisible(nodeIndex, visible)"));
  EXPECT_TRUE(contains(frontend, "setParentPreserveWorldTransform"));
  EXPECT_TRUE(contains(frontend, "Failed to reparent scene node"));
  const size_t restoreSnapshot =
      frontend.find("return restoreViewpointSnapshot(snapshot);");
  ASSERT_NE(restoreSnapshot, std::string::npos);
  const size_t hierarchyStart =
      frontend.find("[this](uint32_t nodeIndex) {", restoreSnapshot);
  ASSERT_NE(hierarchyStart, std::string::npos);
  const size_t hierarchyEnd = frontend.find(
      "subs_.cameraController ? subs_.cameraController->nodeTransformControls",
      hierarchyStart);
  ASSERT_NE(hierarchyEnd, std::string::npos);
  const std::string hierarchyBlock =
      frontend.substr(hierarchyStart, hierarchyEnd - hierarchyStart);
  EXPECT_TRUE(contains(hierarchyBlock, "selectEditableLight({})"));
  EXPECT_TRUE(contains(hierarchyBlock, "setSectionPlaneVisualEditable(false)"));
  EXPECT_TRUE(contains(hierarchyBlock,
                       "selectedBimNativePointDrawCommands_.clear()"));
  EXPECT_TRUE(contains(hierarchyBlock,
                       "selectedBimNativeCurveDrawCommands_.clear()"));
}

TEST(RenderingConventionTests, BimMeasurementToolsUseSelectionBoundsInGui) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimMeasurementPointState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimMeasurementAnnotationState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementPointA_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementPointB_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementAnnotations_"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "bimMeasurementEffectiveImportScale_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementObjectCount_"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementDimensions"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementFootprintArea"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementVolume"));
  EXPECT_TRUE(contains(guiManager, "ComputeBimMeasurement"));
  EXPECT_TRUE(contains(guiManager, "measurementSourceChanged"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.effectiveImportScale"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.objectCount"));
  EXPECT_TRUE(contains(guiManager, "selectionBoundsSize"));
  EXPECT_TRUE(contains(guiManager, "selectionBoundsCenter"));
  EXPECT_TRUE(contains(guiManager, "Measurements"));
  EXPECT_TRUE(contains(guiManager, "Dimensions (X/Y/Z)"));
  EXPECT_TRUE(contains(guiManager, "Footprint (X*Z)"));
  EXPECT_TRUE(contains(guiManager, "Volume (bounds)"));
  EXPECT_TRUE(contains(guiManager, "CaptureBimMeasurementPointFromSelection"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementSnapMode"));
  EXPECT_TRUE(contains(guiManager, "Set A from selection"));
  EXPECT_TRUE(contains(guiManager, "Set B from selection"));
  EXPECT_TRUE(contains(guiManager, "Measurement points"));
  EXPECT_TRUE(contains(guiManager, "Distance: %.3f"));
  EXPECT_TRUE(contains(guiManager, "Horizontal distance"));
  EXPECT_TRUE(contains(guiManager, "Elevation delta"));
  EXPECT_TRUE(contains(guiManager, "Slope angle"));
  EXPECT_TRUE(contains(guiManager, "elevationAxisAngleDegrees"));
  EXPECT_TRUE(contains(guiManager, "Angle to elevation axis"));
  EXPECT_TRUE(contains(guiManager, "Live floor elevation"));
  EXPECT_TRUE(contains(guiManager, "Save measurement annotation"));
  EXPECT_TRUE(contains(guiManager, "Measurement annotations"));
  EXPECT_TRUE(contains(guiManager, "Clear annotations"));
}

TEST(RenderingConventionTests, BimViewpointsExposeBcfImportExportInGui) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string bcfHeader =
      readRepoTextFile("include/Container/utility/BcfViewpoint.h");
  const std::string bcfSource =
      readRepoTextFile("src/utility/BcfViewpoint.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "bcfViewpointPath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BcfTopicArchiveEntryState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicFolderPath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicArchivePath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicArchiveEntries_"));
  EXPECT_TRUE(contains(guiManager, "Container/utility/BcfViewpoint.h"));
  EXPECT_TRUE(contains(guiManager, "BCF viewpoint file"));
  EXPECT_TRUE(contains(guiManager, "Export current BCF"));
  EXPECT_TRUE(contains(guiManager, "Import BCF"));
  EXPECT_TRUE(contains(guiManager, "Export selected BCF"));
  EXPECT_TRUE(contains(guiManager, "BCF Topics"));
  EXPECT_TRUE(contains(guiManager, "BCF topic folder"));
  EXPECT_TRUE(contains(guiManager, "BCF topic archive file"));
  EXPECT_TRUE(contains(guiManager, "Topic title"));
  EXPECT_TRUE(contains(guiManager, "Archive current topic"));
  EXPECT_TRUE(contains(guiManager, "Save topic folder"));
  EXPECT_TRUE(contains(guiManager, "Load topic folder"));
  EXPECT_TRUE(contains(guiManager, "Save topic archive"));
  EXPECT_TRUE(contains(guiManager, "Load topic archive"));
  EXPECT_TRUE(contains(guiManager, "Restore archived topic viewpoint"));
  EXPECT_TRUE(contains(guiManager, "issue server sync is not connected"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveVisualizationInfo"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadVisualizationInfo"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveTopicFolder"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadTopicFolder"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveBcfArchive"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadBcfArchive"));
  EXPECT_TRUE(contains(bcfHeader, "ExportVisualizationInfo"));
  EXPECT_TRUE(contains(bcfHeader, "ImportVisualizationInfo"));
  EXPECT_TRUE(contains(bcfHeader, "SaveTopicFolder"));
  EXPECT_TRUE(contains(bcfHeader, "LoadTopicFolder"));
  EXPECT_TRUE(contains(bcfHeader, "SaveBcfArchive"));
  EXPECT_TRUE(contains(bcfHeader, "LoadBcfArchive"));
  EXPECT_TRUE(contains(bcfSource, "readStandardPerspectiveCamera"));
  EXPECT_TRUE(contains(bcfSource, "CameraViewPoint"));
  EXPECT_TRUE(contains(bcfSource, "CameraDirection"));
  EXPECT_TRUE(contains(bcfSource, "IfcGuid"));
}

TEST(RenderingConventionTests, BimFloorPlanSkipsPointCurvePlaceholders) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");
  const std::string ifcxLoader =
      readRepoTextFile("src/geometry/IfcxLoader.cpp");

  EXPECT_TRUE(contains(dotBimHeader, "enum class GeometryKind"));
  EXPECT_TRUE(contains(dotBimHeader, "GeometryKind geometryKind"));
  EXPECT_TRUE(contains(bimManager, "int64_t y{0}"));
  EXPECT_TRUE(contains(bimManager, "bool preserveElevation"));
  EXPECT_TRUE(contains(bimManager, "preserveElevation ?"));
  EXPECT_TRUE(contains(bimManager, "computeElementBaseElevation"));
  EXPECT_TRUE(contains(bimManager, "storeyBaseElevations"));
  EXPECT_TRUE(contains(bimManager, "floorPlanGround_"));
  EXPECT_TRUE(contains(bimManager, "floorPlanSourceElevation_"));
  EXPECT_TRUE(contains(bimManager, "element.geometryKind !="));
  EXPECT_TRUE(contains(bimManager, "bimGeometryKind(element.geometryKind)"));
  EXPECT_TRUE(contains(usdLoader, "dotbim::GeometryKind::Points"));
  EXPECT_TRUE(contains(usdLoader, "dotbim::GeometryKind::Curves"));
  EXPECT_TRUE(contains(usdLoader, "appendTinyUsdCurvePrim"));
  EXPECT_TRUE(contains(usdLoader, "appendTinyUsdPointsFromStage"));
  EXPECT_TRUE(contains(usdLoader, "tinyUsdPrimNeedsPlaceholderGeometry"));
  EXPECT_TRUE(contains(ifcxLoader, "dotbim::GeometryKind::Points"));
  EXPECT_TRUE(contains(ifcxLoader, "dotbim::GeometryKind::Curves"));
}

TEST(RenderingConventionTests, BimStoreyRangesFeedFloorSliceUi) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string metadataCatalog =
      readRepoTextFile("src/renderer/bim/BimMetadataCatalog.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimStoreyRange"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStoreyRanges()"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStoreyRanges"));
  EXPECT_TRUE(contains(guiManagerHeader, "selectedBimStoreyRangeIndex_"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerStorey"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->sortStoreyRanges"));
  EXPECT_TRUE(contains(metadataCatalog, "storeyRanges_.push_back"));
  EXPECT_TRUE(contains(metadataCatalog, "minElevation"));
  EXPECT_TRUE(contains(metadataCatalog, "maxElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "elementStoreyRanges()"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStoreyRanges"));
  EXPECT_TRUE(contains(guiManager, "Floor slice"));
  EXPECT_TRUE(contains(guiManager, "Spatial Browser"));
  EXPECT_TRUE(contains(guiManager, "Plan storey"));
  EXPECT_TRUE(contains(guiManager, "Previous storey"));
  EXPECT_TRUE(contains(guiManager, "Next storey"));
  EXPECT_TRUE(contains(guiManager, "selectRelativePlanStorey"));
  EXPECT_TRUE(contains(guiManager, "Plan navigation storey"));
  EXPECT_TRUE(contains(guiManager, "Show storey only"));
  EXPECT_TRUE(contains(guiManager, "Plan slice preset"));
  EXPECT_TRUE(contains(guiManager, "Clear storey preset"));
  EXPECT_TRUE(contains(guiManager, "applyStoreyPlanSlice"));
  EXPECT_TRUE(contains(guiManager, "BimSpatialStoreys"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_.normal = {0.0f, -1.0f"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_.offset"));
  EXPECT_TRUE(
      contains(guiManager, "bimFilterState_.storeyFilterEnabled = true"));
  EXPECT_TRUE(contains(guiManager, "floorSliceUsesStoreyFilter"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_ = {}"));
}

TEST(RenderingConventionTests, BimUiGapControlsExposeGracefulPlaceholders) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimLayerVisibilityState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLayerVisibilityState()"));
  EXPECT_TRUE(contains(guiManagerHeader, "pointCloudVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "curvesVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "clashLayerVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "markupLayerVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimQuickFilterSearch_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimPropertySearch_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimSelectionSetMemberState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimSelectionSetState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimSelectionSets_"));
  EXPECT_TRUE(contains(guiManagerHeader, "selectedBimSelectionSetIndex_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimLodStreamingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLodStreamingUiState_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimClipCapHatchingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimClipCapHatchingUiState_"));
  EXPECT_TRUE(contains(guiManagerHeader, "scheduleByClassAndStoreyRows"));
  EXPECT_TRUE(contains(guiManagerHeader, "modelCompareElements"));
  EXPECT_TRUE(contains(guiManagerHeader, "selectedCoordinateReadout"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimIssueOverlayPins()"));
  EXPECT_TRUE(contains(guiManagerHeader, "screenErrorPixels"));
  EXPECT_TRUE(contains(guiManagerHeader, "hatchSpacing"));
  EXPECT_TRUE(contains(guiManagerHeader, "hatchAngleDegrees"));

  EXPECT_TRUE(contains(guiManager, "Property Browser"));
  EXPECT_TRUE(contains(guiManager, "BimPropertyBrowser"));
  EXPECT_TRUE(contains(guiManager, "Property search"));
  EXPECT_TRUE(contains(guiManager, "Clear property search"));
  EXPECT_TRUE(contains(guiManager, "PropertyMatchesSearch"));
  EXPECT_TRUE(contains(guiManager, "Extended properties"));
  EXPECT_TRUE(contains(guiManager, "BimExtendedProperties"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.properties"));
  EXPECT_TRUE(contains(guiManager, "Georeference and Units"));
  EXPECT_TRUE(contains(guiManager, "BimGeoreferenceAndUnits"));
  EXPECT_TRUE(contains(guiManager, "Selected coordinate readout"));
  EXPECT_TRUE(contains(guiManager, "Origin rebase recommended"));
  EXPECT_TRUE(contains(guiManager, "Source up axis"));
  EXPECT_TRUE(contains(guiManager, "Coordinate offset"));
  EXPECT_TRUE(contains(guiManager, "CRS:"));
  EXPECT_TRUE(contains(guiManager, "No CRS metadata exposed"));
  EXPECT_TRUE(contains(guiManager, "Schedules and Quantities"));
  EXPECT_TRUE(contains(guiManager, "BimScheduleClassStoreyRows"));
  EXPECT_TRUE(contains(guiManager, "Material totals"));
  EXPECT_TRUE(contains(guiManager, "Model Compare"));
  EXPECT_TRUE(contains(guiManager, "Capture compare baseline"));
  EXPECT_TRUE(contains(guiManager, "compareBimModels"));
  EXPECT_TRUE(contains(guiManager, "Search and Filters"));
  EXPECT_TRUE(contains(guiManager, "Search filter values"));
  EXPECT_TRUE(contains(guiManager, "Clear all BIM filters"));
  EXPECT_TRUE(contains(guiManager, "BimQuickFilterMatches"));
  EXPECT_TRUE(contains(guiManager, "Use selected type"));
  EXPECT_TRUE(contains(guiManager, "Use selected storey"));
  EXPECT_TRUE(contains(guiManager, "Use selected material"));
  EXPECT_TRUE(contains(guiManager, "Selection Sets"));
  EXPECT_TRUE(contains(guiManager, "Create selection set"));
  EXPECT_TRUE(contains(guiManager, "Create set from selection"));
  EXPECT_TRUE(contains(guiManager, "Active selection set"));
  EXPECT_TRUE(contains(guiManager, "Add selection to set"));
  EXPECT_TRUE(contains(guiManager, "BimSelectionSetMembers"));
  EXPECT_TRUE(contains(guiManager, "Restored BIM selection set member"));
  EXPECT_TRUE(contains(guiManager, "multi-select rendering is not connected"));
  EXPECT_TRUE(contains(guiManager, "IFC Relationships"));
  EXPECT_TRUE(contains(guiManager, "BimIfcRelationshipBrowser"));
  EXPECT_TRUE(contains(guiManager, "Spatial containment"));
  EXPECT_TRUE(contains(guiManager, "Material assignment"));
  EXPECT_TRUE(contains(guiManager, "Type/classification"));
  EXPECT_TRUE(contains(guiManager, "Filter related storey"));
  EXPECT_TRUE(contains(guiManager, "Filter related material"));
  EXPECT_TRUE(contains(guiManager, "Filter related type"));
  EXPECT_TRUE(contains(guiManager, "BimIfcPropertySetSummary"));
  EXPECT_TRUE(contains(guiManager, "BimRelationshipSearchResults"));
  EXPECT_TRUE(contains(guiManager, "BimSelectedRelationshipEdges"));
  EXPECT_TRUE(contains(
      guiManager, "Select an element to browse inferred IFC relationships"));
  EXPECT_TRUE(contains(guiManager, "No spatial hierarchy exposed by renderer"));
  EXPECT_TRUE(contains(guiManager, "Storey elevation ranges are not exposed"));
  EXPECT_TRUE(contains(guiManager, "Layer Visibility"));
  EXPECT_TRUE(contains(guiManager, "Point-cloud visibility"));
  EXPECT_TRUE(contains(guiManager, "Curve visibility"));
  EXPECT_FALSE(contains(guiManager, "Space volumes"));
  EXPECT_FALSE(contains(guiManager, "X-ray layer"));
  EXPECT_TRUE(contains(guiManager, "Clash markers"));
  EXPECT_TRUE(contains(guiManager, "Issue markers"));
  EXPECT_TRUE(contains(guiManager, "Active issue pins"));
  EXPECT_TRUE(contains(guiManager, "bcfIssueOverlayPins_"));
  EXPECT_TRUE(contains(guiManager, "Point-cloud and curve toggles mask"));
  EXPECT_TRUE(contains(guiManager, "LOD / Streaming"));
  EXPECT_TRUE(contains(guiManager, "Loaded BIM objects"));
  EXPECT_TRUE(contains(guiManager, "Auto LOD request"));
  EXPECT_TRUE(contains(guiManager, "Draw budget enabled"));
  EXPECT_TRUE(contains(guiManager, "Max visible BIM objects"));
  EXPECT_TRUE(contains(guiManager, "LOD bias"));
  EXPECT_TRUE(contains(guiManager, "Screen error pixels"));
  EXPECT_TRUE(contains(guiManager, "Pause streaming request"));
  EXPECT_TRUE(contains(guiManager, "Keep visible storeys resident"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cacheable"));
  EXPECT_TRUE(contains(guiManager, "Budget visible"));
  EXPECT_TRUE(contains(guiManager, "meshlet residency buffers are GPU-side"));
  EXPECT_TRUE(contains(guiManager, "Clip / Cap / Hatching"));
  EXPECT_TRUE(contains(guiManager, "Section cap fill"));
  EXPECT_TRUE(contains(guiManager, "Section hatch overlay"));
  EXPECT_TRUE(contains(guiManager, "Cap fill opacity"));
  EXPECT_TRUE(contains(guiManager, "Hatch line width"));
  EXPECT_TRUE(contains(guiManager, "Hatch spacing"));
  EXPECT_TRUE(contains(guiManager, "Hatch angle degrees"));
  EXPECT_TRUE(
      contains(guiManager, "Section caps rebuild when the section plane"));
}

TEST(RenderingConventionTests,
     HiddenBimSelectionDoesNotPopulateSelectedOverlayDrawCommands) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  const size_t blockStart = rendererFrontend.find(
      "p.bim.draws.hoveredDrawCommands = &hoveredBimDrawCommands_");
  ASSERT_NE(blockStart, std::string::npos);
  const size_t selectedCollection = rendererFrontend.find(
      "subs_.bimManager->collectDrawCommandsForObject(selectedBimObjectIndex_",
      blockStart);
  ASSERT_NE(selectedCollection, std::string::npos);
  const size_t blockEnd =
      rendererFrontend.find("p.bim.floorPlanDrawCommands", selectedCollection);
  ASSERT_NE(blockEnd, std::string::npos);

  const std::string selectedOverlayBlock =
      rendererFrontend.substr(blockStart, blockEnd - blockStart);
  EXPECT_TRUE(contains(selectedOverlayBlock, "bimFilter.hideSelection"));
  EXPECT_TRUE(
      contains(selectedOverlayBlock, "selectedBimDrawCommands_.clear()"));
  EXPECT_TRUE(
      contains(selectedOverlayBlock, "p.bim.draws.selectedDrawCommands"));
}

TEST(RenderingConventionTests, BimProductIdentityMatchesGuidOrSourceId) {
  container::renderer::BimElementMetadata selected;
  selected.objectIndex = 7u;
  selected.guid = "wall-guid";
  selected.sourceId = "/Project/Site/Wall";

  container::renderer::BimElementMetadata sameObject;
  sameObject.objectIndex = 7u;
  EXPECT_TRUE(
      container::renderer::sameBimProductIdentity(selected, sameObject));

  container::renderer::BimElementMetadata sameGuid;
  sameGuid.objectIndex = 12u;
  sameGuid.guid = "wall-guid";
  EXPECT_TRUE(container::renderer::sameBimProductIdentity(selected, sameGuid));

  container::renderer::BimElementMetadata sameSource;
  sameSource.objectIndex = 13u;
  sameSource.sourceId = "/Project/Site/Wall";
  EXPECT_TRUE(
      container::renderer::sameBimProductIdentity(selected, sameSource));

  container::renderer::BimElementMetadata conflictingGuidSameSource;
  conflictingGuidSameSource.objectIndex = 14u;
  conflictingGuidSameSource.guid = "door-guid";
  conflictingGuidSameSource.sourceId = "/Project/Site/Wall";
  EXPECT_FALSE(container::renderer::sameBimProductIdentity(
      selected, conflictingGuidSameSource));

  container::renderer::BimElementMetadata differentProduct;
  differentProduct.objectIndex = 21u;
  differentProduct.guid = "door-guid";
  differentProduct.sourceId = "/Project/Site/Door";
  EXPECT_FALSE(
      container::renderer::sameBimProductIdentity(selected, differentProduct));

  container::renderer::BimElementMetadata emptyIdentity;
  emptyIdentity.objectIndex = 22u;
  EXPECT_FALSE(
      container::renderer::sameBimProductIdentity(selected, emptyIdentity));
}

TEST(RenderingConventionTests, BimPointAndCurveDrawListsAreRendererScoped) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredRasterFrameState =
      readRepoTextFile("src/renderer/deferred/DeferredRasterFrameState.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimGeometryDrawLists"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePointDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurveDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointObjectCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveObjectCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePointOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurveOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletSourceClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletEstimatedClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawLists"));
  EXPECT_TRUE(contains(bimManagerHeader, "points{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "curves{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePoints{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurves{}"));

  EXPECT_TRUE(contains(bimManager, "BimGeometryKind::Points"));
  EXPECT_TRUE(contains(bimManager, "BimGeometryKind::Curves"));
  EXPECT_TRUE(contains(bimManager, "appendDrawCommandToGeometryLists"));
  EXPECT_TRUE(contains(bimManager, "pointDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "curveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "nativePointDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "nativeCurveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterCountForModel"));
  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadata_"));
  EXPECT_TRUE(contains(bimManager, "testGeometryDrawLists(pointDrawLists_)"));
  EXPECT_TRUE(contains(bimManager, "testGeometryDrawLists(curveDrawLists_)"));

  EXPECT_TRUE(contains(frameRecorderHeader, "pointDraws"));
  EXPECT_TRUE(contains(frameRecorderHeader, "curveDraws"));
  EXPECT_TRUE(contains(deferredRasterFrameState, "bimSurfaceDrawListSet"));
  EXPECT_FALSE(contains(frameRecorder, "bim.pointDraws"));
  EXPECT_FALSE(contains(frameRecorder, "bim.curveDraws"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "bim.pointDraws"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "bim.curveDraws"));
  EXPECT_TRUE(contains(deferredRasterFrameState, "hasBimOpaqueDrawCommands"));
  EXPECT_TRUE(
      contains(deferredRasterFrameState, "for (const FrameDrawLists* draws"));

  EXPECT_TRUE(contains(rendererFrontend, "pointDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "curveDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "nativePointDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "nativeCurveDrawLists()"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.pointPrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.curvePrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.pointDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.curveDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.nativeCurveDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "bimLayerVisibilityState()"));
  EXPECT_TRUE(contains(rendererFrontend, "bimObjectVisibleByLayer"));
  EXPECT_TRUE(contains(rendererFrontend, "pointCloudVisible"));
  EXPECT_TRUE(contains(rendererFrontend, "curvesVisible"));
}

TEST(RenderingConventionTests, BimDrawCompactionPreparationUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string compactionPlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimDrawCompactionPlanner.h");
  const std::string compactionPlanner =
      readRepoTextFile("src/renderer/bim/BimDrawCompactionPlanner.cpp");
  const std::string gpuVisibilityHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimFrameGpuVisibilityRecorder.h");
  const std::string gpuVisibilityRecorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t recordStart = deferredFrameGraphContext.find(
      "void DeferredRasterFrameGraphContext::afterPrepareFrame");
  ASSERT_NE(recordStart, std::string::npos);
  const size_t recordEnd = deferredFrameGraphContext.find(
      "void DeferredRasterFrameGraphContext::afterCommandBufferBegin",
      recordStart);
  ASSERT_NE(recordEnd, std::string::npos);
  const std::string recordBlock =
      deferredFrameGraphContext.substr(recordStart, recordEnd - recordStart);

  EXPECT_FALSE(contains(frameRecorder, "prepareBimFrameGpuVisibility"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimFrameGpuVisibilityCommands"));
  EXPECT_TRUE(contains(recordBlock, "prepareBimFrameGpuVisibility"));
  EXPECT_FALSE(contains(recordBlock, "makeBimDrawCompactionPlanInputs"));
  EXPECT_FALSE(contains(recordBlock, "buildBimDrawCompactionPlan"));
  EXPECT_FALSE(contains(recordBlock, "prepareDrawCompaction"));
  EXPECT_FALSE(contains(recordBlock, "recordMeshletResidencyUpdate"));
  EXPECT_FALSE(contains(recordBlock, "recordVisibilityFilterUpdate"));
  EXPECT_FALSE(contains(recordBlock, "recordDrawCompactionUpdate"));
  EXPECT_FALSE(contains(recordBlock, "BimDrawCompactionSlot::"));

  EXPECT_TRUE(contains(compactionPlannerHeader, "BimDrawCompactionPlanner"));
  EXPECT_TRUE(contains(compactionPlannerHeader, "BimDrawCompactionPlanInputs"));
  EXPECT_TRUE(contains(compactionPlanner, "transparentSingleSidedSource"));
  EXPECT_TRUE(contains(compactionPlanner,
                       "BimDrawCompactionSlot::TransparentSingleSided"));
  EXPECT_TRUE(
      contains(compactionPlanner, "BimDrawCompactionSlot::NativePointOpaque"));
  EXPECT_TRUE(contains(compactionPlanner,
                       "BimDrawCompactionSlot::NativeCurveTransparent"));
  EXPECT_TRUE(contains(gpuVisibilityHeader, "prepareBimFrameGpuVisibility"));
  EXPECT_TRUE(
      contains(gpuVisibilityHeader, "recordBimFrameGpuVisibilityCommands"));
  EXPECT_TRUE(
      contains(gpuVisibilityRecorder, "makeBimDrawCompactionPlanInputs"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "buildBimDrawCompactionPlan"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordMeshletResidencyUpdate"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordVisibilityFilterUpdate"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordDrawCompactionUpdate"));
  EXPECT_FALSE(contains(gpuVisibilityRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(gpuVisibilityRecorder, "GuiManager"));
  EXPECT_FALSE(contains(gpuVisibilityRecorder, "RenderGraph"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimDrawCompactionPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimFrameGpuVisibilityRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_frame_gpu_visibility_recorder_tests"));
}

TEST(RenderingConventionTests, BimSurfaceDrawRoutingUsesPlanner) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string deferredBimSurfacePassRecorderHeader =
      readRepoTextFile("include/Container/renderer/deferred/"
                       "DeferredRasterBimSurfacePassRecorder.h");
  const std::string deferredBimSurfacePassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterBimSurfacePassRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string routingPlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h");
  const std::string routingPlanner =
      readRepoTextFile("src/renderer/bim/BimSurfaceDrawRoutingPlanner.cpp");
  const std::string passPlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfacePassPlanner.h");
  const std::string passPlanner =
      readRepoTextFile("src/renderer/bim/BimSurfacePassPlanner.cpp");
  const std::string passRecorderHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfacePassRecorder.h");
  const std::string passRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfacePassRecorder.cpp");
  const std::string rasterPassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfaceRasterPassRecorder.h");
  const std::string rasterPassRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfaceRasterPassRecorder.cpp");
  const std::string transparentPickRasterRecorder = readRepoTextFile(
      "src/renderer/picking/TransparentPickRasterPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t deferredBimStart = deferredBimSurfacePassRecorder.find(
      "bool recordDeferredRasterBimSurfacePassCommands");
  ASSERT_NE(deferredBimStart, std::string::npos);
  const size_t pickStart = transparentPickRasterRecorder.find(
      "bool recordTransparentPickFramePassCommands");
  ASSERT_NE(pickStart, std::string::npos);
  const size_t lightingStart = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingStart, std::string::npos);
  const std::string deferredBimBlock =
      deferredBimSurfacePassRecorder.substr(deferredBimStart);
  const std::string pickBlock = transparentPickRasterRecorder.substr(pickStart);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingStart);

  EXPECT_FALSE(contains(frameRecorder, "BimSurfacePassPlanner.h"));
  EXPECT_FALSE(contains(frameRecorder, "BimSurfaceRasterPassRecorder.h"));
  EXPECT_FALSE(
      contains(frameRecorder, "DeferredRasterBimSurfacePassRecorder.h"));
  EXPECT_FALSE(contains(frameRecorderHeader,
                        "DeferredRasterBimSurfacePassRecordInputs"));
  EXPECT_FALSE(
      contains(frameRecorder, "recordDeferredRasterBimSurfacePassCommands"));
  EXPECT_FALSE(contains(frameRecorder, "bimSurfaceFramePassDrawSources"));
  EXPECT_FALSE(contains(frameRecorder, "bimSurfacePassGeometryBinding"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimSurfaceFramePassCommands"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimSurfaceRasterPassCommands"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimSurfacePassCommands"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordBimDepthPrepass"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordBimGBufferPass"));
  EXPECT_FALSE(contains(frameRecorder, "bindSceneGeometryBuffers"));
  EXPECT_TRUE(contains(deferredBimSurfacePassRecorderHeader,
                       "DeferredRasterBimSurfacePassRecordInputs"));
  EXPECT_TRUE(contains(deferredBimSurfacePassRecorder,
                       "isSupportedBimSurfaceRasterPass"));
  EXPECT_TRUE(contains(deferredBimBlock, "recordBimSurfaceFramePassCommands"));
  EXPECT_TRUE(contains(deferredBimSurfacePassRecorder,
                       "BimSurfacePassKind::DepthPrepass"));
  EXPECT_TRUE(
      contains(deferredBimSurfacePassRecorder, "BimSurfacePassKind::GBuffer"));
  EXPECT_FALSE(
      contains(deferredBimSurfacePassRecorderHeader, "FrameRecordParams"));
  EXPECT_FALSE(
      contains(deferredBimSurfacePassRecorderHeader, "FrameRecorder.h"));
  EXPECT_FALSE(contains(deferredBimSurfacePassRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(deferredBimSurfacePassRecorder, "FrameRecorder.h"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterBimSurfacePassInputs"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "deferredRasterBimSurfaceFrameBinding"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "BimSurfacePassKind::DepthPrepass"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "BimSurfacePassKind::GBuffer"));
  EXPECT_FALSE(contains(deferredBimBlock, "buildBimSurfacePassPlan"));
  EXPECT_FALSE(contains(pickBlock, "buildBimSurfacePassPlan"));
  EXPECT_FALSE(contains(lightingBlock, "buildBimSurfacePassPlan"));
  EXPECT_TRUE(contains(pickBlock, "buildBimSurfaceFramePassPlan"));
  EXPECT_TRUE(contains(lightingBlock, "buildBimSurfaceFramePassPlan"));
  EXPECT_FALSE(contains(deferredBimBlock, "RenderingBeginInfo"));
  EXPECT_FALSE(contains(deferredBimBlock, "beginDynamicRendering"));
  EXPECT_FALSE(contains(deferredBimBlock, "endDynamicRendering"));
  EXPECT_FALSE(contains(deferredBimBlock, "recordSceneViewportAndScissor"));
  EXPECT_FALSE(contains(deferredBimBlock, "recordBimSurfacePassCommands"));
  EXPECT_FALSE(contains(deferredBimBlock, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(deferredBimBlock, "vkCmdBindVertexBuffers"));
  EXPECT_FALSE(contains(deferredBimBlock, "vkCmdBindIndexBuffer"));
  EXPECT_FALSE(contains(deferredBimBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(
      deferredBimBlock,
      "for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim))"));
  EXPECT_FALSE(contains(
      pickBlock,
      "for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim))"));
  EXPECT_FALSE(contains(
      lightingBlock,
      "for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim))"));
  EXPECT_FALSE(contains(pickBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_FALSE(contains(lightingBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(passRecorder, "pipelineForBimSurfaceRoute"));
  EXPECT_FALSE(contains(frameRecorder, "surfaceDrawLists"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "deferredRasterSurfaceDrawLists"));
  EXPECT_TRUE(contains(passRecorder, "route.gpuCompactionAllowed"));
  EXPECT_TRUE(contains(passRecorder, "route.cpuFallbackAllowed"));
  EXPECT_TRUE(contains(passRecorder, "drawCompactionReady"));
  EXPECT_TRUE(contains(passRecorder, "drawCompacted"));
  EXPECT_TRUE(contains(passRecorder, "debugOverlay->drawScene"));
  EXPECT_TRUE(contains(passRecorderHeader, "BimSurfacePassRecordInputs"));
  EXPECT_TRUE(contains(passRecorderHeader, "BimSurfacePassGeometryBinding"));
  EXPECT_TRUE(contains(passRecorder, "bindBimSurfaceGeometry"));
  EXPECT_TRUE(contains(passRecorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(passRecorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(passRecorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(
      contains(rasterPassRecorderHeader, "BimSurfaceRasterPassRecordInputs"));
  EXPECT_TRUE(
      contains(rasterPassRecorder, "recordBimSurfaceRasterPassCommands"));
  EXPECT_TRUE(contains(rasterPassRecorder, "beginDynamicRendering"));
  EXPECT_TRUE(contains(rasterPassRecorder, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(rasterPassRecorder, "recordBimSurfacePassCommands"));
  EXPECT_TRUE(contains(rasterPassRecorder, "endDynamicRendering"));
  EXPECT_TRUE(
      contains(rasterPassRecorder, "bimSurfaceRasterPassPushConstants"));
  EXPECT_FALSE(contains(rasterPassRecorderHeader, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterPassRecorderHeader, "FrameRecorder.h"));
  EXPECT_FALSE(contains(rasterPassRecorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(rasterPassRecorder, "FrameRecorder.h"));
  EXPECT_FALSE(contains(rasterPassRecorder, "GuiManager"));
  EXPECT_FALSE(contains(rasterPassRecorder, "LightingManager"));
  EXPECT_FALSE(contains(rasterPassRecorder, "SceneController"));
  EXPECT_TRUE(contains(rasterPassRecorder, "buildBimSurfaceFramePassInputs"));
  EXPECT_TRUE(contains(rasterPassRecorder, "buildBimSurfacePassPlan"));
  EXPECT_TRUE(
      contains(rasterPassRecorderHeader, "BimSurfaceFrameBindingInputs"));
  EXPECT_TRUE(contains(rasterPassRecorder, "buildBimSurfaceFrameBinding"));
  EXPECT_TRUE(contains(rasterPassRecorder, "TransparentPick"));
  EXPECT_TRUE(contains(rasterPassRecorder, "TransparentLighting"));
  EXPECT_TRUE(contains(rasterPassRecorder, "meshGpuVisibilityOwnsCpuFallback"));
  EXPECT_FALSE(contains(frameRecorder, "BimSurfacePassKind::TransparentPick"));
  EXPECT_TRUE(contains(pickBlock, "BimSurfacePassKind::TransparentPick"));
  EXPECT_TRUE(
      contains(lightingBlock, "BimSurfacePassKind::TransparentLighting"));
  EXPECT_FALSE(contains(frameRecorder, "BimSurfacePassInputs"));
  EXPECT_FALSE(contains(lightingPassRecorder, "BimSurfacePassInputs"));
  EXPECT_FALSE(contains(frameRecorder,
                        ".gpuCompactionEligible = &draws == &p.bim.draws"));
  EXPECT_FALSE(contains(frameRecorder, "&draws == &p.bim.draws &&"));
  EXPECT_FALSE(contains(deferredBimBlock,
                        ".gpuCompactionEligible = &draws == &p.bim.draws"));
  EXPECT_FALSE(contains(deferredBimBlock, ".gpuVisibilityOwnsCpuFallback ="));
  EXPECT_FALSE(
      contains(pickBlock, ".gpuCompactionEligible = &draws == &p.bim.draws"));
  EXPECT_FALSE(contains(lightingBlock,
                        ".gpuCompactionEligible = &draws == &p.bim.draws"));
  EXPECT_FALSE(contains(pickBlock, ".gpuVisibilityOwnsCpuFallback ="));
  EXPECT_FALSE(contains(lightingBlock, ".gpuVisibilityOwnsCpuFallback ="));
  EXPECT_FALSE(
      contains(deferredBimBlock, "BimDrawCompactionSlot::OpaqueSingleSided"));
  EXPECT_FALSE(contains(deferredBimBlock,
                        "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_FALSE(
      contains(pickBlock, "BimDrawCompactionSlot::TransparentSingleSided"));
  EXPECT_FALSE(
      contains(lightingBlock, "BimDrawCompactionSlot::TransparentDoubleSided"));

  EXPECT_TRUE(contains(routingPlannerHeader, "BimSurfaceDrawRoutingPlanner"));
  EXPECT_TRUE(contains(routingPlannerHeader, "BimSurfaceDrawRoutingInputs"));
  EXPECT_TRUE(contains(routingPlanner, "primaryOpaqueDrawCommands"));
  EXPECT_TRUE(contains(routingPlanner, "primaryTransparentDrawCommands"));
  EXPECT_TRUE(contains(routingPlanner,
                       "BimDrawCompactionSlot::TransparentDoubleSided"));
  EXPECT_TRUE(contains(routingPlannerHeader, "gpuCompactionEligible"));
  EXPECT_TRUE(contains(routingPlanner, "gpuCompactionEligible"));
  EXPECT_TRUE(contains(routingPlanner, "gpuVisibilityOwnsCpuFallback"));
  EXPECT_EQ(routingPlanner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimSurfaceDrawRoutingPlanner.cpp"));
  EXPECT_TRUE(contains(passPlannerHeader, "BimSurfacePassPlanner"));
  EXPECT_TRUE(contains(passPlannerHeader, "BimSurfacePassSourceKind"));
  EXPECT_TRUE(contains(passPlannerHeader, "TransparentLighting"));
  EXPECT_TRUE(contains(passPlanner, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(passPlanner, "drawKindForSurfacePass"));
  EXPECT_TRUE(contains(passPlanner, "hasBimSurfaceTransparentDrawCommands"));
  EXPECT_TRUE(
      contains(passPlanner, "source == BimSurfacePassSourceKind::Mesh"));
  EXPECT_EQ(passPlanner.find("vkCmd"), std::string::npos);
  EXPECT_EQ(passPlanner.find("VkPipeline"), std::string::npos);
  EXPECT_EQ(passPlanner.find("FrameRecordParams"), std::string::npos);
  EXPECT_EQ(passPlanner.find("BimManager"), std::string::npos);
  EXPECT_EQ(passPlanner.find("DebugOverlayRenderer"), std::string::npos);
  EXPECT_EQ(passPlanner.find("drawCompactionReady"), std::string::npos);
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimSurfacePassPlanner.cpp"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimSurfacePassRecorder.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimSurfaceRasterPassRecorder.cpp"));
  EXPECT_TRUE(contains(srcCmake, "renderer/deferred/"
                                 "DeferredRasterBimSurfacePassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_surface_pass_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "bim_surface_pass_recorder_tests"));
  EXPECT_TRUE(contains(testsCmake, "bim_surface_raster_pass_recorder_tests"));
}

TEST(RenderingConventionTests, BimMeshletLodStreamingMetadataIsIdentitySafe) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string bimDrawFilterState =
      readRepoTextFile("src/renderer/bim/BimDrawFilterState.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string gpuVisibilityRecorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string surfaceRasterPassRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfaceRasterPassRecorder.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletClusterMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimObjectLodStreamingMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "optimizedModelMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectLodStreamingMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheHit"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheStale"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheWriteAttempted"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheWriteSucceeded"));
  EXPECT_TRUE(contains(bimManagerHeader, "cachePath"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheStatus"));

  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterSpansByMeshId"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelCacheKey"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(bimManager, "probeOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadataCacheMatches"));
  EXPECT_TRUE(contains(bimManager, "writeOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "refreshOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "14695981039346656037ull"));
  EXPECT_TRUE(contains(bimManager, "\"schemaVersion\""));
  EXPECT_TRUE(contains(bimManager, "\"residentCandidateClusters\""));
  EXPECT_TRUE(contains(bimManager, "stale refreshed"));
  EXPECT_TRUE(contains(bimManager, "miss written"));
  EXPECT_TRUE(contains(bimManager, "objectLodMetadata_.push_back"));
  EXPECT_TRUE(contains(bimManager, "lodMetadata.objectIndex = objectIndex"));
  EXPECT_TRUE(contains(bimManager, "drawFilterState_->objectMatchesFilter"));
  EXPECT_TRUE(
      contains(bimDrawFilterState, "metadataMatchesFilter(objectIndex"));
  EXPECT_TRUE(contains(bimManager, "visibleMeshletClusterReferences"));

  EXPECT_TRUE(contains(rendererFrontend, "optimizedModelMetadata()"));
  EXPECT_TRUE(contains(rendererFrontend, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(rendererFrontend, "meshletSourceClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStatus"));

  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheable"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheKey"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(contains(guiManagerHeader, "drawBudgetVisibleObjectCount"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cacheable"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cache: %s"));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue(\"Cache key\""));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue(\"Cache path\""));
  EXPECT_TRUE(contains(guiManager, "Budget visible"));
  EXPECT_TRUE(contains(guiManager, "meshlet residency buffers are GPU-side"));
}

TEST(RenderingConventionTests, BimPointCurveStylesAreRendererPlumbed) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string bimLightingOverlayRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPointStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimCurveStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPointCurveStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "float pointSize"));
  EXPECT_TRUE(contains(frameRecorderHeader, "float lineWidth"));
  EXPECT_TRUE(contains(frameRecorderHeader, "pointCurveStyle"));

  EXPECT_FALSE(contains(frameRecorder, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(contains(lightingPassRecorder, "bimLightingOverlayDrawLists"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "recordBimLightingOverlayFrameCommands"));
  EXPECT_TRUE(contains(bimLightingOverlayRecorder, "plan.pointStyle"));
  EXPECT_TRUE(contains(bimLightingOverlayRecorder, "plan.curveStyle"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.pointDraws"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "p.bim.pointCurveStyle.points.pointSize"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "p.bim.pointCurveStyle.curves.lineWidth"));
  EXPECT_TRUE(contains(bimLightingOverlayRecorder, "bindWireframePipeline"));
  EXPECT_TRUE(
      contains(bimLightingOverlayRecorder, "debugOverlay->drawWireframe"));
  EXPECT_TRUE(contains(bimLightingOverlayRecorder, "vkCmdSetLineWidth"));
  EXPECT_FALSE(contains(lightingPassRecorder, "drawBimPointCurveStyleOverlay"));
  EXPECT_FALSE(contains(lightingPassRecorder, "drawStyledGeometryKind"));
  EXPECT_TRUE(
      contains(bimLightingOverlayHeader, "BimLightingOverlayStyleInputs"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner, "buildStylePlan"));
  EXPECT_TRUE(
      contains(bimLightingOverlayPlanner, "sanitizeBimLightingOverlayOpacity"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner, "WireframeDepthFrontCull"));
}

TEST(RenderingConventionTests, BimLightingOverlaysUsePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string plannerSource =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayRecorder.h");
  const std::string recorderSource =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string drawCommandHeader =
      readRepoTextFile("include/Container/renderer/scene/DrawCommand.h");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass = lightingPassRecorder.find(
      "void DeferredRasterLightingPassRecorder::record");
  ASSERT_NE(lightingPass, std::string::npos);
  const std::string lightingBlock = lightingPassRecorder.substr(lightingPass);

  EXPECT_FALSE(contains(frameRecorder, "BimLightingOverlayPlanner.h"));
  EXPECT_TRUE(contains(lightingPassRecorder, "BimLightingOverlayRecorder.h"));
  EXPECT_FALSE(contains(lightingBlock, "bimLightingOverlayInputs"));
  EXPECT_TRUE(contains(lightingBlock, "bimLightingOverlayFrameDrawSources"));
  EXPECT_FALSE(contains(lightingBlock, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(contains(lightingBlock, "recordBimLightingOverlayFrameCommands"));
  EXPECT_FALSE(contains(lightingBlock, "pipelineForBimLightingOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawStyleOverlayRoutes"));
  EXPECT_FALSE(contains(lightingBlock, "drawOverlayPlan"));
  EXPECT_FALSE(contains(lightingBlock, "drawSelectionOutlinePlan"));
  EXPECT_FALSE(contains(lightingBlock, "drawBimPointCurveStyleOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawBimFloorPlanOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawInteractionWireframe"));
  EXPECT_FALSE(contains(lightingBlock, "drawNativePrimitiveHighlight"));
  EXPECT_FALSE(contains(lightingBlock, "drawSelectionOutline ="));
  EXPECT_FALSE(contains(lightingBlock, "std::clamp(p.bim.floorPlan.opacity"));
  EXPECT_FALSE(contains(lightingBlock,
                        "std::max(p.bim.primitivePasses.pointCloud.pointSize"));
  EXPECT_FALSE(contains(lightingBlock,
                        "std::max(p.bim.primitivePasses.curves.lineWidth"));

  EXPECT_TRUE(contains(plannerHeader, "BimLightingOverlayPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "BimLightingOverlayInputs"));
  EXPECT_TRUE(contains(plannerHeader, "BimLightingSelectionOutlinePlan"));
  EXPECT_TRUE(contains(plannerSource, "nativePointHoverWidth"));
  EXPECT_TRUE(contains(plannerSource, "nativeCurveSelectionWidth"));
  EXPECT_TRUE(contains(plannerSource, "sanitizeBimLightingOverlayLineWidth"));
  EXPECT_TRUE(contains(drawCommandHeader, "struct DrawCommand"));
  EXPECT_FALSE(contains(plannerSource, "vkCmd"));
  EXPECT_FALSE(contains(plannerSource, "VkPipeline"));
  EXPECT_FALSE(contains(plannerSource, "FrameRecordParams"));
  EXPECT_FALSE(contains(plannerSource, "GuiManager"));
  EXPECT_FALSE(contains(plannerSource, "DebugOverlayRenderer"));
  EXPECT_FALSE(contains(plannerSource, "LightingManager"));
  EXPECT_TRUE(contains(recorderHeader, "BimLightingOverlayRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "BimLightingOverlayFrameRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "BimLightingOverlayPipelineHandles"));
  EXPECT_TRUE(contains(recorderSource, "pipelineForBimLightingOverlay"));
  EXPECT_TRUE(contains(recorderSource, "recordBimLightingOverlayCommands"));
  EXPECT_TRUE(contains(recorderSource, "vkCmdClearAttachments"));
  EXPECT_TRUE(contains(recorderSource, "selectionMask"));
  EXPECT_TRUE(contains(recorderSource, "selectionOutline"));
  EXPECT_TRUE(contains(recorderSource, "debugOverlay->drawWireframe"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimLightingOverlayPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimLightingOverlayRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_lighting_overlay_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "bim_lighting_overlay_recorder_tests"));
}

TEST(RenderingConventionTests, BimIssueAndClashMarkersAreRendererPlumbed) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string lightingHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string lightingRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string deferredLighting = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(
      contains(frameRecorderHeader, "coordinationMarkerScene"));
  EXPECT_TRUE(
      contains(frameRecorderHeader, "coordinationIssueMarkerDrawCommands"));
  EXPECT_TRUE(
      contains(frameRecorderHeader, "coordinationClashMarkerDrawCommands"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimCoordinationMarkerDrawData"));
  EXPECT_TRUE(contains(bimManagerHeader, "rebuildCoordinationMarkerGeometry"));
  EXPECT_TRUE(contains(bimManagerHeader, "coordinationMarkerDrawData()"));
  EXPECT_TRUE(contains(bimManager,
                       "buildBimCoordinationOverlayMarkerWirePrimitives"));
  EXPECT_TRUE(contains(bimManager, "coordinationMarkerGeometrySignature_"));
  EXPECT_TRUE(contains(bimManager, "resolveIssuePinObjectIndex"));
  EXPECT_TRUE(contains(bimManager, "objectIndicesForGuid(pin.ifcGuid)"));
  EXPECT_TRUE(contains(bimManager, "objectIndicesForSourceId(pin.sourceId)"));
  EXPECT_TRUE(contains(rendererFrontend, "bimIssueOverlayPins()"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "rebuildCoordinationMarkerGeometry("));
  EXPECT_TRUE(contains(rendererFrontend, "coordinationOverlay"));
  EXPECT_TRUE(contains(lightingHeader, "CoordinationIssueMarker"));
  EXPECT_TRUE(contains(lightingHeader, "CoordinationClashMarker"));
  EXPECT_TRUE(contains(lightingRecorder, "coordinationMarkers"));
  EXPECT_TRUE(contains(deferredLighting, "coordinationMarkerScene"));
}

TEST(RenderingConventionTests,
     BimNativePrimitivePassBoundariesAreRendererScoped) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string sectionClipCapPlanner =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassPlanner.cpp");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string primitivePlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimPrimitivePassPlanner.h");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string primitiveRecorderHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimPrimitivePassRecorder.h");
  const std::string primitiveRecorder =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string wireframeShader =
      readRepoTextFile("shaders/wireframe_debug.slang");

  EXPECT_TRUE(
      contains(frameRecorderHeader, "FrameBimPointCloudPrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimCurvePrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "placeholderRangePreviewEnabled"));
  EXPECT_TRUE(contains(frameRecorderHeader, "primitivePasses"));
  EXPECT_FALSE(
      contains(frameRecorderHeader, "recordBimPointCloudPrimitivePass"));
  EXPECT_FALSE(contains(frameRecorderHeader, "recordBimCurvePrimitivePass"));

  EXPECT_TRUE(
      contains(lightingPassRecorder, "recordBimPrimitiveFramePassCommands"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "p.bim.primitivePasses.pointCloud"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.primitivePasses.curves"));
  EXPECT_FALSE(contains(frameRecorder, "buildBimPrimitivePassPlan"));
  EXPECT_TRUE(contains(lightingPassRecorder, "BimPrimitivePassKind::Points"));
  EXPECT_TRUE(contains(lightingPassRecorder, "BimPrimitivePassKind::Curves"));
  EXPECT_TRUE(contains(lightingPassRecorder, "primitivePassDrawLists"));
  EXPECT_TRUE(contains(lightingPassRecorder, "BimPrimitivePassRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimPrimitivePassCommands"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimPointCloudDepth"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimPointCloudNoDepth"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimCurveDepth"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimCurveNoDepth"));
  EXPECT_TRUE(contains(primitivePlanner, "hasBimPrimitivePassDrawCommands"));
  EXPECT_TRUE(contains(primitivePlanner, "placeholderRangePreviewEnabled"));
  EXPECT_TRUE(contains(primitivePlanner, "nativeDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(primitivePlanner, "std::clamp(inputs_.opacity"));
  EXPECT_TRUE(contains(primitivePlanner, "std::max(inputs_.primitiveSize"));
  EXPECT_TRUE(contains(primitivePlannerHeader, "BimPrimitivePassPlanner"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimPrimitivePassPlanner.cpp"));
  EXPECT_TRUE(
      contains(primitiveRecorderHeader, "BimPrimitivePassRecordInputs"));
  EXPECT_TRUE(
      contains(primitiveRecorderHeader, "BimPrimitiveFramePassRecordInputs"));
  EXPECT_TRUE(contains(primitiveRecorder, "buildBimPrimitivePassPlan"));
  EXPECT_TRUE(contains(primitiveRecorder, "recordBimPrimitivePassCommands"));
  EXPECT_TRUE(contains(primitiveRecorder, "plan.cpuDrawSources"));
  EXPECT_TRUE(contains(primitiveRecorder, "plan.gpuSlots"));
  EXPECT_TRUE(contains(primitiveRecorder, "drawCompactionReady"));
  EXPECT_TRUE(contains(primitiveRecorder, "drawCompacted"));
  EXPECT_TRUE(contains(primitiveRecorder, "debugOverlay->drawWireframe"));
  EXPECT_TRUE(contains(primitiveRecorder, "vkCmdBindPipeline"));
  EXPECT_TRUE(contains(primitiveRecorder, "vkCmdSetLineWidth"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimPrimitivePassRecorder.cpp"));
  EXPECT_FALSE(contains(frameRecorder, "drawGpuCompactedNativePoint"));
  EXPECT_FALSE(contains(frameRecorder, "drawGpuCompactedNativeCurve"));
  EXPECT_TRUE(contains(primitivePlanner, "cpuDrawSources"));
  EXPECT_TRUE(contains(primitivePlanner, "draws.opaqueDrawCommands"));
  EXPECT_TRUE(contains(primitivePlanner, "draws.transparentDrawCommands"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "draws.opaqueSingleSidedDrawCommands"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "draws.transparentSingleSidedDrawCommands"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.nativeCurveDraws"));

  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.pointPrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.curvePrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_FALSE(contains(rendererFrontend, "if (!nativePointDrawsAvailable)"));
  EXPECT_FALSE(contains(rendererFrontend, "if (!nativeCurveDrawsAvailable)"));
  EXPECT_TRUE(
      contains(rendererFrontend, "assignFrameDrawLists(p.bim.pointDraws"));
  EXPECT_TRUE(
      contains(rendererFrontend, "assignFrameDrawLists(p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.bim.primitivePasses.pointCloud.enabled"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.bim.primitivePasses.curves.enabled"));

  EXPECT_TRUE(contains(pipelineTypes, "bimPointCloudDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimPointCloudNoDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimCurveDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimCurveNoDepth"));

  EXPECT_TRUE(contains(pipelineBuilder, "VK_PRIMITIVE_TOPOLOGY_POINT_LIST"));
  EXPECT_TRUE(contains(pipelineBuilder, "VK_PRIMITIVE_TOPOLOGY_LINE_LIST"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_point_cloud_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_point_cloud_no_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_curve_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_curve_no_depth_pipeline"));
  EXPECT_TRUE(contains(wireframeShader, "SV_PointSize"));
  EXPECT_TRUE(contains(wireframeShader, "output.pointSize = max(pc.lineWidth"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveGpuVisibilityHasRecorderContract) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string gpuVisibilityRecorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string primitiveRecorder =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  const size_t slotStart =
      bimManagerHeader.find("enum class BimDrawCompactionSlot");
  ASSERT_NE(slotStart, std::string::npos);
  const size_t slotEnd = bimManagerHeader.find("Count", slotStart);
  ASSERT_NE(slotEnd, std::string::npos);
  const std::string slotBlock =
      bimManagerHeader.substr(slotStart, slotEnd - slotStart);
  const bool nativePrimitiveCompactionSlots =
      contains(slotBlock, "NativePoint") || contains(slotBlock, "NativeCurve");

  const size_t pointPass =
      lightingPassRecorder.find("bimPointCloudPrimitivePassStyle");
  ASSERT_NE(pointPass, std::string::npos);
  const size_t curvePass =
      lightingPassRecorder.find("bimCurvePrimitivePassStyle", pointPass);
  ASSERT_NE(curvePass, std::string::npos);
  const size_t curveEnd =
      lightingPassRecorder.find("BimSectionClipCapFramePassStyle", curvePass);
  ASSERT_NE(curveEnd, std::string::npos);
  const std::string pointBlock =
      lightingPassRecorder.substr(pointPass, curvePass - pointPass);
  const std::string curveBlock =
      lightingPassRecorder.substr(curvePass, curveEnd - curvePass);

  EXPECT_TRUE(contains(slotBlock, "NativePointOpaque"));
  EXPECT_TRUE(contains(slotBlock, "NativePointTransparent"));
  EXPECT_TRUE(contains(slotBlock, "NativeCurveOpaque"));
  EXPECT_TRUE(contains(slotBlock, "NativeCurveTransparent"));
  EXPECT_FALSE(contains(frameRecorder, "prepareBimFrameGpuVisibility"));
  EXPECT_TRUE(
      contains(deferredFrameGraphContext, "prepareBimFrameGpuVisibility"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(primitiveRecorder, "plan.cpuDrawSources"));
  EXPECT_TRUE(contains(primitiveRecorder, "plan.gpuSlots"));
  EXPECT_FALSE(contains(pointBlock, "p.bim.draws"));
  EXPECT_FALSE(contains(curveBlock, "p.bim.draws"));
  EXPECT_TRUE(nativePrimitiveCompactionSlots);
  const bool recorderConsumesNativePrimitiveCompaction =
      contains(primitiveRecorder, "drawCompacted") &&
      contains(primitiveRecorder, "drawCompactionReady") &&
      contains(pointBlock, "p.bim.nativePointDrawsUseGpuVisibility") &&
      contains(curveBlock, "p.bim.nativeCurveDrawsUseGpuVisibility");
  if (recorderConsumesNativePrimitiveCompaction) {
    EXPECT_TRUE(contains(primitiveRecorder, "plan.gpuSlots"));
    EXPECT_TRUE(
        contains(primitivePlanner, "BimDrawCompactionSlot::NativePointOpaque"));
    EXPECT_TRUE(contains(primitivePlanner,
                         "BimDrawCompactionSlot::NativePointTransparent"));
    EXPECT_TRUE(
        contains(primitivePlanner, "BimDrawCompactionSlot::NativeCurveOpaque"));
    EXPECT_TRUE(contains(primitivePlanner,
                         "BimDrawCompactionSlot::NativeCurveTransparent"));
  } else {
    EXPECT_TRUE(
        contains(rendererFrontend, "gpuFilteredNativePrimitiveDrawSources"));
    EXPECT_TRUE(contains(rendererFrontend, "return {};"))
        << "RendererFrontend must keep CPU filtering enabled until "
           "FrameRecorder "
           "can consume native primitive GPU visibility.";
  }
}

TEST(RenderingConventionTests,
     BimNativePointCurveRangesBypassTriangleSurfacePasses) {
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string lightingOverlayRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string deferredFrameState =
      readRepoTextFile("src/renderer/deferred/DeferredRasterFrameState.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  const size_t drawBuildSwitch =
      bimManager.find("switch (pending.metadata.geometryKind)");
  ASSERT_NE(drawBuildSwitch, std::string::npos);
  const size_t pointCase =
      bimManager.find("case BimGeometryKind::Points:", drawBuildSwitch);
  ASSERT_NE(pointCase, std::string::npos);
  const size_t curveCase =
      bimManager.find("case BimGeometryKind::Curves:", pointCase);
  ASSERT_NE(curveCase, std::string::npos);
  const size_t meshCase =
      bimManager.find("case BimGeometryKind::Mesh:", curveCase);
  ASSERT_NE(meshCase, std::string::npos);

  const std::string pointBlock =
      bimManager.substr(pointCase, curveCase - pointCase);
  const std::string curveBlock =
      bimManager.substr(curveCase, meshCase - curveCase);
  EXPECT_TRUE(contains(pointBlock, "if (pending.nativeIndexCount > 0u)"));
  EXPECT_TRUE(contains(pointBlock, "nativePointDrawLists_"));
  EXPECT_TRUE(contains(pointBlock, "} else {"));
  EXPECT_TRUE(contains(pointBlock, "pointDrawLists_"));
  EXPECT_TRUE(contains(curveBlock, "if (pending.nativeIndexCount > 0u)"));
  EXPECT_TRUE(contains(curveBlock, "nativeCurveDrawLists_"));
  EXPECT_TRUE(contains(curveBlock, "} else {"));
  EXPECT_TRUE(contains(curveBlock, "curveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "pointCurvePickingCommands"));
  EXPECT_TRUE(contains(bimManager, "geometryDrawListsCoverObject"));
  EXPECT_TRUE(contains(bimManager, "nativePointCurveObjectHasNativeDraw"));

  const size_t collectStart =
      bimManager.find("void BimManager::collectDrawCommandsForObject");
  ASSERT_NE(collectStart, std::string::npos);
  const size_t collectEnd =
      bimManager.find("std::filesystem::path", collectStart);
  ASSERT_NE(collectEnd, std::string::npos);
  const std::string collectBlock =
      bimManager.substr(collectStart, collectEnd - collectStart);
  const size_t nativeSkip =
      collectBlock.find("nativePointCurveObjectHasNativeDraw");
  const size_t placeholderAppend =
      collectBlock.find("outCommands.push_back(objectDrawCommands_");
  ASSERT_NE(nativeSkip, std::string::npos);
  ASSERT_NE(placeholderAppend, std::string::npos);
  EXPECT_LT(nativeSkip, placeholderAppend);

  EXPECT_TRUE(contains(deferredFrameState, "bimSurfaceDrawListSet"));
  EXPECT_FALSE(contains(frameRecorder, "bimDrawListSet"));
  EXPECT_TRUE(
      contains(deferredFrameState,
               "return {&bim.draws, &bim.pointDraws, &bim.curveDraws};"));
  EXPECT_TRUE(
      contains(deferredFrameState,
               "Native point/curve ranges are submitted by the dedicated"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, ".nativeDraws = primitivePassDrawLists"));
  EXPECT_TRUE(
      contains(primitivePlanner, "hasNativeDraws ? inputs_.nativeDraws"));
  EXPECT_TRUE(
      contains(primitivePlanner, "inputs_.placeholderRangePreviewEnabled"));
  const size_t selectionStart =
      lightingOverlayRecorder.find("bool drawSelectionOutline");
  ASSERT_NE(selectionStart, std::string::npos);
  const size_t selectionEnd = lightingOverlayRecorder.find(
      "recordBimLightingOverlayCommands", selectionStart);
  ASSERT_NE(selectionEnd, std::string::npos);
  const std::string selectionBlock = lightingOverlayRecorder.substr(
      selectionStart, selectionEnd - selectionStart);
  EXPECT_TRUE(contains(selectionBlock, "vkCmdClearAttachments"));
  EXPECT_TRUE(contains(selectionBlock, "selectionMask"));
  EXPECT_TRUE(contains(selectionBlock, "selectionOutline"));
  EXPECT_TRUE(contains(lightingOverlayRecorder,
                       ".bimSelectionCommands = inputs.draws.bimSelection"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "p.bim.draws.selectedDrawCommands"));
  EXPECT_TRUE(contains(lightingOverlayRecorder,
                       ".nativePointSelectionCommands = "
                       "inputs.draws.nativePointSelection"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "p.bim.nativePointDraws.selectedDrawCommands"));
  EXPECT_TRUE(contains(lightingOverlayRecorder,
                       ".nativeCurveSelectionCommands = "
                       "inputs.draws.nativeCurveSelection"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "p.bim.nativeCurveDraws.selectedDrawCommands"));
  EXPECT_TRUE(contains(lightingOverlayRecorder, "plan.bimSelectionOutline"));
  EXPECT_TRUE(contains(lightingOverlayRecorder, "plan.nativePointSelection"));
  EXPECT_TRUE(contains(lightingOverlayRecorder, "plan.nativeCurveSelection"));
  EXPECT_TRUE(contains(lightingOverlayRecorder,
                       "BimLightingOverlayPipeline::BimPointCloudDepth"));
  EXPECT_TRUE(contains(lightingOverlayRecorder,
                       "BimLightingOverlayPipeline::BimCurveDepth"));

  const size_t pointFrontendBlockStart =
      rendererFrontend.find("if (bimLayers.pointCloudVisible)");
  ASSERT_NE(pointFrontendBlockStart, std::string::npos);
  const size_t curveFrontendBlockStart = rendererFrontend.find(
      "if (bimLayers.curvesVisible)", pointFrontendBlockStart);
  ASSERT_NE(curveFrontendBlockStart, std::string::npos);
  const size_t frontendBlockEnd = rendererFrontend.find(
      "hoveredBimDrawCommands_.clear()", curveFrontendBlockStart);
  ASSERT_NE(frontendBlockEnd, std::string::npos);
  const std::string frontendBlock = rendererFrontend.substr(
      pointFrontendBlockStart, frontendBlockEnd - pointFrontendBlockStart);
  EXPECT_TRUE(contains(frontendBlock, "assignFrameDrawLists(p.bim.pointDraws"));
  EXPECT_TRUE(
      contains(frontendBlock, "assignFrameDrawLists(p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(frontendBlock, "assignFrameDrawLists(p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(frontendBlock, "assignFrameDrawLists(p.bim.nativeCurveDraws"));
  EXPECT_FALSE(contains(frontendBlock, "if (!nativePointDrawsAvailable)"));
  EXPECT_FALSE(contains(frontendBlock, "if (!nativeCurveDrawsAvailable)"));
  EXPECT_TRUE(
      contains(rendererFrontend, "collectNativePointDrawCommandsForObject"));
  EXPECT_TRUE(
      contains(rendererFrontend, "collectNativeCurveDrawCommandsForObject"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.bim.nativePointDraws.selectedDrawCommands"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.bim.nativeCurveDraws.selectedDrawCommands"));
  EXPECT_TRUE(
      contains(bimManager, "BimManager::collectNativeDrawCommandsForObject"));
  EXPECT_TRUE(contains(bimManager, "selectedCommand.instanceCount = 1u"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveTransparencyDoesNotForcePickDepthFiltering) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  const size_t surfaceHelperStart = rendererFrontend.find(
      "bool hasTransparentBimSurfaceGeometry(const BimManager&");
  ASSERT_NE(surfaceHelperStart, std::string::npos);
  const size_t surfaceHelperEnd = rendererFrontend.find(
      "BimFrameGpuVisibilityInputs bimFrameGpuVisibilityInputs",
      surfaceHelperStart);
  ASSERT_NE(surfaceHelperEnd, std::string::npos);
  const std::string surfaceHelperBlock = rendererFrontend.substr(
      surfaceHelperStart, surfaceHelperEnd - surfaceHelperStart);
  EXPECT_TRUE(contains(surfaceHelperBlock, "pointDrawLists()"));
  EXPECT_TRUE(contains(surfaceHelperBlock, "curveDrawLists()"));
  EXPECT_FALSE(contains(surfaceHelperBlock, "nativePointDrawLists()"));
  EXPECT_FALSE(contains(surfaceHelperBlock, "nativeCurveDrawLists()"));

  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "inputs_.pointCloudVisible &&"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "inputs_.curvesVisible &&"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "inputs_.unfilteredNativePointDraws"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "inputs_.unfilteredNativeCurveDraws"));

  const size_t depthStart = rendererFrontend.find(
      "void RendererFrontend::markDepthVisibilityFrameComplete");
  ASSERT_NE(depthStart, std::string::npos);
  const size_t depthEnd = rendererFrontend.find(
      "bool RendererFrontend::depthVisibilityFrameMatchesCurrentState",
      depthStart);
  ASSERT_NE(depthEnd, std::string::npos);
  const std::string depthBlock =
      rendererFrontend.substr(depthStart, depthEnd - depthStart);
  EXPECT_TRUE(contains(depthBlock, "transparentPickSurfaceGeometry"));
  EXPECT_TRUE(contains(depthBlock, "cpuFilteredSurfaceGeometryRequired"));
  EXPECT_TRUE(contains(depthBlock,
                       "hasAnyGeometry(subs_.bimManager->pointDrawLists())"));
  EXPECT_TRUE(contains(depthBlock,
                       "hasAnyGeometry(subs_.bimManager->curveDrawLists())"));
  EXPECT_TRUE(contains(depthBlock, "hasTransparentBimSurfaceGeometry("));
  EXPECT_TRUE(contains(depthBlock, "filteredDraws"));
  EXPECT_FALSE(contains(depthBlock, "nativePointDrawLists()"));
  EXPECT_FALSE(contains(depthBlock, "nativeCurveDrawLists()"));
  EXPECT_FALSE(
      contains(depthBlock, "hasTransparentBimGeometry(*subs_.bimManager"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveGpuVisibilityHandoffBypassesCpuFiltering) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "nativeCurveDrawsUseGpuVisibility"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "BimFrameDrawSource::GpuFiltered"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "chooseNativePrimitiveDraws"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "plan.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "inputs_.unfilteredNativePointDraws"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "inputs_.unfilteredNativeCurveDraws"));

  const size_t frameStart =
      rendererFrontend.find("RendererFrontend::buildFrameRecordParams");
  ASSERT_NE(frameStart, std::string::npos);
  const size_t frameEnd = rendererFrontend.find(
      "p.transformGizmo = buildTransformGizmoState", frameStart);
  ASSERT_NE(frameEnd, std::string::npos);
  const std::string frameBlock =
      rendererFrontend.substr(frameStart, frameEnd - frameStart);
  const size_t sourceCall =
      frameBlock.find("buildBimFrameDrawRoutingPlan(bimRoutingInputs)");
  const size_t cpuNeed = frameBlock.find("bimRouting.cpuFilteredDrawsRequired");
  const size_t filteredCall = frameBlock.find("filteredDrawLists(bimFilter)");
  ASSERT_NE(sourceCall, std::string::npos);
  ASSERT_NE(cpuNeed, std::string::npos);
  ASSERT_NE(filteredCall, std::string::npos);
  EXPECT_LT(sourceCall, cpuNeed);
  EXPECT_LT(cpuNeed, filteredCall);
  EXPECT_TRUE(contains(frameBlock, "bimFrameDrawRoutingInputs"));
  EXPECT_TRUE(contains(frameBlock, "bimRoutingInputs.cpuFilteredDraws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.nativePointDraws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.nativeCurveDraws"));
  EXPECT_TRUE(contains(frameBlock, "p.bim.nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(frameBlock, "p.bim.nativeCurveDrawsUseGpuVisibility"));
}

TEST(RenderingConventionTests, BimFrameDrawRoutingUsesPlanner) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimFrameDrawRoutingPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t frameStart =
      rendererFrontend.find("RendererFrontend::buildFrameRecordParams");
  ASSERT_NE(frameStart, std::string::npos);
  const size_t frameEnd = rendererFrontend.find(
      "p.transformGizmo = buildTransformGizmoState", frameStart);
  ASSERT_NE(frameEnd, std::string::npos);
  const std::string frameBlock =
      rendererFrontend.substr(frameStart, frameEnd - frameStart);

  EXPECT_TRUE(contains(rendererFrontend, "BimFrameDrawRoutingPlanner.h"));
  EXPECT_TRUE(contains(frameBlock, "bimFrameDrawRoutingInputs"));
  EXPECT_TRUE(contains(frameBlock, "buildBimFrameDrawRoutingPlan"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(contains(frameBlock, "assignFrameDrawLists(p.bim.draws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.pointPrimitivePassEnabled"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.curvePrimitivePassEnabled"));
  EXPECT_FALSE(contains(frameBlock, "gpuFilteredNativePrimitiveDrawSources"));
  EXPECT_FALSE(contains(frameBlock, "needsCpuFilteredBimDrawLists"));
  EXPECT_FALSE(contains(frameBlock, "chooseNativePrimitiveDrawLists"));

  EXPECT_TRUE(contains(plannerHeader, "BimFrameDrawRoutingPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "BimFrameDrawSource"));
  EXPECT_TRUE(contains(planner, "meshDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "nativeCurveDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "chooseNativePrimitiveDraws"));
  EXPECT_EQ(planner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimFrameDrawRoutingPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_frame_draw_routing_planner_tests"));
}

TEST(RenderingConventionTests, BimMeshletLodStreamingMetadataIsSurfaced) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");
  const std::string ifcxLoader =
      readRepoTextFile("src/geometry/IfcxLoader.cpp");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string deferredFrameGraphContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string gpuVisibilityRecorder =
      readRepoTextFile("src/renderer/bim/BimFrameGpuVisibilityRecorder.cpp");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string surfaceRasterPassRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfaceRasterPassRecorder.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string meshletResidencyShader =
      readRepoTextFile("shaders/bim_meshlet_residency.slang");
  const std::string visibilityFilterShader =
      readRepoTextFile("shaders/bim_visibility_filter.slang");
  const std::string drawCompactionShader =
      readRepoTextFile("shaders/bim_draw_compact.slang");
  const std::string shadersCmake = readRepoTextFile("cmake/Shaders.cmake");

  EXPECT_TRUE(contains(dotBimHeader, "MeshletClusterRange"));
  EXPECT_TRUE(contains(dotBimHeader,
                       "std::vector<MeshletClusterRange> meshletClusters"));
  EXPECT_TRUE(contains(dotBimHeader, "uint32_t lodLevel"));
  EXPECT_TRUE(contains(dotBimHeader, "boundsCenter"));
  EXPECT_TRUE(contains(dotBimHeader, "boundsRadius"));

  EXPECT_TRUE(contains(usdLoader, "appendMeshletClustersForRange"));
  EXPECT_TRUE(contains(usdLoader, "model.meshletClusters.push_back"));
  EXPECT_TRUE(contains(usdLoader, ".lodLevel = 0u"));
  EXPECT_TRUE(contains(ifcxLoader, "appendMeshletClustersForRange"));
  EXPECT_TRUE(contains(ifcxLoader, "model.meshletClusters.push_back"));
  EXPECT_TRUE(contains(ifcxLoader, ".lodLevel = 0u"));

  EXPECT_TRUE(contains(bimManagerHeader, "size_t meshletClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletClusterMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimObjectLodStreamingMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletGpuCluster"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletGpuObjectLod"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyEntry"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencySettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityGpuObjectMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityFilterPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityFilterStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionSlot"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "visibilityMaskReadyForDrawCompaction"));
  EXPECT_TRUE(contains(bimManagerHeader, "inputSourceRevision"));
  EXPECT_TRUE(contains(bimManagerHeader, "OpaqueWindingFlipped"));
  EXPECT_TRUE(contains(bimManagerHeader, "OpaqueDoubleSided"));
  EXPECT_TRUE(contains(bimManagerHeader, "NativePointOpaque"));
  EXPECT_TRUE(contains(bimManagerHeader, "NativeCurveTransparent"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactionSlots_"));
  EXPECT_TRUE(contains(bimManagerHeader, "computeReady"));
  EXPECT_TRUE(contains(bimManagerHeader, "dispatchPending"));
  EXPECT_TRUE(contains(bimManagerHeader, "createMeshletResidencyResources"));
  EXPECT_TRUE(contains(bimManagerHeader, "updateMeshletResidencySettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "updateVisibilityFilterSettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordMeshletResidencyUpdate"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordVisibilityFilterUpdate"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordDrawCompactionUpdate"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "drawCompacted(BimDrawCompactionSlot"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactedOpaqueSingleSided"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectLodStreamingMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletClusterBuffer()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletResidencyBuffer()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletResidencyStats()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactionStats()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterCountForModel"));
  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "model.meshletClusters.size()"));
  EXPECT_TRUE(contains(
      bimManager, "meshletClusters_ = buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(
      contains(bimManager, "meshletClusterCount_ = meshletClusters_.size()"));
  EXPECT_TRUE(contains(bimManager, "buildOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManager, "objectLodMetadata_.push_back"));
  EXPECT_TRUE(contains(bimManager, "uploadMeshletResidencyBuffers"));
  EXPECT_TRUE(contains(bimManager, "uploadVisibilityFilterBuffers"));
  EXPECT_TRUE(contains(bimManager, "createVisibilityFilterResources"));
  EXPECT_TRUE(contains(bimManager, "writeVisibilityFilterDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "sameVisibilityFilterSettings"));
  EXPECT_TRUE(contains(bimManager, "visibilityFilterMaskCurrent_"));
  EXPECT_TRUE(contains(bimManager, "invalidateDrawCompactionOutputs"));
  EXPECT_TRUE(contains(bimManager, "writeMeshletResidencyDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "createDrawCompactionResources"));
  EXPECT_TRUE(contains(bimManager, "writeDrawCompactionDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "createComputePipeline"));
  EXPECT_TRUE(contains(bimManager, "vkCmdDispatch"));
  EXPECT_TRUE(contains(bimManager, "vkCmdDrawIndexedIndirectCount"));
  EXPECT_TRUE(contains(bimManager, "drawCompactionSlots_"));
  EXPECT_TRUE(contains(bimManager, "drawCompactionDescriptorSets_"));
  EXPECT_TRUE(contains(bimManager, "visibilityMaskBuffer_"));
  EXPECT_TRUE(contains(bimManager, "visibilityFilterSettings_"));
  EXPECT_TRUE(contains(bimManager, "cameraBuffer"));
  EXPECT_TRUE(contains(bimManager, "objectBuffer"));
  EXPECT_TRUE(contains(bimManager, "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"));
  EXPECT_TRUE(contains(bimManager, "VK_PIPELINE_BIND_POINT_COMPUTE"));
  EXPECT_TRUE(contains(bimManager, "VK_BUFFER_USAGE_STORAGE_BUFFER_BIT"));
  EXPECT_TRUE(contains(bimManager, "meshletResidencyStats_"));
  EXPECT_TRUE(contains(rendererFrontend, "createMeshletResidencyResources"));
  EXPECT_TRUE(contains(rendererFrontend, "bimLodStreamingUiState()"));
  EXPECT_TRUE(contains(rendererFrontend, "updateMeshletResidencySettings"));
  EXPECT_TRUE(contains(rendererFrontend, "updateVisibilityFilterSettings"));
  EXPECT_TRUE(contains(rendererFrontend, "bimFrameGpuVisibilityInputs"));
  EXPECT_TRUE(contains(rendererFrontend, "buildBimFrameDrawRoutingPlan"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "BimFrameDrawRoutingPlanner"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "bimFrameGpuVisibilityAvailable"));
  EXPECT_TRUE(
      contains(bimFrameRoutingPlanner, "plan.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.meshletClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletSourceClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletEstimatedClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletObjectReferenceCount"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.meshletMaxLodLevel"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheable"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.meshletGpuResidentObjectCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletGpuBufferBytes"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletGpuComputeReady"));
  EXPECT_TRUE(contains(frameRecorderHeader, "BimManager*") ||
              contains(frameRecorderHeader, "BimManager *"));
  EXPECT_TRUE(contains(frameRecorderHeader, "opaqueMeshDrawsUseGpuVisibility"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimFrameGpuVisibilityCommands"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "recordBimFrameGpuVisibilityCommands"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordMeshletResidencyUpdate"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordVisibilityFilterUpdate"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(gpuVisibilityRecorder, "recordDrawCompactionUpdate"));
  EXPECT_TRUE(
      contains(surfaceRasterPassRecorder, "gpuVisibilityOwnsCpuFallback"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "usesGpuFilteredBimMeshShadowPath"));
  EXPECT_TRUE(contains(shadowPassRecorder, "recordBimGpuRoutes"));
  EXPECT_TRUE(contains(shadowPassRecorder, "drawCompactionReady(slot)"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_TRUE(
      contains(shadowPassRecorder, "BimDrawCompactionSlot::OpaqueDoubleSided"));
  EXPECT_TRUE(contains(shadowPassRecorder, "drawCompacted("));
  EXPECT_FALSE(contains(frameRecorder, "p.services.bimManager"));
  EXPECT_FALSE(contains(frameRecorder, "p.camera.cameraBuffer"));
  EXPECT_FALSE(contains(frameRecorder, "p.bim.scene.objectBuffer"));
  EXPECT_TRUE(contains(deferredFrameGraphContext, "p.services.bimManager"));
  EXPECT_TRUE(
      contains(deferredFrameGraphContext, "DeferredRasterBufferId::Camera"));
  EXPECT_TRUE(contains(deferredFrameGraphContext, "deferredRasterBuffer("));
  EXPECT_TRUE(contains(deferredFrameGraphContext, "p.bim.scene.objectBuffer"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletSourceClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletEstimatedClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuResidentObjectCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuBufferBytes"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuComputeReady"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLodStreamingUiState()"));
  EXPECT_TRUE(contains(guiManager,
                       "Meshlet clusters: %zu (%zu source, %zu estimated)"));
  EXPECT_TRUE(contains(guiManager, "GPU meshlet residency"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cache: %s"));
  EXPECT_TRUE(contains(guiManager, "TextWrappedPathValue(\"Cache path\""));
  EXPECT_TRUE(
      contains(guiManager, "Draw budget preserves BIM object identity"));
  EXPECT_TRUE(contains(guiManager, "GPU-side and cacheable"));
  EXPECT_TRUE(contains(meshletResidencyShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(meshletResidencyShader, "RWStructuredBuffer"));
  EXPECT_TRUE(contains(meshletResidencyShader, "BimMeshletResidencyEntry"));
  EXPECT_TRUE(contains(meshletResidencyShader, "forceResident"));
  EXPECT_TRUE(contains(meshletResidencyShader, "ConstantBuffer<CameraBuffer>"));
  EXPECT_TRUE(
      contains(meshletResidencyShader, "StructuredBuffer<ObjectBuffer>"));
  EXPECT_TRUE(contains(meshletResidencyShader, "projectedRadiusPixels"));
  EXPECT_TRUE(contains(meshletResidencyShader, "selectLodLevel"));
  EXPECT_TRUE(contains(meshletResidencyShader, "drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(meshletResidencyShader, "screenErrorPixels"));
  EXPECT_TRUE(contains(meshletResidencyShader, "BIM_GEOMETRY_KIND_POINTS"));
  EXPECT_TRUE(contains(meshletResidencyShader, "nativePrimitive"));
  EXPECT_TRUE(contains(visibilityFilterShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(visibilityFilterShader, "BimVisibilityObjectMetadata"));
  EXPECT_TRUE(contains(visibilityFilterShader, "uVisibilityMask"));
  EXPECT_TRUE(contains(visibilityFilterShader, "BIM_VIS_FILTER_TYPE"));
  EXPECT_TRUE(
      contains(visibilityFilterShader, "BIM_VIS_FILTER_ISOLATE_SELECTION"));
  EXPECT_TRUE(contains(drawCompactionShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(drawCompactionShader, "InterlockedAdd"));
  EXPECT_TRUE(contains(drawCompactionShader, "objectIsResident"));
  EXPECT_TRUE(contains(drawCompactionShader, "uVisibilityMask"));
  EXPECT_TRUE(
      contains(drawCompactionShader, "source.firstInstance + instanceIndex"));
  EXPECT_TRUE(contains(drawCompactionShader, "DrawIndexedIndirectCommand"));
  EXPECT_TRUE(contains(shadersCmake, "bim_meshlet_residency.slang"));
  EXPECT_TRUE(contains(shadersCmake, "bim_visibility_filter.slang"));
  EXPECT_TRUE(contains(shadersCmake, "bim_draw_compact.slang"));
  EXPECT_TRUE(contains(shadersCmake, "${SHADER_BASE}.comp.spv"));
}

TEST(RenderingConventionTests, BimGpuVisibilityFeedsShadowCasterSlots) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string shadowPassRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string secondaryPlannerHeader =
      readRepoTextFile("include/Container/renderer/shadow/"
                       "ShadowSecondaryCommandBufferPlanner.h");
  const std::string secondaryPlanner = readRepoTextFile(
      "src/renderer/shadow/ShadowSecondaryCommandBufferPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t secondaryDecision = shadowFramePassRecorder.find(
      "bool ShadowCascadeFramePassRecorder::shouldUseSecondaryCommandBuffer");
  ASSERT_NE(secondaryDecision, std::string::npos);
  const size_t secondaryDecisionEnd = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::drawPlannerInputs", secondaryDecision);
  ASSERT_NE(secondaryDecisionEnd, std::string::npos);
  const std::string secondaryBlock = shadowFramePassRecorder.substr(
      secondaryDecision, secondaryDecisionEnd - secondaryDecision);
  EXPECT_TRUE(
      contains(secondaryBlock, "buildShadowSecondaryCommandBufferPlan"));
  EXPECT_TRUE(
      contains(secondaryBlock, "usesGpuFilteredBimMeshShadowPath(params)"));
  EXPECT_FALSE(contains(secondaryBlock, "return false;"));
  EXPECT_TRUE(contains(secondaryPlannerHeader,
                       "kMinShadowSecondaryCommandBufferCpuCommands"));
  EXPECT_TRUE(
      contains(secondaryPlannerHeader, "ShadowSecondaryCommandBufferPlan"));
  EXPECT_TRUE(
      contains(secondaryPlanner, "!inputs.usesGpuFilteredBimMeshShadowPath"));
  EXPECT_TRUE(
      contains(secondaryPlanner, "inputs.secondaryCommandBuffersEnabled"));
  EXPECT_TRUE(contains(secondaryPlanner, "inputs.shadowPassRecordable"));
  EXPECT_TRUE(
      contains(secondaryPlanner, "inputs.secondaryCommandBufferAvailable"));
  EXPECT_TRUE(contains(secondaryPlanner,
                       "kMinShadowSecondaryCommandBufferCpuCommands"));
  EXPECT_FALSE(contains(secondaryPlanner, "FrameRecordParams"));
  EXPECT_FALSE(contains(secondaryPlanner, "VkCommandBuffer"));
  EXPECT_FALSE(contains(secondaryPlanner, "BimManager"));
  EXPECT_FALSE(contains(secondaryPlanner, "ShadowCullManager"));
  EXPECT_FALSE(contains(secondaryPlanner, "RenderGraph"));
  EXPECT_FALSE(contains(secondaryPlanner, "std::async"));
  EXPECT_FALSE(contains(secondaryPlanner, "vkCmd"));
  EXPECT_TRUE(contains(
      srcCmake, "renderer/shadow/ShadowSecondaryCommandBufferPlanner.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "shadow_secondary_command_buffer_planner_tests"));

  const size_t shadowBody = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::cascadePassRecordInputs");
  ASSERT_NE(shadowBody, std::string::npos);
  const size_t shadowBodyEnd = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::secondaryPassRecordInputs", shadowBody);
  ASSERT_NE(shadowBodyEnd, std::string::npos);
  const std::string shadowBlock =
      shadowFramePassRecorder.substr(shadowBody, shadowBodyEnd - shadowBody);
  const std::string shadowPassPlanner =
      readRepoTextFile("src/renderer/shadow/ShadowPassDrawPlanner.cpp");

  const size_t bimGeometry =
      shadowBlock.find(".bimGeometryReady = hasBimShadowGeometry(params)");
  const size_t routePlan = shadowPassRecorder.find("buildShadowPassDrawPlan");
  const size_t recorderCall =
      shadowPassRecorder.find("recordShadowPassCommands", routePlan);
  const size_t gpuRoutes = shadowPassRecorder.find("recordBimGpuRoutes");
  const size_t cpuRoutes =
      shadowPassRecorder.find("recordBimCpuRoutes", gpuRoutes);
  ASSERT_NE(bimGeometry, std::string::npos);
  ASSERT_NE(routePlan, std::string::npos);
  ASSERT_NE(recorderCall, std::string::npos);
  ASSERT_NE(gpuRoutes, std::string::npos);
  ASSERT_NE(cpuRoutes, std::string::npos);
  EXPECT_LT(routePlan, recorderCall);
  EXPECT_LT(gpuRoutes, cpuRoutes);

  EXPECT_TRUE(
      contains(shadowPassRecorder, "bimDrawCompactionSlot(route.slot)"));
  EXPECT_TRUE(contains(shadowPassRecorder, "drawCompactionReady(slot)"));
  EXPECT_TRUE(contains(shadowPassRecorder, "drawCompacted(slot, cmd)"));
  EXPECT_TRUE(
      contains(shadowPassRecorder, "BimDrawCompactionSlot::OpaqueSingleSided"));
  EXPECT_TRUE(contains(shadowPassRecorder,
                       "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_TRUE(
      contains(shadowPassRecorder, "BimDrawCompactionSlot::OpaqueDoubleSided"));
  EXPECT_TRUE(
      contains(shadowPassPlanner, "ShadowPassBimGpuSlot::OpaqueSingleSided"));
  EXPECT_TRUE(contains(shadowPassPlanner,
                       "ShadowPassBimGpuSlot::OpaqueWindingFlipped"));
  EXPECT_TRUE(
      contains(shadowPassPlanner, "ShadowPassBimGpuSlot::OpaqueDoubleSided"));

  const size_t prepareCascade = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::prepareDrawCommands");
  ASSERT_NE(prepareCascade, std::string::npos);
  const size_t prepareCascadeEnd = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::clearDrawCommandCache",
      prepareCascade);
  ASSERT_NE(prepareCascadeEnd, std::string::npos);
  const std::string prepareBlock = shadowFramePassRecorder.substr(
      prepareCascade, prepareCascadeEnd - prepareCascade);
  EXPECT_TRUE(contains(prepareBlock, "ShadowCascadeDrawPlanner"));
  EXPECT_TRUE(contains(prepareBlock, "planner.build()"));
  EXPECT_TRUE(contains(shadowFramePassRecorder,
                       "params.bim.opaqueMeshDrawsUseGpuVisibility"));
}

TEST(RenderingConventionTests, ShadowCascadePreparationPolicyUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCascadePreparationPlanner.h");
  const std::string planner = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadePreparationPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t prepareDecision = shadowFramePassRecorder.find(
      "bool ShadowCascadeFramePassRecorder::shouldPrepareDrawCommands");
  ASSERT_NE(prepareDecision, std::string::npos);
  const size_t prepareDecisionEnd = shadowFramePassRecorder.find(
      "bool ShadowCascadeFramePassRecorder::useGpuCullForCascade",
      prepareDecision);
  ASSERT_NE(prepareDecisionEnd, std::string::npos);
  const std::string prepareBlock = shadowFramePassRecorder.substr(
      prepareDecision, prepareDecisionEnd - prepareDecision);

  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "ShadowCascadePreparationPlanner.h"));
  EXPECT_TRUE(contains(prepareBlock, "ShadowCascadePreparationPlanInputs"));
  EXPECT_TRUE(contains(prepareBlock, "buildShadowCascadePreparationPlan"));
  EXPECT_TRUE(contains(
      prepareBlock, "inputs.shadowAtlasVisible = context.shadowAtlasVisible"));
  EXPECT_TRUE(contains(prepareBlock, "hasSceneSingleSidedDraws"));
  EXPECT_TRUE(contains(prepareBlock, "hasSceneWindingFlippedDraws"));
  EXPECT_TRUE(contains(prepareBlock, "hasSceneDoubleSidedDraws"));
  EXPECT_TRUE(contains(prepareBlock, "hasBimShadowGeometry(params)"));
  EXPECT_TRUE(contains(prepareBlock, "sceneSingleSidedUsesGpuCull"));
  EXPECT_FALSE(contains(prepareBlock, "return true;"));
  EXPECT_FALSE(contains(prepareBlock, "return false;"));

  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadePreparationPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadePreparationCascadeInputs"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadePreparationPlan"));
  EXPECT_TRUE(contains(planner, "!inputs.shadowAtlasVisible"));
  EXPECT_TRUE(contains(planner, "cascade.shadowPassActive"));
  EXPECT_TRUE(contains(planner, "cascade.shadowPassRecordable"));
  EXPECT_TRUE(contains(planner, "inputs.hasSceneWindingFlippedDraws"));
  EXPECT_TRUE(contains(planner, "inputs.hasSceneDoubleSidedDraws"));
  EXPECT_TRUE(contains(planner, "inputs.hasSceneSingleSidedDraws"));
  EXPECT_TRUE(contains(planner, "cascade.sceneSingleSidedUsesGpuCull"));
  EXPECT_TRUE(contains(planner, "inputs.hasBimShadowGeometry"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "VkCommandBuffer"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "ShadowCullManager"));
  EXPECT_FALSE(contains(planner, "RenderGraph"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(contains(srcCmake,
                       "renderer/shadow/ShadowCascadePreparationPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cascade_preparation_planner_tests"));
}

TEST(RenderingConventionTests, ShadowCascadeGpuCullReadinessUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCascadeGpuCullPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowCascadeGpuCullPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t recordStart = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::prepareFrame");
  ASSERT_NE(recordStart, std::string::npos);
  const size_t sourceUploadEnd = shadowFramePassRecorder.find(
      "if (shouldPrepareDrawCommands(params, context))", recordStart);
  ASSERT_NE(sourceUploadEnd, std::string::npos);
  const std::string sourceUploadBlock = shadowFramePassRecorder.substr(
      recordStart, sourceUploadEnd - recordStart);
  const size_t gpuCullDecision = shadowFramePassRecorder.find(
      "bool ShadowCascadeFramePassRecorder::useGpuCullForCascade");
  ASSERT_NE(gpuCullDecision, std::string::npos);
  const size_t gpuCullDecisionEnd = shadowFramePassRecorder.find(
      "size_t ShadowCascadeFramePassRecorder::cpuCommandCount",
      gpuCullDecision);
  ASSERT_NE(gpuCullDecisionEnd, std::string::npos);
  const std::string gpuCullBlock = shadowFramePassRecorder.substr(
      gpuCullDecision, gpuCullDecisionEnd - gpuCullDecision);

  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "ShadowCascadeGpuCullPlanner.h"));
  EXPECT_TRUE(
      contains(sourceUploadBlock, "buildShadowGpuCullSourceUploadPlan"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".shadowAtlasVisible"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".gpuShadowCullEnabled"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".shadowCullManagerReady"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".sourceDrawCommandsPresent"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".sourceDrawCount"));
  EXPECT_TRUE(contains(sourceUploadBlock, ".sourceDrawCommandsAllSingleInstance"));
  EXPECT_TRUE(contains(sourceUploadBlock,
                       "shadowGpuCullSourceUploadPlan.requiredDrawCapacity"));
  EXPECT_TRUE(contains(sourceUploadBlock,
                       "shadowGpuCullSource.contentRevision"));
  EXPECT_TRUE(contains(sourceUploadBlock,
                       "gpuCullSourceSummary(params)"));
  EXPECT_FALSE(contains(
      sourceUploadBlock,
      "displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_)) &&"));
  EXPECT_FALSE(contains(sourceUploadBlock,
                        "p.draws.opaqueSingleSidedDrawCommands->size()"));
  EXPECT_TRUE(contains(gpuCullBlock, "buildShadowCascadeGpuCullPlan"));
  EXPECT_TRUE(contains(gpuCullBlock, ".gpuShadowCullEnabled"));
  EXPECT_TRUE(contains(gpuCullBlock, ".shadowCullPassActive"));
  EXPECT_TRUE(contains(gpuCullBlock, ".shadowCullManagerReady"));
  EXPECT_TRUE(contains(gpuCullBlock, ".shadowCullDispatchReady"));
  EXPECT_TRUE(contains(gpuCullBlock, ".sceneSingleSidedDrawsAvailable"));
  EXPECT_TRUE(contains(gpuCullBlock, ".sourceDrawCommandsAllSingleInstance"));
  EXPECT_TRUE(contains(gpuCullBlock, ".cascadeIndexInRange"));
  EXPECT_TRUE(contains(gpuCullBlock, ".indirectDrawBuffer"));
  EXPECT_TRUE(contains(gpuCullBlock, ".drawCountBuffer"));
  EXPECT_TRUE(contains(gpuCullBlock, ".maxDrawCount"));
  EXPECT_TRUE(contains(gpuCullBlock, "canDispatchCascadeCull"));
  EXPECT_FALSE(contains(gpuCullBlock, "&& gpuShadowMaxDrawCount > 0"));
  EXPECT_FALSE(contains(gpuCullBlock, "return p.shadows.useGpuShadowCull"));

  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadeGpuCullPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadeGpuCullPlan"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowGpuCullSourceUploadPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowGpuCullSourceUploadPlan"));
  EXPECT_TRUE(contains(planner, "inputs.gpuShadowCullEnabled"));
  EXPECT_TRUE(contains(planner, "inputs.shadowCullPassActive"));
  EXPECT_TRUE(contains(planner, "inputs.shadowCullManagerReady"));
  EXPECT_TRUE(contains(planner, "inputs.shadowCullDispatchReady"));
  EXPECT_TRUE(contains(planner, "inputs.sceneSingleSidedDrawsAvailable"));
  EXPECT_TRUE(contains(planner, "inputs.cascadeIndexInRange"));
  EXPECT_TRUE(contains(planner, "inputs.indirectDrawBuffer"));
  EXPECT_TRUE(contains(planner, "inputs.drawCountBuffer"));
  EXPECT_TRUE(contains(planner, "inputs.maxDrawCount > 0u"));
  EXPECT_TRUE(contains(planner, "buildShadowGpuCullSourceUploadPlan"));
  EXPECT_TRUE(contains(planner, "inputs.shadowAtlasVisible"));
  EXPECT_TRUE(contains(planner, "inputs.sourceDrawCommandsPresent"));
  EXPECT_TRUE(contains(planner, "inputs.sourceDrawCommandsAllSingleInstance"));
  EXPECT_TRUE(contains(planner, "inputs.sourceDrawCount"));
  EXPECT_TRUE(contains(planner, "inputs.sourceDrawCount > 0u"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "ShadowCullManager"));
  EXPECT_FALSE(contains(planner, "RenderGraph"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowCascadeGpuCullPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cascade_gpu_cull_planner_tests"));
}

TEST(RenderingConventionTests, ShadowCullPassGraphUsesPlannerAndRecorder) {
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/deferred/DeferredRasterTechnique.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCullPassPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowCullPassPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCullPassRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/shadow/ShadowCullPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t shadowCullStart =
      deferredRasterTechnique.find("const auto shadowCullIds");
  ASSERT_NE(shadowCullStart, std::string::npos);
  const size_t shadowCullEnd = deferredRasterTechnique.find(
      "RenderPassId::DepthToReadOnly", shadowCullStart);
  ASSERT_NE(shadowCullEnd, std::string::npos);
  const std::string shadowCullBlock = deferredRasterTechnique.substr(
      shadowCullStart, shadowCullEnd - shadowCullStart);

  const size_t readinessStart =
      deferredRasterTechnique.find("shadowCullIds.size()");
  ASSERT_NE(readinessStart, std::string::npos);
  const size_t readinessEnd =
      deferredRasterTechnique.find("shadowPassIds.size()", readinessStart);
  ASSERT_NE(readinessEnd, std::string::npos);
  const std::string readinessBlock = deferredRasterTechnique.substr(
      readinessStart, readinessEnd - readinessStart);

  EXPECT_TRUE(contains(deferredRasterTechnique, "ShadowCullPassPlanner.h"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "ShadowCullPassRecorder.h"));
  EXPECT_TRUE(contains(shadowCullBlock, "buildShadowCullPassPlan"));
  EXPECT_TRUE(contains(shadowCullBlock, "recordShadowCullPassCommands"));
  EXPECT_TRUE(contains(shadowCullBlock, ".shadowAtlasVisible"));
  EXPECT_TRUE(contains(shadowCullBlock, ".gpuShadowCullEnabled"));
  EXPECT_TRUE(contains(shadowCullBlock, ".shadowCullManagerReady"));
  EXPECT_TRUE(contains(shadowCullBlock, ".sceneSingleSidedDrawsAvailable"));
  EXPECT_TRUE(
      contains(shadowCullBlock, ".sourceDrawCommandsAllSingleInstance"));
  EXPECT_TRUE(contains(shadowCullBlock, ".cameraBufferReady"));
  EXPECT_TRUE(contains(shadowCullBlock, ".cascadeIndexInRange"));
  EXPECT_TRUE(contains(shadowCullBlock, ".sourceDrawCount"));
  EXPECT_TRUE(contains(readinessBlock, "buildShadowCullPassPlan"));
  EXPECT_TRUE(
      contains(readinessBlock, ".sourceDrawCommandsAllSingleInstance"));
  EXPECT_TRUE(contains(readinessBlock, ".readiness"));
  EXPECT_FALSE(contains(deferredRasterTechnique, "dispatchCascadeCull("));
  EXPECT_FALSE(contains(readinessBlock, "renderPassNotNeeded()"));
  EXPECT_FALSE(
      contains(readinessBlock,
               "renderPassMissingResource(RenderResourceId::CameraBuffer)"));

  EXPECT_TRUE(contains(plannerHeader, "ShadowCullPassPlanInputs"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCullPassPlan"));
  EXPECT_TRUE(contains(planner, "buildShadowCullPassPlan"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::NotNeeded"));
  EXPECT_TRUE(contains(planner, "RenderPassSkipReason::MissingResource"));
  EXPECT_TRUE(contains(planner, "RenderResourceId::CameraBuffer"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "GuiManager"));
  EXPECT_FALSE(contains(planner, "RenderGraph"));
  EXPECT_FALSE(contains(planner, "ShadowCullManager"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_TRUE(contains(recorderHeader, "ShadowCullPassRecordInputs"));
  EXPECT_TRUE(contains(recorder, "recordShadowCullPassCommands"));
  EXPECT_TRUE(contains(recorder, "dispatchCascadeCull("));
  EXPECT_FALSE(contains(recorder, "FrameRecorder"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "RenderGraph"));
  EXPECT_TRUE(contains(srcCmake, "renderer/shadow/ShadowCullPassPlanner.cpp"));
  EXPECT_TRUE(contains(srcCmake, "renderer/shadow/ShadowCullPassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cull_pass_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cull_pass_recorder_tests"));
}

TEST(RenderingConventionTests, ShadowCascadeDrawPlanningUsesPlanner) {
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string shadowFramePassRecorderHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCascadeFramePassRecorder.h");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCascadeDrawPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowCascadeDrawPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t prepareCascade = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::prepareDrawCommands");
  ASSERT_NE(prepareCascade, std::string::npos);
  const size_t prepareCascadeEnd = shadowFramePassRecorder.find(
      "void ShadowCascadeFramePassRecorder::clearDrawCommandCache",
      prepareCascade);
  ASSERT_NE(prepareCascadeEnd, std::string::npos);
  const std::string prepareBlock = shadowFramePassRecorder.substr(
      prepareCascade, prepareCascadeEnd - prepareCascade);

  EXPECT_TRUE(contains(shadowFramePassRecorderHeader,
                       "ShadowCascadeDrawPlannerInputs"));
  EXPECT_TRUE(contains(shadowFramePassRecorder, "drawPlannerInputs"));
  EXPECT_TRUE(contains(prepareBlock, "ShadowCascadeDrawPlanner planner"));
  EXPECT_TRUE(contains(prepareBlock, "drawPlanCache_ = planner.build()"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "drawPlanCache_.sceneSingleSided"));
  EXPECT_FALSE(contains(prepareBlock, "cascadeIntersectsSphere"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadeDrawPlanner"));
  EXPECT_TRUE(contains(planner, "appendVisibleRun"));
  EXPECT_TRUE(contains(planner, "cpuFallbackAllowed"));
  EXPECT_TRUE(contains(planner, "sceneSingleSidedUsesGpuCull"));
  EXPECT_EQ(planner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowCascadeDrawPlanner.cpp"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowCascadeFramePassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cascade_draw_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cascade_frame_pass_recorder_tests"));
}

TEST(RenderingConventionTests, ShadowPassDrawPlanningUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassDrawPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowPassDrawPlanner.cpp");
  const std::string recorderHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPassRecorder.h");
  const std::string recorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t shadowBodyStart = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::cascadePassRecordInputs");
  ASSERT_NE(shadowBodyStart, std::string::npos);
  const size_t shadowBodyEnd = shadowFramePassRecorder.find(
      "ShadowCascadeFramePassRecorder::secondaryPassRecordInputs",
      shadowBodyStart);
  ASSERT_NE(shadowBodyEnd, std::string::npos);
  const std::string shadowBody = shadowFramePassRecorder.substr(
      shadowBodyStart, shadowBodyEnd - shadowBodyStart);

  EXPECT_TRUE(contains(shadowFramePassRecorder, "ShadowPassRecorder.h"));
  EXPECT_FALSE(contains(frameRecorder, "FrameRecorder::recordShadowPassBody"));
  EXPECT_FALSE(contains(shadowBody, "buildShadowPassDrawPlan"));
  EXPECT_FALSE(contains(shadowBody, "recordShadowPassCommands"));
  EXPECT_TRUE(contains(recorder, "recordShadowCascadePassBodyCommands"));
  EXPECT_TRUE(contains(recorder, "buildShadowPassDrawPlan"));
  EXPECT_TRUE(contains(recorder, "recordShadowPassCommands"));
  EXPECT_TRUE(contains(recorder, "recordShadowCascadePassCommands"));
  EXPECT_TRUE(contains(shadowBody, "shadowPassGeometryBinding"));
  EXPECT_TRUE(contains(shadowBody, "shadowPassGpuIndirectBuffers"));
  EXPECT_FALSE(contains(shadowBody, "shadowPassPlan.sceneGpuRoute.active"));
  EXPECT_FALSE(contains(shadowBody, "shadowPassPlan.sceneCpuRouteCount"));
  EXPECT_FALSE(contains(shadowBody, "shadowPassPlan.bimGpuRouteCount"));
  EXPECT_FALSE(contains(shadowBody, "shadowPassPlan.bimCpuRouteCount"));
  EXPECT_FALSE(contains(frameRecorder, "pipelineForShadowPassRoute"));
  EXPECT_FALSE(contains(frameRecorder, "bimDrawCompactionSlot"));
  EXPECT_FALSE(contains(frameRecorder, "drawInstanceCount"));
  EXPECT_FALSE(contains(shadowBody, "vkCmdSetDepthBias"));
  EXPECT_FALSE(contains(shadowBody, "vkCmdDrawIndexedIndirectCount"));
  EXPECT_FALSE(contains(shadowBody, "vkCmdDrawIndexed("));
  EXPECT_FALSE(contains(shadowBody, "const auto &singleSidedCommands"));
  EXPECT_FALSE(contains(shadowBody, "const auto &windingFlippedCommands"));
  EXPECT_FALSE(contains(shadowBody, "const auto &doubleSidedCommands"));
  EXPECT_FALSE(contains(shadowBody,
                        "drawGpuFilteredBimShadowSlot(BimDrawCompactionSlot"));

  EXPECT_TRUE(contains(plannerHeader, "ShadowPassDrawPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowPassBimGpuSlot"));
  EXPECT_TRUE(contains(planner, "sceneGpuCullActive"));
  EXPECT_TRUE(contains(planner, "bimGpuFilteredMeshActive"));
  EXPECT_TRUE(contains(planner, "OpaqueWindingFlipped"));
  EXPECT_FALSE(contains(planner, "vkCmd"));
  EXPECT_FALSE(contains(planner, "VkPipeline"));
  EXPECT_FALSE(contains(planner, "FrameRecordParams"));
  EXPECT_FALSE(contains(planner, "BimManager"));
  EXPECT_FALSE(contains(planner, "DebugOverlayRenderer"));
  EXPECT_FALSE(contains(planner, "ShadowCullManager"));
  EXPECT_TRUE(contains(srcCmake, "renderer/shadow/ShadowPassDrawPlanner.cpp"));
  EXPECT_TRUE(contains(recorderHeader, "ShadowPassRecordInputs"));
  EXPECT_TRUE(contains(recorderHeader, "ShadowPassPipelineHandles"));
  EXPECT_TRUE(contains(recorderHeader, "ShadowPassGpuIndirectBuffers"));
  EXPECT_TRUE(contains(recorder, "pipelineForShadowPassRoute"));
  EXPECT_TRUE(contains(recorder, "bimDrawCompactionSlot"));
  EXPECT_TRUE(contains(recorder, "recordShadowPassCommands"));
  EXPECT_TRUE(contains(recorder, "vkCmdSetDepthBias"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindVertexBuffers"));
  EXPECT_TRUE(contains(recorder, "vkCmdBindIndexBuffer"));
  EXPECT_TRUE(contains(recorder, "vkCmdDrawIndexedIndirectCount"));
  EXPECT_TRUE(contains(recorder, "vkCmdDrawIndexed"));
  EXPECT_TRUE(contains(recorder, "bimManager->drawCompacted"));
  EXPECT_FALSE(contains(recorder, "FrameRecordParams"));
  EXPECT_FALSE(contains(recorder, "FrameSceneGeometry"));
  EXPECT_FALSE(contains(recorder, "beginDynamicRendering"));
  EXPECT_FALSE(contains(recorder, "endDynamicRendering"));
  EXPECT_FALSE(contains(recorder, "recordShadowPassBody"));
  EXPECT_TRUE(contains(srcCmake, "renderer/shadow/ShadowPassRecorder.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_pass_draw_planner_tests"));
  EXPECT_TRUE(contains(testsCmake, "shadow_pass_recorder_tests"));
}

TEST(RenderingConventionTests,
     ShadowFramePassUsesRegistryPipelineHandleBridge) {
  const std::string shadowBridge = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowPipelineBridge.h");
  const std::string shadowFramePassRecorder = readRepoTextFile(
      "src/renderer/shadow/ShadowCascadeFramePassRecorder.cpp");

  EXPECT_TRUE(contains(shadowBridge, "enum class ShadowPipelineId"));
  EXPECT_TRUE(contains(shadowBridge, "shadowPipelineHandle("));
  EXPECT_TRUE(contains(
      shadowBridge, "params.pipelineHandle(RenderTechniqueId::DeferredRaster"));
  EXPECT_TRUE(contains(shadowBridge, "enum class ShadowPipelineLayoutId"));
  EXPECT_TRUE(contains(shadowBridge, "shadowPipelineLayout("));
  EXPECT_TRUE(contains(
      shadowBridge, "params.pipelineLayout(RenderTechniqueId::DeferredRaster"));
  EXPECT_TRUE(contains(shadowBridge, "\"shadow-depth\""));
  EXPECT_TRUE(contains(shadowBridge, "\"shadow-depth-front-cull\""));
  EXPECT_TRUE(contains(shadowBridge, "\"shadow-depth-no-cull\""));
  EXPECT_TRUE(contains(shadowBridge, "\"shadow\""));
  EXPECT_FALSE(contains(shadowBridge, "fallbackShadowPipelineHandle("));
  EXPECT_FALSE(contains(shadowBridge, "fallbackShadowPipelineLayout("));
  EXPECT_FALSE(contains(shadowBridge, "params.pipeline.pipelines"));
  EXPECT_FALSE(contains(shadowBridge, "params.pipeline.layouts"));
  EXPECT_FALSE(contains(shadowBridge, "GraphicsPipelines"));
  EXPECT_FALSE(contains(shadowBridge, "PipelineLayouts"));

  EXPECT_TRUE(contains(shadowFramePassRecorder, "ShadowPipelineBridge.h"));
  EXPECT_TRUE(contains(shadowFramePassRecorder, "shadowPipelineHandle("));
  EXPECT_TRUE(contains(shadowFramePassRecorder, "shadowPipelineReady("));
  EXPECT_TRUE(contains(shadowFramePassRecorder, "shadowPipelineLayout("));
  EXPECT_FALSE(contains(shadowFramePassRecorder, "params.pipeline.pipelines."));
  EXPECT_FALSE(contains(shadowFramePassRecorder, "p.pipeline.pipelines."));
  EXPECT_FALSE(contains(shadowFramePassRecorder, "params.pipeline.layouts"));
  EXPECT_FALSE(contains(shadowFramePassRecorder, "p.pipeline.layouts"));
}

TEST(RenderingConventionTests, BimSectionCapGeneratedMeshBuildsFillAndHatches) {
  using container::renderer::BimSectionCapBuildOptions;
  using container::renderer::BimSectionCapGeneratedMesh;
  using container::renderer::BimSectionCapTriangle;
  using container::renderer::BuildBimSectionCapMesh;

  const std::array<glm::vec3, 8> vertices{{
      {-1.0f, -1.0f, -1.0f},
      {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f},
      {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f},
      {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f},
      {-1.0f, 1.0f, 1.0f},
  }};
  const std::array<std::array<uint32_t, 3>, 12> faces{{
      {{0u, 1u, 2u}},
      {{0u, 2u, 3u}},
      {{4u, 6u, 5u}},
      {{4u, 7u, 6u}},
      {{0u, 4u, 5u}},
      {{0u, 5u, 1u}},
      {{1u, 5u, 6u}},
      {{1u, 6u, 2u}},
      {{2u, 6u, 7u}},
      {{2u, 7u, 3u}},
      {{3u, 7u, 4u}},
      {{3u, 4u, 0u}},
  }};
  std::array<BimSectionCapTriangle, 12> triangles{};
  for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
    triangles[faceIndex] = BimSectionCapTriangle{
        .objectIndex = 17u,
        .p0 = vertices[faces[faceIndex][0]],
        .p1 = vertices[faces[faceIndex][1]],
        .p2 = vertices[faces[faceIndex][2]],
    };
  }
  BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.2f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  options.crossHatch = true;

  const BimSectionCapGeneratedMesh mesh =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(mesh.valid());
  ASSERT_FALSE(mesh.fillDrawCommands.empty());
  ASSERT_FALSE(mesh.hatchDrawCommands.empty());
  EXPECT_EQ(mesh.fillDrawCommands[0].objectIndex, 17u);
  EXPECT_EQ(mesh.hatchDrawCommands[0].objectIndex, 17u);
  EXPECT_EQ(mesh.fillDrawCommands[0].firstIndex % 3u, 0u);
  EXPECT_EQ(mesh.fillDrawCommands[0].indexCount % 3u, 0u);
  EXPECT_EQ(mesh.hatchDrawCommands[0].indexCount % 2u, 0u);
  for (const container::geometry::Vertex &vertex : mesh.vertices) {
    EXPECT_NEAR(vertex.position.y, 0.0f, 1.0e-5f);
  }
}

TEST(RenderingConventionTests, BimSectionClipCapsExposeShaderStyleContract) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/core/FrameRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string sectionClipCapPlanner =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassPlanner.cpp");
  const std::string sectionClipCapRecorder =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassRecorder.cpp");
  const std::string sectionCapHeader =
      readRepoTextFile("include/Container/renderer/bim/BimSectionCapBuilder.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/pipeline/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string sceneClipCommon =
      readRepoTextFile("shaders/scene_clip_common.slang");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t sectionCapStart =
      lightingPassRecorder.find("recordBimSectionClipCapFramePassCommands");
  ASSERT_NE(sectionCapStart, std::string::npos);
  const size_t sectionCapEnd = lightingPassRecorder.find(
      "recordBimPrimitiveFramePassCommands", sectionCapStart);
  ASSERT_NE(sectionCapEnd, std::string::npos);
  const std::string sectionCapBlock = lightingPassRecorder.substr(
      sectionCapStart, sectionCapEnd - sectionCapStart);

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionBoxClipState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionClipCapStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionClipCapHatchMode"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchSpacing"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchLineWidth"));
  EXPECT_TRUE(contains(frameRecorderHeader, "fillColor"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchColor"));
  EXPECT_TRUE(contains(frameRecorderHeader, "std::array<glm::vec4, 6> planes"));
  EXPECT_TRUE(contains(frameRecorderHeader, "sectionClipCaps"));
  EXPECT_FALSE(contains(frameRecorderHeader, "recordBimSectionClipCapPass"));

  EXPECT_FALSE(contains(frameRecorder, "BimSectionClipCapPassPlanner.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "BimSectionClipCapPassRecorder.h"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "bimSectionClipCapFramePassStyle"));
  EXPECT_FALSE(contains(frameRecorder, "bimSectionClipCapPassInputs"));
  EXPECT_FALSE(contains(frameRecorder, "buildBimSectionClipCapPassPlan"));
  EXPECT_TRUE(
      contains(sectionCapBlock, "recordBimSectionClipCapFramePassCommands"));
  EXPECT_FALSE(contains(sectionCapBlock, "pipelineForBimSectionClipCapPass"));
  EXPECT_FALSE(contains(sectionCapBlock, "vkCmdBindDescriptorSets"));
  EXPECT_FALSE(contains(sectionCapBlock, "vkCmdSetLineWidth"));
  EXPECT_FALSE(contains(sectionCapBlock, "debugOverlay_.drawWireframe"));
  EXPECT_FALSE(contains(frameRecorder, "sectionClipCapStyleActive"));
  EXPECT_FALSE(contains(frameRecorder, "recordBimSectionClipCapPass(cmd, p)"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimSectionClipCapFill"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "DeferredRasterPipelineId::BimSectionClipCapHatch"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "sectionClipCapGeometry.fillDrawCommands"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "sectionClipCapGeometry.hatchDrawCommands"));
  EXPECT_TRUE(
      contains(sectionClipCapRecorder, "pipelineForBimSectionClipCapPass"));
  EXPECT_TRUE(
      contains(sectionClipCapRecorder, "buildBimSectionClipCapPassPlan"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "vkCmdBindDescriptorSets"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "vkCmdSetLineWidth"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "sectionPlaneEnabled = 0u"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "debugOverlay.drawWireframe"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "drawStyledRouteCommands"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "style.fillColor"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "style.hatchColor"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "style.lineWidth"));
  const size_t drawStyleStart =
      sectionCapHeader.find("struct BimSectionCapDrawStyle");
  ASSERT_NE(drawStyleStart, std::string::npos);
  const size_t drawStyleEnd =
      sectionCapHeader.find("struct BimSectionMarkerLine", drawStyleStart);
  ASSERT_NE(drawStyleEnd, std::string::npos);
  const std::string drawStyleBlock =
      sectionCapHeader.substr(drawStyleStart, drawStyleEnd - drawStyleStart);
  EXPECT_TRUE(contains(drawStyleBlock, "float lineWidth"));
  EXPECT_TRUE(contains(sectionClipCapRecorder, "markerCommandsOnly"));
  EXPECT_TRUE(contains(sectionClipCapPlanner, "BimSectionClipCapPassPlanner"));
  EXPECT_TRUE(contains(sectionClipCapPlanner, "Fill"));
  EXPECT_TRUE(contains(sectionClipCapPlanner, "Hatch"));
  EXPECT_TRUE(
      contains(sectionClipCapPlanner, "rasterBimSectionClipCapLineWidth"));
  EXPECT_EQ(sectionClipCapPlanner.find("vkCmd"), std::string::npos);
  EXPECT_EQ(sectionClipCapPlanner.find("VkPipeline"), std::string::npos);
  EXPECT_EQ(sectionClipCapPlanner.find("FrameRecordParams"), std::string::npos);
  EXPECT_EQ(sectionClipCapPlanner.find("DebugOverlayRenderer"),
            std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimSectionClipCapPassRecorder.cpp"));
  EXPECT_TRUE(contains(pipelineTypes, "bimSectionClipCapFill"));
  EXPECT_TRUE(contains(pipelineTypes, "bimSectionClipCapHatch"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_section_clip_cap_fill_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_section_clip_cap_hatch_pipeline"));
  EXPECT_TRUE(contains(rendererFrontend, "bimClipCapHatchingUiState()"));
  EXPECT_TRUE(contains(rendererFrontend, "makeBoxClipPlanes"));
  EXPECT_TRUE(contains(rendererFrontend, "updateSceneClipState"));
  EXPECT_TRUE(contains(rendererFrontend, "appendBimSectionCapClipPlane"));
  EXPECT_TRUE(contains(rendererFrontend, "invertedBoxClipCapsUnsupported"));
  EXPECT_TRUE(contains(rendererFrontend, "rebuildSectionClipCapGeometry"));
  EXPECT_TRUE(contains(rendererFrontend, "sectionClipCapDrawData()"));
  const size_t capStyleEnabledStart =
      rendererFrontend.find("const bool capStyleEnabled");
  ASSERT_NE(capStyleEnabledStart, std::string::npos);
  const size_t capStyleEnabledEnd =
      rendererFrontend.find("p.bim.sectionClipCaps.enabled",
                            capStyleEnabledStart);
  ASSERT_NE(capStyleEnabledEnd, std::string::npos);
  const std::string capStyleEnabledBlock = rendererFrontend.substr(
      capStyleEnabledStart, capStyleEnabledEnd - capStyleEnabledStart);
  EXPECT_TRUE(contains(capStyleEnabledBlock, "capUi.sectionMarkersPreview"));
  EXPECT_TRUE(contains(bimManager, "sameSectionCapBuildOptions"));
  EXPECT_TRUE(contains(bimManager, "sectionClipCapBuildOptionsValid_"));
  EXPECT_TRUE(contains(bimManager, "appendSectionMarkerGeometry"));
  EXPECT_TRUE(contains(bimManager,
                       "sectionClipCapDrawData_.hatchDrawCommands.push_back"));
  EXPECT_TRUE(contains(bimManager, ".lineWidth = marker.lineWidth"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimClipCapHatchingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimBoxClipUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimBoxClipState()"));
  EXPECT_TRUE(contains(guiManager, "Box clip"));
  EXPECT_TRUE(contains(guiManager, "Invert box clip"));
  EXPECT_TRUE(contains(sceneData, "struct SceneClipState"));
  EXPECT_TRUE(contains(sceneData, "boxClipPlanes[6]"));
  EXPECT_TRUE(contains(sceneManagerHeader, "updateSceneClipState"));
  EXPECT_TRUE(contains(sceneManager, "createSceneClipStateBuffer"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 6"));
  EXPECT_TRUE(contains(sceneManager, "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"));

  EXPECT_TRUE(contains(pushConstants, "struct BoxClipState"));
  EXPECT_TRUE(contains(pushConstants, "struct SectionClipCapStyle"));
  EXPECT_TRUE(contains(pushConstants, "SECTION_CLIP_CAP_HATCH_DIAGONAL"));
  EXPECT_TRUE(contains(pushConstants, "SECTION_CLIP_CAP_HATCH_CROSS"));
  EXPECT_TRUE(contains(pushConstants, "bool BoxClipPlanesClip"));
  EXPECT_TRUE(contains(pushConstants, "float SectionClipCapHatchMask"));
  EXPECT_TRUE(contains(pushConstants, "float4 fillColor"));
  EXPECT_TRUE(contains(pushConstants, "float4 hatchColor"));
  EXPECT_TRUE(contains(sceneClipCommon, "[[vk::binding(6, 0)]]"));
  EXPECT_TRUE(contains(sceneClipCommon, "SceneClipPlanesClip"));
  EXPECT_TRUE(contains(sceneClipCommon, "BoxClipPlanesClip"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimSectionClipCapPassPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_section_clip_cap_pass_planner_tests"));
}

TEST(RenderingConventionTests, BimElevationViewUiReusesCameraAndRenderState) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string cameraControllerHeader =
      readRepoTextFile("include/Container/renderer/scene/CameraController.h");
  const std::string cameraController =
      readRepoTextFile("src/renderer/scene/CameraController.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(
      contains(guiManagerHeader, "enum class BimElevationTechnicalStyle"));
  EXPECT_TRUE(contains(guiManagerHeader, "struct BimElevationViewState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimElevationViewState()"));
  EXPECT_TRUE(contains(guiManagerHeader, "consumeBimElevationViewRequest()"));

  EXPECT_TRUE(contains(guiManager, "BIM Elevation View"));
  EXPECT_TRUE(contains(guiManager, "Front elevation"));
  EXPECT_TRUE(contains(guiManager, "Back elevation"));
  EXPECT_TRUE(contains(guiManager, "Left elevation"));
  EXPECT_TRUE(contains(guiManager, "Right elevation"));
  EXPECT_TRUE(contains(guiManager, "Shaded"));
  EXPECT_TRUE(contains(guiManager, "Technical"));
  EXPECT_TRUE(contains(guiManager, "Hidden line"));
  EXPECT_TRUE(contains(guiManager, "applyBimElevationDisplayIntent"));
  EXPECT_TRUE(
      contains(guiManager, "wireframeSettings_.mode = WireframeMode::Full"));
  EXPECT_TRUE(
      contains(guiManager, "wireframeSettings_.mode = WireframeMode::Overlay"));

  EXPECT_TRUE(contains(cameraControllerHeader, "setBimElevationView"));
  EXPECT_TRUE(
      contains(cameraController, "CameraController::setBimElevationView"));
  EXPECT_TRUE(contains(cameraController, "setOrthographic(nodeIndex, true)"));
  EXPECT_TRUE(contains(cameraController, "setViewPreset(nodeIndex, preset)"));

  EXPECT_TRUE(contains(rendererFrontend, "consumeBimElevationViewRequest"));
  EXPECT_TRUE(contains(rendererFrontend, "setBimElevationView"));
  EXPECT_TRUE(contains(rendererFrontend, "uploadCameraBuffers()"));
}

TEST(RenderingConventionTests, BimTechnicalElevationRendererComposesPasses) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string deferredLightingHeader = readRepoTextFile(
      "include/Container/renderer/deferred/DeferredRasterLighting.h");
  const std::string deferredLighting =
      readRepoTextFile("src/renderer/deferred/DeferredRasterLighting.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimTechnicalElevationState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "technicalElevation"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hiddenLineOverlay"));
  EXPECT_TRUE(contains(frameRecorderHeader, "sectionCapsEnabled"));

  EXPECT_TRUE(contains(deferredLightingHeader,
                       "DeferredLightingBimTechnicalElevationSettings"));
  EXPECT_TRUE(contains(deferredLightingHeader, "bimTechnicalElevationEnabled"));
  EXPECT_TRUE(
      contains(deferredLighting, "applyBimTechnicalElevationHiddenLineStyle"));
  EXPECT_TRUE(
      contains(deferredLighting, "state.wireframeSettings.depthTest = true"));
  EXPECT_TRUE(
      contains(deferredLighting, "DeferredLightingWireframeMode::Overlay"));

  EXPECT_TRUE(contains(lightingPassRecorder, "p.bim.technicalElevation"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "technicalElevation.sectionCapsEnabled"));
  EXPECT_TRUE(
      contains(lightingPassRecorder, "technicalElevation.capHatchingEnabled"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "recordDeferredDebugOverlayWireframeOverlayCommands"));
  EXPECT_TRUE(contains(lightingPassRecorder,
                       "recordBimSectionClipCapFramePassCommands"));
  EXPECT_TRUE(
      contains(rendererFrontend, "BimElevationTechnicalStyle::HiddenLine"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.bim.technicalElevation.depthTestLines"));
  EXPECT_TRUE(contains(rendererFrontend, "technicalCapsEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "capUi.capPreview"));
  EXPECT_TRUE(contains(rendererFrontend, "capUi.hatchingPreview"));
  EXPECT_TRUE(contains(rendererFrontend, "capUi.sectionMarkersPreview"));
}

TEST(RenderingConventionTests, BimMetadataCatalogOwnsSemanticCatalogState) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string catalogHeader =
      readRepoTextFile("include/Container/renderer/bim/BimMetadataCatalog.h");
  const std::string catalog =
      readRepoTextFile("src/renderer/bim/BimMetadataCatalog.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  EXPECT_TRUE(contains(bimManagerHeader, "BimMetadataCatalog"));
  EXPECT_TRUE(contains(bimManagerHeader, "metadataCatalog_"));
  EXPECT_EQ(bimManagerHeader.find("elementTypes_"), std::string::npos);
  EXPECT_EQ(bimManagerHeader.find("elementStoreys_"), std::string::npos);
  EXPECT_EQ(bimManagerHeader.find("elementMaterials_"), std::string::npos);
  EXPECT_EQ(bimManagerHeader.find("modelUnitMetadata_"), std::string::npos);
  EXPECT_EQ(bimManagerHeader.find("modelGeoreferenceMetadata_"),
            std::string::npos);

  EXPECT_TRUE(contains(catalogHeader, "class BimMetadataCatalog"));
  EXPECT_TRUE(contains(catalogHeader, "visibilityGpuMetadata"));
  EXPECT_TRUE(contains(catalog, "BimMetadataCatalog::registerStorey"));
  EXPECT_TRUE(contains(catalog, "BimMetadataCatalog::semanticIdForMetadata"));
  EXPECT_TRUE(contains(catalog, "BimMetadataCatalog::visibilityGpuMetadata"));

  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->registerType"));
  EXPECT_TRUE(contains(bimManager, "metadataCatalog_->visibilityGpuMetadata"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimMetadataCatalog.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_metadata_catalog_tests"));
}

TEST(RenderingConventionTests, BimFramePassRecordingFacadesAreBimOwned) {
  const std::string surfaceHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfaceRasterPassRecorder.h");
  const std::string surfaceRecorder =
      readRepoTextFile("src/renderer/bim/BimSurfaceRasterPassRecorder.cpp");
  const std::string primitiveHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimPrimitivePassRecorder.h");
  const std::string primitiveRecorder =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassRecorder.cpp");
  const std::string sectionHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSectionClipCapPassRecorder.h");
  const std::string sectionRecorder =
      readRepoTextFile("src/renderer/bim/BimSectionClipCapPassRecorder.cpp");
  const std::string lightingHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayRecorder.h");
  const std::string lightingRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");

  EXPECT_TRUE(contains(surfaceHeader, "BimSurfaceFramePassRecordInputs"));
  EXPECT_TRUE(contains(surfaceHeader, "buildBimSurfaceFramePassInputs"));
  EXPECT_TRUE(contains(surfaceHeader, "buildBimSurfaceFramePassPlan"));
  EXPECT_TRUE(contains(surfaceHeader, "recordBimSurfaceFramePassCommands"));
  EXPECT_TRUE(contains(surfaceRecorder, "buildBimSurfaceFramePassPlan"));
  EXPECT_TRUE(contains(surfaceRecorder, "buildBimSurfacePassPlan("));
  EXPECT_TRUE(contains(surfaceRecorder, "buildBimSurfaceFramePassInputs"));
  EXPECT_TRUE(contains(surfaceRecorder, "recordBimSurfaceRasterPassCommands"));

  EXPECT_TRUE(contains(primitiveHeader, "BimPrimitiveFramePassRecordInputs"));
  EXPECT_TRUE(
      contains(primitiveHeader, "buildBimPrimitiveFramePassPlanInputs"));
  EXPECT_TRUE(contains(primitiveHeader, "recordBimPrimitiveFramePassCommands"));
  EXPECT_TRUE(contains(primitiveRecorder, "buildBimPrimitivePassPlan("));
  EXPECT_TRUE(
      contains(primitiveRecorder, "buildBimPrimitiveFramePassPlanInputs"));
  EXPECT_TRUE(contains(primitiveRecorder, "recordBimPrimitivePassCommands"));

  EXPECT_TRUE(
      contains(sectionHeader, "BimSectionClipCapFramePassRecordInputs"));
  EXPECT_TRUE(
      contains(sectionHeader, "buildBimSectionClipCapFramePassPlanInputs"));
  EXPECT_TRUE(
      contains(sectionHeader, "recordBimSectionClipCapFramePassCommands"));
  EXPECT_TRUE(contains(sectionRecorder, "buildBimSectionClipCapPassPlan("));
  EXPECT_TRUE(
      contains(sectionRecorder, "buildBimSectionClipCapFramePassPlanInputs"));
  EXPECT_TRUE(contains(sectionRecorder, "recordBimSectionClipCapPassCommands"));

  EXPECT_TRUE(contains(lightingHeader, "BimLightingOverlayFrameRecordInputs"));
  EXPECT_TRUE(contains(lightingHeader, "BimLightingOverlayFrameStyleState"));
  EXPECT_TRUE(contains(lightingHeader, "BimLightingOverlayFrameDrawSources"));
  EXPECT_TRUE(
      contains(lightingHeader, "buildBimLightingOverlayFramePlanInputs"));
  EXPECT_TRUE(contains(lightingHeader, "buildBimLightingOverlayFramePlan"));
  EXPECT_TRUE(
      contains(lightingHeader, "recordBimLightingOverlayFrameCommands"));
  EXPECT_TRUE(contains(lightingRecorder, "buildBimLightingOverlayPlan("));
  EXPECT_TRUE(contains(lightingRecorder, "recordBimLightingOverlayCommands"));

  for (const std::string *bimRecorder :
       {&surfaceHeader, &surfaceRecorder, &primitiveHeader, &primitiveRecorder,
        &sectionHeader, &sectionRecorder, &lightingHeader, &lightingRecorder}) {
    EXPECT_EQ(bimRecorder->find("FrameRecordParams"), std::string::npos);
  }
}

TEST(RenderingConventionTests, BimGenericPropertiesAndGeoreferenceReachUi) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");

  EXPECT_TRUE(contains(dotBimHeader, "ElementProperty"));
  EXPECT_TRUE(contains(dotBimHeader, "ModelGeoreferenceMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimElementProperty"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimModelGeoreferenceMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "modelGeoreferenceMetadata()"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "std::vector<BimElementProperty> properties"));
  EXPECT_TRUE(contains(bimManager, "bimElementProperties(element.properties)"));
  EXPECT_TRUE(contains(
      bimManager, "bimModelGeoreferenceMetadata(model.georeferenceMetadata)"));

  EXPECT_TRUE(contains(
      guiManagerHeader,
      "std::span<const container::renderer::BimElementProperty> properties"));
  EXPECT_TRUE(contains(guiManagerHeader, "sourceUpAxis"));
  EXPECT_TRUE(contains(guiManagerHeader, "coordinateOffset"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.properties"));
  EXPECT_TRUE(contains(rendererFrontend, "modelGeoreferenceMetadata()"));
  EXPECT_TRUE(contains(guiManager, "BimExtendedProperties"));
  EXPECT_TRUE(contains(guiManager, "Source up axis"));
  EXPECT_TRUE(contains(guiManager, "Coordinate offset"));
  EXPECT_TRUE(contains(guiManager, "CRS:"));
  EXPECT_TRUE(contains(usdLoader, "applySourceUpAxis"));
}

TEST(RenderingConventionTests, BimSectionPlaneVisualIsEditableAndRendered) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string overlayPlannerHeader =
      readRepoTextFile("include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string overlayRecorder =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayRecorder.cpp");
  const std::string lightingPassRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(guiHeader, "visualPlaneVisible"));
  EXPECT_TRUE(contains(guiHeader, "visualPlaneEditable"));
  EXPECT_TRUE(contains(guiHeader, "setSectionPlaneState"));
  EXPECT_TRUE(contains(guiManager, "Show cut plane"));
  EXPECT_TRUE(contains(guiManager, "Edit cut plane as object"));
  EXPECT_TRUE(contains(guiManager, "Cut plane origin"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimSectionPlaneVisualState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "sectionPlaneVisualScene"));
  EXPECT_TRUE(contains(frameRecorderHeader, "sectionPlaneVisualDrawCommands"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimSectionPlaneVisualDrawData"));
  EXPECT_TRUE(contains(bimManagerHeader, "rebuildSectionPlaneVisualGeometry"));
  EXPECT_TRUE(contains(bimManager, "sectionPlaneVisualGeometrySignature_"));
  EXPECT_TRUE(contains(overlayPlannerHeader, "SectionPlaneVisual ="));
  EXPECT_TRUE(contains(overlayRecorder, "sectionPlaneVisualGeometryReady"));
  EXPECT_TRUE(contains(lightingPassRecorder, "sectionPlaneVisualScene"));
  EXPECT_TRUE(contains(rendererFrontend, "rebuildSectionPlaneVisualGeometry"));
  EXPECT_TRUE(contains(rendererFrontend, "transformingSectionPlane"));
}
