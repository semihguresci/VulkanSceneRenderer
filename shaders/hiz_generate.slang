// hiz_generate.slang — Compute shader for hierarchical depth buffer generation.
// Generates a min-reduction mip chain from the depth prepass output.
// For reverse-Z (near=1, far=0), we take the minimum (conservative far plane).

#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<HiZPushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float>    gSrcDepth;
[[vk::binding(1, 0)]] SamplerState        gSrcSampler;
[[vk::binding(2, 0)]] RWTexture2D<float>  gDstMip;

float LoadSourceDepth(uint2 coord)
{
    float depth = gSrcDepth.Load(int3(coord, 0));
    return (isfinite(depth) && depth >= 0.0 && depth <= 1.0) ? depth : 0.0;
}

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
    bool copyDepth = pc.dstMipLevel == 0;
    uint2 expectedDstExtent = copyDepth ? srcExtent : max(srcExtent >> 1u, 1u);
    if (any(srcExtent != uint2(pc.srcWidth, pc.srcHeight)) || any(dstExtent != expectedDstExtent))
    {
        // A mismatched depth view cannot establish occlusion.
        gDstMip[dstCoord] = 0.0;
        return;
    }

    if (copyDepth)
    {
        gDstMip[dstCoord] = LoadSourceDepth(dstCoord);
        return;
    }

    // Include every source texel overlapping this destination texel in
    // normalized coordinates. Odd source extents straddle a third texel.
    uint2 srcBegin = dstCoord * 2u;
    uint2 srcEnd = min(srcBegin + 2u + (srcExtent & 1u), srcExtent);
    float minDepth = 1.0;
    for (uint y = srcBegin.y; y < srcEnd.y; ++y)
    {
        for (uint x = srcBegin.x; x < srcEnd.x; ++x)
        {
            minDepth = min(minDepth, LoadSourceDepth(uint2(x, y)));
        }
    }
    gDstMip[dstCoord] = minDepth;
}
