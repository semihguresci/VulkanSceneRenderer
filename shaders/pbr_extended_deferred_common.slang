#pragma once
#include "material_data_common.slang"
#include "pbr_extended_common.slang"

[[vk::binding(23, 0)]] Texture2D<float4> gAnisotropyTexture;
[[vk::binding(24, 0)]] Texture2D<float4> gDiffuseTransmissionTexture;

ExtendedPbrTerms LoadDeferredExtendedPbrTerms(
    GpuMaterial material, int2 pixel, float specularTransmission)
{
    float4 anisotropy = 0.0.xxxx;
    float4 diffuseTransmission = 0.0.xxxx;
    // The scene-wide flag is present in every material. A resolved ordinary
    // material may still share coverage with an extended material, so its own
    // authored factors cannot gate these loads. No-feature scenes use valid
    // ordinary G-buffer fallbacks and never sample them here.
    if (material.anisotropyDiffuseFactors.w > 0.0)
    {
        anisotropy = gAnisotropyTexture.Load(int3(pixel, 0));
        diffuseTransmission = gDiffuseTransmissionTexture.Load(int3(pixel, 0));
    }
    return MakeExtendedPbrTerms(anisotropy.xyz, anisotropy.w,
        DecodeExtendedPbrDiffuseStorageColor(diffuseTransmission),
        diffuseTransmission.w, specularTransmission);
}

float3 ExtendedPbrDeferredShadowNormal(float3 geometricNormal,
    float3 shadingNormal, float3 lightDirection, ExtendedPbrTerms terms)
{
    return terms.diffuseTransmissionFactor > 0.0 &&
           dot(geometricNormal, lightDirection) < 0.0 &&
           dot(shadingNormal, lightDirection) < 0.0
        ? -geometricNormal : geometricNormal;
}
