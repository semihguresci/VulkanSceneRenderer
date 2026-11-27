[[vk::binding(7, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> moments;
[[vk::binding(8, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> display;
[[vk::binding(2, 3)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> denoised;

struct DenoiseComposition
{
    float spatialWeight;
};
[[vk::push_constant]] ConstantBuffer<DenoiseComposition> composition;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    display.GetDimensions(width, height);
    if (any(tid.xy >= uint2(width, height)) || moments[tid.xy].w != 0.0) return;
    float3 value = denoised[tid.xy].rgb;
    if (!all(isfinite(value))) return;
    value = max(value, 0.0.xxx);
    if (composition.spatialWeight < 1.0)
    {
        // Both inputs are scene-linear and use the current output framebuffer grid.
        const float3 reconstructed = display[tid.xy].rgb;
        value = (1.0 - composition.spatialWeight) * reconstructed + composition.spatialWeight * value;
    }
    if (all(isfinite(value))) display[tid.xy] = float4(value, 1.0);
}
