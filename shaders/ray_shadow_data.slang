#pragma once
#include "lighting_structs.slang"
#include "push_constants_common.slang"

static const uint RAY_SHADOW_LAYERS = 9u;
struct RayShadowReadData
{
    uint4 limits; // enabled, first point count, first area count, area samples
    // w: standard MSAA sample count in low 8 bits; bit 8 selects SAMPLE_ZERO
    // depth resolve (otherwise MAX). Zero disables verified fragment reuse.
    uint4 options; // denoise, debug layer (1 based), soft mode, MSAA receiver policy
};
struct RayShadowTraceData
{
    CameraBuffer camera;
    RayShadowReadData read;
    column_major float4x4 previousViewProj;
    column_major float4x4 previousInverseViewProj;
    float4 sectionPlane;
    BoxClipState boxClip;
    uint4 frame; // history valid, sample frame, section enabled, unused
};

float3 RayShadowWorld(uint2 pixel, float depth, CameraBuffer camera)
{
    float2 uv = (float2(pixel) + 0.5) * camera.renderExtent.zw;
    // Scene depth uses reverse-Z and the negative-height viewport.
    float4 world = mul(camera.inverseViewProj,
        float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return world.xyz / world.w;
}
