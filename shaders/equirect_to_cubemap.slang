// equirect_to_cubemap.slang — Compute shader converting an equirectangular
// HDR environment map to a cubemap face.  Dispatched once per face.

static const float PI = 3.14159265359;

#include "push_constants_common.slang"
#include "atmosphere_scattering.slang"

[[vk::push_constant]]
ConstantBuffer<EquirectPushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float4>       equirectMap;
[[vk::binding(1, 0)]] SamplerState            equirectSampler;
[[vk::binding(2, 0)]] RWTexture2DArray<float4> outCubemap;

struct SkyAtmosphereLut : IAtmosphereLut
{
    float3 lookup(float2 position)
    {
        float2 uv = (position + 0.5) / float2(kAtmosphereLutWidth, kAtmosphereLutHeight);
        return equirectMap.SampleLevel(equirectSampler, uv, 0.0).xyz;
    }
}

// Convert a cube face texel to a world-space direction.
float3 CubeTexelToDir(uint face, float2 uv)
{
    // Map uv from [0,1] to [-1,1]
    float2 st = uv * 2.0 - 1.0;

    float3 dir;
    switch (face)
    {
    case 0: dir = float3( 1.0, -st.y, -st.x); break; // +X
    case 1: dir = float3(-1.0, -st.y,  st.x); break; // -X
    case 2: dir = float3( st.x,  1.0,  st.y); break; // +Y
    case 3: dir = float3( st.x, -1.0, -st.y); break; // -Y
    case 4: dir = float3( st.x, -st.y,  1.0); break; // +Z
    case 5: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    default: dir = float3(0.0, 0.0, 1.0); break;
    }
    return normalize(dir);
}

float2 DirToEquirect(float3 dir)
{
    float phi   = atan2(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    float u = phi / (2.0 * PI) + 0.5;
    float v = theta / PI + 0.5;
    return float2(u, 1.0 - v);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.faceSize || dtid.y >= pc.faceSize) return;

    float2 uv = (float2(dtid.xy) + 0.5) / float(pc.faceSize);
    float3 dir = CubeTexelToDir(pc.faceIndex, uv);
    if (pc.skyMode != 0u)
    {
        float3 radiance = pc.horizon.rgb;
        if (pc.skyMode == 2u)
        {
            float elevation = asin(clamp(dot(dir, pc.atmosphereGeometry.xyz), -1.0, 1.0));
            float3 pole = elevation >= 0.0 ? pc.zenith.rgb : pc.ground.rgb;
            float curve = elevation >= 0.0 ? pc.zenith.w : pc.ground.w;
            float band = saturate(abs(elevation) * (2.0 / PI));
            float weight = curve == 1.0 ? band : 1.0 - pow(1.0 - band, 1.0 / curve);
            radiance = lerp(radiance, pole, weight);
        }
        if (pc.skyMode == 3u)
            radiance = EvaluateAtmosphere(dir, pc.ground.rgb, pc.sun,
                                          pc.atmosphereGeometry.xyz, pc.atmosphereDensity, SkyAtmosphereLut());
        outCubemap[uint3(dtid.xy, pc.faceIndex)] = float4(radiance, 1.0);
        return;
    }
    float2 equirectUV = DirToEquirect(dir);

    float4 color = equirectMap.SampleLevel(equirectSampler, equirectUV, 0.0);
    outCubemap[uint3(dtid.xy, pc.faceIndex)] = color;
}
