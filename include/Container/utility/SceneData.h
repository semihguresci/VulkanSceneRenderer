#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace container::gpu {

struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(16) glm::mat4 inverseViewProj{1.0f};
  alignas(16) glm::vec4 cameraWorldPosition{0.0f, 0.0f, 0.0f, 1.0f};
  // Unit world-space forward vector. Lighting uses this for cascade selection
  // without reconstructing camera basis vectors per shaded pixel.
  alignas(16) glm::vec4 cameraForward{0.0f, 0.0f, -1.0f, 0.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  // Inverse-transpose normal transform stored as 3 columns. The implicit final
  // row of a mat4 is unused for normal vectors, so this saves 16 bytes/object.
  alignas(16) glm::vec4 normalMatrix0{1.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 normalMatrix1{0.0f, 1.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 normalMatrix2{0.0f, 0.0f, 1.0f, 0.0f};
  // x = material index; y = object flags; z = pick ID source mask;
  // w = optional semantic/material class id for sidecar renderers.
  alignas(16) glm::uvec4 objectInfo{0, 0, 0, 0};
  // Bounding sphere in world space: xyz = center, w = radius.
  alignas(16) glm::vec4 boundingSphere{0.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuTextureTransform {
  // row0.w stores the source texture coordinate set as a small exact float.
  // row1.w stores a scalar channel override: 0-3 = RGBA, >3 = slot default.
  alignas(16) glm::vec4 row0{1.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 row1{0.0f, 1.0f, 0.0f, 4.0f};
};

constexpr uint32_t kPickIdNone = 0u;
constexpr uint32_t kPickIdBimMask = 0x80000000u;
constexpr uint32_t kPickIdLightMask = 0x40000000u;
constexpr uint32_t kPickIdObjectMask = 0x3fffffffu;

struct GpuMaterial {
  alignas(16) glm::vec4 color{1.0f};
  alignas(16) glm::vec3 emissiveColor{0.0f, 0.0f, 0.0f};
  alignas(4) float emissiveStrength{1.0f};
  alignas(8) glm::vec2 metallicRoughness{0.0f, 1.0f};
  alignas(4) float alphaCutoff{0.5f};
  alignas(4) float normalTextureScale{1.0f};
  alignas(4) float occlusionStrength{1.0f};
  alignas(4) uint32_t baseColorTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t occlusionTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t emissiveTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metallicRoughnessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t roughnessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metalnessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t specularTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t heightTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t opacityTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t transmissionTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t specularColorTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t clearcoatTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t clearcoatRoughnessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t clearcoatNormalTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t thicknessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t sheenColorTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t sheenRoughnessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t iridescenceTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t iridescenceThicknessTextureIndex{
      std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t flags{0};
  alignas(4) float opacityFactor{1.0f};
  alignas(4) float specularFactor{1.0f};
  alignas(4) float heightScale{0.0f};
  alignas(4) float heightOffset{-0.5f};
  alignas(4) float transmissionFactor{0.0f};
  alignas(4) float ior{1.5f};
  alignas(4) float dispersion{0.0f};
  alignas(4) float clearcoatFactor{0.0f};
  alignas(4) float clearcoatRoughnessFactor{0.0f};
  alignas(4) float clearcoatNormalTextureScale{1.0f};
  alignas(4) float thicknessFactor{0.0f};
  alignas(4) float attenuationDistance{std::numeric_limits<float>::infinity()};
  alignas(4) float sheenRoughnessFactor{0.0f};
  alignas(4) float iridescenceFactor{0.0f};
  alignas(4) float iridescenceIor{1.3f};
  alignas(4) float iridescenceThicknessMinimum{100.0f};
  alignas(4) float iridescenceThicknessMaximum{400.0f};
  alignas(16) glm::vec4 specularColorFactor{1.0f, 1.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 attenuationColor{1.0f, 1.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 sheenColorFactor{0.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) GpuTextureTransform baseColorTextureTransform{};
  alignas(16) GpuTextureTransform normalTextureTransform{};
  alignas(16) GpuTextureTransform occlusionTextureTransform{};
  alignas(16) GpuTextureTransform emissiveTextureTransform{};
  alignas(16) GpuTextureTransform metallicRoughnessTextureTransform{};
  alignas(16) GpuTextureTransform roughnessTextureTransform{};
  alignas(16) GpuTextureTransform metalnessTextureTransform{};
  alignas(16) GpuTextureTransform specularTextureTransform{};
  alignas(16) GpuTextureTransform heightTextureTransform{};
  alignas(16) GpuTextureTransform opacityTextureTransform{};
  alignas(16) GpuTextureTransform transmissionTextureTransform{};
  alignas(16) GpuTextureTransform specularColorTextureTransform{};
  alignas(16) GpuTextureTransform clearcoatTextureTransform{};
  alignas(16) GpuTextureTransform clearcoatRoughnessTextureTransform{};
  alignas(16) GpuTextureTransform clearcoatNormalTextureTransform{};
  alignas(16) GpuTextureTransform thicknessTextureTransform{};
  alignas(16) GpuTextureTransform sheenColorTextureTransform{};
  alignas(16) GpuTextureTransform sheenRoughnessTextureTransform{};
  alignas(16) GpuTextureTransform iridescenceTextureTransform{};
  alignas(16) GpuTextureTransform iridescenceThicknessTextureTransform{};
};

struct NormalValidationSettings {
  bool enabled{false};
  bool showFaceFill{true};
  float lineLength{0.16f};
  float lineOffset{0.002f};
  float lineWidth{1.0f};
  float faceAlpha{1.0f};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
  uint32_t sectionPlaneEnabled{0};
  uint32_t semanticColorMode{0};
  uint32_t padding0{0};
  alignas(16) glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
};

struct SceneClipState {
  uint32_t boxClipEnabled{0};
  uint32_t boxClipInvert{0};
  uint32_t boxClipPlaneCount{0};
  uint32_t padding0{0};
  alignas(16) glm::vec4 boxClipPlanes[6]{};
};

inline constexpr uint32_t kObjectFlagAlphaMask = 1u << 0;
inline constexpr uint32_t kObjectFlagAlphaBlend = 1u << 1;
inline constexpr uint32_t kObjectFlagDoubleSided = 1u << 2;
inline constexpr uint32_t kObjectFlagSpecularGlossiness = 1u << 3;
inline constexpr uint32_t kObjectFlagUnlit = 1u << 4;
inline constexpr uint32_t kMaterialTextureDescriptorCapacity = 4096;
inline constexpr uint32_t kMaterialSamplerWrapModeCount = 3;
inline constexpr uint32_t kMaterialSamplerDescriptorCapacity =
    kMaterialSamplerWrapModeCount * kMaterialSamplerWrapModeCount;
inline constexpr uint32_t kMaterialSamplerWrapRepeat = 0;
inline constexpr uint32_t kMaterialSamplerWrapClampToEdge = 1;
inline constexpr uint32_t kMaterialSamplerWrapMirroredRepeat = 2;
inline constexpr uint32_t kMaxExactGBufferMaterialMetadataIndex =
    (1u << 23) - 1u;

struct GpuTextureMetadata {
  alignas(4) uint32_t samplerIndex{0};
  alignas(4) uint32_t padding0{0};
  alignas(4) uint32_t padding1{0};
  alignas(4) uint32_t padding2{0};
};

inline constexpr uint32_t kMaxDeferredPointLights = 12;
inline constexpr uint32_t kMaxClusteredLights = 8192;
inline constexpr uint32_t kTileSize = 16; // pixels
inline constexpr uint32_t kClusterDepthSlices = 16;
inline constexpr uint32_t kMaxLightsPerTile = 128;
inline constexpr float kUnboundedPointLightRange = 0.0f;
inline constexpr float kLightTypePoint = 0.0f;
inline constexpr float kLightTypeSpot = 1.0f;
inline constexpr uint32_t kMaxAreaLights = 256;
inline constexpr float kAreaLightTypeRectangle = 0.0f;
inline constexpr float kAreaLightTypeDisk = 1.0f;
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr uint32_t kShadowMapResolution = 4096;
inline constexpr uint32_t kLocalShadowMapResolution = 2048;
inline constexpr uint32_t kMaxShadowedLocalLightLayers = 24;
inline constexpr uint32_t kLocalShadowPointFaceCount = 6;
inline constexpr uint32_t kLocalShadowSpotLayerCount = 1;
inline constexpr uint32_t kLocalShadowAreaLayerCount = kLocalShadowPointFaceCount;
inline constexpr uint32_t kLocalShadowAreaRefPackedCount =
    (kMaxAreaLights + 3u) / 4u;
inline constexpr float kLocalShadowTypePoint = 0.0f;
inline constexpr float kLocalShadowTypeSpot = 1.0f;
inline constexpr float kLocalShadowTypeArea = 2.0f;
inline constexpr float kDefaultDirectionalPcssLightRadiusDegrees = 0.27f;
inline constexpr float kDefaultDirectionalPcssTanLightRadius = 0.004712424f;
inline constexpr float kDefaultDirectionalContactMaxDistance = 0.35f;
inline constexpr float kDefaultDirectionalContactThickness = 0.04f;
inline constexpr float kDefaultDirectionalContactFadeDistance = 0.30f;

struct LightingSettings {
  uint32_t preset{0};
  float density{1.0f};
  float radiusScale{1.0f};
  float intensityScale{1.0f};
  float directionalIntensity{2.0f};
  float environmentIntensity{1.0f};
  float bounceIntensity{1.0f};
  uint32_t localShadowPointBudget{1};
  uint32_t localShadowLayerBudget{8};
};

struct LightCullingStats {
  uint32_t submittedLights{0};
  uint32_t activeClusters{0};
  uint32_t totalClusters{0};
  uint32_t maxLightsPerCluster{0};
  uint32_t droppedLightReferences{0};
  float clusterCullMs{0.0f};
  float clusteredLightingMs{0.0f};
};

struct ShadowSettings {
  float normalBiasMinTexels{1.5f};
  float normalBiasMaxTexels{3.5f};
  float slopeBiasScale{0.0012f};
  float receiverPlaneBiasScale{1.5f};
  float filterRadiusTexels{1.0f};
  float cascadeBlendFraction{0.1f};
  float constantDepthBias{0.00035f};
  float maxDepthBias{0.006f};
  float rasterConstantBias{-4.0f};
  float rasterSlopeBias{-1.5f};
  float directionalPcssLightRadiusDegrees{
      kDefaultDirectionalPcssLightRadiusDegrees};
  float directionalPcssBlockerSearchRadiusTexels{8.0f};
  float directionalPcssMaxFilterRadiusTexels{12.0f};
  float directionalContactMaxDistance{kDefaultDirectionalContactMaxDistance};
  float directionalContactThickness{kDefaultDirectionalContactThickness};
  float directionalContactFadeDistance{kDefaultDirectionalContactFadeDistance};
  bool directionalPcssEnabled{true};
  bool directionalContactVisibility{false};
  bool localContactVisibility{true};
};

struct ShadowCascadeData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(4) float splitDepth{0.0f};
  alignas(4) float texelSize{0.0f};
  alignas(4) float worldRadius{0.0f};
  alignas(4) float depthRange{0.0f};
};

struct ShadowData {
  ShadowCascadeData cascades[kShadowCascadeCount];
  alignas(16) glm::vec4 biasSettings{1.5f, 3.5f, 0.0012f, 1.5f};
  alignas(16) glm::vec4 filterSettings{1.0f, 0.1f, 0.00035f, 0.006f};
  // x = directional PCSS enabled, y = tan(light angular radius),
  // z = blocker-search radius in texels, w = max adaptive filter radius.
  alignas(16) glm::vec4 softShadowSettings{
      1.0f, kDefaultDirectionalPcssTanLightRadius, 8.0f, 12.0f};
  // x = directional contact enabled, y = max ray distance in world units,
  // z = ray thickness in world units, w = fade distance in world units.
  alignas(16) glm::vec4 contactShadowSettings{
      0.0f, kDefaultDirectionalContactMaxDistance,
      kDefaultDirectionalContactThickness,
      kDefaultDirectionalContactFadeDistance};
};

struct LocalShadowLayerData {
  alignas(16) glm::mat4 viewProj{1.0f};
  // xyz = light/sample position, w = finite shadow range.
  alignas(16) glm::vec4 positionRange{0.0f, 0.0f, 0.0f, 10.0f};
  // xyz = layer forward direction, w = kLocalShadowType*.
  alignas(16) glm::vec4 directionType{0.0f, 0.0f, -1.0f, kLocalShadowTypePoint};
  // x = source light index, y = cube face index, z = layer count for the
  // source light (all emitter cubes for an area light), w = enabled.
  alignas(16) glm::uvec4 meta{0u, 0u, 0u, 0u};
  // x = texel size in world units, y = depth range, z = outer cone cosine,
  // w = source radius in world units for soft local-shadow filtering.
  alignas(16) glm::vec4 params{0.0f, 0.0f, 0.0f, 0.0f};
};

struct LocalShadowData {
  LocalShadowLayerData layers[kMaxShadowedLocalLightLayers];
  // Packed base-layer refs for area lights: each scalar stores baseLayer + 1,
  // or 0 when that area light has no local shadow layer.
  alignas(16) glm::uvec4 areaLightRefs[kLocalShadowAreaRefPackedCount]{};
  // x = active layer count, y = max layer count, z = map resolution,
  // w = enabled.
  alignas(16) glm::uvec4 counts{0u, kMaxShadowedLocalLightLayers,
                                kLocalShadowMapResolution, 0u};
  alignas(16) glm::vec4 biasSettings{1.5f, 3.5f, 0.0012f, 1.5f};
  // x = base PCF radius in texels, y = max soft PCF radius in texels,
  // z = constant depth bias, w = max depth bias.
  alignas(16) glm::vec4 filterSettings{1.0f, 28.0f, 0.00035f, 0.006f};
};

struct ShadowCascadeCullData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(16) glm::mat4 lightView{1.0f};
  alignas(16) glm::vec4 receiverMinBounds{0.0f};
  alignas(16) glm::vec4 receiverMaxBounds{0.0f};
  alignas(16) glm::vec4 casterMinBounds{0.0f};
  alignas(16) glm::vec4 casterMaxBounds{0.0f};
};

struct ShadowCullData {
  ShadowCascadeCullData cascades[kShadowCascadeCount];
};

struct ShadowCullPushConstants {
  uint32_t drawCount{0};
  uint32_t cascadeIndex{0};
  uint32_t outputOffset{0};
  uint32_t objectCount{0};
};

struct ShadowCullCountData {
  std::array<uint32_t, kShadowCascadeCount> visibleCounts{};
};

struct ShadowPushConstants {
  uint32_t objectIndex{0};
  uint32_t cascadeIndex{0};
  uint32_t sectionPlaneEnabled{0};
  uint32_t padding0{0};
  alignas(16) glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
};

struct PointLightData {
  // positionRadius.w > 0 is a finite fade/cull range; 0 is an unbounded
  // KHR_lights_punctual-style range. Light energy is carried by
  // colorIntensity.a as scene-linear point/spot-light intensity.
  alignas(16) glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  alignas(16) glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  // directionInnerCos.xyz is the light's forward direction for spot lights.
  // directionInnerCos.w and coneOuterCosType.x are cosine cone limits.
  // coneOuterCosType.y stores the light type: 0 = point, 1 = spot.
  alignas(16) glm::vec4 directionInnerCos{0.0f, 0.0f, -1.0f, 1.0f};
  alignas(16) glm::vec4 coneOuterCosType{0.0f, 0.0f, 0.0f, 0.0f};
};

struct AreaLightData {
  // positionRange.w > 0 is a finite fade/cull range; 0 means unbounded.
  alignas(16) glm::vec4 positionRange{0.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  // directionType.xyz is the emitting normal; .w stores the area shape:
  // 0 = rectangle, 1 = disk.
  alignas(16) glm::vec4 directionType{0.0f, 0.0f, -1.0f,
                                      kAreaLightTypeRectangle};
  // tangentHalfSize.w stores rectangle half-width or disk radius.
  alignas(16) glm::vec4 tangentHalfSize{1.0f, 0.0f, 0.0f, 0.5f};
  // bitangentHalfSize.w stores rectangle half-height or disk radius.
  alignas(16) glm::vec4 bitangentHalfSize{0.0f, 1.0f, 0.0f, 0.5f};
};

inline constexpr uint32_t kExposureModeManual = 0u;
inline constexpr uint32_t kExposureModeAuto = 1u;

struct ExposureSettings {
  uint32_t mode{kExposureModeManual};
  float manualExposure{0.25f};
  float targetLuminance{0.18f};
  float minExposure{0.03125f};
  float maxExposure{8.0f};
  float adaptationRate{1.5f};
  float meteringLowPercentile{0.50f};
  float meteringHighPercentile{0.95f};
};

struct ExposureStateData {
  alignas(4) float exposure{0.25f};
  alignas(4) float averageLuminance{0.18f};
  alignas(4) float targetExposure{0.25f};
  alignas(4) float initialized{0.0f};
};

struct PostProcessPushConstants {
  uint32_t outputMode{0};
  uint32_t bloomEnabled{0};
  float bloomIntensity{0.3f};
  float exposure{0.25f};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  float cascadeSplits[kShadowCascadeCount]{};
  uint32_t tileCountX{0};
  uint32_t totalLights{0};
  uint32_t depthSliceCount{kClusterDepthSlices};
  uint32_t oitEnabled{0};
  uint32_t exposureMode{kExposureModeManual};
  float targetLuminance{0.18f};
  float minExposure{0.03125f};
  float maxExposure{8.0f};
  float adaptationRate{1.5f};
};

struct LightingData {
  alignas(16) glm::vec4 directionalDirection{0.0f, 0.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 directionalColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  alignas(4) uint32_t pointLightCount{0};
  alignas(4) uint32_t shadowEnabled{0};
  alignas(4) uint32_t gtaoEnabled{0};
  alignas(4) uint32_t tileCullEnabled{0};
  alignas(4) uint32_t prefilteredMipCount{1};
  alignas(4) float environmentIntensity{1.0f};
  alignas(4) uint32_t areaLightCount{0};
  alignas(4) uint32_t localShadowEnabled{0};
  alignas(4) float bounceIntensity{1.0f};
  alignas(4) uint32_t localContactVisibility{0};
  alignas(4) uint32_t padding1{0};
  alignas(4) uint32_t padding2{0};
};

struct TileLightGrid {
  uint32_t offset{0};
  uint32_t count{0};
};

struct TileCullPushConstants {
  uint32_t tileCountX{0};
  uint32_t tileCountY{0};
  uint32_t depthSliceCount{kClusterDepthSlices};
  uint32_t totalLights{0};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
};

struct TiledLightingPushConstants {
  uint32_t tileCountX{0};
  uint32_t tileCountY{0};
  uint32_t depthSliceCount{kClusterDepthSlices};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  uint32_t contactVisibilityEnabled{0};
  uint32_t localShadowEnabled{0};
  float bounceIntensity{1.0f};
};

// GPU-driven rendering: matches VkDrawIndexedIndirectCommand layout.
struct GpuDrawIndexedIndirectCommand {
  uint32_t indexCount{0};
  uint32_t instanceCount{0};
  uint32_t firstIndex{0};
  int32_t vertexOffset{0};
  uint32_t firstInstance{0}; // Encodes objectIndex.
};
static_assert(sizeof(GpuDrawIndexedIndirectCommand) == 20,
              "GpuDrawIndexedIndirectCommand size mismatch with "
              "shaders/draw_indirect_common.slang.");
static_assert(offsetof(GpuDrawIndexedIndirectCommand, indexCount) == 0,
              "GpuDrawIndexedIndirectCommand.indexCount offset");
static_assert(offsetof(GpuDrawIndexedIndirectCommand, instanceCount) == 4,
              "GpuDrawIndexedIndirectCommand.instanceCount offset");
static_assert(offsetof(GpuDrawIndexedIndirectCommand, firstIndex) == 8,
              "GpuDrawIndexedIndirectCommand.firstIndex offset");
static_assert(offsetof(GpuDrawIndexedIndirectCommand, vertexOffset) == 12,
              "GpuDrawIndexedIndirectCommand.vertexOffset offset");
static_assert(offsetof(GpuDrawIndexedIndirectCommand, firstInstance) == 16,
              "GpuDrawIndexedIndirectCommand.firstInstance offset");

static_assert(kMaterialSamplerDescriptorCapacity == 9,
              "Material sampler table currently stores S/T wrap combinations.");
static_assert(sizeof(GpuTextureMetadata) == 16,
              "GpuTextureMetadata size mismatch with shader layout.");
static_assert(alignof(GpuTextureMetadata) == 4,
              "GpuTextureMetadata must remain scalar-aligned.");
static_assert(offsetof(GpuTextureMetadata, samplerIndex) == 0,
              "GpuTextureMetadata.samplerIndex offset");

struct CullPushConstants {
  uint32_t objectCount{0};
  uint32_t pad0{0};
  uint32_t pad1{0};
  uint32_t pad2{0};
};

struct HiZPushConstants {
  uint32_t srcWidth{0};
  uint32_t srcHeight{0};
  uint32_t dstMipLevel{0};
  uint32_t pad0{0};
};

struct GBufferCompositePushConstants {
  uint32_t outputMode{0};
};

struct BloomDownsamplePushConstants {
  uint32_t srcWidth{0};
  uint32_t srcHeight{0};
  uint32_t dstWidth{0};
  uint32_t dstHeight{0};
  float threshold{1.0f};
  float knee{0.1f};
  uint32_t mipLevel{0};
  uint32_t pad0{0};
};

struct BloomUpsamplePushConstants {
  uint32_t srcWidth{0};
  uint32_t srcHeight{0};
  uint32_t dstWidth{0};
  uint32_t dstHeight{0};
  float filterRadius{0.005f};
  float bloomIntensity{0.3f};
  uint32_t isFinalPass{0};
  uint32_t pad0{0};
};

struct ExposureHistogramPushConstants {
  uint32_t width{0};
  uint32_t height{0};
  uint32_t binCount{64};
  float minLogLuminance{-12.0f};
  float maxLogLuminance{12.0f};
  float pad0{0.0f};
  float pad1{0.0f};
  float pad2{0.0f};
};

struct ExposureAdaptPushConstants {
  uint32_t binCount{64};
  uint32_t exposureMode{kExposureModeAuto};
  float minLogLuminance{-12.0f};
  float maxLogLuminance{12.0f};
  float targetLuminance{0.18f};
  float minExposure{0.03125f};
  float maxExposure{8.0f};
  float adaptationRate{1.5f};
  float meteringLowPercentile{0.50f};
  float meteringHighPercentile{0.95f};
  float deltaSeconds{1.0f / 60.0f};
  float manualExposure{0.25f};
  float pad0{0.0f};
  float pad1{0.0f};
  float pad2{0.0f};
  float pad3{0.0f};
};

struct EquirectPushConstants {
  uint32_t faceIndex{0};
  uint32_t faceSize{0};
};

struct IrradiancePushConstants {
  uint32_t faceIndex{0};
  uint32_t faceSize{0};
};

struct PrefilterPushConstants {
  uint32_t faceIndex{0};
  uint32_t faceSize{0};
  float roughness{0.0f};
  uint32_t sourceMipCount{1};
};

struct GtaoPushConstants {
  float aoRadius{1.5f};
  float aoIntensity{1.0f};
  uint32_t sampleCount{16};
  uint32_t pad0{0};
  uint32_t fullWidth{0};
  uint32_t fullHeight{0};
  uint32_t pad1{0};
  uint32_t pad2{0};
};

struct BlurPushConstants {
  uint32_t width{0};
  uint32_t height{0};
  float depthThreshold{0.08f};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  uint32_t orthographicDepth{0};
  float pad1{0.0f};
  float pad2{0.0f};
};

// ---------------------------------------------------------------------------
// Compile-time layout verification.
//
// These asserts guard the host <-> shader struct layout contract. The shader
// counterparts live in `shaders/lighting_structs.slang` (LightingBuffer,
// ShadowBuffer, CameraBuffer, PointLightData, ShadowCascadeData), in
// `shaders/object_data_common.slang` (ObjectBuffer), and in
// `shaders/material_data_common.slang` (GpuMaterial).
//
// If any field is added/removed or an `alignas` is changed, the shader side
// must be updated in lockstep and these numbers re-derived. The sizes below
// are the std140-compatible layouts used when these structs are uploaded to
// uniform or storage buffers.
// ---------------------------------------------------------------------------

static_assert(sizeof(CameraData) == 160,
              "CameraData size mismatch with shaders/lighting_structs.slang "
              "CameraBuffer. Update shader layout in lockstep.");
static_assert(alignof(CameraData) == 16, "CameraData must be 16-byte aligned.");
static_assert(offsetof(CameraData, viewProj) == 0,
              "CameraData.viewProj offset");
static_assert(offsetof(CameraData, inverseViewProj) == 64,
              "CameraData.inverseViewProj offset");
static_assert(offsetof(CameraData, cameraWorldPosition) == 128,
              "CameraData.cameraWorldPosition offset");
static_assert(offsetof(CameraData, cameraForward) == 144,
              "CameraData.cameraForward offset");

static_assert(sizeof(PointLightData) == 64,
              "PointLightData size mismatch with shader PointLightData.");
static_assert(alignof(PointLightData) == 16,
              "PointLightData must be 16-byte aligned.");
static_assert(offsetof(PointLightData, positionRadius) == 0,
              "PointLightData.positionRadius offset");
static_assert(offsetof(PointLightData, colorIntensity) == 16,
              "PointLightData.colorIntensity offset");
static_assert(offsetof(PointLightData, directionInnerCos) == 32,
              "PointLightData.directionInnerCos offset");
static_assert(offsetof(PointLightData, coneOuterCosType) == 48,
              "PointLightData.coneOuterCosType offset");

static_assert(sizeof(AreaLightData) == 80,
              "AreaLightData size mismatch with shader AreaLightData.");
static_assert(alignof(AreaLightData) == 16,
              "AreaLightData must be 16-byte aligned.");
static_assert(offsetof(AreaLightData, positionRange) == 0,
              "AreaLightData.positionRange offset");
static_assert(offsetof(AreaLightData, colorIntensity) == 16,
              "AreaLightData.colorIntensity offset");
static_assert(offsetof(AreaLightData, directionType) == 32,
              "AreaLightData.directionType offset");
static_assert(offsetof(AreaLightData, tangentHalfSize) == 48,
              "AreaLightData.tangentHalfSize offset");
static_assert(offsetof(AreaLightData, bitangentHalfSize) == 64,
              "AreaLightData.bitangentHalfSize offset");

static_assert(sizeof(LightingData) == 80,
              "LightingData size mismatch with shaders/lighting_structs.slang "
              "LightingBuffer. Update shader layout in lockstep.");
static_assert(alignof(LightingData) == 16,
              "LightingData must be 16-byte aligned.");
static_assert(offsetof(LightingData, directionalDirection) == 0,
              "LightingData.directionalDirection offset");
static_assert(offsetof(LightingData, directionalColorIntensity) == 16,
              "LightingData.directionalColorIntensity offset");
static_assert(offsetof(LightingData, pointLightCount) == 32,
              "LightingData.pointLightCount offset");
static_assert(offsetof(LightingData, shadowEnabled) == 36,
              "LightingData.shadowEnabled offset");
static_assert(offsetof(LightingData, gtaoEnabled) == 40,
              "LightingData.gtaoEnabled offset");
static_assert(offsetof(LightingData, tileCullEnabled) == 44,
              "LightingData.tileCullEnabled offset");
static_assert(offsetof(LightingData, prefilteredMipCount) == 48,
              "LightingData.prefilteredMipCount offset");
static_assert(offsetof(LightingData, environmentIntensity) == 52,
              "LightingData.environmentIntensity offset");
static_assert(offsetof(LightingData, areaLightCount) == 56,
              "LightingData.areaLightCount offset");
static_assert(offsetof(LightingData, localShadowEnabled) == 60,
              "LightingData.localShadowEnabled offset");
static_assert(offsetof(LightingData, bounceIntensity) == 64,
              "LightingData.bounceIntensity offset");
static_assert(offsetof(LightingData, localContactVisibility) == 68,
              "LightingData.localContactVisibility offset");
static_assert(sizeof(ShadowCascadeData) == 80,
              "ShadowCascadeData size mismatch with shader ShadowCascadeData.");
static_assert(alignof(ShadowCascadeData) == 16,
              "ShadowCascadeData must be 16-byte aligned.");
static_assert(offsetof(ShadowCascadeData, viewProj) == 0,
              "ShadowCascadeData.viewProj offset");
static_assert(offsetof(ShadowCascadeData, splitDepth) == 64,
              "ShadowCascadeData.splitDepth offset");
static_assert(offsetof(ShadowCascadeData, texelSize) == 68,
              "ShadowCascadeData.texelSize offset");
static_assert(offsetof(ShadowCascadeData, worldRadius) == 72,
              "ShadowCascadeData.worldRadius offset");
static_assert(offsetof(ShadowCascadeData, depthRange) == 76,
              "ShadowCascadeData.depthRange offset");

static_assert(sizeof(ShadowSettings) == 68,
              "ShadowSettings stores shader vectors, PCSS controls, raster "
              "depth-bias controls, and contact-visibility state.");
static_assert(offsetof(ShadowSettings, normalBiasMinTexels) == 0,
              "ShadowSettings.normalBiasMinTexels offset");
static_assert(offsetof(ShadowSettings, normalBiasMaxTexels) == 4,
              "ShadowSettings.normalBiasMaxTexels offset");
static_assert(offsetof(ShadowSettings, slopeBiasScale) == 8,
              "ShadowSettings.slopeBiasScale offset");
static_assert(offsetof(ShadowSettings, receiverPlaneBiasScale) == 12,
              "ShadowSettings.receiverPlaneBiasScale offset");
static_assert(offsetof(ShadowSettings, filterRadiusTexels) == 16,
              "ShadowSettings.filterRadiusTexels offset");
static_assert(offsetof(ShadowSettings, cascadeBlendFraction) == 20,
              "ShadowSettings.cascadeBlendFraction offset");
static_assert(offsetof(ShadowSettings, constantDepthBias) == 24,
              "ShadowSettings.constantDepthBias offset");
static_assert(offsetof(ShadowSettings, maxDepthBias) == 28,
              "ShadowSettings.maxDepthBias offset");
static_assert(offsetof(ShadowSettings, rasterConstantBias) == 32,
              "ShadowSettings.rasterConstantBias offset");
static_assert(offsetof(ShadowSettings, rasterSlopeBias) == 36,
              "ShadowSettings.rasterSlopeBias offset");
static_assert(offsetof(ShadowSettings, directionalPcssLightRadiusDegrees) == 40,
              "ShadowSettings.directionalPcssLightRadiusDegrees offset");
static_assert(offsetof(ShadowSettings,
                       directionalPcssBlockerSearchRadiusTexels) == 44,
              "ShadowSettings.directionalPcssBlockerSearchRadiusTexels "
              "offset");
static_assert(offsetof(ShadowSettings,
                       directionalPcssMaxFilterRadiusTexels) == 48,
              "ShadowSettings.directionalPcssMaxFilterRadiusTexels offset");
static_assert(offsetof(ShadowSettings, directionalContactMaxDistance) == 52,
              "ShadowSettings.directionalContactMaxDistance offset");
static_assert(offsetof(ShadowSettings, directionalContactThickness) == 56,
              "ShadowSettings.directionalContactThickness offset");
static_assert(offsetof(ShadowSettings, directionalContactFadeDistance) == 60,
              "ShadowSettings.directionalContactFadeDistance offset");
static_assert(offsetof(ShadowSettings, directionalPcssEnabled) == 64,
              "ShadowSettings.directionalPcssEnabled offset");
static_assert(offsetof(ShadowSettings, directionalContactVisibility) == 65,
              "ShadowSettings.directionalContactVisibility offset");
static_assert(offsetof(ShadowSettings, localContactVisibility) == 66,
              "ShadowSettings.localContactVisibility offset");

static_assert(sizeof(ShadowData) == 80 * kShadowCascadeCount + 64,
              "ShadowData size mismatch with shaders/lighting_structs.slang "
              "ShadowBuffer. Update shader layout in lockstep.");
static_assert(alignof(ShadowData) == 16, "ShadowData must be 16-byte aligned.");
static_assert(offsetof(ShadowData, cascades) == 0,
              "ShadowData.cascades offset");
static_assert(offsetof(ShadowData, biasSettings) == 320,
              "ShadowData.biasSettings offset");
static_assert(offsetof(ShadowData, filterSettings) == 336,
              "ShadowData.filterSettings offset");
static_assert(offsetof(ShadowData, softShadowSettings) == 352,
              "ShadowData.softShadowSettings offset");
static_assert(offsetof(ShadowData, contactShadowSettings) == 368,
              "ShadowData.contactShadowSettings offset");

static_assert(sizeof(LocalShadowLayerData) == 128,
              "LocalShadowLayerData size mismatch with shader "
              "LocalShadowLayerData.");
static_assert(alignof(LocalShadowLayerData) == 16,
              "LocalShadowLayerData must be 16-byte aligned.");
static_assert(offsetof(LocalShadowLayerData, viewProj) == 0,
              "LocalShadowLayerData.viewProj offset");
static_assert(offsetof(LocalShadowLayerData, positionRange) == 64,
              "LocalShadowLayerData.positionRange offset");
static_assert(offsetof(LocalShadowLayerData, directionType) == 80,
              "LocalShadowLayerData.directionType offset");
static_assert(offsetof(LocalShadowLayerData, meta) == 96,
              "LocalShadowLayerData.meta offset");
static_assert(offsetof(LocalShadowLayerData, params) == 112,
              "LocalShadowLayerData.params offset");
static_assert(sizeof(LocalShadowData) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers +
                      sizeof(glm::uvec4) * kLocalShadowAreaRefPackedCount +
                      sizeof(glm::uvec4) + sizeof(glm::vec4) * 2,
              "LocalShadowData size mismatch with shader LocalShadowBuffer.");
static_assert(alignof(LocalShadowData) == 16,
              "LocalShadowData must be 16-byte aligned.");
static_assert(offsetof(LocalShadowData, layers) == 0,
              "LocalShadowData.layers offset");
static_assert(offsetof(LocalShadowData, areaLightRefs) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers,
              "LocalShadowData.areaLightRefs offset");
static_assert(offsetof(LocalShadowData, counts) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers +
                      sizeof(glm::uvec4) * kLocalShadowAreaRefPackedCount,
              "LocalShadowData.counts offset");
static_assert(offsetof(LocalShadowData, biasSettings) ==
                  offsetof(LocalShadowData, counts) + sizeof(glm::uvec4),
              "LocalShadowData.biasSettings offset");
static_assert(offsetof(LocalShadowData, filterSettings) ==
                  offsetof(LocalShadowData, biasSettings) + sizeof(glm::vec4),
              "LocalShadowData.filterSettings offset");

static_assert(
    sizeof(ShadowCascadeCullData) == 192,
    "ShadowCascadeCullData size mismatch with shaders/lighting_structs.slang "
    "ShadowCascadeCullData. Update shader layout in lockstep.");
static_assert(alignof(ShadowCascadeCullData) == 16,
              "ShadowCascadeCullData must be 16-byte aligned.");
static_assert(offsetof(ShadowCascadeCullData, viewProj) == 0,
              "ShadowCascadeCullData.viewProj offset");
static_assert(offsetof(ShadowCascadeCullData, lightView) == 64,
              "ShadowCascadeCullData.lightView offset");
static_assert(offsetof(ShadowCascadeCullData, receiverMinBounds) == 128,
              "ShadowCascadeCullData.receiverMinBounds offset");
static_assert(offsetof(ShadowCascadeCullData, receiverMaxBounds) == 144,
              "ShadowCascadeCullData.receiverMaxBounds offset");
static_assert(offsetof(ShadowCascadeCullData, casterMinBounds) == 160,
              "ShadowCascadeCullData.casterMinBounds offset");
static_assert(offsetof(ShadowCascadeCullData, casterMaxBounds) == 176,
              "ShadowCascadeCullData.casterMaxBounds offset");

static_assert(
    sizeof(ShadowCullData) ==
        sizeof(ShadowCascadeCullData) * kShadowCascadeCount,
    "ShadowCullData size mismatch with shaders/lighting_structs.slang "
    "ShadowCullData. Update shader layout in lockstep.");
static_assert(alignof(ShadowCullData) == 16,
              "ShadowCullData must be 16-byte aligned.");
static_assert(sizeof(ShadowCullPushConstants) == 16,
              "ShadowCullPushConstants must remain 16 bytes.");
static_assert(offsetof(ShadowCullPushConstants, drawCount) == 0,
              "ShadowCullPushConstants.drawCount offset");
static_assert(offsetof(ShadowCullPushConstants, cascadeIndex) == 4,
              "ShadowCullPushConstants.cascadeIndex offset");
static_assert(offsetof(ShadowCullPushConstants, outputOffset) == 8,
              "ShadowCullPushConstants.outputOffset offset");
static_assert(offsetof(ShadowCullPushConstants, objectCount) == 12,
              "ShadowCullPushConstants.objectCount offset");
static_assert(
    sizeof(ShadowCullCountData) == sizeof(uint32_t) * kShadowCascadeCount,
    "ShadowCullCountData stores one visible count per shadow cascade.");

static_assert(sizeof(BindlessPushConstants) == 32,
              "BindlessPushConstants size mismatch with "
              "shaders/push_constants_common.slang BindlessPushConstants.");
static_assert(offsetof(BindlessPushConstants, objectIndex) == 0,
              "BindlessPushConstants.objectIndex offset");
static_assert(offsetof(BindlessPushConstants, sectionPlaneEnabled) == 4,
              "BindlessPushConstants.sectionPlaneEnabled offset");
static_assert(offsetof(BindlessPushConstants, semanticColorMode) == 8,
              "BindlessPushConstants.semanticColorMode offset");
static_assert(offsetof(BindlessPushConstants, sectionPlane) == 16,
              "BindlessPushConstants.sectionPlane offset");

static_assert(sizeof(ShadowPushConstants) == 32,
              "ShadowPushConstants size mismatch with "
              "shaders/push_constants_common.slang ShadowPushConstants.");
static_assert(offsetof(ShadowPushConstants, objectIndex) == 0,
              "ShadowPushConstants.objectIndex offset");
static_assert(offsetof(ShadowPushConstants, cascadeIndex) == 4,
              "ShadowPushConstants.cascadeIndex offset");
static_assert(offsetof(ShadowPushConstants, sectionPlaneEnabled) == 8,
              "ShadowPushConstants.sectionPlaneEnabled offset");
static_assert(offsetof(ShadowPushConstants, sectionPlane) == 16,
              "ShadowPushConstants.sectionPlane offset");

static_assert(
    sizeof(ExposureSettings) == 32,
    "ExposureSettings stores CPU-side post-process exposure controls.");
static_assert(offsetof(ExposureSettings, mode) == 0,
              "ExposureSettings.mode offset");
static_assert(offsetof(ExposureSettings, manualExposure) == 4,
              "ExposureSettings.manualExposure offset");
static_assert(offsetof(ExposureSettings, targetLuminance) == 8,
              "ExposureSettings.targetLuminance offset");
static_assert(offsetof(ExposureSettings, minExposure) == 12,
              "ExposureSettings.minExposure offset");
static_assert(offsetof(ExposureSettings, maxExposure) == 16,
              "ExposureSettings.maxExposure offset");
static_assert(offsetof(ExposureSettings, adaptationRate) == 20,
              "ExposureSettings.adaptationRate offset");
static_assert(offsetof(ExposureSettings, meteringLowPercentile) == 24,
              "ExposureSettings.meteringLowPercentile offset");
static_assert(offsetof(ExposureSettings, meteringHighPercentile) == 28,
              "ExposureSettings.meteringHighPercentile offset");

static_assert(sizeof(PostProcessPushConstants) == 76,
              "PostProcessPushConstants size mismatch with "
              "shaders/push_constants_common.slang PostProcessPushConstants.");
static_assert(offsetof(PostProcessPushConstants, outputMode) == 0,
              "PostProcessPushConstants.outputMode offset");
static_assert(offsetof(PostProcessPushConstants, bloomEnabled) == 4,
              "PostProcessPushConstants.bloomEnabled offset");
static_assert(offsetof(PostProcessPushConstants, bloomIntensity) == 8,
              "PostProcessPushConstants.bloomIntensity offset");
static_assert(offsetof(PostProcessPushConstants, exposure) == 12,
              "PostProcessPushConstants.exposure offset");
static_assert(offsetof(PostProcessPushConstants, cameraNear) == 16,
              "PostProcessPushConstants.cameraNear offset");
static_assert(offsetof(PostProcessPushConstants, cameraFar) == 20,
              "PostProcessPushConstants.cameraFar offset");
static_assert(offsetof(PostProcessPushConstants, cascadeSplits) == 24,
              "PostProcessPushConstants.cascadeSplits offset");
static_assert(offsetof(PostProcessPushConstants, tileCountX) == 40,
              "PostProcessPushConstants.tileCountX offset");
static_assert(offsetof(PostProcessPushConstants, totalLights) == 44,
              "PostProcessPushConstants.totalLights offset");
static_assert(offsetof(PostProcessPushConstants, depthSliceCount) == 48,
              "PostProcessPushConstants.depthSliceCount offset");
static_assert(offsetof(PostProcessPushConstants, oitEnabled) == 52,
              "PostProcessPushConstants.oitEnabled offset");
static_assert(offsetof(PostProcessPushConstants, exposureMode) == 56,
              "PostProcessPushConstants.exposureMode offset");
static_assert(offsetof(PostProcessPushConstants, targetLuminance) == 60,
              "PostProcessPushConstants.targetLuminance offset");
static_assert(offsetof(PostProcessPushConstants, minExposure) == 64,
              "PostProcessPushConstants.minExposure offset");
static_assert(offsetof(PostProcessPushConstants, maxExposure) == 68,
              "PostProcessPushConstants.maxExposure offset");
static_assert(offsetof(PostProcessPushConstants, adaptationRate) == 72,
              "PostProcessPushConstants.adaptationRate offset");

static_assert(sizeof(TileLightGrid) == 8,
              "TileLightGrid size mismatch with shaders/lighting_structs.slang "
              "TileLightGrid.");
static_assert(offsetof(TileLightGrid, offset) == 0,
              "TileLightGrid.offset offset");
static_assert(offsetof(TileLightGrid, count) == 4,
              "TileLightGrid.count offset");

static_assert(sizeof(TileCullPushConstants) == 24,
              "TileCullPushConstants size mismatch with "
              "shaders/push_constants_common.slang TileCullPushConstants.");
static_assert(offsetof(TileCullPushConstants, tileCountX) == 0,
              "TileCullPushConstants.tileCountX offset");
static_assert(offsetof(TileCullPushConstants, tileCountY) == 4,
              "TileCullPushConstants.tileCountY offset");
static_assert(offsetof(TileCullPushConstants, depthSliceCount) == 8,
              "TileCullPushConstants.depthSliceCount offset");
static_assert(offsetof(TileCullPushConstants, totalLights) == 12,
              "TileCullPushConstants.totalLights offset");
static_assert(offsetof(TileCullPushConstants, cameraNear) == 16,
              "TileCullPushConstants.cameraNear offset");
static_assert(offsetof(TileCullPushConstants, cameraFar) == 20,
              "TileCullPushConstants.cameraFar offset");

static_assert(
    sizeof(TiledLightingPushConstants) == 32,
    "TiledLightingPushConstants size mismatch with "
    "shaders/push_constants_common.slang TiledLightingPushConstants.");
static_assert(offsetof(TiledLightingPushConstants, tileCountX) == 0,
              "TiledLightingPushConstants.tileCountX offset");
static_assert(offsetof(TiledLightingPushConstants, tileCountY) == 4,
              "TiledLightingPushConstants.tileCountY offset");
static_assert(offsetof(TiledLightingPushConstants, depthSliceCount) == 8,
              "TiledLightingPushConstants.depthSliceCount offset");
static_assert(offsetof(TiledLightingPushConstants, cameraNear) == 12,
              "TiledLightingPushConstants.cameraNear offset");
static_assert(offsetof(TiledLightingPushConstants, cameraFar) == 16,
              "TiledLightingPushConstants.cameraFar offset");
static_assert(offsetof(TiledLightingPushConstants, contactVisibilityEnabled) == 20,
              "TiledLightingPushConstants.contactVisibilityEnabled offset");
static_assert(offsetof(TiledLightingPushConstants, localShadowEnabled) == 24,
              "TiledLightingPushConstants.localShadowEnabled offset");
static_assert(offsetof(TiledLightingPushConstants, bounceIntensity) == 28,
              "TiledLightingPushConstants.bounceIntensity offset");

static_assert(sizeof(CullPushConstants) == 16,
              "CullPushConstants size mismatch with "
              "shaders/push_constants_common.slang CullPushConstants.");
static_assert(offsetof(CullPushConstants, objectCount) == 0,
              "CullPushConstants.objectCount offset");
static_assert(offsetof(CullPushConstants, pad0) == 4,
              "CullPushConstants.pad0 offset");
static_assert(offsetof(CullPushConstants, pad1) == 8,
              "CullPushConstants.pad1 offset");
static_assert(offsetof(CullPushConstants, pad2) == 12,
              "CullPushConstants.pad2 offset");

static_assert(sizeof(HiZPushConstants) == 16,
              "HiZPushConstants size mismatch with "
              "shaders/push_constants_common.slang HiZPushConstants.");
static_assert(offsetof(HiZPushConstants, srcWidth) == 0,
              "HiZPushConstants.srcWidth offset");
static_assert(offsetof(HiZPushConstants, srcHeight) == 4,
              "HiZPushConstants.srcHeight offset");
static_assert(offsetof(HiZPushConstants, dstMipLevel) == 8,
              "HiZPushConstants.dstMipLevel offset");
static_assert(offsetof(HiZPushConstants, pad0) == 12,
              "HiZPushConstants.pad0 offset");

static_assert(
    sizeof(GBufferCompositePushConstants) == 4,
    "GBufferCompositePushConstants size mismatch with "
    "shaders/push_constants_common.slang GBufferCompositePushConstants.");
static_assert(offsetof(GBufferCompositePushConstants, outputMode) == 0,
              "GBufferCompositePushConstants.outputMode offset");

static_assert(
    sizeof(BloomDownsamplePushConstants) == 32,
    "BloomDownsamplePushConstants size mismatch with "
    "shaders/push_constants_common.slang BloomDownsamplePushConstants.");
static_assert(offsetof(BloomDownsamplePushConstants, srcWidth) == 0,
              "BloomDownsamplePushConstants.srcWidth offset");
static_assert(offsetof(BloomDownsamplePushConstants, srcHeight) == 4,
              "BloomDownsamplePushConstants.srcHeight offset");
static_assert(offsetof(BloomDownsamplePushConstants, dstWidth) == 8,
              "BloomDownsamplePushConstants.dstWidth offset");
static_assert(offsetof(BloomDownsamplePushConstants, dstHeight) == 12,
              "BloomDownsamplePushConstants.dstHeight offset");
static_assert(offsetof(BloomDownsamplePushConstants, threshold) == 16,
              "BloomDownsamplePushConstants.threshold offset");
static_assert(offsetof(BloomDownsamplePushConstants, knee) == 20,
              "BloomDownsamplePushConstants.knee offset");
static_assert(offsetof(BloomDownsamplePushConstants, mipLevel) == 24,
              "BloomDownsamplePushConstants.mipLevel offset");
static_assert(offsetof(BloomDownsamplePushConstants, pad0) == 28,
              "BloomDownsamplePushConstants.pad0 offset");

static_assert(
    sizeof(BloomUpsamplePushConstants) == 32,
    "BloomUpsamplePushConstants size mismatch with "
    "shaders/push_constants_common.slang BloomUpsamplePushConstants.");
static_assert(offsetof(BloomUpsamplePushConstants, srcWidth) == 0,
              "BloomUpsamplePushConstants.srcWidth offset");
static_assert(offsetof(BloomUpsamplePushConstants, srcHeight) == 4,
              "BloomUpsamplePushConstants.srcHeight offset");
static_assert(offsetof(BloomUpsamplePushConstants, dstWidth) == 8,
              "BloomUpsamplePushConstants.dstWidth offset");
static_assert(offsetof(BloomUpsamplePushConstants, dstHeight) == 12,
              "BloomUpsamplePushConstants.dstHeight offset");
static_assert(offsetof(BloomUpsamplePushConstants, filterRadius) == 16,
              "BloomUpsamplePushConstants.filterRadius offset");
static_assert(offsetof(BloomUpsamplePushConstants, bloomIntensity) == 20,
              "BloomUpsamplePushConstants.bloomIntensity offset");
static_assert(offsetof(BloomUpsamplePushConstants, isFinalPass) == 24,
              "BloomUpsamplePushConstants.isFinalPass offset");
static_assert(offsetof(BloomUpsamplePushConstants, pad0) == 28,
              "BloomUpsamplePushConstants.pad0 offset");

static_assert(
    sizeof(ExposureHistogramPushConstants) == 32,
    "ExposureHistogramPushConstants size mismatch with "
    "shaders/push_constants_common.slang ExposureHistogramPushConstants.");
static_assert(offsetof(ExposureHistogramPushConstants, width) == 0,
              "ExposureHistogramPushConstants.width offset");
static_assert(offsetof(ExposureHistogramPushConstants, height) == 4,
              "ExposureHistogramPushConstants.height offset");
static_assert(offsetof(ExposureHistogramPushConstants, binCount) == 8,
              "ExposureHistogramPushConstants.binCount offset");
static_assert(offsetof(ExposureHistogramPushConstants, minLogLuminance) == 12,
              "ExposureHistogramPushConstants.minLogLuminance offset");
static_assert(offsetof(ExposureHistogramPushConstants, maxLogLuminance) == 16,
              "ExposureHistogramPushConstants.maxLogLuminance offset");
static_assert(offsetof(ExposureHistogramPushConstants, pad0) == 20,
              "ExposureHistogramPushConstants.pad0 offset");
static_assert(offsetof(ExposureHistogramPushConstants, pad1) == 24,
              "ExposureHistogramPushConstants.pad1 offset");
static_assert(offsetof(ExposureHistogramPushConstants, pad2) == 28,
              "ExposureHistogramPushConstants.pad2 offset");

static_assert(sizeof(ExposureStateData) == 16,
              "ExposureStateData size mismatch with "
              "shaders/lighting_structs.slang ExposureStateData.");
static_assert(offsetof(ExposureStateData, exposure) == 0,
              "ExposureStateData.exposure offset");
static_assert(offsetof(ExposureStateData, averageLuminance) == 4,
              "ExposureStateData.averageLuminance offset");
static_assert(offsetof(ExposureStateData, targetExposure) == 8,
              "ExposureStateData.targetExposure offset");
static_assert(offsetof(ExposureStateData, initialized) == 12,
              "ExposureStateData.initialized offset");

static_assert(sizeof(LocalShadowLayerData) == 128,
              "LocalShadowLayerData size mismatch with "
              "shaders/lighting_structs.slang LocalShadowLayerData.");
static_assert(offsetof(LocalShadowLayerData, viewProj) == 0,
              "LocalShadowLayerData.viewProj offset");
static_assert(offsetof(LocalShadowLayerData, positionRange) == 64,
              "LocalShadowLayerData.positionRange offset");
static_assert(offsetof(LocalShadowLayerData, directionType) == 80,
              "LocalShadowLayerData.directionType offset");
static_assert(offsetof(LocalShadowLayerData, meta) == 96,
              "LocalShadowLayerData.meta offset");
static_assert(offsetof(LocalShadowLayerData, params) == 112,
              "LocalShadowLayerData.params offset");
static_assert(sizeof(LocalShadowData) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers +
                      sizeof(glm::uvec4) * kLocalShadowAreaRefPackedCount +
                      sizeof(glm::uvec4) + sizeof(glm::vec4) * 2,
              "LocalShadowData size mismatch with "
              "shaders/lighting_structs.slang LocalShadowBuffer.");
static_assert(offsetof(LocalShadowData, layers) == 0,
              "LocalShadowData.layers offset");
static_assert(offsetof(LocalShadowData, areaLightRefs) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers,
              "LocalShadowData.areaLightRefs offset");
static_assert(offsetof(LocalShadowData, counts) ==
                  sizeof(LocalShadowLayerData) * kMaxShadowedLocalLightLayers +
                      sizeof(glm::uvec4) * kLocalShadowAreaRefPackedCount,
              "LocalShadowData.counts offset");

static_assert(
    sizeof(ExposureAdaptPushConstants) == 64,
    "ExposureAdaptPushConstants size mismatch with "
    "shaders/push_constants_common.slang ExposureAdaptPushConstants.");
static_assert(offsetof(ExposureAdaptPushConstants, binCount) == 0,
              "ExposureAdaptPushConstants.binCount offset");
static_assert(offsetof(ExposureAdaptPushConstants, exposureMode) == 4,
              "ExposureAdaptPushConstants.exposureMode offset");
static_assert(offsetof(ExposureAdaptPushConstants, minLogLuminance) == 8,
              "ExposureAdaptPushConstants.minLogLuminance offset");
static_assert(offsetof(ExposureAdaptPushConstants, maxLogLuminance) == 12,
              "ExposureAdaptPushConstants.maxLogLuminance offset");
static_assert(offsetof(ExposureAdaptPushConstants, targetLuminance) == 16,
              "ExposureAdaptPushConstants.targetLuminance offset");
static_assert(offsetof(ExposureAdaptPushConstants, minExposure) == 20,
              "ExposureAdaptPushConstants.minExposure offset");
static_assert(offsetof(ExposureAdaptPushConstants, maxExposure) == 24,
              "ExposureAdaptPushConstants.maxExposure offset");
static_assert(offsetof(ExposureAdaptPushConstants, adaptationRate) == 28,
              "ExposureAdaptPushConstants.adaptationRate offset");
static_assert(offsetof(ExposureAdaptPushConstants, meteringLowPercentile) == 32,
              "ExposureAdaptPushConstants.meteringLowPercentile offset");
static_assert(offsetof(ExposureAdaptPushConstants, meteringHighPercentile) ==
                  36,
              "ExposureAdaptPushConstants.meteringHighPercentile offset");
static_assert(offsetof(ExposureAdaptPushConstants, deltaSeconds) == 40,
              "ExposureAdaptPushConstants.deltaSeconds offset");
static_assert(offsetof(ExposureAdaptPushConstants, manualExposure) == 44,
              "ExposureAdaptPushConstants.manualExposure offset");

static_assert(sizeof(EquirectPushConstants) == 8,
              "EquirectPushConstants size mismatch with "
              "shaders/push_constants_common.slang EquirectPushConstants.");
static_assert(offsetof(EquirectPushConstants, faceIndex) == 0,
              "EquirectPushConstants.faceIndex offset");
static_assert(offsetof(EquirectPushConstants, faceSize) == 4,
              "EquirectPushConstants.faceSize offset");

static_assert(sizeof(IrradiancePushConstants) == 8,
              "IrradiancePushConstants size mismatch with "
              "shaders/push_constants_common.slang IrradiancePushConstants.");
static_assert(offsetof(IrradiancePushConstants, faceIndex) == 0,
              "IrradiancePushConstants.faceIndex offset");
static_assert(offsetof(IrradiancePushConstants, faceSize) == 4,
              "IrradiancePushConstants.faceSize offset");

static_assert(sizeof(PrefilterPushConstants) == 16,
              "PrefilterPushConstants size mismatch with "
              "shaders/push_constants_common.slang PrefilterPushConstants.");
static_assert(offsetof(PrefilterPushConstants, faceIndex) == 0,
              "PrefilterPushConstants.faceIndex offset");
static_assert(offsetof(PrefilterPushConstants, faceSize) == 4,
              "PrefilterPushConstants.faceSize offset");
static_assert(offsetof(PrefilterPushConstants, roughness) == 8,
              "PrefilterPushConstants.roughness offset");
static_assert(offsetof(PrefilterPushConstants, sourceMipCount) == 12,
              "PrefilterPushConstants.sourceMipCount offset");

static_assert(sizeof(GtaoPushConstants) == 32,
              "GtaoPushConstants size mismatch with "
              "shaders/push_constants_common.slang GtaoPushConstants.");
static_assert(offsetof(GtaoPushConstants, aoRadius) == 0,
              "GtaoPushConstants.aoRadius offset");
static_assert(offsetof(GtaoPushConstants, aoIntensity) == 4,
              "GtaoPushConstants.aoIntensity offset");
static_assert(offsetof(GtaoPushConstants, sampleCount) == 8,
              "GtaoPushConstants.sampleCount offset");
static_assert(offsetof(GtaoPushConstants, pad0) == 12,
              "GtaoPushConstants.pad0 offset");
static_assert(offsetof(GtaoPushConstants, fullWidth) == 16,
              "GtaoPushConstants.fullWidth offset");
static_assert(offsetof(GtaoPushConstants, fullHeight) == 20,
              "GtaoPushConstants.fullHeight offset");
static_assert(offsetof(GtaoPushConstants, pad1) == 24,
              "GtaoPushConstants.pad1 offset");
static_assert(offsetof(GtaoPushConstants, pad2) == 28,
              "GtaoPushConstants.pad2 offset");

static_assert(sizeof(BlurPushConstants) == 32,
              "BlurPushConstants size mismatch with "
              "shaders/push_constants_common.slang BlurPushConstants.");
static_assert(offsetof(BlurPushConstants, width) == 0,
              "BlurPushConstants.width offset");
static_assert(offsetof(BlurPushConstants, height) == 4,
              "BlurPushConstants.height offset");
static_assert(offsetof(BlurPushConstants, depthThreshold) == 8,
              "BlurPushConstants.depthThreshold offset");
static_assert(offsetof(BlurPushConstants, cameraNear) == 12,
              "BlurPushConstants.cameraNear offset");
static_assert(offsetof(BlurPushConstants, cameraFar) == 16,
              "BlurPushConstants.cameraFar offset");
static_assert(offsetof(BlurPushConstants, orthographicDepth) == 20,
              "BlurPushConstants.orthographicDepth offset");
static_assert(offsetof(BlurPushConstants, pad1) == 24,
              "BlurPushConstants.pad1 offset");
static_assert(offsetof(BlurPushConstants, pad2) == 28,
              "BlurPushConstants.pad2 offset");

static_assert(sizeof(ObjectData) == 144,
              "ObjectData size mismatch with shaders/object_data_common.slang. "
              "Update shader ObjectBuffer in lockstep.");
static_assert(alignof(ObjectData) == 16, "ObjectData must be 16-byte aligned.");
static_assert(offsetof(ObjectData, model) == 0, "ObjectData.model offset");
static_assert(offsetof(ObjectData, normalMatrix0) == 64,
              "ObjectData.normalMatrix0 offset");
static_assert(offsetof(ObjectData, normalMatrix1) == 80,
              "ObjectData.normalMatrix1 offset");
static_assert(offsetof(ObjectData, normalMatrix2) == 96,
              "ObjectData.normalMatrix2 offset");
static_assert(offsetof(ObjectData, objectInfo) == 112,
              "ObjectData.objectInfo offset");
static_assert(offsetof(ObjectData, boundingSphere) == 128,
              "ObjectData.boundingSphere offset");

static_assert(
    sizeof(SceneClipState) == 112,
    "SceneClipState size mismatch with shaders/scene_clip_common.slang.");
static_assert(alignof(SceneClipState) == 16,
              "SceneClipState must be 16-byte aligned.");
static_assert(offsetof(SceneClipState, boxClipEnabled) == 0,
              "SceneClipState.boxClipEnabled offset");
static_assert(offsetof(SceneClipState, boxClipPlaneCount) == 8,
              "SceneClipState.boxClipPlaneCount offset");
static_assert(offsetof(SceneClipState, boxClipPlanes) == 16,
              "SceneClipState.boxClipPlanes offset");

static_assert(sizeof(GpuTextureTransform) == 32,
              "GpuTextureTransform size mismatch with "
              "shaders/material_data_common.slang.");
static_assert(alignof(GpuTextureTransform) == 16,
              "GpuTextureTransform must be 16-byte aligned.");
static_assert(offsetof(GpuTextureTransform, row0) == 0,
              "GpuTextureTransform.row0 offset");
static_assert(offsetof(GpuTextureTransform, row1) == 16,
              "GpuTextureTransform.row1 offset");

static_assert(sizeof(GpuMaterial) == 896,
              "GpuMaterial size mismatch with "
              "shaders/material_data_common.slang. Update shader GpuMaterial "
              "in lockstep.");
static_assert(alignof(GpuMaterial) == 16,
              "GpuMaterial must be 16-byte aligned.");
static_assert(offsetof(GpuMaterial, color) == 0, "GpuMaterial.color offset");
static_assert(offsetof(GpuMaterial, emissiveColor) == 16,
              "GpuMaterial.emissiveColor offset");
static_assert(offsetof(GpuMaterial, emissiveStrength) == 28,
              "GpuMaterial.emissiveStrength offset");
static_assert(offsetof(GpuMaterial, metallicRoughness) == 32,
              "GpuMaterial.metallicRoughness offset");
static_assert(offsetof(GpuMaterial, alphaCutoff) == 40,
              "GpuMaterial.alphaCutoff offset");
static_assert(offsetof(GpuMaterial, normalTextureScale) == 44,
              "GpuMaterial.normalTextureScale offset");
static_assert(offsetof(GpuMaterial, occlusionStrength) == 48,
              "GpuMaterial.occlusionStrength offset");
static_assert(offsetof(GpuMaterial, baseColorTextureIndex) == 52,
              "GpuMaterial.baseColorTextureIndex offset");
static_assert(offsetof(GpuMaterial, normalTextureIndex) == 56,
              "GpuMaterial.normalTextureIndex offset");
static_assert(offsetof(GpuMaterial, occlusionTextureIndex) == 60,
              "GpuMaterial.occlusionTextureIndex offset");
static_assert(offsetof(GpuMaterial, emissiveTextureIndex) == 64,
              "GpuMaterial.emissiveTextureIndex offset");
static_assert(offsetof(GpuMaterial, metallicRoughnessTextureIndex) == 68,
              "GpuMaterial.metallicRoughnessTextureIndex offset");
static_assert(offsetof(GpuMaterial, roughnessTextureIndex) == 72,
              "GpuMaterial.roughnessTextureIndex offset");
static_assert(offsetof(GpuMaterial, metalnessTextureIndex) == 76,
              "GpuMaterial.metalnessTextureIndex offset");
static_assert(offsetof(GpuMaterial, specularTextureIndex) == 80,
              "GpuMaterial.specularTextureIndex offset");
static_assert(offsetof(GpuMaterial, heightTextureIndex) == 84,
              "GpuMaterial.heightTextureIndex offset");
static_assert(offsetof(GpuMaterial, opacityTextureIndex) == 88,
              "GpuMaterial.opacityTextureIndex offset");
static_assert(offsetof(GpuMaterial, transmissionTextureIndex) == 92,
              "GpuMaterial.transmissionTextureIndex offset");
static_assert(offsetof(GpuMaterial, specularColorTextureIndex) == 96,
              "GpuMaterial.specularColorTextureIndex offset");
static_assert(offsetof(GpuMaterial, clearcoatTextureIndex) == 100,
              "GpuMaterial.clearcoatTextureIndex offset");
static_assert(offsetof(GpuMaterial, clearcoatRoughnessTextureIndex) == 104,
              "GpuMaterial.clearcoatRoughnessTextureIndex offset");
static_assert(offsetof(GpuMaterial, clearcoatNormalTextureIndex) == 108,
              "GpuMaterial.clearcoatNormalTextureIndex offset");
static_assert(offsetof(GpuMaterial, thicknessTextureIndex) == 112,
              "GpuMaterial.thicknessTextureIndex offset");
static_assert(offsetof(GpuMaterial, sheenColorTextureIndex) == 116,
              "GpuMaterial.sheenColorTextureIndex offset");
static_assert(offsetof(GpuMaterial, sheenRoughnessTextureIndex) == 120,
              "GpuMaterial.sheenRoughnessTextureIndex offset");
static_assert(offsetof(GpuMaterial, iridescenceTextureIndex) == 124,
              "GpuMaterial.iridescenceTextureIndex offset");
static_assert(offsetof(GpuMaterial, iridescenceThicknessTextureIndex) == 128,
              "GpuMaterial.iridescenceThicknessTextureIndex offset");
static_assert(offsetof(GpuMaterial, flags) == 132, "GpuMaterial.flags offset");
static_assert(offsetof(GpuMaterial, opacityFactor) == 136,
              "GpuMaterial.opacityFactor offset");
static_assert(offsetof(GpuMaterial, specularFactor) == 140,
              "GpuMaterial.specularFactor offset");
static_assert(offsetof(GpuMaterial, heightScale) == 144,
              "GpuMaterial.heightScale offset");
static_assert(offsetof(GpuMaterial, heightOffset) == 148,
              "GpuMaterial.heightOffset offset");
static_assert(offsetof(GpuMaterial, transmissionFactor) == 152,
              "GpuMaterial.transmissionFactor offset");
static_assert(offsetof(GpuMaterial, ior) == 156, "GpuMaterial.ior offset");
static_assert(offsetof(GpuMaterial, dispersion) == 160,
              "GpuMaterial.dispersion offset");
static_assert(offsetof(GpuMaterial, clearcoatFactor) == 164,
              "GpuMaterial.clearcoatFactor offset");
static_assert(offsetof(GpuMaterial, clearcoatRoughnessFactor) == 168,
              "GpuMaterial.clearcoatRoughnessFactor offset");
static_assert(offsetof(GpuMaterial, clearcoatNormalTextureScale) == 172,
              "GpuMaterial.clearcoatNormalTextureScale offset");
static_assert(offsetof(GpuMaterial, thicknessFactor) == 176,
              "GpuMaterial.thicknessFactor offset");
static_assert(offsetof(GpuMaterial, attenuationDistance) == 180,
              "GpuMaterial.attenuationDistance offset");
static_assert(offsetof(GpuMaterial, sheenRoughnessFactor) == 184,
              "GpuMaterial.sheenRoughnessFactor offset");
static_assert(offsetof(GpuMaterial, iridescenceFactor) == 188,
              "GpuMaterial.iridescenceFactor offset");
static_assert(offsetof(GpuMaterial, iridescenceIor) == 192,
              "GpuMaterial.iridescenceIor offset");
static_assert(offsetof(GpuMaterial, iridescenceThicknessMinimum) == 196,
              "GpuMaterial.iridescenceThicknessMinimum offset");
static_assert(offsetof(GpuMaterial, iridescenceThicknessMaximum) == 200,
              "GpuMaterial.iridescenceThicknessMaximum offset");
static_assert(offsetof(GpuMaterial, specularColorFactor) == 208,
              "GpuMaterial.specularColorFactor offset");
static_assert(offsetof(GpuMaterial, attenuationColor) == 224,
              "GpuMaterial.attenuationColor offset");
static_assert(offsetof(GpuMaterial, sheenColorFactor) == 240,
              "GpuMaterial.sheenColorFactor offset");
static_assert(offsetof(GpuMaterial, baseColorTextureTransform) == 256,
              "GpuMaterial.baseColorTextureTransform offset");
static_assert(offsetof(GpuMaterial, normalTextureTransform) == 288,
              "GpuMaterial.normalTextureTransform offset");
static_assert(offsetof(GpuMaterial, occlusionTextureTransform) == 320,
              "GpuMaterial.occlusionTextureTransform offset");
static_assert(offsetof(GpuMaterial, emissiveTextureTransform) == 352,
              "GpuMaterial.emissiveTextureTransform offset");
static_assert(offsetof(GpuMaterial, metallicRoughnessTextureTransform) == 384,
              "GpuMaterial.metallicRoughnessTextureTransform offset");
static_assert(offsetof(GpuMaterial, roughnessTextureTransform) == 416,
              "GpuMaterial.roughnessTextureTransform offset");
static_assert(offsetof(GpuMaterial, metalnessTextureTransform) == 448,
              "GpuMaterial.metalnessTextureTransform offset");
static_assert(offsetof(GpuMaterial, specularTextureTransform) == 480,
              "GpuMaterial.specularTextureTransform offset");
static_assert(offsetof(GpuMaterial, heightTextureTransform) == 512,
              "GpuMaterial.heightTextureTransform offset");
static_assert(offsetof(GpuMaterial, opacityTextureTransform) == 544,
              "GpuMaterial.opacityTextureTransform offset");
static_assert(offsetof(GpuMaterial, transmissionTextureTransform) == 576,
              "GpuMaterial.transmissionTextureTransform offset");
static_assert(offsetof(GpuMaterial, specularColorTextureTransform) == 608,
              "GpuMaterial.specularColorTextureTransform offset");
static_assert(offsetof(GpuMaterial, clearcoatTextureTransform) == 640,
              "GpuMaterial.clearcoatTextureTransform offset");
static_assert(offsetof(GpuMaterial, clearcoatRoughnessTextureTransform) == 672,
              "GpuMaterial.clearcoatRoughnessTextureTransform offset");
static_assert(offsetof(GpuMaterial, clearcoatNormalTextureTransform) == 704,
              "GpuMaterial.clearcoatNormalTextureTransform offset");
static_assert(offsetof(GpuMaterial, thicknessTextureTransform) == 736,
              "GpuMaterial.thicknessTextureTransform offset");
static_assert(offsetof(GpuMaterial, sheenColorTextureTransform) == 768,
              "GpuMaterial.sheenColorTextureTransform offset");
static_assert(offsetof(GpuMaterial, sheenRoughnessTextureTransform) == 800,
              "GpuMaterial.sheenRoughnessTextureTransform offset");
static_assert(offsetof(GpuMaterial, iridescenceTextureTransform) == 832,
              "GpuMaterial.iridescenceTextureTransform offset");
static_assert(offsetof(GpuMaterial, iridescenceThicknessTextureTransform) ==
                  864,
              "GpuMaterial.iridescenceThicknessTextureTransform offset");
static_assert(kMaterialTextureDescriptorCapacity - 1u <=
                  kMaxExactGBufferMaterialMetadataIndex,
              "G-buffer float material metadata must round-trip all material "
              "indices and the thin-surface half-bit.");

} // namespace container::gpu
