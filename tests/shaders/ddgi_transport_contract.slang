#include "ddgi_transport_common.slang"

struct DdgiTransportQuery
{
    GpuMaterial material;
    float4 transmissionAlphaMetallicRoughness;
    float4 tintIncidence;
    uint4 controls; // enabled, requested crossings, object flags, 0 thin / 1 dielectric
    float4 throughput;
    float4 directionDistance;
    float4 normalEntering;
    float4 randomReserved;
};
struct DdgiTransportResult
{
    float4 transmittanceSupported;
    float4 throughputContinued;
    float4 dielectricDirectionTir;
    float4 absorptionSupported;
};
[[vk::binding(0, 0)]] StructuredBuffer<DdgiTransportQuery> uQueries;
[[vk::binding(1, 0)]] RWStructuredBuffer<DdgiTransportResult> uResults;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    DdgiTransportQuery query = uQueries[tid.x];
    float4 parameters = query.transmissionAlphaMetallicRoughness;
    DdgiTransportResult result = (DdgiTransportResult)0;
    if (query.controls.w == 1u)
    {
        DdgiDielectricResult dielectric = DdgiSampleDielectric(query.material,
            query.material.flags | query.controls.z, parameters.x, parameters.y,
            parameters.z, parameters.w, query.tintIncidence.rgb,
            query.directionDistance.xyz, query.normalEntering.xyz,
            query.normalEntering.w != 0.0, query.randomReserved.xy,
            query.controls.x != 0u);
        result.transmittanceSupported = float4(dielectric.weight, float(dielectric.supported));
        result.dielectricDirectionTir = float4(dielectric.direction,
            float(dielectric.totalInternalReflection));
        float3 absorption;
        bool absorptionSupported = DdgiVolumeAbsorption(query.material,
            query.directionDistance.w, absorption);
        result.absorptionSupported = float4(absorption, absorptionSupported ? 1.0 : 0.0);
        uResults[tid.x] = result;
        return;
    }
    DdgiThinGlassResult glass = DdgiThinGlassTransmission(query.material,
        query.material.flags | query.controls.z, parameters.x, parameters.y,
        parameters.z, parameters.w, query.tintIncidence.rgb, query.tintIncidence.w,
        query.controls.x != 0u);
    result.transmittanceSupported = float4(glass.transmittance, float(glass.supported));
    float3 throughput = query.throughput.rgb;
    bool continued = true;
    for (uint crossed = 0u; crossed < min(query.controls.y, DDGI_MAX_GLASS_CROSSINGS + 1u); ++crossed)
    {
        continued = DdgiAdvanceThinGlass(throughput, glass, crossed);
        if (!continued) break;
    }
    result.throughputContinued = float4(throughput, continued ? 1.0 : 0.0);
    uResults[tid.x] = result;
}
