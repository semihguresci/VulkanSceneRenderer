#include "path_transport_common.slang"
#include "path_dielectric_common.slang"
#include "material_data_common.slang"
#include "ray_scene_data.slang"
#include "ray_surface_common.slang"
#include "ray_footprint_common.slang"

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
    float4 footprintA;
    float4 footprintB;
    float4 footprintC;
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
[[vk::binding(2, 0)]] Texture2D<float4> contractTexture;
[[vk::binding(3, 0)]] SamplerState contractSampler;

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
    if (operation >= 19u && operation <= 22u)
    {
        PtVolumeMixture material;
        material.dielectric.reflection = bsdf;
        material.dielectric.transmission = query.sampleValue.rgb;
        material.dielectric.eta = query.previousMoment.w;
        material.conductor = query.previousMean.rgb;
        material.metallic = query.previousMoment.x;
        if (operation == 19u)
        {
            PtBsdfEvaluation evaluated = PtEvaluateVolumeMixture(material, query.view.xyz, query.light.xyz);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.auxiliary.z = float(evaluated.valid);
        }
        else
        {
            float3 u = float3(PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 2u, query.controls.y));
            if (operation == 21u) u = query.light.xyz;
            PtBsdfSample sampled = PtSampleVolumeMixture(material, query.view.xyz, u);
            PtBsdfEvaluation evaluated = PtEvaluateVolumeMixture(material, query.view.xyz, sampled.direction);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.directionPdf = float4(sampled.direction, sampled.pdf);
            result.weightValid = float4(sampled.weight, float(sampled.valid));
            result.auxiliary = float4(float(sampled.delta), float(sampled.rejected), float(evaluated.valid), 0);
            if (operation == 22u)
            {
                float3 light = PtUniformSphere(float2(PtRandom(query.controls.z, query.controls.w, 0u, 3u, query.controls.y),
                    PtRandom(query.controls.z, query.controls.w, 0u, 4u, query.controls.y)));
                PtBsdfEvaluation direct = PtEvaluateVolumeMixture(material, query.view.xyz, light);
                float lightPdf = 1.0 / (4.0 * PT_PI);
                float3 estimate = direct.value * abs(light.z) * PtPowerHeuristic(lightPdf, direct.pdf) / lightPdf;
                if (sampled.valid != 0u) estimate += sampled.weight * (sampled.delta != 0u ? 1.0 : PtPowerHeuristic(sampled.pdf, lightPdf));
                result.weightValid.rgb = estimate;
            }
        }
    }
    if (operation == 23u)
    {
        // Exercise the production raster material inputs independently of BSDFs.
        result.valuePdf = float4(ResolveDeferredTextureBackedColor(query.diffuseRoughness.rgb, query.controls.y),
            ResolveDeferredTextureBackedFactor(query.diffuseRoughness.x, query.controls.y));
        result.auxiliary.x = ResolveDeferredTextureBackedRoughness(query.diffuseRoughness.x, query.controls.y);
    }
    if (operation >= 24u && operation <= 27u)
    {
        // Ray-cone footprint contracts (#82): the reference renderer's slant
        // geometry, per-coordinate-set texel density on a reconstructed hit,
        // equal-solid-angle probe spread, and the mip an explicit gradient
        // selects on authored-mip hardware texture state.
        if (operation == 24u)
        {
            RayCone cone = {query.footprintA.x, query.footprintA.y};
            result.valuePdf.x = RayConeWorldFootprint(cone, query.footprintA.z,
                query.light.xyz, query.view.xyz);
        }
        else if (operation == 25u)
        {
            // footprintA/B/C carry per-corner UV pairs (xy = UV0, zw = UV1),
            // view/light/previousMean carry the three local positions, and
            // inverseView doubles as the instance model transform.
            RayTriangleData uv;
            uv.uv[0] = query.footprintA;
            uv.uv[1] = query.footprintB;
            uv.uv[2] = query.footprintC;
            uv.vertexAlpha = float4(1.0, 1.0, 1.0, 0.0);
            RaySurfaceTriangleData triangle;
            triangle.positions[0] = float4(query.view.xyz, 1.0);
            triangle.positions[1] = float4(query.light.xyz, 1.0);
            triangle.positions[2] = float4(query.previousMean.xyz, 1.0);
            triangle.normals[0] = triangle.normals[1] = triangle.normals[2] = float4(0.0, 0.0, 1.0, 0.0);
            triangle.tangents[0] = triangle.tangents[1] = triangle.tangents[2] = float4(1.0, 0.0, 0.0, 1.0);
            triangle.colors[0] = triangle.colors[1] = triangle.colors[2] = float4(1.0, 1.0, 1.0, 1.0);
            RaySurfaceInstanceData instance;
            instance.model = query.inverseView;
            instance.normalColumns[0] = float4(1.0, 0.0, 0.0, 1.0);
            instance.normalColumns[1] = float4(0.0, 1.0, 0.0, 0.0);
            instance.normalColumns[2] = float4(0.0, 0.0, 1.0, 0.0);
            RayInstanceData metadata;
            metadata.info = uint4(0u, 0u, 0u, 0u);
            RaySurfacePoint point = ReconstructRaySurface(uv, triangle, metadata, instance,
                query.previousMoment.xy, query.controls.y != 0u);
            result.valuePdf = float4(point.texelDensity, 0.0, float(point.valid));
            result.directionPdf = point.uv;
            result.auxiliary = float4(point.position, 1.0);
        }
        else if (operation == 26u)
            result.valuePdf.x = RayConeSolidAngleSpread(float(query.controls.z));
        else
        {
            float gradient = max(query.previousMean.x, 0.0);
            result.valuePdf = contractTexture.SampleGrad(contractSampler, query.light.xy,
                float2(gradient, 0.0), float2(0.0, gradient));
            result.directionPdf = contractTexture.SampleLevel(contractSampler,
                query.light.xy, max(query.previousMean.y, 0.0));
        }
    }
    results[tid.x] = result;
}
