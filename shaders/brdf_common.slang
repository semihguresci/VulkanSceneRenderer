// brdf_common.slang — Shared PBR BRDF utilities.
// Included by deferred_directional, point_light, gbuffer_composite, etc.
#pragma once

static const float PI = 3.14159265359;
static const float kShadowedDiffuseBounceMinReceiverTurn = 1.0;
static const float kShadowedDiffuseBounceMinVisibility = 0.75;

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float FiniteScalarOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 Finite2Or(float2 value, float2 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 Finite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 SafeNormalize(float3 v, float3 fallback)
{
    float lenSq = dot(v, v);
    if (!all(isfinite(v)) || lenSq < 1e-8)
    {
        return fallback;
    }
    return v * rsqrt(lenSq);
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(N, V)), roughness) *
           GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * Pow5(saturate(1.0 - cosTheta));
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxReflectivity = max((1.0 - roughness).xxx, F0);
    return F0 + (maxReflectivity - F0) * Pow5(saturate(1.0 - cosTheta));
}

float IorToF0(float ior)
{
    if (!isfinite(ior) || ior <= 0.0)
    {
        return 1.0;
    }
    float safeIor = max(ior, 1.0);
    float f0 = (safeIor - 1.0) / (safeIor + 1.0);
    return saturate(f0 * f0);
}

float3 EvaluatePbrF0(float3 albedo, float metallic, float specularFactor,
                     float3 specularColorFactor, float ior)
{
    albedo = saturate(Finite3Or(albedo, 0.0.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    specularFactor = saturate(FiniteScalarOr(specularFactor, 1.0));
    specularColorFactor =
        saturate(Finite3Or(specularColorFactor, 1.0.xxx));
    float3 dielectricF0 =
        min(IorToF0(ior).xxx * max(specularColorFactor, 0.0.xxx), 1.0.xxx) *
        saturate(specularFactor);
    return lerp(dielectricF0, albedo, saturate(metallic));
}

float EvaluateDielectricF0(float specularFactor, float3 specularColorFactor,
                           float ior)
{
    specularFactor = saturate(FiniteScalarOr(specularFactor, 1.0));
    float3 color = max(Finite3Or(specularColorFactor, 1.0.xxx), 0.0.xxx);
    float colorMagnitude = max(max(color.x, color.y), color.z);
    return saturate(IorToF0(ior) * saturate(specularFactor) * colorMagnitude);
}

float3 EvaluateDielectricF0Color(float specularFactor,
                                 float3 specularColorFactor, float ior)
{
    specularFactor = saturate(FiniteScalarOr(specularFactor, 1.0));
    specularColorFactor =
        max(Finite3Or(specularColorFactor, 1.0.xxx), 0.0.xxx);
    return saturate(IorToF0(ior).xxx * saturate(specularFactor) *
                    max(specularColorFactor, 0.0.xxx));
}

float3 EvaluatePbrF0FromDielectricF0(float3 albedo, float metallic,
                                     float dielectricF0)
{
    albedo = saturate(Finite3Or(albedo, 0.0.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    dielectricF0 = FiniteScalarOr(dielectricF0, 0.04);
    return lerp(saturate(dielectricF0).xxx, albedo, saturate(metallic));
}

float MaxComponent3(float3 value)
{
    return max(max(value.x, value.y), value.z);
}

float EstimateDeferredIridescenceThickness(float minimumThickness,
                                           float maximumThickness)
{
    minimumThickness = FiniteScalarOr(minimumThickness, 0.0);
    maximumThickness = FiniteScalarOr(maximumThickness, minimumThickness);
    return 0.5 * (max(minimumThickness, 0.0) + max(maximumThickness, 0.0));
}

float3 ApproximateDeferredIridescenceTint(float factor, float ior,
                                          float thicknessNm, float NdotV)
{
    factor = saturate(FiniteScalarOr(factor, 0.0));
    if (factor <= 0.0)
    {
        return 1.0.xxx;
    }

    ior = max(FiniteScalarOr(ior, 1.3), 1.0);
    thicknessNm = max(FiniteScalarOr(thicknessNm, 0.0), 0.0);
    NdotV = saturate(FiniteScalarOr(NdotV, 1.0));
    float phase = thicknessNm * 0.018 + ior * 1.7 + (1.0 - NdotV) * 4.0;
    if (!isfinite(phase))
    {
        return 1.0.xxx;
    }
    float3 wave = 0.5.xxx + 0.5 * cos(phase.xxx + float3(0.0, 2.094, 4.188));
    return Finite3Or(
        lerp(1.0.xxx, 0.65.xxx + 0.7 * wave, saturate(factor)),
        1.0.xxx);
}

float3 EvaluateDeferredDielectricF0(float dielectricF0,
                                    float3 specularColorHint,
                                    float iridescenceFactor,
                                    float iridescenceIor,
                                    float iridescenceThicknessNm,
                                    float NdotV)
{
    dielectricF0 = saturate(FiniteScalarOr(dielectricF0, 0.04));
    float3 colorHint = max(Finite3Or(specularColorHint, 0.0.xxx), 0.0.xxx);
    float3 iridescenceTint = ApproximateDeferredIridescenceTint(
        iridescenceFactor, iridescenceIor, iridescenceThicknessNm, NdotV);
    colorHint *= iridescenceTint;

    float hintMagnitude = MaxComponent3(colorHint);
    if (!all(isfinite(colorHint)) || hintMagnitude <= 1e-5)
    {
        return 0.0.xxx;
    }

    return saturate(dielectricF0 * colorHint / hintMagnitude);
}

float3 EvaluateDeferredPbrF0(float3 albedo, float metallic,
                             float dielectricF0, float3 specularColorHint,
                             float iridescenceFactor, float iridescenceIor,
                             float iridescenceThicknessNm, float NdotV)
{
    albedo = saturate(Finite3Or(albedo, 0.0.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    float3 dielectricF0Color = EvaluateDeferredDielectricF0(
        dielectricF0, specularColorHint, iridescenceFactor, iridescenceIor,
        iridescenceThicknessNm, NdotV);
    return lerp(dielectricF0Color, albedo, saturate(metallic));
}

float3 EvaluateDeferredPbrF0FromDielectricColor(
    float3 albedo, float metallic, float3 dielectricF0Color,
    float iridescenceFactor, float iridescenceIor,
    float iridescenceThicknessNm, float NdotV)
{
    albedo = saturate(Finite3Or(albedo, 0.0.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    dielectricF0Color = saturate(Finite3Or(dielectricF0Color, 0.04.xxx));
    float3 iridescenceTint = ApproximateDeferredIridescenceTint(
        iridescenceFactor, iridescenceIor, iridescenceThicknessNm, NdotV);
    return lerp(saturate(dielectricF0Color * iridescenceTint), albedo,
                saturate(metallic));
}

float3 EvaluateSpecularIblMultipleScattering(float3 prefilteredColor,
                                             float3 F0, float2 brdf)
{
    prefilteredColor = max(Finite3Or(prefilteredColor, 0.0.xxx), 0.0.xxx);
    F0 = saturate(Finite3Or(F0, 0.04.xxx));
    brdf = max(Finite2Or(brdf, 0.0.xx), 0.0.xx);
    float3 singleScatter = max(F0 * brdf.x + brdf.y, 0.0.xxx);
    float singleScatterEnergy = saturate(brdf.x + brdf.y);
    float missingEnergy = saturate(1.0 - singleScatterEnergy);
    float3 averageFresnel = F0 + (1.0.xxx - F0) * (1.0 / 21.0);
    float3 multiScatter =
        singleScatter * averageFresnel * missingEnergy /
        max(1.0.xxx - averageFresnel * missingEnergy, 1e-4.xxx);
    return prefilteredColor * (singleScatter + multiScatter);
}

float SmoothRangeAttenuation(float distanceSq, float radius)
{
    if (!isfinite(distanceSq) || distanceSq < 0.0)
    {
        return 0.0;
    }

    if (!isfinite(radius))
    {
        return 0.0;
    }

    if (radius <= 0.0)
    {
        return 1.0;
    }

    float safeRadius = max(radius, 1e-3);
    float invRadiusSq = 1.0 / (safeRadius * safeRadius);
    float normalizedSq = distanceSq * invRadiusSq;
    float smoothFactor = saturate(1.0 - normalizedSq);
    return smoothFactor * smoothFactor;
}

float PointLightAttenuation(float distanceSq, float radius)
{
    // colorIntensity.a is scene-linear point-light intensity. Positive radius
    // is only a finite fade/cull range; non-positive radius means no finite
    // cutoff. Energy follows inverse-square falloff outside the small
    // source-radius clamp.
    static const float MIN_POINT_LIGHT_DISTANCE_SQ = 0.01;
    if (!isfinite(distanceSq) || distanceSq < 0.0)
    {
        return 0.0;
    }
    float smoothCutoff = SmoothRangeAttenuation(distanceSq, radius);
    return smoothCutoff / max(distanceSq, MIN_POINT_LIGHT_DISTANCE_SQ);
}

float3 EvaluatePbrDirectLight(float3 albedo, float3 F0, float metallic,
                              float roughness, float3 N, float3 V, float3 L,
                              float3 radiance)
{
    albedo = max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx);
    F0 = saturate(Finite3Or(F0, 0.04.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    roughness = clamp(FiniteScalarOr(roughness, 1.0), 0.045, 1.0);
    radiance = max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
    N = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));

    float NdotL = saturate(FiniteScalarOr(dot(N, L), 0.0));
    float NdotV = saturate(FiniteScalarOr(dot(N, V), 0.0));
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return 0.0.xxx;
    }

    float3 H = SafeNormalize(V + L, N);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 diffuse = (1.0 - F) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * NdotL;
}

float3 EvaluateDiffuseBounceLight(float3 albedo, float metallic,
                                  float3 shadingNormal,
                                  float3 geometricNormal, float3 V, float3 L,
                                  float3 radiance, float directVisibility,
                                  float occlusion, float bounceIntensity)
{
    albedo = max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx);
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    shadingNormal = SafeNormalize(shadingNormal, float3(0.0, 1.0, 0.0));
    geometricNormal = SafeNormalize(geometricNormal, shadingNormal);
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));
    radiance = max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
    directVisibility = saturate(FiniteScalarOr(directVisibility, 1.0));
    occlusion = saturate(FiniteScalarOr(occlusion, 1.0));
    bounceIntensity = max(FiniteScalarOr(bounceIntensity, 0.0), 0.0);

    float directNdotL =
        saturate(FiniteScalarOr(dot(geometricNormal, L), 0.0));
    float receiverTurnsFromLight = 1.0 - directNdotL;
    float indirectReceiverTurn = lerp(
        receiverTurnsFromLight,
        max(receiverTurnsFromLight, kShadowedDiffuseBounceMinReceiverTurn),
        1.0 - directVisibility);
    float visibleReceiver =
        0.35 + 0.65 * saturate(abs(FiniteScalarOr(dot(shadingNormal, V), 0.0)));
    float aoWeight = 0.25 + 0.75 * occlusion;
    float shadowWeight =
        kShadowedDiffuseBounceMinVisibility +
        (1.0 - kShadowedDiffuseBounceMinVisibility) * directVisibility;
    float diffuseEnergy = saturate(1.0 - metallic);
    float bounceWeight = min(bounceIntensity * indirectReceiverTurn *
                                 visibleReceiver * aoWeight * shadowWeight,
                             0.65);

    return albedo * diffuseEnergy * radiance * (bounceWeight / PI);
}

float DeferredClearcoatBaseEnergyScale(float clearcoatFactor)
{
    return 1.0 - saturate(FiniteScalarOr(clearcoatFactor, 0.0)) * 0.04;
}

float3 EvaluateDeferredClearcoatDirectLight(float clearcoatFactor,
                                            float clearcoatRoughness,
                                            float3 N, float3 V, float3 L,
                                            float3 radiance)
{
    float clearcoat = saturate(FiniteScalarOr(clearcoatFactor, 0.0));
    if (clearcoat <= 0.0)
    {
        return 0.0.xxx;
    }

    float roughness = clamp(FiniteScalarOr(clearcoatRoughness, 1.0), 0.045, 1.0);
    float3 clearcoatF0 = IorToF0(1.5).xxx;
    return EvaluatePbrDirectLight(
        0.0.xxx, clearcoatF0, 1.0, roughness, N, V, L, radiance) *
        clearcoat;
}

float DeferredSheenBaseEnergyScale(float3 sheenColor)
{
    sheenColor = max(Finite3Or(sheenColor, 0.0.xxx), 0.0.xxx);
    return 1.0 - saturate(MaxComponent3(sheenColor) * 0.25);
}

float3 EvaluateDeferredSheenDirectLight(float3 sheenColor,
                                        float sheenRoughness, float3 N,
                                        float3 V, float3 L, float3 radiance)
{
    float3 color = max(Finite3Or(sheenColor, 0.0.xxx), 0.0.xxx);
    if (MaxComponent3(color) <= 0.0)
    {
        return 0.0.xxx;
    }

    radiance = max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
    N = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));
    float NdotL = saturate(FiniteScalarOr(dot(N, L), 0.0));
    float NdotV = saturate(FiniteScalarOr(dot(N, V), 0.0));
    float roughness = clamp(FiniteScalarOr(sheenRoughness, 1.0), 0.045, 1.0);
    float grazing = pow(1.0 - NdotV, 2.0) * (1.0 - 0.35 * roughness);
    return color * radiance * (NdotL * grazing * 0.35);
}

float3 EvaluateDeferredLayeredDirectLight(
    float3 albedo, float3 F0, float metallic, float roughness, float3 N,
    float3 V, float3 L, float3 radiance, float clearcoatFactor,
    float clearcoatRoughness, float3 sheenColor, float sheenRoughness)
{
    float3 lighting = EvaluatePbrDirectLight(
        albedo, F0, metallic, roughness, N, V, L, radiance);

    lighting *= DeferredClearcoatBaseEnergyScale(clearcoatFactor);
    lighting += EvaluateDeferredClearcoatDirectLight(
        clearcoatFactor, clearcoatRoughness, N, V, L, radiance);

    lighting *= DeferredSheenBaseEnergyScale(sheenColor);
    lighting += EvaluateDeferredSheenDirectLight(
        sheenColor, sheenRoughness, N, V, L, radiance);

    return lighting;
}

float3 EvaluateThinSurfaceTransmission(float3 albedo, float3 normal, float3 V,
                                       float3 L, float3 radiance,
                                       float alpha, bool isBackFace)
{
    albedo = max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx);
    radiance = max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
    normal = SafeNormalize(normal, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));
    float wrappedDiffuse =
        saturate((FiniteScalarOr(dot(normal, L), 0.0) + 0.45) / 1.45);
    float backLight = saturate(FiniteScalarOr(dot(-normal, L), 0.0));
    float viewThrough = pow(saturate(FiniteScalarOr(dot(-L, V), 0.0)), 2.0);
    float faceWeight = isBackFace ? 1.0 : 0.45;
    float opacityWeight = saturate(1.0 - FiniteScalarOr(alpha, 1.0) * 0.5);
    float transmission = max(wrappedDiffuse * 0.35, backLight * viewThrough) *
                         faceWeight * opacityWeight;
    return albedo * radiance * transmission;
}

bool TryReconstructWorldPositionFromClip(
    float4 clipPosition,
    column_major float4x4 inverseViewProj,
    out float3 worldPosition)
{
    worldPosition = 0.0.xxx;
    if (!all(isfinite(clipPosition)))
    {
        return false;
    }

    float4 projected = mul(inverseViewProj, clipPosition);
    if (!all(isfinite(projected)) || projected.w <= 1e-6)
    {
        return false;
    }

    float3 candidate = projected.xyz / projected.w;
    if (!all(isfinite(candidate)))
    {
        return false;
    }

    worldPosition = candidate;
    return true;
}

bool TryReconstructWorldPosition(float2 texCoord, float depth,
                                 column_major float4x4 inverseViewProj,
                                 out float3 worldPosition)
{
    // texCoord is framebuffer UV from a negative-height scene viewport. Convert
    // it back to scene NDC explicitly before applying inverse view-projection.
    worldPosition = 0.0.xxx;
    if (!all(isfinite(texCoord)) || !isfinite(depth) ||
        depth < 0.0 || depth > 1.0)
    {
        return false;
    }

    float2 ndcXY = texCoord * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y; // Negative-height viewport: framebuffer Y is inverted relative to NDC Y.
    float4 clipPosition = float4(ndcXY, depth, 1.0);
    return TryReconstructWorldPositionFromClip(
        clipPosition, inverseViewProj, worldPosition);
}

float3 ReconstructWorldPosition(float2 texCoord, float depth,
                                column_major float4x4 inverseViewProj)
{
    float3 worldPosition = 0.0.xxx;
    if (!TryReconstructWorldPosition(
            texCoord, depth, inverseViewProj, worldPosition))
    {
        return 0.0.xxx;
    }
    return worldPosition;
}
