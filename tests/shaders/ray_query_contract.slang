// A GPU contract probe, independent of the raster camera/frustum/shadow maps.
#include "ray_scene_data.slang"
#include "material_data_common.slang"
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
struct Probe {
    float4 originAndMin;
    float4 directionAndMax;
    // Legacy: mask, accept candidate, unused, zero. Alpha: mask, float factor
    // bits, float cutoff bits, material flags + 1.
    uint4 options;
};
[[vk::binding(1, 0)]] StructuredBuffer<Probe> probes;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> results;
[[vk::binding(3, 0)]] StructuredBuffer<RayTriangleData> triangles;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    Probe probe = probes[id.x];
    RayDesc ray;
    ray.Origin = probe.originAndMin.xyz;
    ray.TMin = probe.originAndMin.w;
    ray.Direction = probe.directionAndMax.xyz;
    ray.TMax = probe.directionAndMax.w;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, probe.options.x, ray);
    uint candidates = 0;
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            ++candidates;
            bool accept = probe.options.y != 0;
            if (probe.options.w != 0u)
            {
                float vertexAlpha = RayTriangleColorAlpha(
                    triangles[query.CandidatePrimitiveIndex()], query.CandidateTriangleBarycentrics());
                float alpha = ResolveGpuMaterialAlpha(
                    asfloat(probe.options.y) * vertexAlpha, probe.options.w - 1u);
                accept = isfinite(alpha) && alpha >= asfloat(probe.options.z);
            }
            if (accept) query.CommitNonOpaqueTriangleHit();
        }
    }
    bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    results[id.x] = uint4(hit ? 1 : 0, hit ? query.CommittedInstanceID() : 0xffffffff,
                         candidates, hit ? asuint(query.CommittedRayT()) : 0);
}
