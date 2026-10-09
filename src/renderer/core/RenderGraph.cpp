#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/temporal/TemporalManager.h"

#include "Container/renderer/core/FrameRecorder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {

namespace {

constexpr uint32_t kMissingPassIndex = std::numeric_limits<uint32_t>::max();

constexpr bool isValidResource(RenderResourceId id) {
  return static_cast<size_t>(id) < kRenderResourceIdCount;
}

constexpr std::array<std::string_view, kRenderPassIdCount> kRenderPassNames = {
    "FrustumCull",
    "DepthPrepass",
    "BimDepthPrepass",
    "HiZGenerate",
    "OcclusionCull",
    "CullStatsReadback",
    "GBuffer",
    "BimGBuffer",
    "TransparentPick",
    "OitClear",
    "ShadowCullCascade0",
    "ShadowCullCascade1",
    "ShadowCullCascade2",
    "ShadowCullCascade3",
    "ShadowCascade0",
    "ShadowCascade1",
    "ShadowCascade2",
    "ShadowCascade3",
    "LocalShadowDepth",
    "DepthToReadOnly",
    "TileCull",
    "GTAO",
    "Lighting",
    "TransformGizmos",
    "ExposureAdaptation",
    "OitResolve",
    "Bloom",
    "PostProcess",
    "TemporalVelocity",
    "TemporalResolve",
    "RaySceneBuild",
    "RayShadowTrace",
    "RayShadowFilter",
};

static_assert(kRenderPassNames.size() == kRenderPassIdCount);

constexpr std::array<std::string_view, kRenderResourceIdCount>
    kRenderResourceNames = {
        "SceneGeometry",
        "BimGeometry",
        "CameraBuffer",
        "ObjectBuffer",
        "BimObjectBuffer",
        "LightingData",
        "ShadowData",
        "EnvironmentMaps",
        "FrustumCullDraws",
        "HiZPyramid",
        "OcclusionCullDraws",
        "CullStats",
        "SceneDepth",
        "GBufferAlbedo",
        "GBufferNormal",
        "GBufferMaterial",
        "GBufferEmissive",
        "GBufferSpecular",
        "GBufferMaterialLayers",
        "GBufferMaterialSheen",
        "PickId",
        "PickDepth",
        "OitStorage",
        "ShadowCullCascade0",
        "ShadowCullCascade1",
        "ShadowCullCascade2",
        "ShadowCullCascade3",
        "ShadowAtlas",
        "LocalShadowData",
        "LocalShadowAtlas",
        "TileLightGrid",
        "AmbientOcclusion",
        "SceneColor",
        "ExposureState",
        "BloomTexture",
        "SwapchainImage",
        "TemporalMotion",
        "TemporalColor",
        "RayScene",
        "RayShadowRaw",
        "RayShadowVisibility",
};

static_assert(kRenderResourceNames.size() == kRenderResourceIdCount);

constexpr std::array<RenderPassId, 0> kNoDependencies{};
constexpr std::array<RenderResourceId, 0> kNoResources{};
constexpr std::array<RenderResourceTransition, 0> kNoTransitions{};
constexpr std::array kHiZDependencies{RenderPassId::DepthPrepass};
constexpr std::array kOcclusionDependencies{
    RenderPassId::FrustumCull,
    RenderPassId::HiZGenerate,
};
constexpr std::array kCullStatsDependencies{
    RenderPassId::FrustumCull,
    RenderPassId::OcclusionCull,
};
constexpr std::array kShadowCullPassIds{
    RenderPassId::ShadowCullCascade0,
    RenderPassId::ShadowCullCascade1,
    RenderPassId::ShadowCullCascade2,
    RenderPassId::ShadowCullCascade3,
};
constexpr std::array kShadowCascadePassIds{
    RenderPassId::ShadowCascade0,
    RenderPassId::ShadowCascade1,
    RenderPassId::ShadowCascade2,
    RenderPassId::ShadowCascade3,
};
constexpr std::array kShadowCullDependencies{RenderPassId::FrustumCull};
constexpr std::array kTileCullDependencies{RenderPassId::DepthToReadOnly};
constexpr std::array kGtaoDependencies{
    RenderPassId::DepthToReadOnly,
    RenderPassId::GBuffer,
};
constexpr std::array kBimGBufferDependencies{
    RenderPassId::BimDepthPrepass,
};
constexpr std::array kExposureAdaptationDependencies{
    RenderPassId::Lighting,
};
constexpr std::array kBloomDependencies{RenderPassId::Lighting};

constexpr std::array kDepthPrepassScheduleDependencies{
    RenderPassId::FrustumCull,
};
constexpr std::array kBimDepthPrepassScheduleDependencies{
    RenderPassId::DepthPrepass,
};
constexpr std::array kGBufferScheduleDependencies{
    RenderPassId::DepthPrepass,
    RenderPassId::CullStatsReadback,
};
constexpr std::array kBimGBufferScheduleDependencies{
    RenderPassId::BimDepthPrepass,
    RenderPassId::GBuffer,
};
constexpr std::array kTransparentPickScheduleDependencies{
    RenderPassId::GBuffer,
};
constexpr std::array kHiZOptionalScheduleDependencies{
    RenderPassId::BimDepthPrepass,
};
constexpr std::array kGBufferOptionalScheduleDependencies{
    RenderPassId::BimDepthPrepass,
};
constexpr std::array kDepthToReadOnlyOptionalScheduleDependencies{
    RenderPassId::BimGBuffer,
    RenderPassId::LocalShadowDepth,
};
constexpr std::array kTransparentPickOptionalScheduleDependencies{
    RenderPassId::BimGBuffer,
};
constexpr std::array kOitClearScheduleDependencies{RenderPassId::GBuffer};
constexpr std::array kShadowCascade0ScheduleDependencies{
    RenderPassId::ShadowCullCascade0,
};
constexpr std::array kShadowCascade1ScheduleDependencies{
    RenderPassId::ShadowCullCascade1,
};
constexpr std::array kShadowCascade2ScheduleDependencies{
    RenderPassId::ShadowCullCascade2,
};
constexpr std::array kShadowCascade3ScheduleDependencies{
    RenderPassId::ShadowCullCascade3,
};
constexpr std::array<RenderPassId, 0> kLocalShadowDepthScheduleDependencies{};
constexpr std::array kDepthToReadOnlyScheduleDependencies{
    RenderPassId::GBuffer,
    RenderPassId::TransparentPick,
    RenderPassId::ShadowCascade0,
    RenderPassId::ShadowCascade1,
    RenderPassId::ShadowCascade2,
    RenderPassId::ShadowCascade3,
};
constexpr std::array kTileCullScheduleDependencies{RenderPassId::DepthToReadOnly};
constexpr std::array kLightingScheduleDependencies{
    RenderPassId::DepthToReadOnly,
    RenderPassId::TileCull,
    RenderPassId::GTAO,
    RenderPassId::OitClear,
};
constexpr std::array kTransformGizmosScheduleDependencies{
    RenderPassId::Lighting,
    RenderPassId::TemporalResolve,
};
constexpr std::array kTemporalVelocityScheduleDependencies{RenderPassId::Lighting};
constexpr std::array kTemporalResolveScheduleDependencies{RenderPassId::TemporalVelocity};
constexpr std::array kExposureAdaptationScheduleDependencies{
    RenderPassId::TransformGizmos,
};
constexpr std::array kOitResolveScheduleDependencies{
    RenderPassId::TransformGizmos,
    RenderPassId::ExposureAdaptation,
};
constexpr std::array kBloomScheduleDependencies{
    RenderPassId::ExposureAdaptation,
    RenderPassId::OitResolve,
};
constexpr std::array kPostProcessScheduleDependencies{
    RenderPassId::Bloom,
    RenderPassId::OitResolve,
};

constexpr std::array kFrustumCullReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::ObjectBuffer,
};
constexpr std::array kFrustumCullWrites{
    RenderResourceId::FrustumCullDraws,
    RenderResourceId::CullStats,
};
constexpr std::array kDepthPrepassReads{
    RenderResourceId::CameraBuffer,
};
constexpr std::array kDepthPrepassOptionalReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::FrustumCullDraws,
};
constexpr std::array kDepthPrepassWrites{
    RenderResourceId::SceneDepth,
};
constexpr std::array kBimDepthPrepassReads{
    RenderResourceId::BimGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::BimObjectBuffer,
    RenderResourceId::SceneDepth,
};
constexpr std::array kBimDepthPrepassWrites{
    RenderResourceId::SceneDepth,
};
constexpr std::array kHiZReads{
    RenderResourceId::SceneDepth,
};
constexpr std::array kHiZWrites{
    RenderResourceId::HiZPyramid,
    RenderResourceId::SceneDepth,
};
constexpr std::array kOcclusionCullReads{
    RenderResourceId::FrustumCullDraws,
    RenderResourceId::HiZPyramid,
    RenderResourceId::CameraBuffer,
};
constexpr std::array kOcclusionCullWrites{
    RenderResourceId::OcclusionCullDraws,
    RenderResourceId::CullStats,
};
constexpr std::array kCullStatsReadbackReads{
    RenderResourceId::FrustumCullDraws,
    RenderResourceId::OcclusionCullDraws,
    RenderResourceId::CullStats,
};
constexpr std::array kGBufferReads{
    RenderResourceId::CameraBuffer,
    RenderResourceId::SceneDepth,
};
constexpr std::array kGBufferOptionalReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::OcclusionCullDraws,
};
constexpr std::array kGBufferWrites{
    RenderResourceId::SceneDepth,
    RenderResourceId::GBufferAlbedo,
    RenderResourceId::GBufferNormal,
    RenderResourceId::GBufferMaterial,
    RenderResourceId::GBufferEmissive,
    RenderResourceId::GBufferSpecular,
    RenderResourceId::GBufferMaterialLayers,
    RenderResourceId::GBufferMaterialSheen,
    RenderResourceId::PickId,
};
constexpr std::array kBimGBufferReads{
    RenderResourceId::BimGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::BimObjectBuffer,
    RenderResourceId::SceneDepth,
    RenderResourceId::GBufferAlbedo,
    RenderResourceId::GBufferNormal,
    RenderResourceId::GBufferMaterial,
    RenderResourceId::GBufferEmissive,
    RenderResourceId::GBufferSpecular,
    RenderResourceId::GBufferMaterialLayers,
    RenderResourceId::GBufferMaterialSheen,
    RenderResourceId::PickId,
};
constexpr std::array kBimGBufferWrites{
    RenderResourceId::SceneDepth,
    RenderResourceId::GBufferAlbedo,
    RenderResourceId::GBufferNormal,
    RenderResourceId::GBufferMaterial,
    RenderResourceId::GBufferEmissive,
    RenderResourceId::GBufferSpecular,
    RenderResourceId::GBufferMaterialLayers,
    RenderResourceId::GBufferMaterialSheen,
    RenderResourceId::PickId,
};
constexpr std::array kTransparentPickReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::BimGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::BimObjectBuffer,
    RenderResourceId::SceneDepth,
    RenderResourceId::PickId,
};
constexpr std::array kTransparentPickWrites{
    RenderResourceId::PickId,
    RenderResourceId::PickDepth,
};
constexpr std::array kOitClearWrites{
    RenderResourceId::OitStorage,
};
constexpr std::array kShadowCullReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::ShadowData,
};
constexpr std::array kShadowCullCascade0Writes{
    RenderResourceId::ShadowCullCascade0,
};
constexpr std::array kShadowCullCascade1Writes{
    RenderResourceId::ShadowCullCascade1,
};
constexpr std::array kShadowCullCascade2Writes{
    RenderResourceId::ShadowCullCascade2,
};
constexpr std::array kShadowCullCascade3Writes{
    RenderResourceId::ShadowCullCascade3,
};
constexpr std::array kShadowCascadeReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::BimGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::BimObjectBuffer,
    RenderResourceId::ShadowData,
};
constexpr std::array kShadowCascade0OptionalReads{
    RenderResourceId::ShadowCullCascade0,
};
constexpr std::array kShadowCascade1OptionalReads{
    RenderResourceId::ShadowCullCascade1,
};
constexpr std::array kShadowCascade2OptionalReads{
    RenderResourceId::ShadowCullCascade2,
};
constexpr std::array kShadowCascade3OptionalReads{
    RenderResourceId::ShadowCullCascade3,
};
constexpr std::array kShadowCascadeWrites{
    RenderResourceId::ShadowAtlas,
};
constexpr std::array kLocalShadowDepthReads{
    RenderResourceId::SceneGeometry,
    RenderResourceId::BimGeometry,
    RenderResourceId::CameraBuffer,
    RenderResourceId::ObjectBuffer,
    RenderResourceId::BimObjectBuffer,
    RenderResourceId::LocalShadowData,
};
constexpr std::array kLocalShadowDepthWrites{
    RenderResourceId::LocalShadowAtlas,
};
constexpr std::array kDepthToReadOnlyReads{
    RenderResourceId::SceneDepth,
};
constexpr std::array kDepthToReadOnlyWrites{
    RenderResourceId::SceneDepth,
};
constexpr std::array kTileCullReads{
    RenderResourceId::SceneDepth,
    RenderResourceId::CameraBuffer,
    RenderResourceId::LightingData,
};
constexpr std::array kTileCullWrites{
    RenderResourceId::TileLightGrid,
};
constexpr std::array kGtaoReads{
    RenderResourceId::SceneDepth,
    RenderResourceId::GBufferNormal,
    RenderResourceId::CameraBuffer,
};
constexpr std::array kGtaoWrites{
    RenderResourceId::AmbientOcclusion,
};
constexpr std::array kLightingReads{
    RenderResourceId::SceneDepth,
    RenderResourceId::GBufferAlbedo,
    RenderResourceId::GBufferNormal,
    RenderResourceId::GBufferMaterial,
    RenderResourceId::GBufferEmissive,
    RenderResourceId::GBufferSpecular,
    RenderResourceId::GBufferMaterialLayers,
    RenderResourceId::GBufferMaterialSheen,
    RenderResourceId::LightingData,
    RenderResourceId::EnvironmentMaps,
};
constexpr std::array kLightingOptionalReads{
    RenderResourceId::RayShadowVisibility, RenderResourceId::ShadowAtlas,
    RenderResourceId::LocalShadowAtlas,    RenderResourceId::TileLightGrid,
    RenderResourceId::AmbientOcclusion,    RenderResourceId::OitStorage,
};
constexpr std::array kLightingWrites{
    RenderResourceId::SceneColor,
    RenderResourceId::OitStorage,
};
constexpr std::array kTransformGizmosReads{
    RenderResourceId::CameraBuffer,
    RenderResourceId::SceneColor,
};
constexpr std::array kTransformGizmosWrites{
    RenderResourceId::SceneColor,
};
constexpr std::array kExposureAdaptationReads{
    RenderResourceId::SceneColor,
};
constexpr std::array kExposureAdaptationWrites{
    RenderResourceId::ExposureState,
};
constexpr std::array kOitResolveReads{
    RenderResourceId::OitStorage,
};
constexpr std::array kBloomReads{
    RenderResourceId::SceneColor,
};
constexpr std::array kBloomWrites{
    RenderResourceId::BloomTexture,
};
constexpr std::array kPostProcessReads{
    RenderResourceId::SceneColor,
    RenderResourceId::GBufferAlbedo,
    RenderResourceId::GBufferNormal,
    RenderResourceId::GBufferMaterial,
    RenderResourceId::GBufferEmissive,
    RenderResourceId::SceneDepth,
};
constexpr std::array kTemporalColorOptionalReads{
    RenderResourceId::TemporalColor};
constexpr std::array kPostProcessOptionalReads{
    RenderResourceId::TemporalColor, RenderResourceId::OitStorage,
    RenderResourceId::BloomTexture,
    RenderResourceId::ExposureState,
    RenderResourceId::ShadowAtlas,
    RenderResourceId::TileLightGrid,
};
constexpr std::array kPostProcessWrites{
    RenderResourceId::SwapchainImage,
};

constexpr std::array<RenderResourceTransition, 2> kHiZTransitions{{
    {RenderResourceId::SceneDepth,
     RenderResourceState::DepthStencilAttachment,
     RenderResourceState::DepthStencilReadOnly},
    {RenderResourceId::SceneDepth,
     RenderResourceState::DepthStencilReadOnly,
     RenderResourceState::DepthStencilAttachment},
}};
constexpr std::array<RenderResourceTransition, 1> kDepthToReadOnlyTransitions{{
    {RenderResourceId::SceneDepth,
     RenderResourceState::DepthStencilAttachment,
     RenderResourceState::DepthStencilReadOnly},
}};
constexpr std::array<RenderResourceTransition, 1> kExposureAdaptationTransitions{{
    {RenderResourceId::SceneColor,
     RenderResourceState::ColorAttachment,
     RenderResourceState::ShaderRead},
}};
constexpr std::array<RenderResourceTransition, 1> kBloomTransitions{{
    {RenderResourceId::SceneColor,
     RenderResourceState::ColorAttachment,
     RenderResourceState::ShaderRead},
}};
constexpr std::array<RenderResourceTransition, 1> kPostProcessTransitions{{
    {RenderResourceId::SwapchainImage,
     RenderResourceState::Present,
     RenderResourceState::ColorAttachment},
}};

bool isValidId(RenderPassId id) {
  return static_cast<size_t>(id) < kRenderPassIdCount;
}

void addDependency(std::vector<uint32_t>& dependencies, uint32_t dependency) {
  if (dependency == kMissingPassIndex) return;
  if (std::ranges::find(dependencies, dependency) == dependencies.end()) {
    dependencies.push_back(dependency);
  }
}

void validateScheduleDependencies(
    RenderPassId passId,
    std::span<const RenderPassId> dependencies) {
  for (size_t i = 0; i < dependencies.size(); ++i) {
    const RenderPassId dependencyId = dependencies[i];
    if (isValidId(dependencyId)) continue;

    throw std::runtime_error(
        "render graph pass " + std::string(renderPassName(passId)) +
        " declares invalid schedule dependency");
  }

  for (size_t i = 0; i < dependencies.size(); ++i) {
    for (size_t j = i + 1; j < dependencies.size(); ++j) {
      if (dependencies[i] != dependencies[j]) continue;

      throw std::runtime_error(
          "render graph pass " + std::string(renderPassName(passId)) +
          " declares duplicate schedule dependency " +
          std::string(renderPassName(dependencies[i])));
    }
  }
}

void validateResourceAccess(
    RenderPassId passId,
    std::string_view accessKind,
    std::span<const RenderResourceId> resources) {
  for (size_t i = 0; i < resources.size(); ++i) {
    const RenderResourceId resourceId = resources[i];
    if (isValidResource(resourceId)) continue;

    throw std::runtime_error(
        "render graph pass " + std::string(renderPassName(passId)) +
        " declares invalid " + std::string(accessKind) + " resource");
  }

  for (size_t i = 0; i < resources.size(); ++i) {
    for (size_t j = i + 1; j < resources.size(); ++j) {
      if (resources[i] != resources[j]) continue;

      throw std::runtime_error(
          "render graph pass " + std::string(renderPassName(passId)) +
          " declares duplicate " + std::string(accessKind) + " resource " +
          std::string(renderResourceName(resources[i])));
    }
  }
}

void validateReadAccessOverlap(
    RenderPassId passId,
    std::span<const RenderResourceId> reads,
    std::span<const RenderResourceId> optionalReads) {
  for (RenderResourceId read : reads) {
    if (std::ranges::find(optionalReads, read) == optionalReads.end()) {
      continue;
    }

    throw std::runtime_error(
        "render graph pass " + std::string(renderPassName(passId)) +
        " declares " + std::string(renderResourceName(read)) +
        " as both required and optional read");
  }
}

void validateResourceTransitions(
    RenderPassId passId,
    std::span<const RenderResourceTransition> transitions) {
  for (const RenderResourceTransition& transition : transitions) {
    if (isValidResource(transition.resource)) continue;

    throw std::runtime_error(
        "render graph pass " + std::string(renderPassName(passId)) +
        " declares invalid transition resource");
  }
}

}  // namespace

std::string_view renderPassName(RenderPassId id) {
  if (!isValidId(id)) return {};
  return kRenderPassNames[static_cast<size_t>(id)];
}

RenderPassId renderPassIdFromName(std::string_view name) {
  for (size_t i = 0; i < kRenderPassNames.size(); ++i) {
    if (kRenderPassNames[i] == name) {
      return static_cast<RenderPassId>(i);
    }
  }
  return RenderPassId::Invalid;
}

bool isProtectedRenderPass(RenderPassId id) {
  switch (id) {
  case RenderPassId::RaySceneBuild:
  case RenderPassId::RayShadowTrace:
  case RenderPassId::RayShadowFilter:
  case RenderPassId::DepthPrepass:
  case RenderPassId::GBuffer:
  case RenderPassId::OitClear:
  case RenderPassId::TransparentPick:
  case RenderPassId::DepthToReadOnly:
  case RenderPassId::Lighting:
  case RenderPassId::ExposureAdaptation:
  case RenderPassId::OitResolve:
  case RenderPassId::PostProcess:
  case RenderPassId::TemporalVelocity:
  case RenderPassId::TemporalResolve:
    return true;
    default:
      return false;
  }
}

std::span<const RenderPassId> renderPassDependencies(RenderPassId id) {
  switch (id) {
    case RenderPassId::HiZGenerate:
      return kHiZDependencies;
    case RenderPassId::OcclusionCull:
      return kOcclusionDependencies;
    case RenderPassId::BimGBuffer:
      return kBimGBufferDependencies;
    case RenderPassId::ShadowCullCascade0:
    case RenderPassId::ShadowCullCascade1:
    case RenderPassId::ShadowCullCascade2:
    case RenderPassId::ShadowCullCascade3:
      return kShadowCullDependencies;
    case RenderPassId::TileCull:
      return kTileCullDependencies;
    case RenderPassId::GTAO:
      return kGtaoDependencies;
    case RenderPassId::ExposureAdaptation:
      return kExposureAdaptationDependencies;
    case RenderPassId::Bloom:
      return kBloomDependencies;
    default:
      return kNoDependencies;
  }
}

std::span<const RenderPassId> renderPassScheduleDependencies(RenderPassId id) {
  switch (id) {
    case RenderPassId::DepthPrepass:
      return kDepthPrepassScheduleDependencies;
    case RenderPassId::BimDepthPrepass:
      return kBimDepthPrepassScheduleDependencies;
    case RenderPassId::HiZGenerate:
      return kHiZDependencies;
    case RenderPassId::OcclusionCull:
      return kOcclusionDependencies;
    case RenderPassId::CullStatsReadback:
      return kCullStatsDependencies;
    case RenderPassId::GBuffer:
      return kGBufferScheduleDependencies;
    case RenderPassId::BimGBuffer:
      return kBimGBufferScheduleDependencies;
    case RenderPassId::TransparentPick:
      return kTransparentPickScheduleDependencies;
    case RenderPassId::OitClear:
      return kOitClearScheduleDependencies;
    case RenderPassId::ShadowCullCascade0:
    case RenderPassId::ShadowCullCascade1:
    case RenderPassId::ShadowCullCascade2:
    case RenderPassId::ShadowCullCascade3:
      return kShadowCullDependencies;
    case RenderPassId::ShadowCascade0:
      return kShadowCascade0ScheduleDependencies;
    case RenderPassId::ShadowCascade1:
      return kShadowCascade1ScheduleDependencies;
    case RenderPassId::ShadowCascade2:
      return kShadowCascade2ScheduleDependencies;
    case RenderPassId::ShadowCascade3:
      return kShadowCascade3ScheduleDependencies;
    case RenderPassId::LocalShadowDepth:
      return kLocalShadowDepthScheduleDependencies;
    case RenderPassId::DepthToReadOnly:
      return kDepthToReadOnlyScheduleDependencies;
    case RenderPassId::TileCull:
      return kTileCullScheduleDependencies;
    case RenderPassId::GTAO:
      return kGtaoDependencies;
    case RenderPassId::Lighting:
      return kLightingScheduleDependencies;
    case RenderPassId::TemporalVelocity:
      return kTemporalVelocityScheduleDependencies;
    case RenderPassId::TemporalResolve:
      return kTemporalResolveScheduleDependencies;
    case RenderPassId::TransformGizmos:
      return kTransformGizmosScheduleDependencies;
    case RenderPassId::ExposureAdaptation:
      return kExposureAdaptationScheduleDependencies;
    case RenderPassId::OitResolve:
      return kOitResolveScheduleDependencies;
    case RenderPassId::Bloom:
      return kBloomScheduleDependencies;
    case RenderPassId::PostProcess:
      return kPostProcessScheduleDependencies;
    default:
      return kNoDependencies;
  }
}

std::string_view renderResourceName(RenderResourceId id) {
  if (!isValidResource(id)) return {};
  return kRenderResourceNames[static_cast<size_t>(id)];
}

bool isExternalRenderResource(RenderResourceId id) {
  switch (id) {
    case RenderResourceId::SceneGeometry:
    case RenderResourceId::BimGeometry:
    case RenderResourceId::CameraBuffer:
    case RenderResourceId::ObjectBuffer:
    case RenderResourceId::BimObjectBuffer:
    case RenderResourceId::LightingData:
    case RenderResourceId::ShadowData:
    case RenderResourceId::LocalShadowData:
    case RenderResourceId::EnvironmentMaps:
      return true;
    default:
      return false;
  }
}

std::span<const RenderResourceId> renderPassResourceReads(RenderPassId id) {
  switch (id) {
    case RenderPassId::FrustumCull:
      return kFrustumCullReads;
    case RenderPassId::DepthPrepass:
      return kDepthPrepassReads;
    case RenderPassId::BimDepthPrepass:
      return kBimDepthPrepassReads;
    case RenderPassId::HiZGenerate:
      return kHiZReads;
    case RenderPassId::OcclusionCull:
      return kOcclusionCullReads;
    case RenderPassId::GBuffer:
      return kGBufferReads;
    case RenderPassId::BimGBuffer:
      return kBimGBufferReads;
    case RenderPassId::TransparentPick:
      return kTransparentPickReads;
    case RenderPassId::ShadowCullCascade0:
    case RenderPassId::ShadowCullCascade1:
    case RenderPassId::ShadowCullCascade2:
    case RenderPassId::ShadowCullCascade3:
      return kShadowCullReads;
    case RenderPassId::ShadowCascade0:
    case RenderPassId::ShadowCascade1:
    case RenderPassId::ShadowCascade2:
    case RenderPassId::ShadowCascade3:
      return kShadowCascadeReads;
    case RenderPassId::LocalShadowDepth:
      return kLocalShadowDepthReads;
    case RenderPassId::DepthToReadOnly:
      return kDepthToReadOnlyReads;
    case RenderPassId::TileCull:
      return kTileCullReads;
    case RenderPassId::GTAO:
      return kGtaoReads;
    case RenderPassId::Lighting:
      return kLightingReads;
    case RenderPassId::TransformGizmos:
      return kTransformGizmosReads;
    case RenderPassId::ExposureAdaptation:
      return kExposureAdaptationReads;
    case RenderPassId::OitResolve:
      return kOitResolveReads;
    case RenderPassId::Bloom:
      return kBloomReads;
    case RenderPassId::PostProcess:
      return kPostProcessReads;
    default:
      return kNoResources;
  }
}

std::span<const RenderResourceId> renderPassOptionalResourceReads(
    RenderPassId id) {
  switch (id) {
    case RenderPassId::CullStatsReadback:
      // Readback reports frustum counts or zeros when later culling is skipped.
      return kCullStatsReadbackReads;
    case RenderPassId::DepthPrepass:
      return kDepthPrepassOptionalReads;
    case RenderPassId::GBuffer:
      return kGBufferOptionalReads;
    case RenderPassId::ShadowCascade0:
      return kShadowCascade0OptionalReads;
    case RenderPassId::ShadowCascade1:
      return kShadowCascade1OptionalReads;
    case RenderPassId::ShadowCascade2:
      return kShadowCascade2OptionalReads;
    case RenderPassId::ShadowCascade3:
      return kShadowCascade3OptionalReads;
    case RenderPassId::Lighting:
      return kLightingOptionalReads;
  case RenderPassId::ExposureAdaptation:
  case RenderPassId::Bloom:
    return kTemporalColorOptionalReads;
    case RenderPassId::PostProcess:
      return kPostProcessOptionalReads;
    default:
      return kNoResources;
  }
}

std::span<const RenderResourceId> renderPassResourceWrites(RenderPassId id) {
  switch (id) {
    case RenderPassId::FrustumCull:
      return kFrustumCullWrites;
    case RenderPassId::DepthPrepass:
      return kDepthPrepassWrites;
    case RenderPassId::BimDepthPrepass:
      return kBimDepthPrepassWrites;
    case RenderPassId::HiZGenerate:
      return kHiZWrites;
    case RenderPassId::OcclusionCull:
      return kOcclusionCullWrites;
    case RenderPassId::GBuffer:
      return kGBufferWrites;
    case RenderPassId::BimGBuffer:
      return kBimGBufferWrites;
    case RenderPassId::TransparentPick:
      return kTransparentPickWrites;
    case RenderPassId::OitClear:
      return kOitClearWrites;
    case RenderPassId::ShadowCullCascade0:
      return kShadowCullCascade0Writes;
    case RenderPassId::ShadowCullCascade1:
      return kShadowCullCascade1Writes;
    case RenderPassId::ShadowCullCascade2:
      return kShadowCullCascade2Writes;
    case RenderPassId::ShadowCullCascade3:
      return kShadowCullCascade3Writes;
    case RenderPassId::ShadowCascade0:
    case RenderPassId::ShadowCascade1:
    case RenderPassId::ShadowCascade2:
    case RenderPassId::ShadowCascade3:
      return kShadowCascadeWrites;
    case RenderPassId::LocalShadowDepth:
      return kLocalShadowDepthWrites;
    case RenderPassId::DepthToReadOnly:
      return kDepthToReadOnlyWrites;
    case RenderPassId::TileCull:
      return kTileCullWrites;
    case RenderPassId::GTAO:
      return kGtaoWrites;
    case RenderPassId::Lighting:
      return kLightingWrites;
    case RenderPassId::TransformGizmos:
      return kTransformGizmosWrites;
    case RenderPassId::ExposureAdaptation:
      return kExposureAdaptationWrites;
    case RenderPassId::Bloom:
      return kBloomWrites;
    case RenderPassId::PostProcess:
      return kPostProcessWrites;
    default:
      return kNoResources;
  }
}

std::span<const RenderPassId> renderPassOptionalScheduleDependencies(
    RenderPassId id) {
  switch (id) {
    case RenderPassId::HiZGenerate:
      return kHiZOptionalScheduleDependencies;
    case RenderPassId::GBuffer:
      return kGBufferOptionalScheduleDependencies;
    case RenderPassId::TransparentPick:
      return kTransparentPickOptionalScheduleDependencies;
    case RenderPassId::DepthToReadOnly:
      return kDepthToReadOnlyOptionalScheduleDependencies;
    default:
      return kNoDependencies;
  }
}

std::span<const RenderResourceTransition> renderPassResourceTransitions(
    RenderPassId id) {
  switch (id) {
    case RenderPassId::HiZGenerate:
      return kHiZTransitions;
    case RenderPassId::DepthToReadOnly:
      return kDepthToReadOnlyTransitions;
    case RenderPassId::ExposureAdaptation:
      return kExposureAdaptationTransitions;
    case RenderPassId::Bloom:
      return kBloomTransitions;
    case RenderPassId::PostProcess:
      return kPostProcessTransitions;
    default:
      return kNoTransitions;
  }
}

std::string_view renderResourceStateName(RenderResourceState state) {
  switch (state) {
    case RenderResourceState::Undefined:
      return "undefined";
    case RenderResourceState::ColorAttachment:
      return "color attachment";
    case RenderResourceState::DepthStencilAttachment:
      return "depth/stencil attachment";
    case RenderResourceState::DepthStencilReadOnly:
      return "depth/stencil read-only";
    case RenderResourceState::ShaderRead:
      return "shader read";
    case RenderResourceState::ShaderWrite:
      return "shader write";
    case RenderResourceState::TransferRead:
      return "transfer read";
    case RenderResourceState::Present:
      return "present";
  }
  return {};
}

std::string_view renderPassSkipReasonName(RenderPassSkipReason reason) {
  switch (reason) {
    case RenderPassSkipReason::None:
      return "active";
    case RenderPassSkipReason::Disabled:
      return "disabled";
    case RenderPassSkipReason::MissingPassDependency:
      return "missing pass dependency";
    case RenderPassSkipReason::MissingResource:
      return "missing resource";
    case RenderPassSkipReason::MissingRecordCallback:
      return "missing record callback";
    case RenderPassSkipReason::NotNeeded:
      return "not needed";
  }
  return {};
}

std::span<const RenderPassId> shadowCullPassIds() {
  return kShadowCullPassIds;
}

std::span<const RenderPassId> shadowCascadePassIds() {
  return kShadowCascadePassIds;
}

RenderGraph::RenderGraph() {
  clear();
}

const RenderPassNode& RenderGraph::addPass(RenderPassId id,
                                           RenderPassNode::RecordFn fn) {
  return addPass(id, renderPassScheduleDependencies(id), std::move(fn));
}

const RenderPassNode& RenderGraph::addPass(
    RenderPassId id,
    std::initializer_list<RenderPassId> scheduleDependencies,
    RenderPassNode::RecordFn fn) {
  return addPass(
      id,
      std::span<const RenderPassId>(
          scheduleDependencies.begin(), scheduleDependencies.size()),
      std::move(fn));
}

const RenderPassNode& RenderGraph::addPass(
    RenderPassId id,
    std::span<const RenderPassId> scheduleDependencies,
    RenderPassNode::RecordFn fn) {
  if (!isValidId(id)) {
    throw std::runtime_error("invalid render graph pass id");
  }

  if (indexFor(id) != kMissingPassIndex) {
    throw std::runtime_error(
        "duplicate render graph pass: " + std::string(renderPassName(id)));
  }

  validateScheduleDependencies(id, scheduleDependencies);

  const uint32_t index = static_cast<uint32_t>(passes_.size());
  const auto reads = renderPassResourceReads(id);
  const auto optionalReads = renderPassOptionalResourceReads(id);
  const auto writes = renderPassResourceWrites(id);
  const auto transitions = renderPassResourceTransitions(id);
  validateResourceAccess(id, "read", reads);
  validateResourceAccess(id, "optional read", optionalReads);
  validateResourceAccess(id, "write", writes);
  validateReadAccessOverlap(id, reads, optionalReads);
  validateResourceTransitions(id, transitions);

  passes_.push_back({
      id,
      std::string(renderPassName(id)),
      std::vector<RenderPassId>(
          scheduleDependencies.begin(), scheduleDependencies.end()),
      std::vector<RenderResourceId>(reads.begin(), reads.end()),
      std::vector<RenderResourceId>(optionalReads.begin(), optionalReads.end()),
      std::vector<RenderResourceId>(writes.begin(), writes.end()),
      std::vector<RenderResourceTransition>(
          transitions.begin(), transitions.end()),
      true,
      {},
      std::move(fn)});
  if (isValidId(id)) {
    passIndexById_[static_cast<size_t>(id)] = index;
  }
  executionOrderDirty_ = true;
  activePlanDirty_ = true;
  invalidatePreparedFrame();
  return passes_.back();
}

void RenderGraph::compile() {
  enum class VisitState : uint8_t {
    Unvisited,
    Visiting,
    Visited,
  };

  auto topologicalSort =
      [this](const std::vector<std::vector<uint32_t>>& dependencyIndices) {
    std::vector<VisitState> visitState(passes_.size(), VisitState::Unvisited);
    std::vector<uint32_t> nextExecutionOrder;
    nextExecutionOrder.reserve(passes_.size());

    auto visit = [&](auto&& self, uint32_t index) -> void {
      if (visitState[index] == VisitState::Visited) return;
      if (visitState[index] == VisitState::Visiting) {
        throw std::runtime_error(
            "cycle detected in render graph near pass " + passes_[index].name);
      }

      visitState[index] = VisitState::Visiting;
      for (uint32_t dependencyIndex : dependencyIndices[index]) {
        self(self, dependencyIndex);
      }

      visitState[index] = VisitState::Visited;
      nextExecutionOrder.push_back(index);
    };

    for (uint32_t index = 0; index < passes_.size(); ++index) {
      visit(visit, index);
    }

    return nextExecutionOrder;
  };

  std::vector<std::vector<uint32_t>> dependencyIndices(passes_.size());
  for (uint32_t index = 0; index < passes_.size(); ++index) {
    for (RenderPassId dependencyId : passes_[index].scheduleDependencies) {
      const uint32_t dependencyIndex = indexFor(dependencyId);
      if (dependencyIndex == kMissingPassIndex) {
        throw std::runtime_error(
            "render graph pass " + passes_[index].name +
            " depends on missing pass " +
            std::string(renderPassName(dependencyId)));
      }
      addDependency(dependencyIndices[index], dependencyIndex);
    }

    for (RenderPassId dependencyId :
         renderPassOptionalScheduleDependencies(passes_[index].id)) {
      const uint32_t dependencyIndex = indexFor(dependencyId);
      if (dependencyIndex == kMissingPassIndex) continue;
      addDependency(dependencyIndices[index], dependencyIndex);
    }
  }

  executionOrder_ = topologicalSort(dependencyIndices);

  const auto resourceDependencies = buildResourceDependencies();
  for (uint32_t index = 0; index < dependencyIndices.size(); ++index) {
    for (uint32_t dependencyIndex : resourceDependencies[index]) {
      addDependency(dependencyIndices[index], dependencyIndex);
    }
  }

  executionOrder_ = topologicalSort(dependencyIndices);
  (void)buildResourceDependencies();
  executionPassIds_.clear();
  executionPassIds_.reserve(executionOrder_.size());
  for (uint32_t index : executionOrder_) {
    executionPassIds_.push_back(passes_[index].id);
  }
  executionOrderDirty_ = false;
  activePlanDirty_ = true;
}

void RenderGraph::execute(VkCommandBuffer cmd,
                          const FrameRecordParams& params) const {
  execute(cmd, params, RenderPassExecutionHooks{});
}

void RenderGraph::execute(VkCommandBuffer cmd,
                          const FrameRecordParams& params,
                          const RenderPassExecutionHooks& hooks) const {
  prepareFrame(params);
  executePreparedFrame(cmd, params, hooks);
}

void RenderGraph::prepareFrame(const FrameRecordParams& params) const {
  const uint64_t preparedSignature = computePreparedFrameSignature(params);
  if (!preparedFramePlanDirty_ && !executionOrderDirty_ &&
      !activePlanDirty_ && preparedSignature == preparedFrameSignature_) {
    return;
  }
  rebuildFrameExecutionOrder(params);
  preparedFrameSignature_ = preparedSignature;
  preparedFramePlanDirty_ = false;
}

void RenderGraph::executePreparedFrame(VkCommandBuffer cmd,
                                       const FrameRecordParams& params) const {
  executePreparedFrame(cmd, params, RenderPassExecutionHooks{});
}

void RenderGraph::executePreparedFrame(
    VkCommandBuffer cmd,
    const FrameRecordParams& params,
    const RenderPassExecutionHooks& hooks) const {
  if (lastFrameExecutionStatuses_.empty() || executionOrderDirty_ ||
      activePlanDirty_ || preparedFramePlanDirty_) {
    prepareFrame(params);
  }

  const std::vector<uint32_t> executionOrder = lastFrameExecutionOrder_;
  executing_ = true;
  struct ExecutingGuard {
    const RenderGraph& graph;
    ~ExecutingGuard() { graph.executing_ = false; }
  } executingGuard{*this};
  for (uint32_t index : executionOrder) {
    const auto& pass = passes_[index];
    if (pass.record) {
      if (hooks.beginPass) {
        hooks.beginPass(pass.id, cmd);
      }
      const auto start = std::chrono::steady_clock::now();
      pass.record(cmd, params);
      if (hooks.endPass) {
        const auto elapsed =
            std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();
        hooks.endPass(pass.id, cmd, elapsed);
      }
    }
  }
}

bool RenderGraph::setPassEnabled(RenderPassId id, bool enabled) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;
  if (pass->enabled != enabled) {
    pass->enabled = enabled;
    activePlanDirty_ = true;
    invalidatePreparedFrame();
  }
  return true;
}

bool RenderGraph::setPassRecord(RenderPassId id, RenderPassNode::RecordFn fn) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;

  pass->record = std::move(fn);
  activePlanDirty_ = true;
  invalidatePreparedFrame();
  return true;
}

bool RenderGraph::setPassReadiness(RenderPassId id,
                                   RenderPassNode::ReadinessFn fn) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;

  pass->readiness = std::move(fn);
  invalidatePreparedFrame();
  return true;
}

bool RenderGraph::setPassScheduleDependencies(
    RenderPassId id,
    std::initializer_list<RenderPassId> scheduleDependencies) {
  return setPassScheduleDependencies(
      id,
      std::span<const RenderPassId>(
          scheduleDependencies.begin(), scheduleDependencies.size()));
}

bool RenderGraph::setPassScheduleDependencies(
    RenderPassId id,
    std::span<const RenderPassId> scheduleDependencies) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;

  validateScheduleDependencies(id, scheduleDependencies);

  pass->scheduleDependencies.assign(
      scheduleDependencies.begin(), scheduleDependencies.end());
  executionOrderDirty_ = true;
  activePlanDirty_ = true;
  invalidatePreparedFrame();
  return true;
}

bool RenderGraph::setPassResourceAccess(
    RenderPassId id,
    std::initializer_list<RenderResourceId> reads,
    std::initializer_list<RenderResourceId> optionalReads,
    std::initializer_list<RenderResourceId> writes) {
  return setPassResourceAccess(
      id,
      std::span<const RenderResourceId>(reads.begin(), reads.size()),
      std::span<const RenderResourceId>(
          optionalReads.begin(), optionalReads.size()),
      std::span<const RenderResourceId>(writes.begin(), writes.size()));
}

bool RenderGraph::setPassResourceAccess(
    RenderPassId id,
    std::span<const RenderResourceId> reads,
    std::span<const RenderResourceId> optionalReads,
    std::span<const RenderResourceId> writes) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;

  validateResourceAccess(id, "read", reads);
  validateResourceAccess(id, "optional read", optionalReads);
  validateResourceAccess(id, "write", writes);
  validateReadAccessOverlap(id, reads, optionalReads);

  pass->reads.assign(reads.begin(), reads.end());
  pass->optionalReads.assign(optionalReads.begin(), optionalReads.end());
  pass->writes.assign(writes.begin(), writes.end());
  executionOrderDirty_ = true;
  activePlanDirty_ = true;
  invalidatePreparedFrame();
  return true;
}

bool RenderGraph::setPassResourceTransitions(
    RenderPassId id,
    std::initializer_list<RenderResourceTransition> transitions) {
  return setPassResourceTransitions(
      id,
      std::span<const RenderResourceTransition>(
          transitions.begin(), transitions.size()));
}

bool RenderGraph::setPassResourceTransitions(
    RenderPassId id,
    std::span<const RenderResourceTransition> transitions) {
  RenderPassNode* pass = mutablePass(id);
  if (pass == nullptr) return false;

  validateResourceTransitions(id, transitions);
  pass->transitions.assign(transitions.begin(), transitions.end());
  invalidatePreparedFrame();
  return true;
}

const RenderPassNode* RenderGraph::findPass(RenderPassId id) const {
  const uint32_t index = indexFor(id);
  return index != kMissingPassIndex ? &passes_[index] : nullptr;
}

uint32_t RenderGraph::passCount() const {
  return static_cast<uint32_t>(passes_.size());
}

uint32_t RenderGraph::enabledPassCount() const {
  return static_cast<uint32_t>(
      std::ranges::count_if(passes_, &RenderPassNode::enabled));
}

std::span<const RenderPassId> RenderGraph::executionPassIds() const {
  ensureCompiled();
  return executionPassIds_;
}

std::span<const RenderPassId> RenderGraph::activeExecutionPassIds() const {
  ensureActivePlan();
  return activeExecutionPassIds_;
}

std::span<const RenderPassExecutionStatus> RenderGraph::executionStatuses() const {
  ensureActivePlan();
  return executionStatuses_;
}

std::span<const RenderPassId> RenderGraph::lastFrameActiveExecutionPassIds() const {
  if (lastFrameExecutionStatuses_.empty()) {
    return activeExecutionPassIds();
  }
  return lastFrameActiveExecutionPassIds_;
}

std::span<const RenderPassExecutionStatus>
RenderGraph::lastFrameExecutionStatuses() const {
  if (lastFrameExecutionStatuses_.empty()) {
    return executionStatuses();
  }
  return lastFrameExecutionStatuses_;
}

const RenderPassExecutionStatus* RenderGraph::executionStatus(
    RenderPassId id) const {
  if (executing_ && !lastFrameExecutionStatuses_.empty()) {
    const uint32_t runtimeIndex = indexFor(id);
    return runtimeIndex != kMissingPassIndex
               ? &lastFrameExecutionStatuses_[runtimeIndex]
               : nullptr;
  }

  const uint32_t index = indexFor(id);
  ensureActivePlan();
  return index != kMissingPassIndex ? &executionStatuses_[index] : nullptr;
}

bool RenderGraph::isPassActive(RenderPassId id) const {
  const auto* status = executionStatus(id);
  return status != nullptr && status->active;
}

std::span<const RenderResourceEdge> RenderGraph::resourceEdges() const {
  ensureCompiled();
  return resourceEdges_;
}

RenderGraphDebugModel RenderGraph::debugModel() const {
  ensureActivePlan();

  RenderGraphDebugModel model{};
  model.passes.reserve(passes_.size());
  for (const RenderPassNode& pass : passes_) {
    const RenderPassExecutionStatus* status = executionStatus(pass.id);
    RenderGraphPassDebugState passState{};
    passState.passName = std::string(renderPassName(pass.id));
    passState.enabled = pass.enabled;
    passState.active = status != nullptr ? status->active : false;
    passState.skipReason =
        status != nullptr ? std::string(renderPassSkipReasonName(
                             status->skipReason))
                          : std::string(renderPassSkipReasonName(
                                RenderPassSkipReason::MissingRecordCallback));
    model.passes.push_back(std::move(passState));
  }
  return model;
}

void RenderGraph::clear() {
  passes_.clear();
  passIndexById_.fill(kMissingPassIndex);
  executionOrder_.clear();
  activeExecutionOrder_.clear();
  executionPassIds_.clear();
  activeExecutionPassIds_.clear();
  lastFrameExecutionOrder_.clear();
  lastFrameActiveExecutionPassIds_.clear();
  activePasses_.clear();
  executionStatuses_.clear();
  lastFrameExecutionStatuses_.clear();
  resourceEdges_.clear();
  executionOrderDirty_ = false;
  activePlanDirty_ = true;
  preparedFramePlanDirty_ = true;
  activePlanSignature_ = 0;
  preparedFrameSignature_ = 0;
  executing_ = false;
}

uint32_t RenderGraph::indexFor(RenderPassId id) const {
  if (!isValidId(id)) return kMissingPassIndex;
  const uint32_t index = passIndexById_[static_cast<size_t>(id)];
  if (index >= passes_.size()) return kMissingPassIndex;
  return index;
}

RenderPassNode* RenderGraph::mutablePass(RenderPassId id) {
  const uint32_t index = indexFor(id);
  return index != kMissingPassIndex ? &passes_[index] : nullptr;
}

std::vector<std::vector<uint32_t>> RenderGraph::buildResourceDependencies() {
  resourceEdges_.clear();

  std::array<std::vector<uint32_t>, kRenderResourceIdCount> writersByResource{};
  for (uint32_t index = 0; index < passes_.size(); ++index) {
    for (RenderResourceId resourceId : passes_[index].writes) {
      if (!isValidResource(resourceId) ||
          isExternalRenderResource(resourceId)) {
        continue;
      }
      writersByResource[static_cast<size_t>(resourceId)].push_back(index);
    }
  }

  std::vector<std::vector<uint32_t>> dependencyIndices(passes_.size());
  std::array<uint32_t, kRenderResourceIdCount> lastWriter{};
  lastWriter.fill(kMissingPassIndex);

  auto trackReads = [&](const RenderPassNode& pass,
                        uint32_t passIndex,
                        std::span<const RenderResourceId> reads,
                        bool required) {
    for (RenderResourceId resourceId : reads) {
      if (!isValidResource(resourceId) ||
          isExternalRenderResource(resourceId)) {
        continue;
      }

      const uint32_t writerIndex =
          lastWriter[static_cast<size_t>(resourceId)];
      uint32_t dependencyIndex = writerIndex;

      if (dependencyIndex == kMissingPassIndex) {
        const auto& writers = writersByResource[static_cast<size_t>(resourceId)];
        if (writers.size() == 1u && writers.front() != passIndex) {
          dependencyIndex = writers.front();
        }
      }

      if (dependencyIndex == kMissingPassIndex) {
        if (required) {
          throw std::runtime_error(
              "render graph pass " + pass.name + " reads " +
              std::string(renderResourceName(resourceId)) +
              " before any earlier pass writes it");
        }
        continue;
      }

      addDependency(dependencyIndices[passIndex], dependencyIndex);
      resourceEdges_.push_back({
          resourceId,
          passes_[dependencyIndex].id,
          pass.id,
      });
    }
  };

  for (uint32_t index : executionOrder_) {
    const RenderPassNode& pass = passes_[index];

    trackReads(pass, index, pass.reads, true);
    trackReads(pass, index, pass.optionalReads, false);

    for (RenderResourceId resourceId : pass.writes) {
      if (!isValidResource(resourceId)) continue;
      lastWriter[static_cast<size_t>(resourceId)] = index;
    }
  }

  return dependencyIndices;
}

uint64_t RenderGraph::computeActivePlanSignature() const {
  uint64_t signature = 1469598103934665603ull;
  auto mix = [&signature](uint64_t value) {
    signature ^= value;
    signature *= 1099511628211ull;
  };

  mix(static_cast<uint64_t>(passes_.size()));
  for (const RenderPassNode& pass : passes_) {
    mix(static_cast<uint64_t>(pass.id));
    mix(pass.enabled ? 1u : 0u);
    mix(pass.record ? 1u : 0u);
  }
  return signature;
}

uint64_t RenderGraph::computePreparedFrameSignature(
    const FrameRecordParams& params) const {
  uint64_t signature = computeActivePlanSignature();
  auto mix = [&signature](uint64_t value) {
    signature ^= value;
    signature *= 1099511628211ull;
  };
  auto mixPointer = [&mix](const void* ptr) {
    mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)));
  };
  auto mixFloat = [&mix](float value) {
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    mix(bits);
  };
  auto mixDrawList = [&mix, &mixPointer](const std::vector<DrawCommand>* draws) {
    mixPointer(draws);
    mix(draws != nullptr ? static_cast<uint64_t>(draws->size()) : 0u);
  };

  mix(params.runtime.frameSlot);
  mix(params.runtime.imageIndex);
  mix(static_cast<uint64_t>(params.runtime.activeTechnique));
  mix(params.scene.objectDataRevision);
  mix(params.bim.scene.objectDataRevision);
  mixPointer(params.registries.resourceBindings);
  mixPointer(params.registries.pipelineHandles);
  mixPointer(params.registries.pipelineLayouts);
  mix(params.debug.displayMode);
  mix(params.debug.debugDirectionalOnly ? 1u : 0u);
  mix(params.debug.debugVisualizePointLightStencil ? 1u : 0u);
  mix(params.debug.debugFreezeCulling ? 1u : 0u);
  mixPointer(params.services.temporalManager);
  mix(params.services.temporalManager && params.services.temporalManager->active() ? 1u : 0u);
  mixFloat(params.camera.nearPlane);
  mixFloat(params.camera.farPlane);
  mix(params.camera.orthographic ? 1u : 0u);
  mixDrawList(params.draws.opaqueDrawCommands);
  mixDrawList(params.draws.opaqueSingleSidedDrawCommands);
  mixDrawList(params.draws.opaqueWindingFlippedDrawCommands);
  mixDrawList(params.draws.opaqueDoubleSidedDrawCommands);
  mixDrawList(params.draws.transparentDrawCommands);
  mixDrawList(params.draws.transparentSingleSidedDrawCommands);
  mixDrawList(params.draws.transparentWindingFlippedDrawCommands);
  mixDrawList(params.draws.transparentDoubleSidedDrawCommands);
  mixDrawList(params.bim.draws.opaqueDrawCommands);
  mixDrawList(params.bim.draws.opaqueSingleSidedDrawCommands);
  mixDrawList(params.bim.draws.opaqueWindingFlippedDrawCommands);
  mixDrawList(params.bim.draws.opaqueDoubleSidedDrawCommands);
  mixDrawList(params.bim.draws.transparentDrawCommands);
  mixDrawList(params.bim.draws.transparentSingleSidedDrawCommands);
  mixDrawList(params.bim.draws.transparentWindingFlippedDrawCommands);
  mixDrawList(params.bim.draws.transparentDoubleSidedDrawCommands);
  mix(params.shadows.localShadowLayerCount);
  mix(params.shadows.useGpuShadowCull ? 1u : 0u);
  mix(params.shadows.useShadowSecondaryCommandBuffers ? 1u : 0u);
  mix(params.shadows.renderPass != VK_NULL_HANDLE
          ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                params.shadows.renderPass))
          : 0u);
  mixPointer(params.shadows.shadowFramebuffers);
  mixPointer(params.shadows.localShadowFramebuffers);
  mixPointer(params.shadows.shadowData);
  mixPointer(params.shadows.localShadowData);
  mixPointer(params.shadows.shadowManager);
  mixPointer(params.shadows.shadowCullManager);
  const auto& shadowSettings = params.shadows.shadowSettings;
  mixFloat(shadowSettings.normalBiasMinTexels);
  mixFloat(shadowSettings.normalBiasMaxTexels);
  mixFloat(shadowSettings.slopeBiasScale);
  mixFloat(shadowSettings.receiverPlaneBiasScale);
  mixFloat(shadowSettings.filterRadiusTexels);
  mixFloat(shadowSettings.cascadeBlendFraction);
  mixFloat(shadowSettings.constantDepthBias);
  mixFloat(shadowSettings.maxDepthBias);
  mixFloat(shadowSettings.rasterConstantBias);
  mixFloat(shadowSettings.rasterSlopeBias);
  mixFloat(shadowSettings.directionalPcssLightRadiusDegrees);
  mixFloat(shadowSettings.directionalPcssBlockerSearchRadiusTexels);
  mixFloat(shadowSettings.directionalPcssMaxFilterRadiusTexels);
  mixFloat(shadowSettings.directionalContactMaxDistance);
  mixFloat(shadowSettings.directionalContactThickness);
  mixFloat(shadowSettings.directionalContactFadeDistance);
  mix(shadowSettings.directionalPcssEnabled ? 1u : 0u);
  mix(shadowSettings.directionalContactVisibility ? 1u : 0u);
  mix(shadowSettings.localContactVisibility ? 1u : 0u);
  mix(shadowSettings.areaShadowQuality);
  mixPointer(params.services.gpuCullManager);
  mixPointer(params.services.bimManager);
  mixPointer(params.services.bloomManager);
  mixPointer(params.services.telemetry);
  mixPointer(params.services.gpuProfiler);
  mix(params.postProcess.renderPass != VK_NULL_HANDLE
          ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                params.postProcess.renderPass))
          : 0u);
  const auto& exposureSettings = params.postProcess.exposureSettings;
  mix(exposureSettings.mode);
  mixFloat(exposureSettings.manualExposure);
  mixFloat(exposureSettings.targetLuminance);
  mixFloat(exposureSettings.minExposure);
  mixFloat(exposureSettings.maxExposure);
  mixFloat(exposureSettings.adaptationRate);
  mixFloat(exposureSettings.meteringLowPercentile);
  mixFloat(exposureSettings.meteringHighPercentile);
  return signature;
}

void RenderGraph::invalidatePreparedFrame() {
  preparedFramePlanDirty_ = true;
  preparedFrameSignature_ = 0;
}

void RenderGraph::ensureActivePlan() const {
  ensureCompiled();

  const uint64_t signature = computeActivePlanSignature();
  if (!activePlanDirty_ && signature == activePlanSignature_) return;

  rebuildActiveExecutionOrder();
  activePlanSignature_ = signature;
  activePlanDirty_ = false;
}

void RenderGraph::rebuildFrameExecutionOrder(
    const FrameRecordParams& params) const {
  ensureActivePlan();

  lastFrameExecutionOrder_.clear();
  lastFrameActiveExecutionPassIds_.clear();
  lastFrameExecutionStatuses_ = executionStatuses_;

  std::vector<uint8_t> frameActivePasses(passes_.size(), 0u);
  std::array<uint8_t, kRenderResourceIdCount> availableResources{};
  for (size_t i = 0; i < availableResources.size(); ++i) {
    const auto resourceId = static_cast<RenderResourceId>(i);
    availableResources[i] = isExternalRenderResource(resourceId) ? 1u : 0u;
  }

  auto findInactiveRequiredPass =
      [&](const RenderPassNode& pass) -> RenderPassId {
    for (RenderPassId dependencyId : renderPassDependencies(pass.id)) {
      const uint32_t dependencyIndex = indexFor(dependencyId);
      if (dependencyIndex != kMissingPassIndex &&
          frameActivePasses[dependencyIndex] == 0u) {
        return dependencyId;
      }
    }
    return RenderPassId::Invalid;
  };

  auto findMissingRequiredResource =
      [&](const RenderPassNode& pass) -> RenderResourceId {
    for (RenderResourceId resourceId : pass.reads) {
      if (!isValidResource(resourceId)) continue;
      if (isExternalRenderResource(resourceId)) continue;
      if (availableResources[static_cast<size_t>(resourceId)] == 0u) {
        return resourceId;
      }
    }
    return RenderResourceId::Invalid;
  };

  auto skipPass = [&](uint32_t index,
                      RenderPassSkipReason reason,
                      RenderPassId blockingPass = RenderPassId::Invalid,
                      RenderResourceId blockingResource =
                          RenderResourceId::Invalid) {
    auto& status = lastFrameExecutionStatuses_[index];
    status.active = false;
    status.skipReason = reason;
    status.blockingPass = blockingPass;
    status.blockingResource = blockingResource;
  };

  for (uint32_t index : executionOrder_) {
    const RenderPassNode& pass = passes_[index];
    auto& status = lastFrameExecutionStatuses_[index];

    if (!status.active) {
      continue;
    }

    const RenderPassId inactivePass = findInactiveRequiredPass(pass);
    if (inactivePass != RenderPassId::Invalid) {
      skipPass(index, RenderPassSkipReason::MissingPassDependency,
               inactivePass);
      continue;
    }

    const RenderResourceId missingResource = findMissingRequiredResource(pass);
    if (missingResource != RenderResourceId::Invalid) {
      skipPass(index, RenderPassSkipReason::MissingResource,
               RenderPassId::Invalid, missingResource);
      continue;
    }

    if (pass.readiness) {
      RenderPassReadiness readiness = pass.readiness(params);
      if (!readiness.ready) {
        RenderPassSkipReason reason = readiness.skipReason;
        if (reason == RenderPassSkipReason::None) {
          reason = RenderPassSkipReason::NotNeeded;
        }
        skipPass(index, reason, readiness.blockingPass,
                 readiness.blockingResource);
        continue;
      }
    }

    frameActivePasses[index] = 1u;
    lastFrameExecutionOrder_.push_back(index);
    lastFrameActiveExecutionPassIds_.push_back(pass.id);
    status.active = true;
    status.skipReason = RenderPassSkipReason::None;
    status.blockingPass = RenderPassId::Invalid;
    status.blockingResource = RenderResourceId::Invalid;

    for (RenderResourceId resourceId : pass.writes) {
      if (!isValidResource(resourceId)) continue;
      availableResources[static_cast<size_t>(resourceId)] = 1u;
    }
  }
}

void RenderGraph::rebuildActiveExecutionOrder() const {
  activeExecutionOrder_.clear();
  activeExecutionPassIds_.clear();
  activePasses_.assign(passes_.size(), 0u);
  executionStatuses_.assign(passes_.size(), {});
  for (uint32_t index = 0; index < passes_.size(); ++index) {
    executionStatuses_[index].id = passes_[index].id;
  }

  std::array<uint8_t, kRenderResourceIdCount> availableResources{};
  for (size_t i = 0; i < availableResources.size(); ++i) {
    const auto resourceId = static_cast<RenderResourceId>(i);
    availableResources[i] = isExternalRenderResource(resourceId) ? 1u : 0u;
  }

  auto findInactiveRequiredPass = [this](const RenderPassNode& pass) {
    for (RenderPassId dependencyId : renderPassDependencies(pass.id)) {
      const uint32_t dependencyIndex = indexFor(dependencyId);
      if (dependencyIndex != kMissingPassIndex &&
          activePasses_[dependencyIndex] == 0u) {
        return dependencyId;
      }
    }
    return RenderPassId::Invalid;
  };

  auto findMissingRequiredResource = [&](const RenderPassNode& pass) {
    for (RenderResourceId resourceId : pass.reads) {
      if (!isValidResource(resourceId)) continue;
      if (isExternalRenderResource(resourceId)) continue;
      if (availableResources[static_cast<size_t>(resourceId)] == 0u) {
        return resourceId;
      }
    }
    return RenderResourceId::Invalid;
  };

  for (uint32_t index : executionOrder_) {
    const RenderPassNode& pass = passes_[index];

    auto& status = executionStatuses_[index];
    status.id = pass.id;
    status.active = false;
    status.skipReason = RenderPassSkipReason::None;
    status.blockingPass = RenderPassId::Invalid;
    status.blockingResource = RenderResourceId::Invalid;

    if (!pass.enabled) {
      status.skipReason = RenderPassSkipReason::Disabled;
      continue;
    }

    if (!pass.record) {
      status.skipReason = RenderPassSkipReason::MissingRecordCallback;
      continue;
    }

    const RenderPassId inactivePass = findInactiveRequiredPass(pass);
    if (inactivePass != RenderPassId::Invalid) {
      status.skipReason = RenderPassSkipReason::MissingPassDependency;
      status.blockingPass = inactivePass;
      continue;
    }

    const RenderResourceId missingResource = findMissingRequiredResource(pass);
    if (missingResource != RenderResourceId::Invalid) {
      status.skipReason = RenderPassSkipReason::MissingResource;
      status.blockingResource = missingResource;
      continue;
    }

    activePasses_[index] = 1u;
    activeExecutionOrder_.push_back(index);
    activeExecutionPassIds_.push_back(pass.id);
    status.active = true;

    for (RenderResourceId resourceId : pass.writes) {
      if (!isValidResource(resourceId)) continue;
      availableResources[static_cast<size_t>(resourceId)] = 1u;
    }
  }
}

void RenderGraph::ensureCompiled() const {
  if (!executionOrderDirty_) return;
  const_cast<RenderGraph*>(this)->compile();
}

}  // namespace container::renderer
