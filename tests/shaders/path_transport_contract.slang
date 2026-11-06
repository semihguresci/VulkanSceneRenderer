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
        PtBsdfEvaluation evaluated = PtEvaluateBsdf(bsdf, query.view.xyz, query.light.xyz, query.footprintC.w);
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
        PtBsdfSample sampled = PtSampleBsdf(bsdf, query.view.xyz, random, query.footprintC.w);
        result.directionPdf = float4(sampled.direction, sampled.pdf);
        result.weightValid = float4(sampled.weight, float(sampled.valid));
        PtBsdfEvaluation evaluated = PtEvaluateBsdf(bsdf, query.view.xyz, sampled.direction, query.footprintC.w);
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
            PtBsdfEvaluation direct = PtEvaluateBsdf(bsdf, query.view.xyz, lightDirection, query.footprintC.w);
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
        float3 diffuseTransmission = query.footprintC.rgb;
        float anisotropy = query.footprintC.w;
        if (operation == 9u)
        {
            PtBsdfEvaluation evaluated = PtEvaluateDiffuseTransmissionBsdf(thin, diffuseTransmission,
                query.view.xyz, query.light.xyz, anisotropy);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.auxiliary.x = (1.0 - PtDiffuseTransmissionProbability(thin, diffuseTransmission, query.view.xyz)) *
                PtThinProbability(thin, query.view.xyz);
            result.auxiliary.z = float(evaluated.valid);
        }
        else
        {
            float3 u = float3(PtRandom(query.controls.z, query.controls.w, 0u, 0u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 1u, query.controls.y),
                             PtRandom(query.controls.z, query.controls.w, 0u, 2u, query.controls.y));
            if (operation == 11u) u = query.previousMean.xyz;
            PtBsdfSample sampled = PtSampleDiffuseTransmissionBsdf(thin, diffuseTransmission, query.view.xyz, u, anisotropy);
            PtBsdfEvaluation evaluated = PtEvaluateDiffuseTransmissionBsdf(thin, diffuseTransmission,
                query.view.xyz, sampled.direction, anisotropy);
            result.valuePdf = float4(evaluated.value, evaluated.pdf);
            result.directionPdf = float4(sampled.direction, sampled.pdf);
            result.weightValid = float4(sampled.weight, float(sampled.valid));
            result.auxiliary = float4(float(sampled.delta), float(sampled.rejected), float(evaluated.valid), 0.0);
            if (operation == 12u)
            {
                float3 light = PtUniformSphere(float2(
                    PtRandom(query.controls.z, query.controls.w, 0u, 3u, query.controls.y),
                    PtRandom(query.controls.z, query.controls.w, 0u, 4u, query.controls.y)));
                PtBsdfEvaluation direct = PtEvaluateDiffuseTransmissionBsdf(thin, diffuseTransmission,
                    query.view.xyz, light, anisotropy);
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
    if (operation >= 24u && operation <= 29u)
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
        else if (operation == 27u)
        {
            float gradient = max(query.previousMean.x, 0.0);
            float2 dx, dy;
            RayConeTransformGradients(gradient, query.footprintA.xy, query.footprintA.zw, dx, dy);
            result.valuePdf = contractTexture.SampleGrad(contractSampler, query.light.xy, dx, dy);
            result.directionPdf = contractTexture.SampleLevel(contractSampler,
                query.light.xy, max(query.previousMean.y, 0.0));
        }
        else if (operation == 28u)
        {
            // Shared KHR_materials_dispersion channel mapping.
            GpuMaterial probe = (GpuMaterial)0;
            probe.ior = query.previousMoment.w;
            probe.dispersion = query.sampleValue.w;
            result.valuePdf = float4(GpuMaterialChannelIors(probe),
                GpuMaterialChannelIor(probe.ior, probe.dispersion, query.controls.z));
        }
        else
        {
            // Spectral dispersion estimator: each uniformly sampled channel
            // transports its own IOR with a 3x weight; the Monte-Carlo mean
            // converges to the per-channel quadrature of the transmitted
            // delta through a smooth white dielectric.
            PtVolumeBsdf volume;
            volume.reflection = bsdf;
            volume.transmission = query.sampleValue.rgb;
            float authoredIor = query.previousMoment.w;
            float dispersion = query.footprintA.x;
            float3 channelWeights = 0.0.xxx;
            for (uint channel = 0u; channel < 3u; ++channel)
            {
                volume.eta = GpuMaterialChannelIor(authoredIor, dispersion, channel);
                PtBsdfSample sampled = PtSampleVolumeBsdf(volume, query.view.xyz,
                    float3(0.99, 0.5, 0.5));
                if (sampled.valid == 0u || sampled.delta == 0u || sampled.direction.z >= 0.0)
                {
                    result.auxiliary = float4(-1.0, float(channel), 0.0, 0.0);
                    channelWeights = -1.0.xxx;
                    break;
                }
                channelWeights[channel] = sampled.weight[channel];
            }
            if (channelWeights.x >= 0.0)
            {
                float3 estimate = 0.0.xxx;
                const uint samples = 8192u;
                for (uint i = 0u; i < samples; ++i)
                {
                    float random = PtRandom(query.controls.z, i, 0u, 9u, query.controls.y);
                    uint channel = min(uint(random * 3.0), 2u);
                    estimate += 3.0 * channelWeights[channel] *
                        (channel == 0u ? float3(1, 0, 0) :
                         channel == 1u ? float3(0, 1, 0) : float3(0, 0, 1));
                }
                result.valuePdf = float4(channelWeights, 1.0);
                result.directionPdf = float4(estimate / float(samples), 0.0);
                result.auxiliary = float4(channelWeights, 1.0);
            }
        }
    }
    if (operation == 30u)
    {
        bool orthographic = query.controls.y != 0u;
        PtCameraRay ray = PtGenerateCameraRay(query.light.xy, query.controls.zw,
            query.inverseProjection, query.inverseView, orthographic);
        if (ray.valid != 0u)
            result.valuePdf = float4(PtCameraRayFootprint(query.light.xy, query.controls.zw,
                query.inverseProjection, query.inverseView, orthographic, ray.direction, 0.0.xxx, 0.0), 0.0, 1.0);
    }
    results[tid.x] = result;
}
