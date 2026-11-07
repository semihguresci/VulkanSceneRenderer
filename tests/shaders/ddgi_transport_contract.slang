#include "ddgi_transport_common.slang"

struct DdgiTransportQuery
{
    GpuMaterial material;
    float4 transmissionAlphaMetallicRoughness;
    float4 tintIncidence;
    uint4 controls; // enabled, requested crossings, object flags, 0 thin / 1 dielectric / 2 media
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

DdgiTransportResult CheckDdgiMediumStack(DdgiTransportQuery query)
{
    DdgiTransportResult result = (DdgiTransportResult)0;
    DdgiMedium outer = { 4u, 2u, query.material.ior };
    DdgiMedium inner = { 7u, 3u, query.randomReserved.z };
    DdgiMedium differentMaterial = { inner.instanceIndex, outer.materialIndex, inner.ior };
    DdgiMediumStack stack = (DdgiMediumStack)0;
    result.transmittanceSupported.x = DdgiCanCrossMedium(stack, outer, false) ? 0.0 : 1.0;
    result.throughputContinued.x = DdgiAmbientIor(stack, true);
    bool valid = DdgiCanCrossMedium(stack, outer, true);
    DdgiCommitMediumCrossing(stack, outer, true);
    result.transmittanceSupported.y = DdgiCanCrossMedium(stack, outer, true) ? 0.0 : 1.0;
    result.throughputContinued.y = DdgiAmbientIor(stack, true);
    valid = valid && DdgiCanCrossMedium(stack, inner, true);
    DdgiCommitMediumCrossing(stack, inner, true);
    result.transmittanceSupported.z = DdgiCanCrossMedium(stack, outer, false) ? 0.0 : 1.0;
    result.transmittanceSupported.w = DdgiCanCrossMedium(stack, differentMaterial, false) ? 0.0 : 1.0;
    result.throughputContinued.z = DdgiAmbientIor(stack, false);

    GpuMaterial liquid = query.material;
    liquid.attenuationColor = float4(query.tintIncidence.rgb, 1.0);
    liquid.attenuationDistance = query.tintIncidence.w;
    float3 innerAbsorption;
    bool absorbedInner = DdgiVolumeAbsorption(liquid, query.normalEntering.w, innerAbsorption);
    valid = valid && DdgiCanCrossMedium(stack, inner, false);
    DdgiCommitMediumCrossing(stack, inner, false);
    result.dielectricDirectionTir.z = float(stack.media[stack.count - 1u].materialIndex);
    result.throughputContinued.w = DdgiAmbientIor(stack, false);
    float3 outerAbsorption;
    bool absorbedOuter = DdgiVolumeAbsorption(query.material,
        query.directionDistance.w + query.randomReserved.w, outerAbsorption);
    result.absorptionSupported = float4(outerAbsorption * innerAbsorption,
        absorbedOuter && absorbedInner ? 1.0 : 0.0);
    valid = valid && DdgiCanCrossMedium(stack, outer, false);
    DdgiCommitMediumCrossing(stack, outer, false);
    result.dielectricDirectionTir.x = float(stack.count);
    for (uint i = 0u; i < DDGI_MAX_MEDIA; ++i)
    {
        DdgiMedium boundary = { i + 10u, i, 1.5 };
        valid = valid && DdgiCanCrossMedium(stack, boundary, true);
        DdgiCommitMediumCrossing(stack, boundary, true);
    }
    result.dielectricDirectionTir.y = DdgiCanCrossMedium(stack, outer, true) ? 0.0 : 1.0;
    result.dielectricDirectionTir.w = valid ? 1.0 : 0.0;
    return result;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    DdgiTransportQuery query = uQueries[tid.x];
    float4 parameters = query.transmissionAlphaMetallicRoughness;
    DdgiTransportResult result = (DdgiTransportResult)0;
    if (query.controls.w == 2u)
    {
        uResults[tid.x] = CheckDdgiMediumStack(query);
        return;
    }
    if (query.controls.w == 1u)
    {
        DdgiDielectricResult dielectric = DdgiSampleDielectric(query.material,
            query.material.flags | query.controls.z, parameters.x, parameters.y,
            parameters.z, parameters.w, query.tintIncidence.rgb,
            query.directionDistance.xyz, query.normalEntering.xyz,
            query.normalEntering.w != 0.0, query.randomReserved.xy,
            query.controls.x != 0u, query.randomReserved.z == 0.0 ? 1.0 : query.randomReserved.z);
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
