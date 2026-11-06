// Opaque scene geometry shadowing for volume single scattering. The host binds
// the existing raster atlases, so this also works without ray-query support.
// Include after the caller declares its camera and lighting constant buffers.
#pragma once

#include "shadow_common.slang"
#include "local_shadow_common.slang"

[[vk::binding(10, 0)]] ConstantBuffer<ShadowBuffer> volumeShadow;
[[vk::binding(11, 0)]] Texture2DArray<float> volumeShadowAtlas;
[[vk::binding(12, 0)]] ConstantBuffer<LocalShadowBuffer> volumeLocalShadow;
[[vk::binding(13, 0)]] Texture2DArray<float> volumeLocalShadowAtlas;

float VolumeShadowCompareTap(Texture2DArray<float> atlas, int2 texel,
                             uint layer, uint2 extent, float compareDepth)
{
    // Atlas borders are reverse-Z far/lit, matching ShadowManager's sampler.
    if (any(texel < int2(0, 0)) || any(texel >= int2(extent))) return 1.0;
    float storedDepth = atlas.Load(int4(texel, int(layer), 0));
    return !isfinite(storedDepth) || compareDepth > storedDepth ? 1.0 : 0.0;
}

float VolumeShadowBilinearPcf(Texture2DArray<float> atlas, float2 uv,
                              uint layer, float compareDepth)
{
    if (!all(isfinite(uv)) || !isfinite(compareDepth) ||
        any(uv < 0.0.xx) || any(uv > 1.0.xx)) return 1.0;
    uint width, height, layers;
    atlas.GetDimensions(width, height, layers);
    if (width == 0u || height == 0u || layer >= layers) return 1.0;
    float2 pixel = uv * float2(width, height) - 0.5.xx;
    int2 base = int2(floor(pixel));
    float2 weight = frac(pixel);
    float depth = saturate(compareDepth);
    uint2 extent = uint2(width, height);
    float a = VolumeShadowCompareTap(atlas, base, layer, extent, depth);
    float b = VolumeShadowCompareTap(atlas, base + int2(1, 0), layer, extent, depth);
    float c = VolumeShadowCompareTap(atlas, base + int2(0, 1), layer, extent, depth);
    float d = VolumeShadowCompareTap(atlas, base + int2(1, 1), layer, extent, depth);
    return lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y);
}

float VolumeDirectionalCascadeVisibility(float3 world, uint cascade)
{
    float3 projected;
    if (!ProjectShadowCascade(world, cascade, volumeShadow, projected) ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    // A medium has no surface normal: normal/slope offsets would move the
    // scattering location. Only the map's small constant depth bias applies.
    float bias = max(ShadowFiniteOr(volumeShadow.filterSettings.z, 0.0), 0.0);
    return VolumeShadowBilinearPcf(volumeShadowAtlas, ShadowNdcToUv(projected.xy),
                                   cascade, projected.z + bias);
}

float VolumeDirectionalGeometryVisibility(float3 world)
{
    if (lighting.shadowEnabled == 0u || !all(isfinite(world))) return 1.0;
    float3 forward = SafeNormalize(camera.cameraForward.xyz, float3(0.0, 0.0, -1.0));
    float viewDepth = dot(world - camera.cameraWorldPosition.xyz, forward);
    if (!isfinite(viewDepth) || viewDepth <= 0.0) return 1.0;
    uint cascade = SelectCascade(viewDepth, volumeShadow);
    float visibility = VolumeDirectionalCascadeVisibility(world, cascade);
    if (cascade + 1u < SHADOW_CASCADE_COUNT)
    {
        float split = ShadowFiniteOr(volumeShadow.cascades[cascade].splitDepth, 0.0);
        float previousSplit = cascade == 0u ? 0.0 :
            ShadowFiniteOr(volumeShadow.cascades[cascade - 1u].splitDepth, 0.0);
        float blendZone = (split - previousSplit) *
            clamp(ShadowFiniteOr(volumeShadow.filterSettings.y, 0.0), 0.0, 0.45);
        if (blendZone > 1e-6 && viewDepth > split - blendZone)
            visibility = lerp(visibility,
                VolumeDirectionalCascadeVisibility(world, cascade + 1u),
                saturate((viewDepth - (split - blendZone)) / blendZone));
    }
    return visibility;
}

float VolumeLocalGeometryVisibility(float3 world, uint lightIndex, PointLightData light)
{
    if (!LocalShadowMapsEnabled(lighting, volumeLocalShadow) ||
        !all(isfinite(world)) ||
        !PointLightHasLocalShadowLayers(light, volumeLocalShadow)) return 1.0;
    uint baseLayer = LocalShadowBaseLayerFromPointLight(light);
    uint layerCount = LocalShadowLayerCountFromPointLight(light);
    bool spot = light.coneOuterCosType.y >= 0.5;
    if (!spot && layerCount < LOCAL_SHADOW_POINT_FACE_COUNT) return 1.0;
    uint layerIndex = baseLayer + (spot ? 0u :
        SelectPointLocalShadowFace(world - light.positionRadius.xyz));
    if (layerIndex >= baseLayer + layerCount) return 1.0;
    LocalShadowLayerData layer = volumeLocalShadow.layers[layerIndex];
    if (layer.meta.w == 0u || layer.meta.x != lightIndex) return 1.0;
    float distance = length(world - layer.positionRange.xyz);
    float range = layer.positionRange.w;
    if (!isfinite(distance) || !isfinite(range) || range <= 1e-4 ||
        distance <= 1e-4 || distance >= range) return 1.0;
    float4 clip = mul(layer.viewProj, float4(world, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return 1.0;
    float3 projected = clip.xyz / clip.w;
    if (!all(isfinite(projected)) || projected.z < 0.0 || projected.z > 1.0) return 1.0;
    float bias = max(LocalShadowFiniteOr(volumeLocalShadow.filterSettings.z, 0.0), 0.0);
    float visibility = VolumeShadowBilinearPcf(volumeLocalShadowAtlas,
        projected.xy * 0.5 + 0.5, layerIndex, projected.z + bias);
    // Match surface shadow fades at finite-range atlas boundaries.
    float rangeVisibility = saturate((range - distance) / max(range * 0.05, 1e-4));
    return lerp(1.0, visibility, rangeVisibility);
}
