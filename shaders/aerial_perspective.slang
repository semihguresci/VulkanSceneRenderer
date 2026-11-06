#pragma once
#include "lighting_structs.slang"
#include "atmosphere_scattering.slang"

struct AerialAtmosphereLut : IAtmosphereLut
{
    ConstantBuffer<LightingBuffer> light;

    float3 lookup(float2 position)
    {
        uint2 lo = uint2(position);
        uint2 hi = min(lo + 1, uint2(kAtmosphereLutWidth - 1, kAtmosphereLutHeight - 1));
        float2 weight = frac(position);
        uint row0 = lo.y * kAtmosphereLutWidth;
        uint row1 = hi.y * kAtmosphereLutWidth;
        float3 low = lerp(light.atmosphereLut[row0 + lo.x].xyz,
                          light.atmosphereLut[row0 + hi.x].xyz, weight.x);
        float3 high = lerp(light.atmosphereLut[row1 + lo.x].xyz,
                           light.atmosphereLut[row1 + hi.x].xyz, weight.x);
        return lerp(low, high, weight.y);
    }
};

float3 ApplyAerialPerspective(float3 surface, float3 worldPosition,
                             ConstantBuffer<CameraBuffer> camera,
                             ConstantBuffer<LightingBuffer> light)
{
    float scale = light.aerialGeometry.w;
    if (scale <= 0.0)
        return surface;

    float3 displacement = worldPosition - camera.cameraWorldPosition.xyz;
    bool orthographic = all(camera.viewProj[3].xyz == 0.0);
    float distance = orthographic ? dot(displacement, camera.cameraForward.xyz)
                                  : length(displacement);
    if (!isfinite(distance) || distance <= 1e-5)
        return surface;
    float3 direction = orthographic ? camera.cameraForward.xyz : displacement / distance;
    direction = RotateEnvironmentDirection(direction, light.environmentOrientation.xy);
    float radius = light.aerialGeometry.x;
    float top = radius + light.aerialGeometry.y;
    float3 origin = float3(0.0, radius + light.aerialGeometry.z, 0.0);
    float ground = GroundDistance(origin, direction, radius);
    float endOfAir = ground >= 0.0 ? ground : AtmosphereExit(origin, direction, top);
    float segment = min(distance * scale, endOfAir);
    if (segment <= 0.0)
        return surface;

    AerialAtmosphereLut lut = {light};
    float3 transmittance;
    float3 scattered = IntegrateAtmospherePath(origin, direction, segment,
        ground >= 0.0 && segment == ground, light.environmentSunDirection.xyz,
        radius, top, light.aerialDensity.xyz, 8, 6, light.environmentSunRadiance.w > 0.0, lut, transmittance);
    return surface * transmittance + scattered * light.aerialDensity.w;
}
