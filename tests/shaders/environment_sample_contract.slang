[[vk::binding(0, 0)]] TextureCube<float4> radiance;
[[vk::binding(1, 0)]] SamplerState radianceSampler;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> output;
struct Queries { float4 directions[6]; };
[[vk::push_constant]] ConstantBuffer<Queries> queries;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 6) return;
    float4 query = queries.directions[id.x];
    output[id.x] = radiance.SampleLevel(radianceSampler, query.xyz, query.w);
}
