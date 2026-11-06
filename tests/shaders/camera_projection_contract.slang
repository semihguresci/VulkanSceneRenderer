#include "lighting_structs.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(0, 1)]] RWStructuredBuffer<float4> uResult;
struct ProjectionQuery { float4 worldPoint; };
[[vk::push_constant]] ConstantBuffer<ProjectionQuery> query;

[numthreads(1, 1, 1)]
[shader("compute")]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    float4 clip = mul(uCamera.viewProj, query.worldPoint);
    uResult[0] = clip;
    uResult[1] = mul(uCamera.inverseViewProj, clip);
}
