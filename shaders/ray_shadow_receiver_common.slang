#pragma once
#include "ray_shadow_data.slang"

bool RayShadowHasStandardMsaa(uint policy)
{
    uint samples = policy & 0xffu;
    return samples == 2u || samples == 4u || samples == 8u || samples == 16u;
}

float2 RayShadowStandardSampleLocation(uint samples, uint index)
{
    // Upper-left framebuffer origin and Vulkan's standard sample patterns:
    // https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-multisampling
    static const float2 samples2[2] = {
        float2(.75, .75), float2(.25, .25)};
    static const float2 samples4[4] = {
        float2(.375, .125), float2(.875, .375),
        float2(.125, .625), float2(.625, .875)};
    static const float2 samples8[8] = {
        float2(.5625, .3125), float2(.4375, .6875),
        float2(.8125, .5625), float2(.3125, .1875),
        float2(.1875, .8125), float2(.0625, .4375),
        float2(.6875, .9375), float2(.9375, .0625)};
    static const float2 samples16[16] = {
        float2(.5625, .5625), float2(.4375, .3125),
        float2(.3125, .625), float2(.75, .4375),
        float2(.1875, .375), float2(.625, .8125),
        float2(.8125, .6875), float2(.6875, .1875),
        float2(.375, .875), float2(.5, .0625),
        float2(.25, .125), float2(.125, .75),
        float2(0, .5), float2(.9375, .25),
        float2(.875, .9375), float2(.0625, 0)};
    if (samples == 2u) return samples2[index];
    if (samples == 4u) return samples4[index];
    if (samples == 8u) return samples8[index];
    return samples16[index];
}

float RayShadowReceiverDepthTolerance(float depth)
{
    return max(abs(depth) * 2.0e-6, 1.0e-12);
}

bool RayShadowResolvedSampleOffset(uint2 pixel, float depth, float4 neighbours,
                                   uint2 extent, uint policy, out float2 offset)
{
    offset = 0.5.xx;
    if (!RayShadowHasStandardMsaa(policy)) return false;
    uint samples = policy & 0xffu;
    if ((policy & 0x100u) != 0u)
    {
        offset = RayShadowStandardSampleLocation(samples, 0u);
        return true;
    }
    // On a planar receiver reverse-Z raster depth is affine in framebuffer XY.
    // The MAX resolve preserves its slope, allowing us to select its sample.
    // Both sides must agree numerically; edges/background do not prove a slope.
    if (any(pixel == 0u) || any(pixel + 1u >= extent) ||
        !all(isfinite(neighbours)) || any(neighbours <= 0.0.xxxx)) return false;
    float2 negativeSlope = depth.xx - neighbours.xz;
    float2 positiveSlope = neighbours.yw - depth.xx;
    float tolerance = RayShadowReceiverDepthTolerance(depth) * 2.0;
    if (any(abs(negativeSlope - positiveSlope) > tolerance.xx)) return false;
    float2 slope = (negativeSlope + positiveSlope) * 0.5;
    float best = -1e30;
    for (uint i = 0u; i < samples; ++i)
    {
        float2 sample = RayShadowStandardSampleLocation(samples, i);
        float projectedDepth = dot(sample - 0.5.xx, slope);
        if (projectedDepth > best)
        {
            best = projectedDepth;
            offset = sample;
        }
    }
    return true;
}

float3 RayShadowWorldAtSample(uint2 pixel, float2 offset, float depth,
                              CameraBuffer camera)
{
    float2 uv = (float2(pixel) + offset) * camera.renderExtent.zw;
    // Reverse-Z scene depth and the negative-height scene viewport.
    float4 world = mul(camera.inverseViewProj,
        float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return world.xyz / world.w;
}

float3 RayShadowReceiverWorld(uint2 pixel, float depth, Texture2D<float> depths,
                              CameraBuffer camera, uint policy,
                              out bool reliable, out float2 offset)
{
    if (!RayShadowHasStandardMsaa(policy) || (policy & 0x100u) != 0u)
    {
        reliable = RayShadowHasStandardMsaa(policy);
        offset = reliable ? RayShadowStandardSampleLocation(policy & 0xffu, 0u) : 0.5.xx;
        return RayShadowWorldAtSample(pixel, offset, depth, camera);
    }
    uint2 extent = uint2(camera.renderExtent.xy);
    int2 p = int2(pixel), end = int2(extent) - 1;
    float4 neighbours = float4(
        depths.Load(int3(clamp(p + int2(-1, 0), int2(0), end), 0)),
        depths.Load(int3(clamp(p + int2(1, 0), int2(0), end), 0)),
        depths.Load(int3(clamp(p + int2(0, -1), int2(0), end), 0)),
        depths.Load(int3(clamp(p + int2(0, 1), int2(0), end), 0)));
    reliable = RayShadowResolvedSampleOffset(pixel, depth, neighbours, extent, policy, offset);
    return RayShadowWorldAtSample(pixel, offset, depth, camera);
}

float3 RayShadowReceiverWorld(uint2 pixel, float depth, Texture2D<float4> surfaces,
                              CameraBuffer camera, uint policy, out bool reliable)
{
    if (!RayShadowHasStandardMsaa(policy) || (policy & 0x100u) != 0u)
    {
        reliable = RayShadowHasStandardMsaa(policy);
        float2 offset = reliable ? RayShadowStandardSampleLocation(policy & 0xffu, 0u) : 0.5.xx;
        return RayShadowWorldAtSample(pixel, offset, depth, camera);
    }
    uint2 extent = uint2(camera.renderExtent.xy);
    int2 p = int2(pixel), end = int2(extent) - 1;
    float4 neighbours = float4(
        surfaces.Load(int3(clamp(p + int2(-1, 0), int2(0), end), 0)).w,
        surfaces.Load(int3(clamp(p + int2(1, 0), int2(0), end), 0)).w,
        surfaces.Load(int3(clamp(p + int2(0, -1), int2(0), end), 0)).w,
        surfaces.Load(int3(clamp(p + int2(0, 1), int2(0), end), 0)).w);
    float2 offset;
    reliable = RayShadowResolvedSampleOffset(pixel, depth, neighbours, extent, policy, offset);
    return RayShadowWorldAtSample(pixel, offset, depth, camera);
}

float RayShadowReceiverPlaneTolerance(float3 world, CameraBuffer camera)
{
    float scale = max(max(max(abs(world.x), abs(world.y)), abs(world.z)),
                      length(world - camera.cameraWorldPosition.xyz));
    return max(1e-6, scale * 2e-6);
}
