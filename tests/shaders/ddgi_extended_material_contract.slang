#include "ddgi_material_common.slang"

struct ExtendedDdgiQuery
{
    GpuMaterial material;
    float4 smoothNormal;
    float4 geometricNormal;
    float4 tangent;
    float4 finalNormal;
    float4 view;
    float4 light;
    float4 uv;
    float4 frontIrradiance;
    float4 backIrradiance;
    float4 albedo;
    float4 f0Metallic;
    float4 radianceRoughness;
    float4 occlusionLayerTransmission;
    uint4 controls;
};
struct ExtendedDdgiResult
{
    float4 direct;
    float4 feedback;
    float4 anisotropy;
    float4 diffuse;
    float4 visibilityNormal;
    float4 resolvedUv;
    float4 sampledLayerFactors;
    float4 sampledSheenIridescence;
    float4 deferredLayerFactors;
    float4 deferredSheen;
};
[[vk::binding(0, 0)]] StructuredBuffer<ExtendedDdgiQuery> uQueries;
[[vk::binding(1, 0)]] RWStructuredBuffer<ExtendedDdgiResult> uResults;
[[vk::binding(2, 0)]] SamplerState testSampler;
[[vk::binding(3, 0)]] Texture2D<float4> dataTexture;
[[vk::binding(4, 0)]] Texture2D<float4> colorTexture;

float4 SampleExtendedContract(uint index, float4 uv, GpuTextureTransform transform)
{
    if (index == kInvalidGpuMaterialTexture) return 1.0.xxxx;
    float2 selected = ApplyGpuTextureTransform(uv.xy, uv.zw, transform);
    return index == 1u ? colorTexture.SampleLevel(testSampler, selected, 0.0) :
                        dataTexture.SampleLevel(testSampler, selected, 0.0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    ExtendedDdgiQuery query = uQueries[tid.x];
    RaySurfacePoint point = (RaySurfacePoint)0;
    point.shadingNormal = query.smoothNormal.xyz;
    point.geometricNormal = query.geometricNormal.xyz;
    point.tangent = query.tangent;
    point.uv = query.uv;
    DdgiShadingMaterial material = (DdgiShadingMaterial)0;
    material.normal = query.finalNormal.xyz;
    material.albedo = query.albedo.rgb;
    material.f0 = query.f0Metallic.rgb;
    material.metallic = query.f0Metallic.w;
    material.roughness = query.radianceRoughness.w;
    material.occlusion = query.occlusionLayerTransmission.x;
    material.layerScale = query.occlusionLayerTransmission.y;
    float4 anisotropy = 1.0.xxxx, diffuse = 1.0.xxxx, color = 1.0.xxxx;
    if (query.controls.x != 0u)
    {
        anisotropy = SampleExtendedContract(query.material.anisotropyTextureIndex, point.uv,
            query.material.anisotropyTextureTransform);
        diffuse = SampleExtendedContract(query.material.diffuseTransmissionTextureIndex, point.uv,
            query.material.diffuseTransmissionTextureTransform);
        color = SampleExtendedContract(query.material.diffuseTransmissionColorTextureIndex, point.uv,
            query.material.diffuseTransmissionColorTextureTransform);
    }
    material.extended = ResolveDdgiExtendedPbrTerms(query.material, point, material.normal,
        query.occlusionLayerTransmission.z, anisotropy, diffuse, color);
    ExtendedDdgiResult result;
    result.direct = float4(EvaluateDdgiHitDirect(material, point.geometricNormal,
        query.view.xyz, query.light.xyz, query.radianceRoughness.rgb), 0);
    result.feedback = float4(EvaluateDdgiDiffuseFeedback(material, query.view.xyz,
        query.frontIrradiance.rgb, query.backIrradiance.rgb), 0);
    result.anisotropy = float4(material.extended.anisotropyTangent, material.extended.anisotropyStrength);
    result.diffuse = float4(material.extended.diffuseTransmissionColor, material.extended.diffuseTransmissionFactor);
    result.visibilityNormal = float4(DdgiDirectVisibilityNormal(material,
        point.geometricNormal, query.light.xyz), 0);
    result.resolvedUv = float4(ApplyGpuTextureTransform(point.uv.xy, point.uv.zw,
        query.material.anisotropyTextureTransform),ApplyGpuTextureTransform(point.uv.xy, point.uv.zw,
        query.material.diffuseTransmissionTextureTransform));
    GpuMaterial m = query.material;
    result.sampledLayerFactors = float4(
        ResolveDdgiScalarFactor(m.transmissionFactor,
            SampleExtendedContract(m.transmissionTextureIndex, point.uv, m.transmissionTextureTransform),
            m.transmissionTextureTransform, 0u),
        ResolveDdgiScalarFactor(m.clearcoatFactor,
            SampleExtendedContract(m.clearcoatTextureIndex, point.uv, m.clearcoatTextureTransform),
            m.clearcoatTextureTransform, 0u),
        ResolveDdgiScalarFactor(m.clearcoatRoughnessFactor,
            SampleExtendedContract(m.clearcoatRoughnessTextureIndex, point.uv, m.clearcoatRoughnessTextureTransform),
            m.clearcoatRoughnessTextureTransform, 1u),
        ResolveDdgiScalarFactor(m.sheenRoughnessFactor,
            SampleExtendedContract(m.sheenRoughnessTextureIndex, point.uv, m.sheenRoughnessTextureTransform),
            m.sheenRoughnessTextureTransform, 3u));
    result.sampledSheenIridescence = float4(
        ResolveMaterialColorFactor(m.sheenColorFactor.rgb) *
            SampleExtendedContract(m.sheenColorTextureIndex, point.uv, m.sheenColorTextureTransform).rgb,
        ResolveDdgiScalarFactor(m.iridescenceFactor,
            SampleExtendedContract(m.iridescenceTextureIndex, point.uv, m.iridescenceTextureTransform),
            m.iridescenceTextureTransform, 0u));
    DeferredMaterialLayerTerms deferred = ResolveDeferredMaterialLayerTerms(m);
    result.deferredLayerFactors = float4(deferred.transmissionFactor, deferred.clearcoatFactor,
        deferred.clearcoatRoughness, deferred.sheenRoughness);
    result.deferredSheen = float4(deferred.sheenColor, deferred.iridescenceFactor);
    uResults[tid.x] = result;
}
