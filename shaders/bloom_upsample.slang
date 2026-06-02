// bloom_upsample.slang — Bloom progressive upsample with 9-tap tent filter.
// Uses a 3x3 tent filter for smooth upsampling, additively blends with the next higher mip.

#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<BloomUpsamplePushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float4> srcTexture;   // Lower-res mip being upsampled
[[vk::binding(1, 0)]] SamplerState linearSampler;
[[vk::binding(2, 0)]] Texture2D<float4> dstReadTexture; // Current mip to blend with
[[vk::binding(3, 0)]] RWTexture2D<float4> dstTexture;   // Output

float3 BloomFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float BloomFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float3 SampleBloomFinite(Texture2D<float4> tex, SamplerState samp, float2 uv)
{
    return max(BloomFinite3Or(tex.SampleLevel(samp, uv, 0.0).rgb, 0.0.xxx),
               0.0.xxx);
}

// 9-tap tent filter for upsampling (3x3 bilinear taps).
float3 UpsampleTent9(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texelSize, float radius)
{
    radius = max(BloomFiniteOr(radius, 1.0), 0.0);
    float2 d = texelSize * radius;

    float3 a = SampleBloomFinite(tex, samp, uv + float2(-d.x, -d.y));
    float3 b = SampleBloomFinite(tex, samp, uv + float2( 0.0, -d.y));
    float3 c = SampleBloomFinite(tex, samp, uv + float2( d.x, -d.y));

    float3 d0 = SampleBloomFinite(tex, samp, uv + float2(-d.x, 0.0));
    float3 e = SampleBloomFinite(tex, samp, uv);
    float3 f = SampleBloomFinite(tex, samp, uv + float2( d.x, 0.0));

    float3 g = SampleBloomFinite(tex, samp, uv + float2(-d.x,  d.y));
    float3 h = SampleBloomFinite(tex, samp, uv + float2( 0.0,  d.y));
    float3 i = SampleBloomFinite(tex, samp, uv + float2( d.x,  d.y));

    // Tent filter weights: 1 2 1 / 2 4 2 / 1 2 1 → sum = 16
    float3 result = e * 4.0;
    result += (b + d0 + f + h) * 2.0;
    result += (a + c + g + i);
    result *= (1.0 / 16.0);

    return result;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 srcResolution = uint2(pc.srcWidth, pc.srcHeight);
    uint2 dstResolution = uint2(pc.dstWidth, pc.dstHeight);

    if (dtid.x >= dstResolution.x || dtid.y >= dstResolution.y)
        return;

    float2 texelSize = 1.0 / float2(srcResolution);
    float2 uv = (float2(dtid.xy) + 0.5) / float2(dstResolution);

    float3 upsampled = UpsampleTent9(srcTexture, linearSampler, uv, texelSize, pc.filterRadius);

    // Additive blend with the destination mip.
    float3 existing = SampleBloomFinite(dstReadTexture, linearSampler, uv);
    float3 result = max(BloomFinite3Or(existing + upsampled, 0.0.xxx), 0.0.xxx);

    dstTexture[dtid.xy] = float4(result, 1.0);
}
