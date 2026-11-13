#include "oit_common.slang"

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColor;
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> headPointerImage;
[[vk::binding(2, 0)]] StructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(3, 0)]] ConstantBuffer<OitMetadataBuffer> uOit;

// The host only dispatches when this frame has valid OIT lists.
struct NativeComposeOptions { uint oitEnabled; };
static const NativeComposeOptions pc = {1u};

float FiniteOr(float value, float fallback) { return isfinite(value) ? value : fallback; }
float3 FiniteColorOr(float3 value, float3 fallback) { return all(isfinite(value)) ? value : fallback; }
#include "oit_resolve_common.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (pixel.x >= uOit.viewportWidth || pixel.y >= uOit.viewportHeight ||
        headPointerImage.Load(int2(pixel)) == INVALID_NODE_INDEX)
        return;

    // One invocation owns this texel. Keep scene alpha, which carries volume
    // coverage when OpenVDB ran, and leave the lists available to diagnostics.
    float4 background = sceneColor[pixel];
    float3 color = ResolveOitPixel(pixel, background.rgb).compositedColor;
    // Conversion to the half-float target must not turn bright radiance into
    // infinity, which later display and capture shaders treat as invalid.
    sceneColor[pixel] = float4(clamp(color, -65504.0.xxx, 65504.0.xxx), background.a);
}
