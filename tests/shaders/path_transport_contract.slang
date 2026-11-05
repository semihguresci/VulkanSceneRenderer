#include "path_transport_common.slang"
#include "path_dielectric_common.slang"

struct Query
{
    float4 diffuseRoughness;
    float4 f0Scale;
    float4 view;
    float4 light;
    uint4 controls;
    float4 previousMean;
    float4 previousMoment;
    float4 sampleValue;
    column_major float4x4 inverseProjection;
    column_major float4x4 inverseView;
};
struct Result
{
    float4 valuePdf;
    float4 directionPdf;
    float4 weightValid;
    float4 auxiliary;
};
[[vk::binding(0, 0)]] StructuredBuffer<Query> queries;
[[vk::binding(1, 0)]] RWStructuredBuffer<Result> results;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint count, stride;
    queries.GetDimensions(count, stride);
    if (tid.x >= count) return;
    Query query = queries[tid.x];
    PtBsdf bsdf;
    bsdf.diffuse = query.diffuseRoughness.rgb;
    bsdf.roughness = query.diffuseRoughness.w;
    bsdf.f0 = query.f0Scale.rgb;
    bsdf.specularScale = query.f0Scale.w;
    Result result = (Result)0;
    uint operation = query.controls.x;
    if (operation == 0u)
    {
        PtBsdfEvaluation evaluated = PtEvaluateBsdf(bsdf, query.view.xyz, query.light.xyz);
        result.valuePdf = float4(evaluated.value, evaluated.pdf);
        result.auxiliary.x = PtSpecularProbability(bsdf, query.view.xyz);
        result.auxiliary.z = float(evaluated.valid);
    }
    else if (operation == 1u || operation == 2u || operation == 4u)
    {
        float3 random = float3(
            PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
            PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
            PtRandom(query.controls.z, query.controls.w, 0u, 2u, query.controls.y));
        if (operation == 4u) random = query.previousMean.xyz;
        PtBsdfSample sampled = PtSampleBsdf(bsdf, query.view.xyz, random);
        result.directionPdf = float4(sampled.direction, sampled.pdf);
        result.weightValid = float4(sampled.weight, float(sampled.valid));
        PtBsdfEvaluation evaluated = PtEvaluateBsdf(bsdf, query.view.xyz, sampled.direction);
        result.valuePdf = float4(evaluated.value, evaluated.pdf);
        result.auxiliary.x = float(sampled.delta);
        result.auxiliary.y = float(sampled.rejected);
        result.auxiliary.z = float(evaluated.valid);
        if (operation == 2u)
        {
            // Two-proposal next-event estimator for a constant unit environment:
            // one uniform-sphere light sample and one BSDF sample, with MIS.
            float2 lightRandom = float2(
                PtRandom(query.controls.z, query.controls.w, 0u, 3u, query.controls.y),
                PtRandom(query.controls.z, query.controls.w, 0u, 4u, query.controls.y));
            float3 lightDirection = PtUniformSphere(lightRandom);
            PtBsdfEvaluation direct = PtEvaluateBsdf(bsdf, query.view.xyz, lightDirection);
            float lightPdf = 1.0 / (4.0 * PT_PI);
            float3 estimate = direct.value * max(0.0, lightDirection.z) *
                (PtPowerHeuristic(lightPdf, direct.pdf) / lightPdf);
            if (sampled.valid != 0u)
                estimate += sampled.weight * (sampled.delta != 0u ? 1.0 :
                    PtPowerHeuristic(sampled.pdf, lightPdf));
            result.weightValid.rgb = estimate;
        }
    }
    else if (operation == 3u)
    {
        float3 mean = query.previousMean.xyz, moment = query.previousMoment.xyz;
        uint sampleCount = query.controls.y;
        bool accepted = PtAccumulate(mean, moment, sampleCount, query.sampleValue.xyz);
        result.valuePdf = float4(mean, float(sampleCount));
        result.directionPdf = float4(moment, float(accepted));
    }
    else if (operation == 5u)
    {
        result.valuePdf = float4(
            PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
            PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
            PtRandom(query.controls.z, query.controls.w, 1u, 0u, query.controls.y),
            PtRandom(query.controls.z, query.controls.w, 1u, 1u, query.controls.y));
    }
    else if (operation == 6u)
    {
        float3 throughput = query.sampleValue.xyz;
        bool continued = PtRussianRoulette(throughput, query.controls.z,
            query.controls.w, query.previousMean.x);
        result.weightValid = float4(throughput, float(continued));
    }
    else if (operation == 7u)
        result.valuePdf.x = PtPowerHeuristic(query.light.x, query.light.y);
    else if (operation == 8u)
    {
        PtCameraRay ray = PtGenerateCameraRay(query.light.xy, query.controls.zw,
            query.inverseProjection, query.inverseView, query.controls.y != 0u);
        result.valuePdf = float4(ray.origin, float(ray.valid));
        result.directionPdf = float4(ray.direction, 0.0);
        result.auxiliary.xy = float2(ray.minimum, ray.maximum);
    }
    else if (operation >= 9u && operation <= 12u)
    {
        PtThinBsdf thin;
        thin.reflection = bsdf;
        thin.transmission = query.sampleValue.rgb;
        thin.roughness = query.sampleValue.w;
        if (operation == 9u)
        {
            PtBsdfEvaluation evaluated = PtEvaluateThinBsdf(thin, query.view.xyz, query.light.xyz);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.auxiliary.x = PtThinProbability(thin, query.view.xyz);
            result.auxiliary.z = float(evaluated.valid);
        }
        else
        {
            float3 u = float3(PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 2u, query.controls.y));
            if (operation == 11u) u = query.previousMean.xyz;
            PtBsdfSample sampled = PtSampleThinBsdf(thin, query.view.xyz, u);
            PtBsdfEvaluation evaluated = PtEvaluateThinBsdf(thin, query.view.xyz, sampled.direction);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.directionPdf = float4(sampled.direction, sampled.pdf);
            result.weightValid = float4(sampled.weight, float(sampled.valid));
            result.auxiliary = float4(float(sampled.delta), float(sampled.rejected), float(evaluated.valid), 0.0);
            if (operation == 12u)
            {
                float3 light = PtUniformSphere(float2(
                    PtRandom(query.controls.z, query.controls.w, 0u, 3u, query.controls.y),
                    PtRandom(query.controls.z, query.controls.w, 0u, 4u, query.controls.y)));
                PtBsdfEvaluation direct = PtEvaluateThinBsdf(thin, query.view.xyz, light);
                float lightPdf = 1.0 / (4.0 * PT_PI);
                float3 estimate = direct.value * abs(light.z) * PtPowerHeuristic(lightPdf, direct.pdf) / lightPdf;
                if (sampled.valid != 0u)
                    estimate += sampled.weight * (sampled.delta != 0u ? 1.0 : PtPowerHeuristic(sampled.pdf, lightPdf));
                result.weightValid.rgb = estimate;
            }
        }
    }
    if (operation >= 13u && operation <= 18u)
    {
        PtVolumeBsdf volume;
        volume.reflection = bsdf;
        volume.transmission = query.sampleValue.rgb;
        volume.eta = query.previousMoment.w;
        if (operation == 13u)
        {
            PtBsdfEvaluation evaluated = PtEvaluateVolumeBsdf(volume, query.view.xyz, query.light.xyz);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.auxiliary.z = float(evaluated.valid);
        }
        else if (operation == 16u)
        {
            bool valid = PtValidAbsorption(query.sampleValue.rgb, query.sampleValue.w) &&
                isfinite(query.previousMean.x) && query.previousMean.x >= 0.0;
            if (valid) result.valuePdf = float4(PtAbsorption(query.sampleValue.rgb, query.sampleValue.w, query.previousMean.x), 1.0);
        }
        else if (operation == 18u)
        {
            float cosine;
            result.valuePdf = float4(PtVolumeFresnel(volume, query.view.z, cosine), cosine);
        }
        else
        {
            float3 u = float3(PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 2u, query.controls.y));
            if (operation == 15u) u = query.previousMean.xyz;
            PtBsdfSample sampled = PtSampleVolumeBsdf(volume, query.view.xyz, u);
            PtBsdfEvaluation evaluated = PtEvaluateVolumeBsdf(volume, query.view.xyz, sampled.direction);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.directionPdf = float4(sampled.direction, sampled.pdf);
            result.weightValid = float4(sampled.weight, float(sampled.valid));
            result.auxiliary = float4(float(sampled.delta), float(sampled.rejected), float(evaluated.valid), 0.0);
            if (operation == 17u)
            {
                float3 light = PtUniformSphere(float2(
                    PtRandom(query.controls.z, query.controls.w, 0u, 3u, query.controls.y),
                    PtRandom(query.controls.z, query.controls.w, 0u, 4u, query.controls.y)));
                PtBsdfEvaluation direct = PtEvaluateVolumeBsdf(volume, query.view.xyz, light);
                float lightPdf = 1.0 / (4.0 * PT_PI);
                float3 estimate = direct.value * abs(light.z) * PtPowerHeuristic(lightPdf, direct.pdf) / lightPdf;
                if (sampled.valid != 0u)
                    estimate += sampled.weight * (sampled.delta != 0u ? 1.0 : PtPowerHeuristic(sampled.pdf, lightPdf));
                result.weightValid.rgb = estimate;
            }
        }
    }
    results[tid.x] = result;
}
