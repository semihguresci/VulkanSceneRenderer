#include "occlusion_sphere_common.slang"

struct SphereQuery
{
    column_major float4x4 viewProj;
    float4 sphere;
};

struct SphereResult
{
    float4 bounds;
    float4 depthAndValid;
};

[[vk::binding(0, 0)]] StructuredBuffer<SphereQuery> queries;
[[vk::binding(1, 0)]] RWStructuredBuffer<SphereResult> results;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    SphereQuery query = queries[id.x];
    float2 minUV, maxUV;
    float closestDepth;
    bool valid = ProjectSphere(query.viewProj, query.sphere.xyz, query.sphere.w,
                               minUV, maxUV, closestDepth);
    SphereResult result;
    result.bounds = float4(minUV, maxUV);
    result.depthAndValid = float4(closestDepth, valid ? 1.0 : 0.0, 0.0, 0.0);
    results[id.x] = result;
}
