#include "brdf_common.slang"
#include "area_light_common.slang"
#include "local_shadow_common.slang"
#include "ray_shadow_read.slang"
#include "shadow_common.slang"
#include "screen_space_light_shadow_common.slang"
#include "push_constants_common.slang"

#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> uPointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
[[vk::binding(7, 3)]] ConstantBuffer<ShadowBuffer> uShadow;
[[vk::binding(8, 3)]] Texture2DArray<float> gShadowAtlas;
[[vk::binding(9, 3)]] SamplerComparisonState gShadowSampler;
[[vk::binding(10, 3)]] TextureCube<float4> gIrradianceCubemap;
[[vk::binding(11, 3)]] TextureCube<float4> gPrefilteredCubemap;
[[vk::binding(12, 3)]] Texture2D<float2> gBrdfLUT;
[[vk::binding(13, 3)]] SamplerState envSampler;
[[vk::binding(14, 3)]] SamplerState brdfSampler;
[[vk::binding(18, 3)]] ConstantBuffer<LocalShadowBuffer> uLocalShadow;
[[vk::binding(19, 3)]] Texture2DArray<float> gLocalShadowAtlas;
[[vk::binding(20, 3)]] SamplerComparisonState gLocalShadowSampler;
[[vk::binding(6, 3)]] Texture2D<float> gSceneDepthTexture;

#ifdef FORWARD_RAY_QUERY
#define RAY_SCENE_SET 2
#include "ray_visibility_common.slang"
#include "ray_shadow_receiver_common.slang"
#endif

#include "scene_clip_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "surface_normal_common.slang"
#include "alpha_mask_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

static const uint kObjectFlagAlphaCutoff = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;
static const uint kObjectFlagDoubleSided = 0x4u;
static const float DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD = 0.02;
static const float DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD = 1.0e-4;
static const float LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD = 1.0e-4;
static const float LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD = 0.02;

float ForwardReceiverDepthTolerance(float depth)
{
    return max(abs(depth) * 2.0e-6, 1.0e-12);
}

#ifdef FORWARD_RAY_QUERY
bool ForwardTraceReceiver(float2 pixelPosition, out uint instanceId,
                          out float3 hit, out float depth)
{
    instanceId = 0xffffffffu;
    hit = 0.0.xxx;
    depth = 0.0;
    float2 uv = pixelPosition * uCamera.renderExtent.zw;
    // Reverse-Z scene projection and negative-height scene viewport.
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearClip = mul(uCamera.inverseViewProj, float4(ndc, 1.0, 1.0));
    float4 farClip = mul(uCamera.inverseViewProj, float4(ndc, 0.0, 1.0));
    if (!all(isfinite(nearClip)) || !all(isfinite(farClip)) ||
        abs(nearClip.w) <= 1e-20 || abs(farClip.w) <= 1e-20)
        return false;
    float3 nearWorld = nearClip.xyz / nearClip.w;
    float3 farWorld = farClip.xyz / farClip.w;
    float3 delta = farWorld - nearWorld;
    float distance = length(delta);
    if (!isfinite(distance) || distance <= 1e-5) return false;
    RayDesc ray;
    ray.Origin = nearWorld;
    ray.Direction = delta / distance;
    ray.TMin = 0.0;
    ray.TMax = distance;
    // Camera rays use the scene's front-face policy. Unlike visibility rays,
    // they need the closest accepted hit, so traversal must not end at the first.
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
    query.TraceRayInline(gRayScene, RAY_FLAG_NONE, 0xffu, ray);
    float ambiguousDistance = distance;
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        RayInstanceData instance = uRayInstances[query.CandidateInstanceID()];
        float candidateDistance = query.CandidateTriangleRayT();
        float3 candidateHit = ray.Origin + ray.Direction * candidateDistance;
        if (SectionPlaneClips(candidateHit, uRayTrace.frame.z, uRayTrace.sectionPlane) ||
            BoxClipPlanesClip(candidateHit, uRayTrace.boxClip)) continue;
        GpuMaterial material = uMaterials[instance.info.y];
        uint flags = instance.info.z | material.flags;
        if ((flags & kObjectFlagTransparentOnly) != 0u || material.transmissionFactor > 0.0)
            continue;
        // Raster alpha coverage uses derivatives/dithering, and vertex height
        // displacement has no TLAS representation. Neither can prove identity.
        if ((flags & kObjectFlagAlphaCutoff) != 0u ||
            (material.heightTextureIndex != 0xffffffffu && abs(material.heightScale) > 1e-7))
        {
            ambiguousDistance = min(ambiguousDistance, candidateDistance);
            continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT ||
        ambiguousDistance <= query.CommittedRayT()) return false;
    instanceId = query.CommittedInstanceID();
    hit = ray.Origin + ray.Direction * query.CommittedRayT();
    float4 clip = mul(uCamera.viewProj, float4(hit, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 0.0) return false;
    depth = clip.z / clip.w;
    return isfinite(depth) && depth > 0.0;
}

bool ForwardVerifyMsaaReceiver(float4 position, float3 world,
                               float3 geometricNormal, float resolvedDepth)
{
    uint samples = uRayTrace.read.options.w & 0xffu;
    if ((samples != 2u && samples != 4u && samples != 8u && samples != 16u) ||
        dot(geometricNormal, geometricNormal) < 0.5) return false;
    float planeTolerance = RayShadowReceiverPlaneTolerance(world, uCamera);
    uint centerInstance;
    float3 centerHit;
    float centerDepth;
    if (!ForwardTraceReceiver(position.xy, centerInstance, centerHit, centerDepth) ||
        abs(centerDepth - position.z) > ForwardReceiverDepthTolerance(position.z))
        return false;
    if (length(centerHit - world) > planeTolerance) return false;
    float expectedResolvedDepth = 0.0;
    bool sampleZeroResolve = (uRayTrace.read.options.w & 0x100u) != 0u;
    for (uint i = 0u; i < samples; ++i)
    {
        uint sampleInstance;
        float3 sampleHit;
        float sampleDepth;
        float2 samplePosition = floor(position.xy) + RayShadowStandardSampleLocation(samples, i);
        if (!ForwardTraceReceiver(samplePosition, sampleInstance, sampleHit, sampleDepth) ||
            sampleInstance != centerInstance ||
            abs(dot(sampleHit - world, geometricNormal)) > planeTolerance)
            return false;
        if (!sampleZeroResolve || i == 0u)
            expectedResolvedDepth = max(expectedResolvedDepth, sampleDepth);
    }
    // Query identity and the raster triangle plane must agree at every sample,
    // and reproduce the actual depth resolve. A slope bound alone would admit
    // another close parallel surface in a mixed pixel.
    return abs(expectedResolvedDepth - resolvedDepth) <=
           ForwardReceiverDepthTolerance(resolvedDepth);
}

bool ForwardHasRenderableSelectedArea()
{
    for (uint i = 0u; i < uRayTrace.read.limits.z; ++i)
    {
        float4 colorIntensity = uAreaLights[i].colorIntensity;
        if (all(isfinite(colorIntensity)) && colorIntensity.a > 0.0 &&
            any(colorIntensity.rgb > 0.0.xxx)) return true;
    }
    return false;
}
#endif

bool ForwardRayReceiverMatches(float4 position, float3 world,
                               float3 geometricNormal, out bool depthMatches)
{
    depthMatches = false;
    float resolvedDepth = gSceneDepthTexture.Load(int3(uint2(position.xy), 0));
    // Reverse-Z MSAA depth resolves to the nearest sample. Visibility traced
    // for that surface must not shadow a different primitive in this pixel.
    // A slope-based allowance can admit another nearby primitive. Only allow
    // numerical depth error; the query variant traces unmatched fragments.
    if (!isfinite(resolvedDepth) || resolvedDepth <= 0.0) return false;
    depthMatches = abs(resolvedDepth - position.z) <= ForwardReceiverDepthTolerance(position.z);
#ifdef FORWARD_RAY_QUERY
    if (RayShadowHasStandardMsaa(uRayTrace.read.options.w))
    {
        bool reconstructionReliable;
        float2 resolvedOffset;
        float3 tracedWorld = RayShadowReceiverWorld(uint2(position.xy), resolvedDepth,
            gSceneDepthTexture, uCamera, uRayTrace.read.options.w,
            reconstructionReliable, resolvedOffset);
        // This is the exact compute/filter origin. Require its receiver plane
        // for every shared MSAA lookup, including the numeric-depth fast path.
        // Uncertain edge gradients use per-fragment rays for hard lights too.
        if (!reconstructionReliable || dot(geometricNormal, geometricNormal) < 0.5 ||
            abs(dot(tracedWorld - world, geometricNormal)) >
                RayShadowReceiverPlaneTolerance(world, uCamera))
        {
            depthMatches = false;
            return false;
        }
        // Binary visibility belongs to a point, not just its receiver plane.
        // A MAX-depth tie on a flat surface can select a sample away from the
        // fragment centre and miss a thin blocker. Require the raster address
        // as well as the world position to agree; world precision tolerances
        // must not admit a deliberate subpixel shift. Filtered soft visibility
        // keeps its independent plane/coverage verification below.
        float2 tracedPosition = floor(position.xy) + resolvedOffset;
        depthMatches = depthMatches &&
            all(abs(tracedPosition - position.xy) <= 1e-4.xx) &&
            length(tracedWorld - world) <=
                RayShadowReceiverPlaneTolerance(world, uCamera);
    }
    if (uRayTrace.read.options.x != 0u && uRayTrace.read.options.z != 0u &&
        RayShadowHasStandardMsaa(uRayTrace.read.options.w) && ForwardHasRenderableSelectedArea())
        return ForwardVerifyMsaaReceiver(position, world, geometricNormal, resolvedDepth);
#endif
    return depthMatches;
}

float ForwardSelectedRayVisibility(float4 position, float3 world,
    float3 geometricNormal, float3 direction, float distance, uint layer,
    bool receiverMatches)
{
#ifdef FORWARD_RAY_QUERY
    if (!receiverMatches)
        return TraceRayVisibility(world, geometricNormal, direction, distance);
#endif
    return RayShadowVisibility(position, layer);
}

float ForwardSelectedAreaRayVisibility(float4 position, AreaLightData light,
    float3 world, float3 geometricNormal, uint layer, bool receiverMatches)
{
#ifdef FORWARD_RAY_QUERY
    if (!receiverMatches)
        return TraceAreaRayVisibility(light, world, geometricNormal,
                                      uint2(position.xy), layer);
#endif
    return RayShadowVisibility(position, layer);
}

bool ShouldTracePointLightContactShadow(float3 radiance, float lightVisibility)
{
    float3 finiteRadiance =
        all(isfinite(radiance)) ? max(radiance, 0.0.xxx) : 0.0.xxx;
    float radianceMagnitude =
        max(max(finiteRadiance.x, finiteRadiance.y), finiteRadiance.z);
    return isfinite(lightVisibility) &&
        radianceMagnitude > LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD &&
        lightVisibility > LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD;
}

float ForwardFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 ForwardFinite2Or(float2 value, float2 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 ForwardFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

static const float kForwardAmbientFloor = 0.025;

float3 ComputeForwardAmbientFloor(float3 baseColor,
                                  float occlusion,
                                  float environmentIntensity,
                                  float bounceIntensity)
{
    float environmentFloorScale =
        saturate(max(ForwardFiniteOr(environmentIntensity, 0.0), 0.0));
    float bounceFloorScale =
        saturate(max(ForwardFiniteOr(bounceIntensity, 0.0), 0.0));
    float ambientFloorScale = max(environmentFloorScale, bounceFloorScale);
    float diffuseOcclusion = saturate(ForwardFiniteOr(occlusion, 1.0));
    return max(ForwardFinite3Or(baseColor, 0.0.xxx), 0.0.xxx) *
        (kForwardAmbientFloor * ambientFloorScale * diffuseOcclusion);
}

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(4)]] float4 inTangent;
    [[vk::location(5)]] float2 inTexCoord1;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 baseColor;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint baseColorTextureIndex;
    [[vk::location(3)]] uint normalTextureIndex;
    [[vk::location(4)]] uint occlusionTextureIndex;
    [[vk::location(5)]] uint emissiveTextureIndex;
    [[vk::location(6)]] uint metallicRoughnessTextureIndex;
    [[vk::location(7)]] float3 emissiveColor;
    [[vk::location(8)]] float emissiveStrength;
    [[vk::location(9)]] float2 metallicRoughness;
    [[vk::location(10)]] float3 worldPos;
    [[vk::location(11)]] float alphaCutoff;
    [[vk::location(12)]] float normalTextureScale;
    [[vk::location(13)]] float occlusionStrength;
    [[vk::location(14)]] uint flags;
    [[vk::location(15)]] float3 worldNormal;
    [[vk::location(16)]] float4 worldTangent;
    [[vk::location(17)]] uint roughnessTextureIndex;
    [[vk::location(18)]] uint metalnessTextureIndex;
    [[vk::location(19)]] uint specularTextureIndex;
    [[vk::location(20)]] uint opacityTextureIndex;
    [[vk::location(21)]] uint transmissionTextureIndex;
    [[vk::location(22)]] float opacityFactor;
    [[vk::location(23)]] float specularFactor;
    [[vk::location(24)]] float transmissionFactor;
    [[vk::location(25)]] uint objectIndex;
    [[vk::location(26)]] float2 texCoord1;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    ObjectBuffer obj = uObjects[objectIndex];
    GpuMaterial material = uMaterials[obj.objectInfo.x];
    float2 heightTexCoord = ApplyGpuTextureTransform(
        input.inTexCoord, input.inTexCoord1, material.heightTextureTransform);

    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, heightTexCoord,
        material.heightTextureIndex, material.heightTextureTransform,
        material.heightScale,
        material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    float3x3 modelMatrix = (float3x3)obj.model;
    float3 normalVector =
        obj.normalMatrix0.xyz * input.inNormal.x +
        obj.normalMatrix1.xyz * input.inNormal.y +
        obj.normalMatrix2.xyz * input.inNormal.z;
    float3 worldNormal = SafeNormalize(normalVector, float3(0.0, 1.0, 0.0));
    float4 worldTangent = BuildWorldTangent(modelMatrix, worldNormal, input.inTangent);

    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = material.color * float4(input.inColor, 1.0);
    output.texCoord = input.inTexCoord;
    output.texCoord1 = input.inTexCoord1;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.normalTextureIndex = material.normalTextureIndex;
    output.occlusionTextureIndex = material.occlusionTextureIndex;
    output.emissiveTextureIndex = material.emissiveTextureIndex;
    output.metallicRoughnessTextureIndex =
        material.metallicRoughnessTextureIndex;
    output.emissiveColor = material.emissiveColor;
    output.emissiveStrength = material.emissiveStrength;
    output.metallicRoughness = material.metallicRoughness;
    output.worldPos = worldPos.xyz;
    output.alphaCutoff = material.alphaCutoff;
    output.normalTextureScale = material.normalTextureScale;
    output.occlusionStrength = material.occlusionStrength;
    output.flags = material.flags | obj.objectInfo.y;
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.roughnessTextureIndex = material.roughnessTextureIndex;
    output.metalnessTextureIndex = material.metalnessTextureIndex;
    output.specularTextureIndex = material.specularTextureIndex;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.transmissionTextureIndex = material.transmissionTextureIndex;
    output.opacityFactor = material.opacityFactor;
    output.specularFactor = material.specularFactor;
    output.transmissionFactor = material.transmissionFactor;
    output.objectIndex = objectIndex;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float3 rayReceiverNormal = 0.0.xxx;
#ifdef FORWARD_RAY_QUERY
    // World-position derivatives describe the raster triangle, including
    // smooth-shaded meshes. Evaluate them before discards or light divergence;
    // zero tangents request the shared conservative ray-direction bias.
    float3 receiverDx = SafeNormalize(ddx(vertIn.worldPos), 0.0.xxx);
    float3 receiverDy = SafeNormalize(ddy(vertIn.worldPos), 0.0.xxx);
    rayReceiverNormal = SafeNormalize(cross(receiverDx, receiverDy), 0.0.xxx);
#endif
    if ((vertIn.flags & kObjectFlagTransparentOnly) != 0u)
    {
        discard;
    }

    if (SceneClipPlanesClip(vertIn.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    GpuMaterial material =
        uMaterials[uObjects[vertIn.objectIndex].objectInfo.x];
    float2 baseColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.baseColorTextureTransform);
    float2 normalTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.normalTextureTransform);
    float2 occlusionTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.occlusionTextureTransform);
    float2 emissiveTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.emissiveTextureTransform);
    float2 metallicRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.metallicRoughnessTextureTransform);
    float2 roughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.roughnessTextureTransform);
    float2 metalnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.metalnessTextureTransform);
    float2 specularTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.specularTextureTransform);
    float2 opacityTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.opacityTextureTransform);
    float2 transmissionTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.transmissionTextureTransform);
    float2 specularColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.specularColorTextureTransform);
    float2 clearcoatTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatTextureTransform);
    float2 clearcoatRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatRoughnessTextureTransform);
    float2 clearcoatNormalTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatNormalTextureTransform);
    float2 thicknessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.thicknessTextureTransform);
    float2 sheenColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.sheenColorTextureTransform);
    float2 sheenRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.sheenRoughnessTextureTransform);
    float2 iridescenceTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.iridescenceTextureTransform);
    float2 iridescenceThicknessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.iridescenceThicknessTextureTransform);

    float3 baseColor = vertIn.baseColor.rgb;
    float baseAlpha = vertIn.baseColor.a;

    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        float4 texColor = SamplePbrTexture(
            vertIn.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx);
        baseColor *= texColor.rgb;
        baseAlpha *= texColor.a;
    }
    baseAlpha = EvaluateOpacity(
        baseAlpha, vertIn.opacityTextureIndex, vertIn.opacityFactor,
        opacityTexCoord, material.opacityTextureTransform);

    if ((vertIn.flags & kObjectFlagAlphaCutoff) != 0u &&
        !AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos))
    {
        discard;
    }

    if (baseAlpha <= 0.01)
    {
        discard;
    }

    bool rayReceiverDepthMatches = false;
    bool rayReceiverMatches = (vertIn.flags & kPbrObjectFlagUnlit) == 0u &&
        RayDirectionalSelected() &&
        ForwardRayReceiverMatches(vertIn.pos, vertIn.worldPos, rayReceiverNormal,
                                  rayReceiverDepthMatches);
    bool useRayVisibility = RayDirectionalSelected();
#ifndef FORWARD_RAY_QUERY
    useRayVisibility = useRayVisibility && rayReceiverMatches;
#endif

    float sampledMetallic = EvaluateMetalness(
        vertIn.metallicRoughness.x, vertIn.metallicRoughnessTextureIndex,
        vertIn.metalnessTextureIndex, metallicRoughnessTexCoord,
        metalnessTexCoord, material.metalnessTextureTransform);
    float sampledRoughness = EvaluateRoughness(
        vertIn.metallicRoughness.y, vertIn.metallicRoughnessTextureIndex,
        vertIn.roughnessTextureIndex, metallicRoughnessTexCoord,
        roughnessTexCoord, material.roughnessTextureTransform, vertIn.flags);
    float specularFactor = EvaluateSpecularFactor(
        vertIn.specularFactor, vertIn.specularTextureIndex, specularTexCoord,
        material.specularTextureTransform);
    float transmissionFactor = EvaluateTransmissionFactor(
        vertIn.transmissionFactor, vertIn.transmissionTextureIndex,
        transmissionTexCoord, material.transmissionTextureTransform);
    float3 specularColor = EvaluateSpecularColor(
        material.specularColorFactor.rgb, material.specularColorTextureIndex,
        specularColorTexCoord, vertIn.specularTextureIndex, specularTexCoord);
    float clearcoat = EvaluateClearcoatFactor(
        material.clearcoatFactor, material.clearcoatTextureIndex,
        clearcoatTexCoord, material.clearcoatTextureTransform);
    float clearcoatRoughness = EvaluateClearcoatRoughness(
        material.clearcoatRoughnessFactor,
        material.clearcoatRoughnessTextureIndex, clearcoatRoughnessTexCoord,
        material.clearcoatRoughnessTextureTransform);
    float3 sheenColor = EvaluateSheenColor(
        material.sheenColorFactor.rgb, material.sheenColorTextureIndex,
        sheenColorTexCoord);
    float sheenRoughness = EvaluateSheenRoughness(
        material.sheenRoughnessFactor, material.sheenRoughnessTextureIndex,
        sheenRoughnessTexCoord, material.sheenRoughnessTextureTransform);
    float iridescence = EvaluateIridescenceFactor(
        material.iridescenceFactor, material.iridescenceTextureIndex,
        iridescenceTexCoord, material.iridescenceTextureTransform);
    float iridescenceThickness = EvaluateIridescenceThickness(
        material.iridescenceThicknessMinimum,
        material.iridescenceThicknessMaximum,
        material.iridescenceThicknessTextureIndex, iridescenceThicknessTexCoord,
        material.iridescenceThicknessTextureTransform);
    float volumeThickness = EvaluateThickness(
        material.thicknessFactor, material.thicknessTextureIndex,
        thicknessTexCoord, material.thicknessTextureTransform);

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, normalTexCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);

    float occlusion = 1.0;
    if (vertIn.occlusionTextureIndex != 0xffffffff)
    {
        float sampledOcclusion = SamplePbrTexture(
            vertIn.occlusionTextureIndex, occlusionTexCoord, 1.0.xxxx).r;
        occlusion = lerp(1.0, sampledOcclusion, saturate(vertIn.occlusionStrength));
    }

    float3 emissive = vertIn.emissiveColor;
    if (vertIn.emissiveTextureIndex != 0xffffffff)
    {
        emissive *= SamplePbrTexture(
            vertIn.emissiveTextureIndex, emissiveTexCoord, 1.0.xxxx).rgb;
    }
    emissive *= vertIn.emissiveStrength;

    float metallic = sampledMetallic;
    float roughness = sampledRoughness;
    if (pc.semanticColorMode != kBimSemanticColorModeOff)
    {
        baseColor = EvaluateBimSemanticColor(
            uObjects[vertIn.objectIndex].objectInfo.w, pc.semanticColorMode);
        metallic = 0.0;
        roughness = 0.74;
        emissive = 0.0.xxx;
        transmissionFactor = 0.0;
        clearcoat = 0.0;
        sheenColor = 0.0.xxx;
        iridescence = 0.0;
        volumeThickness = 0.0;
    }
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - vertIn.worldPos,
                             float3(0.0, 0.0, 1.0));
    float NdotV = max(dot(surfaceNormal.shadingNormal, V), 0.0);
    float3 iridescenceTint = ApproximateIridescenceTint(
        iridescence, material.iridescenceIor, iridescenceThickness, NdotV);
    float3 F0 = EvaluatePbrF0(
        baseColor, metallic, specularFactor, specularColor * iridescenceTint,
        material.ior);

    float3 F_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    float environmentIntensity =
        max(ForwardFiniteOr(uLighting.environmentIntensity, 0.0), 0.0);
    float3 irradiance =
        ForwardFinite3Or(
            gIrradianceCubemap.Sample(envSampler, surfaceNormal.shadingNormal).rgb,
            0.0.xxx) *
        environmentIntensity;
    float3 diffuseIBL = irradiance * baseColor;
    float3 R = reflect(-V, surfaceNormal.shadingNormal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor =
        ForwardFinite3Or(
            gPrefilteredCubemap.SampleLevel(envSampler, R,
                                            roughness * maxMipLevel).rgb,
            0.0.xxx) *
        environmentIntensity;
    float2 brdf = max(
        ForwardFinite2Or(
            gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness)),
            0.0.xx),
        0.0.xx);
    float3 specularIBL =
        EvaluateSpecularIblMultipleScattering(prefilteredColor, F0, brdf);

    float diffuseAO = occlusion;
    float specularOcclusion = saturate(pow(NdotV + diffuseAO, exp2(-16.0 * roughness - 1.0)) - 1.0 + diffuseAO);
    specularOcclusion = lerp(1.0, specularOcclusion, saturate(1.0 - roughness));
    float3 lighting = kD_ibl * diffuseIBL * diffuseAO + specularIBL * specularOcclusion;
    lighting += ComputeForwardAmbientFloor(
        baseColor, occlusion, environmentIntensity, uLighting.bounceIntensity);

    if ((vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        lighting = baseColor;
        baseAlpha *= lerp(1.0, 0.35, transmissionFactor);
    }

    float3 directLighting = 0.0.xxx;
    float3 directionalL = SafeNormalize(-uLighting.directionalDirection.xyz,
                                        float3(0.0, 1.0, 0.0));
    float3 directionalRadiance =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;
    const bool thinSurface = IsThinSurfaceGpuMaterial(vertIn.flags);
    float shadowFactor = 1.0;
    float contactVisibility = 1.0;
    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        float3 cameraForward =
            SafeNormalize(uCamera.cameraForward.xyz, float3(0.0, 0.0, -1.0));
        float viewDepth =
            max(dot(vertIn.worldPos - uCamera.cameraWorldPosition.xyz,
                    cameraForward), 0.0);
        float NdotLForShadow = dot(surfaceNormal.geometricNormal, directionalL);
        if (!useRayVisibility && uLighting.shadowEnabled != 0u &&
            (NdotLForShadow > 0.0 || thinSurface))
        {
            shadowFactor = ComputeShadowFactor(
                vertIn.worldPos, surfaceNormal.geometricNormal,
                uLighting.directionalDirection.xyz, viewDepth,
                uShadow, gShadowAtlas, gShadowSampler);
            float directionalRadianceMagnitude =
                max(max(directionalRadiance.r, directionalRadiance.g),
                    directionalRadiance.b);
            if (shadowFactor > DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD &&
                directionalRadianceMagnitude >
                    DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD &&
                ShadowFiniteOr(uShadow.contactShadowSettings.x, 0.0) > 0.0)
            {
                contactVisibility = ScreenSpaceDirectionalContactVisibilityDepthOnly(
                    vertIn.worldPos, surfaceNormal.geometricNormal,
                    directionalL, uShadow.contactShadowSettings.y,
                    uShadow.contactShadowSettings.z,
                    uShadow.contactShadowSettings.w, gSceneDepthTexture,
                    uCamera.viewProj, uCamera.inverseViewProj);
            }
        }
        if (useRayVisibility)
            shadowFactor = any(directionalRadiance > 0.0.xxx)
                ? ForwardSelectedRayVisibility(vertIn.pos, vertIn.worldPos,
                    rayReceiverNormal, directionalL, 1e20, 0u,
                    rayReceiverDepthMatches)
                : 1.0;
        float directShadowVisibility = shadowFactor * contactVisibility;
        directLighting += EvaluateDeferredLayeredDirectLight(
            baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal, V,
            directionalL, directionalRadiance * directShadowVisibility,
            clearcoat, clearcoatRoughness, sheenColor, sheenRoughness);
        directLighting += EvaluateDiffuseBounceLight(
            baseColor, metallic, surfaceNormal.shadingNormal,
            surfaceNormal.geometricNormal, V, directionalL,
            directionalRadiance, shadowFactor, occlusion,
            uLighting.bounceIntensity);
        if (thinSurface)
        {
            directLighting += EvaluateThinSurfaceTransmission(
                baseColor, surfaceNormal.shadingNormal, V, directionalL,
                directionalRadiance * directShadowVisibility, saturate(baseAlpha),
                !isFrontFace);
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        for (uint i = 0; i < uLighting.pointLightCount && i < MAX_CLUSTERED_LIGHTS; ++i)
        {
            PointLightData light = uPointLights[i];
            bool rayPointSelected = useRayVisibility && RayPointSelected(i);
            float3 lightVector = light.positionRadius.xyz - vertIn.worldPos;
            float lightRadius = light.positionRadius.w;
            float distanceSq = dot(lightVector, lightVector);
            if (PointLightHasFiniteRange(lightRadius) &&
                distanceSq >= lightRadius * lightRadius)
            {
                continue;
            }

            float invDistance = rsqrt(max(distanceSq, 1e-8));
            float3 L = lightVector * invDistance;
            float attenuation = PointLightAttenuation(distanceSq, lightRadius);
            float coneAttenuation = SpotLightConeAttenuation(
                light.directionInnerCos, light.coneOuterCosType, L);
            float3 radiance =
                light.colorIntensity.rgb * light.colorIntensity.a * attenuation *
                coneAttenuation;
            float3 unshadowedRadiance = radiance;
            float lightVisibility = 1.0;
            if (!rayPointSelected && LocalShadowMapsEnabled(uLighting, uLocalShadow))
            {
                lightVisibility *= LocalPointLightShadowVisibility(
                    light, vertIn.worldPos, surfaceNormal.geometricNormal, L,
                    uLocalShadow, gLocalShadowAtlas, gLocalShadowSampler);
            }
            if (!rayPointSelected && uLighting.localContactVisibility != 0u &&
                ShouldTracePointLightContactShadow(radiance, lightVisibility))
            {
                lightVisibility *= ScreenSpacePointLightContactVisibilityDepthOnly(
                    vertIn.worldPos, surfaceNormal.geometricNormal,
                    light.positionRadius.xyz, gSceneDepthTexture,
                    uCamera.viewProj, uCamera.inverseViewProj);
            }
            if (rayPointSelected)
                lightVisibility = PointLightHasRenderableIntensity(light) &&
                    distanceSq > 1e-10 && any(radiance > 0.0.xxx)
                    ? ForwardSelectedRayVisibility(vertIn.pos, vertIn.worldPos,
                        rayReceiverNormal, L, sqrt(distanceSq), 1u + i,
                        rayReceiverDepthMatches)
                    : 1.0;
            radiance *= lightVisibility;
            directLighting += EvaluateDeferredLayeredDirectLight(
                baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal,
                V, L, radiance, clearcoat, clearcoatRoughness, sheenColor,
                sheenRoughness);
            directLighting += EvaluateDiffuseBounceLight(
                baseColor, metallic, surfaceNormal.shadingNormal,
                surfaceNormal.geometricNormal, V, L, unshadowedRadiance,
                lightVisibility, occlusion, uLighting.bounceIntensity);
            if (thinSurface)
            {
                directLighting += EvaluateThinSurfaceTransmission(
                    baseColor, surfaceNormal.shadingNormal, V, L, radiance,
                    saturate(baseAlpha), !isFrontFace);
            }
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        uint areaLightCount = min(uLighting.areaLightCount, MAX_AREA_LIGHTS);
        for (uint i = 0u; i < areaLightCount; ++i)
        {
            AreaLightData light = uAreaLights[i];
            bool rayAreaSelected = useRayVisibility && RayAreaSelected(i);
            AreaLightFrame frame = BuildAreaLightFrame(light);
            float areaShadowVisibility = 1.0;
            uint areaShadowRef = PackedLocalShadowAreaRef(uLocalShadow, i);
            if (!rayAreaSelected && LocalShadowMapsEnabled(uLighting, uLocalShadow) &&
                areaShadowRef != 0u)
            {
                // Integrate the emitter samples backed by rendered cube maps.
                areaShadowVisibility = 0.0;
                float visibilityWeightSum = 0.0;
                uint visibilitySampleCount =
                    LocalAreaShadowSampleCount(areaShadowRef, uLocalShadow);
                [loop]
                for (uint visibilitySample = 0u;
                     visibilitySample < visibilitySampleCount;
                     ++visibilitySample)
                {
                    float3 samplePosition = LocalAreaShadowSamplePosition(
                        areaShadowRef, visibilitySample, uLocalShadow);
                    float3 visibilitySurfaceToLight;
                    float weight = AreaLightPositionContributionWeight(
                        light, frame, vertIn.worldPos, samplePosition,
                        1.0 / float(visibilitySampleCount),
                        visibilitySurfaceToLight);
                    if (weight <= 0.0)
                    {
                        continue;
                    }
                    areaShadowVisibility += weight * LocalAreaLightShadowVisibility(
                        i, light, vertIn.worldPos, surfaceNormal.geometricNormal,
                        visibilitySample, uLocalShadow, gLocalShadowAtlas,
                        gLocalShadowSampler);
                    visibilityWeightSum += weight;
                }
                areaShadowVisibility =
                    visibilityWeightSum > 1.0e-4
                        ? areaShadowVisibility / visibilityWeightSum
                        : 1.0;
            }
            if (rayAreaSelected)
                areaShadowVisibility = ForwardSelectedAreaRayVisibility(
                    vertIn.pos, light, vertIn.worldPos, rayReceiverNormal,
                    5u + i, rayReceiverMatches);
            [unroll]
            for (uint sampleIndex = 0u;
                 sampleIndex < AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
                 ++sampleIndex)
            {
                float3 L;
                float3 radiance = EvaluateAreaLightSampleRadiance(
                    light, frame, vertIn.worldPos, sampleIndex, L);
                float lightVisibility = areaShadowVisibility;
                float3 directRadiance = radiance * lightVisibility;
                directLighting += EvaluateDeferredLayeredDirectLight(
                    baseColor, F0, metallic, roughness,
                    surfaceNormal.shadingNormal, V, L, directRadiance,
                    clearcoat, clearcoatRoughness, sheenColor, sheenRoughness);
                directLighting += EvaluateDiffuseBounceLight(
                    baseColor, metallic, surfaceNormal.shadingNormal,
                    surfaceNormal.geometricNormal, V, L, radiance,
                    lightVisibility, occlusion, uLighting.bounceIntensity);
                if (thinSurface)
                {
                    directLighting += EvaluateThinSurfaceTransmission(
                        baseColor, surfaceNormal.shadingNormal, V, L,
                        directRadiance, saturate(baseAlpha), !isFrontFace);
                }
            }
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        if (clearcoat > 0.0)
        {
            float3 clearcoatNormal = surfaceNormal.shadingNormal;
            if (material.clearcoatNormalTextureIndex != kInvalidMaterialTexture)
            {
                clearcoatNormal = EvaluateSurfaceNormal(
                    vertIn.worldNormal, vertIn.worldTangent,
                    clearcoatNormalTexCoord,
                    material.clearcoatNormalTextureIndex,
                    material.clearcoatNormalTextureScale, faceSign).shadingNormal;
            }
            float clearcoatNdotV = max(dot(clearcoatNormal, V), 0.0);
            float3 clearcoatF0 = IorToF0(1.5).xxx;
            float3 clearcoatR = reflect(-V, clearcoatNormal);
            float3 clearcoatPrefiltered = ForwardFinite3Or(
                gPrefilteredCubemap.SampleLevel(
                    envSampler, clearcoatR,
                    clearcoatRoughness * maxMipLevel).rgb,
                0.0.xxx) *
                environmentIntensity;
            float2 clearcoatBrdf =
                max(ForwardFinite2Or(
                        gBrdfLUT.Sample(
                            brdfSampler,
                            float2(clearcoatNdotV, clearcoatRoughness)),
                        0.0.xx),
                    0.0.xx);
            float3 clearcoatIbl =
                EvaluateSpecularIblMultipleScattering(
                    clearcoatPrefiltered, clearcoatF0, clearcoatBrdf);
            lighting = lighting * (1.0 - clearcoat * 0.04) +
                       clearcoatIbl * clearcoat;
        }

        float sheenWeight = MaxComponent(sheenColor);
        if (sheenWeight > 0.0)
        {
            float grazing = pow(1.0 - NdotV, 2.0);
            float sheenScale = grazing * (1.0 - 0.35 * sheenRoughness);
            lighting = lighting * (1.0 - saturate(sheenWeight * 0.25)) +
                       sheenColor * irradiance * sheenScale * 0.35;
        }
        lighting += directLighting;
    }

    lighting += emissive;

    float transmissionWeight = saturate(transmissionFactor * (1.0 - metallic));
    if (transmissionWeight > 0.0)
    {
        float3 transmittedColor = 0.0.xxx;
        if (volumeThickness > 0.0)
        {
            float safeIor = max(material.ior, 1.0);
            float halfSpread =
                (safeIor - 1.0) * 0.025 * max(material.dispersion, 0.0);
            float3 iors = max(float3(safeIor - halfSpread, safeIor,
                                     safeIor + halfSpread),
                              1.0.xxx);
            float etaR = isFrontFace ? (1.0 / iors.r) : iors.r;
            float etaG = isFrontFace ? (1.0 / iors.g) : iors.g;
            float etaB = isFrontFace ? (1.0 / iors.b) : iors.b;
            float3 refrR = refract(-V, surfaceNormal.shadingNormal, etaR);
            float3 refrG = refract(-V, surfaceNormal.shadingNormal, etaG);
            float3 refrB = refract(-V, surfaceNormal.shadingNormal, etaB);
            bool refrRValid = dot(refrR, refrR) > 1e-6 && all(isfinite(refrR));
            bool refrGValid = dot(refrG, refrG) > 1e-6 && all(isfinite(refrG));
            bool refrBValid = dot(refrB, refrB) > 1e-6 && all(isfinite(refrB));
            if (refrRValid)
            {
                transmittedColor.r = ForwardFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrR, roughness * maxMipLevel).r,
                    0.0);
            }
            if (refrGValid)
            {
                transmittedColor.g = ForwardFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrG, roughness * maxMipLevel).g,
                    0.0);
            }
            if (refrBValid)
            {
                transmittedColor.b = ForwardFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrB, roughness * maxMipLevel).b,
                    0.0);
            }
        }
        else
        {
            transmittedColor = ForwardFinite3Or(
                gPrefilteredCubemap.SampleLevel(
                    envSampler, -V, roughness * maxMipLevel).rgb,
                0.0.xxx);
        }
        transmittedColor *= environmentIntensity;

        if (dot(transmittedColor, transmittedColor) > 0.0 &&
            all(isfinite(transmittedColor)))
        {
            transmittedColor = ApplyBeerLambert(
                transmittedColor * baseColor, material.attenuationColor.rgb,
                material.attenuationDistance, volumeThickness);
            lighting = lerp(lighting, transmittedColor + emissive,
                            transmissionWeight * 0.75);
        }
        baseAlpha *= lerp(1.0, 0.35, transmissionWeight);
    }

    if (RayDirectionalSelected() && uRayShadowRead.options.y != 0u)
        return float4(RayShadowDebug(vertIn.pos), 1.0);
    return float4(lighting, 1.0);
}
