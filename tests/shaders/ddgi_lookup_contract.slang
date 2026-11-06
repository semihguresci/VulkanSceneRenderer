#define DDGI_SET 0
#include "ddgi_lighting_common.slang"

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
    if (query.world.w >= 2.0)
    {
        GpuMaterial material = (GpuMaterial)0;
        material.transmissionTextureIndex = 0u;
        material.heightTextureIndex = kInvalidGpuMaterialTexture;
        material.transmissionFactor = query.world.w == 3.0 ? 0.0005 : 0.0;
        uint flags = query.viewDirection.w > 0.0 ? kGpuMaterialFlagAlphaBlend : 0u;
        DdgiLookupResult front = LookupDdgiDiffuseReceiver(material, flags,
            query.shadingNormal.w, query.world.xyz, query.geometricNormal.xyz,
            query.shadingNormal.xyz, query.viewDirection.xyz);
        uResults[tid.x] = float4(front.irradiance, front.coverage);
        return;
    }
    if (query.world.w > 0.0)
    {
        GpuMaterial material = (GpuMaterial)0;
        material.transmissionTextureIndex = kInvalidGpuMaterialTexture;
        material.heightTextureIndex = kInvalidGpuMaterialTexture;
        uint flags = query.viewDirection.w > 0.0 ? kGpuMaterialFlagAlphaBlend : 0u;
        DdgiLookupResult back = LookupDdgiDiffuseBackReceiver(material, flags, 0.0,
            query.shadingNormal.w, query.world.xyz, query.geometricNormal.xyz,
            query.shadingNormal.xyz, query.viewDirection.xyz);
        uResults[tid.x] = float4(back.irradiance, back.coverage);
        return;
    }
    DdgiLookupResult result = LookupDdgiIrradiance(query.world.xyz,
        query.geometricNormal.xyz, query.shadingNormal.xyz, query.viewDirection.xyz);
    uResults[tid.x] = float4(result.irradiance, result.coverage);
}
