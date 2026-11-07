#pragma once
#include "area_light_common.slang"
#include "push_constants_common.slang"
#include "ray_footprint_common.slang"
#include "ray_surface_common.slang"

[[vk::binding(10, 0)]] TextureCube<float4> environmentIrradiance;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> reflectionPointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> reflectionAreaLights;
[[vk::binding(2, 2)]] SamplerState reflectionMaterialSamplers[];
[[vk::binding(4, 2)]] StructuredBuffer<GpuTextureMetadata> reflectionTextureMetadata;
[[vk::binding(5, 2)]] Texture2D reflectionMaterialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(6, 2)]] ConstantBuffer<BoxClipState> reflectionBoxClip;
[[vk::binding(3, 3)]] RaytracingAccelerationStructure reflectionScene;
[[vk::binding(4, 3)]] StructuredBuffer<RayTriangleData> reflectionTriangles;
[[vk::binding(5, 3)]] StructuredBuffer<RayInstanceData> reflectionInstances;
[[vk::binding(6, 3)]] StructuredBuffer<RaySurfaceTriangleData> reflectionSurfaceTriangles;
[[vk::binding(7, 3)]] StructuredBuffer<RaySurfaceInstanceData> reflectionSurfaceInstances;

float4 reflectionTexture(uint index, float4 uv, GpuTextureTransform transform, float2 gradients = 0.0.xx)
{
    if (index >= MATERIAL_TEXTURE_DESCRIPTOR_COUNT) return 1.0.xxxx;
    uint texture = NonUniformResourceIndex(index);
    uint sampler = NonUniformResourceIndex(ResolveMaterialSamplerIndex(reflectionTextureMetadata[texture]));
    float2 coord = ApplyGpuTextureTransform(uv.xy, uv.zw, transform);
    float gradient = transform.row0.w >= 0.5 ? gradients.y : gradients.x;
    float2 dx, dy;
    if (RayConeTransformGradients(gradient, transform.row0.xy, transform.row1.xy, dx, dy))
        return reflectionMaterialTextures[texture].SampleGrad(reflectionMaterialSamplers[sampler], coord, dx, dy);
    return reflectionMaterialTextures[texture].SampleLevel(reflectionMaterialSamplers[sampler], coord, 0.0);
}

bool reflectionCandidateAccepted(uint instanceIndex, uint primitive, float2 bary, float3 world, bool frontFace)
{
    if (instanceIndex >= settings.rayLimits.x || !all(isfinite(world)) ||
        SectionPlaneClips(world, settings.rayLimits.z, settings.sectionPlane) ||
        BoxClipPlanesClip(world, reflectionBoxClip)) return false;
    RayInstanceData instance = reflectionInstances[instanceIndex];
    if (instance.info.x >= settings.rayLimits.y || primitive >= settings.rayLimits.y - instance.info.x)
        return false;
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);
    if (instance.info.y >= materialCount) return false;
    GpuMaterial material = materials[instance.info.y];
    uint flags = instance.info.z | material.flags;
    // Match the opaque screen source. Transparent and refractive draws are
    // composited separately and are not reflection hits or shadow blockers.
    if ((flags & kGpuMaterialFlagAlphaBlend) != 0u || material.transmissionFactor > 0.0 ||
        (!frontFace && (flags & kGpuMaterialFlagDoubleSided) == 0u)) return false;
    if ((flags & kGpuMaterialFlagAlphaMask) == 0u) return true;
    RayTriangleData triangle = reflectionTriangles[instance.info.x + primitive];
    float4 uv = triangle.uv[0] + (triangle.uv[1] - triangle.uv[0]) * bary.x +
        (triangle.uv[2] - triangle.uv[0]) * bary.y;
    // Evaluate coverage at base mip, like the shared binary visibility path.
    float alpha = material.color.a * RayTriangleColorAlpha(triangle, bary) * saturate(material.opacityFactor) *
        reflectionTexture(material.baseColorTextureIndex, uv, material.baseColorTextureTransform).a *
        SelectGpuTextureChannel(reflectionTexture(material.opacityTextureIndex, uv,
            material.opacityTextureTransform), material.opacityTextureTransform, 0u);
    return isfinite(alpha) && alpha >= material.alphaCutoff;
}

RayDesc reflectionRay(float3 world, float3 normal, float3 direction, float distance)
{
    float scale = max(max(abs(world.x), abs(world.y)), abs(world.z));
    float bias = max(0.00025, scale * 2e-7);
    RayDesc ray;
    ray.Origin = world + normal * bias;
    ray.Direction = direction;
    ray.TMin = bias;
    ray.TMax = max(distance - bias * 2.0, bias);
    return ray;
}

float reflectionDirectVisibility(float3 position, float3 geometricNormal, float3 direction, float distance)
{
    if (dot(geometricNormal, direction) <= 0.0) return 0.0;
    RayDesc ray = reflectionRay(position, geometricNormal, direction, distance);
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(reflectionScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            reflectionCandidateAccepted(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), ray.Origin + ray.Direction * query.CandidateTriangleRayT(),
                query.CandidateTriangleFrontFace()))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float3 reflectionHitRadiance(RaySurfacePoint hit, float3 view, float2 gradients)
{
    GpuMaterial m = materials[hit.materialIndex];
    uint flags = m.flags | hit.objectFlags;
    float3 color = max(m.color.rgb * hit.color * reflectionTexture(m.baseColorTextureIndex, hit.uv,
        m.baseColorTextureTransform, gradients).rgb, 0.0.xxx);
    float3 emission = max(m.emissiveColor * m.emissiveStrength * reflectionTexture(m.emissiveTextureIndex,
        hit.uv, m.emissiveTextureTransform, gradients).rgb, 0.0.xxx);
    if ((flags & 0x10u) != 0u) return color + emission; // glTF unlit

    float3 normal = hit.shadingNormal;
    if (m.normalTextureIndex != kInvalidGpuMaterialTexture)
    {
        float3 tangentNormal = reflectionTexture(m.normalTextureIndex, hit.uv,
            m.normalTextureTransform, gradients).xyz * 2.0 - 1.0;
        tangentNormal.xy *= m.normalTextureScale;
        normal = SafeNormalize(hit.tangent.xyz * tangentNormal.x + hit.bitangent * tangentNormal.y +
            hit.shadingNormal * tangentNormal.z, hit.shadingNormal);
    }
    float face = hit.frontFace != 0u ? 1.0 : -1.0;
    float3 geometricNormal = hit.geometricNormal * face;
    normal *= face;
    if (dot(normal, geometricNormal) <= 0.0 || dot(normal, view) <= 0.0) normal = geometricNormal;
    float4 mr = reflectionTexture(m.metallicRoughnessTextureIndex, hit.uv, m.metallicRoughnessTextureTransform, gradients);
    float metallic = saturate(m.metallicRoughness.x * mr.b * SelectGpuTextureChannel(
        reflectionTexture(m.metalnessTextureIndex, hit.uv, m.metalnessTextureTransform, gradients),
        m.metalnessTextureTransform, 0u));
    float roughness = clamp(m.metallicRoughness.y * mr.g * SelectGpuTextureChannel(
        reflectionTexture(m.roughnessTextureIndex, hit.uv, m.roughnessTextureTransform, gradients),
        m.roughnessTextureTransform, 0u), 0.045, 1.0);
    float specular = saturate(m.specularFactor * SelectGpuTextureChannel(
        reflectionTexture(m.specularTextureIndex, hit.uv, m.specularTextureTransform, gradients),
        m.specularTextureTransform, 3u));
    float3 specularColor = saturate(m.specularColorFactor.rgb * reflectionTexture(
        m.specularColorTextureIndex, hit.uv, m.specularColorTextureTransform, gradients).rgb);
    float3 f0 = EvaluatePbrF0WithDielectricTint(color, metallic, specular, specularColor, m.ior, 1.0.xxx);

    // One evaluated hit only. Direct shadows use the same scene and clips,
    // and local-light counts stay bounded independently of scene complexity.
    float3 radiance = emission;
    float3 direction = SafeNormalize(-lights.directionalDirection.xyz, float3(0, 1, 0));
    float3 incident = max(lights.directionalColorIntensity.rgb * lights.directionalColorIntensity.a, 0.0.xxx);
    if (any(incident > 0.0.xxx) && dot(normal, direction) > 0.0)
        radiance += EvaluatePbrDirectLight(color, f0, metallic, roughness, normal, view, direction, incident) *
            reflectionDirectVisibility(hit.position, geometricNormal, direction, kMaxDistance);
    uint lightBudget = settings.maxSteps <= 24u ? 4u : 8u;
    for (uint i = 0u; i < min(lights.pointLightCount, lightBudget); ++i)
    {
        PointLightData light = reflectionPointLights[i];
        float3 delta = light.positionRadius.xyz - hit.position;
        float distanceSq = dot(delta, delta);
        if (!PointLightHasRenderableIntensity(light) || distanceSq <= 1e-10 || !isfinite(distanceSq)) continue;
        float distance = sqrt(distanceSq);
        direction = delta / distance;
        float attenuation = PointLightAttenuation(distanceSq, light.positionRadius.w) * LocalLightAngularIntensity(light, direction);
        if (attenuation <= 0.0 || dot(normal, direction) <= 0.0) continue;
        incident = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        radiance += EvaluatePbrDirectLight(color, f0, metallic, roughness, normal, view, direction, incident) *
            reflectionDirectVisibility(hit.position, geometricNormal, direction, distance);
    }
    for (uint i = 0u; i < min(lights.areaLightCount, lightBudget / 2u); ++i)
    {
        AreaLightData light = reflectionAreaLights[i];
        if (!AreaLightHasRenderableRadiance(light)) continue;
        AreaLightFrame area = BuildAreaLightFrame(light);
        float attenuation = AreaLightPositionContributionWeight(light, area, hit.position, area.position, 1.0, direction);
        if (attenuation <= 0.0 || dot(normal, direction) <= 0.0) continue;
        incident = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        radiance += EvaluatePbrDirectLight(color, f0, metallic, roughness, normal, view, direction, incident) *
            reflectionDirectVisibility(hit.position, geometricNormal, direction, length(area.position - hit.position));
    }
    float nDotV = saturate(dot(normal, view));
    float3 kD = (1.0 - FresnelSchlickRoughness(nDotV, f0, roughness)) * (1.0 - metallic);
    float3 diffuse = environmentIrradiance.SampleLevel(environmentSampler, RotateEnvironmentDirection(
        normal, lights.environmentOrientation.xy), 0.0).rgb * kD * color;
    float3 specularIbl = environmentSpecular.SampleLevel(environmentSampler, RotateEnvironmentDirection(
        reflect(-view, normal), lights.environmentOrientation.xy),
        roughness * max(float(lights.prefilteredMipCount) - 1.0, 0.0)).rgb;
    float2 brdf = max(brdfLut.SampleLevel(brdfSampler, float2(nDotV, roughness), 0.0), 0.0.xx);
    return radiance + max(lights.environmentIntensity, 0.0) *
        (diffuse + EvaluateSpecularIblMultipleScattering(specularIbl, f0, brdf));
}

struct ReflectionRayHit
{
    float3 radiance;
    float distance;
    float confidence;
    uint materialIndex;
};

ReflectionRayHit traceFallbackReflection(float3 position, float3 normal, float3 direction, float roughness)
{
    ReflectionRayHit result = {};
    RayDesc ray = reflectionRay(position, normal, direction, kMaxDistance);
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(reflectionScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            reflectionCandidateAccepted(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), ray.Origin + ray.Direction * query.CandidateTriangleRayT(),
                query.CandidateTriangleFrontFace()))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return result;
    uint instanceIndex = query.CommittedInstanceID();
    RayInstanceData instance = reflectionInstances[instanceIndex];
    uint triangle = instance.info.x + query.CommittedPrimitiveIndex();
    RaySurfacePoint point = ReconstructRaySurface(reflectionTriangles[triangle], reflectionSurfaceTriangles[triangle],
        instance, reflectionSurfaceInstances[instanceIndex], query.CommittedTriangleBarycentrics(),
        query.CommittedTriangleFrontFace());
    if (point.valid == 0u) return result;
    result.materialIndex = point.materialIndex;
    result.distance = query.CommittedRayT();
    RayCone cone = {0.0, 2.0 / float(min(settings.extent.x, settings.extent.y)) + roughness * roughness};
    float2 gradients = RayConeGradient(cone, result.distance, point.geometricNormal, -direction, point.texelDensity);
    if (settings.debugView == 0u || settings.debugView == 4u)
        result.radiance = reflectionHitRadiance(point, -direction, gradients);
    if (!all(isfinite(result.radiance))) return (ReflectionRayHit)0;
    result.radiance = clamp(result.radiance, 0.0.xxx, 65504.0.xxx);
    result.confidence = 1.0 - smoothstep(0.70, 1.0, result.distance / kMaxDistance);
    return result;
}
