#pragma once
#include "material_data_common.slang"

// Opt-in diffuse transport approximation for an explicitly transmitting,
// zero-thickness smooth dielectric sheet. Rays keep their direction: no solid
// refraction, volume absorption, caustics or alpha-blend inference. Omitted
// reflected energy is conservative rather than an extra diffuse contribution.
static const uint DDGI_MAX_GLASS_CROSSINGS = 8u;
static const float DDGI_MIN_TRANSPORT_THROUGHPUT = 1e-4;

struct DdgiThinGlassResult
{
    float3 transmittance;
    uint supported;
};

DdgiThinGlassResult DdgiThinGlassTransmission(GpuMaterial material, uint flags,
    float transmission, float alpha, float metallic, float roughness,
    float3 tint, float incidenceCosine, bool enabled)
{
    DdgiThinGlassResult result = (DdgiThinGlassResult)0;
    if (!enabled || !isfinite(transmission) || transmission <= 0.0 ||
        !isfinite(material.transmissionFactor) ||
        !isfinite(alpha) || abs(alpha - 1.0) > 1e-5 ||
        !isfinite(metallic) || metallic > 1e-5 || metallic < 0.0 ||
        !isfinite(roughness) || roughness < 0.0 || roughness > 0.1 ||
        !all(isfinite(tint)) || !isfinite(incidenceCosine) ||
        !isfinite(material.ior) || material.ior < 1.0 || material.ior > 3.0 ||
        !isfinite(material.thicknessFactor) || abs(material.thicknessFactor) > 1e-7 ||
        material.thicknessTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.heightScale) ||
        (material.heightTextureIndex != kInvalidGpuMaterialTexture && abs(material.heightScale) > 1e-7) ||
        !isfinite(material.dispersion) || abs(material.dispersion) > 1e-7 ||
        !isfinite(material.specularFactor) || abs(material.specularFactor - 1.0) > 1e-5 ||
        !all(isfinite(material.specularColorFactor.rgb)) ||
        any(abs(material.specularColorFactor.rgb - 1.0.xxx) > 1e-5.xxx) ||
        material.specularTextureIndex != kInvalidGpuMaterialTexture ||
        material.specularColorTextureIndex != kInvalidGpuMaterialTexture ||
        material.normalTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.clearcoatFactor) || abs(material.clearcoatFactor) > 1e-7 ||
        material.clearcoatTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.iridescenceFactor) || abs(material.iridescenceFactor) > 1e-7 ||
        material.iridescenceTextureIndex != kInvalidGpuMaterialTexture ||
        !all(isfinite(material.sheenColorFactor.rgb)) ||
        any(abs(material.sheenColorFactor.rgb) > 1e-7.xxx) ||
        material.sheenColorTextureIndex != kInvalidGpuMaterialTexture ||
        (flags & 0x10u) != 0u)
        return result;
    float ratio = (material.ior - 1.0) / (material.ior + 1.0);
    float f0 = ratio * ratio;
    float fresnel = material.ior == 1.0 ? 0.0 :
        f0 + (1.0 - f0) * pow(1.0 - saturate(abs(incidenceCosine)), 5.0);
    result.transmittance = saturate(tint) * saturate(transmission) * (1.0 - fresnel);
    result.supported = 1u;
    return result;
}

// Returning black at the crossing cap also blocks later environment hits.
// This function is used by the real traversal and the GPU contract fixture.
bool DdgiAdvanceThinGlass(inout float3 throughput, DdgiThinGlassResult glass,
                         uint alreadyCrossed)
{
    if (glass.supported == 0u || alreadyCrossed >= DDGI_MAX_GLASS_CROSSINGS ||
        !all(isfinite(throughput)))
        return false;
    throughput *= glass.transmittance;
    return all(isfinite(throughput)) && max(max(throughput.x, throughput.y), throughput.z) >=
        DDGI_MIN_TRANSPORT_THROUGHPUT;
}

// Bounded secondary dielectric transport. Volume boundaries use their actual
// traced thickness; KHR_materials_volume explicitly requires ray tracers to
// ignore the raster thickness texture. The authored factor selects thin/solid.
// Visible GGX normals sample rough interfaces. We omit non-TIR reflected paths
// and their caustics, but preserve total internal reflection inside a volume.
struct DdgiDielectricResult
{
    float3 direction;
    float3 weight;
    uint supported;
    uint volume;
    uint totalInternalReflection;
};

float DdgiDielectricFresnel(float incidentCosine, float etaIncident, float etaTransmitted)
{
    if (etaIncident == etaTransmitted) return 0.0;
    float ci = saturate(incidentCosine);
    float eta = etaIncident / etaTransmitted;
    float sinTransmittedSquared = eta * eta * max(0.0, 1.0 - ci * ci);
    if (sinTransmittedSquared >= 1.0) return 1.0;
    float ct = sqrt(max(0.0, 1.0 - sinTransmittedSquared));
    float parallel = (etaTransmitted * ci - etaIncident * ct) /
        max(etaTransmitted * ci + etaIncident * ct, 1e-20);
    float perpendicular = (etaIncident * ci - etaTransmitted * ct) /
        max(etaIncident * ci + etaTransmitted * ct, 1e-20);
    return saturate(0.5 * (parallel * parallel + perpendicular * perpendicular));
}

bool DdgiRefract(float3 direction, float3 opposingNormal, float etaIncident,
                float etaTransmitted, out float3 transmitted)
{
    transmitted = 0.0.xxx;
    if (!all(isfinite(direction)) || !all(isfinite(opposingNormal)) ||
        !isfinite(etaIncident) || !isfinite(etaTransmitted) ||
        etaIncident <= 0.0 || etaTransmitted <= 0.0) return false;
    if (etaIncident == etaTransmitted)
    {
        transmitted = direction;
        return true;
    }
    float cosine = saturate(-dot(direction, opposingNormal));
    float eta = etaIncident / etaTransmitted;
    float discriminant = 1.0 - eta * eta * max(0.0, 1.0 - cosine * cosine);
    if (discriminant <= 0.0) return false;
    transmitted = eta * direction + (eta * cosine - sqrt(discriminant)) * opposingNormal;
    return all(isfinite(transmitted)) && dot(transmitted, transmitted) > 1e-10;
}

float DdgiSmithG1(float cosine, float alpha)
{
    if (cosine <= 0.0) return 0.0;
    float cosineSquared = cosine * cosine;
    return 2.0 / (1.0 + sqrt(1.0 + alpha * alpha *
        max(0.0, 1.0 - cosineSquared) / max(cosineSquared, 1e-20)));
}

float3 DdgiVisibleGgxNormal(float3 opposingNormal, float3 incidentView,
                          float alpha, float2 random)
{
    float3 axis = abs(opposingNormal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 tangent = normalize(cross(axis, opposingNormal));
    float3 bitangent = cross(opposingNormal, tangent);
    float3 view = float3(dot(incidentView, tangent), dot(incidentView, bitangent),
                        dot(incidentView, opposingNormal));
    float3 stretchedView = normalize(float3(alpha * view.xy, max(view.z, 1e-7)));
    float lensSquared = dot(stretchedView.xy, stretchedView.xy);
    float3 t1 = lensSquared > 1e-12 ?
        float3(-stretchedView.y, stretchedView.x, 0) * rsqrt(lensSquared) : float3(1, 0, 0);
    float3 t2 = cross(stretchedView, t1);
    float radius = sqrt(random.x), phi = 6.283185307179586 * random.y;
    float p1 = radius * cos(phi), p2 = radius * sin(phi);
    p2 = lerp(sqrt(max(0.0, 1.0 - p1 * p1)), p2, (1.0 + stretchedView.z) * 0.5);
    float3 normal = p1 * t1 + p2 * t2 +
        sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * stretchedView;
    normal = normalize(float3(alpha * normal.xy, max(normal.z, 1e-7)));
    return normalize(tangent * normal.x + bitangent * normal.y + opposingNormal * normal.z);
}

// Infinity is the glTF default attenuation distance. Invalid authored inputs
// remain unsupported rather than becoming clear glass. Zero color extinguishes
// its channel exactly, including distances smaller than the reference distance.
bool DdgiVolumeAbsorption(GpuMaterial material, float distance, out float3 absorption)
{
    absorption = 0.0.xxx;
    if (!isfinite(distance) || distance < 0.0 ||
        !all(isfinite(material.attenuationColor.rgb)) ||
        any(material.attenuationColor.rgb < 0.0.xxx) ||
        any(material.attenuationColor.rgb > 1.0.xxx) ||
        isnan(material.attenuationDistance) || material.attenuationDistance <= 0.0)
        return false;
    absorption = 1.0.xxx;
    if (distance == 0.0 || !isfinite(material.attenuationDistance)) return true;
    float exponent = distance / material.attenuationDistance;
    // An extremely dense valid medium can overflow x/d. Evaluate that limit
    // explicitly so a white channel remains one rather than 0*infinity = NaN.
    if (!isfinite(exponent))
    {
        absorption = float3(material.attenuationColor.r == 1.0 ? 1.0 : 0.0,
                            material.attenuationColor.g == 1.0 ? 1.0 : 0.0,
                            material.attenuationColor.b == 1.0 ? 1.0 : 0.0);
        return true;
    }
    absorption = exp(log(max(material.attenuationColor.rgb, 1e-30.xxx)) * exponent);
    if (material.attenuationColor.r == 0.0) absorption.r = 0.0;
    if (material.attenuationColor.g == 0.0) absorption.g = 0.0;
    if (material.attenuationColor.b == 0.0) absorption.b = 0.0;
    return all(isfinite(absorption));
}

DdgiDielectricResult DdgiSampleDielectric(GpuMaterial material, uint flags,
    float transmission, float alpha, float metallic, float roughness, float3 tint,
    float3 direction, float3 geometricNormal, bool entering, float2 random, bool enabled)
{
    DdgiDielectricResult result = (DdgiDielectricResult)0;
    if (!enabled || !isfinite(material.transmissionFactor) ||
        !isfinite(transmission) || transmission < 0.0 || transmission > 1.0 ||
        !isfinite(alpha) || abs(alpha - 1.0) > 1e-5 ||
        !isfinite(metallic) || metallic > 1e-5 || metallic < 0.0 ||
        !isfinite(roughness) || roughness < 0.0 || roughness > 1.0 ||
        !all(isfinite(tint)) || !all(isfinite(direction)) ||
        abs(dot(direction, direction) - 1.0) > 1e-3 ||
        !all(isfinite(geometricNormal)) || abs(dot(geometricNormal, geometricNormal) - 1.0) > 1e-3 ||
        !all(isfinite(random)) || any(random < 0.0.xx) || any(random >= 1.0.xx) ||
        !isfinite(material.ior) || material.ior < 1.0 || material.ior > 3.0 ||
        !isfinite(material.thicknessFactor) || material.thicknessFactor < 0.0 ||
        !isfinite(material.heightScale) ||
        (material.heightTextureIndex != kInvalidGpuMaterialTexture && abs(material.heightScale) > 1e-7) ||
        !isfinite(material.dispersion) || abs(material.dispersion) > 1e-7 ||
        !isfinite(material.specularFactor) || abs(material.specularFactor - 1.0) > 1e-5 ||
        !all(isfinite(material.specularColorFactor.rgb)) ||
        any(abs(material.specularColorFactor.rgb - 1.0.xxx) > 1e-5.xxx) ||
        material.specularTextureIndex != kInvalidGpuMaterialTexture ||
        material.specularColorTextureIndex != kInvalidGpuMaterialTexture ||
        material.normalTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.clearcoatFactor) || abs(material.clearcoatFactor) > 1e-7 ||
        material.clearcoatTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.iridescenceFactor) || abs(material.iridescenceFactor) > 1e-7 ||
        material.iridescenceTextureIndex != kInvalidGpuMaterialTexture ||
        !all(isfinite(material.sheenColorFactor.rgb)) ||
        any(abs(material.sheenColorFactor.rgb) > 1e-7.xxx) ||
        material.sheenColorTextureIndex != kInvalidGpuMaterialTexture ||
        (flags & 0x10u) != 0u) return result;
    result.volume = material.thicknessFactor > 0.0 ? 1u : 0u;
    if ((entering || result.volume == 0u) && transmission <= 0.0) return result;
    float3 absorption;
    if (result.volume != 0u && !DdgiVolumeAbsorption(material, 0.0, absorption)) return result;
    float3 normal = entering ? geometricNormal : -geometricNormal;
    float macroIncidence = dot(-direction, normal);
    if (macroIncidence < 0.0 || (macroIncidence <= 1e-7 && material.ior != 1.0)) return result;
    if (material.ior == 1.0)
    {
        result.direction = direction;
        result.weight = entering || result.volume == 0u ? saturate(tint) * transmission : 1.0.xxx;
        result.supported = 1u;
        return result;
    }
    float ggxAlpha = max(roughness * roughness, 1e-4);
    float3 microNormal = roughness <= 1e-4 ? normal :
        DdgiVisibleGgxNormal(normal, -direction, ggxAlpha, random);
    float etaIncident = result.volume != 0u && !entering ? material.ior : 1.0;
    float etaTransmitted = result.volume != 0u && !entering ? 1.0 : material.ior;
    float incidentCosine = saturate(dot(-direction, microNormal));
    float fresnel = DdgiDielectricFresnel(incidentCosine, etaIncident, etaTransmitted);
    if (result.volume == 0u)
        result.direction = reflect(-direction, microNormal);
    else if (!DdgiRefract(direction, microNormal, etaIncident, etaTransmitted, result.direction))
    {
        result.direction = reflect(direction, microNormal);
        if (dot(result.direction, normal) <= 0.0) return (DdgiDielectricResult)0;
        result.totalInternalReflection = 1u;
        result.weight = DdgiSmithG1(dot(result.direction, normal), ggxAlpha).xxx;
        result.supported = 1u;
        return result;
    }
    // Discard sampled normals that send transmission into the wrong macro
    // hemisphere. Keeping that probability mass black is conservative.
    if (!all(isfinite(result.direction)) || dot(result.direction, normal) >= 0.0)
        return (DdgiDielectricResult)0;
    float geometry = DdgiSmithG1(-dot(result.direction, normal), ggxAlpha);
    result.weight = ((1.0 - fresnel) * geometry).xxx;
    if (entering || result.volume == 0u)
        result.weight *= saturate(tint) * transmission;
    if (result.volume != 0u)
        result.weight *= (etaIncident * etaIncident) / (etaTransmitted * etaTransmitted);
    result.direction = normalize(result.direction);
    result.supported = all(isfinite(result.weight)) ? 1u : 0u;
    return result;
}
