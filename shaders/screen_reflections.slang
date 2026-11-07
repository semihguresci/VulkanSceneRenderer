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

struct ScreenReflectionConstants
{
    uint2 extent;
    uint maxSteps;
    uint debugView;
    uint4 history; // save, reproject, reserved, reserved
#if SCREEN_REFLECTION_RAY_QUERY
    uint4 rayLimits;
    float4 sectionPlane;
#endif
};
[[vk::push_constant]] ConstantBuffer<ScreenReflectionConstants> settings;

static const float kMaxDistance = 40.0;
static const float kMaxRoughness = 0.55;

#if SCREEN_REFLECTION_RAY_QUERY
#include "screen_reflections_ray_common.slang"
#endif

struct ReflectionHit
{
    int2 pixel;
    float distance;
    float confidence;
};

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

ReflectionHit traceScreenReflection(float3 origin, float3 direction)
{
    ReflectionHit hit = {};
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
    float2 startPixel = temporalSceneNdcToUv(start.xy / start.w) * settings.extent;
    float2 endPixel = temporalSceneNdcToUv(end.xy / end.w) * settings.extent;
    float2 delta = endPixel - startPixel;
    float span = max(abs(delta.x), abs(delta.y));
    if (span < 2.0)
        return hit;

    float limit = kMaxDistance * clipFraction * 0.9999;
    float startDepth = reflectionViewDepth(origin);
    float depthSlope = dot(direction, camera.cameraForward.xyz);
    // A full pixel stride catches narrow geometry at close range. On longer
    // projections the fixed ray budget caps the total texture fetch count.
    float stride = max(1.0, span / float(settings.maxSteps));
    for (uint step = 0; step < settings.maxSteps; ++step)
    {
        float at = 1.5 + float(step) * stride;
        if (at > span)
            break;
        float fraction = at / span;
        int2 pixel = int2(floor(lerp(startPixel, endPixel, fraction)));
        if (any(pixel < 0) || any(pixel >= int2(settings.extent)))
            break;
        float3 surface;
        if (!reflectionPosition(pixel, surface))
            continue;
        float t = reflectionRayDistance(fraction, start.w, end.w, limit);
        float surfaceDepth = reflectionViewDepth(surface);
        float thickness = 0.024 + min(surfaceDepth, 40.0) * 0.0025;
        // Check the continuous intersection with the sampled pixel's depth
        // plane. Point tests alone miss near geometry between ray samples.
        if (abs(depthSlope) > 1e-4)
        {
            float intersection = (surfaceDepth - startDepth) / depthSlope;
            if (intersection > thickness && intersection < limit)
            {
                float4 projected = mul(camera.viewProj, float4(origin + direction * intersection, 1.0));
                float2 projectedPixel = temporalSceneNdcToUv(projected.xy / projected.w) * settings.extent;
                if (all(abs(projectedPixel - (float2(pixel) + 0.5)) < 0.85))
                    t = intersection;
            }
        }
        float offset = startDepth + t * depthSlope - surfaceDepth;
        if (offset < -thickness * 0.25 || offset > thickness || t <= thickness)
            continue;
        if (dot(reflectionNormal(pixel), direction) >= -0.015)
            continue;

        float2 uv = reflectionPixelUv(pixel);
        float2 edge = min(uv, 1.0 - uv);
        hit.pixel = pixel;
        hit.distance = t;
        hit.confidence = smoothstep(0.0, 0.045, min(edge.x, edge.y)) *
            (1.0 - smoothstep(0.70, 1.0, t / kMaxDistance));
        return hit;
    }
    return hit;
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
            ReflectionHit hit = traceScreenReflection(world + normal * 0.008, direction);
            float roughnessWeight = 1.0 - smoothstep(0.30, kMaxRoughness, roughness);
            float fallbackConfidence = 0.0;
            float fallbackDistance = 0.0;
            uint fallbackMaterial = 0u;
#if SCREEN_REFLECTION_RAY_QUERY
            ReflectionRayHit rayHit = {};
            if (hit.confidence < 1.0 && roughnessWeight > 0.0)
                rayHit = traceFallbackReflection(world, normal, direction, roughness);
            fallbackConfidence = (1.0 - hit.confidence) * rayHit.confidence * roughnessWeight;
            fallbackDistance = rayHit.distance;
            fallbackMaterial = rayHit.materialIndex;
#endif
            hit.confidence *= roughnessWeight;
            if (settings.debugView == 1)
                result.rgb = (hit.confidence + fallbackConfidence).xxx;
            else if (settings.debugView == 2)
                result.rgb = ((fallbackConfidence > hit.confidence ? fallbackDistance : hit.distance) / kMaxDistance).xxx;
            else if (settings.debugView == 3)
                result.rgb = float3(0.0, hit.confidence, fallbackConfidence);
            else if (hit.confidence + fallbackConfidence > 0.0)
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
                float ao = saturate(parameters.z);
                if (lights.gtaoEnabled != 0u)
                    ao *= saturate(ambientOcclusion.SampleLevel(aoSampler, reflectionPixelUv(pixel), 0.0));
                float specularOcclusion = saturate(pow(nDotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
                float scale = lerp(1.0, specularOcclusion, 1.0 - roughness) *
                    DeferredClearcoatBaseEnergyScale(layers.clearcoatFactor) *
                    DeferredSheenBaseEnergyScale(layers.sheenColor);
                float3 originalIbl = EvaluateSpecularIblMultipleScattering(fallback, f0, brdf);
                // Each route spends only its share of the original IBL.
                // The ray branch fills the complement of screen confidence.
                float3 change = 0.0.xxx;
                if (hit.confidence > 0.0)
                    change += hit.confidence * (EvaluateSpecularIblMultipleScattering(
                        roughReflectionColor(hit, roughness), f0, brdf) - originalIbl);
#if SCREEN_REFLECTION_RAY_QUERY
                if (fallbackConfidence > 0.0)
                    change += fallbackConfidence * (EvaluateSpecularIblMultipleScattering(
                        rayHit.radiance, f0, brdf) - originalIbl);
#endif
                float3 correction = change * scale;
                if (all(isfinite(correction)))
                {
                    bool fromRay = fallbackConfidence > hit.confidence;
                    uint hitMaterial = fromRay ? fallbackMaterial :
                        DecodeGBufferMaterialIndex(materialImage.Load(int3(hit.pixel, 0)).w);
                    uint hitKey = hitMaterial < 0x8000u ? hitMaterial | (fromRay ? 0x8000u : 0u) : 0x10000u;
                    bool accepted;
                    correction = filterReflection(pixel, world, normal, materialIndex, hitKey,
                        fromRay ? fallbackDistance : hit.distance, roughness,
                        hit.confidence + fallbackConfidence, correction, accepted, stored);
                    result.rgb = settings.debugView == 4u ? float(accepted).xxx :
                        clamp(base.rgb + correction, 0.0.xxx, 65504.0.xxx);
                }
            }
        }
    }
    if (settings.history.x != 0u)
        currentReflection[pixel] = stored;
    sceneColor[pixel] = result;
}
