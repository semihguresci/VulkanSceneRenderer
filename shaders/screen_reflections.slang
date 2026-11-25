#include "brdf_common.slang"
#include "lighting_structs.slang"
#include "material_data_common.slang"
#include "pbr_extended_deferred_common.slang"
#include "temporal_common.slang"

#ifndef SCREEN_REFLECTION_RAY_QUERY
#define SCREEN_REFLECTION_RAY_QUERY 0
#endif

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> camera;
[[vk::binding(2, 0)]] Texture2D<float4> albedoImage;
[[vk::binding(3, 0)]] Texture2D<float4> normalImage;
[[vk::binding(4, 0)]] Texture2D<float4> materialImage;
[[vk::binding(11, 0)]] TextureCube<float4> environmentSpecular;
[[vk::binding(12, 0)]] Texture2D<float2> brdfLut;
[[vk::binding(13, 0)]] SamplerState environmentSampler;
[[vk::binding(14, 0)]] SamplerState brdfSampler;
[[vk::binding(15, 0)]] Texture2D<float> ambientOcclusion;
[[vk::binding(16, 0)]] SamplerState aoSampler;
[[vk::binding(17, 0)]] Texture2D<float4> specularImage;
[[vk::binding(21, 0)]] Texture2D<float4> materialLayers;
[[vk::binding(22, 0)]] Texture2D<float4> materialSheen;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> lights;
[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> materials;
[[vk::binding(0, 3)]] Texture2D<float4> opaqueColor;
[[vk::binding(1, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColor;
[[vk::binding(2, 3)]] Texture2D<float> sceneDepth;
[[vk::binding(8, 3)]] Texture2D<uint4> previousReflection;
[[vk::binding(9, 3)]] [[vk::image_format("rgba32ui")]] RWTexture2D<uint4> currentReflection;
[[vk::binding(10, 3)]] Texture2D<float2> depthIntervals;

struct ScreenReflectionConstants
{
    uint2 extent;
    uint maxSteps;
    uint debugView;
    uint4 history; // save, reproject, virtual depth mip count (zero uses the linear trace), reserved
#if SCREEN_REFLECTION_RAY_QUERY
    uint4 rayLimits;
    float4 sectionPlane;
#endif
};
[[vk::push_constant]] ConstantBuffer<ScreenReflectionConstants> settings;

static const float kMaxDistance = 40.0;
static const float kMaxRoughness = 0.55;

#if SCREEN_REFLECTION_RAY_QUERY
#include "depth_normal_common.slang"
#include "screen_reflections_ray_common.slang"
#endif

struct ReflectionHit
{
    int2 pixel;
    float distance;
    float confidence;
    uint reason;
};

static const uint kReflectionHit = 0u;
static const uint kReflectionIneligible = 1u;
static const uint kReflectionInvalidRay = 2u;
static const uint kReflectionScreenExit = 3u;
static const uint kReflectionNoDepth = 4u;
static const uint kReflectionThickness = 5u;
static const uint kReflectionBackFace = 6u;
static const uint kReflectionBudget = 7u;

float3 reflectionReasonColor(uint reason)
{
    return float3(reason & 1u, (reason >> 1u) & 1u, (reason >> 2u) & 1u);
}

float2 reflectionPixelUv(int2 pixel)
{
    return (float2(pixel) + 0.5) / float2(settings.extent);
}

float3 reflectionNormal(int2 pixel)
{
    return SafeNormalize(normalImage.Load(int3(pixel, 0)).xyz * 2.0 - 1.0,
        float3(0.0, 1.0, 0.0));
}

bool reflectionPosition(int2 pixel, out float3 world)
{
    float depth = sceneDepth.Load(int3(pixel, 0));
    return depth > 0.0 && depth <= 1.0 &&
        TryReconstructWorldPosition(reflectionPixelUv(pixel), depth,
            camera.inverseViewProj, world);
}

float reflectionViewDepth(float3 world)
{
    return dot(world - camera.cameraWorldPosition.xyz, camera.cameraForward.xyz);
}

uint reflectionHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float3 sampleRoughDirection(float3 direction, float roughness, uint2 pixel)
{
    if (settings.history.x == 0u || roughness <= 0.12)
        return direction;
    uint seed = reflectionHash(pixel.x + pixel.y * settings.extent.x) ^ camera.temporalInfo.y;
    float2 noise = float2(reflectionHash(seed) & 0xffffu, reflectionHash(seed ^ 0x934ff631u) & 0xffffu) / 65536.0;
    float angle = noise.x * 6.28318531;
    float radius = roughness * roughness * 0.45 * sqrt(noise.y) * smoothstep(0.12, 0.24, roughness);
    float3 axis = abs(direction.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(axis, direction));
    return normalize(direction + radius * (cos(angle) * tangent + sin(angle) * cross(direction, tangent)));
}

uint packReflectionNormal(float3 normal)
{
    uint3 quantized = uint3(round(saturate(normal * 0.5 + 0.5) * 31.0));
    return quantized.x | (quantized.y << 5u) | (quantized.z << 10u);
}

float3 unpackReflectionNormal(uint bits)
{
    float3 components = float3(bits & 31u, (bits >> 5u) & 31u, (bits >> 10u) & 31u);
    return SafeNormalize(components * (2.0 / 31.0) - 1.0, float3(0, 1, 0));
}

// History stores the signed correction per unit of replacement confidence.
// Only the current sample determines how much of the base lobe is replaced.
float3 filterReflection(int2 pixel, float3 world, float3 normal, uint materialIndex, uint hitKey,
    float hitDistance, float roughness, float confidence, float3 correction, out bool accepted,
    out uint4 stored)
{
    accepted = false;
    stored = uint4(0);
    if (settings.history.x == 0u || materialIndex > 0xffffu || hitKey > 0xffffu || confidence < 0.01)
        return correction;
    float depth = sceneDepth.Load(int3(pixel, 0));
    if (depth < 1e-5)
        return correction;

    uint distanceCode = uint(round(saturate(log2(1.0 + hitDistance) / log2(1.0 + kMaxDistance)) * 255.0));
    if (settings.history.y != 0u)
    {
        TemporalProjectedPoint projected = temporalProjectSceneClip(mul(camera.previousJitteredViewProj, float4(world, 1.0)));
        if (projected.valid && all(projected.uv >= 0.0) && all(projected.uv < 1.0))
        {
            int2 address = int2(projected.uv * settings.extent);
            uint4 past = previousReflection.Load(int3(address, 0));
            float previousConfidence = float((past.y >> 16u) & 255u) / 255.0;
            float previousDepth = f16tof32(past.z & 0xffffu);
            float3 previousWorld;
            if (previousConfidence > 0.0 && abs(previousConfidence - confidence) < 0.25 &&
                (past.z >> 16u) == materialIndex && (past.w >> 16u) == hitKey &&
                abs(float(past.y >> 24u) - float(distanceCode)) <= 2.0 + 12.0 * roughness &&
                dot(unpackReflectionNormal(past.w), normal) > 0.94 &&
                TryReconstructWorldPosition(reflectionPixelUv(address), previousDepth,
                    camera.previousInverseViewProj, previousWorld) &&
                distance(world, previousWorld) < 0.02 + 0.012 * reflectionViewDepth(world))
            {
                float3 old = float3(f16tof32(past.x & 0xffffu), f16tof32(past.x >> 16u), f16tof32(past.y & 0xffffu));
                // Large changes indicate moving reflected content or lighting.
                // Clamp smaller stochastic deviations before accumulating.
                float3 magnitude = max(abs(correction), abs(old));
                float peak = max(magnitude.x, max(magnitude.y, magnitude.z));
                if (all(abs(old - correction) <= 0.015 + 0.4 * peak))
                {
                    float clip = 0.01 + 0.20 * peak;
                    float weight = lerp(0.45, 0.8, smoothstep(0.12, 0.5, roughness));
                    correction = lerp(correction, clamp(old, correction - clip, correction + clip), weight);
                    accepted = true;
                }
            }
        }
    }

    correction = clamp(correction, -65504.0.xxx, 65504.0.xxx);
    uint confidenceCode = uint(round(saturate(confidence) * 255.0));
    stored = uint4(f32tof16(correction.x) | (f32tof16(correction.y) << 16u),
        f32tof16(correction.z) | (confidenceCode << 16u) | (distanceCode << 24u),
        f32tof16(depth) | (materialIndex << 16u), packReflectionNormal(normal) | (hitKey << 16u));
    return correction;
}

// Bound the projected ray to the Vulkan clip volume before dividing by W.
// Interpolating world distance then uses the reciprocal of each endpoint W.
float reflectionClipLimit(float start, float end)
{
    return end < 0.0 ? max(start, 0.0) / max(start - end, 1e-7) : 1.0;
}

float reflectionRayDistance(float fraction, float startW, float endW, float limit)
{
    float startWeight = (1.0 - fraction) / startW;
    float endWeight = fraction / endW;
    return limit * endWeight / (startWeight + endWeight);
}

struct ProjectedReflectionRay
{
    float3 origin;
    float3 direction;
    float2 startPixel;
    float2 delta;
    float2 clipW;
    float2 reverseDepth;
    float startDepth;
    float depthSlope;
    float limit;
};

float traceDistance(ProjectedReflectionRay ray, float fraction)
{
    return reflectionRayDistance(fraction, ray.clipW.x, ray.clipW.y, ray.limit);
}

float traceShiftedDepth(ProjectedReflectionRay ray, float fraction, float2 clipOffset, float fallback)
{
    float inverseW = lerp(1.0 / ray.clipW.x, 1.0 / ray.clipW.y, fraction);
    float shiftedW = 1.0 + clipOffset.y * inverseW;
    if (shiftedW <= 1e-7)
        return fallback;
    return (lerp(ray.reverseDepth.x, ray.reverseDepth.y, fraction) + clipOffset.x * inverseW) / shiftedW;
}

bool testReflectionHit(ProjectedReflectionRay ray, int2 pixel, float reverseDepth,
    float fraction, float2 cellInterval, bool boundToCell, inout ReflectionHit hit)
{
    float3 surface;
    if (reverseDepth <= 0.0 || reverseDepth > 1.0 || !isfinite(reverseDepth) ||
        !TryReconstructWorldPosition(reflectionPixelUv(pixel), reverseDepth, camera.inverseViewProj, surface))
        return false;

    float t = traceDistance(ray, fraction);
    float surfaceDepth = reflectionViewDepth(surface);
    float thickness = 0.024 + min(surfaceDepth, 40.0) * 0.0025;
    // Check the continuous ray against the pixel's depth plane. At mip 0 the
    // cell interval bounds the closest approach to that plane.
    if (abs(ray.depthSlope) > 1e-4)
    {
        float intersection = (surfaceDepth - ray.startDepth) / ray.depthSlope;
        if (boundToCell)
        {
            t = clamp(intersection, traceDistance(ray, cellInterval.x), traceDistance(ray, cellInterval.y));
        }
        else if (intersection > thickness && intersection < ray.limit)
        {
            float4 projected = mul(camera.viewProj, float4(ray.origin + ray.direction * intersection, 1.0));
            float2 projectedPixel = temporalSceneNdcToUv(projected.xy / projected.w) * settings.extent;
            if (all(abs(projectedPixel - (float2(pixel) + 0.5)) < 0.85))
                t = intersection;
        }
    }
    float offset = ray.startDepth + t * ray.depthSlope - surfaceDepth;
    if (offset < -thickness * 0.25 || offset > thickness || t <= thickness)
    {
        if (hit.reason != kReflectionBackFace)
            hit.reason = kReflectionThickness;
        return false;
    }
    if (dot(reflectionNormal(pixel), ray.direction) >= -0.015)
    {
        hit.reason = kReflectionBackFace;
        return false;
    }

    float2 uv = reflectionPixelUv(pixel);
    float2 edge = min(uv, 1.0 - uv);
    hit.pixel = pixel;
    hit.distance = t;
    hit.confidence = smoothstep(0.0, 0.045, min(edge.x, edge.y)) *
        (1.0 - smoothstep(0.70, 1.0, t / kMaxDistance));
    hit.reason = kReflectionHit;
    return true;
}

ReflectionHit traceLinearReflection(ProjectedReflectionRay ray, float span, uint exitReason)
{
    ReflectionHit hit = {};
    hit.reason = exitReason;
    // A full pixel stride catches narrow geometry at close range. On longer
    // projections the fixed ray budget caps the total texture fetch count.
    float stride = max(1.0, span / float(settings.maxSteps));
    for (uint step = 0; step < settings.maxSteps; ++step)
    {
        float at = 1.5 + float(step) * stride;
        if (at > span)
            return hit;
        float fraction = at / span;
        int2 pixel = int2(floor(ray.startPixel + ray.delta * fraction));
        if (any(pixel < 0) || any(pixel >= int2(settings.extent)))
            return hit;
        if (testReflectionHit(ray, pixel, sceneDepth.Load(int3(pixel, 0)), fraction, 0.0.xx, false, hit))
            return hit;
    }
    if (1.5 + float(settings.maxSteps) * stride <= span)
        hit.reason = kReflectionBudget;
    return hit;
}

ReflectionHit traceHiZReflection(ProjectedReflectionRay ray, float span, float roughness, uint exitReason)
{
    ReflectionHit hit = {};
    hit.reason = exitReason;
    float roughnessFraction = smoothstep(0.12, kMaxRoughness, roughness);
    uint budget = uint(ceil(float(settings.maxSteps) * lerp(1.0, 0.55, roughnessFraction)));
    // Reserve most visits for descent and final hit checks. A fixed 16-pixel
    // seed cannot cross a long, empty reflection at higher resolutions.
    uint spanMip = uint(ceil(log2(max(1.0, 4.0 * span / float(budget)))));
    uint maxMip = min(settings.history.z - 1u, max(spanMip, 4u + uint(2.0 * roughnessFraction)));
    // Start at the receiver's native pixels to avoid descending through the
    // whole pyramid just to leave the surface that emitted the ray.
    uint mip = 0u;
    float fraction = 1.5 / span;
    float parentExit = 0.0;
    bool leftReceiver = false;
    uint advancedCells = 0u;
    // Keep a separate bound on refinement and on leaving the biased emitter.
    // Neither should consume the interval budget for tracing the reflection.
    uint visitLimit = budget + 2u * maxMip;
    // A tiny ownership probe selects the next cell on a shared edge. The
    // actual interval still begins at the boundary for depth and hit tests.
    float fractionEpsilon = 0.001 / span;
    // The maximum rear tolerance is .024 + 40 * .0025 m, and the front
    // tolerance is one quarter of that. Move the respective ray endpoints
    // into those tolerances before rejecting a cell's full depth interval.
    float2 depthDirection = mul(camera.viewProj, float4(camera.cameraForward.xyz, 0.0)).zw;
    for (uint visit = 0u; visit < visitLimit && advancedCells < budget && fraction < 1.0; ++visit)
    {
        uint mipWidth = settings.extent.x;
        uint mipHeight = settings.extent.y;
        if (mip != 0u)
        {
            uint mipCount;
            depthIntervals.GetDimensions(mip - 1u, mipWidth, mipHeight, mipCount);
        }
        float2 mipSize = float2(mipWidth, mipHeight);
        float2 probe = ray.startPixel + ray.delta * min(fraction + fractionEpsilon, 1.0);
        int2 cell = int2(floor(probe * mipSize / float2(settings.extent)));
        if (any(cell < 0) || any(cell >= int2(mipWidth, mipHeight)))
            return hit;

        float2 boundary = (float2(cell) + step(0.0.xx, ray.delta)) * float2(settings.extent) / mipSize;
        float2 exitAt = float2(
            abs(ray.delta.x) > 1e-6 ? (boundary.x - ray.startPixel.x) / ray.delta.x : 1.0,
            abs(ray.delta.y) > 1e-6 ? (boundary.y - ray.startPixel.y) / ray.delta.y : 1.0);
        float exitFraction = min(1.0, min(exitAt.x, exitAt.y));
        if (exitFraction <= fraction)
        {
            fraction += fractionEpsilon;
            ++advancedCells;
            continue;
        }

        float2 depth;
        if (mip == 0u)
        {
            float leaf = sceneDepth.Load(int3(cell, 0));
            depth = isfinite(leaf) && leaf > 0.0 && leaf <= 1.0 ? leaf.xx : float2(0.0, 1.0);
        }
        else
            depth = depthIntervals.Load(int3(cell, mip - 1u));

        float farFraction = ray.depthSlope >= 0.0 ? exitFraction : fraction;
        float nearFraction = ray.depthSlope >= 0.0 ? fraction : exitFraction;
        float deepest = traceShiftedDepth(ray, farFraction, depthDirection * 0.031, 0.0);
        float closest = traceShiftedDepth(ray, nearFraction, depthDirection * -0.124, 1.0);
        if (depth.x > 0.0 && deepest <= depth.x + 1e-6 && closest >= depth.y - 1e-6)
        {
            if (mip > 0u)
            {
                parentExit = exitFraction;
                --mip;
                continue;
            }
            if (testReflectionHit(ray, cell, depth.x, (fraction + exitFraction) * 0.5,
                float2(fraction, exitFraction), true, hit))
                return hit;
        }
        else
            leftReceiver = true;
        fraction = exitFraction;
        if (leftReceiver)
        {
            ++advancedCells;
            // This ray interval was already selected for descent. Visit its
            // remaining child cells before testing a coarser interval again.
            if (fraction >= parentExit)
                mip = min(mip + 1u, maxMip);
        }
    }
    if (fraction < 1.0)
        hit.reason = kReflectionBudget;
    return hit;
}

ReflectionHit traceScreenReflection(float3 origin, float3 direction, float roughness)
{
    ReflectionHit hit = {};
    hit.reason = kReflectionInvalidRay;
    float4 start = mul(camera.viewProj, float4(origin, 1.0));
    float4 end = mul(camera.viewProj, float4(origin + direction * kMaxDistance, 1.0));
    if (!all(isfinite(start)) || !all(isfinite(end)) || start.w <= 1e-5)
        return hit;
    float clipFraction = min(reflectionClipLimit(start.z, end.z),
        reflectionClipLimit(start.w - start.z, end.w - end.z));
    clipFraction = min(clipFraction, reflectionClipLimit(start.w + start.x, end.w + end.x));
    clipFraction = min(clipFraction, reflectionClipLimit(start.w - start.x, end.w - end.x));
    clipFraction = min(clipFraction, reflectionClipLimit(start.w + start.y, end.w + end.y));
    clipFraction = min(clipFraction, reflectionClipLimit(start.w - start.y, end.w - end.y));
    end = lerp(start, end, clipFraction * 0.9999);
    if (end.w <= 1e-5)
        return hit;

    // Scene draws use a negative-height viewport. Convert both endpoints to
    // pixel coordinates before stepping along the dominant screen axis.
    float3 startNdc = start.xyz / start.w;
    float3 endNdc = end.xyz / end.w;
    float2 startPixel = temporalSceneNdcToUv(startNdc.xy) * settings.extent;
    float2 delta = temporalSceneNdcToUv(endNdc.xy) * settings.extent - startPixel;
    float span = max(abs(delta.x), abs(delta.y));
    if (!isfinite(span) || span < 2.0)
        return hit;

    ProjectedReflectionRay ray = {origin, direction, startPixel, delta, float2(start.w, end.w),
        float2(startNdc.z, endNdc.z), reflectionViewDepth(origin), dot(direction, camera.cameraForward.xyz),
        kMaxDistance * clipFraction * 0.9999};
    uint exitReason = clipFraction < 1.0 ? kReflectionScreenExit : kReflectionNoDepth;
    if (settings.history.z == 0u)
        return traceLinearReflection(ray, span, exitReason);
    return traceHiZReflection(ray, span, roughness, exitReason);
}

float3 roughReflectionColor(ReflectionHit hit, float roughness)
{
    float3 result = opaqueColor.Load(int3(hit.pixel, 0)).rgb;
    int radius = int(round(roughness * roughness * min(12.0, 2.0 + hit.distance)));
    if (radius == 0)
        return result;
    float3 center;
    if (!reflectionPosition(hit.pixel, center))
        return result;
    float centerDepth = reflectionViewDepth(center);
    float3 centerNormal = reflectionNormal(hit.pixel);
    uint centerMaterial = DecodeGBufferMaterialIndex(materialImage.Load(int3(hit.pixel, 0)).w);
    float weight = 1.0;
    const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    for (uint i = 0; i < 4; ++i)
    {
        int2 pixel = hit.pixel + offsets[i] * radius;
        if (any(pixel < 0) || any(pixel >= int2(settings.extent)))
            continue;
        float3 world;
        if (!reflectionPosition(pixel, world) ||
            abs(reflectionViewDepth(world) - centerDepth) > 0.03 + centerDepth * 0.01 ||
            dot(reflectionNormal(pixel), centerNormal) < 0.85 ||
            DecodeGBufferMaterialIndex(materialImage.Load(int3(pixel, 0)).w) != centerMaterial)
            continue;
        result += opaqueColor.Load(int3(pixel, 0)).rgb * 0.5;
        weight += 0.5;
    }
    return result / weight;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= settings.extent))
        return;
    int2 pixel = int2(id.xy);
    float4 base = opaqueColor.Load(int3(pixel, 0));
    float4 result = settings.debugView != 0 ? float4(0.0, 0.0, 0.0, base.a) : base;
    if (settings.debugView == 6u)
        result.rgb = reflectionReasonColor(kReflectionIneligible);
    uint4 stored = uint4(0);
    float3 world;
    float4 albedo = albedoImage.Load(int3(pixel, 0));
    float4 parameters = materialImage.Load(int3(pixel, 0));
    if (albedo.a > 0.0 && parameters.y < kMaxRoughness &&
        reflectionPosition(pixel, world))
    {
        float roughness = clamp(parameters.y, 0.045, 1.0);
        float metallic = saturate(parameters.x);
        uint materialIndex = DecodeGBufferMaterialIndex(parameters.w);
        GpuMaterial material = materials[materialIndex];
        DeferredMaterialLayerTerms layers = DecodeDeferredMaterialLayerTerms(material,
            materialLayers.Load(int3(pixel, 0)), materialSheen.Load(int3(pixel, 0)));
        ExtendedPbrTerms extended = LoadDeferredExtendedPbrTerms(material, pixel, layers.transmissionFactor);
        float3 normal = reflectionNormal(pixel);
        float3 view = SafeNormalize(camera.cameraWorldPosition.xyz - world, float3(0.0, 0.0, 1.0));
        bool orthographic = all(camera.viewProj[3].xyz == 0.0);
        float3 traceView = orthographic ? -camera.cameraForward.xyz : view;
        float3 direction = reflect(-traceView, ExtendedPbrIblNormal(normal, traceView, roughness, extended));
        direction = sampleRoughDirection(direction, roughness, id.xy);
        // Opaque/alpha-masked base lobes only. With aerial perspective the HDR
        // input contains distance-dependent eye-path in-scattering.
        if (layers.transmissionFactor <= 0.0 && lights.aerialGeometry.w <= 0.0 && dot(normal, direction) > 0.0)
        {
            ReflectionHit hit = traceScreenReflection(world + normal * 0.008, direction, roughness);
            if (settings.debugView == 5u || settings.debugView == 6u)
            {
                if (settings.debugView == 5u && hit.reason == kReflectionHit)
                    result.rgb = float3(reflectionPixelUv(hit.pixel), hit.distance);
                if (settings.debugView == 6u)
                    result.rgb = reflectionReasonColor(hit.reason);
                sceneColor[pixel] = result;
                return;
            }
            float roughnessWeight = 1.0 - smoothstep(0.30, kMaxRoughness, roughness);
            hit.confidence = roughnessWeight * saturate(hit.confidence);
            float fallbackConfidence = 0.0;
            float fallbackDistance = 0.0;
            uint fallbackMaterial = 0u;
#if SCREEN_REFLECTION_RAY_QUERY
            ReflectionRayHit rayHit = {};
            float remainingConfidence = roughnessWeight - hit.confidence;
            if (remainingConfidence > 0.0)
            {
                // The normal map steers the ray, but it can bend the direction
                // below the actual surface and cause a hit on the receiver.
                float3 geometricNormal = ReconstructDepthGeometricNormal(id.xy, settings.extent,
                    world, normal, sceneDepth, camera.inverseViewProj);
                if (dot(geometricNormal, direction) > 0.0)
                    rayHit = traceFallbackReflection(world, geometricNormal, direction, roughness);
            }
            fallbackConfidence = remainingConfidence * saturate(rayHit.confidence);
            fallbackDistance = rayHit.distance;
            fallbackMaterial = rayHit.materialIndex;
#endif
            float replacementConfidence = hit.confidence + fallbackConfidence;
            if (settings.debugView == 1)
                result.rgb = replacementConfidence.xxx;
            else if (settings.debugView == 2)
                result.rgb = ((fallbackConfidence > hit.confidence ? fallbackDistance : hit.distance) / kMaxDistance).xxx;
            else if (settings.debugView == 3)
                result.rgb = float3(0.0, hit.confidence, fallbackConfidence);
            else if (replacementConfidence > 0.0)
            {
                // Replace precisely the base specular IBL already in the HDR
                // input. Direct, diffuse, clearcoat, and sheen remain intact.
                float nDotV = max(dot(normal, view), 0.0);
                float3 dielectric = specularImage.Load(int3(pixel, 0)).rgb;
                float3 f0 = EvaluateDeferredPbrF0FromDielectricColor(albedo.rgb, metallic,
                    dielectric, layers.iridescenceFactor, material.iridescenceIor,
                    layers.iridescenceThickness, nDotV);
                float2 brdf = max(brdfLut.SampleLevel(brdfSampler, float2(nDotV, roughness), 0.0), 0.0.xx);
                float3 fallbackDirection = reflect(-view, ExtendedPbrIblNormal(normal, view, roughness, extended));
                float3 fallback = environmentSpecular.SampleLevel(environmentSampler,
                    RotateEnvironmentDirection(fallbackDirection, lights.environmentOrientation.xy),
                    roughness * max(float(lights.prefilteredMipCount) - 1.0, 0.0)).rgb *
                    max(lights.environmentIntensity, 0.0);
                float ao = saturate(FiniteScalarOr(parameters.z, 1.0));
                if (lights.gtaoEnabled != 0u)
                    ao *= saturate(FiniteScalarOr(
                        ambientOcclusion.SampleLevel(aoSampler, reflectionPixelUv(pixel), 0.0), 1.0));
                float scale = SpecularIblOcclusion(nDotV, roughness, ao) *
                    DeferredClearcoatBaseEnergyScale(layers.clearcoatFactor) *
                    DeferredSheenBaseEnergyScale(layers.sheenColor);
                float3 originalRadiance = max(Finite3Or(fallback, 0.0.xxx), 0.0.xxx);
                // Mix the two signed deltas independently of the amount of
                // the original lobe that this frame will replace.
                float rayShare = fallbackConfidence / replacementConfidence;
                float3 change = 0.0.xxx;
                if (hit.confidence > 0.0)
                    change += (1.0 - rayShare) * (max(Finite3Or(
                        roughReflectionColor(hit, roughness), 0.0.xxx), 0.0.xxx) - originalRadiance);
#if SCREEN_REFLECTION_RAY_QUERY
                if (fallbackConfidence > 0.0)
                    change += rayShare * (rayHit.radiance - originalRadiance);
#endif
                float3 correction = change * SpecularIblReflectance(f0, brdf) * scale;
                if (all(isfinite(correction)))
                {
                    bool fromRay = fallbackConfidence > hit.confidence;
                    uint hitMaterial = fromRay ? fallbackMaterial :
                        DecodeGBufferMaterialIndex(materialImage.Load(int3(hit.pixel, 0)).w);
                    uint hitKey = hitMaterial < 0x8000u ? hitMaterial | (fromRay ? 0x8000u : 0u) : 0x10000u;
                    bool accepted;
                    correction = filterReflection(pixel, world, normal, materialIndex, hitKey,
                        fromRay ? fallbackDistance : hit.distance, roughness,
                        replacementConfidence, correction, accepted, stored);
                    result.rgb = settings.debugView == 4u ? float(accepted).xxx :
                        clamp(base.rgb + correction * replacementConfidence, 0.0.xxx, 65504.0.xxx);
                }
            }
        }
    }
    if (settings.history.x != 0u)
        currentReflection[pixel] = stored;
    sceneColor[pixel] = result;
}
