// Bounded area-emitter quadrature and raster atlas visibility for volumes.
// Include after declaring lighting and including openvdb_geometry_shadow.slang.
#pragma once

#include "area_light_common.slang"

static const uint VOLUME_MAX_AREA_LIGHTS = 8u;
static const uint VOLUME_AREA_LIGHT_SAMPLES = 4u;

float3 VolumeAreaSamplePosition(AreaLightFrame frame, uint sample)
{
    float2 offset;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        // Centroids of four equal-area disk sectors, matching the four-domain
        // raster shadow origins. Each sample represents one quarter of dA.
        static const float2 signs[4] = {
            float2(1, 1), float2(-1, 1), float2(-1, -1), float2(1, -1)};
        offset = signs[sample] * 0.424413181578;
    }
    else
        offset = float2((sample & 1u) != 0u ? 0.5 : -0.5,
                         (sample & 2u) != 0u ? 0.5 : -0.5);
    return frame.position + frame.tangent * (offset.x * frame.halfSize.x) +
           frame.bitangent * (offset.y * frame.halfSize.y);
}

float VolumeAreaCubeVisibility(float3 world, uint areaIndex, uint cubeBase)
{
    uint maximumLayers = min(volumeLocalShadow.counts.x, MAX_SHADOWED_LOCAL_LIGHT_LAYERS);
    if (cubeBase >= maximumLayers || LOCAL_SHADOW_POINT_FACE_COUNT > maximumLayers - cubeBase)
        return 1.0;
    float3 origin = volumeLocalShadow.layers[cubeBase].positionRange.xyz;
    uint layerIndex = cubeBase + SelectPointLocalShadowFace(world - origin);
    LocalShadowLayerData layer = volumeLocalShadow.layers[layerIndex];
    if (layer.meta.w == 0u || layer.meta.x != areaIndex ||
        layer.directionType.w != LOCAL_SHADOW_TYPE_AREA) return 1.0;
    float distance = length(world - origin);
    float range = layer.positionRange.w;
    if (!isfinite(distance) || !isfinite(range) || range <= 1e-4 ||
        distance <= 1e-4 || distance >= range) return 1.0;
    float4 clip = mul(layer.viewProj, float4(world, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return 1.0;
    float3 projected = clip.xyz / clip.w;
    if (!all(isfinite(projected)) || projected.z < 0.0 || projected.z > 1.0) return 1.0;
    // No surface normal exists at a scattering sample. Match the point/spot
    // medium path's constant bias and raw four-tap reverse-Z PCF.
    float bias = max(LocalShadowFiniteOr(volumeLocalShadow.filterSettings.z, 0.0), 0.0);
    float visibility = VolumeShadowBilinearPcf(volumeLocalShadowAtlas,
        projected.xy * 0.5 + 0.5, layerIndex, projected.z + bias);
    float fade = saturate((range - distance) / max(range * 0.05, 1e-4));
    return lerp(1.0, visibility, fade);
}

float VolumeAreaGeometryVisibility(float3 world, uint areaIndex,
                                    AreaLightData light, AreaLightFrame frame)
{
    if (!LocalShadowMapsEnabled(lighting, volumeLocalShadow)) return 1.0;
    uint encodedLayer = PackedLocalShadowAreaRef(volumeLocalShadow, areaIndex);
    uint sampleCount = LocalAreaShadowSampleCount(encodedLayer, volumeLocalShadow);
    if (sampleCount == 0u) return 1.0;
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    // The surface renderer's atlas budget supplies one to four cube origins.
    // Average their geometry visibility independently of the four radiance
    // samples. This is a bounded soft-shadow approximation, not LTC/PCSS.
    [loop] for (uint sample = 0u; sample < sampleCount; ++sample)
    {
        float3 position = LocalAreaShadowSamplePosition(encodedLayer, sample, volumeLocalShadow);
        float3 toLight;
        float weight = AreaLightPositionContributionWeight(light, frame, world, position,
            1.0 / float(sampleCount), toLight);
        if (!isfinite(weight) || weight <= 0.0) continue;
        uint cubeBase = encodedLayer - 1u + sample * LOCAL_SHADOW_POINT_FACE_COUNT;
        weightedVisibility += weight * VolumeAreaCubeVisibility(world, areaIndex, cubeBase);
        totalWeight += weight;
    }
    return totalWeight > 0.0 ? saturate(weightedVisibility / totalWeight) : 1.0;
}
