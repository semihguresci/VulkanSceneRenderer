// frustum_cull.slang — Compute shader for GPU-driven frustum culling.
// One thread per object. Tests bounding sphere against 6 frustum planes
// extracted from the viewProj matrix. Surviving objects write their
// VkDrawIndexedIndirectCommand to the output buffer.

#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "draw_indirect_common.slang"

#include "object_data_common.slang"
#include "frustum_sphere_common.slang"

[[vk::push_constant]]
ConstantBuffer<CullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer>                uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer>               uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand>  uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>                      uDrawCount;

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
    float4x4 cullingViewProj = StableFrustumViewProj(
        uCamera.viewProj, uCamera.unjitteredViewProj,
        uCamera.temporalInfo.w, uCamera.renderExtent.xy);
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
