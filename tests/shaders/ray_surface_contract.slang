// Compile contract, not a material/alpha correctness test. The explicit
// acceptance buffer stands in for the future consumer's candidate policy.
#include "ray_surface_common.slang"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] StructuredBuffer<RayTriangleData> uvTriangles;
[[vk::binding(2, 0)]] StructuredBuffer<RaySurfaceTriangleData> surfaceTriangles;
[[vk::binding(3, 0)]] StructuredBuffer<RayInstanceData> instances;
[[vk::binding(4, 0)]] StructuredBuffer<RaySurfaceInstanceData> surfaceInstances;
struct RaySurfaceProbe
{
    float4 originAndMin;
    float4 directionAndMax;
};
[[vk::binding(5, 0)]] StructuredBuffer<RaySurfaceProbe> probes;
[[vk::binding(6, 0)]] StructuredBuffer<uint> candidateAcceptance;
[[vk::binding(7, 0)]] RWStructuredBuffer<RaySurfacePoint> results;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    RaySurfaceProbe probe = probes[id.x];
    RayDesc ray;
    ray.Origin = probe.originAndMin.xyz;
    ray.TMin = probe.originAndMin.w;
    ray.Direction = probe.directionAndMax.xyz;
    ray.TMax = probe.directionAndMax.w;
    // No facing cull: probe relocation must be able to observe a back face.
    // A radiance consumer can select a different documented facing policy.
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint instanceIndex = query.CandidateInstanceID();
        // Consumer must replace this hook with clipping/material alpha and
        // supported transmission/displacement policy before radiance tracing.
        if (candidateAcceptance[instanceIndex] != 0u)
            query.CommitNonOpaqueTriangleHit();
    }
    RaySurfacePoint point = (RaySurfacePoint)0;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint instanceIndex = query.CommittedInstanceID();
        RayInstanceData metadata = instances[instanceIndex];
        uint triangleIndex = metadata.info.x + query.CommittedPrimitiveIndex();
        point = ReconstructRaySurface(uvTriangles[triangleIndex],
            surfaceTriangles[triangleIndex], metadata, surfaceInstances[instanceIndex],
            query.CommittedTriangleBarycentrics(), query.CommittedTriangleFrontFace());
        point = OrientRaySurfaceForShading(point, 0u);
    }
    results[id.x] = point;
}
