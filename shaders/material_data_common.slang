#pragma once

// Must match container::gpu::kMaterialTextureDescriptorCapacity.
static const uint MATERIAL_TEXTURE_DESCRIPTOR_COUNT = 4096u;
// Must match container::gpu::kMaterialSamplerDescriptorCapacity.
static const uint MATERIAL_SAMPLER_DESCRIPTOR_COUNT = 9u;
static const uint kInvalidGpuMaterialTexture = 0xffffffffu;
// Float metadata stores materialIndex plus a 0.5 thin-surface bit. Keep the
// material index range below 2^23 so the integer and half-bit round-trip in
// float32.
static const uint MAX_EXACT_GBUFFER_MATERIAL_INDEX = 8388607u;
static const uint kGpuMaterialFlagAlphaMask = 0x1u;
static const uint kGpuMaterialFlagAlphaBlend = 0x2u;
static const uint kGpuMaterialFlagDoubleSided = 0x4u;
static const float kGBufferMaterialThinSurfaceBit = 0.5;
static const uint kBimSemanticColorModeOff = 0u;

struct GpuTextureTransform
{
    // row0.w stores the source texture coordinate set as a small exact float.
    // row1.w stores a scalar channel override: 0-3 = RGBA, >3 = slot default.
    float4 row0;
    float4 row1;
};

struct GpuTextureMetadata
{
    uint samplerIndex;
    uint padding0;
    uint padding1;
    uint padding2;
};

struct GpuMaterial
{
    // Must match container::gpu::GpuMaterial in include/Container/utility/SceneData.h.
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    float alphaCutoff;
    float normalTextureScale;
    float occlusionStrength;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint roughnessTextureIndex;
    uint metalnessTextureIndex;
    uint specularTextureIndex;
    uint heightTextureIndex;
    uint opacityTextureIndex;
    uint transmissionTextureIndex;
    uint specularColorTextureIndex;
    uint clearcoatTextureIndex;
    uint clearcoatRoughnessTextureIndex;
    uint clearcoatNormalTextureIndex;
    uint thicknessTextureIndex;
    uint sheenColorTextureIndex;
    uint sheenRoughnessTextureIndex;
    uint iridescenceTextureIndex;
    uint iridescenceThicknessTextureIndex;
    uint flags;
    float opacityFactor;
    float specularFactor;
    float heightScale;
    float heightOffset;
    float transmissionFactor;
    float ior;
    float dispersion;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float clearcoatNormalTextureScale;
    float thicknessFactor;
    float attenuationDistance;
    float sheenRoughnessFactor;
    float iridescenceFactor;
    float iridescenceIor;
    float iridescenceThicknessMinimum;
    float iridescenceThicknessMaximum;
    float4 specularColorFactor;
    float4 attenuationColor;
    float4 sheenColorFactor;
    GpuTextureTransform baseColorTextureTransform;
    GpuTextureTransform normalTextureTransform;
    GpuTextureTransform occlusionTextureTransform;
    GpuTextureTransform emissiveTextureTransform;
    GpuTextureTransform metallicRoughnessTextureTransform;
    GpuTextureTransform roughnessTextureTransform;
    GpuTextureTransform metalnessTextureTransform;
    GpuTextureTransform specularTextureTransform;
    GpuTextureTransform heightTextureTransform;
    GpuTextureTransform opacityTextureTransform;
    GpuTextureTransform transmissionTextureTransform;
    GpuTextureTransform specularColorTextureTransform;
    GpuTextureTransform clearcoatTextureTransform;
    GpuTextureTransform clearcoatRoughnessTextureTransform;
    GpuTextureTransform clearcoatNormalTextureTransform;
    GpuTextureTransform thicknessTextureTransform;
    GpuTextureTransform sheenColorTextureTransform;
    GpuTextureTransform sheenRoughnessTextureTransform;
    GpuTextureTransform iridescenceTextureTransform;
    GpuTextureTransform iridescenceThicknessTextureTransform;
};

struct DeferredMaterialLayerTerms
{
    float transmissionFactor;
    float clearcoatFactor;
    float clearcoatRoughness;
    float3 sheenColor;
    float sheenRoughness;
    float iridescenceFactor;
    float iridescenceThickness;
};

bool HasGpuMaterialTexture(uint textureIndex)
{
    return textureIndex != kInvalidGpuMaterialTexture;
}

float3 EvaluateBimSemanticColor(uint semanticId, uint semanticColorMode)
{
    if (semanticColorMode == kBimSemanticColorModeOff)
    {
        return 1.0.xxx;
    }
    if (semanticId == 0u)
    {
        return float3(0.55, 0.57, 0.60);
    }

    const uint paletteIndex =
        (semanticId - 1u + semanticColorMode * 3u) % 14u;
    switch (paletteIndex)
    {
    case 0u:
        return float3(0.16, 0.47, 0.86);
    case 1u:
        return float3(0.86, 0.29, 0.24);
    case 2u:
        return float3(0.13, 0.58, 0.39);
    case 3u:
        return float3(0.88, 0.58, 0.16);
    case 4u:
        return float3(0.55, 0.36, 0.80);
    case 5u:
        return float3(0.10, 0.65, 0.72);
    case 6u:
        return float3(0.82, 0.33, 0.58);
    case 7u:
        return float3(0.45, 0.52, 0.22);
    case 8u:
        return float3(0.93, 0.42, 0.18);
    case 9u:
        return float3(0.22, 0.62, 0.93);
    case 10u:
        return float3(0.62, 0.43, 0.25);
    case 11u:
        return float3(0.42, 0.46, 0.58);
    case 12u:
        return float3(0.70, 0.28, 0.42);
    default:
        return float3(0.34, 0.68, 0.51);
    }
}

float MaxGpuMaterialComponent(float3 value)
{
    return max(max(value.x, value.y), value.z);
}

float ResolveDeferredTextureBackedFactor(float factor, uint textureIndex)
{
    float value = saturate(factor);
    if (HasGpuMaterialTexture(textureIndex) && value <= 0.0)
    {
        value = 1.0;
    }
    return value;
}

float ResolveDeferredTextureBackedRoughness(float factor, uint textureIndex)
{
    float value = factor;
    if (HasGpuMaterialTexture(textureIndex) && value <= 0.0)
    {
        value = 0.5;
    }
    return clamp(value, 0.045, 1.0);
}

float3 ResolveDeferredTextureBackedColor(float3 factor, uint textureIndex)
{
    float3 value = max(factor, 0.0.xxx);
    if (HasGpuMaterialTexture(textureIndex) &&
        MaxGpuMaterialComponent(value) <= 0.0)
    {
        value = 1.0.xxx;
    }
    return value;
}

DeferredMaterialLayerTerms ResolveDeferredMaterialLayerTerms(GpuMaterial material)
{
    DeferredMaterialLayerTerms terms;
    terms.transmissionFactor = ResolveDeferredTextureBackedFactor(
        material.transmissionFactor, material.transmissionTextureIndex);
    terms.clearcoatFactor = ResolveDeferredTextureBackedFactor(
        material.clearcoatFactor, material.clearcoatTextureIndex);
    terms.clearcoatRoughness = ResolveDeferredTextureBackedRoughness(
        material.clearcoatRoughnessFactor,
        material.clearcoatRoughnessTextureIndex);
    terms.sheenColor = ResolveDeferredTextureBackedColor(
        material.sheenColorFactor.rgb, material.sheenColorTextureIndex);
    terms.sheenRoughness = ResolveDeferredTextureBackedRoughness(
        material.sheenRoughnessFactor, material.sheenRoughnessTextureIndex);
    terms.iridescenceFactor = ResolveDeferredTextureBackedFactor(
        material.iridescenceFactor, material.iridescenceTextureIndex);
    terms.iridescenceThickness = material.iridescenceThicknessMaximum;
    return terms;
}

DeferredMaterialLayerTerms DecodeDeferredMaterialLayerTerms(
    GpuMaterial material, float4 layers, float4 sheen)
{
    // Texture sampling and UV transforms happen once in the geometry pass.
    // Lighting must use the sampled values, rather than texture-presence
    // guesses from the material record.
    DeferredMaterialLayerTerms terms;
    terms.transmissionFactor = ResolveDeferredTextureBackedFactor(
        material.transmissionFactor, material.transmissionTextureIndex);
    terms.clearcoatFactor = saturate(isfinite(layers.x) ? layers.x : 0.0);
    terms.clearcoatRoughness = clamp(isfinite(layers.y) ? layers.y : 1.0, 0.045, 1.0);
    terms.iridescenceFactor = saturate(isfinite(layers.z) ? layers.z : 0.0);
    terms.iridescenceThickness = max(isfinite(layers.w) ? layers.w : 0.0, 0.0);
    terms.sheenColor = all(isfinite(sheen.rgb)) ? max(sheen.rgb, 0.0.xxx) : 0.0.xxx;
    terms.sheenRoughness = clamp(isfinite(sheen.w) ? sheen.w : 1.0, 0.045, 1.0);
    return terms;
}

float2 SelectGpuTextureCoord(float2 texCoord0, float2 texCoord1,
                             GpuTextureTransform transform)
{
    return transform.row0.w >= 0.5 ? texCoord1 : texCoord0;
}

float2 ApplyGpuTextureTransform(float2 texCoord0, float2 texCoord1,
                                GpuTextureTransform transform)
{
    float2 texCoord = SelectGpuTextureCoord(texCoord0, texCoord1, transform);
    return float2(dot(transform.row0.xy, texCoord) + transform.row0.z,
                  dot(transform.row1.xy, texCoord) + transform.row1.z);
}

float SelectTextureChannel(float4 value, uint channel)
{
    if (channel == 1u)
    {
        return value.g;
    }
    if (channel == 2u)
    {
        return value.b;
    }
    if (channel == 3u)
    {
        return value.a;
    }
    return value.r;
}

float SelectGpuTextureChannel(float4 value, GpuTextureTransform transform,
                              uint fallbackChannel)
{
    uint authoredChannel = uint(round(max(transform.row1.w, 0.0)));
    return SelectTextureChannel(
        value, authoredChannel <= 3u ? authoredChannel : fallbackChannel);
}

bool IsThinSurfaceGpuMaterial(uint flags)
{
    return (flags & kGpuMaterialFlagDoubleSided) != 0u &&
        (flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) != 0u;
}

float EncodeGBufferMaterialMetadata(uint materialIndex, bool thinSurface)
{
    return float(min(materialIndex, MAX_EXACT_GBUFFER_MATERIAL_INDEX)) +
        (thinSurface ? kGBufferMaterialThinSurfaceBit : 0.0);
}

uint DecodeGBufferMaterialIndex(float encodedMetadata)
{
    return uint(floor(max(encodedMetadata, 0.0)));
}

bool DecodeGBufferThinSurface(float encodedMetadata)
{
    return frac(max(encodedMetadata, 0.0)) >= 0.25;
}
