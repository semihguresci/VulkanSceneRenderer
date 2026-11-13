#include "oit_common.slang"

#ifndef DLSS_TRANSPARENCY
#define DLSS_TRANSPARENCY 0
#endif

[[vk::binding(0, 0)]] SamplerState colorSampler;
[[vk::binding(1, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(2, 0)]] Texture2D<float> reactiveTexture;
[[vk::binding(3, 0)]] RWTexture2D<float4> outputColor;
[[vk::binding(4, 0)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(5, 0)]] StructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(6, 0)]] StructuredBuffer<uint> nodeCounter;
[[vk::binding(7, 0)]] ConstantBuffer<OitMetadataBuffer> uOit;
#if DLSS_TRANSPARENCY
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> transparentLayer;
#endif

struct ComposeConstants { uint width; uint height; uint oitEnabled; uint reactiveFlags; };
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
    float4 sceneColor = sceneColorTexture.Load(int3(pixel, 0));
    float3 color = FiniteColorOr(sceneColor.rgb, 0.0.xxx);
    float reactive = saturate(FiniteOr(reactiveTexture.Load(int3(pixel, 0)), 1.0));
    if ((pc.reactiveFlags & 1u) != 0u) reactive = 1.0;
    // Alpha is volume coverage only when that producer has run this frame.
    // Ordinary material/sky alpha has no temporal meaning.
    if ((pc.reactiveFlags & 2u) != 0u)
        reactive = max(reactive, saturate(FiniteOr(sceneColor.a, 1.0)));
#if DLSS_TRANSPARENCY
    float4 layer = 0.0.xxxx;
#endif
    if (pc.oitEnabled != 0u)
    {
        OitResolveResult oit = ResolveOitPixel(pixel, color);
#if DLSS_TRANSPARENCY
        layer = float4(clamp(oit.transparentColor, 0.0.xxx, 65000.0.xxx), 1.0 - oit.revealage);
#else
        color = oit.compositedColor;
#endif
        // One velocity cannot describe multiple layers. Reject history for any
        // covered transparent pixel, including pool overflow/partial lists.
        if (headPointerImage.Load(int2(pixel)) != INVALID_NODE_INDEX ||
            nodeCounter[OIT_COUNTER_OVERFLOW_INDEX] != 0u)
            reactive = 1.0;
    }
#if DLSS_TRANSPARENCY
    transparentLayer[pixel] = layer;
#endif
    outputColor[pixel] = float4(clamp(color, 0.0.xxx, 65000.0.xxx), reactive);
}
