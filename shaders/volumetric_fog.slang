#include "brdf_common.slang"
#include "lighting_structs.slang"

#if defined(FOG_INJECT)
#define injectFog computeMain
#elif defined(FOG_INTEGRATE)
#define integrateFog computeMain
#elif defined(FOG_COMPOSITE)
#define compositeFog computeMain
#endif

struct FogFrameData
{
    CameraBuffer cameraData;
    column_major float4x4 previousViewProj;
    float4 previousPosition;
    float4 previousForward;
    uint4 grid;       // x, y, z slice counts, frame ordinal
    uint4 viewport;   // width, height, display mode, local light limit
    float4 medium;    // extinction, albedo, HG g, view-space distance limit
    float4 temporal;  // history blend, near plane, orthographic, reserved
};

[[vk::binding(0, 0)]] ConstantBuffer<FogFrameData> fog;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColor;
[[vk::binding(3, 0)]] StructuredBuffer<float4> previousFroxels;
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> currentFroxels;
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> cumulativeFroxels;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> lighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> pointLights;

// Reuse the same raster-atlas visibility as the sparse-volume route. These
// helpers avoid surface-normal bias and use a four-tap reverse-Z comparison.
#define camera fog.cameraData
#include "openvdb_geometry_shadow.slang"
#undef camera

uint FroxelIndex(uint3 coordinate)
{
    return (coordinate.z * fog.grid.y + coordinate.y) * fog.grid.x + coordinate.x;
}

float FogSliceDepth(float z)
{
    float t = z / float(fog.grid.z);
    return fog.medium.w * t * t;
}

float FogSliceCoordinate(float depth)
{
    return sqrt(saturate(depth / fog.medium.w)) * float(fog.grid.z);
}

struct FogRay
{
    float3 origin;
    float3 direction;
    float distancePerViewDepth;
};

bool MakeFogRay(float2 uv, out FogRay ray)
{
    ray.origin = fog.cameraData.cameraWorldPosition.xyz;
    ray.direction = fog.cameraData.cameraForward.xyz;
    ray.distancePerViewDepth = 1.0;
    float3 nearPoint, middlePoint;
    if (!TryReconstructWorldPosition(uv, 1.0, fog.cameraData.inverseViewProj, nearPoint) ||
        !TryReconstructWorldPosition(uv, 0.5, fog.cameraData.inverseViewProj, middlePoint)) return false;
    ray.direction = SafeNormalize(middlePoint - nearPoint, ray.direction);
    float cosine = dot(ray.direction, fog.cameraData.cameraForward.xyz);
    if (!isfinite(cosine) || cosine < 1e-4) return false;
    ray.distancePerViewDepth = 1.0 / cosine;
    if (fog.temporal.z > 0.5)
        ray.origin = nearPoint - ray.direction * (fog.temporal.y * ray.distancePerViewDepth);
    return all(isfinite(ray.origin));
}

float FogPhase(float cosine)
{
    float g = fog.medium.z;
    float denominator = max(1.0 + g * g - 2.0 * g * clamp(cosine, -1.0, 1.0), 1e-4);
    return 0.07957747155 * (1.0 - g * g) * rsqrt(denominator) / denominator;
}

float3 IncidentFogRadiance(float3 position, float3 viewRay)
{
    float3 lightDirection = SafeNormalize(-lighting.directionalDirection.xyz, float3(0.0, 1.0, 0.0));
    float3 directional = max(lighting.directionalColorIntensity.rgb, 0.0.xxx) *
                         max(lighting.directionalColorIntensity.a, 0.0);
    float3 radiance = 0.0.xxx;
    if (any(directional > 0.0.xxx))
        radiance += directional * FogPhase(dot(lightDirection, viewRay)) * VolumeDirectionalGeometryVisibility(position);

    uint lightCount = min(lighting.pointLightCount, fog.viewport.w);
    for (uint index = 0u; index < lightCount; ++index)
    {
        PointLightData light = pointLights[index];
        if (!PointLightHasRenderableIntensity(light)) continue;
        float3 delta = light.positionRadius.xyz - position;
        float distanceSq = dot(delta, delta);
        if (!isfinite(distanceSq) || distanceSq <= 1e-8 ||
            (PointLightHasFiniteRange(light.positionRadius.w) && distanceSq >= light.positionRadius.w * light.positionRadius.w))
            continue;
        float3 towardLight = delta * rsqrt(distanceSq);
        float attenuation = PointLightAttenuation(distanceSq, light.positionRadius.w) * LocalLightAngularIntensity(light, towardLight);
        if (!(attenuation > 0.0)) continue;
        float3 energy = max(light.colorIntensity.rgb, 0.0.xxx) * light.colorIntensity.a;
        radiance += energy * attenuation * FogPhase(dot(towardLight, viewRay)) * VolumeLocalGeometryVisibility(position, index, light);
    }
    return all(isfinite(radiance)) ? radiance : 0.0.xxx;
}

float FogSampleOffset(uint3 coordinate)
{
    if (fog.temporal.x == 0.0) return 0.5;
    uint seed = FroxelIndex(coordinate) ^ (fog.grid.w * 0x9e3779b9u);
    seed = (seed ^ (seed >> 16u)) * 0x7feb352du;
    seed = (seed ^ (seed >> 15u)) * 0x846ca68bu;
    seed ^= seed >> 16u;
    return 0.2 + 0.6 * float(seed & 65535u) / 65535.0;
}

bool PreviousFogSample(float3 position, out float4 previous)
{
    previous = 0.0.xxxx;
    float4 clip = mul(fog.previousViewProj, float4(position, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return false;
    float2 ndc = clip.xy / clip.w;
    float depth = dot(position - fog.previousPosition.xyz, fog.previousForward.xyz);
    if (any(abs(ndc) >= 1.0.xx) || depth <= 0.0 || depth >= fog.medium.w) return false;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float3 coordinate = clamp(float3(uv * float2(fog.grid.xy), FogSliceCoordinate(depth)) - 0.5.xxx,
                              0.0.xxx, float3(fog.grid.xyz - 1u));
    uint3 lo = uint3(coordinate);
    uint3 hi = min(lo + 1u, fog.grid.xyz - 1u);
    float3 w = frac(coordinate);
    float4 row0 = lerp(previousFroxels[FroxelIndex(uint3(lo.x, lo.y, lo.z))],
                       previousFroxels[FroxelIndex(uint3(hi.x, lo.y, lo.z))], w.x);
    float4 row1 = lerp(previousFroxels[FroxelIndex(uint3(lo.x, hi.y, lo.z))],
                       previousFroxels[FroxelIndex(uint3(hi.x, hi.y, lo.z))], w.x);
    float4 row2 = lerp(previousFroxels[FroxelIndex(uint3(lo.x, lo.y, hi.z))],
                       previousFroxels[FroxelIndex(uint3(hi.x, lo.y, hi.z))], w.x);
    float4 row3 = lerp(previousFroxels[FroxelIndex(uint3(lo.x, hi.y, hi.z))],
                       previousFroxels[FroxelIndex(uint3(hi.x, hi.y, hi.z))], w.x);
    previous = lerp(lerp(row0, row1, w.y), lerp(row2, row3, w.y), w.z);
    return all(isfinite(previous));
}

[shader("compute")]
[numthreads(4, 4, 4)]
void injectFog(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= fog.grid.xyz)) return;
    FogRay ray;
    float4 cell = 0.0.xxxx;
    if (MakeFogRay((float2(id.xy) + 0.5.xx) / float2(fog.grid.xy), ray))
    {
        float depth = FogSliceDepth(float(id.z) + FogSampleOffset(id));
        float3 position = ray.origin + ray.direction * (depth * ray.distancePerViewDepth);
        float3 source = IncidentFogRadiance(position, ray.direction) * (fog.medium.x * fog.medium.y);
        float4 previous;
        if (fog.temporal.x > 0.0 && PreviousFogSample(position, previous))
        {
            // A lit history sample across a moving shadow edge is rejected
            // before it can linger. Small variations still average over time.
            float3 peak = max(max(source, previous.rgb), 0.003.xxx);
            float3 relative = abs(source - previous.rgb) / peak;
            float rejection = smoothstep(0.16, 0.55, max(relative.x, max(relative.y, relative.z)));
            source = lerp(source, previous.rgb, fog.temporal.x * (1.0 - rejection));
        }
        cell = float4(source, fog.medium.x);
    }
    currentFroxels[FroxelIndex(id)] = cell;
}

float4 AdvanceFog(float4 accumulated, float4 cell, float stepLength)
{
    float opticalDepth = max(cell.w * stepLength, 0.0);
    float stepTransmittance = exp(-opticalDepth);
    float integratedLength = opticalDepth < 0.001 ? stepLength * (1.0 - 0.5 * opticalDepth) :
        (1.0 - stepTransmittance) / cell.w;
    accumulated.rgb += accumulated.a * cell.rgb * integratedLength;
    accumulated.a *= stepTransmittance;
    return accumulated;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateFog(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= fog.grid.xy)) return;
    FogRay ray;
    if (!MakeFogRay((float2(id.xy) + 0.5.xx) / float2(fog.grid.xy), ray))
        ray.distancePerViewDepth = 1.0;
    float4 accumulated = float4(0.0, 0.0, 0.0, 1.0);
    for (uint z = 0; z < fog.grid.z; ++z)
    {
        uint index = FroxelIndex(uint3(id.xy, z));
        float length = (FogSliceDepth(float(z + 1u)) - FogSliceDepth(float(z))) * ray.distancePerViewDepth;
        accumulated = AdvanceFog(accumulated, currentFroxels[index], length);
        cumulativeFroxels[index] = accumulated;
    }
}

float3 PartialFog(uint2 coordinate, uint z, float stepLength)
{
    float4 accumulated = float4(0.0, 0.0, 0.0, 1.0);
    if (z > 0u) accumulated = cumulativeFroxels[FroxelIndex(uint3(coordinate, z - 1u))];
    return AdvanceFog(accumulated, currentFroxels[FroxelIndex(uint3(coordinate, z))], stepLength).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeFog(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= fog.viewport.xy)) return;
    float2 uv = (float2(id.xy) + 0.5.xx) / float2(fog.viewport.xy);
    FogRay ray;
    if (!MakeFogRay(uv, ray)) return;
    float depth = sceneDepth.Load(int3(id.xy, 0));
    float viewDepth = fog.medium.w;
    float3 position;
    if (depth > 0.0 && depth <= 1.0 && TryReconstructWorldPosition(uv, depth, fog.cameraData.inverseViewProj, position))
        viewDepth = clamp(dot(position - fog.cameraData.cameraWorldPosition.xyz, fog.cameraData.cameraForward.xyz),
                          0.0, fog.medium.w);
    uint z = min(uint(FogSliceCoordinate(viewDepth)), fog.grid.z - 1u);
    float partialLength = max(viewDepth - FogSliceDepth(float(z)), 0.0) * ray.distancePerViewDepth;
    float2 coordinate = clamp(uv * float2(fog.grid.xy) - 0.5.xx, 0.0.xx, float2(fog.grid.xy - 1u));
    uint2 lo = uint2(coordinate);
    uint2 hi = min(lo + 1u, fog.grid.xy - 1u);
    float2 w = frac(coordinate);
    float3 a = lerp(PartialFog(lo, z, partialLength), PartialFog(uint2(hi.x, lo.y), z, partialLength), w.x);
    float3 b = lerp(PartialFog(uint2(lo.x, hi.y), z, partialLength), PartialFog(hi, z, partialLength), w.x);
    float3 scatter = lerp(a, b, w.y);
    float transmittance = exp(-fog.medium.x * viewDepth * ray.distancePerViewDepth);
    float4 source = sceneColor[id.xy];
    if (fog.viewport.z == 3u) source.rgb = transmittance.xxx;
    else if (fog.viewport.z == 4u) source.rgb = scatter;
    else source.rgb = source.rgb * transmittance + scatter;
    sceneColor[id.xy] = source;
}
