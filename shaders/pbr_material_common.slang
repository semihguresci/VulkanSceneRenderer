#pragma once

#include "brdf_common.slang"

static const uint kInvalidMaterialTexture = 0xffffffffu;
static const uint kPbrObjectFlagSpecularGlossiness = 0x8u;
static const uint kPbrObjectFlagUnlit = 0x10u;

float4 SamplePbrTexture(uint textureIndex, float2 texCoord, float4 fallback)
{
    if (textureIndex == kInvalidMaterialTexture)
    {
        return fallback;
    }

    uint texIndex = NonUniformResourceIndex(textureIndex);
    uint samplerIndex = NonUniformResourceIndex(
        min(uTextureMetadata[texIndex].samplerIndex,
            MATERIAL_SAMPLER_DESCRIPTOR_COUNT - 1u));
    return materialTextures[texIndex].Sample(
        materialSamplers[samplerIndex], texCoord);
}

float4 SamplePbrTextureLevel(uint textureIndex, float2 texCoord, float lod,
                             float4 fallback)
{
    if (textureIndex == kInvalidMaterialTexture)
    {
        return fallback;
    }

    uint texIndex = NonUniformResourceIndex(textureIndex);
    uint samplerIndex = NonUniformResourceIndex(
        min(uTextureMetadata[texIndex].samplerIndex,
            MATERIAL_SAMPLER_DESCRIPTOR_COUNT - 1u));
    return materialTextures[texIndex].SampleLevel(
        materialSamplers[samplerIndex], texCoord, lod);
}

float TextureLuminance(float3 value)
{
    return dot(value, float3(0.2126, 0.7152, 0.0722));
}

float MaxComponent(float3 value)
{
    return max(max(value.x, value.y), value.z);
}

float TextureBackedScalarFactor(float factor, uint textureIndex)
{
    float value = factor;
    if (textureIndex != kInvalidMaterialTexture && value <= 0.0)
    {
        value = 1.0;
    }
    return value;
}

float3 TextureBackedColorFactor(float3 factor, uint textureIndex)
{
    float3 value = max(factor, 0.0.xxx);
    if (textureIndex != kInvalidMaterialTexture && MaxComponent(value) <= 0.0)
    {
        value = 1.0.xxx;
    }
    return value;
}

float EvaluateOpacity(float baseAlpha, uint opacityTextureIndex,
                      float opacityFactor, float2 texCoord,
                      GpuTextureTransform opacityTextureTransform)
{
    float alpha = baseAlpha * saturate(opacityFactor);
    if (opacityTextureIndex != kInvalidMaterialTexture)
    {
        alpha *= SelectGpuTextureChannel(
            SamplePbrTexture(opacityTextureIndex, texCoord, 1.0.xxxx),
            opacityTextureTransform, 0u);
    }
    return saturate(alpha);
}

float EvaluateMetalness(float factor, uint metallicRoughnessTextureIndex,
                        uint metalnessTextureIndex,
                        float2 metallicRoughnessTexCoord,
                        float2 metalnessTexCoord,
                        GpuTextureTransform metalnessTextureTransform)
{
    float metalness = factor;
    if (metallicRoughnessTextureIndex != kInvalidMaterialTexture)
    {
        metalness *= SamplePbrTexture(
            metallicRoughnessTextureIndex, metallicRoughnessTexCoord,
            1.0.xxxx).b;
    }
    if (metalnessTextureIndex != kInvalidMaterialTexture)
    {
        metalness *= SelectGpuTextureChannel(
            SamplePbrTexture(
                metalnessTextureIndex, metalnessTexCoord, 1.0.xxxx),
            metalnessTextureTransform, 0u);
    }
    return saturate(metalness);
}

float EvaluateRoughness(float factor, uint metallicRoughnessTextureIndex,
                        uint roughnessTextureIndex,
                        float2 metallicRoughnessTexCoord,
                        float2 roughnessTexCoord,
                        GpuTextureTransform roughnessTextureTransform,
                        uint objectFlags)
{
    if ((objectFlags & kPbrObjectFlagSpecularGlossiness) != 0u)
    {
        float glossiness = factor;
        if (roughnessTextureIndex != kInvalidMaterialTexture)
        {
            glossiness *= SelectGpuTextureChannel(
                SamplePbrTexture(
                    roughnessTextureIndex, roughnessTexCoord, 1.0.xxxx),
                roughnessTextureTransform, 3u);
        }
        return clamp(1.0 - saturate(glossiness), 0.045, 1.0);
    }

    float roughness = factor;
    if (metallicRoughnessTextureIndex != kInvalidMaterialTexture)
    {
        roughness *= SamplePbrTexture(
            metallicRoughnessTextureIndex, metallicRoughnessTexCoord,
            1.0.xxxx).g;
    }
    if (roughnessTextureIndex != kInvalidMaterialTexture)
    {
        roughness *= SelectGpuTextureChannel(
            SamplePbrTexture(
                roughnessTextureIndex, roughnessTexCoord, 1.0.xxxx),
            roughnessTextureTransform, 0u);
    }
    return clamp(roughness, 0.045, 1.0);
}

float EvaluateSpecularFactor(float factor, uint specularTextureIndex,
                             float2 specularTexCoord,
                             GpuTextureTransform specularTextureTransform)
{
    float specular = factor;
    if (specularTextureIndex != kInvalidMaterialTexture)
    {
        float4 sample = SamplePbrTexture(
            specularTextureIndex, specularTexCoord, 1.0.xxxx);
        specular *= SelectGpuTextureChannel(
            sample, specularTextureTransform, 3u);
    }
    return saturate(specular);
}

float3 EvaluateSpecularColor(float3 factor, uint specularColorTextureIndex,
                             float2 specularColorTexCoord,
                             uint specularTextureIndex,
                             float2 specularTexCoord)
{
    float3 color = factor;
    if (specularColorTextureIndex != kInvalidMaterialTexture)
    {
        color *= SamplePbrTexture(
            specularColorTextureIndex, specularColorTexCoord, 1.0.xxxx).rgb;
    }
    else if (specularTextureIndex != kInvalidMaterialTexture)
    {
        color *= SamplePbrTexture(
            specularTextureIndex, specularTexCoord, 1.0.xxxx).rgb;
    }
    return max(color, 0.0.xxx);
}

float EvaluateTransmissionFactor(float factor, uint transmissionTextureIndex,
                                 float2 texCoord,
                                 GpuTextureTransform transmissionTextureTransform)
{
    float transmission =
        TextureBackedScalarFactor(factor, transmissionTextureIndex);
    if (transmissionTextureIndex != kInvalidMaterialTexture)
    {
        transmission *= SelectGpuTextureChannel(
            SamplePbrTexture(
                transmissionTextureIndex, texCoord, 1.0.xxxx),
            transmissionTextureTransform, 0u);
    }
    return saturate(transmission);
}

float EvaluateClearcoatFactor(float factor, uint clearcoatTextureIndex,
                              float2 texCoord,
                              GpuTextureTransform clearcoatTextureTransform)
{
    float clearcoat =
        TextureBackedScalarFactor(factor, clearcoatTextureIndex);
    if (clearcoatTextureIndex != kInvalidMaterialTexture)
    {
        clearcoat *= SelectGpuTextureChannel(
            SamplePbrTexture(
                clearcoatTextureIndex, texCoord, 1.0.xxxx),
            clearcoatTextureTransform, 0u);
    }
    return saturate(clearcoat);
}

float EvaluateClearcoatRoughness(float factor,
                                 uint clearcoatRoughnessTextureIndex,
                                 float2 texCoord,
                                 GpuTextureTransform clearcoatRoughnessTextureTransform)
{
    float roughness =
        TextureBackedScalarFactor(factor, clearcoatRoughnessTextureIndex);
    if (clearcoatRoughnessTextureIndex != kInvalidMaterialTexture)
    {
        roughness *= SelectGpuTextureChannel(
            SamplePbrTexture(
                clearcoatRoughnessTextureIndex, texCoord, 1.0.xxxx),
            clearcoatRoughnessTextureTransform, 1u);
    }
    return clamp(roughness, 0.045, 1.0);
}

float EvaluateThickness(float factor, uint thicknessTextureIndex,
                        float2 texCoord,
                        GpuTextureTransform thicknessTextureTransform)
{
    float thickness = max(factor, 0.0);
    if (thicknessTextureIndex != kInvalidMaterialTexture)
    {
        thickness *= SelectGpuTextureChannel(
            SamplePbrTexture(
                thicknessTextureIndex, texCoord, 1.0.xxxx),
            thicknessTextureTransform, 1u);
    }
    return max(thickness, 0.0);
}

float3 EvaluateSheenColor(float3 factor, uint sheenColorTextureIndex,
                          float2 texCoord)
{
    float3 color =
        TextureBackedColorFactor(factor, sheenColorTextureIndex);
    if (sheenColorTextureIndex != kInvalidMaterialTexture)
    {
        color *= SamplePbrTexture(
            sheenColorTextureIndex, texCoord, 1.0.xxxx).rgb;
    }
    return color;
}

float EvaluateSheenRoughness(float factor, uint sheenRoughnessTextureIndex,
                             float2 texCoord,
                             GpuTextureTransform sheenRoughnessTextureTransform)
{
    float roughness =
        TextureBackedScalarFactor(factor, sheenRoughnessTextureIndex);
    if (sheenRoughnessTextureIndex != kInvalidMaterialTexture)
    {
        roughness *= SelectGpuTextureChannel(
            SamplePbrTexture(
                sheenRoughnessTextureIndex, texCoord, 1.0.xxxx),
            sheenRoughnessTextureTransform, 3u);
    }
    return clamp(roughness, 0.045, 1.0);
}

float EvaluateIridescenceFactor(float factor, uint iridescenceTextureIndex,
                                float2 texCoord,
                                GpuTextureTransform iridescenceTextureTransform)
{
    float iridescence =
        TextureBackedScalarFactor(factor, iridescenceTextureIndex);
    if (iridescenceTextureIndex != kInvalidMaterialTexture)
    {
        iridescence *= SelectGpuTextureChannel(
            SamplePbrTexture(
                iridescenceTextureIndex, texCoord, 1.0.xxxx),
            iridescenceTextureTransform, 0u);
    }
    return saturate(iridescence);
}

float EvaluateIridescenceThickness(float minimumThickness,
                                   float maximumThickness,
                                   uint thicknessTextureIndex,
                                   float2 texCoord,
                                   GpuTextureTransform thicknessTextureTransform)
{
    float thicknessWeight = 1.0;
    if (thicknessTextureIndex != kInvalidMaterialTexture)
    {
        thicknessWeight = SelectGpuTextureChannel(
            SamplePbrTexture(
                thicknessTextureIndex, texCoord, 1.0.xxxx),
            thicknessTextureTransform, 1u);
    }
    return lerp(minimumThickness, maximumThickness, saturate(thicknessWeight));
}

float3 ApplyHeightDisplacement(float3 objectPosition, float3 objectNormal,
                               float2 texCoord, uint heightTextureIndex,
                               GpuTextureTransform heightTextureTransform,
                               float heightScale, float heightOffset)
{
    if (heightTextureIndex == kInvalidMaterialTexture ||
        abs(heightScale) <= 1e-7)
    {
        return objectPosition;
    }

    float height = SelectGpuTextureChannel(
        SamplePbrTextureLevel(heightTextureIndex, texCoord, 0.0, 0.5.xxxx),
        heightTextureTransform, 0u);
    float3 normal = SafeNormalize(objectNormal, float3(0.0, 1.0, 0.0));
    return objectPosition + normal * ((height + heightOffset) * heightScale);
}

float3 ApplyBeerLambert(float3 color, float3 attenuationColor,
                        float attenuationDistance, float distance)
{
    if (!all(isfinite(attenuationColor)) || attenuationDistance <= 0.0 ||
        !isfinite(attenuationDistance) || distance <= 0.0)
    {
        return color;
    }

    float3 safeColor = max(attenuationColor, 0.0001.xxx);
    float3 transmittance = exp(log(safeColor) * (distance / attenuationDistance));
    return color * saturate(transmittance);
}

float3 ApproximateIridescenceTint(float factor, float ior, float thicknessNm,
                                  float NdotV)
{
    if (factor <= 0.0)
    {
        return 1.0.xxx;
    }

    float phase = thicknessNm * 0.018 + ior * 1.7 + (1.0 - NdotV) * 4.0;
    float3 wave = 0.5.xxx + 0.5 * cos(phase.xxx + float3(0.0, 2.094, 4.188));
    return lerp(1.0.xxx, 0.65.xxx + 0.7 * wave, saturate(factor));
}
