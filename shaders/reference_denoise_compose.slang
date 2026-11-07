[[vk::binding(7, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> moments;
[[vk::binding(8, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> display;
[[vk::binding(2, 3)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> denoised;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    display.GetDimensions(width, height);
    if (any(tid.xy >= uint2(width, height)) || moments[tid.xy].w != 0.0) return;
    float3 value = denoised[tid.xy].rgb;
    if (all(isfinite(value))) display[tid.xy] = float4(max(value, 0.0.xxx), 1.0);
}
