#ifndef CONTAINER_PATH_EXPERIMENTAL_MATERIALS
#define CONTAINER_PATH_EXPERIMENTAL_MATERIALS

#include "path_transport_common.slang"

// Thin scattering moves part of the coherent dielectric transmission into
// Lambertian reflection and transmission. Keep the residual BTDF tint and the
// other diffuse shares independent, including zero color channels.
void PtApplyThinScatter(inout float3 reflection, inout float3 diffuseTransmission,
    inout float3 specularTransmission, float dielectricTransmission,
    float strength, float anisotropy, float3 color)
{
    float forward = 0.5 * (1.0 + anisotropy);
    float3 scattered = dielectricTransmission * strength * color;
    reflection += scattered * (1.0 - forward);
    diffuseTransmission += scattered * forward;
    specularTransmission *= 1.0 - strength;
}

float PtRetroProposalProbability(float retroreflection)
{
    // Preserve the ordinary proposal at full retroreflection because it also
    // samples the unchanged opposite-hemisphere lobes.
    return 0.5 * retroreflection;
}

float3 PtMirroredView(float3 view)
{
    return float3(-view.xy, view.z);
}

PtBsdfEvaluation PtEvaluateRetroBsdf(PtThinBsdf bsdf, float3 tint, float3 view,
    float3 light, float anisotropy, float retroreflection)
{
    if (!(retroreflection >= 0.0 && retroreflection <= 1.0)) return (PtBsdfEvaluation)0;
    PtBsdfEvaluation base = PtEvaluateDiffuseTransmissionBsdf(bsdf, tint, view, light, anisotropy);
    if (retroreflection == 0.0 || base.valid == 0u) return base;
    const float probability = PtRetroProposalProbability(retroreflection);
    base.pdf *= 1.0 - probability;
    if (light.z > 0.0)
    {
        const PtBsdfEvaluation mirrored = PtEvaluateBsdf(bsdf.reflection, PtMirroredView(view), light, anisotropy);
        if (mirrored.valid == 0u) return (PtBsdfEvaluation)0;
        base.value = lerp(base.value, mirrored.value, retroreflection);
        base.pdf += probability * mirrored.pdf;
    }
    return base;
}

PtBsdfSample PtSampleRetroBsdf(PtThinBsdf bsdf, float3 tint, float3 view,
    float3 random, float anisotropy, float retroreflection)
{
    if (retroreflection == 0.0)
        return PtSampleDiffuseTransmissionBsdf(bsdf, tint, view, random, anisotropy);
    if (!(retroreflection > 0.0 && retroreflection <= 1.0) || !all(isfinite(random)) ||
        any(random < 0.0.xxx) || any(random >= 1.0.xxx)) return PtRejectedBsdfSample();
    const float probability = PtRetroProposalProbability(retroreflection);
    const bool mirrored = random.x < probability;
    const float branchProbability = mirrored ? probability : 1.0 - probability;
    float choice = mirrored ? random.x : random.x - probability;
    choice = min(choice / branchProbability, asfloat(0x3f7fffffu));
    PtBsdfSample result;
    if (mirrored)
        result = PtSampleBsdf(bsdf.reflection, PtMirroredView(view), float3(choice, random.yz), anisotropy);
    else
        result = PtSampleDiffuseTransmissionBsdf(bsdf, tint, view, float3(choice, random.yz), anisotropy);
    if (result.valid == 0u) return result;
    if (result.delta != 0u)
    {
        // At normal incidence the two delta directions coincide. Keep their
        // separately allocated energies and masses in the chosen proposal.
        const float share = mirrored ? retroreflection : (result.direction.z > 0.0 ? 1.0 - retroreflection : 1.0);
        result.pdf *= branchProbability;
        result.weight *= share / branchProbability;
        if (!all(isfinite(result.weight))) return PtRejectedBsdfSample();
        return result;
    }
    const PtBsdfEvaluation evaluated = PtEvaluateRetroBsdf(bsdf, tint, view, result.direction, anisotropy, retroreflection);
    if (evaluated.valid == 0u || evaluated.pdf <= 0.0) return PtRejectedBsdfSample();
    result.pdf = evaluated.pdf;
    result.weight = evaluated.value * (abs(result.direction.z) / evaluated.pdf);
    if (!all(isfinite(result.weight))) return PtRejectedBsdfSample();
    return result;
}

#endif
