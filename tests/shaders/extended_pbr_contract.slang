#include "pbr_extended_common.slang"
#include "material_data_common.slang"

// This contract has no sampled-image descriptors. EvaluateSurfaceNormal is
// always called with an invalid normal-texture index, so this fallback is
// deliberately unused; it only makes the production frame include standalone.
float4 SamplePbrTexture(uint index, float2 uv, float4 fallback)
{
    return fallback;
}
#include "surface_normal_common.slang"

struct ContractQuery
{
    float4 view;
    float4 light;
    float4 f0;
    float4 radiance;
    float4 frame; // model X scale, authored tangent sign, face sign, IBL normal scale
    float4 textureRG;
    uint4 info; // material array index, reserved
};

struct ContractResult
{
    float4 direct;
    float4 legacyDirect;
    float4 layered;
    float4 legacyLayered;
    float4 coatOnly;
    float4 anisotropyTangent;
    float4 sourceTangent;
    float4 sourceBitangent;
    float4 abiFactors;
    float4 abiColor;
    uint4 abiIndices;
    float4 abiTransforms[6];
    float4 iblInput;
    float4 iblNormal;
    float4 diffuseWeights;
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuMaterial> materials;
[[vk::binding(1, 0)]] StructuredBuffer<ContractQuery> queries;
[[vk::binding(2, 0)]] RWStructuredBuffer<ContractResult> results;

[shader("compute")]
[numthreads(32, 1, 1)]
void computeMain(uint3 dispatch : SV_DispatchThreadID)
{
    uint count, stride;
    queries.GetDimensions(count, stride);
    if (dispatch.x >= count) return;
    ContractQuery query = queries[dispatch.x];
    GpuMaterial material = materials[query.info.x];
    float3x3 model = float3x3(query.frame.x, 0, 0, 0, 1, 0, 0, 0, 1);
    float4 worldTangent = BuildWorldTangent(model, float3(0, 0, 1),
                                           float4(1, 0, 0, query.frame.y));
    SurfaceNormalState surface = EvaluateSurfaceNormal(float3(0, 0, 1),
        worldTangent, 0.0.xx, 0xffffffffu, 1.0, query.frame.z);
    float3 tangent = BuildAnisotropyTangent(surface, worldTangent, query.textureRG.xy,
                                           material.anisotropyDiffuseFactors.y);
    ExtendedPbrTerms terms = MakeExtendedPbrTerms(tangent,
        material.anisotropyDiffuseFactors.x, material.diffuseTransmissionColorFactor.rgb,
        material.anisotropyDiffuseFactors.z, material.transmissionFactor);
    float metallic = material.metallicRoughness.x;
    float roughness = material.metallicRoughness.y;
    float3 N = surface.shadingNormal;
    ContractResult result;
    result.direct = float4(EvaluateExtendedPbrDirectLight(material.color.rgb,
        query.f0.rgb, metallic, roughness, N, query.view.xyz, query.light.xyz,
        query.radiance.rgb, terms), HasExtendedPbrEffect(terms) ? 1.0 : 0.0);
    result.legacyDirect = float4(EvaluatePbrDirectLight(material.color.rgb,
        query.f0.rgb, metallic, roughness, N, query.view.xyz, query.light.xyz,
        query.radiance.rgb), 0.0);
    result.layered = float4(EvaluateExtendedPbrLayeredDirectLight(material.color.rgb,
        query.f0.rgb, metallic, roughness, N, query.view.xyz, query.light.xyz,
        query.radiance.rgb, material.clearcoatFactor, material.clearcoatRoughnessFactor,
        material.sheenColorFactor.rgb, material.sheenRoughnessFactor, terms), 0.0);
    result.legacyLayered = float4(EvaluateDeferredLayeredDirectLight(material.color.rgb,
        query.f0.rgb, metallic, roughness, N, query.view.xyz, query.light.xyz,
        query.radiance.rgb, material.clearcoatFactor, material.clearcoatRoughnessFactor,
        material.sheenColorFactor.rgb, material.sheenRoughnessFactor), 0.0);
    result.coatOnly = float4(EvaluateDeferredClearcoatDirectLight(material.clearcoatFactor,
        material.clearcoatRoughnessFactor, N, query.view.xyz, query.light.xyz,
        query.radiance.rgb), 0.0);
    result.anisotropyTangent = float4(tangent, 0.0);
    result.sourceTangent = float4(surface.tangent, worldTangent.w);
    result.sourceBitangent = float4(surface.bitangent, 0.0);
    result.abiFactors = material.anisotropyDiffuseFactors;
    result.abiColor = material.diffuseTransmissionColorFactor;
    result.abiIndices = uint4(material.anisotropyTextureIndex,
        material.diffuseTransmissionTextureIndex, material.diffuseTransmissionColorTextureIndex,
        material.extendedPadding);
    result.abiTransforms[0] = material.anisotropyTextureTransform.row0;
    result.abiTransforms[1] = material.anisotropyTextureTransform.row1;
    result.abiTransforms[2] = material.diffuseTransmissionTextureTransform.row0;
    result.abiTransforms[3] = material.diffuseTransmissionTextureTransform.row1;
    result.abiTransforms[4] = material.diffuseTransmissionColorTextureTransform.row0;
    result.abiTransforms[5] = material.diffuseTransmissionColorTextureTransform.row1;
    result.iblInput = float4(N * query.frame.w, 0.0);
    result.iblNormal = float4(ExtendedPbrIblNormal(result.iblInput.xyz, query.view.xyz,
                                                 roughness, terms), 0.0);
    result.diffuseWeights = float4(ExtendedPbrDiffuseReflectionWeight(terms),
        ExtendedPbrDiffuseTransmissionWeight(terms), 0.0, 0.0);
    results[dispatch.x] = result;
}
