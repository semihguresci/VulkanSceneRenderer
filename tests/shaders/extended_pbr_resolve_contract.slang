#include "pbr_extended_common.slang"

struct ResolvePushConstants
{
    float4 colorAndFactor;
};
[[vk::push_constant]] ConstantBuffer<ResolvePushConstants> pc;

struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(uint vertex : SV_VertexID)
{
    static const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    VertexOutput output;
    output.position = float4(positions[vertex], 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain() : SV_Target0
{
    ExtendedPbrTerms terms = MakeExtendedPbrTerms(float3(1, 0, 0), 0.0,
        pc.colorAndFactor.rgb, pc.colorAndFactor.w, 0.0);
    return EncodeExtendedPbrDiffuseStorage(terms);
}

struct ResolveResult
{
    float4 storage;
    float4 decodedColor;
    float4 reweighted;
};
[[vk::binding(0, 0)]] Texture2D<float4> resolvedStorage;
[[vk::binding(1, 0)]] RWStructuredBuffer<ResolveResult> results;

[shader("compute")]
[numthreads(32, 1, 1)]
void computeMain(uint3 dispatch : SV_DispatchThreadID)
{
    uint width, height;
    resolvedStorage.GetDimensions(width, height);
    if (dispatch.x >= width) return;
    float4 storage = resolvedStorage.Load(int3(dispatch.x, 0, 0));
    float3 color = DecodeExtendedPbrDiffuseStorageColor(storage);
    ExtendedPbrTerms terms = MakeExtendedPbrTerms(float3(1, 0, 0), 0.0,
                                                 color, storage.w, 0.0);
    ResolveResult output;
    output.storage = storage;
    output.decodedColor = float4(color, storage.w);
    output.reweighted = float4(color * storage.w, HasExtendedPbrEffect(terms) ? 1.0 : 0.0);
    results[dispatch.x] = output;
}
