#pragma once
#include "ray_shadow_data.slang"
#include "material_data_common.slang"

struct RayTriangleData { float4 uv[3]; };
struct RayInstanceData { uint4 info; };
[[vk::binding(0, 2)]] ConstantBuffer<RayShadowTraceData> uRayTrace;
[[vk::binding(1, 2)]] RaytracingAccelerationStructure gRayScene;
[[vk::binding(2, 2)]] StructuredBuffer<RayTriangleData> uRayTriangles;
[[vk::binding(3, 2)]] StructuredBuffer<RayInstanceData> uRayInstances;

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
    float3 orientedNormal = dot(normal, direction) >= 0.0 ? normal : -normal;
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
