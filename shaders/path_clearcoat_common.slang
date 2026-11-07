#ifndef CONTAINER_PATH_CLEARCOAT_COMMON
#define CONTAINER_PATH_CLEARCOAT_COMMON

#include "path_transport_common.slang"

// Directions and the independent clearcoat normal use the base shading frame.
// The glTF layer operator evaluates Fresnel once, from the view direction.
// The coating's GGX lobe therefore uses unit Fresnel in its own normal frame.
struct PtClearcoat
{
    float factor;
    float roughness;
    float3 normal;
};

bool PtValidClearcoat(PtClearcoat coat)
{
    return isfinite(coat.factor) && coat.factor >= 0.0 && coat.factor <= 1.0 &&
        isfinite(coat.roughness) && coat.roughness >= 0.0 && coat.roughness <= 1.0 &&
        PtValidDirection(coat.normal);
}

float PtClearcoatWeight(PtClearcoat coat, float3 view)
{
    if (coat.factor == 0.0) return 0.0;
    if (!PtValidClearcoat(coat) || !PtValidDirection(view)) return -1.0;
    return coat.factor * PtFresnel(0.04.xxx, abs(dot(view, coat.normal))).x;
}

float3 PtClearcoatTangent(float3 normal)
{
    return abs(normal.z) < 0.999
        ? PtNormalize(cross(float3(0.0, 0.0, 1.0), normal))
        : PtNormalize(cross(float3(0.0, 1.0, 0.0), normal));
}

float3 PtClearcoatLocal(float3 normal, float3 direction)
{
    const float3 tangent = PtClearcoatTangent(normal);
    return float3(dot(direction, tangent), dot(direction, cross(normal, tangent)), dot(direction, normal));
}

float3 PtClearcoatWorld(float3 normal, float3 direction)
{
    const float3 tangent = PtClearcoatTangent(normal);
    return tangent * direction.x + cross(normal, tangent) * direction.y + normal * direction.z;
}

PtBsdf PtClearcoatBsdf(PtClearcoat coat)
{
    PtBsdf bsdf = (PtBsdf)0;
    bsdf.f0 = 1.0.xxx;
    bsdf.specularScale = 1.0;
    bsdf.roughness = coat.roughness;
    return bsdf;
}

PtBsdfEvaluation PtEvaluateClearcoat(PtClearcoat coat, float3 view, float3 light)
{
    if (!PtValidClearcoat(coat) || !PtValidDirection(view) || !PtValidDirection(light))
        return (PtBsdfEvaluation)0;
    if (view.z <= 0.0 || light.z <= 0.0)
    {
        PtBsdfEvaluation empty = (PtBsdfEvaluation)0;
        empty.valid = 1u;
        return empty;
    }
    PtBsdfEvaluation result = PtEvaluateBsdf(PtClearcoatBsdf(coat),
        PtClearcoatLocal(coat.normal, view), PtClearcoatLocal(coat.normal, light));
    if (result.valid == 0u) return result;
    const float cosine = abs(light.z);
    // The caller multiplies by the base-frame cosine. Preserve the physical
    // coat-normal cosine when the two normal maps describe different frames.
    if (cosine == 0.0) result.value = 0.0.xxx;
    else result.value *= max(dot(light, coat.normal), 0.0) / cosine;
    if (!all(isfinite(result.value))) return (PtBsdfEvaluation)0;
    return result;
}

PtBsdfSample PtSampleClearcoat(PtClearcoat coat, float3 view, float3 random)
{
    if (!PtValidClearcoat(coat) || !PtValidDirection(view) || view.z <= 0.0 || !all(isfinite(random)) ||
        any(random < 0.0.xxx) || any(random >= 1.0.xxx)) return PtRejectedBsdfSample();
    if (dot(view, coat.normal) <= 0.0) return (PtBsdfSample)0;
    if (coat.roughness == 0.0)
    {
        PtBsdfSample result = (PtBsdfSample)0;
        result.direction = reflect(-view, coat.normal);
        if (result.direction.z <= 0.0) return (PtBsdfSample)0;
        result.pdf = 1.0;
        result.weight = 1.0.xxx;
        result.valid = result.delta = 1u;
#if REFERENCE_DLSS_RR
        result.specularReflection = 1u;
#endif
        return result;
    }
    PtBsdfSample result = PtSampleBsdf(PtClearcoatBsdf(coat), PtClearcoatLocal(coat.normal, view), random);
    if (result.valid != 0u) result.direction = PtClearcoatWorld(coat.normal, result.direction);
    if (result.valid != 0u && result.direction.z <= 0.0) return (PtBsdfSample)0;
    return result;
}

PtBsdfSample PtClearcoatDelta(PtClearcoat coat, float3 view, float3 direction)
{
    if (coat.roughness != 0.0 || !PtValidClearcoat(coat) || !PtValidDirection(view) ||
        view.z <= 0.0 || direction.z <= 0.0 || dot(view, coat.normal) <= 0.0 ||
        !all(direction == reflect(-view, coat.normal)))
        return (PtBsdfSample)0;
    return PtSampleClearcoat(coat, view, 0.0.xxx);
}

PtBsdfEvaluation PtEvaluateClearcoatLayer(PtClearcoat coat, float3 view, float3 light,
    PtBsdfEvaluation base)
{
    if (coat.factor == 0.0) return base;
    const float q = PtClearcoatWeight(coat, view);
    if (q < 0.0 || base.valid == 0u) return (PtBsdfEvaluation)0;
    const PtBsdfEvaluation coating = PtEvaluateClearcoat(coat, view, light);
    if (coating.valid == 0u) return (PtBsdfEvaluation)0;
    base.value = base.value * (1.0 - q) + coating.value * q;
    base.pdf = base.pdf * (1.0 - q) + coating.pdf * q;
    if (!all(isfinite(base.value)) || !isfinite(base.pdf) || base.pdf < 0.0)
        return (PtBsdfEvaluation)0;
    return base;
}

// The caller samples the coating with probability q and its ordinary BSDF
// with probability 1-q, remapping the branch random number before sampling.
// For a delta outcome, supply the OTHER proposal's coincident discrete event.
// Its weight*pdf is the complete event numerator before this outer mixture.
PtBsdfSample PtCompleteClearcoatSample(PtClearcoat coat, float3 view, bool sampledCoat,
    PtBsdfSample selected, PtBsdfEvaluation baseAtDirection, PtBsdfSample coincidentOtherDelta)
{
    if (coat.factor == 0.0) return selected;
    const float q = PtClearcoatWeight(coat, view);
    if (q < 0.0) return PtRejectedBsdfSample();
    if (selected.valid == 0u) return selected;
    const float probability = sampledCoat ? q : 1.0 - q;
    if (probability <= 0.0 || !isfinite(selected.pdf) || selected.pdf <= 0.0 ||
        !PtValidDirection(selected.direction)) return PtRejectedBsdfSample();
    if (selected.delta != 0u)
    {
        float3 numerator = selected.weight * (probability * selected.pdf);
        selected.pdf *= probability;
        if (coincidentOtherDelta.valid != 0u)
        {
            if (coincidentOtherDelta.delta == 0u || !all(coincidentOtherDelta.direction == selected.direction) ||
                !isfinite(coincidentOtherDelta.pdf) || coincidentOtherDelta.pdf <= 0.0)
                return PtRejectedBsdfSample();
            const float otherMass = (1.0 - probability) * coincidentOtherDelta.pdf;
            selected.pdf += otherMass;
            numerator += coincidentOtherDelta.weight * otherMass;
        }
        selected.weight = numerator / selected.pdf;
    }
    else
    {
        const PtBsdfEvaluation evaluated = PtEvaluateClearcoatLayer(coat, view, selected.direction, baseAtDirection);
        if (evaluated.valid == 0u || evaluated.pdf <= 0.0) return PtRejectedBsdfSample();
        selected.pdf = evaluated.pdf;
        selected.weight = evaluated.value * (abs(selected.direction.z) / evaluated.pdf);
    }
    if (!all(isfinite(selected.weight)) || !isfinite(selected.pdf)) return PtRejectedBsdfSample();
    return selected;
}

#endif
