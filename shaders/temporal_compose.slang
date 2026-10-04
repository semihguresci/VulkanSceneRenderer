#include "oit_common.slang"

[[vk::binding(0, 0)]] SamplerState colorSampler;
[[vk::binding(1, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(2, 0)]] Texture2D<float> reactiveTexture;
[[vk::binding(3, 0)]] RWTexture2D<float4> outputColor;
[[vk::binding(4, 0)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(5, 0)]] StructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(6, 0)]] StructuredBuffer<uint> nodeCounter;
[[vk::binding(7, 0)]] ConstantBuffer<OitMetadataBuffer> uOit;

struct ComposeConstants { uint width; uint height; uint oitEnabled; uint forceReactive; };
[[vk::push_constant]] ConstantBuffer<ComposeConstants> pc;

float FiniteOr(float value, float fallback) { return isfinite(value) ? value : fallback; }
float3 FiniteColorOr(float3 value, float3 fallback) { return all(isfinite(value)) ? value : fallback; }
#include "oit_resolve_common.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height) return;
    uint2 pixel = id.xy;
    float3 color = FiniteColorOr(sceneColorTexture.Load(int3(pixel, 0)).rgb, 0.0.xxx);
    float reactive = saturate(FiniteOr(reactiveTexture.Load(int3(pixel, 0)), 1.0));
    if (pc.forceReactive != 0u) reactive = 1.0;
    if (pc.oitEnabled != 0u)
    {
        OitResolveResult oit = ResolveOitPixel(pixel, color);
        color = oit.compositedColor;
        // One velocity cannot describe multiple layers. Reject history for any
        // covered transparent pixel, including pool overflow/partial lists.
        if (headPointerImage.Load(int2(pixel)) != INVALID_NODE_INDEX ||
            nodeCounter[0] >= uOit.nodeCapacity)
            reactive = 1.0;
    }
    outputColor[pixel] = float4(clamp(color, 0.0.xxx, 65000.0.xxx), reactive);
}
