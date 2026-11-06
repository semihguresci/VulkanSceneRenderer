#pragma once
#include "brdf_common.slang"

// Fully sampled, scene-linear inputs. This type deliberately has no material
// buffer, descriptor, texture, or alpha/coverage dependencies.
struct ExtendedPbrTerms
{
    float3 anisotropyTangent;
    float anisotropyStrength;
    float3 diffuseTransmissionColor;
    float diffuseTransmissionFactor;
    float specularTransmissionFactor;
};

ExtendedPbrTerms MakeExtendedPbrTerms(float3 tangent, float strength,
                                     float3 color, float factor,
                                     float specularTransmission)
{
    ExtendedPbrTerms terms;
    terms.anisotropyTangent = Finite3Or(tangent, float3(1.0, 0.0, 0.0));
    terms.anisotropyStrength = saturate(FiniteScalarOr(strength, 0.0));
    terms.diffuseTransmissionColor = saturate(Finite3Or(color, 1.0.xxx));
    terms.diffuseTransmissionFactor = saturate(FiniteScalarOr(factor, 0.0));
    terms.specularTransmissionFactor = saturate(FiniteScalarOr(specularTransmission, 0.0));
    return terms;
}

bool HasExtendedPbrEffect(ExtendedPbrTerms terms)
{
    // Black transmitted color still takes energy away from reflected diffuse.
    return FiniteScalarOr(terms.anisotropyStrength, 0.0) > 0.0 ||
           FiniteScalarOr(terms.diffuseTransmissionFactor, 0.0) > 0.0;
}

float4 EncodeExtendedPbrDiffuseStorage(ExtendedPbrTerms terms)
{
    // Premultiplication makes hardware MSAA resolve average color*factor once,
    // including uncovered and ordinary-material samples cleared to zero.
    return float4(terms.diffuseTransmissionColor * terms.diffuseTransmissionFactor,
                  terms.diffuseTransmissionFactor);
}

float3 DecodeExtendedPbrDiffuseStorageColor(float4 storage)
{
    return isfinite(storage.w) && storage.w > 0.0
        ? max(Finite3Or(storage.rgb, 0.0.xxx), 0.0.xxx) / storage.w
        : 0.0.xxx;
}

float ExtendedPbrDiffuseReflectionWeight(ExtendedPbrTerms terms)
{
    if (!HasExtendedPbrEffect(terms)) return 1.0;
    float transmission = saturate(FiniteScalarOr(terms.specularTransmissionFactor, 0.0));
    float diffuse = saturate(FiniteScalarOr(terms.diffuseTransmissionFactor, 0.0));
    return (1.0 - transmission) * (1.0 - diffuse);
}

float ExtendedPbrDiffuseTransmissionWeight(ExtendedPbrTerms terms)
{
    float transmission = saturate(FiniteScalarOr(terms.specularTransmissionFactor, 0.0));
    float diffuse = saturate(FiniteScalarOr(terms.diffuseTransmissionFactor, 0.0));
    return (1.0 - transmission) * diffuse;
}

float3 ExtendedPbrIblNormal(float3 N, float3 V, float roughness, ExtendedPbrTerms terms)
{
    float strength = saturate(FiniteScalarOr(terms.anisotropyStrength, 0.0));
    // Preserve the exact caller normal and isotropic reflection when disabled.
    if (strength <= 0.0) return N;
    float3 normal = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    float3 view = SafeNormalize(V, normal);
    float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 fallbackTangent = SafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
    float3 tangent = Finite3Or(terms.anisotropyTangent, fallbackTangent);
    tangent = SafeNormalize(tangent - normal * dot(normal, tangent), fallbackTangent);
    float3 direction = cross(normal, tangent);
    float3 anisotropicNormal = cross(cross(direction, view), direction);
    float bend = 1.0 - strength * (1.0 - saturate(FiniteScalarOr(roughness, 1.0)));
    float bend2 = bend * bend;
    // Khronos's approximation mixes the unnormalized projected view vector.
    // Normalizing it first changes the lobe when V is close to direction.
    float3 mixed = lerp(anisotropicNormal, normal, bend2 * bend2);
    float largest = max(max(abs(mixed.x), abs(mixed.y)), abs(mixed.z));
    if (!all(isfinite(mixed)) || largest <= 0.0) return normal;
    // Scale before normalization: a small valid mixture near V || direction
    // must not be discarded by SafeNormalize's generic length threshold.
    float3 result = SafeNormalize(mixed / largest, normal);
    return dot(result, normal) < 0.0 ? -result : result;
}

float3 EvaluateExtendedPbrDirectLight(
    float3 albedo, float3 F0, float metallic, float roughness, float3 N,
    float3 V, float3 L, float3 radiance, ExtendedPbrTerms terms)
{
    if (!HasExtendedPbrEffect(terms))
    {
        return EvaluatePbrDirectLight(albedo, F0, metallic, roughness, N, V, L, radiance);
    }
    terms = MakeExtendedPbrTerms(terms.anisotropyTangent, terms.anisotropyStrength,
        terms.diffuseTransmissionColor, terms.diffuseTransmissionFactor,
        terms.specularTransmissionFactor);
    // Only the diffuse base is split. F0 and the specular reflection lobe do
    // not change when diffuse transmission or its independent color changes.
    float residualDiffuse = 1.0 - terms.specularTransmissionFactor;
    float reflectionWeight = residualDiffuse * (1.0 - terms.diffuseTransmissionFactor);
    float3 lighting = EvaluateAnisotropicPbrDirectLight(
        max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx) * reflectionWeight,
        F0, metallic, roughness, N, V, L, radiance,
        terms.anisotropyTangent, terms.anisotropyStrength);
    if (terms.diffuseTransmissionFactor <= 0.0)
    {
        return lighting;
    }

    N = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));
    float NdotV = saturate(FiniteScalarOr(dot(N, V), 0.0));
    float signedNdotL = FiniteScalarOr(dot(N, L), 0.0);
    if (NdotV <= 0.0 || signedNdotL >= 0.0)
    {
        return lighting;
    }

    // Mirror the back-side incident direction across the surface before the
    // dielectric Fresnel evaluation, matching Khronos's punctual thin-surface
    // policy. V+L itself is degenerate for a directly backlit surface.
    float3 mirroredL = SafeNormalize(L - 2.0 * N * signedNdotL, N);
    float3 H = SafeNormalize(V + mirroredL, N);
    float3 F = FresnelSchlick(saturate(dot(H, V)), saturate(Finite3Or(F0, 0.04.xxx)));
    float weight = (1.0 - saturate(FiniteScalarOr(metallic, 0.0))) *
        residualDiffuse * terms.diffuseTransmissionFactor;
    float3 diffuseBtdf = (1.0 - F) * terms.diffuseTransmissionColor * (weight / PI);
    return lighting + diffuseBtdf * max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx) *
        saturate(-signedNdotL);
}

float3 EvaluateExtendedPbrLayeredDirectLight(
    float3 albedo, float3 F0, float metallic, float roughness, float3 N,
    float3 V, float3 L, float3 radiance, float clearcoatFactor,
    float clearcoatRoughness, float3 sheenColor, float sheenRoughness,
    ExtendedPbrTerms terms)
{
    if (!HasExtendedPbrEffect(terms))
    {
        // Retain all legacy layer ordering/rounding, including existing
        // specular-transmission behavior, while both new effects are disabled.
        return EvaluateDeferredLayeredDirectLight(albedo, F0, metallic, roughness,
            N, V, L, radiance, clearcoatFactor, clearcoatRoughness,
            sheenColor, sheenRoughness);
    }
    float3 lighting = EvaluateExtendedPbrDirectLight(
        albedo, F0, metallic, roughness, N, V, L, radiance, terms);
    lighting *= DeferredClearcoatBaseEnergyScale(clearcoatFactor);
    // The clearcoat remains the existing isotropic lobe.
    lighting += EvaluateDeferredClearcoatDirectLight(
        clearcoatFactor, clearcoatRoughness, N, V, L, radiance);
    lighting *= DeferredSheenBaseEnergyScale(sheenColor);
    lighting += EvaluateDeferredSheenDirectLight(
        sheenColor, sheenRoughness, N, V, L, radiance);
    return lighting;
}
