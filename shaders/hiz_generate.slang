// hiz_generate.slang — Compute shader for hierarchical depth buffer generation.
// Reverse-Z culling reduces to the farthest depth. The reflection variant
// tracks the nearest and farthest visible surfaces, with no stored mip zero.

#include "push_constants_common.slang"

#ifndef HIZ_REFLECTION_NEAREST
#define HIZ_REFLECTION_NEAREST 0
#endif

[[vk::push_constant]]
ConstantBuffer<HiZPushConstants> pc;

#if HIZ_REFLECTION_NEAREST
[[vk::binding(0, 0)]] Texture2D<float2> gSrcDepth;
[[vk::binding(2, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> gDstMip;

float2 LoadSourceDepth(uint2 coord)
{
    float2 depth = gSrcDepth.Load(int3(coord, 0));
    // Virtual mip one reads the single-channel scene depth. Zero is sky.
    if (pc.dstMipLevel == 1u)
        return (isfinite(depth.x) && depth.x > 0.0 && depth.x <= 1.0) ? depth.xx : float2(0.0, 1.0);
    return depth;
}
#else
[[vk::binding(0, 0)]] Texture2D<float>    gSrcDepth;
[[vk::binding(1, 0)]] SamplerState        gSrcSampler;
[[vk::binding(2, 0)]] RWTexture2D<float>  gDstMip;

float LoadSourceDepth(uint2 coord)
{
    float depth = gSrcDepth.Load(int3(coord, 0));
    return (isfinite(depth) && depth >= 0.0 && depth <= 1.0) ? depth : 0.0;
}
#endif

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 dstCoord = dtid.xy;
    uint2 dstExtent;
    gDstMip.GetDimensions(dstExtent.x, dstExtent.y);
    if (any(dstCoord >= dstExtent))
        return;

    uint2 srcExtent;
    gSrcDepth.GetDimensions(srcExtent.x, srcExtent.y);
    #if HIZ_REFLECTION_NEAREST
    uint2 expectedDstExtent = max(srcExtent >> 1u, 1u);
    #else
    bool copyDepth = pc.dstMipLevel == 0;
    uint2 expectedDstExtent = copyDepth ? srcExtent : max(srcExtent >> 1u, 1u);
    #endif
    if (any(srcExtent != uint2(pc.srcWidth, pc.srcHeight)) || any(dstExtent != expectedDstExtent))
    {
        // A mismatched view cannot bound the depth of any surface.
        #if HIZ_REFLECTION_NEAREST
        gDstMip[dstCoord] = float2(1.0, 0.0);
        #else
        gDstMip[dstCoord] = 0.0;
        #endif
        return;
    }

    #if !HIZ_REFLECTION_NEAREST
    if (copyDepth)
    {
        gDstMip[dstCoord] = LoadSourceDepth(dstCoord);
        return;
    }
    #endif

    // Include every source texel overlapping this destination texel in
    // normalized coordinates. Odd source extents straddle a third texel.
    uint2 srcBegin = dstCoord * 2u;
    uint2 srcEnd = min(srcBegin + 2u + (srcExtent & 1u), srcExtent);
    #if HIZ_REFLECTION_NEAREST
    float2 reducedDepth = float2(0.0, 1.0);
    #else
    float reducedDepth = 1.0;
    #endif
    for (uint y = srcBegin.y; y < srcEnd.y; ++y)
    {
        for (uint x = srcBegin.x; x < srcEnd.x; ++x)
        {
            #if HIZ_REFLECTION_NEAREST
            float2 sampleDepth = LoadSourceDepth(uint2(x, y));
            reducedDepth = float2(max(reducedDepth.x, sampleDepth.x), min(reducedDepth.y, sampleDepth.y));
            #else
            reducedDepth = min(reducedDepth, LoadSourceDepth(uint2(x, y)));
            #endif
        }
    }
    gDstMip[dstCoord] = reducedDepth;
}
