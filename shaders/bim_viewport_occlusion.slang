#include "lighting_structs.slang"
#include "object_data_common.slang"
#include "occlusion_sphere_common.slang"
#include "bim_residency_flags_common.slang"

struct BimMeshletResidencyEntry
{
    uint flags;
    uint selectedLod;
    uint firstCluster;
    uint clusterCount;
};

struct BimViewportOcclusionPushConstants
{
    uint objectCount;
};

[[vk::push_constant]]
ConstantBuffer<BimViewportOcclusionPushConstants> pc;
[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] RWStructuredBuffer<BimMeshletResidencyEntry> uResidency;
[[vk::binding(3, 0)]] Texture2D<float> gHiZPyramid;

bool occluded(float4 bounds)
{
    float2 minUV, maxUV;
    float closestDepth;
    if (!ProjectSphere(uCamera.viewProj, bounds.xyz, bounds.w, minUV, maxUV, closestDepth))
        return false;

    uint width, height, levels;
    gHiZPyramid.GetDimensions(0u, width, height, levels);
    const float2 projectedPixels = (maxUV - minUV) * float2(width, height);
    const float extent = max(projectedPixels.x, projectedPixels.y);
    if (extent > 96.0)
        return false;

    // At this mip a projected axis overlaps at most two texels. Include every
    // overlapping min-depth texel, including coverage from an odd source size.
    const uint mip = min(uint(ceil(log2(max(1.0, extent)))), levels - 1u);
    uint mipWidth, mipHeight;
    gHiZPyramid.GetDimensions(mip, mipWidth, mipHeight, levels);
    const uint2 mipExtent = uint2(mipWidth, mipHeight);
    const uint2 begin = min(uint2(floor(minUV * float2(mipExtent))), mipExtent - 1u);
    const uint2 end = min(uint2(floor(maxUV * float2(mipExtent))), mipExtent - 1u);
    float conservativeDepth = 1.0;
    for (uint y = begin.y; y <= end.y; ++y)
    {
        for (uint x = begin.x; x <= end.x; ++x)
        {
            conservativeDepth = min(conservativeDepth, gHiZPyramid.Load(int3(int2(x, y), int(mip))));
        }
    }

    // The reverse-Z comparison leaves ties and near-coplanar reliefs visible.
    return closestDepth + 0.005 < conservativeDepth;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= pc.objectCount)
        return;

    const uint flags = uResidency[index].flags;
    if ((flags & BIM_MESHLET_IN_VIEWPORT_FRUSTUM) != 0u &&
        !occluded(uObjects[index].boundingSphere))
        uResidency[index].flags = flags | BIM_MESHLET_VISIBLE_AFTER_HIZ;
}
