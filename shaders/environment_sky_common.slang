#pragma once
#include "depth_normal_common.slang"

float3 SampleEnvironmentBackground(float2 uv, float4x4 inverseViewProj,
                                   float intensity, TextureCube<float4> environment,
                                   SamplerState environmentSampler)
{
    float3 nearWorld;
    float3 farWorld;
    if (!TryReconstructWorldPosition(uv, 1.0, inverseViewProj, nearWorld) ||
        !TryReconstructWorldPosition(uv, 1e-4, inverseViewProj, farWorld))
        return 0.0.xxx;
    float3 direction = SafeNormalize(farWorld - nearWorld, float3(0, 0, -1));
    float3 sky = environment.SampleLevel(environmentSampler, direction, 0.0).rgb;
    float scale = isfinite(intensity) ? max(intensity, 0.0) : 0.0;
    float3 radiance = sky * scale;
    return all(isfinite(radiance)) ? radiance : 0.0.xxx;
}
