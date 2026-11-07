#include "brdf_common.slang"
#include "lighting_structs.slang"
#include "oit_common.slang"

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
    float4 temporal;  // history blend, near plane, orthographic, transparent OIT active
};

[[vk::binding(0, 0)]] ConstantBuffer<FogFrameData> fog;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColor;
[[vk::binding(3, 0)]] StructuredBuffer<float4> previousFroxels;
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> currentFroxels;
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> cumulativeFroxels;
[[vk::binding(6, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> oitHeads;
[[vk::binding(7, 0)]] RWStructuredBuffer<TransparentNode> oitNodes;
[[vk::binding(8, 0)]] ConstantBuffer<OitMetadataBuffer> oitMetadata;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> lighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> pointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> areaLights;

// Reuse the same raster-atlas visibility as the sparse-volume route. These
// helpers avoid surface-normal bias and use a four-tap reverse-Z comparison.
#define camera fog.cameraData
#include "openvdb_geometry_shadow.slang"
#undef camera
#include "volume_area_light_common.slang"

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

float3 FogAreaRadiance(float3 position, float3 viewRay)
{
    float3 result = 0.0.xxx;
    // Keep the quadrature budget proportional to the punctual-light budget.
    uint count = min(lighting.areaLightCount,
        min(VOLUME_MAX_AREA_LIGHTS, fog.viewport.w / VOLUME_AREA_LIGHT_SAMPLES));
    [loop] for (uint index = 0u; index < count; ++index)
    {
        AreaLightData light = areaLights[index];
        if (!AreaLightHasRenderableRadiance(light) || !all(isfinite(light.positionRange)) ||
            !all(isfinite(light.directionType)) || !all(isfinite(light.tangentHalfSize)) ||
            !all(isfinite(light.bitangentHalfSize))) continue;
        AreaLightFrame frame = BuildAreaLightFrame(light);
        if (!isfinite(frame.area) || frame.area <= 0.0) continue;
        float solidAngle = 0.0;
        float phaseMass = 0.0;
        [unroll] for (uint sample = 0u; sample < VOLUME_AREA_LIGHT_SAMPLES; ++sample)
        {
            float3 samplePosition = VolumeAreaSamplePosition(frame, sample);
            float3 towardLight;
            float weight = AreaLightPositionContributionWeight(light, frame, position, samplePosition,
                1.0 / float(VOLUME_AREA_LIGHT_SAMPLES), towardLight);
            if (!isfinite(weight) || weight <= 0.0) continue;
            solidAngle += weight;
            phaseMass += weight * FogPhase(dot(towardLight, viewRay));
        }
        if (!isfinite(solidAngle) || solidAngle <= 0.0 || !isfinite(phaseMass)) continue;
        // A planar emitter subtends at most a hemisphere. Bound unresolved
        // near-field quadrature and the integral of the normalized phase.
        phaseMass = saturate(phaseMass * min(1.0, (2.0 * PI) / solidAngle));
        float3 radiance = max(light.colorIntensity.rgb, 0.0.xxx) * max(light.colorIntensity.a, 0.0);
        result += radiance * phaseMass * VolumeAreaGeometryVisibility(position, index, light, frame);
    }
    return result;
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
    radiance += FogAreaRadiance(position, viewRay);
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

float FogViewDepth(float2 uv, float depth)
{
    float3 position;
    if (depth > 0.0 && depth <= 1.0 && TryReconstructWorldPosition(uv, depth, fog.cameraData.inverseViewProj, position))
        return clamp(dot(position - fog.cameraData.cameraWorldPosition.xyz, fog.cameraData.cameraForward.xyz),
                     0.0, fog.medium.w);
    return fog.medium.w;
}

struct FogColumns
{
    uint2 lo;
    uint2 hi;
    float2 weights;
};

float3 FoggedColor(float3 source, float viewDepth, float distancePerViewDepth, FogColumns columns)
{
    float transmittance = exp(-fog.medium.x * viewDepth * distancePerViewDepth);
    if (fog.viewport.z == 3u) return transmittance.xxx;
    uint z = min(uint(FogSliceCoordinate(viewDepth)), fog.grid.z - 1u);
    float partialLength = max(viewDepth - FogSliceDepth(float(z)), 0.0) * distancePerViewDepth;
    uint2 lo = columns.lo, hi = columns.hi;
    float2 w = columns.weights;
    float3 a = lerp(PartialFog(lo, z, partialLength), PartialFog(uint2(hi.x, lo.y), z, partialLength), w.x);
    float3 b = lerp(PartialFog(uint2(lo.x, hi.y), z, partialLength), PartialFog(hi, z, partialLength), w.x);
    float3 scatter = lerp(a, b, w.y);
    return fog.viewport.z == 4u ? scatter : source * transmittance + scatter;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeFog(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= fog.viewport.xy)) return;
    float2 uv = (float2(id.xy) + 0.5.xx) / float2(fog.viewport.xy);
    FogRay ray;
    if (!MakeFogRay(uv, ray)) return;
    float2 coordinate = clamp(uv * float2(fog.grid.xy) - 0.5.xx, 0.0.xx, float2(fog.grid.xy - 1u));
    FogColumns columns;
    columns.lo = uint2(coordinate);
    columns.hi = min(columns.lo + 1u, fog.grid.xy - 1u);
    columns.weights = frac(coordinate);
    float4 source = sceneColor[id.xy];
    float viewDepth = FogViewDepth(uv, sceneDepth.Load(int3(id.xy, 0)));
    source.rgb = FoggedColor(source.rgb, viewDepth, ray.distancePerViewDepth, columns);
    sceneColor[id.xy] = source;

    if (fog.temporal.w == 0.0 || id.x >= oitMetadata.viewportWidth || id.y >= oitMetadata.viewportHeight)
        return;

    // Each node belongs to one pixel. Match the resolver's traversal limit
    // and preserve alpha so standard over blends fog to each visible depth.
    uint current = oitHeads[id.xy];
    [loop] for (uint i = 0u; i < MAX_OIT_NODES_PER_PIXEL && current < oitMetadata.nodeCapacity; ++i)
    {
        TransparentNode node = oitNodes[current];
        float nodeDepth = FogViewDepth(uv, node.depth);
        node.color.rgb = FoggedColor(node.color.rgb, nodeDepth, ray.distancePerViewDepth, columns);
        oitNodes[current].color = node.color;
        current = node.next;
    }
}
