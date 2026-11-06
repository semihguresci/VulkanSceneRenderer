#define DDGI_SET 3
#include "material_data_common.slang"
[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> uPointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
#include "area_light_common.slang"
#include "ray_surface_common.slang"
#include "ray_footprint_common.slang"
#include "ddgi_material_common.slang"
#include "ddgi_transport_common.slang"
#include "ddgi_lookup_common.slang"

[[vk::binding(0, 2)]] ConstantBuffer<DdgiTraceSceneData> uDdgiScene;
[[vk::binding(1, 2)]] RaytracingAccelerationStructure gDdgiScene;
[[vk::binding(2, 2)]] StructuredBuffer<RayTriangleData> uDdgiTriangles;
[[vk::binding(3, 2)]] StructuredBuffer<RayInstanceData> uDdgiInstances;
[[vk::binding(4, 2)]] StructuredBuffer<RaySurfaceTriangleData> uDdgiSurfaceTriangles;
[[vk::binding(5, 2)]] StructuredBuffer<RaySurfaceInstanceData> uDdgiSurfaceInstances;
[[vk::binding(5, DDGI_SET)]] RWStructuredBuffer<DdgiRaySample> uDdgiRays;
[[vk::binding(6, DDGI_SET)]] StructuredBuffer<DdgiProbeUpdate> uDdgiUpdates;
[[vk::binding(9, DDGI_SET)]] TextureCube<float4> gDdgiEnvironment;
[[vk::binding(10, DDGI_SET)]] SamplerState gDdgiEnvironmentSampler;

float4 DdgiTextureGrad(uint index, float4 uv, GpuTextureTransform transform, float2 texGrad)
{
    if (index == kInvalidGpuMaterialTexture || index >= MATERIAL_TEXTURE_DESCRIPTOR_COUNT)
        return 1.0.xxxx;
    uint textureIndex = NonUniformResourceIndex(index);
    uint samplerIndex = NonUniformResourceIndex(
        ResolveMaterialSamplerIndex(uTextureMetadata[textureIndex]));
    float2 coord = ApplyGpuTextureTransform(uv.xy, uv.zw, transform);
    float gradient = transform.row0.w >= 0.5 ? texGrad.y : texGrad.x;
    if (gradient > 0.0)
    {
        float2 dx, dy;
        RayConeTransformGradients(gradient, transform.row0.xy, transform.row1.xy, dx, dy);
        return materialTextures[textureIndex].SampleGrad(materialSamplers[samplerIndex],
            coord, dx, dy);
    }
    return materialTextures[textureIndex].SampleLevel(materialSamplers[samplerIndex], coord, 0.0);
}
float4 DdgiTexture(uint index, float4 uv, GpuTextureTransform transform)
{
    // Coverage candidates and visibility shadow connections carry no footprint.
    return DdgiTextureGrad(index, uv, transform, 0.0.xx);
}
float DdgiScalarGrad(float factor, uint index, float4 uv, GpuTextureTransform transform,
                     uint channel, float2 texGrad)
{
    return ResolveDdgiScalarFactor(factor, DdgiTextureGrad(index, uv, transform, texGrad),
        transform, channel);
}
float DdgiScalar(float factor, uint index, float4 uv, GpuTextureTransform transform,
                 uint channel)
{
    return DdgiScalarGrad(factor, index, uv, transform, channel, 0.0.xx);
}
bool DdgiCandidateAccepted(uint instanceIndex, uint primitive, float2 bary, float3 world)
{
    if (instanceIndex >= uDdgiScene.limits.x || !all(isfinite(world)) ||
        SectionPlaneClips(world, uDdgi.frame.w, uDdgiScene.sectionPlane) ||
        BoxClipPlanesClip(world, uDdgiScene.boxClip)) return false;
    RayInstanceData instance = uDdgiInstances[instanceIndex];
    if (instance.info.x >= uDdgiScene.limits.y || primitive >= uDdgiScene.limits.y - instance.info.x)
        return false;
    GpuMaterial material = uMaterials[instance.info.y];
    if (((instance.info.z | material.flags) & kGpuMaterialFlagAlphaMask) != 0u)
    {
        RayTriangleData triangle = uDdgiTriangles[instance.info.x + primitive];
        float4 uv = triangle.uv[0] * (1.0 - bary.x - bary.y) + triangle.uv[1] * bary.x + triangle.uv[2] * bary.y;
        float alpha = material.color.a * RayTriangleColorAlpha(triangle, bary) *
            saturate(material.opacityFactor) *
            DdgiTexture(material.baseColorTextureIndex, uv, material.baseColorTextureTransform).a;
        alpha *= SelectGpuTextureChannel(DdgiTexture(material.opacityTextureIndex, uv,
            material.opacityTextureTransform), material.opacityTextureTransform, 0u);
        if (!isfinite(alpha) || alpha < material.alphaCutoff) return false;
    }
    return true;
}
struct DdgiClosestHit
{
    RaySurfacePoint point;
    uint instanceIndex;
    float distance;
    uint hit;
};
DdgiClosestHit DdgiTraceClosest(RayDesc ray)
{
    DdgiClosestHit result = (DdgiClosestHit)0;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(gDdgiScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            DdgiCandidateAccepted(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), ray.Origin + ray.Direction * query.CandidateTriangleRayT()))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return result;
    result.hit = 1u;
    result.instanceIndex = query.CommittedInstanceID();
    RayInstanceData instance = uDdgiInstances[result.instanceIndex];
    uint triangleIndex = instance.info.x + query.CommittedPrimitiveIndex();
    result.point = ReconstructRaySurface(uDdgiTriangles[triangleIndex],
        uDdgiSurfaceTriangles[triangleIndex], instance, uDdgiSurfaceInstances[result.instanceIndex],
        query.CommittedTriangleBarycentrics(), query.CommittedTriangleFrontFace());
    result.distance = query.CommittedRayT();
    return result;
}
struct DdgiSurfaceTransmission
{
    float transmission;
    float alpha;
    float metallic;
    float roughness;
    float3 tint;
    uint flags;
};
DdgiSurfaceTransmission DdgiResolveSurfaceTransmission(RaySurfacePoint point, GpuMaterial material,
                                                        float2 texGrad)
{
    DdgiSurfaceTransmission result = (DdgiSurfaceTransmission)0;
    float4 baseColor = DdgiTextureGrad(material.baseColorTextureIndex, point.uv,
        material.baseColorTextureTransform, texGrad);
    float alpha = material.color.a * point.colorAlpha * material.opacityFactor * baseColor.a *
        SelectGpuTextureChannel(DdgiTextureGrad(material.opacityTextureIndex, point.uv,
            material.opacityTextureTransform, texGrad), material.opacityTextureTransform, 0u);
    float transmission = DdgiScalarGrad(material.transmissionFactor, material.transmissionTextureIndex,
        point.uv, material.transmissionTextureTransform, 0u, texGrad);
    float metallic = material.metallicRoughness.x *
        DdgiTextureGrad(material.metallicRoughnessTextureIndex, point.uv,
            material.metallicRoughnessTextureTransform, texGrad).b *
        SelectGpuTextureChannel(DdgiTextureGrad(material.metalnessTextureIndex, point.uv,
            material.metalnessTextureTransform, texGrad), material.metalnessTextureTransform, 0u);
    uint flags = point.objectFlags | material.flags;
    alpha = ResolveGpuMaterialAlpha(alpha, material.flags);
    float roughness = material.metallicRoughness.y;
    if ((flags & 0x8u) != 0u)
        roughness = 1.0 - saturate(roughness * SelectGpuTextureChannel(
            DdgiTextureGrad(material.roughnessTextureIndex, point.uv, material.roughnessTextureTransform, texGrad),
            material.roughnessTextureTransform, 3u));
    else
        roughness *= DdgiTextureGrad(material.metallicRoughnessTextureIndex, point.uv,
            material.metallicRoughnessTextureTransform, texGrad).g * SelectGpuTextureChannel(
                DdgiTextureGrad(material.roughnessTextureIndex, point.uv, material.roughnessTextureTransform, texGrad),
                material.roughnessTextureTransform, 0u);
    result.transmission = transmission;
    result.alpha = alpha;
    result.metallic = metallic;
    result.roughness = roughness;
    result.tint = material.color.rgb * point.color * baseColor.rgb;
    result.flags = flags;
    return result;
}
DdgiThinGlassResult DdgiSurfaceThinGlass(RaySurfacePoint point, GpuMaterial material,
    float3 direction, float2 texGrad)
{
    if (point.valid == 0u || (uint(uDdgi.intensityBias.w) & 3u) == 0u)
        return (DdgiThinGlassResult)0;
    DdgiSurfaceTransmission surface = DdgiResolveSurfaceTransmission(point, material, texGrad);
    return DdgiThinGlassTransmission(material, surface.flags, surface.transmission,
        surface.alpha, surface.metallic, surface.roughness, surface.tint,
        dot(point.geometricNormal, direction),
        (uint(uDdgi.intensityBias.w) & 3u) != 0u);
}
DdgiDielectricResult DdgiSurfaceDielectric(RaySurfacePoint point, GpuMaterial material,
                                         float3 direction, uint seed, float2 texGrad, float ambientIor)
{
    if (point.valid == 0u || (uint(uDdgi.intensityBias.w) & 2u) == 0u)
        return (DdgiDielectricResult)0;
    DdgiSurfaceTransmission surface = DdgiResolveSurfaceTransmission(point, material, texGrad);
    return DdgiSampleDielectric(material, surface.flags, surface.transmission,
        surface.alpha, surface.metallic, surface.roughness, surface.tint, direction,
        point.geometricNormal, point.frontFace != 0u,
        float2(DdgiRandom(seed), DdgiRandom(seed ^ 0xa511e9b3u)), true, ambientIor);
}
float DdgiNextRayMinimum(float hitDistance, float minimumBias)
{
    // Keep the original ray origin so the reported visibility distance remains
    // in probe space after all sheets. Float-relative progress avoids repeatedly
    // hitting the same plane at large world coordinates.
    return hitDistance + max(minimumBias, abs(hitDistance) * 2e-7);
}
float3 DdgiLightVisibility(float3 world, float3 geometricNormal, float3 direction, float distance)
{
    // A tangent normal map cannot transmit incident light through the
    // geometric back hemisphere of an opaque surface.
    if (dot(geometricNormal, direction) <= 0.0) return 0.0.xxx;
    float scale = max(max(abs(world.x), abs(world.y)), abs(world.z));
    float bias = max(2e-5, scale * 2e-7);
    RayDesc ray;
    ray.Origin = world + geometricNormal * bias;
    ray.Direction = direction;
    ray.TMin = bias;
    ray.TMax = max(distance - bias * 2.0, bias);
    if ((uint(uDdgi.intensityBias.w) & 3u) == 0u)
    {
        // Preserve the inexpensive binary shadow query in the default mode.
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
        query.TraceRayInline(gDdgiScene, RAY_FLAG_NONE, 0xffu, ray);
        while (query.Proceed())
        {
            if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                DdgiCandidateAccepted(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                    query.CandidateTriangleBarycentrics(), ray.Origin + direction * query.CandidateTriangleRayT()))
                query.CommitNonOpaqueTriangleHit();
        }
        return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0.xxx : 1.0.xxx;
    }
    float3 throughput = 1.0.xxx;
    for (uint crossed = 0u; crossed <= DDGI_MAX_GLASS_CROSSINGS; ++crossed)
    {
        DdgiClosestHit hit = DdgiTraceClosest(ray);
        if (hit.hit == 0u) return throughput;
        GpuMaterial material = uMaterials[uDdgiInstances[hit.instanceIndex].info.y];
        DdgiThinGlassResult glass = DdgiSurfaceThinGlass(hit.point, material, direction, 0.0.xx);
        if (!DdgiAdvanceThinGlass(throughput, glass, crossed)) return 0.0.xxx;
        ray.TMin = DdgiNextRayMinimum(hit.distance, bias);
        if (!isfinite(ray.TMin)) return 0.0.xxx;
        if (ray.TMin >= ray.TMax) return throughput;
    }
    return 0.0.xxx;
}

DdgiShadingMaterial DdgiEvaluateMaterial(RaySurfacePoint point, GpuMaterial material,
    float3 V, float2 texGrad)
{
    DdgiShadingMaterial result = (DdgiShadingMaterial)0;
    result.albedo = max(Finite3Or(material.color.rgb * point.color *
        DdgiTextureGrad(material.baseColorTextureIndex, point.uv,
            material.baseColorTextureTransform, texGrad).rgb, 0.0.xxx), 0.0.xxx);
    result.metallic = saturate(FiniteScalarOr(material.metallicRoughness.x, 0.0) *
        DdgiTextureGrad(material.metallicRoughnessTextureIndex, point.uv,
            material.metallicRoughnessTextureTransform, texGrad).b);
    result.metallic *= SelectGpuTextureChannel(DdgiTextureGrad(material.metalnessTextureIndex,
        point.uv, material.metalnessTextureTransform, texGrad), material.metalnessTextureTransform, 0u);
    result.metallic = saturate(FiniteScalarOr(result.metallic, 0.0));
    uint flags = point.objectFlags | material.flags;
    float roughness = FiniteScalarOr(material.metallicRoughness.y, 1.0);
    if ((flags & 0x8u) != 0u)
    {
        roughness *= SelectGpuTextureChannel(DdgiTextureGrad(material.roughnessTextureIndex,
            point.uv, material.roughnessTextureTransform, texGrad), material.roughnessTextureTransform, 3u);
        roughness = 1.0 - saturate(roughness);
    }
    else
    {
        roughness *= DdgiTextureGrad(material.metallicRoughnessTextureIndex, point.uv,
            material.metallicRoughnessTextureTransform, texGrad).g;
        roughness *= SelectGpuTextureChannel(DdgiTextureGrad(material.roughnessTextureIndex,
            point.uv, material.roughnessTextureTransform, texGrad), material.roughnessTextureTransform, 0u);
    }
    result.roughness = clamp(FiniteScalarOr(roughness, 1.0), 0.045, 1.0);
    float specular = DdgiScalarGrad(material.specularFactor, material.specularTextureIndex,
        point.uv, material.specularTextureTransform, 3u, texGrad);
    float3 sampledSpecularColor = material.specularColorTextureIndex != kInvalidGpuMaterialTexture ?
        DdgiTextureGrad(material.specularColorTextureIndex, point.uv,
            material.specularColorTextureTransform, texGrad).rgb :
        DdgiTextureGrad(material.specularTextureIndex, point.uv,
            material.specularTextureTransform, texGrad).rgb;
    float3 specularColor = saturate(Finite3Or(material.specularColorFactor.rgb * sampledSpecularColor, 1.0.xxx));
    float iridescence = DdgiScalarGrad(material.iridescenceFactor, material.iridescenceTextureIndex,
        point.uv, material.iridescenceTextureTransform, 0u, texGrad);
    float filmThickness = lerp(material.iridescenceThicknessMinimum, material.iridescenceThicknessMaximum,
        saturate(SelectGpuTextureChannel(DdgiTextureGrad(material.iridescenceThicknessTextureIndex,
            point.uv, material.iridescenceThicknessTextureTransform, texGrad),
            material.iridescenceThicknessTextureTransform, 1u)));
    result.normal = point.shadingNormal;
    if (material.normalTextureIndex != kInvalidGpuMaterialTexture)
    {
        float3 tangentNormal = DdgiTextureGrad(material.normalTextureIndex, point.uv,
            material.normalTextureTransform, texGrad).xyz * 2.0 - 1.0;
        tangentNormal.xy *= FiniteScalarOr(material.normalTextureScale, 1.0);
        tangentNormal = SafeNormalize(tangentNormal, float3(0, 0, 1));
        result.normal = SafeNormalize(point.tangent.xyz * tangentNormal.x +
            point.bitangent * tangentNormal.y + point.shadingNormal * tangentNormal.z, point.shadingNormal);
        if (dot(result.normal, point.shadingNormal) < 0.0) result.normal = -result.normal;
    }
    result.f0 = EvaluatePbrF0WithDielectricTint(result.albedo, result.metallic,
        specular, specularColor, material.ior, ApproximateDeferredIridescenceTint(iridescence,
            material.iridescenceIor, filmThickness, saturate(dot(result.normal, V))));
    float clearcoat = DdgiScalarGrad(material.clearcoatFactor, material.clearcoatTextureIndex,
        point.uv, material.clearcoatTextureTransform, 0u, texGrad);
    float3 sheen = ResolveDeferredTextureBackedColor(material.sheenColorFactor.rgb,
        material.sheenColorTextureIndex) * DdgiTextureGrad(material.sheenColorTextureIndex,
            point.uv, material.sheenColorTextureTransform, texGrad).rgb;
    result.layerScale = DeferredClearcoatBaseEnergyScale(clearcoat) * DeferredSheenBaseEnergyScale(sheen);
    result.clearcoat = clearcoat;
    result.clearcoatRoughness = DdgiScalarGrad(material.clearcoatRoughnessFactor,
        material.clearcoatRoughnessTextureIndex, point.uv, material.clearcoatRoughnessTextureTransform, 1u, texGrad);
    result.sheenColor = sheen;
    result.sheenRoughness = DdgiScalarGrad(material.sheenRoughnessFactor,
        material.sheenRoughnessTextureIndex, point.uv, material.sheenRoughnessTextureTransform, 3u, texGrad);
    result.emissive = max(Finite3Or(material.emissiveColor * material.emissiveStrength *
        DdgiTextureGrad(material.emissiveTextureIndex, point.uv, material.emissiveTextureTransform, texGrad).rgb, 0.0.xxx), 0.0.xxx);
    result.occlusion = lerp(1.0, saturate(DdgiTextureGrad(material.occlusionTextureIndex, point.uv,
        material.occlusionTextureTransform, texGrad).r), saturate(FiniteScalarOr(material.occlusionStrength, 1.0)));
    result.unlit = (flags & 0x10u) != 0u ? 1u : 0u;
    // The AS has undisplaced triangles and this transport route is diffuse.
    // Keep unsupported closest geometry in visibility, with black radiance,
    // rather than accidentally turning it into an environment leak.
    float transmission = DdgiScalarGrad(material.transmissionFactor, material.transmissionTextureIndex,
        point.uv, material.transmissionTextureTransform, 0u, texGrad);
    result.extended = ResolveDdgiExtendedPbrTerms(material, point, result.normal, transmission,
        DdgiTextureGrad(material.anisotropyTextureIndex, point.uv, material.anisotropyTextureTransform, texGrad),
        DdgiTextureGrad(material.diffuseTransmissionTextureIndex, point.uv, material.diffuseTransmissionTextureTransform, texGrad),
        DdgiTextureGrad(material.diffuseTransmissionColorTextureIndex, point.uv, material.diffuseTransmissionColorTextureTransform, texGrad));
    result.unsupported = ((flags & kGpuMaterialFlagAlphaBlend) != 0u || IsThinSurfaceGpuMaterial(flags) ||
        !isfinite(material.transmissionFactor) || transmission > 0.0 || !isfinite(material.heightScale) ||
        (material.heightTextureIndex != kInvalidGpuMaterialTexture && abs(material.heightScale) > 1e-7)) ? 1u : 0u;
    return result;
}
float3 DdgiShadeSurface(RaySurfacePoint point, DdgiShadingMaterial material, float3 V, uint seed)
{
    if (material.unsupported != 0u) return 0.0.xxx;
    if (material.unlit != 0u) return material.albedo + material.emissive;
    float faceSign = point.frontFace == 0u ? -1.0 : 1.0;
    float3 geometricNormal = point.geometricNormal * faceSign;
    float3 radiance = material.emissive;
    float3 directional = max(Finite3Or(uLighting.directionalColorIntensity.rgb *
        uLighting.directionalColorIntensity.w, 0.0.xxx), 0.0.xxx);
    float3 L = SafeNormalize(-uLighting.directionalDirection.xyz, float3(0, 1, 0));
    float3 visibilityNormal = DdgiDirectVisibilityNormal(material, geometricNormal, L);
    if (any(directional > 0.0.xxx) && any(visibilityNormal != 0.0.xxx))
        radiance += EvaluateDdgiHitDirect(material, geometricNormal, V, L, directional) *
            DdgiLightVisibility(point.position, visibilityNormal, L, 1e20);
    uint pointCount = min(uLighting.pointLightCount, MAX_CLUSTERED_LIGHTS);
    uint pointSamples = min(pointCount, uDdgiScene.limits.z);
    float pointWeight = float(pointCount) / float(max(pointSamples, 1u));
    for (uint selected = 0u; selected < pointSamples; ++selected)
    {
        uint index = DdgiSelectedLightIndex(selected, pointSamples, pointCount, DdgiHash(seed ^ 0x68bc21ebu));
        PointLightData light = uPointLights[index];
        float3 delta = light.positionRadius.xyz - point.position;
        float distanceSq = dot(delta, delta);
        if (!PointLightHasRenderableIntensity(light) || !isfinite(distanceSq) || distanceSq <= 1e-10) continue;
        float distance = sqrt(distanceSq); L = delta / distance;
        float attenuation = PointLightAttenuation(distanceSq, light.positionRadius.w) *
            LocalLightAngularIntensity(light, L);
        visibilityNormal = DdgiDirectVisibilityNormal(material, geometricNormal, L);
        if (attenuation <= 0.0 || !any(visibilityNormal != 0.0.xxx)) continue;
        radiance += EvaluateDdgiHitDirect(material, geometricNormal, V, L, light.colorIntensity.rgb *
            light.colorIntensity.a * attenuation * pointWeight) * DdgiLightVisibility(point.position, visibilityNormal, L, distance);
    }
    uint areaCount = min(uLighting.areaLightCount, MAX_AREA_LIGHTS);
    uint areaSamples = min(areaCount, uDdgiScene.limits.z);
    float areaWeight = float(areaCount) / float(max(areaSamples, 1u));
    uint samples = clamp(uDdgiScene.limits.w, 1u, 64u);
    for (uint selected = 0u; selected < areaSamples; ++selected)
    {
        uint index = DdgiSelectedLightIndex(selected, areaSamples, areaCount, DdgiHash(seed ^ 0x02e5be93u));
        AreaLightData light = uAreaLights[index];
        if (!AreaLightHasRenderableRadiance(light)) continue;
        AreaLightFrame frame = BuildAreaLightFrame(light);
        for (uint sample = 0u; sample < samples; ++sample)
        {
            // Stratified uniform emitter area, PDF=1/area for both shapes.
            float2 uv = float2((float(sample) + DdgiRandom(seed + index * 131u + sample * 2u)) / float(samples),
                frac(float(sample) * 0.61803398875 + DdgiRandom(seed + index * 131u + sample * 2u + 1u)));
            float2 offset = uv * 2.0 - 1.0;
            if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
                offset = sqrt(uv.x) * float2(cos(uv.y * 2.0 * PI), sin(uv.y * 2.0 * PI));
            float3 position = frame.position + frame.tangent * offset.x * frame.halfSize.x +
                                               frame.bitangent * offset.y * frame.halfSize.y;
            float weight = AreaLightPositionContributionWeight(light, frame, point.position, position,
                1.0 / float(samples), L);
            visibilityNormal = DdgiDirectVisibilityNormal(material, geometricNormal, L);
            if (weight <= 0.0 || !any(visibilityNormal != 0.0.xxx)) continue;
            radiance += EvaluateDdgiHitDirect(material, geometricNormal, V, L, light.colorIntensity.rgb *
                light.colorIntensity.a * weight * areaWeight) * DdgiLightVisibility(point.position,
                    visibilityNormal, L, length(position - point.position));
        }
    }
    // Previous-only feedback amortizes diffuse bounces across submissions; no
    // current target atlas reads or environment double-counting at shaded hits.
    DdgiLookupResult previous = LookupDdgiIrradiance(point.position, geometricNormal, material.normal, V);
    float3 backIrradiance = 0.0.xxx;
    if (material.extended.diffuseTransmissionFactor > 0.0)
    {
        DdgiLookupResult back = LookupDdgiIrradiance(point.position, -geometricNormal, -material.normal, -V);
        backIrradiance = back.irradiance;
    }
    radiance += EvaluateDdgiDiffuseFeedback(material, V, previous.irradiance, backIrradiance);
    return max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint raysPerProbe = uDdgi.extents.w;
    if (raysPerProbe == 0u || raysPerProbe > DDGI_MAX_RAYS_PER_PROBE ||
        tid.x >= uDdgi.extents.z * raysPerProbe) return;
    uint updateIndex = tid.x / raysPerProbe, rayIndex = tid.x % raysPerProbe;
    DdgiProbeUpdate update = uDdgiUpdates[updateIndex];
    if (update.probeIndex >= uDdgi.counts.w || rayIndex >= update.rayCount) return;
    DdgiRaySample sample = (DdgiRaySample)0;
    float3 direction = DdgiRayDirection(rayIndex, update.rayCount, update.probeIndex, uDdgi.frame.x);
    sample.directionAndReserved = float4(direction, 0.0);
    sample.radianceAndDistance.w = uDdgi.trace.x;
    float3 origin = uDdgiBasePositions[update.probeIndex].xyz + uDdgiStates[update.probeIndex].relocation;
    DdgiGpuVolume volume = DdgiProbeVolumeForIndex(uDdgi, update.probeIndex);
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction;
    ray.TMin = min(max(min(min(volume.spacingNormalBias.x, volume.spacingNormalBias.y),
        volume.spacingNormalBias.z) * 1e-4, 1e-5), uDdgi.trace.x * 0.01);
    ray.TMax = uDdgi.trace.x;
    float minimumBias = ray.TMin;
    float3 throughput = 1.0.xxx;
    float traveled = 0.0;
    float segmentBias = 0.0;
    DdgiMediumStack media = (DdgiMediumStack)0;
    // Probe-ray footprints (#82): each of rayCount equal-solid-angle sphere
    // samples owns 4*pi/rayCount steradians; the equivalent cone's diameter
    // growth per unit travel drives hit-texture filtering.
    RayCone cone = (RayCone)0;
    cone.spread = RayConeSolidAngleSpread(update.rayCount);
    for (uint crossed = 0u; crossed <= DDGI_MAX_GLASS_CROSSINGS; ++crossed)
    {
        DdgiClosestHit hit = DdgiTraceClosest(ray);
        if (hit.hit == 0u)
        {
            // An unmatched volume entry must never become a clear environment
            // path, including clipped, open or malformed transmitting meshes.
            if (media.count != 0u)
            {
                sample.radianceAndDistance.w = clamp(traveled, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            float3 environment = gDdgiEnvironment.SampleLevel(gDdgiEnvironmentSampler,
                RotateEnvironmentDirection(ray.Direction, uLighting.environmentOrientation.xy), 0.0).rgb *
                max(FiniteScalarOr(uLighting.environmentIntensity, 0.0), 0.0);
            sample.radianceAndDistance.rgb = throughput * max(Finite3Or(environment, 0.0.xxx), 0.0.xxx);
            break;
        }
        RayInstanceData instance = uDdgiInstances[hit.instanceIndex];
        RaySurfacePoint point = hit.point;
        GpuMaterial material = uMaterials[instance.info.y];
        float totalDistance = traveled + hit.distance;
        // Slant-corrected world footprint of this probe-ray cone at the hit;
        // refracted continuations inherit it as their starting width.
        float worldFoot = RayConeWorldFootprint(cone, hit.distance,
            point.geometricNormal, -ray.Direction);
        float2 texGrad = worldFoot * point.texelDensity;
        bool entering = point.frontFace != 0u;
        DdgiMedium boundary = { hit.instanceIndex, instance.info.y, material.ior };
        DdgiDielectricResult dielectric = DdgiSurfaceDielectric(point, material, ray.Direction,
            DdgiHash(tid.x ^ (uDdgi.frame.x * 12347u) ^ (crossed * 0x68bc21ebu)), texGrad,
            DdgiAmbientIor(media, entering));
        if (media.count != 0u)
        {
            float3 absorption;
            DdgiMedium current = media.media[media.count - 1u];
            if (!DdgiVolumeAbsorption(uMaterials[current.materialIndex], hit.distance + segmentBias, absorption))
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            throughput *= absorption;
            // An interior opaque surface or thin sheet remains blocking.
            if (dielectric.supported == 0u || dielectric.volume == 0u)
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
        }
        if (dielectric.supported != 0u)
        {
            if (crossed >= DDGI_MAX_GLASS_CROSSINGS ||
                (dielectric.volume != 0u && !DdgiCanCrossMedium(media, boundary, entering)))
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            throughput *= dielectric.weight;
            if (!all(isfinite(throughput)) ||
                max(max(throughput.x, throughput.y), throughput.z) < DDGI_MIN_TRANSPORT_THROUGHPUT)
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            if (dielectric.volume != 0u && dielectric.totalInternalReflection == 0u)
                DdgiCommitMediumCrossing(media, boundary, entering);
            // A changed direction starts a new segment. Preserve total world
            // distance and the original probe direction in the visibility atlas.
            // This is a conservative path-distance moment, not a refracted
            // straight-line blocker location or a caustics estimator.
            float worldScale = max(max(abs(point.position.x), abs(point.position.y)), abs(point.position.z));
            segmentBias = max(minimumBias, worldScale * 2e-7);
            traveled = totalDistance + segmentBias;
            ray.Origin = point.position + dielectric.direction * segmentBias;
            ray.Direction = dielectric.direction;
            ray.TMin = segmentBias;
            ray.TMax = uDdgi.trace.x - traveled;
            if (!all(isfinite(ray.Origin)) || !isfinite(ray.TMax) || ray.TMax <= ray.TMin)
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            // The refracted segment restarts at this surface, so its cone
            // inherits the slant-corrected footprint measured here.
            cone.width = worldFoot;
            continue;
        }
        DdgiThinGlassResult glass = DdgiSurfaceThinGlass(point, material, ray.Direction, texGrad);
        if (glass.supported != 0u)
        {
            // Traversed glass is not an enclosing back face and does not turn
            // into an opaque visibility moment. Stop conservatively at the cap.
            if (!DdgiAdvanceThinGlass(throughput, glass, crossed))
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            // The straight continuation keeps the original ray origin, so the
            // cone's accumulated width + spread * total distance already
            // covers the travelled segment; no footprint update is needed.
            ray.TMin = DdgiNextRayMinimum(hit.distance, minimumBias);
            if (!isfinite(ray.TMin))
            {
                sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
                sample.directionAndReserved.w = 2.0;
                break;
            }
            if (ray.TMin >= ray.TMax) break;
            continue;
        }
        bool front = point.frontFace != 0u;
        bool doubleSided = ((instance.info.z | material.flags) & kGpuMaterialFlagDoubleSided) != 0u;
        sample.radianceAndDistance.w = clamp(totalDistance, 0.0, uDdgi.trace.x);
        // Only a straight probe ray's enclosing back face is relocation
        // evidence. A bent path can end behind an otherwise exterior surface.
        sample.directionAndReserved.w = !front && !doubleSided ? (traveled > 0.0 ? 2.0 : -1.0) : 1.0;
        if (point.valid == 0u && (front || doubleSided)) sample.directionAndReserved.w = 2.0;
        if (point.valid != 0u && (front || doubleSided))
        {
            point = OrientRaySurfaceForShading(point, material.flags);
            DdgiShadingMaterial evaluated = DdgiEvaluateMaterial(point, material, -ray.Direction, texGrad);
            sample.directionAndReserved.w = evaluated.unsupported != 0u ? 2.0 : 1.0;
            sample.radianceAndDistance.rgb = throughput * DdgiShadeSurface(point, evaluated, -ray.Direction,
                DdgiHash(tid.x + uDdgi.frame.x * 12347u));
        }
        break;
    }
    uDdgiRays[update.firstRay + rayIndex] = sample;
}
