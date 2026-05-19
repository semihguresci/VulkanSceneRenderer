// shadow_common.slang — Shared shadow sampling functions.
// Included by deferred_directional.slang for CSM shadow evaluation.
#pragma once

#include "lighting_structs.slang"

static const float SHADOW_PCF_KERNEL_RADIUS = 2.0;
static const uint SHADOW_PCSS_BLOCKER_SAMPLE_COUNT = 16u;

float ShadowFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 ShadowNdcToUv(float2 shadowNdcXY)
{
    // Shadow cascades render with a positive-height viewport, so the atlas
    // keeps Vulkan's default framebuffer orientation and does not need the
    // scene pass Y inversion used for negative-height viewports.
    return shadowNdcXY * 0.5 + 0.5;
}

float2 DirectionalShadowPoissonOffset(uint sampleIndex)
{
    static const float2 offsets[16] = {
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
    };
    return offsets[sampleIndex % SHADOW_PCSS_BLOCKER_SAMPLE_COUNT];
}

float ShadowStableRotation(float2 shadowUv, uint cascadeIndex)
{
    float seed = dot(shadowUv, float2(127.1, 311.7)) +
        float(cascadeIndex) * 19.19;
    return frac(sin(seed) * 43758.5453123) * 6.28318530718;
}

float2 RotateDirectionalShadowOffset(float2 offset, float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return float2(offset.x * c - offset.y * s,
                  offset.x * s + offset.y * c);
}

float ComputeReceiverPlaneDepthBias(float3 shadowNDC, ShadowBuffer shadowData)
{
    float scale = max(ShadowFiniteOr(shadowData.biasSettings.w, 0.0), 0.0);
    float maxBias = max(ShadowFiniteOr(shadowData.filterSettings.w, 0.0), 0.0);
    if (scale <= 0.0 || maxBias <= 0.0 || !all(isfinite(shadowNDC)))
    {
        return 0.0;
    }

    // Directional CSM sampling is reached through material, cascade, and
    // shadow-enable branches. Screen-space derivatives in that non-uniform
    // control flow can alternate between lanes and make large receivers flash.
    // Keep this term derivative-free; stable slope, constant, and normal bias
    // still handle acne while PCSS controls the penumbra width.
    return 0.0;
}

float ComputeSlopeScaledBias(float3 normal, float3 lightDir,
                             ShadowBuffer shadowData)
{
    float NdotL = saturate(ShadowFiniteOr(dot(normal, -lightDir), 0.0));
    float slope = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.12);
    float constantBias = max(ShadowFiniteOr(shadowData.filterSettings.z, 0.0),
                             0.0);
    float slopeScale = max(ShadowFiniteOr(shadowData.biasSettings.z, 0.0),
                           0.0);
    float maxBias =
        max(ShadowFiniteOr(shadowData.filterSettings.w, constantBias), 0.0);
    return min(constantBias + slope * slopeScale, maxBias);
}

float3 OffsetShadowSamplePosition(
    float3 worldPosition,
    float3 normal,
    float3 lightDir,
    uint cascadeIndex,
    ShadowBuffer shadowData)
{
    float minNormalBiasTexels =
        max(ShadowFiniteOr(shadowData.biasSettings.x, 0.0), 0.0);
    float maxNormalBiasTexels =
        max(ShadowFiniteOr(shadowData.biasSettings.y, minNormalBiasTexels),
            minNormalBiasTexels);
    float normalBiasTexels = lerp(
        minNormalBiasTexels,
        maxNormalBiasTexels,
        saturate(1.0 - ShadowFiniteOr(dot(normal, -lightDir), 0.0)));
    float normalBias =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].texelSize, 0.0),
            0.0) *
        normalBiasTexels;
    return worldPosition + normal * normalBias;
}

bool ProjectShadowCascade(
    float3 samplePos,
    uint cascadeIndex,
    ShadowBuffer shadowData,
    out float3 shadowNDC)
{
    shadowNDC = 0.0.xxx;

    float4 shadowClip =
        mul(shadowData.cascades[cascadeIndex].viewProj, float4(samplePos, 1.0));
    if (!all(isfinite(shadowClip)) || shadowClip.w <= 1e-6)
    {
        return false;
    }

    float3 projectedNDC = shadowClip.xyz / shadowClip.w;
    if (!all(isfinite(projectedNDC)))
    {
        return false;
    }

    shadowNDC = projectedNDC;
    return true;
}

float SampleWeightedPcfWithRadius(
    float2 shadowUV,
    uint cascadeIndex,
    float compareDepth,
    float filterRadiusTexels,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    if (!isfinite(compareDepth))
    {
        return 1.0;
    }

    compareDepth = saturate(compareDepth);
    filterRadiusTexels = max(ShadowFiniteOr(filterRadiusTexels, 0.0), 0.0);
    ShadowCascadeData cascade = shadowData.cascades[cascadeIndex];
    float cascadeExtent =
        max(ShadowFiniteOr(cascade.worldRadius, 0.0) * 2.0, 1e-4);
    float cascadeTexelSize = max(ShadowFiniteOr(cascade.texelSize, 0.0), 0.0);
    float filterStepTexels =
        filterRadiusTexels / SHADOW_PCF_KERNEL_RADIUS;
    float2 texelSize = (filterStepTexels * cascadeTexelSize / cascadeExtent).xx;
    float shadow = 0.0;
    float totalWeight = 0.0;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 o = float2(float(x), float(y));
            float weight = exp2(-dot(o, o) * 0.55);
            float2 tapUv = shadowUV + o * texelSize;
            if (all(isfinite(tapUv)) &&
                all(tapUv >= 0.0.xx) && all(tapUv <= 1.0.xx))
            {
                shadow += shadowAtlas.SampleCmpLevelZero(
                    shadowSampler,
                    float3(tapUv, float(cascadeIndex)),
                    compareDepth) * weight;
            }
            else
            {
                shadow += weight;
            }
            totalWeight += weight;
        }
    }

    return shadow / max(totalWeight, 1e-4);
}

float SampleWeightedPcf(
    float2 shadowUV,
    uint cascadeIndex,
    float compareDepth,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    float filterRadiusTexels =
        max(ShadowFiniteOr(shadowData.filterSettings.x, 0.0), 0.0);
    return SampleWeightedPcfWithRadius(
        shadowUV, cascadeIndex, compareDepth, filterRadiusTexels, shadowData,
        shadowAtlas, shadowSampler);
}

float DirectionalPcssAverageBlockerDepth(
    float2 shadowUV,
    uint cascadeIndex,
    float receiverDepth,
    float searchRadiusTexels,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    out uint blockerCount)
{
    blockerCount = 0u;
    if (!all(isfinite(shadowUV)) || !isfinite(receiverDepth) ||
        !isfinite(searchRadiusTexels) || searchRadiusTexels <= 0.0)
    {
        return 0.0;
    }

    uint width = 0u;
    uint height = 0u;
    uint layers = 0u;
    shadowAtlas.GetDimensions(width, height, layers);
    if (width == 0u || height == 0u || cascadeIndex >= layers)
    {
        return 0.0;
    }

    float2 atlasSize = float2(float(width), float(height));
    int2 minTexel = int2(0, 0);
    int2 maxTexel = int2(int(width) - 1, int(height) - 1);
    int2 baseTexel = clamp(int2(floor(shadowUV * atlasSize)),
                           minTexel,
                           maxTexel);
    float rotation = ShadowStableRotation(shadowUV, cascadeIndex);
    float avgBlockerDepth = 0.0;
    float receiverCompareDepth = saturate(receiverDepth);

    [unroll]
    for (uint sampleIndex = 0u;
         sampleIndex < SHADOW_PCSS_BLOCKER_SAMPLE_COUNT;
         ++sampleIndex)
    {
        float2 o = RotateDirectionalShadowOffset(
            DirectionalShadowPoissonOffset(sampleIndex), rotation);
        int2 sampleTexel =
            int2(round(float2(baseTexel) + o * searchRadiusTexels));
        if (any(sampleTexel < minTexel) ||
            any(sampleTexel > maxTexel))
        {
            continue;
        }
        float storedDepth =
            shadowAtlas.Load(int4(sampleTexel, int(cascadeIndex), 0));

        // Reverse-Z: stored depths larger than the receiver compare depth are
        // closer to the light and therefore can cast onto this receiver.
        if (isfinite(storedDepth) && storedDepth > receiverCompareDepth)
        {
            avgBlockerDepth += storedDepth;
            blockerCount += 1u;
        }
    }

    return blockerCount > 0u ? avgBlockerDepth / float(blockerCount) : 0.0;
}

float DirectionalPcssSearchRadiusTexels(
    uint cascadeIndex,
    float receiverDepth,
    float lightTanRadius,
    float baseSearchRadiusTexels,
    float maxSearchRadiusTexels,
    ShadowBuffer shadowData)
{
    float depthRange =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].depthRange, 0.0),
            1.0e-4);
    float texelSizeWorld =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].texelSize, 0.0),
            1.0e-5);
    float receiverDistanceFromNearWorld =
        saturate(1.0 - receiverDepth) * depthRange;
    float adaptiveSearchRadiusTexels =
        receiverDistanceFromNearWorld * lightTanRadius / texelSizeWorld;

    return clamp(max(baseSearchRadiusTexels, adaptiveSearchRadiusTexels),
                 baseSearchRadiusTexels,
                 max(maxSearchRadiusTexels, baseSearchRadiusTexels));
}

float DirectionalPcssFilterRadiusTexels(
    float2 shadowUV,
    uint cascadeIndex,
    float receiverDepth,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas)
{
    float baseFilterRadiusTexels =
        max(ShadowFiniteOr(shadowData.filterSettings.x, 0.0), 0.0);
    if (ShadowFiniteOr(shadowData.softShadowSettings.x, 0.0) <= 0.0)
    {
        return baseFilterRadiusTexels;
    }

    float lightTanRadius =
        max(ShadowFiniteOr(shadowData.softShadowSettings.y, 0.0), 0.0);
    float maxBlockerSearchRadiusTexels =
        max(ShadowFiniteOr(shadowData.softShadowSettings.z,
                           baseFilterRadiusTexels),
            baseFilterRadiusTexels);
    float maxPcssFilterRadiusTexels =
        max(ShadowFiniteOr(shadowData.softShadowSettings.w,
                           baseFilterRadiusTexels),
            baseFilterRadiusTexels);
    if (lightTanRadius <= 0.0 || maxBlockerSearchRadiusTexels <= 0.0 ||
        maxPcssFilterRadiusTexels <= baseFilterRadiusTexels)
    {
        return baseFilterRadiusTexels;
    }

    float blockerSearchRadiusTexels = DirectionalPcssSearchRadiusTexels(
        cascadeIndex, receiverDepth, lightTanRadius, baseFilterRadiusTexels,
        maxBlockerSearchRadiusTexels, shadowData);
    uint blockerCount = 0u;
    float avgBlockerDepth = DirectionalPcssAverageBlockerDepth(
        shadowUV, cascadeIndex, receiverDepth, blockerSearchRadiusTexels,
        shadowData, shadowAtlas, blockerCount);
    if (blockerCount == 0u)
    {
        return baseFilterRadiusTexels;
    }

    float depthRange =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].depthRange, 0.0),
            1.0e-4);
    float texelSizeWorld =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].texelSize, 0.0),
            1.0e-5);
    float depthSeparationWorld =
        max(avgBlockerDepth - receiverDepth, 0.0) * depthRange;
    float penumbraTexels =
        depthSeparationWorld * lightTanRadius / texelSizeWorld;

    return clamp(max(baseFilterRadiusTexels, penumbraTexels),
                 baseFilterRadiusTexels, maxPcssFilterRadiusTexels);
}

// Select the cascade index based on the fragment's view-space depth.
// splitDepths are in linear view-space depth, compared with reverse-Z convention
// where near=1 and far=0 in NDC but splitDepths use positive linear distances.
uint SelectCascade(float viewDepth, ShadowBuffer shadowData)
{
    if (!isfinite(viewDepth) || viewDepth <= 0.0)
    {
        return SHADOW_CASCADE_COUNT - 1;
    }

    for (uint i = 0; i < SHADOW_CASCADE_COUNT - 1; ++i)
    {
        float splitDepth = ShadowFiniteOr(shadowData.cascades[i].splitDepth, 0.0);
        if (splitDepth > 0.0 && viewDepth < splitDepth)
        {
            return i;
        }
    }
    return SHADOW_CASCADE_COUNT - 1;
}

// Sample the shadow factor for a single cascade (no blending).
// Returns 1.0 if fully lit, 0.0 if fully shadowed.
float SampleCascadeShadow(
    float3 samplePos,
    float3 normal,
    float3 lightDir,
    uint cascadeIndex,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    float3 shadowNDC = 0.0.xxx;
    if (!ProjectShadowCascade(samplePos, cascadeIndex, shadowData, shadowNDC))
    {
        return 1.0;
    }

    float2 shadowUV = ShadowNdcToUv(shadowNDC.xy);

    if (!all(isfinite(shadowUV)) ||
        any(shadowUV < 0.0) || any(shadowUV > 1.0) ||
        shadowNDC.z < 0.0 || shadowNDC.z > 1.0)
    {
        return 1.0;
    }

    float bias = ComputeSlopeScaledBias(normal, lightDir, shadowData) +
                 ComputeReceiverPlaneDepthBias(shadowNDC, shadowData);
    float receiverDepth = saturate(shadowNDC.z);
    // Reverse-Z: push compare depth towards the light (larger = closer = near=1).
    float compareDepth = shadowNDC.z + bias;
    float baseFilterRadiusTexels =
        max(ShadowFiniteOr(shadowData.filterSettings.x, 0.0), 0.0);
    float filterRadiusTexels = baseFilterRadiusTexels;
    bool pcssCanExpandPenumbra =
        ShadowFiniteOr(shadowData.softShadowSettings.x, 0.0) > 0.0 &&
        ShadowFiniteOr(shadowData.softShadowSettings.y, 0.0) > 0.0 &&
        ShadowFiniteOr(shadowData.softShadowSettings.z,
                       baseFilterRadiusTexels) > 0.0 &&
        ShadowFiniteOr(shadowData.softShadowSettings.w,
                       baseFilterRadiusTexels) > baseFilterRadiusTexels;
    if (pcssCanExpandPenumbra)
    {
        filterRadiusTexels = DirectionalPcssFilterRadiusTexels(
            shadowUV, cascadeIndex, receiverDepth, shadowData, shadowAtlas);
    }

    return SampleWeightedPcfWithRadius(
        shadowUV, cascadeIndex, compareDepth, filterRadiusTexels, shadowData,
        shadowAtlas, shadowSampler);
}

// Compute the shadow factor for a world-space position.
// Returns 1.0 if fully lit, 0.0 if fully shadowed.
// Blends between adjacent cascades in the overlap zone for smooth transitions.
float ComputeShadowFactor(
    float3 worldPosition,
    float3 normal,
    float3 lightDir,
    float viewDepth,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    if (!all(isfinite(worldPosition)) ||
        !all(isfinite(normal)) ||
        !all(isfinite(lightDir)) ||
        !isfinite(viewDepth))
    {
        return 1.0;
    }

    uint cascadeIndex = SelectCascade(viewDepth, shadowData);

    // Normal offset bias is scaled by each cascade's texel size.
    float3 samplePos = OffsetShadowSamplePosition(
        worldPosition, normal, lightDir, cascadeIndex, shadowData);
    float shadow = SampleCascadeShadow(
        samplePos, normal, lightDir, cascadeIndex,
        shadowData, shadowAtlas, shadowSampler);

    // Blend with the next cascade near the split boundary for smooth transitions.
    if (cascadeIndex < SHADOW_CASCADE_COUNT - 1)
    {
        float splitDepth =
            ShadowFiniteOr(shadowData.cascades[cascadeIndex].splitDepth, 0.0);
        float prevSplit = (cascadeIndex == 0)
            ? 0.0
            : ShadowFiniteOr(shadowData.cascades[cascadeIndex - 1].splitDepth,
                             0.0);
        float cascadeRange = splitDepth - prevSplit;
        float blendFraction =
            clamp(ShadowFiniteOr(shadowData.filterSettings.y, 0.0), 0.0, 0.45);
        float blendZone = cascadeRange * blendFraction;
        float blendStart = splitDepth - blendZone;

        if (isfinite(viewDepth) && cascadeRange > 1e-6 && viewDepth > blendStart)
        {
            float3 nextSamplePos = OffsetShadowSamplePosition(
                worldPosition, normal, lightDir, cascadeIndex + 1u,
                shadowData);
            float nextShadow = SampleCascadeShadow(
                nextSamplePos, normal, lightDir, cascadeIndex + 1,
                shadowData, shadowAtlas, shadowSampler);

            float blendFactor = (viewDepth - blendStart) / max(blendZone, 1e-6);
            blendFactor = saturate(blendFactor);
            shadow = lerp(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}
