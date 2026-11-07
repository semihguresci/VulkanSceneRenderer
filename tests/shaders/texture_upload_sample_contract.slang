[[vk::binding(0, 0)]] Texture2D<float4> uImage;
[[vk::binding(1, 0)]] SamplerState uSampler;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> uSamples;

[numthreads(1, 1, 1)]
[shader("compute")]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint count, stride;
    uSamples.GetDimensions(count, stride);
    if (tid.x < count)
        uSamples[tid.x] = uImage.SampleLevel(uSampler, float2(0.5, 0.5), float(tid.x));
}
