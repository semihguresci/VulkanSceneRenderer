#pragma once
#include "material_data_common.slang"
#include "ray_surface_common.slang"
#include "pbr_extended_common.slang"

// Fully sampled terms shared by real probe-hit shading and GPU contracts.
// Alpha/candidate acceptance and stochastic dielectric transport are separate.
struct DdgiShadingMaterial
{
    float3 albedo;
    float3 f0;
    float3 emissive;
    float3 normal;
    float metallic;
    float roughness;
    float layerScale;
    float occlusion;
    float clearcoat;
    float clearcoatRoughness;
    float3 sheenColor;
    float sheenRoughness;
    ExtendedPbrTerms extended;
    uint unsupported;
    uint unlit;
};

ExtendedPbrTerms ResolveDdgiExtendedPbrTerms(GpuMaterial material,
    RaySurfacePoint point, float3 finalNormal, float specularTransmission,
    float4 anisotropyTexel, float4 diffuseTexel, float4 diffuseColorTexel)
{
    // The absent anisotropy texture points along +T, not white's diagonal RG.
    if (material.anisotropyTextureIndex == kInvalidGpuMaterialTexture ||
        material.anisotropyTextureIndex >= MATERIAL_TEXTURE_DESCRIPTOR_COUNT)
        anisotropyTexel = float4(1.0, 0.5, 1.0, 1.0);
    float strength = saturate(FiniteScalarOr(material.anisotropyDiffuseFactors.x, 0.0)) *
        saturate(FiniteScalarOr(anisotropyTexel.b, 0.0));
    float diffuse = saturate(FiniteScalarOr(material.anisotropyDiffuseFactors.z, 0.0)) *
        saturate(FiniteScalarOr(SelectGpuTextureChannel(diffuseTexel,
            material.diffuseTransmissionTextureTransform, 3u), 0.0));
    float3 color = saturate(Finite3Or(material.diffuseTransmissionColorFactor.rgb, 1.0.xxx)) *
        saturate(Finite3Or(diffuseColorTexel.rgb, 0.0.xxx));
    return MakeExtendedPbrTerms(ResolveRayAnisotropyTangent(point, finalNormal,
        anisotropyTexel.rg, material.anisotropyDiffuseFactors.y), strength,
        color, diffuse, specularTransmission);
}

// Returns the geometric normal facing the light, or zero for an unsupported
// hemisphere. A normal map alone cannot open a path through the geometric face.
float3 DdgiDirectVisibilityNormal(DdgiShadingMaterial material,
                                  float3 geometricNormal, float3 lightDirection)
{
    float shadingDot = dot(material.normal, lightDirection);
    float geometricDot = dot(geometricNormal, lightDirection);
    if (!isfinite(shadingDot) || !isfinite(geometricDot)) return 0.0.xxx;
    if (shadingDot > 0.0 && geometricDot > 0.0) return geometricNormal;
    if (material.extended.diffuseTransmissionFactor > 0.0 &&
        shadingDot < 0.0 && geometricDot < 0.0) return -geometricNormal;
    return 0.0.xxx;
}

float3 EvaluateDdgiHitDirect(DdgiShadingMaterial material,
    float3 geometricNormal, float3 V, float3 L, float3 radiance)
{
    if (!any(DdgiDirectVisibilityNormal(material, geometricNormal, L) != 0.0.xxx))
        return 0.0.xxx;
    // Preserve the existing diffuse-only probe response for ordinary hits.
    if (!HasExtendedPbrEffect(material.extended))
    {
        float cosine = saturate(dot(material.normal, L));
        if (cosine <= 0.0 || dot(material.normal, V) <= 0.0) return 0.0.xxx;
        float3 H = SafeNormalize(V + L, material.normal);
        float3 energy = (1.0.xxx - FresnelSchlick(saturate(dot(H, V)), material.f0)) *
            (1.0 - material.metallic) * material.layerScale;
        return max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx) *
            saturate(material.albedo) * energy * (cosine / PI);
    }
    return EvaluateExtendedPbrLayeredDirectLight(material.albedo, material.f0,
        material.metallic, material.roughness, material.normal, V, L, radiance,
        material.clearcoat, material.clearcoatRoughness, material.sheenColor,
        material.sheenRoughness, material.extended);
}

float3 EvaluateDdgiDiffuseFeedback(DdgiShadingMaterial material, float3 V,
                                   float3 frontIrradiance, float3 backIrradiance)
{
    float3 energy = (1.0.xxx - FresnelSchlickRoughness(saturate(dot(material.normal, V)),
        material.f0, material.roughness)) * (1.0 - material.metallic) * material.layerScale;
    if (!HasExtendedPbrEffect(material.extended))
        return frontIrradiance * saturate(material.albedo) * energy * material.occlusion / PI;
    float residual = 1.0 - material.extended.specularTransmissionFactor;
    float frontWeight = residual * (1.0 - material.extended.diffuseTransmissionFactor);
    float backWeight = residual * material.extended.diffuseTransmissionFactor;
    // Front-side material AO does not describe occlusion behind the sheet.
    return (max(Finite3Or(frontIrradiance, 0.0.xxx), 0.0.xxx) * saturate(material.albedo) * frontWeight * material.occlusion +
            max(Finite3Or(backIrradiance, 0.0.xxx), 0.0.xxx) * material.extended.diffuseTransmissionColor * backWeight) *
        energy / PI;
}
