#pragma once

#include "pbr_extended_common.slang"

// Include after pbr_material_common and surface_normal_common, with the scene
// material texture/sampler bindings already declared. All returned inputs are
// fully sampled and scene-linear; this helper never promotes zero factors.
ExtendedPbrTerms SampleExtendedPbrTerms(
    GpuMaterial material, SurfaceNormalState surfaceNormal,
    float4 worldTangent, float2 texCoord0, float2 texCoord1,
    float specularTransmissionFactor)
{
    float4 anisotropySample = SamplePbrTexture(
        material.anisotropyTextureIndex,
        ApplyGpuTextureTransform(texCoord0, texCoord1,
                                 material.anisotropyTextureTransform),
        float4(1.0, 0.5, 1.0, 1.0));
    float3 tangent = BuildAnisotropyTangent(
        surfaceNormal, worldTangent, anisotropySample.rg,
        material.anisotropyDiffuseFactors.y);
    float strength = material.anisotropyDiffuseFactors.x * anisotropySample.b;

    float4 factorSample = SamplePbrTexture(
        material.diffuseTransmissionTextureIndex,
        ApplyGpuTextureTransform(texCoord0, texCoord1,
                                 material.diffuseTransmissionTextureTransform),
        1.0.xxxx);
    float factor = material.anisotropyDiffuseFactors.z *
        SelectGpuTextureChannel(factorSample,
                                 material.diffuseTransmissionTextureTransform,
                                 3u);
    // Vulkan sRGB texture formats already decode these RGB texels to linear.
    // An authored black factor remains black even when a texture is present.
    float3 color = material.diffuseTransmissionColorFactor.rgb *
        SamplePbrTexture(
            material.diffuseTransmissionColorTextureIndex,
            ApplyGpuTextureTransform(
                texCoord0, texCoord1,
                material.diffuseTransmissionColorTextureTransform),
            1.0.xxxx).rgb;
    return MakeExtendedPbrTerms(tangent, strength, color, factor,
                                specularTransmissionFactor);
}

float3 ExtendedPbrShadowBiasNormal(
    float3 geometricNormal, float3 surfaceToLight, ExtendedPbrTerms terms)
{
    // Explicit backside diffuse light needs the same shadow tests as front
    // light, with the receiver offset toward its emitter. Preserve the exact
    // old normal when diffuse transmission is disabled (including sampled 0).
    return terms.diffuseTransmissionFactor > 0.0 &&
           dot(geometricNormal, surfaceToLight) < 0.0
        ? -geometricNormal : geometricNormal;
}
