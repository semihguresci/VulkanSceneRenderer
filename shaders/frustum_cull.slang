// frustum_cull.slang — Compute shader for GPU-driven frustum culling.
// One thread per object. Tests bounding sphere against 6 frustum planes
// extracted from the viewProj matrix. Surviving objects write their
// VkDrawIndexedIndirectCommand to the output buffer.

#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "draw_indirect_common.slang"

#include "object_data_common.slang"

[[vk::push_constant]]
ConstantBuffer<CullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer>                uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer>               uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand>  uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>                      uDrawCount;

// Extract frustum planes from the viewProj matrix (Gribb-Hartmann method).
// Planes point inward (positive half-space = inside frustum).
struct FrustumPlanes
{
    float4 planes[6];
};

FrustumPlanes ExtractFrustumPlanes(float4x4 vp)
{
    FrustumPlanes f;
    // In Slang, matrix indexing is row-first: vp[row][column]. We multiply
    // clip = mul(vp, worldPosition), so the clip inequalities must be built
    // from rows, not GLSL-style column access.
    // Left
    f.planes[0] = float4(vp[3][0] + vp[0][0],
                         vp[3][1] + vp[0][1],
                         vp[3][2] + vp[0][2],
                         vp[3][3] + vp[0][3]);
    // Right
    f.planes[1] = float4(vp[3][0] - vp[0][0],
                         vp[3][1] - vp[0][1],
                         vp[3][2] - vp[0][2],
                         vp[3][3] - vp[0][3]);
    // Bottom
    f.planes[2] = float4(vp[3][0] + vp[1][0],
                         vp[3][1] + vp[1][1],
                         vp[3][2] + vp[1][2],
                         vp[3][3] + vp[1][3]);
    // Top
    f.planes[3] = float4(vp[3][0] - vp[1][0],
                         vp[3][1] - vp[1][1],
                         vp[3][2] - vp[1][2],
                         vp[3][3] - vp[1][3]);
    // Near (Vulkan zero-to-one: clip z >= 0, i.e. the z row)
    f.planes[4] = float4(vp[2][0],
                         vp[2][1],
                         vp[2][2],
                         vp[2][3]);
    // Far (Vulkan zero-to-one: clip z <= w, i.e. w - z >= 0)
    f.planes[5] = float4(vp[3][0] - vp[2][0],
                         vp[3][1] - vp[2][1],
                         vp[3][2] - vp[2][2],
                         vp[3][3] - vp[2][3]);

    // Normalize each plane.
    for (int i = 0; i < 6; ++i)
    {
        float len = length(f.planes[i].xyz);
        if (len > 1e-6)
            f.planes[i] /= len;
    }
    return f;
}

bool SphereFrustumTest(FrustumPlanes f, float3 center, float radius)
{
    for (int i = 0; i < 6; ++i)
    {
        float dist = dot(f.planes[i].xyz, center) + f.planes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= pc.objectCount) return;

    DrawIndexedIndirectCommand draw = uInputDraws[idx];
    uint objectIndex = draw.firstInstance;
    ObjectBuffer obj = uObjects[objectIndex];
    float4 bs = obj.boundingSphere;
    float3 center = bs.xyz;
    float  radius = bs.w;

    // If bounding sphere radius is 0, always pass (degenerate / not computed).
    // Stable unjittered planes with a fixed half-pixel guard band. Hi-Z uses
    // the jittered matrix because its depth was rasterized with that matrix.
    float4x4 cullingViewProj = uCamera.viewProj;
    if (uCamera.temporalInfo.w != 0u) {
        cullingViewProj = uCamera.unjitteredViewProj;
        cullingViewProj[0] *= uCamera.renderExtent.x / (uCamera.renderExtent.x + 1.0);
        cullingViewProj[1] *= uCamera.renderExtent.y / (uCamera.renderExtent.y + 1.0);
    }
    bool visible = (radius <= 0.0) ? true
                 : SphereFrustumTest(ExtractFrustumPlanes(cullingViewProj),
                                     center, radius);

    if (visible)
    {
        uint outIdx;
        InterlockedAdd(uDrawCount[0], 1, outIdx);
        uOutputDraws[outIdx] = draw;
    }
}
