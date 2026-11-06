#include "path_transport_common.slang"
#include "path_dielectric_common.slang"
#include "material_data_common.slang"
#include "area_light_common.slang"
#include "ray_surface_common.slang"
#include "ray_footprint_common.slang"
#include "push_constants_common.slang"
#ifndef REFERENCE_EMPTY_SCENE
#define REFERENCE_EMPTY_SCENE 0
#endif

[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> materials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> textureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> lighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> pointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> areaLights;

struct FrameData
{
    column_major float4x4 inverseProjection;
    column_major float4x4 inverseView;
    float4 sectionPlane;
    BoxClipState boxClip;
    uint4 limits;
    uint4 extentSamples;
    uint4 sampling;
    float4 environmentRadiance;
    float4 trace;
};
[[vk::binding(0, 2)]] ConstantBuffer<FrameData> frame;
#if !REFERENCE_EMPTY_SCENE
[[vk::binding(1, 2)]] RaytracingAccelerationStructure scene;
[[vk::binding(2, 2)]] StructuredBuffer<RayTriangleData> triangles;
[[vk::binding(3, 2)]] StructuredBuffer<RayInstanceData> instances;
[[vk::binding(4, 2)]] StructuredBuffer<RaySurfaceTriangleData> surfaceTriangles;
[[vk::binding(5, 2)]] StructuredBuffer<RaySurfaceInstanceData> surfaceInstances;
#endif
[[vk::binding(6, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> means;
[[vk::binding(7, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> moments;
[[vk::binding(8, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> display;
[[vk::binding(9, 2)]] TextureCube<float4> environment;
[[vk::binding(10, 2)]] SamplerState environmentSampler;
[[vk::binding(11, 2)]] RWStructuredBuffer<uint4> diagnostics;

float4 PtTextureGrad(uint index, float4 uv, GpuTextureTransform transform, float2 texGrad)
{
    if (index == kInvalidGpuMaterialTexture) return 1.0.xxxx;
    if (index >= MATERIAL_TEXTURE_DESCRIPTOR_COUNT) return asfloat(0x7fc00000u).xxxx;
    uint texture = NonUniformResourceIndex(index);
    uint sampler = NonUniformResourceIndex(ResolveMaterialSamplerIndex(textureMetadata[texture]));
    float2 coord = ApplyGpuTextureTransform(uv.xy, uv.zw, transform);
    float gradient = transform.row0.w >= 0.5 ? texGrad.y : texGrad.x;
    if (gradient > 0.0)
    {
        // Secondary footprints (#82): an isotropic UV gradient in the
        // authored coordinate set, mapped through KHR_texture_transform's
        // linear rows, selects the filtered mip like the reference
        // renderer's SampleGrad route.
        return materialTextures[texture].SampleGrad(materialSamplers[sampler],
            coord, transform.row0.xy * gradient, transform.row1.xy * gradient);
    }
    return materialTextures[texture].SampleLevel(materialSamplers[sampler], coord, 0.0);
}
float4 PtTexture(uint index, float4 uv, GpuTextureTransform transform)
{
    // Coverage candidates and shadow-connection blockers carry no footprint.
    return PtTextureGrad(index, uv, transform, 0.0.xx);
}
float PtScalarGrad(float factor, uint index, float4 uv, GpuTextureTransform transform,
                   uint channel, float2 texGrad)
{
    return factor * SelectGpuTextureChannel(
        PtTextureGrad(index, uv, transform, texGrad), transform, channel);
}
float PtScalar(float factor, uint index, float4 uv, GpuTextureTransform transform, uint channel)
{
    return PtScalarGrad(factor, index, uv, transform, channel, 0.0.xx);
}
struct PtHit { RaySurfacePoint point; float distance; uint hit; uint fault; };
uint PtCoverageSeed(uint pixel, uint sampleIndex, uint bounce, uint rayKind)
{
    uint value = PtHash(pixel ^ PtHash(frame.sampling.z));
    value = PtHash(value ^ sampleIndex);
    value = PtHash(value ^ bounce);
    return PtHash(value ^ (0x414c5048u + rayKind));
}
float PtCoverageRandom(uint seed, uint instanceIndex, uint primitive)
{
    // Candidate enumeration order is unspecified. Address decisions by the
    // stable instance/triangle, never by the traversal iteration number.
    uint value = PtHash(PtHash(seed ^ instanceIndex) ^ primitive);
    return (float(value >> 9u) + 0.5) * (1.0 / 8388608.0);
}
PtHit PtClosest(RayDesc ray, uint coverageSeed, bool volumeOnly = false)
{
    PtHit result = (PtHit)0;
#if !REFERENCE_EMPTY_SCENE
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint instanceIndex = query.CandidateInstanceID(), primitive = query.CandidatePrimitiveIndex();
        float3 position = ray.Origin + ray.Direction * query.CandidateTriangleRayT();
        if (!volumeOnly && (SectionPlaneClips(position, frame.limits.w & 1u, frame.sectionPlane) ||
            BoxClipPlanesClip(position, frame.boxClip))) continue;
        if (instanceIndex >= frame.limits.x) { result.fault = 2u; continue; }
        RayInstanceData instance = instances[instanceIndex];
        if (instance.info.x >= frame.limits.y || primitive >= frame.limits.y - instance.info.x ||
            instance.info.y >= frame.limits.z) { result.fault = 2u; continue; }
        GpuMaterial material = materials[instance.info.y];
        uint flags = instance.info.z | material.flags;
        bool volume = material.thicknessFactor > 0.0 && material.transmissionFactor > 0.0;
        if (volume)
        {
            RayTriangleData triangle = triangles[instance.info.x + primitive];
            float2 bary = query.CandidateTriangleBarycentrics();
            float4 uv = triangle.uv[0] * (1.0 - bary.x - bary.y) + triangle.uv[1] * bary.x + triangle.uv[2] * bary.y;
            float metallic = material.metallicRoughness.x * PtTexture(material.metallicRoughnessTextureIndex, uv,
                material.metallicRoughnessTextureTransform).b * PtScalar(1.0, material.metalnessTextureIndex,
                uv, material.metalnessTextureTransform, 0u);
            volume = metallic < 1.0;
        }
        if (volumeOnly && !volume) continue;
        if (!query.CandidateTriangleFrontFace() && !volume && (flags & kGpuMaterialFlagDoubleSided) == 0u) continue;
        if (volume && (flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) != 0u)
        { result.fault = 1u; continue; } // Coverage holes do not define a closed medium.
        if ((flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) != 0u)
        {
            RayTriangleData triangle = triangles[instance.info.x + primitive];
            float2 bary = query.CandidateTriangleBarycentrics();
            float4 uv = triangle.uv[0] * (1.0 - bary.x - bary.y) + triangle.uv[1] * bary.x + triangle.uv[2] * bary.y;
            float alpha = material.color.a * RayTriangleColorAlpha(triangle, bary) * material.opacityFactor *
                PtTexture(material.baseColorTextureIndex, uv, material.baseColorTextureTransform).a *
                PtScalar(1.0, material.opacityTextureIndex, uv, material.opacityTextureTransform, 0u);
            if (!isfinite(alpha)) { result.fault = 2u; continue; }
            if ((flags & kGpuMaterialFlagAlphaMask) != 0u)
            {
                if (alpha < material.alphaCutoff) continue;
            }
            else if (PtCoverageRandom(coverageSeed, instanceIndex, primitive) >= saturate(alpha)) continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    if (result.fault != 0u || query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return result;
    uint instanceIndex = query.CommittedInstanceID();
    RayInstanceData instance = instances[instanceIndex];
    uint triangle = instance.info.x + query.CommittedPrimitiveIndex();
    result.point = ReconstructRaySurface(triangles[triangle], surfaceTriangles[triangle], instance,
        surfaceInstances[instanceIndex], query.CommittedTriangleBarycentrics(), query.CommittedTriangleFrontFace());
    result.distance = query.CommittedRayT();
    result.hit = 1u;
    if (result.point.valid == 0u) { result.fault = 2u; InterlockedOr(diagnostics[0].w, 8u); }
#endif
    return result;
}
struct PtMaterial
{
    PtBsdf bsdf;
    float3 transmission;
    float transmissionRoughness;
    float eta;
    float3 base;
    float3 specularColor;
    float specular;
    float metallic;
    float transmissionFactor;
    uint volume;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float3 emission;
    uint fault;
};
PtThinBsdf PtTransport(PtMaterial material)
{
    PtThinBsdf result;
    result.reflection = material.bsdf;
    result.transmission = material.transmission;
    result.roughness = material.transmissionRoughness;
    return result;
}
PtVolumeMixture PtVolumeTransport(PtMaterial material)
{
    PtVolumeMixture result;
    result.dielectric.reflection = material.bsdf;
    result.dielectric.reflection.diffuse = material.base * (1.0 - material.transmissionFactor);
    float ratio = (material.eta - 1.0) / (material.eta + 1.0);
    result.dielectric.reflection.f0 = material.specularColor * (ratio * ratio);
    result.dielectric.reflection.specularScale = material.specular;
    result.dielectric.transmission = material.base * material.transmissionFactor;
    result.dielectric.eta = material.eta;
    result.conductor = material.base;
    result.metallic = material.metallic;
    return result;
}
void PtSetVolumeEta(inout PtMaterial material, float eta)
{
    material.eta = eta;
}
PtMaterial PtResolveMaterial(RaySurfacePoint input, float2 texGrad)
{
    PtMaterial result = (PtMaterial)0;
    GpuMaterial m = materials[input.materialIndex];
    uint flags = input.objectFlags | m.flags;
    if (!isfinite(m.transmissionFactor) || !isfinite(m.thicknessFactor) || m.thicknessFactor < 0.0 ||
        !isfinite(m.dispersion) || m.dispersion < 0.0 || !isfinite(m.clearcoatFactor) ||
        !all(isfinite(m.sheenColorFactor)) || !isfinite(m.iridescenceFactor) ||
        !all(isfinite(m.anisotropyDiffuseFactors)) || !isfinite(m.heightScale))
    { result.fault = 2u; return result; }
    // Never silently substitute opaque or unlayered transport for unsupported
    // material models. Such paths are diagnosed and excluded from raw moments.
    if ((flags & (0x8u | 0x10u)) != 0u ||
        m.clearcoatFactor != 0.0 ||
        any(m.sheenColorFactor.rgb != 0.0.xxx) || m.iridescenceFactor != 0.0 ||
        m.anisotropyDiffuseFactors.x != 0.0 || m.anisotropyDiffuseFactors.z != 0.0 ||
        (m.heightTextureIndex != kInvalidGpuMaterialTexture && m.heightScale != 0.0))
    { result.fault = 1u; return result; }
    RaySurfacePoint point = OrientRaySurfaceForShading(input, m.flags |
        (m.thicknessFactor > 0.0 && m.transmissionFactor > 0.0 ? kGpuMaterialFlagDoubleSided : 0u));
    float3 base = m.color.rgb * point.color * PtTextureGrad(m.baseColorTextureIndex,
        point.uv, m.baseColorTextureTransform, texGrad).rgb;
    float4 packed = PtTextureGrad(m.metallicRoughnessTextureIndex, point.uv,
        m.metallicRoughnessTextureTransform, texGrad);
    float metallic = m.metallicRoughness.x * packed.b *
        PtScalarGrad(1.0, m.metalnessTextureIndex, point.uv, m.metalnessTextureTransform, 0u, texGrad);
    float roughness = m.metallicRoughness.y * packed.g *
        PtScalarGrad(1.0, m.roughnessTextureIndex, point.uv, m.roughnessTextureTransform, 0u, texGrad);
    float specular = PtScalarGrad(m.specularFactor, m.specularTextureIndex, point.uv,
        m.specularTextureTransform, 3u, texGrad);
    float3 specularColor = m.specularColorFactor.rgb * PtTextureGrad(m.specularColorTextureIndex,
        point.uv, m.specularColorTextureTransform, texGrad).rgb;
    float transmission = PtScalarGrad(m.transmissionFactor, m.transmissionTextureIndex,
        point.uv, m.transmissionTextureTransform, 0u, texGrad);
    if (!isfinite(m.ior) || m.ior < 1.0 || !isfinite(metallic) || metallic < 0.0 || metallic > 1.0 ||
        !isfinite(specular) || specular < 0.0 || specular > 1.0 ||
        !isfinite(transmission) || transmission < 0.0 || transmission > 1.0) { result.fault = 2u; return result; }
    if (transmission > 0.0 && metallic < 1.0 && m.dispersion > 0.0)
    { result.fault = 1u; return result; }
    result.volume = m.thicknessFactor > 0.0 && m.transmissionFactor > 0.0 && metallic < 1.0 ? 1u : 0u;
    if (result.volume != 0u && !PtValidAbsorption(m.attenuationColor.rgb, m.attenuationDistance))
    { result.fault = 2u; return result; }
    float dielectric = (m.ior - 1.0) / (m.ior + 1.0);
    dielectric *= dielectric;
    float scale = metallic + (1.0 - metallic) * specular;
    result.bsdf.diffuse = base * (1.0 - metallic) * (1.0 - transmission);
    result.transmission = base * (1.0 - metallic) * transmission;
    // The glTF thin-sheet rough lobe narrows as IOR approaches one. At IOR one
    // it is straight-through even with a rough reflection lobe.
    result.transmissionRoughness = roughness * clamp(2.0 * m.ior - 2.0, 0.0, 1.0);
    result.bsdf.roughness = roughness;
    result.bsdf.f0 = 0.0.xxx;
    [branch] if (scale > 0.0)
    {
        result.bsdf.f0 = (dielectric * specularColor * specular * (1.0 - metallic) + base * metallic) / scale;
    }
    result.bsdf.specularScale = scale;
    result.eta = m.ior;
    result.base = base;
    result.specularColor = specularColor;
    result.specular = specular;
    result.metallic = metallic;
    result.transmissionFactor = transmission;
    result.normal = point.shadingNormal;
    if (m.normalTextureIndex != kInvalidGpuMaterialTexture)
    {
        float3 normal = PtTextureGrad(m.normalTextureIndex, point.uv,
            m.normalTextureTransform, texGrad).xyz * 2.0 - 1.0;
        normal.xy *= m.normalTextureScale;
        result.normal = PtNormalize(point.tangent.xyz * normal.x + point.bitangent * normal.y + point.shadingNormal * normal.z);
        if (dot(result.normal, point.shadingNormal) < 0.0) result.normal = -result.normal;
    }
    result.tangent = PtNormalize(point.tangent.xyz - result.normal * dot(point.tangent.xyz, result.normal));
    if (all(result.tangent == 0.0.xxx)) result.tangent = RaySurfaceFallbackTangent(result.normal);
    result.bitangent = cross(result.normal, result.tangent);
    result.emission = m.emissiveColor * m.emissiveStrength * PtTextureGrad(m.emissiveTextureIndex,
        point.uv, m.emissiveTextureTransform, texGrad).rgb;
    if (!PtValidThinBsdf(PtTransport(result)) || !PtValidDirection(result.normal) ||
        !all(isfinite(result.emission)) || any(result.emission < 0.0.xxx))
    { result.fault = 2u; InterlockedOr(diagnostics[0].w, 4u); }
    return result;
}
float3 PtLocal(PtMaterial m, float3 value)
{ return float3(dot(value, m.tangent), dot(value, m.bitangent), dot(value, m.normal)); }
float3 PtWorld(PtMaterial m, float3 value)
{ return PtNormalize(m.tangent * value.x + m.bitangent * value.y + m.normal * value.z); }
uint4 PtLightCounts()
{
    return uint4(lighting.environmentIntensity > 0.0 ? 1u : 0u,
        any(lighting.directionalColorIntensity.rgb * lighting.directionalColorIntensity.a > 0.0.xxx) ? 1u : 0u,
        lighting.pointLightCount, lighting.areaLightCount);
}
uint PtLightCount(uint4 counts) { return counts.x + counts.y + counts.z + counts.w; }
float3 PtEnvironment(float3 direction)
{
    float3 value = frame.environmentRadiance.rgb;
    if ((frame.limits.w & 4u) != 0u)
        value = environment.SampleLevel(environmentSampler,
            RotateEnvironmentDirection(direction, lighting.environmentOrientation.xy), 0.0).rgb;
    return value * lighting.environmentIntensity;
}
struct PtEmitterHit { float distance; uint index; uint hit; float3 position; };
PtEmitterHit PtAreaHit(RayDesc ray, uint count, uint ignoredEmitter)
{
    PtEmitterHit hit = (PtEmitterHit)0;
    hit.distance = ray.TMax;
    for (uint i = 0u; i < count; ++i)
    {
        if (i == ignoredEmitter) continue;
        AreaLightFrame light = BuildAreaLightFrame(areaLights[i]);
        float denominator = dot(ray.Direction, light.normal);
        if (denominator == 0.0) continue;
        float distance = dot(light.position - ray.Origin, light.normal) / denominator;
        if (!isfinite(distance) || distance < ray.TMin || distance >= hit.distance) continue;
        float3 point = ray.Origin + ray.Direction * distance;
        float2 offset = float2(dot(point - light.position, light.tangent), dot(point - light.position, light.bitangent));
        bool inside = abs(light.type - AREA_LIGHT_TYPE_DISK) < 0.5 ?
            dot(offset, offset) <= light.halfSize.x * light.halfSize.x : all(abs(offset) <= light.halfSize);
        if (inside) { hit.hit = 1u; hit.distance = distance; hit.index = i; hit.position = point; }
    }
    return hit;
}
float PtAreaPdf(AreaLightFrame light, float3 origin, float3 target, uint lightCount)
{
    float3 offset = target - origin;
    float square = dot(offset, offset);
    if (square == 0.0) return 0.0;
    float cosine = dot(light.normal, -PtNormalize(offset));
    return cosine > 0.0 ? square / (light.area * cosine * float(lightCount)) : 0.0;
}
float PtBias(float3 position)
{ return max(frame.trace.x, max(max(abs(position.x), abs(position.y)), abs(position.z)) * 2e-7); }
static const uint PT_MAX_MEDIA = 8u;
static const uint PT_MAX_MEDIUM_PROBE_HITS = 128u;
struct PtMediumState { uint material[PT_MAX_MEDIA]; uint count; };
bool PtCompatibleMedium(uint a, uint b)
{
    GpuMaterial x = materials[a], y = materials[b];
    return x.ior == y.ior && x.attenuationDistance == y.attenuationDistance && all(x.attenuationColor.rgb == y.attenuationColor.rgb);
}
float PtMediumIor(PtMediumState state)
{
    if (state.count == 0u) return 1.0;
    return materials[state.material[state.count - 1u]].ior;
}
float3 PtMediumAttenuation(PtMediumState state, float distance)
{
    if (state.count == 0u) return 1.0.xxx;
    GpuMaterial medium = materials[state.material[state.count - 1u]];
    return PtAbsorption(medium.attenuationColor.rgb, medium.attenuationDistance, distance);
}
bool PtCrossMedium(inout PtMediumState state, uint material, bool frontFace, out float eta, inout uint fault)
{
    float incidentIor = PtMediumIor(state);
    if (frontFace)
    {
        if (state.count == PT_MAX_MEDIA) { fault = 1u; eta = 1.0; return false; }
        state.material[state.count++] = material;
    }
    else
    {
        if (state.count == 0u) { fault = 1u; eta = 1.0; return false; }
        if (!PtCompatibleMedium(state.material[state.count - 1u], material))
        { fault = 1u; eta = 1.0; return false; }
        --state.count;
    }
    eta = PtMediumIor(state) / incidentIor;
    return true;
}
PtMediumState PtInitialMedium(RayDesc ray, uint seed, inout uint fault)
{
    PtMediumState inside = (PtMediumState)0, scan = (PtMediumState)0;
    if ((frame.limits.w & 16u) == 0u) return inside;
    // Infer containment from oriented boundary crossings independently of
    // opaque surfaces, primary near/far clipping and scene inspection planes.
    // Unmatched exits identify enclosing media, nearest/innermost first.
    ray.TMin = 0.0;
    ray.TMax = frame.trace.y;
    for (uint step = 0u; step <= PT_MAX_MEDIUM_PROBE_HITS; ++step)
    {
        PtHit hit = (PtHit)0;
        if (ray.TMax > ray.TMin) hit = PtClosest(ray, seed, true);
        if (hit.fault != 0u) { fault = hit.fault; return inside; }
        if (hit.hit == 0u)
        {
            if (scan.count != 0u) { fault = 1u; return inside; }
            for (uint i = 0u; i < inside.count / 2u; ++i)
            {
                uint value = inside.material[i];
                inside.material[i] = inside.material[inside.count - 1u - i];
                inside.material[inside.count - 1u - i] = value;
            }
            return inside;
        }
        if (step == PT_MAX_MEDIUM_PROBE_HITS || (frame.limits.w & 1u) != 0u || frame.boxClip.planeCount != 0u)
        { fault = 1u; return inside; }
        GpuMaterial material = materials[hit.point.materialIndex];
        if (!isfinite(material.ior) || material.ior < 1.0 || !PtValidAbsorption(material.attenuationColor.rgb, material.attenuationDistance))
        { fault = 2u; return inside; }
        if (hit.point.frontFace != 0u)
        {
            if (scan.count == PT_MAX_MEDIA) { fault = 1u; return inside; }
            scan.material[scan.count++] = hit.point.materialIndex;
        }
        else if (scan.count != 0u)
        {
            if (!PtCompatibleMedium(scan.material[scan.count - 1u], hit.point.materialIndex))
            { fault = 1u; return inside; }
            --scan.count;
        }
        else
        {
            if (inside.count == PT_MAX_MEDIA) { fault = 1u; return inside; }
            inside.material[inside.count++] = hit.point.materialIndex;
        }
        ray.TMin = hit.distance + PtBias(hit.point.position);
    }
    fault = 1u;
    return inside;
}
PtBsdfEvaluation PtEvaluateSurface(PtMaterial material, float3 view, float3 light)
{
    if (material.volume != 0u) return PtEvaluateVolumeMixture(PtVolumeTransport(material), view, light);
    return PtEvaluateThinBsdf(PtTransport(material), view, light);
}
PtBsdfSample PtSampleSurface(PtMaterial material, float3 view, float3 u)
{
    if (material.volume != 0u) return PtSampleVolumeMixture(PtVolumeTransport(material), view, u);
    return PtSampleThinBsdf(PtTransport(material), view, u);
}
static const uint PT_MAX_NULL_SHEETS = 64u;
bool PtVisible(float3 position, float3 normal, float3 direction, float distance, uint areaCount,
    uint ignoredEmitter, uint coverageSeed, PtMediumState medium, bool deltaLight,
    out float3 transmittance, out float nullPdf, inout uint fault)
{
    transmittance = 1.0.xxx;
    nullPdf = 1.0;
    float bias = PtBias(position);
    RayDesc ray;
    ray.Origin = position + normal * bias;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = distance - 2.0 * bias;
    if (ray.TMax <= 0.0) return false;
    float3 segmentStart = position;
    for (uint sheet = 0u; sheet <= PT_MAX_NULL_SHEETS; ++sheet)
    {
        PtHit hit = PtClosest(ray, coverageSeed);
        if (hit.fault != 0u) { fault = hit.fault; return false; }
        PtEmitterHit emitter = PtAreaHit(ray, areaCount, ignoredEmitter);
        if (emitter.hit != 0u && (hit.hit == 0u || emitter.distance < hit.distance)) return false;
        if (hit.hit == 0u)
        {
            // An unbounded environment connection cannot remain inside a
            // closed volume. Finite lights may legitimately be in that medium.
            if (distance == frame.trace.y && medium.count != 0u) { fault = 1u; return false; }
            transmittance *= PtMediumAttenuation(medium, distance - dot(segmentStart - position, direction));
            return true;
        }
        transmittance *= PtMediumAttenuation(medium, PtLength(hit.point.position - segmentStart));
        PtMaterial blocker = PtResolveMaterial(hit.point, 0.0.xx);
        if (blocker.fault != 0u) { fault |= blocker.fault; return false; }
        if (blocker.volume != 0u)
        {
            PtMediumState crossed = medium;
            float eta;
            if (!PtCrossMedium(crossed, hit.point.materialIndex, hit.point.frontFace != 0u, eta, fault)) return false;
            PtSetVolumeEta(blocker, eta);
            if (eta != 1.0)
            {
                // Unidirectional NEE cannot connect a delta point/directional
                // emitter through a bending specular boundary (a caustic).
                if (deltaLight && blocker.bsdf.roughness == 0.0 && any(blocker.transmission > 0.0.xxx)) fault = 1u;
                return false;
            }
            medium = crossed;
        }
        // Only a delta straight-through sheet belongs on an unbent shadow
        // connection. Rough transmission is sampled by actual BSDF paths.
        if (blocker.volume == 0u && (blocker.transmissionRoughness != 0.0 || all(blocker.transmission == 0.0.xxx))) return false;
        if (sheet == PT_MAX_NULL_SHEETS) { fault |= 1u; return false; }
        float3 view = PtLocal(blocker, -direction);
        if (view.z <= 0.0) return false;
        float3 coefficient;
        float probability;
        if (blocker.volume != 0u)
            coefficient = PtVolumeNullTransmission(PtVolumeTransport(blocker), view, probability);
        else
        {
            PtThinBsdf bsdf = PtTransport(blocker);
            coefficient = bsdf.transmission * (1.0.xxx - PtFresnel(bsdf.reflection.f0, view.z) * bsdf.reflection.specularScale);
            probability = PtThinProbability(bsdf, view);
        }
        if (all(coefficient == 0.0.xxx)) return false;
        transmittance *= coefficient;
        nullPdf *= probability;
        segmentStart = hit.point.position;
        ray.TMin = hit.distance + PtBias(hit.point.position);
        if (ray.TMin >= ray.TMax)
        {
            transmittance *= PtMediumAttenuation(medium, max(0.0, distance - dot(segmentStart - position, direction)));
            return true;
        }
    }
    return false;
}
float3 PtDirect(PtHit hit, PtMaterial material, float3 view, float3 geometricNormal,
    uint pixel, uint sampleIndex, uint bounce, uint4 counts, bool useMis, PtMediumState medium, inout uint fault)
{
    uint count = PtLightCount(counts);
    if (count == 0u) return 0.0.xxx;
    uint index = min(uint(PtRandom(pixel, sampleIndex, bounce, 2u, frame.sampling.z) * float(count)), count - 1u);
    float2 u = float2(PtRandom(pixel, sampleIndex, bounce, 3u, frame.sampling.z),
                     PtRandom(pixel, sampleIndex, bounce, 4u, frame.sampling.z));
    float3 direction = 0.0.xxx, radiance = 0.0.xxx;
    float distance = frame.trace.y, pdf = 1.0 / float(count);
    bool delta = true;
    uint ignoredEmitter = 0xffffffffu;
    if (index < counts.x)
    {
        direction = PtUniformSphere(u);
        radiance = PtEnvironment(direction);
        pdf /= 4.0 * PT_PI;
        delta = false;
    }
    else if (index < counts.x + counts.y)
    {
        direction = PtNormalize(-lighting.directionalDirection.xyz);
        radiance = lighting.directionalColorIntensity.rgb * lighting.directionalColorIntensity.a;
    }
    else if (index < counts.x + counts.y + counts.z)
    {
        PointLightData light = pointLights[index - counts.x - counts.y];
        float3 offset = light.positionRadius.xyz - hit.point.position;
        float square = dot(offset, offset);
        if (square <= 0.0) { fault = 2u; return 0.0.xxx; }
        distance = sqrt(square);
        direction = offset / distance;
        radiance = light.colorIntensity.rgb * light.colorIntensity.a *
            (SmoothRangeAttenuation(square, light.positionRadius.w) / square) *
            SpotLightConeAttenuation(light.directionInnerCos, light.coneOuterCosType, direction);
    }
    else
    {
        ignoredEmitter = index - counts.x - counts.y - counts.z;
        AreaLightData data = areaLights[ignoredEmitter];
        AreaLightFrame light = BuildAreaLightFrame(data);
        float2 offset = 2.0 * u - 1.0;
        if (abs(light.type - AREA_LIGHT_TYPE_DISK) < 0.5)
            offset = sqrt(u.x) * float2(cos(2.0 * PT_PI * u.y), sin(2.0 * PT_PI * u.y));
        float3 target = light.position + light.tangent * offset.x * light.halfSize.x + light.bitangent * offset.y * light.halfSize.y;
        direction = PtNormalize(target - hit.point.position);
        distance = PtLength(target - hit.point.position);
        pdf = PtAreaPdf(light, hit.point.position, target, count);
        radiance = data.colorIntensity.rgb * data.colorIntensity.a * SmoothRangeAttenuation(distance * distance, data.positionRange.w);
        delta = false;
    }
    if (!PtValidDirection(direction)) { fault = 2u; return 0.0.xxx; }
    float3 localDirection = PtLocal(material, direction);
    if (pdf <= 0.0 || localDirection.z * dot(direction, geometricNormal) <= 0.0) return 0.0.xxx;
    PtBsdfEvaluation evaluated = PtEvaluateSurface(material, PtLocal(material, view), localDirection);
    if (evaluated.valid == 0u || !isfinite(pdf) || !all(isfinite(radiance)) || any(radiance < 0.0.xxx))
    { fault = 2u; return 0.0.xxx; }
    if (all(evaluated.value == 0.0.xxx) || all(radiance == 0.0.xxx)) return 0.0.xxx;
    float3 transmittance;
    float nullPdf;
    float3 offsetNormal = geometricNormal * (localDirection.z < 0.0 ? -1.0 : 1.0);
    if (material.volume != 0u && localDirection.z < 0.0)
    {
        float eta;
        if (!PtCrossMedium(medium, hit.point.materialIndex, hit.point.frontFace != 0u, eta, fault)) return 0.0.xxx;
    }
    if (!PtVisible(hit.point.position, offsetNormal, direction, distance, counts.w, ignoredEmitter,
        PtCoverageSeed(pixel, sampleIndex, bounce, 1u), medium, delta, transmittance, nullPdf, fault)) return 0.0.xxx;
    float weight = !delta && useMis ? PtPowerHeuristic(pdf, evaluated.pdf * nullPdf) : 1.0;
    return radiance * transmittance * evaluated.value * (abs(dot(material.normal, direction)) * weight / pdf);
}
float3 PtTracePath(uint2 pixel, uint sampleIndex, out uint fault)
{
    fault = 0u;
    uint address = pixel.x + pixel.y * frame.extentSamples.x;
    float2 jitter = float2(PtRandom(address, sampleIndex, 0u, 0u, frame.sampling.z),
                          PtRandom(address, sampleIndex, 0u, 1u, frame.sampling.z));
    // Float32 addition can round a jittered last-column/row sample onto the
    // next pixel (and past the image bound). Keep it inside its owning pixel;
    // this only bounds coordinate rounding, never the path's radiance.
    float2 pixelUpper = asfloat(asuint(float2(pixel + 1u)) - uint2(1u, 1u));
    float2 samplePosition = min(float2(pixel) + jitter, pixelUpper);
    PtCameraRay primary = PtGenerateCameraRay(samplePosition, frame.extentSamples.xy,
        frame.inverseProjection, frame.inverseView, (frame.limits.w & 2u) != 0u);
    if (primary.valid == 0u) { fault = 2u; InterlockedOr(diagnostics[0].w, 16u); return 0.0.xxx; }
    // Secondary footprints (#82): the camera cone starts from the angular
    // spacing between neighbouring sample rays (perspective) or from the
    // sample's world footprint itself (orthographic, where neighbouring
    // directions are parallel). Chords between unit directions approximate
    // the full cone angle.
    RayCone cone = (RayCone)0;
    {
        PtCameraRay adjacent = PtGenerateCameraRay(samplePosition + float2(1.0, 0.0),
            frame.extentSamples.xy, frame.inverseProjection, frame.inverseView,
            (frame.limits.w & 2u) != 0u);
        if (adjacent.valid == 0u)
            adjacent = PtGenerateCameraRay(samplePosition + float2(0.0, 1.0),
                frame.extentSamples.xy, frame.inverseProjection, frame.inverseView,
                (frame.limits.w & 2u) != 0u);
        if (adjacent.valid != 0u)
        {
            cone.spread = PtLength(adjacent.direction - primary.direction);
            cone.width = PtLength(adjacent.origin - primary.origin);
        }
        else cone.spread = 2.0 / float(max(frame.extentSamples.y, 1u));
    }
    uint pointCount, stride, areaCount, materialCount;
    materials.GetDimensions(materialCount, stride);
    if (frame.limits.z > materialCount) { fault = 2u; return 0.0.xxx; }
    pointLights.GetDimensions(pointCount, stride);
    areaLights.GetDimensions(areaCount, stride);
    uint4 counts = PtLightCounts();
    if (counts.z > min(pointCount, MAX_CLUSTERED_LIGHTS) || counts.w > min(areaCount, MAX_AREA_LIGHTS))
    { fault = 2u; return 0.0.xxx; }
    uint lightCount = PtLightCount(counts);
    RayDesc ray;
    ray.Origin = primary.origin;
    ray.Direction = primary.direction;
    ray.TMin = primary.minimum;
    ray.TMax = min(primary.maximum, frame.trace.y);
    if (ray.TMax <= ray.TMin)
    {
        float3 value = PtEnvironment(primary.direction);
        if (!all(isfinite(value)) || any(value < 0.0.xxx)) { fault = 2u; return 0.0.xxx; }
        return value;
    }
    float3 result = 0.0.xxx, throughput = 1.0.xxx, previousPosition = primary.origin;
    float previousPdf = 0.0;
    float etaScale = 1.0;
    bool previousDelta = true;
    PtMediumState medium = PtInitialMedium(ray, PtCoverageSeed(address, sampleIndex, 0u, 2u), fault);
    if (fault != 0u) return 0.0.xxx;
    float3 segmentStart = ray.Origin;
    uint depth = 0u, nullSheets = 0u;
    for (uint bounce = 0u; bounce < frame.sampling.x + PT_MAX_NULL_SHEETS; ++bounce)
    {
        PtHit hit = PtClosest(ray, PtCoverageSeed(address, sampleIndex, bounce, 0u));
        if (hit.fault != 0u) { fault = hit.fault; return 0.0.xxx; }
        PtEmitterHit emitter = PtAreaHit(ray, counts.w, 0xffffffffu);
        if (emitter.hit != 0u && (hit.hit == 0u || emitter.distance < hit.distance))
        {
            throughput *= PtMediumAttenuation(medium, PtLength(emitter.position - segmentStart));
            AreaLightData light = areaLights[emitter.index];
            AreaLightFrame basis = BuildAreaLightFrame(light);
            if (dot(basis.normal, -ray.Direction) > 0.0)
            {
                float pdf = PtAreaPdf(basis, previousPosition, emitter.position, lightCount);
                float weight = bounce == 0u || previousDelta ? 1.0 : PtPowerHeuristic(previousPdf, pdf);
                float square = dot(emitter.position - previousPosition, emitter.position - previousPosition);
                result += throughput * light.colorIntensity.rgb * light.colorIntensity.a * SmoothRangeAttenuation(square, light.positionRange.w) * weight;
            }
            break;
        }
        if (hit.hit == 0u)
        {
            if (medium.count != 0u) { fault = 1u; return 0.0.xxx; }
            float pdf = lightCount > 0u && counts.x != 0u ? 1.0 / (4.0 * PT_PI * float(lightCount)) : 0.0;
            float weight = bounce == 0u || previousDelta ? 1.0 : PtPowerHeuristic(previousPdf, pdf);
            result += throughput * PtEnvironment(ray.Direction) * weight;
            break;
        }
        // Secondary footprints (#82): the committed hit shades with the
        // ray cone's slant-corrected world diameter, and every continued
        // segment inherits it as its starting width, exactly like the
        // reference renderer's surface-bounce cone propagation.
        float worldFoot = RayConeWorldFootprint(cone, hit.distance,
            hit.point.geometricNormal, -ray.Direction);
        float2 texGrad = worldFoot * hit.point.texelDensity;
        cone.width = worldFoot;
        PtMaterial material = PtResolveMaterial(hit.point, texGrad);
        if (material.fault != 0u) { fault = material.fault; return 0.0.xxx; }
        throughput *= PtMediumAttenuation(medium, PtLength(hit.point.position - segmentStart));
        PtMediumState crossed = medium;
        if (material.volume != 0u)
        {
            float eta;
            if (!PtCrossMedium(crossed, hit.point.materialIndex, hit.point.frontFace != 0u, eta, fault)) return 0.0.xxx;
            PtSetVolumeEta(material, eta);
        }
        result += throughput * material.emission; // Emissive mesh triangles use the BSDF proposal only.
        float3 geometricNormal = hit.point.geometricNormal * (hit.point.frontFace != 0u ? 1.0 : -1.0);
        float3 view = -ray.Direction;
        if (dot(material.normal, view) <= 0.0) break;
        bool continuePath = depth + 1u < frame.sampling.x;
        result += throughput * PtDirect(hit, material, view, geometricNormal, address, sampleIndex, bounce, counts, continuePath, medium, fault);
        if (fault != 0u) return 0.0.xxx;
        bool canPassNull = material.volume != 0u ? material.eta == 1.0 : material.transmissionRoughness == 0.0;
        if (!continuePath && (!canPassNull || all(material.transmission == 0.0.xxx))) break;
        float3 u = float3(PtRandom(address, sampleIndex, bounce, 5u, frame.sampling.z),
                         PtRandom(address, sampleIndex, bounce, 6u, frame.sampling.z),
                         PtRandom(address, sampleIndex, bounce, 7u, frame.sampling.z));
        PtBsdfSample sampled = PtSampleSurface(material, PtLocal(material, view), u);
        if (sampled.rejected != 0u) { fault = 2u; return 0.0.xxx; }
        if (sampled.valid == 0u) break;
        float3 direction = PtWorld(material, sampled.direction);
        if (sampled.direction.z * dot(direction, geometricNormal) <= 0.0) break;
        bool transmission = sampled.direction.z < 0.0;
        bool nullSheet = sampled.delta != 0u && transmission && (material.volume == 0u || material.eta == 1.0);
        if (!nullSheet && !continuePath) break;
        if (nullSheet && nullSheets == PT_MAX_NULL_SHEETS) { fault = 1u; return 0.0.xxx; }
        throughput *= sampled.weight;
        if (material.volume != 0u && transmission)
        {
            medium = crossed;
            etaScale *= material.eta * material.eta;
        }
        if (!all(isfinite(throughput)) || any(throughput < 0.0.xxx) || !isfinite(etaScale) || etaScale <= 0.0)
        { fault = 2u; return 0.0.xxx; }
        if (nullSheet)
        {
            ++nullSheets;
            // MIS compares the complete path probability, including each
            // stochastic null choice. The competing light proposal connects
            // from the last real scattering point and integrates sheet tint.
            previousPdf *= sampled.pdf;
        }
        else
        {
            ++depth;
            float3 rouletteThroughput = throughput * etaScale;
            if (!all(isfinite(rouletteThroughput))) { fault = 2u; return 0.0.xxx; }
            if (!PtRussianRoulette(rouletteThroughput, depth, frame.sampling.y,
                PtRandom(address, sampleIndex, bounce, 8u, frame.sampling.z))) break;
            throughput = rouletteThroughput / etaScale;
            previousPdf = sampled.pdf;
            previousDelta = sampled.delta != 0u;
            previousPosition = hit.point.position;
        }
        ray.Origin = hit.point.position + geometricNormal * (sampled.direction.z < 0.0 ? -1.0 : 1.0) * PtBias(hit.point.position);
        segmentStart = hit.point.position;
        ray.Direction = direction;
        ray.TMin = 0.0;
        ray.TMax = frame.trace.y;
    }
    if (!all(isfinite(result)) || any(result < 0.0.xxx)) { fault = 2u; return 0.0.xxx; }
    return result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= frame.extentSamples.xy)) return;
    float3 mean = 0.0.xxx, moment = 0.0.xxx;
    uint count = 0u, flags = 0u;
    if (frame.extentSamples.z != 0u)
    {
        float4 old = means[tid.xy], oldMoment = moments[tid.xy];
        mean = old.rgb; count = uint(old.w); moment = oldMoment.rgb; flags = uint(oldMoment.w);
    }
    for (uint i = 0u; i < frame.extentSamples.w; ++i)
    {
        uint fault;
        float3 radiance;
        if ((frame.limits.w & 8u) != 0u) { fault = 1u; radiance = 0.0.xxx; }
        else radiance = PtTracePath(tid.xy, frame.extentSamples.z + i, fault);
        if ((fault & 2u) != 0u) InterlockedOr(diagnostics[0].w, 2u);
        if (fault == 0u && !PtAccumulate(mean, moment, count, radiance))
        { fault = 2u; InterlockedOr(diagnostics[0].w, 1u); }
        if (fault != 0u)
        {
            flags |= fault;
            if ((fault & 1u) != 0u) InterlockedAdd(diagnostics[0].x, 1u);
            if ((fault & 2u) != 0u) InterlockedAdd(diagnostics[0].y, 1u);
        }
        else InterlockedAdd(diagnostics[0].z, 1u);
    }
    means[tid.xy] = float4(mean, float(count));
    moments[tid.xy] = float4(moment, float(flags));
    display[tid.xy] = flags != 0u ? float4(1.0, (flags & 2u) != 0u ? 1.0 : 0.0, (flags & 1u) != 0u ? 1.0 : 0.0, 1.0) : float4(mean, 1.0);
}
