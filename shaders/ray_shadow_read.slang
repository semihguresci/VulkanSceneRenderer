#pragma once
#include "ray_shadow_data.slang"
#ifndef RAY_SHADOW_READ_SET
#define RAY_SHADOW_READ_SET 1
#endif
[[vk::binding(3, RAY_SHADOW_READ_SET)]] ConstantBuffer<RayShadowReadData> uRayShadowRead;
[[vk::binding(4, RAY_SHADOW_READ_SET)]] Texture2DArray<float> gRayShadowVisibility;

bool RayDirectionalSelected() { return uRayShadowRead.limits.x != 0u; }
bool RayPointSelected(uint index)
{
    return RayDirectionalSelected() && index < uRayShadowRead.limits.y;
}
bool RayAreaSelected(uint index)
{
    return RayDirectionalSelected() && index < uRayShadowRead.limits.z;
}
float RayShadowVisibility(float4 position, uint layer)
{
    uint width, height, layers;
    gRayShadowVisibility.GetDimensions(width, height, layers);
    uint2 pixel = min(uint2(position.xy), uint2(width - 1u, height - 1u));
    return saturate(gRayShadowVisibility.Load(int4(pixel, min(layer, layers - 1u), 0)));
}

float3 RayShadowDebug(float4 position)
{
    if (uRayShadowRead.options.y == 10u)
    {
        // Red: emitter samples / 32; green: selected local lights / 8;
        // blue: directional query. This shows the configured upper budget.
        return float3(float(uRayShadowRead.options.z != 0u ? uRayShadowRead.limits.w : 1u) / 32.0,
                      float(uRayShadowRead.limits.y + uRayShadowRead.limits.z) / 8.0, 1.0);
    }
    return RayShadowVisibility(position, uRayShadowRead.options.y - 1u).xxx;
}
