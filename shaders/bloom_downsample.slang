// bloom_downsample.slang — Bloom brightness extraction + progressive downsample.
// Uses a 13-tap filter for high-quality downsampling (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare").

#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<BloomDownsamplePushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float4> srcTexture;
[[vk::binding(1, 0)]] SamplerState linearSampler;
[[vk::binding(2, 0)]] RWTexture2D<float4> dstTexture;

float3 BloomFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 SampleBloomFinite(Texture2D<float4> tex, SamplerState samp, float2 uv)
{
    return max(BloomFinite3Or(tex.SampleLevel(samp, uv, 0.0).rgb, 0.0.xxx),
               0.0.xxx);
}

// Soft knee threshold curve (Karis 2014).
float3 ThresholdFilter(float3 color, float threshold, float knee)
{
    color = max(BloomFinite3Or(color, 0.0.xxx), 0.0.xxx);
    float brightness = max(color.r, max(color.g, color.b));
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 1e-4);
    return color * max(contribution, 0.0);
}

// 13-tap downsample filter (box filter + cross filter for anti-flicker).
float3 DownsampleBox13(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texelSize)
{
    // Center
    float3 A = SampleBloomFinite(tex, samp, uv);

    // 4 corners at half-texel offset
    float3 B = SampleBloomFinite(tex, samp, uv + float2(-1.0, -1.0) * texelSize);
    float3 C = SampleBloomFinite(tex, samp, uv + float2( 1.0, -1.0) * texelSize);
    float3 D = SampleBloomFinite(tex, samp, uv + float2(-1.0,  1.0) * texelSize);
    float3 E = SampleBloomFinite(tex, samp, uv + float2( 1.0,  1.0) * texelSize);

    // 4 edges at full-texel offset
    float3 F = SampleBloomFinite(tex, samp, uv + float2(-2.0,  0.0) * texelSize);
    float3 G = SampleBloomFinite(tex, samp, uv + float2( 2.0,  0.0) * texelSize);
    float3 H = SampleBloomFinite(tex, samp, uv + float2( 0.0, -2.0) * texelSize);
    float3 I = SampleBloomFinite(tex, samp, uv + float2( 0.0,  2.0) * texelSize);

    // 4 corners at 2-texel offset
    float3 J = SampleBloomFinite(tex, samp, uv + float2(-2.0, -2.0) * texelSize);
    float3 K = SampleBloomFinite(tex, samp, uv + float2( 2.0, -2.0) * texelSize);
    float3 L = SampleBloomFinite(tex, samp, uv + float2(-2.0,  2.0) * texelSize);
    float3 M = SampleBloomFinite(tex, samp, uv + float2( 2.0,  2.0) * texelSize);

    // Weighted combination (anti-firefly weights from Jimenez):
    // Center box (A,B,C,D,E): weight 0.5
    // Edge boxes (F..I cross, J..M corners): weight 0.125 each
    float3 result = 0.0.xxx;
    result += A * 0.125;
    result += (B + C + D + E) * 0.125;
    result += (F + G + H + I) * 0.0625;
    result += (J + K + L + M) * 0.03125;
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

    float3 color = DownsampleBox13(srcTexture, linearSampler, uv, texelSize);

    // Apply brightness threshold only on the first mip (extract pass).
    if (pc.mipLevel == 0)
    {
        color = ThresholdFilter(color, pc.threshold, pc.knee);
    }

    color = max(BloomFinite3Or(color, 0.0.xxx), 0.0.xxx);
    dstTexture[dtid.xy] = float4(color, 1.0);
}
