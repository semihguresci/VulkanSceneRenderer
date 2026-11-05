#include "material_data_common.slang"

[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[];
[[vk::binding(0, 1)]] RWStructuredBuffer<float4> uResults;

struct SamplingQuery { uint count; uint padding; float2 uv; };
[[vk::push_constant]] ConstantBuffer<SamplingQuery> query;

[numthreads(32, 1, 1)]
[shader("compute")]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= query.count) return;
    GpuTextureMetadata metadata = uTextureMetadata[tid.x];
    uResults[tid.x * 2] = materialTextures[NonUniformResourceIndex(tid.x)].SampleLevel(
        materialSamplers[NonUniformResourceIndex(metadata.samplerIndex)], query.uv, 0);
    uResults[tid.x * 2 + 1] = float4(metadata.samplerIndex, metadata.samplerCount, 0, 0);
}
