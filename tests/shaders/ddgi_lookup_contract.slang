#define DDGI_SET 0
#include "ddgi_lookup_common.slang"

struct DdgiLookupQuery
{
    float4 world;
    float4 geometricNormal;
    float4 shadingNormal;
    float4 viewDirection;
};

[[vk::binding(12, 0)]] StructuredBuffer<DdgiLookupQuery> uQueries;
[[vk::binding(13, 0)]] RWStructuredBuffer<float4> uResults;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    DdgiLookupQuery query = uQueries[tid.x];
    DdgiLookupResult result = LookupDdgiIrradiance(query.world.xyz,
        query.geometricNormal.xyz, query.shadingNormal.xyz, query.viewDirection.xyz);
    uResults[tid.x] = float4(result.irradiance, result.coverage);
}
