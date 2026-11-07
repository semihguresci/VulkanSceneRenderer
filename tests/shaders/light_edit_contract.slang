#include "lighting_structs.slang"

// Read the same published set used by both raster lighting paths. The test
// never substitutes an input UBO or SSBO for these production bindings.
[[vk::binding(0, 0)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 0)]] StructuredBuffer<PointLightData> uPoints;
[[vk::binding(2, 0)]] StructuredBuffer<AreaLightData> uAreas;
[[vk::binding(0, 1)]] RWStructuredBuffer<uint4> uResult;
struct LightQuery { uint pointIndex; uint areaIndex; };
[[vk::push_constant]] ConstantBuffer<LightQuery> query;

[numthreads(1, 1, 1)]
[shader("compute")]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uResult[0] = asuint(uLighting.directionalDirection);
    uResult[1] = asuint(uLighting.directionalColorIntensity);
    uResult[2] = uint4(uLighting.pointLightCount, uLighting.shadowEnabled,
                      uLighting.gtaoEnabled, uLighting.tileCullEnabled);
    uResult[3] = uint4(uLighting.prefilteredMipCount,
                      asuint(uLighting.environmentIntensity),
                      uLighting.areaLightCount, uLighting.localShadowEnabled);
    uResult[4] = uint4(asuint(uLighting.bounceIntensity),
                      uLighting.localContactVisibility, uLighting.areaLightingMode,
                      uLighting.areaLightSampleCount);
    for (uint row = 5; row < 14; ++row) uResult[row] = uint4(0);
    if (query.pointIndex < uLighting.pointLightCount)
    {
        PointLightData p = uPoints[query.pointIndex];
        uResult[5] = asuint(p.positionRadius);
        uResult[6] = asuint(p.colorIntensity);
        uResult[7] = asuint(p.directionInnerCos);
        uResult[8] = asuint(p.coneOuterCosType);
    }
    if (query.areaIndex < uLighting.areaLightCount)
    {
        AreaLightData a = uAreas[query.areaIndex];
        uResult[9] = asuint(a.positionRange);
        uResult[10] = asuint(a.colorIntensity);
        uResult[11] = asuint(a.directionType);
        uResult[12] = asuint(a.tangentHalfSize);
        uResult[13] = asuint(a.bitangentHalfSize);
    }
}
