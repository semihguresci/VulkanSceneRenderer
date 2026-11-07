#pragma once
#include "depth_normal_common.slang"

float3 SampleEnvironmentBackground(float2 uv, float4x4 inverseViewProj,
                                   float intensity, float4 orientation,
                                   float4 background, uint mipCount,
                                   float4 sunDirection, float3 sunRadiance,
                                   TextureCube<float4> environment,
                                   SamplerState environmentSampler)
{
    if (background.w > 0.5)
        return background.rgb;
    float3 nearWorld;
    float3 farWorld;
    if (!TryReconstructWorldPosition(uv, 1.0, inverseViewProj, nearWorld) ||
        !TryReconstructWorldPosition(uv, 1e-4, inverseViewProj, farWorld))
        return 0.0.xxx;
    float3 direction = SafeNormalize(farWorld - nearWorld, float3(0, 0, -1));
    float mip = orientation.z * max(float(mipCount) - 1.0, 0.0);
    float3 localDirection = RotateEnvironmentDirection(direction, orientation.xy);
    float3 sky = environment.SampleLevel(environmentSampler, localDirection, mip).rgb;
    float scale = isfinite(intensity) ? max(intensity, 0.0) : 0.0;
    float3 radiance = sky * scale;
    if (dot(localDirection, sunDirection.xyz) >= sunDirection.w && any(sunRadiance > 0.0))
        radiance = min(radiance + sunRadiance * scale, 60000.0.xxx);
    return all(isfinite(radiance)) ? radiance : 0.0.xxx;
}
