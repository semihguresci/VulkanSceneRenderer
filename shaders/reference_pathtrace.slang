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
    float4 controls;
    float4 ground;
    float4 groundColor;
};
[[vk::binding(0, 2)]] ConstantBuffer<FrameData> frame;
#if !REFERENCE_EMPTY_SCENE
[[vk::binding(1, 2)]] RaytracingAccelerationStructure scene;
[[vk::binding(2, 2)]] StructuredBuffer<RayTriangleData> triangles;
[[vk::binding(3, 2)]] StructuredBuffer<RayInstanceData> instances;
[[vk::binding(4, 2)]] StructuredBuffer<RaySurfaceTriangleData> surfaceTriangles;
[[vk::binding(5, 2)]] StructuredBuffer<RaySurfaceInstanceData> surfaceInstances;
[[vk::binding(12, 2)]] StructuredBuffer<uint> meshEmitters;
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
        float2 dx, dy;
        RayConeTransformGradients(gradient, transform.row0.xy, transform.row1.xy, dx, dy);
        return materialTextures[texture].SampleGrad(materialSamplers[sampler],
            coord, dx, dy);
    }
    return materialTextures[texture].SampleLevel(materialSamplers[sampler], coord, 0.0);
}
float4 PtTexture(uint index, float4 uv, GpuTextureTransform transform)
{
    // Coverage and medium-boundary classification use mip zero.
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
struct PtHit
{
    RaySurfacePoint point;
    float distance;
    float boundaryMetallic;
    uint instanceIndex;
    uint triangleIndex;
    uint ground;
    uint hit;
    uint fault;
};
float4 PtTriangleUv(RayTriangleData triangle, float2 bary)
{
    return triangle.uv[0] * (1.0 - bary.x - bary.y) + triangle.uv[1] * bary.x + triangle.uv[2] * bary.y;
}
static const uint PT_MATERIAL_UNLIT = 0x10u;
bool PtPotentialVolume(GpuMaterial material, uint flags)
{
    return (flags & PT_MATERIAL_UNLIT) == 0u &&
        material.thicknessFactor > 0.0 && material.transmissionFactor > 0.0;
}
float PtBoundaryMetallic(GpuMaterial material, float4 uv)
{
    // Classify medium boundaries with the same mip-zero inputs for candidate
    // facing, initial containment and committed-hit transport.
    return material.metallicRoughness.x * PtTexture(material.metallicRoughnessTextureIndex, uv,
        material.metallicRoughnessTextureTransform).b * PtScalar(1.0, material.metalnessTextureIndex,
        uv, material.metalnessTextureTransform, 0u);
}
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
float PtCoverage(GpuMaterial material, uint flags, RayTriangleData triangle,
                 float2 bary, inout uint fault)
{
    if ((flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) == 0u)
        return 1.0;
    float4 uv = PtTriangleUv(triangle, bary);
    float alpha = material.color.a * RayTriangleColorAlpha(triangle, bary) * material.opacityFactor *
        PtTexture(material.baseColorTextureIndex, uv, material.baseColorTextureTransform).a *
        PtScalar(1.0, material.opacityTextureIndex, uv, material.opacityTextureTransform, 0u);
    if (!isfinite(alpha)) { fault = 2u; return 0.0; }
    if ((flags & kGpuMaterialFlagAlphaMask) != 0u)
        return alpha < material.alphaCutoff ? 0.0 : 1.0;
    return saturate(alpha);
}
PtHit PtClosest(RayDesc ray, uint coverageSeed, bool volumeOnly = false,
                uint targetInstance = 0xffffffffu, uint targetTriangle = 0xffffffffu,
                bool autofocus = false)
{
    PtHit result = (PtHit)0;
#if !REFERENCE_EMPTY_SCENE
    if (ray.TMin >= ray.TMax) return result;
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
        if (instanceIndex == targetInstance && instance.info.x + primitive == targetTriangle)
            continue;
        GpuMaterial material = materials[instance.info.y];
        uint flags = instance.info.z | material.flags;
        bool volume = PtPotentialVolume(material, flags);
        if (volume)
        {
            RayTriangleData triangle = triangles[instance.info.x + primitive];
            volume = PtBoundaryMetallic(material, PtTriangleUv(triangle, query.CandidateTriangleBarycentrics())) < 1.0;
        }
        if (volumeOnly && !volume) continue;
        if (!query.CandidateTriangleFrontFace() && !volume && (flags & kGpuMaterialFlagDoubleSided) == 0u) continue;
        if (volume && (flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) != 0u)
        { result.fault = 1u; continue; } // Coverage holes do not define a closed medium.
        if ((flags & (kGpuMaterialFlagAlphaMask | kGpuMaterialFlagAlphaBlend)) != 0u)
        {
            RayTriangleData triangle = triangles[instance.info.x + primitive];
            float2 bary = query.CandidateTriangleBarycentrics();
            float alpha = PtCoverage(material, flags, triangle, bary, result.fault);
            if (autofocus ? alpha < 0.5 : PtCoverageRandom(coverageSeed, instanceIndex, primitive) >= alpha)
                continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    if (result.fault != 0u || query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return result;
    uint instanceIndex = query.CommittedInstanceID();
    RayInstanceData instance = instances[instanceIndex];
    uint triangle = instance.info.x + query.CommittedPrimitiveIndex();
    RayTriangleData triangleUv = triangles[triangle];
    float2 bary = query.CommittedTriangleBarycentrics();
    GpuMaterial material = materials[instance.info.y];
    if (PtPotentialVolume(material, instance.info.z | material.flags))
        result.boundaryMetallic = PtBoundaryMetallic(material, PtTriangleUv(triangleUv, bary));
    result.point = ReconstructRaySurface(triangleUv, surfaceTriangles[triangle], instance,
        surfaceInstances[instanceIndex], bary, query.CommittedTriangleFrontFace());
    result.distance = query.CommittedRayT();
    result.instanceIndex = instanceIndex;
    result.triangleIndex = triangle;
    result.hit = 1u;
    if (result.point.valid == 0u) { result.fault = 2u; InterlockedOr(diagnostics[0].w, 8u); }
#endif
    return result;
}
static const float PT_GROUND_SURFACE = 1.0;
static const float PT_GROUND_MATTE = 2.0;

PtHit PtGroundHit(RayDesc ray)
{
    PtHit hit = (PtHit)0;
    if (frame.ground.x == 0.0 || ray.Direction.y >= 0.0) return hit;
    float distance = (frame.ground.y - ray.Origin.y) / ray.Direction.y;
    if (!isfinite(distance) || distance <= ray.TMin || distance >= ray.TMax) return hit;
    float3 position = ray.Origin + distance * ray.Direction;
    position.y = frame.ground.y;
    if (!all(isfinite(position)) ||
        SectionPlaneClips(position, frame.limits.w & 1u, frame.sectionPlane) ||
        BoxClipPlanesClip(position, frame.boxClip)) return hit;
    hit.point.position = position;
    hit.point.geometricNormal = hit.point.shadingNormal = float3(0, 1, 0);
    hit.point.frontFace = 1u;
    hit.point.valid = 1u;
    hit.distance = distance;
    hit.ground = hit.hit = 1u;
    return hit;
}

PtHit PtClosestSurface(RayDesc ray, uint coverageSeed, bool autofocus = false)
{
    PtHit ground = (PtHit)0;
    if (frame.ground.x == PT_GROUND_SURFACE)
        ground = PtGroundHit(ray);
    if (ground.hit != 0u) ray.TMax = ground.distance;
    PtHit mesh = PtClosest(ray, coverageSeed, false, 0xffffffffu, 0xffffffffu, autofocus);
    return mesh.hit != 0u || mesh.fault != 0u ? mesh : ground;
}

struct PtMaterial
{
    PtBsdf bsdf;
    float anisotropy;
    float3 transmission;
    float3 diffuseTransmission;
    float transmissionRoughness;
    float eta;
    // Authored KHR_materials_dispersion value; active only on refractive
    // volume surfaces, where one sampled RGB channel transports each path.
    float dispersion;
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
    uint unlit;
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
float3 PtEmission(GpuMaterial material, RaySurfacePoint point, float2 texGrad)
{
    return material.emissiveColor * material.emissiveStrength * PtTextureGrad(material.emissiveTextureIndex,
        point.uv, material.emissiveTextureTransform, texGrad).rgb;
}
float3 PtUnlitRadiance(GpuMaterial material, RaySurfacePoint point, float2 texGrad)
{
    return material.color.rgb * point.color * PtTextureGrad(material.baseColorTextureIndex,
        point.uv, material.baseColorTextureTransform, texGrad).rgb + PtEmission(material, point, texGrad);
}
PtMaterial PtResolveMaterial(PtHit hit, float2 texGrad, bool visibility = false)
{
    PtMaterial result = (PtMaterial)0;
    if (hit.ground != 0u)
    {
        result.normal = hit.point.shadingNormal;
        result.tangent = float3(1, 0, 0);
        result.bitangent = float3(0, 0, -1);
        result.base = frame.groundColor.rgb;
        result.metallic = frame.ground.z;
        result.eta = 1.5;
        result.bsdf.diffuse = result.base * (1.0 - result.metallic);
        result.bsdf.f0 = lerp(0.04.xxx, result.base, result.metallic);
        result.bsdf.roughness = frame.ground.w;
        result.bsdf.specularScale = 1.0;
        return result;
    }
    RaySurfacePoint input = hit.point;
    GpuMaterial m = materials[input.materialIndex];
    uint flags = input.objectFlags | m.flags;
    if ((flags & PT_MATERIAL_UNLIT) != 0u)
    {
        result.unlit = 1u;
        if (visibility) return result;
        result.emission = PtUnlitRadiance(m, input, texGrad);
        if (!all(isfinite(result.emission)) || any(result.emission < 0.0.xxx))
            result.fault = 2u;
        return result;
    }
    if (!isfinite(m.transmissionFactor) || !isfinite(m.thicknessFactor) || m.thicknessFactor < 0.0 ||
        !isfinite(m.dispersion) || m.dispersion < 0.0 || !isfinite(m.clearcoatFactor) ||
        !all(isfinite(m.sheenColorFactor)) || !isfinite(m.iridescenceFactor) ||
        !all(isfinite(m.anisotropyDiffuseFactors)) || any(m.anisotropyDiffuseFactors.xz < 0.0.xx) ||
        any(m.anisotropyDiffuseFactors.xz > 1.0.xx) || !isfinite(m.heightScale))
    { result.fault = 2u; return result; }
    // Never silently substitute opaque or unlayered transport for unsupported
    // material models. Such paths are diagnosed and excluded from raw moments.
    if ((flags & 0x8u) != 0u ||
        m.clearcoatFactor != 0.0 ||
        any(m.sheenColorFactor.rgb != 0.0.xxx) || m.iridescenceFactor != 0.0 ||
        (m.heightTextureIndex != kInvalidGpuMaterialTexture && m.heightScale != 0.0))
    { result.fault = 1u; return result; }
    bool potentialVolume = PtPotentialVolume(m, flags);
    if (potentialVolume && (!isfinite(hit.boundaryMetallic) || hit.boundaryMetallic < 0.0 || hit.boundaryMetallic > 1.0))
    { result.fault = 2u; return result; }
    result.volume = potentialVolume && hit.boundaryMetallic < 1.0 ? 1u : 0u;
    // Only blockers that can transmit need the connection's footprint.
    if (visibility && (m.transmissionFactor <= 0.0 || (potentialVolume && result.volume == 0u)))
        texGrad = 0.0.xx;
    RaySurfacePoint point = OrientRaySurfaceForShading(input, m.flags |
        (potentialVolume ? kGpuMaterialFlagDoubleSided : 0u));
    float3 base = m.color.rgb * point.color * PtTextureGrad(m.baseColorTextureIndex,
        point.uv, m.baseColorTextureTransform, texGrad).rgb;
    float4 packed = PtTextureGrad(m.metallicRoughnessTextureIndex, point.uv,
        m.metallicRoughnessTextureTransform, texGrad);
    float metallic = m.metallicRoughness.x * packed.b *
        PtScalarGrad(1.0, m.metalnessTextureIndex, point.uv, m.metalnessTextureTransform, 0u, texGrad);
    // A fully metallic point on an authored volume does not cross a medium
    // boundary. Keep it opaque even when a coarser mip contains dielectric texels.
    if (potentialVolume && result.volume == 0u) metallic = hit.boundaryMetallic;
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
    if (result.volume != 0u && !PtValidAbsorption(m.attenuationColor.rgb, m.attenuationDistance))
    { result.fault = 2u; return result; }
    // Dispersion is transported spectrally at the first dispersive refractive
    // interface; thin sheets have no refraction and keep the authored IOR.
    result.dispersion = result.volume != 0u ? m.dispersion : 0.0;
    float dielectric = (m.ior - 1.0) / (m.ior + 1.0);
    dielectric *= dielectric;
    float scale = metallic + (1.0 - metallic) * specular;
    result.bsdf.diffuse = base * (1.0 - metallic) * (1.0 - transmission);
    result.transmission = base * (1.0 - metallic) * transmission;
    if (m.anisotropyDiffuseFactors.z > 0.0 && metallic < 1.0 && transmission < 1.0)
    {
        float diffuseFactor = PtScalarGrad(m.anisotropyDiffuseFactors.z, m.diffuseTransmissionTextureIndex,
            point.uv, m.diffuseTransmissionTextureTransform, 3u, texGrad);
        if (!isfinite(diffuseFactor) || diffuseFactor < 0.0 || diffuseFactor > 1.0)
        { result.fault = 2u; return result; }
        if (diffuseFactor > 0.0)
        {
            // A volume interface needs a separate diffuse boundary proposal.
            if (m.thicknessFactor > 0.0) { result.fault = 1u; return result; }
            float3 tint = m.diffuseTransmissionColorFactor.rgb * PtTextureGrad(
                m.diffuseTransmissionColorTextureIndex, point.uv,
                m.diffuseTransmissionColorTextureTransform, texGrad).rgb;
            if (!PtValidDiffuseTransmission(tint)) { result.fault = 2u; return result; }
            result.bsdf.diffuse *= 1.0 - diffuseFactor;
            result.diffuseTransmission = tint * ((1.0 - metallic) * (1.0 - transmission) * diffuseFactor);
        }
    }
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
    if (m.anisotropyDiffuseFactors.x > 0.0 && scale > 0.0)
    {
        float3 anisotropyTexel = float3(1.0, 0.5, 1.0);
        if (m.anisotropyTextureIndex != kInvalidGpuMaterialTexture)
            anisotropyTexel = PtTextureGrad(m.anisotropyTextureIndex, point.uv,
                m.anisotropyTextureTransform, texGrad).rgb;
        if (!all(isfinite(anisotropyTexel)) || any(anisotropyTexel < 0.0.xxx) ||
            any(anisotropyTexel > 1.0.xxx))
        { result.fault = 2u; return result; }
        result.anisotropy = m.anisotropyDiffuseFactors.x * anisotropyTexel.b;
        if (result.anisotropy > 0.0)
        {
            // The zero-perpendicular-width limit is singular. Volume refraction
            // also requires its own anisotropic interface proposal and Jacobian.
            if (roughness == 0.0 || result.volume != 0u)
            { result.fault = 1u; return result; }
            result.tangent = ResolveRayAnisotropyTangent(point, result.normal,
                anisotropyTexel.rg, m.anisotropyDiffuseFactors.y);
        }
    }
    result.bitangent = cross(result.normal, result.tangent);
    result.emission = PtEmission(m, point, texGrad);
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
float PtNextNullRayMinimum(PtHit hit)
{
    return hit.distance + max(PtBias(hit.point.position), abs(hit.distance) * 2e-7);
}
static const uint PT_MAX_MEDIA = 8u;
static const uint PT_MAX_MEDIUM_PROBE_HITS = 128u;
struct PtMediumState { uint material[PT_MAX_MEDIA]; uint count; };
bool PtCompatibleMedium(uint a, uint b)
{
    GpuMaterial x = materials[a], y = materials[b];
    return x.ior == y.ior && x.dispersion == y.dispersion &&
        x.attenuationDistance == y.attenuationDistance && all(x.attenuationColor.rgb == y.attenuationColor.rgb);
}
float PtMediumIor(PtMediumState state, uint channel)
{
    if (state.count == 0u) return 1.0;
    GpuMaterial medium = materials[state.material[state.count - 1u]];
    return GpuMaterialChannelIor(medium.ior, medium.dispersion, channel);
}
float3 PtMediumAttenuation(PtMediumState state, float distance)
{
    if (state.count == 0u) return 1.0.xxx;
    GpuMaterial medium = materials[state.material[state.count - 1u]];
    return PtAbsorption(medium.attenuationColor.rgb, medium.attenuationDistance, distance);
}
bool PtCrossMedium(inout PtMediumState state, uint material, bool frontFace,
    uint channel, out float eta, inout uint fault)
{
    float incidentIor = PtMediumIor(state, channel);
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
    eta = PtMediumIor(state, channel) / incidentIor;
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
    return PtEvaluateDiffuseTransmissionBsdf(PtTransport(material), material.diffuseTransmission,
        view, light, material.anisotropy);
}
PtBsdfSample PtSampleSurface(PtMaterial material, float3 view, float3 u)
{
    if (material.volume != 0u) return PtSampleVolumeMixture(PtVolumeTransport(material), view, u);
    return PtSampleDiffuseTransmissionBsdf(PtTransport(material), material.diffuseTransmission,
        view, u, material.anisotropy);
}
static const uint PT_MAX_NULL_SHEETS = 64u;
bool PtVisible(float3 position, float3 normal, float3 direction, float distance, uint areaCount,
    uint ignoredEmitter, uint coverageSeed, PtMediumState medium, bool deltaLight, uint spectral,
    RayCone cone, uint targetInstance, uint targetTriangle,
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
    if (frame.ground.x == PT_GROUND_SURFACE && PtGroundHit(ray).hit != 0u) return false;
    float3 segmentStart = position;
    // A spectral path evaluates every medium boundary at its active channel's
    // IOR, so dispersive blockers bend and reject straight NEE connections.
    uint channel = spectral == 0xffffffffu ? 1u : spectral;
    for (uint sheet = 0u; sheet <= PT_MAX_NULL_SHEETS; ++sheet)
    {
        PtHit hit = PtClosest(ray, coverageSeed, false, targetInstance, targetTriangle);
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
        // Straight-through light and BSDF proposals start at the same
        // scattering point and use the same cone and texture footprint.
        float worldFoot = RayConeWorldFootprint(cone, hit.distance, hit.point.geometricNormal, -direction);
        PtMaterial blocker = PtResolveMaterial(hit, worldFoot * hit.point.texelDensity, true);
        if (blocker.fault != 0u) { fault |= blocker.fault; return false; }
        if (blocker.volume != 0u)
        {
            PtMediumState crossed = medium;
            float eta;
            if (!PtCrossMedium(crossed, hit.point.materialIndex, hit.point.frontFace != 0u, channel, eta, fault)) return false;
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
            float diffuseProbability = PtDiffuseTransmissionProbability(bsdf, blocker.diffuseTransmission, view);
            probability = (1.0 - diffuseProbability) * PtThinProbability(bsdf, view);
        }
        if (all(coefficient == 0.0.xxx)) return false;
        transmittance *= coefficient;
        nullPdf *= probability;
        segmentStart = hit.point.position;
        ray.TMin = PtNextNullRayMinimum(hit);
        if (ray.TMin >= ray.TMax)
        {
            transmittance *= PtMediumAttenuation(medium, max(0.0, distance - dot(segmentStart - position, direction)));
            return true;
        }
    }
    return false;
}
#if !REFERENCE_EMPTY_SCENE
float PtMeshPdf(RaySurfacePoint point, uint flags, uint triangleCount, float3 origin,
                RaySurfaceTriangleData triangle, RaySurfaceInstanceData surface)
{
    float3 offset = origin - point.position;
    float distanceSquared = dot(offset, offset);
    if (!(distanceSquared > 0.0)) return 0.0;
    float cosine = dot(point.geometricNormal, offset * rsqrt(distanceSquared));
    if ((flags & kGpuMaterialFlagDoubleSided) != 0u) cosine = abs(cosine);
    if (cosine <= 0.0) return 0.0;
    float3 first = mul((float3x3)surface.model, triangle.positions[1].xyz - triangle.positions[0].xyz);
    float3 second = mul((float3x3)surface.model, triangle.positions[2].xyz - triangle.positions[0].xyz);
    float area = 0.5 * PtLength(cross(first, second));
    return area > 0.0
        ? distanceSquared / (cosine * area * float(triangleCount) * float(frame.sampling.w))
        : 0.0;
}
float PtMeshHitPdf(PtHit hit, float3 previousPosition)
{
    uint first = 0u, last = frame.sampling.w;
    while (first < last)
    {
        uint middle = first + (last - first) / 2u;
        if (meshEmitters[middle] < hit.instanceIndex) first = middle + 1u;
        else last = middle;
    }
    if (first >= frame.sampling.w) return 0.0;
    if (meshEmitters[first] != hit.instanceIndex) return 0.0;
    RayInstanceData metadata = instances[hit.instanceIndex];
    return PtMeshPdf(hit.point, metadata.info.z | materials[metadata.info.y].flags,
        metadata.info.w, previousPosition, surfaceTriangles[hit.triangleIndex], surfaceInstances[hit.instanceIndex]);
}
struct PtMeshSample
{
    float3 direction;
    float3 radiance;
    float distance;
    float pdf;
    uint instanceIndex;
    uint triangleIndex;
};
PtMeshSample PtSampleMesh(uint index, float triangleSample, float2 u, float3 origin,
                          RayCone cone, inout uint fault)
{
    PtMeshSample result = (PtMeshSample)0;
    uint instanceIndex = meshEmitters[index];
    RayInstanceData metadata = instances[instanceIndex];
    uint primitive = min(uint(triangleSample * float(metadata.info.w)), metadata.info.w - 1u);
    uint triangleIndex = metadata.info.x + primitive;
    RayTriangleData uv = triangles[triangleIndex];
    RaySurfaceTriangleData triangle = surfaceTriangles[triangleIndex];
    RaySurfaceInstanceData surface = surfaceInstances[instanceIndex];
    float root = sqrt(u.x);
    float2 bary = root * float2(1.0 - u.y, u.y);
    RaySurfacePoint point = ReconstructRaySurface(uv, triangle, metadata, surface, bary, true);
    if (point.valid == 0u ||
        SectionPlaneClips(point.position, frame.limits.w & 1u, frame.sectionPlane) ||
        BoxClipPlanesClip(point.position, frame.boxClip)) return result;
    GpuMaterial material = materials[metadata.info.y];
    uint flags = metadata.info.z | material.flags;
    result.pdf = PtMeshPdf(point, flags, metadata.info.w, origin, triangle, surface);
    if (result.pdf <= 0.0) return result;
    float coverage = PtCoverage(material, flags, uv, bary, fault);
    if (coverage <= 0.0) { result.pdf = 0.0; return result; }
    float3 offset = point.position - origin;
    result.distance = PtLength(offset);
    result.direction = offset / result.distance;
    result.instanceIndex = instanceIndex;
    result.triangleIndex = triangleIndex;
    float worldFoot = RayConeWorldFootprint(cone, result.distance, point.geometricNormal, -result.direction);
    float2 texGrad = worldFoot * point.texelDensity;
    result.radiance = coverage * ((flags & PT_MATERIAL_UNLIT) != 0u
        ? PtUnlitRadiance(material, point, texGrad) : PtEmission(material, point, texGrad));
    return result;
}
#endif
float3 PtDirect(PtHit hit, PtMaterial material, float3 view, float3 geometricNormal,
    uint pixel, uint sampleIndex, uint bounce, uint4 counts, bool useMis, PtMediumState medium,
    uint spectral, RayCone cone, bool meshProposal, inout uint fault)
{
    uint count = meshProposal ? frame.sampling.w : PtLightCount(counts);
    if (count == 0u) return 0.0.xxx;
    uint dimension = meshProposal ? 10u : 2u;
    uint index = min(uint(PtRandom(pixel, sampleIndex, bounce, dimension, frame.sampling.z) * float(count)), count - 1u);
    float2 u = float2(PtRandom(pixel, sampleIndex, bounce, dimension + 1u, frame.sampling.z),
                     PtRandom(pixel, sampleIndex, bounce, dimension + 2u, frame.sampling.z));
    float3 direction = 0.0.xxx, radiance = 0.0.xxx;
    float distance = frame.trace.y, pdf = 1.0 / float(count);
    bool delta = true;
    uint ignoredEmitter = 0xffffffffu;
    uint targetInstance = 0xffffffffu, targetTriangle = 0xffffffffu;
    if (meshProposal)
    {
#if !REFERENCE_EMPTY_SCENE
        float triangleSample = PtRandom(pixel, sampleIndex, bounce, dimension + 3u, frame.sampling.z);
        PtMeshSample light = PtSampleMesh(index, triangleSample, u, hit.point.position, cone, fault);
        if (fault != 0u || light.pdf <= 0.0) return 0.0.xxx;
        direction = light.direction;
        radiance = light.radiance;
        distance = light.distance;
        pdf = light.pdf;
        targetInstance = light.instanceIndex;
        targetTriangle = light.triangleIndex;
        delta = false;
#else
        return 0.0.xxx;
#endif
    }
    else if (index < counts.x)
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
    if (frame.ground.x == PT_GROUND_MATTE)
    {
        if (!isfinite(pdf) || !all(isfinite(radiance)) || any(radiance < 0.0.xxx))
        { fault = 2u; return 0.0.xxx; }
        float cosine = dot(geometricNormal, direction);
        if (pdf <= 0.0 || cosine <= 0.0 || all(radiance == 0.0.xxx)) return 0.0.xxx;
        float3 incident = radiance * (cosine / pdf);
        float3 transmittance;
        float nullPdf;
        bool visible = PtVisible(hit.point.position, geometricNormal, direction, distance, counts.w,
            ignoredEmitter, PtCoverageSeed(pixel, sampleIndex, bounce, 1u), medium, delta, spectral,
            cone, targetInstance, targetTriangle, transmittance, nullPdf, fault);
        float3 blocked = visible ? incident * (1.0.xxx - saturate(transmittance)) : incident;
        float3 luma = float3(0.2126, 0.7152, 0.0722);
        return float3(dot(blocked, luma), dot(incident, luma), 0.0);
    }
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
        if (!PtCrossMedium(medium, hit.point.materialIndex, hit.point.frontFace != 0u,
                spectral == 0xffffffffu ? 1u : spectral, eta, fault)) return 0.0.xxx;
    }
    if (!PtVisible(hit.point.position, offsetNormal, direction, distance, counts.w, ignoredEmitter,
        PtCoverageSeed(pixel, sampleIndex, bounce, 1u), medium, delta, spectral, cone, targetInstance, targetTriangle,
        transmittance, nullPdf, fault)) return 0.0.xxx;
    float weight = !delta && useMis ? PtPowerHeuristic(pdf, evaluated.pdf * nullPdf) : 1.0;
    return radiance * transmittance * evaluated.value * (abs(dot(material.normal, direction)) * weight / pdf);
}
float3 PtTraceShadowMatte(RayDesc ray, uint pixel, uint sampleIndex, uint4 counts,
                          RayCone cone, inout uint fault)
{
    PtHit ground = PtGroundHit(ray);
    if (ground.hit == 0u) return 0.0.xxx;
    RayDesc foreground = ray;
    foreground.TMax = ground.distance;
    PtHit mesh = PtClosest(foreground, PtCoverageSeed(pixel, sampleIndex, 0u, 0u));
    fault = mesh.fault;
    if (mesh.hit != 0u || fault != 0u || PtAreaHit(foreground, counts.w, 0xffffffffu).hit != 0u)
        return 0.0.xxx;
    PtMediumState medium = PtInitialMedium(ray, PtCoverageSeed(pixel, sampleIndex, 0u, 2u), fault);
    if (fault != 0u) return 0.0.xxx;

    cone.width = RayConeWorldFootprint(cone, ground.distance, ground.point.geometricNormal, -ray.Direction);
    float3 value = float3(0, 0, 1);
    uint proposals = frame.sampling.w != 0u ? 2u : 1u;
    for (uint proposal = 0u; proposal < proposals; ++proposal)
    {
        value += PtDirect(ground, (PtMaterial)0, -ray.Direction, ground.point.geometricNormal,
            pixel, sampleIndex, 0u, counts, false, medium, 0xffffffffu, cone, proposal != 0u, fault);
        if (fault != 0u) return 0.0.xxx;
    }
    if (!all(isfinite(value))) { fault = 2u; return 0.0.xxx; }
    return value;
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
    bool orthographic = (frame.limits.w & 2u) != 0u;
    PtCameraRay primary = PtGenerateCameraRay(samplePosition, frame.extentSamples.xy,
        frame.inverseProjection, frame.inverseView, orthographic);
    if (primary.valid == 0u) { fault = 2u; InterlockedOr(diagnostics[0].w, 16u); return 0.0.xxx; }
    float3 lensOffset = 0.0.xxx;
    [branch] if (frame.trace.z > 0.0)
    {
        // Independent of pixel jitter, light/mesh proposals and BSDF dimensions.
        const float2 lensRandom = float2(PtRandom(address, sampleIndex, 0u, 14u, frame.sampling.z),
                                        PtRandom(address, sampleIndex, 0u, 15u, frame.sampling.z));
        if (!PtApplyThinLens(primary, frame.inverseView, lensRandom, frame.trace.z, frame.trace.w, lensOffset))
        { fault = 2u; InterlockedOr(diagnostics[0].w, 16u); return 0.0.xxx; }
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
        if (frame.ground.x == PT_GROUND_MATTE) return 0.0.xxx;
        float3 value = PtEnvironment(primary.direction);
        if (!all(isfinite(value)) || any(value < 0.0.xxx)) { fault = 2u; return 0.0.xxx; }
        return value;
    }
    float2 footprint = PtCameraRayFootprint(samplePosition, frame.extentSamples.xy,
        frame.inverseProjection, frame.inverseView, orthographic, primary.direction, lensOffset, frame.trace.w);
    RayCone cone = {footprint.x, footprint.y};
    if (frame.ground.x == PT_GROUND_MATTE)
        return PtTraceShadowMatte(ray, address, sampleIndex, counts, cone, fault);
    float3 result = 0.0.xxx, throughput = 1.0.xxx, previousPosition = primary.origin;
    float previousPdf = 0.0;
    float etaScale = 1.0;
    bool previousDelta = true;
    PtMediumState medium = PtInitialMedium(ray, PtCoverageSeed(address, sampleIndex, 0u, 2u), fault);
    if (fault != 0u) return 0.0.xxx;
    float3 segmentStart = ray.Origin;
    uint depth = 0u, nullSheets = 0u;
    // 0xffffffff marks an RGB path; the first dispersive refractive interface
    // selects one channel and carries a 3x-weighted monochromatic estimate.
    uint spectral = 0xffffffffu;
    for (uint bounce = 0u; bounce < frame.sampling.x + PT_MAX_NULL_SHEETS; ++bounce)
    {
        PtHit hit = PtClosestSurface(ray, PtCoverageSeed(address, sampleIndex, bounce, 0u));
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
        // The surface shades with the slant-corrected diameter. Only a
        // scattering event starts a new cone with that surface width.
        float worldFoot = RayConeWorldFootprint(cone, hit.distance,
            hit.point.geometricNormal, -ray.Direction);
        RayCone scatteringCone = {worldFoot, cone.spread};
        PtMaterial material = PtResolveMaterial(hit, worldFoot * hit.point.texelDensity);
        if (material.fault != 0u) { fault = material.fault; return 0.0.xxx; }
        throughput *= PtMediumAttenuation(medium, PtLength(hit.point.position - segmentStart));
        PtMediumState crossed = medium;
        if (material.volume != 0u)
        {
            float eta;
            if (!PtCrossMedium(crossed, hit.point.materialIndex, hit.point.frontFace != 0u,
                    spectral == 0xffffffffu ? 1u : spectral, eta, fault)) return 0.0.xxx;
            PtSetVolumeEta(material, eta);
        }
        float meshWeight = 1.0;
#if !REFERENCE_EMPTY_SCENE
        if (!previousDelta && frame.sampling.w != 0u && any(material.emission > 0.0.xxx))
        {
            float meshPdf = PtMeshHitPdf(hit, previousPosition);
            if (meshPdf > 0.0) meshWeight = PtPowerHeuristic(previousPdf, meshPdf);
        }
#endif
        result += throughput * material.emission * meshWeight;
        if (material.unlit != 0u) break;
        // KHR_materials_dispersion (#47): after the first dispersive refractive
        // interface, one uniformly sampled RGB channel transports the path with
        // a 3x weight, keeping per-channel estimates unbiased while refraction,
        // Fresnel, absorption and every later boundary use that channel's IOR.
        // The hit's own emission above stays full RGB: it is emitted at the
        // interface and never crosses it.
        if (material.volume != 0u && material.dispersion > 0.0 &&
            any(material.transmission > 0.0.xxx))
        {
            if (spectral == 0xffffffffu)
            {
                spectral = min(uint(PtRandom(address, sampleIndex, bounce, 9u,
                    frame.sampling.z) * 3.0), 2u);
                throughput *= 3.0 * (spectral == 0u ? float3(1, 0, 0) :
                    spectral == 1u ? float3(0, 1, 0) : float3(0, 0, 1));
                if (!all(isfinite(throughput)) || any(throughput < 0.0.xxx))
                { fault = 2u; return 0.0.xxx; }
            }
            PtSetVolumeEta(material, PtMediumIor(crossed, spectral) /
                PtMediumIor(medium, spectral));
        }
        else if (spectral != 0xffffffffu && material.volume != 0u)
            PtSetVolumeEta(material, PtMediumIor(crossed, spectral) /
                PtMediumIor(medium, spectral));
        float3 geometricNormal = hit.point.geometricNormal * (hit.point.frontFace != 0u ? 1.0 : -1.0);
        float3 view = -ray.Direction;
        if (dot(material.normal, view) <= 0.0) break;
        bool continuePath = depth + 1u < frame.sampling.x;
        uint proposals = frame.sampling.w != 0u ? 2u : 1u;
        for (uint proposal = 0u; proposal < proposals; ++proposal)
        {
            result += throughput * PtDirect(hit, material, view, geometricNormal, address, sampleIndex,
                bounce, counts, continuePath, medium, spectral, scatteringCone, proposal != 0u, fault);
            if (fault != 0u) return 0.0.xxx;
        }
        bool canPassNull = material.volume != 0u ? material.eta == 1.0 : material.transmissionRoughness == 0.0;
        if (!continuePath && (!canPassNull || all(material.transmission == 0.0.xxx))) break;
        float3 u = float3(PtRandom(address, sampleIndex, bounce, 5u, frame.sampling.z),
                         PtRandom(address, sampleIndex, bounce, 6u, frame.sampling.z),
                         PtRandom(address, sampleIndex, bounce, 7u, frame.sampling.z));
        PtBsdfSample sampled = PtSampleSurface(material, PtLocal(material, view), u);
        if (sampled.rejected != 0u) { fault = 2u; return 0.0.xxx; }
        if (sampled.valid == 0u) break;
        bool transmission = sampled.direction.z < 0.0;
        bool nullSheet = sampled.delta != 0u && transmission && (material.volume == 0u || material.eta == 1.0);
        float3 direction = nullSheet ? ray.Direction : PtWorld(material, sampled.direction);
        if (sampled.direction.z * dot(direction, geometricNormal) <= 0.0) break;
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
        segmentStart = hit.point.position;
        if (nullSheet)
        {
            ++nullSheets;
            // MIS compares the complete path probability, including each
            // stochastic null choice. The competing light proposal connects
            // from the last real scattering point and integrates sheet tint.
            previousPdf *= sampled.pdf;
            // Preserve the original direction, cone and distance reference
            // through this sheet, just as on the competing light connection.
            ray.TMin = PtNextNullRayMinimum(hit);
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
            ray.Origin = hit.point.position + geometricNormal * (transmission ? -1.0 : 1.0) * PtBias(hit.point.position);
            ray.Direction = direction;
            ray.TMin = 0.0;
            ray.TMax = frame.trace.y;
            cone = scatteringCone;
        }
    }
    if (!all(isfinite(result)) || any(result < 0.0.xxx)) { fault = 2u; return 0.0.xxx; }
    return result;
}

float PtAutofocusDistance()
{
    if ((frame.limits.w & (2u | 8u)) != 0u) return 0.0;
    PtCameraRay center = PtGenerateCameraRay(0.5 * float2(frame.extentSamples.xy),
        frame.extentSamples.xy, frame.inverseProjection, frame.inverseView, false);
    if (center.valid == 0u) return 0.0;
    RayDesc ray;
    ray.Origin = center.origin;
    ray.Direction = center.direction;
    ray.TMin = center.minimum;
    ray.TMax = min(center.maximum, frame.trace.y);
    PtHit target = PtClosestSurface(ray, 0u, true);
    if (target.hit == 0u || target.fault != 0u) return 0.0;
    float3 forward = PtNormalize(mul(frame.inverseView, float4(0, 0, -1, 0)).xyz);
    return target.distance * dot(center.direction, forward);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= frame.extentSamples.xy)) return;
    if (frame.controls.y != 0.0 && all(tid.xy == uint2(0u, 0u)))
        diagnostics[1].x = asuint(PtAutofocusDistance());
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
        if (fault == 0u && frame.controls.x > 0.0)
        {
            float peak = max(radiance.x, max(radiance.y, radiance.z));
            if (peak > frame.controls.x) radiance = (radiance / peak) * frame.controls.x;
        }
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
    float3 shown = mean;
    if (frame.ground.x == PT_GROUND_MATTE)
        shown = (mean.y > 0.0 ? saturate(mean.x / mean.y) * saturate(mean.z) : 0.0).xxx;
    display[tid.xy] = flags != 0u ? float4(1.0, (flags & 2u) != 0u ? 1.0 : 0.0, (flags & 1u) != 0u ? 1.0 : 0.0, 1.0) : float4(shown, 1.0);
}
