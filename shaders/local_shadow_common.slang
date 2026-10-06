// local_shadow_common.slang - Shared light-space local shadow-map sampling.
// Point/spot/area lighting should use this for real local-light occlusion.
// Deferred opaque point/spot passes may add screen-space contact visibility as
// an explicit detail term; area and transparent paths stay atlas-only here.
#pragma once

#include "lighting_structs.slang"
#include "brdf_common.slang"
#include "area_light_common.slang"

static const uint LOCAL_SHADOW_POISSON_SAMPLE_COUNT = 32u;
static const uint LOCAL_SHADOW_PCF_FAST_SAMPLE_COUNT = 12u;
static const float LOCAL_SHADOW_PCF_KERNEL_RADIUS = 1.0;

float LocalShadowFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

bool LocalShadowMapsEnabled(uint localShadowEnabled,
                            LocalShadowBuffer localShadow)
{
    return localShadowEnabled != 0u &&
           localShadow.counts.x != 0u &&
           localShadow.counts.z != 0u &&
           localShadow.counts.w != 0u;
}

bool LocalShadowMapsEnabled(LightingBuffer lighting,
                            LocalShadowBuffer localShadow)
{
    return LocalShadowMapsEnabled(lighting.localShadowEnabled, localShadow);
}

uint LocalShadowBaseLayerFromPointLight(PointLightData light)
{
    float encoded = light.coneOuterCosType.z;
    if (!isfinite(encoded) || encoded < 1.0)
    {
        return MAX_SHADOWED_LOCAL_LIGHT_LAYERS;
    }
    return uint(encoded + 0.5) - 1u;
}

uint LocalShadowLayerCountFromPointLight(PointLightData light)
{
    float encoded = light.coneOuterCosType.w;
    if (!isfinite(encoded) || encoded < 1.0)
    {
        return 0u;
    }
    return uint(encoded + 0.5);
}

bool PointLightHasLocalShadowLayers(PointLightData light,
                                    LocalShadowBuffer localShadow)
{
    uint baseLayer = LocalShadowBaseLayerFromPointLight(light);
    uint layerCount = LocalShadowLayerCountFromPointLight(light);
    if (baseLayer >= MAX_SHADOWED_LOCAL_LIGHT_LAYERS ||
        layerCount == 0u ||
        baseLayer >= localShadow.counts.x)
    {
        return false;
    }

    if (layerCount > MAX_SHADOWED_LOCAL_LIGHT_LAYERS - baseLayer)
    {
        return false;
    }

    return baseLayer + layerCount <= localShadow.counts.x;
}

uint SelectPointLocalShadowFace(float3 lightToReceiver)
{
    float3 a = abs(lightToReceiver);
    if (a.x >= a.y && a.x >= a.z)
    {
        return lightToReceiver.x >= 0.0 ? 0u : 1u;
    }
    if (a.y >= a.x && a.y >= a.z)
    {
        return lightToReceiver.y >= 0.0 ? 2u : 3u;
    }
    return lightToReceiver.z >= 0.0 ? 4u : 5u;
}

uint PackedLocalShadowAreaRef(LocalShadowBuffer localShadow, uint areaIndex)
{
    if (areaIndex >= MAX_AREA_LIGHTS)
    {
        return 0u;
    }
    uint4 packed = localShadow.areaLightRefs[areaIndex / 4u];
    return packed[areaIndex & 3u];
}

uint LocalAreaShadowSampleCount(uint encodedLayer, LocalShadowBuffer localShadow)
{
    if (encodedLayer == 0u || encodedLayer > localShadow.counts.x) return 0u;
    uint layerCount = localShadow.layers[encodedLayer - 1u].meta.z;
    if (layerCount > localShadow.counts.x - (encodedLayer - 1u)) return 0u;
    return min(layerCount / LOCAL_SHADOW_POINT_FACE_COUNT,
               AREA_LIGHT_SHADOW_VISIBILITY_SAMPLE_COUNT);
}

float3 LocalAreaShadowSamplePosition(uint encodedLayer, uint sampleIndex,
                                    LocalShadowBuffer localShadow)
{
    uint sampleBase = encodedLayer - 1u + sampleIndex * LOCAL_SHADOW_POINT_FACE_COUNT;
    return localShadow.layers[sampleBase].positionRange.xyz;
}

// The CPU cube views use -Y up except for the two Y faces.
float3 LocalShadowCubeUp(uint face)
{
    if (face == 2u) return float3(0.0, 0.0, 1.0);
    if (face == 3u) return float3(0.0, 0.0, -1.0);
    return float3(0.0, -1.0, 0.0);
}

// Convert the filter direction to the neighboring face and its own perspective
// depth. Array-layer border samples otherwise turn a blocked seam into light.
float SampleLocalShadowCubeTap(uint layerIndex, float2 tapUv,
    float3 receiverPosition, float bias, LocalShadowBuffer localShadow,
    Texture2DArray<float> atlas, SamplerComparisonState sampler,
    float invResolution)
{
    LocalShadowLayerData layer = localShadow.layers[layerIndex];
    float3 forward = layer.directionType.xyz;
    float3 up = LocalShadowCubeUp(layer.meta.y);
    float3 right = cross(forward, up);
    float2 ndc = tapUv * 2.0 - 1.0;
    float3 direction = SafeNormalize(forward + right * ndc.x + up * ndc.y, forward);
    uint face = SelectPointLocalShadowFace(direction);
    uint cubeBase = layerIndex - layer.meta.y;
    uint tapLayer = cubeBase + face;
    if (tapLayer >= localShadow.counts.x) return 1.0;
    float receiverDistance = length(receiverPosition - layer.positionRange.xyz);
    float3 tapPosition = layer.positionRange.xyz + direction * receiverDistance;
    float4 clip = mul(localShadow.layers[tapLayer].viewProj, float4(tapPosition, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return 1.0;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    // Keep the hardware bilinear footprint inside the selected layer. Filter
    // tap directions already cross the seam explicitly.
    uv = clamp(uv, (0.5 * invResolution).xx, (1.0 - 0.5 * invResolution).xx);
    return atlas.SampleCmpLevelZero(sampler, float3(uv, float(tapLayer)),
                                   saturate(projected.z + bias));
}

float LocalShadowSlopeBias(float3 normal, float3 surfaceToLight,
                           LocalShadowBuffer localShadow)
{
    float NdotL =
        saturate(LocalShadowFiniteOr(dot(normal, surfaceToLight), 0.0));
    float slope = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.12);
    float constantBias =
        max(LocalShadowFiniteOr(localShadow.filterSettings.z, 0.0), 0.0);
    float slopeScale =
        max(LocalShadowFiniteOr(localShadow.biasSettings.z, 0.0), 0.0);
    float maxBias =
        max(LocalShadowFiniteOr(localShadow.filterSettings.w, constantBias),
            0.0);
    return min(constantBias + slope * slopeScale, maxBias);
}

float LocalShadowReceiverPlaneBias(float3 shadowNdc,
                                   LocalShadowBuffer localShadow)
{
    // Local shadows are sampled from per-light loops in tiled and transparent
    // paths. Screen-space derivatives inside those non-uniform branches can
    // overestimate bias and create view-dependent light leaks.
    return 0.0;
}

float LocalShadowSoftFilterRadiusTexels(
    LocalShadowLayerData layer,
    float distanceToLight,
    LocalShadowBuffer localShadow)
{
    float baseRadiusTexels =
        max(LocalShadowFiniteOr(localShadow.filterSettings.x, 0.0), 0.0);
    float maxSoftRadiusTexels =
        max(LocalShadowFiniteOr(localShadow.filterSettings.y,
                                baseRadiusTexels),
            baseRadiusTexels);
    float texelSizeWorld =
        max(LocalShadowFiniteOr(layer.params.x, 0.0), 1.0e-5);
    float sourceRadiusWorld =
        max(LocalShadowFiniteOr(layer.params.w, 0.0), 0.0);
    float lightRange =
        max(LocalShadowFiniteOr(layer.positionRange.w, 0.0), 1.0e-4);
    float receiverT = saturate(distanceToLight / lightRange);

    // Area and finite-radius local lights need the PCF footprint to grow in
    // light texel space as the receiver moves away from the source. This is a
    // bounded penumbra approximation, not a screen-space contact term.
    float projectedSourceRadiusTexels =
        (sourceRadiusWorld / texelSizeWorld) * lerp(0.20, 0.85, receiverT);
    return clamp(max(baseRadiusTexels, projectedSourceRadiusTexels),
                 max(baseRadiusTexels, 0.25),
                 maxSoftRadiusTexels);
}

float2 LocalShadowPoissonOffset(uint sampleIndex)
{
    static const float2 offsets[32] = {
        float2(0.000000, 0.000000),
        float2(0.130526, 0.321439),
        float2(-0.363221, 0.225041),
        float2(0.392938, -0.177877),
        float2(-0.086824, -0.492404),
        float2(0.572062, 0.205212),
        float2(-0.576713, -0.112348),
        float2(0.213938, -0.681998),
        float2(-0.231052, 0.666525),
        float2(0.746285, -0.443145),
        float2(-0.748956, 0.398171),
        float2(0.521132, 0.612118),
        float2(-0.469846, -0.671010),
        float2(0.033328, 0.845572),
        float2(-0.054318, -0.838721),
        float2(0.859472, 0.077016),
        float2(-0.881298, -0.061628),
        float2(0.315178, 0.911571),
        float2(-0.344774, 0.861288),
        float2(0.589778, -0.742762),
        float2(-0.604023, -0.721308),
        float2(0.935302, -0.256183),
        float2(-0.923125, 0.225019),
        float2(0.103689, -0.962455),
        float2(-0.123415, 0.953126),
        float2(0.726911, 0.609110),
        float2(-0.713281, -0.640237),
        float2(0.437944, -0.887671),
        float2(-0.442079, 0.879304),
        float2(0.958230, 0.284005),
        float2(-0.946223, -0.294604),
        float2(0.005117, 0.998930),
    };
    return offsets[sampleIndex % LOCAL_SHADOW_POISSON_SAMPLE_COUNT];
}

float LocalShadowStableRotation(float2 shadowUv, uint layerIndex,
                                float atlasResolution)
{
    atlasResolution = max(LocalShadowFiniteOr(atlasResolution, 1.0), 1.0);
    shadowUv = all(isfinite(shadowUv)) ? saturate(shadowUv) : 0.0.xx;
    float2 shadowTexel = floor(shadowUv * atlasResolution);
    float seed = dot(shadowTexel, float2(127.1, 311.7)) +
        float(layerIndex) * 17.17;
    return frac(sin(seed) * 43758.5453123) * 6.28318530718;
}

float2 RotateLocalShadowPoissonOffset(float2 offset, float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return float2(offset.x * c - offset.y * s,
                  offset.x * s + offset.y * c);
}

float LocalShadowOuterSampleWeight(float filterRadiusTexels)
{
    filterRadiusTexels = max(LocalShadowFiniteOr(filterRadiusTexels, 0.0), 0.0);
    return smoothstep(1.25, 1.75, filterRadiusTexels);
}

float3 OffsetLocalShadowSamplePosition(
    float3 worldPosition,
    float3 normal,
    float3 surfaceToLight,
    LocalShadowLayerData layer,
    LocalShadowBuffer localShadow)
{
    float minNormalBiasTexels =
        max(LocalShadowFiniteOr(localShadow.biasSettings.x, 0.0), 0.0);
    float maxNormalBiasTexels =
        max(LocalShadowFiniteOr(localShadow.biasSettings.y,
                                minNormalBiasTexels),
            minNormalBiasTexels);
    float normalBiasTexels = lerp(
        minNormalBiasTexels,
        maxNormalBiasTexels,
        saturate(1.0 - LocalShadowFiniteOr(dot(normal, surfaceToLight), 0.0)));
    return worldPosition +
        normal * max(LocalShadowFiniteOr(layer.params.x, 0.0), 0.0) *
        normalBiasTexels;
}

float SampleLocalShadowLayerPcf(
    uint layerIndex,
    float2 shadowUv,
    float compareDepth,
    float filterRadiusTexels,
    float3 receiverPosition,
    float bias,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    if (!isfinite(compareDepth))
    {
        return 1.0;
    }

    compareDepth = saturate(compareDepth);
    float atlasResolution =
        max(LocalShadowFiniteOr(float(localShadow.counts.z),
                                float(LOCAL_SHADOW_MAP_RESOLUTION)),
            float(LOCAL_SHADOW_MAP_RESOLUTION));
    float invResolution = 1.0 / atlasResolution;
    float filterStepTexels =
        filterRadiusTexels / LOCAL_SHADOW_PCF_KERNEL_RADIUS;
    float2 texelSize = (filterStepTexels * invResolution).xx;
    float rotation = LocalShadowStableRotation(
        shadowUv, layerIndex, atlasResolution);
    float outerSampleWeight = LocalShadowOuterSampleWeight(filterRadiusTexels);
    uint outerSampleCount = uint(ceil(outerSampleWeight *
        float(LOCAL_SHADOW_POISSON_SAMPLE_COUNT -
              LOCAL_SHADOW_PCF_FAST_SAMPLE_COUNT)));
    uint activeSampleCount = min(
        LOCAL_SHADOW_PCF_FAST_SAMPLE_COUNT + outerSampleCount,
        LOCAL_SHADOW_POISSON_SAMPLE_COUNT);
    float shadow = 0.0;
    float totalWeight = 0.0;

    [loop]
    for (uint sampleIndex = 0u;
         sampleIndex < activeSampleCount;
         ++sampleIndex)
    {
        float2 o = RotateLocalShadowPoissonOffset(
            LocalShadowPoissonOffset(sampleIndex), rotation);
        float weight = exp2(-dot(o, o) * 0.70);
        if (sampleIndex >= LOCAL_SHADOW_PCF_FAST_SAMPLE_COUNT)
        {
            weight *= outerSampleWeight;
        }
        float2 tapUv = shadowUv + o * texelSize;
        LocalShadowLayerData layer = localShadow.layers[layerIndex];
        bool cubeLayer = layer.directionType.w == LOCAL_SHADOW_TYPE_POINT ||
                         layer.directionType.w == LOCAL_SHADOW_TYPE_AREA;
        if (cubeLayer)
        {
            shadow += SampleLocalShadowCubeTap(layerIndex, tapUv,
                receiverPosition, bias, localShadow, localShadowAtlas,
                localShadowSampler, invResolution) * weight;
        }
        else if (all(isfinite(tapUv)) &&
            all(tapUv >= 0.0.xx) && all(tapUv <= 1.0.xx))
        {
            shadow += localShadowAtlas.SampleCmpLevelZero(
                localShadowSampler,
                float3(tapUv, float(layerIndex)),
                compareDepth) * weight;
        }
        else
        {
            shadow += weight;
        }
        totalWeight += weight;
    }

    return shadow / max(totalWeight, 1e-4);
}

float SampleLocalShadowLayer(
    uint layerIndex,
    float3 worldPosition,
    float3 receiverNormal,
    float3 surfaceToLight,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    if (localShadow.counts.w == 0u ||
        layerIndex >= localShadow.counts.x ||
        layerIndex >= MAX_SHADOWED_LOCAL_LIGHT_LAYERS)
    {
        return 1.0;
    }

    LocalShadowLayerData layer = localShadow.layers[layerIndex];
    if (layer.meta.w == 0u)
    {
        return 1.0;
    }

    float3 lightToReceiver = worldPosition - layer.positionRange.xyz;
    float distanceToLight = length(lightToReceiver);
    float rawLightRange = layer.positionRange.w;
    if (!isfinite(rawLightRange) || rawLightRange <= 1e-4)
    {
        return 1.0;
    }
    float lightRange = max(rawLightRange, 1e-4);
    if (!isfinite(distanceToLight) ||
        distanceToLight <= 1e-4 ||
        distanceToLight >= lightRange)
    {
        return 1.0;
    }
    float rangeFadeStart = lightRange * 0.95;
    float rangeVisibility =
        saturate((lightRange - distanceToLight) /
                 max(lightRange - rangeFadeStart, 1e-4));

    float3 normal = SafeNormalize(receiverNormal, -surfaceToLight);
    float3 samplePosition = OffsetLocalShadowSamplePosition(
        worldPosition, normal, surfaceToLight, layer, localShadow);
    bool cubeLayer = layer.directionType.w == LOCAL_SHADOW_TYPE_POINT ||
                     layer.directionType.w == LOCAL_SHADOW_TYPE_AREA;
    if (cubeLayer)
    {
        // Normal bias can move a receiver across a face boundary before PCF.
        layerIndex = layerIndex - layer.meta.y + SelectPointLocalShadowFace(
            samplePosition - layer.positionRange.xyz);
        if (layerIndex >= localShadow.counts.x) return 1.0;
        layer = localShadow.layers[layerIndex];
    }
    float4 shadowClip = mul(layer.viewProj, float4(samplePosition, 1.0));
    if (!all(isfinite(shadowClip)) || shadowClip.w <= 1e-6)
    {
        return 1.0;
    }

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    if (!all(isfinite(shadowNdc)))
    {
        return 1.0;
    }

    float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    if (!all(isfinite(shadowUv)) ||
        any(shadowUv < 0.0.xx) || any(shadowUv > 1.0.xx) ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    float bias = LocalShadowSlopeBias(normal, surfaceToLight, localShadow) +
        LocalShadowReceiverPlaneBias(shadowNdc, localShadow);
    float filterRadiusTexels = LocalShadowSoftFilterRadiusTexels(
        layer, distanceToLight, localShadow);
    float shadow = SampleLocalShadowLayerPcf(
        layerIndex, shadowUv, shadowNdc.z + bias, filterRadiusTexels,
        samplePosition, bias, localShadow, localShadowAtlas, localShadowSampler);
    return lerp(1.0, shadow, rangeVisibility);
}

float LocalPointLightShadowVisibility(
    PointLightData light,
    float3 worldPosition,
    float3 receiverNormal,
    float3 surfaceToLight,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    uint baseLayer = LocalShadowBaseLayerFromPointLight(light);
    uint layerCount = LocalShadowLayerCountFromPointLight(light);
    if (!PointLightHasLocalShadowLayers(light, localShadow))
    {
        return 1.0;
    }

    bool isSpot = light.coneOuterCosType.y >= 0.5;
    uint layerIndex = baseLayer;
    if (!isSpot && layerCount >= LOCAL_SHADOW_POINT_FACE_COUNT)
    {
        layerIndex += SelectPointLocalShadowFace(
            worldPosition - light.positionRadius.xyz);
    }
    if (layerIndex >= baseLayer + layerCount)
    {
        return 1.0;
    }

    return SampleLocalShadowLayer(
        layerIndex, worldPosition, receiverNormal, surfaceToLight,
        localShadow, localShadowAtlas, localShadowSampler);
}

// Equal-area emitter domains match CPU cube origins. Filtering integrates
// residual area within each domain, rather than stacking four hard silhouettes.
float3 AreaShadowDomainPosition(AreaLightFrame frame, uint domain, uint domains,
                               uint tap, uint taps, float2 jitter)
{
    uint columns = taps == 16u ? 4u : 8u;
    uint rows = taps / columns;
    float2 subcell = frac(float(tap) * float2(0.754877666, 0.569840296) + jitter);
    float2 q = float2((float(tap % columns) + subcell.x) / float(columns),
                      (float(tap / columns) + subcell.y) / float(rows));
    float2 offset;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        float angle = 2.0 * PI * (float(domain) + q.y) / float(domains);
        offset = sqrt(q.x) * float2(cos(angle), sin(angle));
    }
    else if (domains == 4u)
    {
        offset = q + float2(float(domain & 1u), float((domain >> 1u) & 1u)) - 1.0;
    }
    else
    {
        offset = float2(2.0 * (float(domain) + q.x) / float(domains) - 1.0,
                        2.0 * q.y - 1.0);
    }
    return frame.position + frame.tangent * (offset.x * frame.halfSize.x) +
           frame.bitangent * (offset.y * frame.halfSize.y);
}

// Project a filter ray onto the actual receiver plane. Comparing at a constant
// radial distance would shadow the sloping receiver itself near cube seams.
bool AreaShadowProjectTap(uint cubeBase, float3 target, float3 receiver,
                          float3 normal, LocalShadowBuffer localShadow,
                          out uint layerIndex, out float3 projected,
                          out float3 direction)
{
    float3 origin = localShadow.layers[cubeBase].positionRange.xyz;
    layerIndex = cubeBase;
    projected = 0.0.xxx;
    direction = SafeNormalize(target - origin, normal);
    float denominator = dot(direction, normal);
    float distance = dot(receiver - origin, normal) / denominator;
    if (!isfinite(distance) || abs(denominator) < 1e-5 || distance <= 1e-5) return false;
    layerIndex = cubeBase + SelectPointLocalShadowFace(direction);
    float4 clip = mul(localShadow.layers[layerIndex].viewProj,
                      float4(origin + direction * distance, 1.0));
    projected = clip.xyz / clip.w;
    // Local shadow maps use a positive-height viewport: no scene Y inversion.
    projected.xy = projected.xy * 0.5 + 0.5;
    return all(isfinite(projected)) && clip.w > 1e-5 &&
           projected.z >= 0.0 && projected.z <= 1.0;
}

float AreaShadowBlockerDepth(uint cubeBase, float3 target, float3 receiver,
    float3 normal, float3 lightNormal, float bias, LocalShadowBuffer localShadow,
    Texture2DArray<float> atlas)
{
    uint layerIndex; float3 projected, direction;
    if (!AreaShadowProjectTap(cubeBase, target, receiver, normal, localShadow,
                              layerIndex, projected, direction)) return 0.0;
    int resolution = int(localShadow.counts.z);
    int2 texel = clamp(int2(projected.xy * float(resolution)),
                       int2(0, 0), int2(resolution - 1, resolution - 1));
    float stored = atlas.Load(int4(texel, int(layerIndex), 0));
    if (!isfinite(stored) || stored <= projected.z + bias) return 0.0;
    LocalShadowLayerData layer = localShadow.layers[layerIndex];
    float nearPlane = layer.params.z;
    float farPlane = nearPlane + layer.params.y;
    // Linearize finite perspective reverse-Z before averaging blocker depth.
    float axialDistance = nearPlane * farPlane /
        max(nearPlane + stored * (farPlane - nearPlane), 1e-6);
    float radialDistance = axialDistance / max(dot(direction, layer.directionType.xyz), 1e-5);
    float depth = radialDistance * dot(direction, lightNormal);
    return isfinite(depth) && depth > nearPlane ? depth : 0.0;
}

float SampleAreaShadowDomain(uint encodedLayer, uint domain, uint domains,
    AreaLightData light, float3 worldPosition, float3 receiverNormal,
    LocalShadowBuffer localShadow, Texture2DArray<float> atlas,
    SamplerComparisonState sampler)
{
    uint cubeBase = encodedLayer - 1u + domain * LOCAL_SHADOW_POINT_FACE_COUNT;
    LocalShadowLayerData layer = localShadow.layers[cubeBase];
    uint quality = uint(clamp(layer.params.w, 0.0, 3.0));
    float3 surfaceToLight = SafeNormalize(layer.positionRange.xyz - worldPosition,
                                          float3(0, 1, 0));
    uint centerFace = SelectPointLocalShadowFace(worldPosition - layer.positionRange.xyz);
    if (quality == 0u)
        return SampleLocalShadowLayer(cubeBase + centerFace, worldPosition,
            receiverNormal, surfaceToLight, localShadow, atlas, sampler);
    AreaLightFrame frame = BuildAreaLightFrame(light);
    float3 normal = SafeNormalize(receiverNormal, -surfaceToLight);
    float3 receiver = OffsetLocalShadowSamplePosition(worldPosition, normal,
        surfaceToLight, layer, localShadow);
    float receiverDepth = dot(receiver - layer.positionRange.xyz, frame.normal);
    if (!isfinite(receiverDepth) || receiverDepth <= layer.params.z) return 1.0;
    float bias = LocalShadowSlopeBias(normal, surfaceToLight, localShadow);

    // Other domain centers seed the search even when this center ray is lit.
    float blockerSum = 0.0; float blockers = 0.0;
    [loop] for (uint seed = 0u; seed < domains; ++seed)
    {
        uint seedBase = encodedLayer - 1u + seed * LOCAL_SHADOW_POINT_FACE_COUNT;
        float depth = AreaShadowBlockerDepth(seedBase, receiver, receiver,
            normal, frame.normal, bias, localShadow, atlas);
        if (depth > 0.0 && depth < receiverDepth) { blockerSum += depth; blockers += 1.0; }
    }
    float searchScale = blockers > 0.0 ?
        max(receiverDepth / (blockerSum / blockers) - 1.0, 0.0) : 3.0;
    [loop] for (uint tap = 0u; tap < 16u; ++tap)
    {
        float3 emitter = AreaShadowDomainPosition(frame, domain, domains, tap, 16u, 0.5.xx);
        float3 target = receiver + (emitter - layer.positionRange.xyz) * searchScale;
        float depth = AreaShadowBlockerDepth(cubeBase, target, receiver,
            normal, frame.normal, bias, localShadow, atlas);
        if (depth > 0.0 && depth < receiverDepth) { blockerSum += depth; blockers += 1.0; }
    }
    if (blockers == 0.0) return 1.0;
    float blockerDepth = blockerSum / blockers;
    float penumbraScale = max(receiverDepth / max(blockerDepth, layer.params.z) - 1.0, 0.0);
    uint taps = 8u << quality;
    float visibility = 0.0;
    float halfTexel = 0.5 / float(localShadow.counts.z);
    float4 centerClip = mul(localShadow.layers[cubeBase + centerFace].viewProj,
                             float4(receiver, 1.0));
    float2 centerUv = centerClip.xy / max(centerClip.w, 1e-5) * 0.5 + 0.5;
    float rotation = LocalShadowStableRotation(centerUv, cubeBase, float(localShadow.counts.z));
    // World/light-texel seeds break aligned finite-sample bands without needing
    // submitted-frame randomness or a visibility history. TAA remains optional.
    float2 jitter = frac(float2(rotation * 0.159154943, rotation * 0.618033989));
    [loop] for (uint tap = 0u; tap < taps; ++tap)
    {
        float3 emitter = AreaShadowDomainPosition(frame, domain, domains, tap, taps, jitter);
        float3 target = receiver + (emitter - layer.positionRange.xyz) * penumbraScale;
        uint tapLayer; float3 projected, direction;
        if (AreaShadowProjectTap(cubeBase, target, receiver, normal, localShadow,
                                 tapLayer, projected, direction))
        {
            float2 uv = clamp(projected.xy, halfTexel.xx, (1.0 - halfTexel).xx);
            visibility += atlas.SampleCmpLevelZero(sampler, float3(uv, float(tapLayer)),
                saturate(projected.z + bias));
        }
        else visibility += 1.0;
    }
    float distance = length(worldPosition - layer.positionRange.xyz);
    float fade = saturate((layer.positionRange.w - distance) /
                           max(layer.positionRange.w * 0.05, 1e-4));
    return lerp(1.0, visibility / float(taps), fade);
}

float LocalAreaLightShadowVisibility(
    uint areaIndex,
    AreaLightData light,
    float3 worldPosition,
    float3 receiverNormal,
    uint visibilitySample,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    uint encodedLayer = PackedLocalShadowAreaRef(localShadow, areaIndex);
    if (encodedLayer == 0u)
    {
        return 1.0;
    }

    uint sampleCount = LocalAreaShadowSampleCount(encodedLayer, localShadow);
    if (visibilitySample >= sampleCount) return 1.0;
    return SampleAreaShadowDomain(encodedLayer, visibilitySample, sampleCount,
        light, worldPosition, receiverNormal,
        localShadow, localShadowAtlas, localShadowSampler);
}
