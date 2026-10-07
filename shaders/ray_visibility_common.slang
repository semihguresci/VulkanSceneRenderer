#pragma once
#include "ray_shadow_data.slang"
#include "material_data_common.slang"
#include "brdf_common.slang"
#include "area_light_common.slang"

#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 2
#endif

struct RayTriangleData { float4 uv[3]; };
struct RayInstanceData { uint4 info; };
[[vk::binding(0, RAY_SCENE_SET)]] ConstantBuffer<RayShadowTraceData> uRayTrace;
[[vk::binding(1, RAY_SCENE_SET)]] RaytracingAccelerationStructure gRayScene;
[[vk::binding(2, RAY_SCENE_SET)]] StructuredBuffer<RayTriangleData> uRayTriangles;
[[vk::binding(3, RAY_SCENE_SET)]] StructuredBuffer<RayInstanceData> uRayInstances;

uint RayHash(uint value)
{
    value ^= value >> 16u; value *= 0x7feb352du;
    value ^= value >> 15u; value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
float RayRandom(uint seed) { return float(RayHash(seed) >> 8u) / 16777216.0; }

float4 RayTexture(uint index, float2 uv)
{
    if (index == 0xffffffffu) return 1.0.xxxx;
    uint textureIndex = NonUniformResourceIndex(index);
    uint samplerIndex = NonUniformResourceIndex(min(
        uTextureMetadata[textureIndex].samplerIndex, MATERIAL_SAMPLER_DESCRIPTOR_COUNT - 1u));
    // Inline traversal has no raster derivatives. Base mip preserves thin
    // cutouts; texture transforms, UV sets, opacity factors/channels are shared.
    return materialTextures[textureIndex].SampleLevel(materialSamplers[samplerIndex], uv, 0.0);
}

float TraceRayVisibility(float3 world, float3 normal, float3 direction, float distance)
{
    float scale = max(max(abs(world.x), abs(world.y)), abs(world.z));
    float bias = max(2e-5, scale * 2e-7);
    // Missing or discontinuous depth neighbours cannot provide a receiver
    // normal. Move along the visibility ray in that case; an invented normal
    // can move the origin behind a thin receiver and make it occlude itself.
    float3 orientedNormal = SafeNormalize(normal, direction);
    if (dot(orientedNormal, direction) < 0.0) orientedNormal = -orientedNormal;
    RayDesc ray;
    ray.Origin = world + orientedNormal * bias;
    ray.Direction = direction;
    ray.TMin = bias;
    ray.TMax = max(distance - bias * 2.0, bias);
    // Visibility rays travel receiver -> light, opposite the raster shadow
    // camera. Cull query-front faces to match raster's light-facing back cull.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> query;
    query.TraceRayInline(gRayScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        RayInstanceData instance = uRayInstances[query.CandidateInstanceID()];
        float3 hit = ray.Origin + ray.Direction * query.CandidateTriangleRayT();
        if (SectionPlaneClips(hit, uRayTrace.frame.z, uRayTrace.sectionPlane) ||
            BoxClipPlanesClip(hit, uRayTrace.boxClip)) continue;
        GpuMaterial material = uMaterials[instance.info.y];
        uint flags = instance.info.z | material.flags;
        // Blended/transmissive surfaces intentionally do not cast binary
        // shadows. Their receivers continue to use the raster route.
        if ((flags & 2u) != 0u || material.transmissionFactor > 0.0) continue;
        if ((flags & 1u) != 0u)
        {
            RayTriangleData triangle = uRayTriangles[instance.info.x + query.CandidatePrimitiveIndex()];
            float2 bary = query.CandidateTriangleBarycentrics();
            float4 uv = triangle.uv[0] * (1.0 - bary.x - bary.y) +
                        triangle.uv[1] * bary.x + triangle.uv[2] * bary.y;
            float alpha = material.color.a * saturate(material.opacityFactor);
            alpha *= RayTexture(material.baseColorTextureIndex,
                ApplyGpuTextureTransform(uv.xy, uv.zw, material.baseColorTextureTransform)).a;
            alpha *= SelectGpuTextureChannel(RayTexture(material.opacityTextureIndex,
                ApplyGpuTextureTransform(uv.xy, uv.zw, material.opacityTextureTransform)),
                material.opacityTextureTransform, 0u);
            if (!isfinite(alpha) || alpha < material.alphaCutoff) continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float TraceAreaRayVisibility(AreaLightData light, float3 world, float3 normal,
                            uint2 pixel, uint layer)
{
    if (!all(isfinite(light.colorIntensity)) || light.colorIntensity.a <= 0.0 ||
        !any(light.colorIntensity.rgb > 0.0.xxx))
        return 1.0;
    AreaLightFrame frame = BuildAreaLightFrame(light);
    uint samples = uRayTrace.read.options.z != 0u ? uRayTrace.read.limits.w : 1u;
    float sum = 0.0, weights = 0.0;
    uint extentWidth = uint(uRayTrace.camera.renderExtent.x);
    uint seed = RayHash(pixel.x + pixel.y * extentWidth + layer * 97531u +
                        uRayTrace.frame.y * 12347u);
    for (uint i = 0; i < samples; ++i)
    {
        float2 uv = float2((float(i) + RayRandom(seed + i * 2u)) / float(samples),
                          frac(float(i) * 0.61803398875 + RayRandom(seed + i * 2u + 1u)));
        float2 offset = uv * 2.0 - 1.0;
        if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
            offset = sqrt(uv.x) * float2(cos(uv.y * 2.0 * PI), sin(uv.y * 2.0 * PI));
        if (uRayTrace.read.options.z == 0u) offset = 0.0.xx;
        float3 origin = frame.position + frame.tangent * offset.x * frame.halfSize.x +
                        frame.bitangent * offset.y * frame.halfSize.y;
        float3 direction;
        float weight = AreaLightPositionContributionWeight(light, frame, world, origin, 1.0, direction);
        if (weight > 0.0)
        {
            sum += weight * TraceRayVisibility(world, normal, direction, length(origin - world));
            weights += weight;
        }
    }
    return weights > 1e-8 ? sum / weights : 1.0;
}
