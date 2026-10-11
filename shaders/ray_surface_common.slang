#pragma once
#include "ray_scene_data.slang"

// Dedicated optional buffers: 192 bytes/triangle and 112 bytes/instance.
// Matches RaySurfaceTriangleData/RaySurfaceInstanceData in RaySceneExtraction.h.
// No bindings are declared here; each consumer owns its buffers/lifetime.
struct RaySurfaceTriangleData
{
    float4 positions[3];
    float4 normals[3];
    float4 tangents[3];
    float4 colors[3];
};
struct RaySurfaceInstanceData
{
    column_major float4x4 model;
    // Inverse-transpose columns; normalColumns[0].w is model handedness.
    float4 normalColumns[3];
};
struct RaySurfacePoint
{
    float3 position;
    float3 geometricNormal;
    float3 shadingNormal;
    float4 tangent;
    float3 bitangent;
    float3 color;
    float4 uv;
    uint materialIndex;
    uint objectFlags;
    uint frontFace;
    uint valid;
};

float3 RaySurfaceNormalize(float3 value, float3 fallback)
{
    if (!all(isfinite(value))) return fallback;
    float scale = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (scale <= 0.0) return fallback;
    value /= scale;
    return value * rsqrt(dot(value, value));
}
float3 RaySurfaceFallbackTangent(float3 normal)
{
    float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                      : float3(1.0, 0.0, 0.0);
    return RaySurfaceNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}
float3 RaySurfaceTransformNormal(RaySurfaceInstanceData instance, float3 normal)
{
    return instance.normalColumns[0].xyz * normal.x +
           instance.normalColumns[1].xyz * normal.y +
           instance.normalColumns[2].xyz * normal.z;
}

// Accept only an already selected, consumer-approved hit. Closest traversal
// must continue after candidate commits (no ACCEPT_FIRST_HIT flag). The caller
// owns clipping, alpha, transmission/displacement support and ray-facing policy.
// An attribute-valid point is not a claim of a supported shaded material.
RaySurfacePoint ReconstructRaySurface(RayTriangleData uv,
    RaySurfaceTriangleData triangle, RayInstanceData metadata,
    RaySurfaceInstanceData instance, float2 barycentrics, bool frontFace)
{
    RaySurfacePoint result = (RaySurfacePoint)0;
    result.materialIndex = metadata.info.y;
    result.objectFlags = metadata.info.z;
    result.frontFace = frontFace ? 1u : 0u;
    if (!all(isfinite(barycentrics)) || any(barycentrics < 0.0.xx) ||
        barycentrics.x + barycentrics.y > 1.0) return result;
    for (uint column = 0u; column < 4u; ++column)
        if (!all(isfinite(instance.model[column]))) return result;
    for (uint column = 0u; column < 3u; ++column)
        if (!all(isfinite(instance.normalColumns[column]))) return result;

    float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                            barycentrics.x, barycentrics.y);
    float3 edge0 = RaySurfaceNormalize(
        triangle.positions[1].xyz - triangle.positions[0].xyz, 0.0.xxx);
    float3 edge1 = RaySurfaceNormalize(
        triangle.positions[2].xyz - triangle.positions[0].xyz, 0.0.xxx);
    float3 objectGeometricNormal = RaySurfaceNormalize(cross(edge0, edge1), 0.0.xxx);
    if (dot(objectGeometricNormal, objectGeometricNormal) == 0.0) return result;
    // Vulkan's ray-facing classification uses object-space winding. Transform
    // the authored face normal, rather than crossing mirrored world edges.
    result.geometricNormal = RaySurfaceNormalize(
        RaySurfaceTransformNormal(instance, objectGeometricNormal), 0.0.xxx);
    if (dot(result.geometricNormal, result.geometricNormal) == 0.0) return result;

    float3 objectPosition = 0.0.xxx;
    float3 worldNormal = 0.0.xxx, worldTangent = 0.0.xxx;
    float tangentHandedness = 0.0;
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        if (!all(isfinite(triangle.positions[corner])) ||
            !all(isfinite(triangle.normals[corner])) ||
            !all(isfinite(triangle.tangents[corner])) ||
            !all(isfinite(triangle.colors[corner])) ||
            !all(isfinite(uv.uv[corner]))) return result;
        objectPosition += triangle.positions[corner].xyz * weights[corner];
        // Match raster's per-vertex transform/normalization before interpolation
        // under nonuniform scales. Re-orthogonalize the interpolated frame below.
        float3 normal = RaySurfaceNormalize(
            RaySurfaceTransformNormal(instance, triangle.normals[corner].xyz),
            result.geometricNormal);
        float3 tangent = mul((float3x3)instance.model, triangle.tangents[corner].xyz);
        tangent -= normal * dot(normal, tangent);
        tangent = RaySurfaceNormalize(tangent, RaySurfaceFallbackTangent(normal));
        float handedness = abs(triangle.tangents[corner].w) > 0.5
            ? triangle.tangents[corner].w : 1.0;
        handedness *= instance.normalColumns[0].w;
        handedness = handedness < 0.0 ? -1.0 : 1.0;
        worldNormal += normal * weights[corner];
        worldTangent += tangent * weights[corner];
        tangentHandedness += handedness * weights[corner];
        result.color += triangle.colors[corner].xyz * weights[corner];
        result.uv += uv.uv[corner] * weights[corner];
    }
    result.position = mul(instance.model, float4(objectPosition, 1.0)).xyz;
    result.shadingNormal = RaySurfaceNormalize(worldNormal, result.geometricNormal);
    worldTangent -= result.shadingNormal * dot(result.shadingNormal, worldTangent);
    worldTangent = RaySurfaceNormalize(worldTangent,
        RaySurfaceFallbackTangent(result.shadingNormal));
    float sign = abs(tangentHandedness) > 0.5 && tangentHandedness < 0.0 ? -1.0 : 1.0;
    result.tangent = float4(worldTangent, sign);
    result.bitangent = RaySurfaceNormalize(
        cross(result.shadingNormal, worldTangent) * sign, 0.0.xxx);
    result.valid = all(isfinite(result.position)) && all(isfinite(result.color)) &&
        all(isfinite(result.uv)) && all(isfinite(result.shadingNormal)) &&
        all(isfinite(result.tangent)) && all(isfinite(result.bitangent)) ? 1u : 0u;
    return result;
}

// Keep canonical geometry and frontFace for relocation. A lighting consumer
// separately opts into glTF's back-face normal orientation for double-sided
// materials, after resolving both instance and material flags.
RaySurfacePoint OrientRaySurfaceForShading(RaySurfacePoint point, uint materialFlags)
{
    if (point.frontFace == 0u && ((point.objectFlags | materialFlags) & 4u) != 0u)
    {
        point.shadingNormal = -point.shadingNormal;
        point.bitangent = -point.bitangent;
    }
    return point;
}
