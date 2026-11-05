#include "material_data_common.slang"

[[vk::binding(0, 0)]] StructuredBuffer<float4> uQueries;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> uResults;
[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[];

#include "pbr_material_common.slang"

[numthreads(32, 1, 1)]
[shader("compute")]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint count, stride;
    uQueries.GetDimensions(count, stride);
    if (tid.x >= count) return;
    float4 query = uQueries[tid.x];
    uResults[tid.x] = SamplePbrTextureLevel(uint(query.w), query.xy, query.z, 0.0.xxxx);
}
