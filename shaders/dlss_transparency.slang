[[vk::binding(0, 0)]] SamplerState layerSampler;
[[vk::binding(1, 0)]] Texture2D<float4> transparentLayer;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> displayColor;

struct LayerConstants
{
    uint2 displayExtent;
    float2 renderJitterUv;
};
[[vk::push_constant]] ConstantBuffer<LayerConstants> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.displayExtent))
        return;
    // DLSS produces an unjittered image. Sample the current premultiplied
    // raster layer at the corresponding point on the jittered render grid.
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.displayExtent) + pc.renderJitterUv;
    float4 layer = transparentLayer.SampleLevel(layerSampler, uv, 0.0);
    float3 background = displayColor[id.xy].rgb;
    displayColor[id.xy] = float4(clamp(layer.rgb + background * (1.0 - layer.a), 0.0.xxx, 65000.0.xxx), 1.0);
}
