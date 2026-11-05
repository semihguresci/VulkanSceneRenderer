[[vk::binding(0, 0)]] TextureCube<float4> radiance;
[[vk::binding(1, 0)]] SamplerState radianceSampler;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 6) return;
    static const float3 directions[6] = {
        float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
        float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)
    };
    output[id.x] = radiance.SampleLevel(radianceSampler, directions[id.x], 0);
}
