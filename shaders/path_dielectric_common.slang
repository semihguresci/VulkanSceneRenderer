#pragma once
#include "path_transport_common.slang"

// Relative eta is the transmitted IOR / incident IOR. The shading frame faces
// the incident medium, so view.z is positive on either side of a boundary.
// Walter's generalized half vector and its refraction Jacobian are evaluated
// in radiance transport mode. Fresnel retains glTF's Schlick convention with
// the transmitted angle on exit and an unscaled total-internal-reflection limit.
struct PtVolumeBsdf
{
    PtBsdf reflection;
    float3 transmission;
    float eta;
};
bool PtValidVolumeBsdf(PtVolumeBsdf bsdf)
{
    return PtValidBsdf(bsdf.reflection) && all(isfinite(bsdf.transmission)) &&
        all(bsdf.transmission >= 0.0.xxx) && all(bsdf.transmission <= 1.0.xxx) &&
        isfinite(bsdf.eta) && bsdf.eta > 0.0;
}
float3 PtVolumeFresnel(PtVolumeBsdf bsdf, float cosine, out float transmittedCosine)
{
    cosine = saturate(cosine);
    float sineSquared = max(0.0, 1.0 - cosine * cosine) / (bsdf.eta * bsdf.eta);
    transmittedCosine = sqrt(max(0.0, 1.0 - sineSquared));
    if (sineSquared >= 1.0) return 1.0.xxx;
    // Index matching has no dielectric reflection at any microfacet angle.
    if (bsdf.eta == 1.0 && all(bsdf.reflection.f0 == 0.0.xxx)) return 0.0.xxx;
    return PtFresnel(bsdf.reflection.f0, bsdf.eta < 1.0 ? transmittedCosine : cosine) * bsdf.reflection.specularScale;
}
float PtVolumeReflectionProbability(PtVolumeBsdf bsdf, float3 fresnel)
{
    float reflection = PtLuminance(fresnel);
    float transmission = PtLuminance(bsdf.transmission * (1.0.xxx - fresnel));
    return reflection + transmission > 0.0 ? reflection / (reflection + transmission) : 0.0;
}
float PtVolumeMicrofacetProbability(PtVolumeBsdf bsdf, float3 view)
{
    if (all(bsdf.reflection.diffuse == 0.0.xxx)) return 1.0;
    if (bsdf.reflection.specularScale == 0.0 && all(bsdf.transmission == 0.0.xxx) && bsdf.eta >= 1.0) return 0.0;
    float unused;
    float3 fresnel = PtVolumeFresnel(bsdf, view.z, unused);
    float microfacet = PtLuminance(fresnel + bsdf.transmission * (1.0.xxx - fresnel));
    float diffuse = PtLuminance(bsdf.reflection.diffuse * (1.0.xxx - fresnel));
    return clamp(microfacet / max(microfacet + diffuse, 1e-30), 0.05, 0.95);
}
PtThinBsdf PtIndexMatchedBsdf(PtVolumeBsdf bsdf)
{
    PtThinBsdf result;
    result.reflection = bsdf.reflection;
    if (all(result.reflection.f0 == 0.0.xxx)) result.reflection.specularScale = 0.0;
    result.transmission = bsdf.transmission;
    result.roughness = 0.0;
    return result;
}
PtBsdfEvaluation PtEvaluateVolumeBsdf(PtVolumeBsdf bsdf, float3 view, float3 light)
{
    PtBsdfEvaluation result = (PtBsdfEvaluation)0;
    if (!PtValidVolumeBsdf(bsdf) || !PtValidDirection(view) || !PtValidDirection(light)) return result;
    if (bsdf.eta == 1.0) return PtEvaluateThinBsdf(PtIndexMatchedBsdf(bsdf), view, light);
    result.valid = 1u;
    if (view.z <= 0.0 || light.z == 0.0) return result;
    bool reflection = light.z > 0.0;
    float3 halfVector = PtNormalize(view + light * (reflection ? 1.0 : bsdf.eta));
    if (halfVector.z < 0.0) halfVector = -halfVector;
    float vh = dot(view, halfVector), lh = dot(light, halfVector);
    if (vh <= 0.0 || lh * light.z <= 0.0) return result;
    float cosineTransmitted;
    float3 fresnel = PtVolumeFresnel(bsdf, vh, cosineTransmitted);
    float microfacetProbability = PtVolumeMicrofacetProbability(bsdf, view);
    float reflectionProbability = PtVolumeReflectionProbability(bsdf, fresnel);
    if (reflection)
    {
        result.value = bsdf.reflection.diffuse * (1.0.xxx - fresnel) / PT_PI;
        result.pdf = (1.0 - microfacetProbability) * light.z / PT_PI;
    }
    if (bsdf.reflection.roughness > 0.0)
    {
        float alpha = bsdf.reflection.roughness * bsdf.reflection.roughness;
        float lengthView = length(float3(alpha * view.xy, view.z));
        float lengthLight = length(float3(alpha * light.xy, light.z));
        float distribution = PtGgxDistribution(halfVector, alpha);
        float visibility = 0.5 / (abs(light.z) * lengthView + view.z * lengthLight);
        if (reflection)
        {
            result.value += fresnel * distribution * visibility;
            result.pdf += microfacetProbability * reflectionProbability * distribution / (2.0 * (view.z + lengthView));
        }
        else
        {
            float denominator = vh + bsdf.eta * lh;
            float square = denominator * denominator;
            if (square == 0.0) return result;
            result.value = bsdf.transmission * (1.0.xxx - fresnel) *
                (4.0 * distribution * visibility * abs(vh * lh) / square);
            float halfPdf = 2.0 * distribution * vh / (view.z + lengthView);
            float jacobian = abs(lh) * bsdf.eta * bsdf.eta / square;
            result.pdf = microfacetProbability * (1.0 - reflectionProbability) * halfPdf * jacobian;
        }
    }
    if (!all(isfinite(result.value)) || !isfinite(result.pdf) || result.pdf < 0.0) return (PtBsdfEvaluation)0;
    return result;
}
PtBsdfSample PtSampleVolumeBsdf(PtVolumeBsdf bsdf, float3 view, float3 u)
{
    if (!PtValidVolumeBsdf(bsdf) || !PtValidDirection(view) || view.z <= 0.0 ||
        !all(isfinite(u)) || any(u < 0.0.xxx) || any(u >= 1.0.xxx)) return PtRejectedBsdfSample();
    if (bsdf.eta == 1.0) return PtSampleThinBsdf(PtIndexMatchedBsdf(bsdf), view, u);
    PtBsdfSample result = (PtBsdfSample)0;
    float probability = PtVolumeMicrofacetProbability(bsdf, view);
    if (u.x < probability)
    {
        float3 halfVector = float3(0, 0, 1);
        if (bsdf.reflection.roughness > 0.0)
            halfVector = PtSampleGgxVisibleNormal(view, bsdf.reflection.roughness * bsdf.reflection.roughness, u.yz);
        float cosineTransmitted;
        float3 fresnel = PtVolumeFresnel(bsdf, dot(view, halfVector), cosineTransmitted);
        if (all(fresnel == 0.0.xxx) && all(bsdf.transmission * (1.0.xxx - fresnel) == 0.0.xxx)) return result;
        float reflectionProbability = PtVolumeReflectionProbability(bsdf, fresnel);
        bool reflection = u.x / probability < reflectionProbability;
        if (reflection)
            result.direction = reflect(-view, halfVector);
        else
            result.direction = PtNormalize(-view / bsdf.eta +
                (dot(view, halfVector) / bsdf.eta - cosineTransmitted) * halfVector);
        if ((reflection && result.direction.z <= 0.0) || (!reflection && result.direction.z >= 0.0)) return (PtBsdfSample)0;
        if (bsdf.reflection.roughness == 0.0)
        {
            result.delta = 1u;
            result.pdf = probability * (reflection ? reflectionProbability : 1.0 - reflectionProbability);
            result.weight = reflection ? fresnel / result.pdf :
                bsdf.transmission * (1.0.xxx - fresnel) / (result.pdf * bsdf.eta * bsdf.eta);
            if (!all(isfinite(result.weight)) || !PtValidDirection(result.direction)) return PtRejectedBsdfSample();
            result.valid = 1u;
            return result;
        }
    }
    else result.direction = PtCosineHemisphere(u.yz);
    PtBsdfEvaluation evaluated = PtEvaluateVolumeBsdf(bsdf, view, result.direction);
    if (evaluated.valid == 0u || evaluated.pdf <= 0.0) return PtRejectedBsdfSample();
    result.pdf = evaluated.pdf;
    result.weight = evaluated.value * (abs(result.direction.z) / evaluated.pdf);
    if (!all(isfinite(result.weight))) return PtRejectedBsdfSample();
    result.valid = 1u;
    return result;
}
bool PtValidAbsorption(float3 color, float distance)
{
    return all(isfinite(color)) && all(color >= 0.0.xxx) && all(color <= 1.0.xxx) &&
        !isnan(distance) && distance > 0.0;
}
float3 PtAbsorption(float3 color, float attenuationDistance, float distance)
{
    if (distance == 0.0 || isinf(attenuationDistance)) return 1.0.xxx;
    float3 result;
    for (uint c = 0u; c < 3u; ++c)
        result[c] = color[c] == 0.0 ? 0.0 : exp(log(color[c]) * (distance / attenuationDistance));
    return result;
}
