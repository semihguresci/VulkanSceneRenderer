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

struct ScreenReflectionConstants
{
    uint2 extent;
    uint maxSteps;
    uint debugView;
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
            dot(reflectionNormal(pixel), centerNormal) < 0.85)
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
    float3 world;
    float4 albedo = albedoImage.Load(int3(pixel, 0));
    float4 parameters = materialImage.Load(int3(pixel, 0));
    if (albedo.a > 0.0 && parameters.y < kMaxRoughness &&
        reflectionPosition(pixel, world))
    {
        float roughness = clamp(parameters.y, 0.045, 1.0);
        float metallic = saturate(parameters.x);
        GpuMaterial material = materials[DecodeGBufferMaterialIndex(parameters.w)];
        DeferredMaterialLayerTerms layers = DecodeDeferredMaterialLayerTerms(material,
            materialLayers.Load(int3(pixel, 0)), materialSheen.Load(int3(pixel, 0)));
        ExtendedPbrTerms extended = LoadDeferredExtendedPbrTerms(material, pixel, layers.transmissionFactor);
        float3 normal = reflectionNormal(pixel);
        float3 view = SafeNormalize(camera.cameraWorldPosition.xyz - world, float3(0.0, 0.0, 1.0));
        bool orthographic = all(camera.viewProj[3].xyz == 0.0);
        float3 traceView = orthographic ? -camera.cameraForward.xyz : view;
        float3 direction = reflect(-traceView, ExtendedPbrIblNormal(normal, traceView, roughness, extended));
        // Opaque/alpha-masked base lobes only. With aerial perspective the HDR
        // input contains distance-dependent eye-path in-scattering.
        if (layers.transmissionFactor <= 0.0 && lights.aerialGeometry.w <= 0.0 && dot(normal, direction) > 0.0)
        {
            ReflectionHit hit = traceScreenReflection(world + normal * 0.008, direction);
            float roughnessWeight = 1.0 - smoothstep(0.30, kMaxRoughness, roughness);
            float fallbackConfidence = 0.0;
            float fallbackDistance = 0.0;
#if SCREEN_REFLECTION_RAY_QUERY
            ReflectionRayHit rayHit = {};
            if (hit.confidence < 1.0 && roughnessWeight > 0.0)
                rayHit = traceFallbackReflection(world, normal, direction, roughness);
            fallbackConfidence = (1.0 - hit.confidence) * rayHit.confidence * roughnessWeight;
            fallbackDistance = rayHit.distance;
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
#if SCREEN_REFLECTION_RAY_QUERY
                // Each route spends only its share of the original IBL.
                // The ray branch fills the complement of screen confidence.
                float3 change = 0.0.xxx;
                if (hit.confidence > 0.0)
                    change += hit.confidence * (EvaluateSpecularIblMultipleScattering(
                        roughReflectionColor(hit, roughness), f0, brdf) - originalIbl);
                if (fallbackConfidence > 0.0)
                    change += fallbackConfidence * (EvaluateSpecularIblMultipleScattering(
                        rayHit.radiance, f0, brdf) - originalIbl);
                if (all(isfinite(change)))
                    result.rgb = clamp(base.rgb + change * scale, 0.0.xxx, 65504.0.xxx);
#else
                float3 change = EvaluateSpecularIblMultipleScattering(roughReflectionColor(hit, roughness), f0, brdf) - originalIbl;
                if (all(isfinite(change)))
                    result.rgb = max(base.rgb + change * scale * hit.confidence, 0.0.xxx);
#endif
            }
        }
    }
    sceneColor[pixel] = result;
}
