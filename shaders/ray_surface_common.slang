#pragma once
#include "ray_scene_data.slang"

// Matches Container/utility/SceneData.h. This describes local deformation,
// independently of the instance model determinant stored below.
static const uint kObjectFlagDeformationWindingFlipped = 1u << 5;

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
    float colorAlpha;
    float4 uv;
    // sqrt(UV area / world area) per coordinate set (x = UV0, y = UV1).
    // Multiplied by a world-space ray footprint it gives the UV gradient
    // magnitude for explicit-gradient texture filtering; 0 marks a
    // set with no usable footprint.
    float2 texelDensity;
    uint materialIndex;
    uint objectFlags;
    uint frontFace;
    uint valid;
    // Local reconstruction diagnostics only; this point is never a buffer ABI.
    uint reconstructionFault;
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

bool RaySurfaceCanonicalizeBarycentrics(inout float2 barycentrics)
{
    if (!all(isfinite(barycentrics)) || any(barycentrics < 0.0.xx) ||
        any(barycentrics > 1.0.xx)) return false;
    const float larger = max(barycentrics.x, barycentrics.y);
    if (larger >= 0.5)
    {
        // Hardware intersection coordinates are float32. At a shared edge a
        // valid committed hit can round just beyond u+v=1. Preserve the larger
        // coordinate and its exact Sterbenz complement, accepting no more than
        // one float32 ULP at unity. Computing u+v first would accept a looser
        // band because its rounding can hide an excess greater than this bound.
        precise float complement = 1.0 - larger;
        precise float excess = min(barycentrics.x, barycentrics.y) - complement;
        if (excess > 1.1920928955078125e-7) return false;
        if (excess > 0.0)
        {
            if (barycentrics.x <= barycentrics.y) barycentrics.x = complement;
            else barycentrics.y = complement;
        }
    }
    return true;
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
    if (!all(isfinite(barycentrics))) { result.reconstructionFault = 8192u; return result; }
    if (any(barycentrics < 0.0.xx)) { result.reconstructionFault = 16384u; return result; }
    if (!RaySurfaceCanonicalizeBarycentrics(barycentrics)) { result.reconstructionFault = 32768u; return result; }
    for (uint column = 0u; column < 4u; ++column)
        if (!all(isfinite(instance.model[column]))) { result.reconstructionFault = 65536u; return result; }
    for (uint column = 0u; column < 3u; ++column)
        if (!all(isfinite(instance.normalColumns[column]))) { result.reconstructionFault = 131072u; return result; }

    float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                            barycentrics.x, barycentrics.y);
    float3 edge0 = RaySurfaceNormalize(
        triangle.positions[1].xyz - triangle.positions[0].xyz, 0.0.xxx);
    float3 edge1 = RaySurfaceNormalize(
        triangle.positions[2].xyz - triangle.positions[0].xyz, 0.0.xxx);
    float3 objectGeometricNormal = RaySurfaceNormalize(cross(edge0, edge1), 0.0.xxx);
    if (dot(objectGeometricNormal, objectGeometricNormal) == 0.0) { result.reconstructionFault = 262144u; return result; }
    if ((metadata.info.z & kObjectFlagDeformationWindingFlipped) != 0u)
        objectGeometricNormal = -objectGeometricNormal;
    // Vulkan's ray-facing classification uses object-space winding. Transform
    // the authored face normal, correcting local mirrored deformation just as
    // the TLAS facing flag does. Normals/tangent.w already contain skinning.
    result.geometricNormal = RaySurfaceNormalize(
        RaySurfaceTransformNormal(instance, objectGeometricNormal), 0.0.xxx);
    if (dot(result.geometricNormal, result.geometricNormal) == 0.0) { result.reconstructionFault = 524288u; return result; }

    float3 objectPosition = 0.0.xxx;
    float3 worldNormal = 0.0.xxx, worldTangent = 0.0.xxx;
    float tangentHandedness = 0.0;
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        if (!all(isfinite(triangle.positions[corner])) ||
            !all(isfinite(triangle.normals[corner])) ||
            !all(isfinite(triangle.tangents[corner])) ||
            !all(isfinite(triangle.colors[corner])) ||
            !all(isfinite(uv.uv[corner]))) { result.reconstructionFault = 1048576u; return result; }
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
        result.uv += uv.uv[corner] * weights[corner];
    }
    // Affine differences preserve constant attributes exactly. Summing three
    // independently rounded barycentric products can turn white into >1 and
    // incorrectly reject a physically valid unit-albedo reference material.
    float4 color = triangle.colors[0] +
        (triangle.colors[1] - triangle.colors[0]) * barycentrics.x +
        (triangle.colors[2] - triangle.colors[0]) * barycentrics.y;
    // A valid triangle's interpolant lies in its componentwise vertex hull.
    // Enforce that bound against rounding, preserving authored HDR values.
    color = clamp(color, min(triangle.colors[0], min(triangle.colors[1], triangle.colors[2])),
                         max(triangle.colors[0], max(triangle.colors[1], triangle.colors[2])));
    result.color = color.xyz;
    result.colorAlpha = color.w;
    // Per-set UV-to-world area ratio for ray-footprint texture filtering.
    // World edges use the instance's linear transform, matching the
    // interpolated position above. An unavailable footprint keeps the base
    // mip for that UV set, without invalidating the surface attributes.
    float3 worldEdge0 = mul((float3x3)instance.model,
        triangle.positions[1].xyz - triangle.positions[0].xyz);
    float3 worldEdge1 = mul((float3x3)instance.model,
        triangle.positions[2].xyz - triangle.positions[0].xyz);
    float worldArea = length(cross(worldEdge0, worldEdge1));
    [unroll] for (uint set = 0u; set < 2u; ++set)
    {
        float2 first = set == 0u ? uv.uv[0].xy : uv.uv[0].zw;
        float2 second = set == 0u ? uv.uv[1].xy : uv.uv[1].zw;
        float2 third = set == 0u ? uv.uv[2].xy : uv.uv[2].zw;
        float2 edgeU = second - first, edgeV = third - first;
        float uvArea = abs(edgeU.x * edgeV.y - edgeU.y * edgeV.x);
        float density = uvArea > 0.0 && isfinite(uvArea) &&
                        worldArea > 0.0 && isfinite(worldArea)
            ? sqrt(uvArea / worldArea) : 0.0;
        result.texelDensity[set] = isfinite(density) ? density : 0.0;
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
        isfinite(result.colorAlpha) &&
        all(isfinite(result.uv)) && all(isfinite(result.shadingNormal)) &&
        all(isfinite(result.tangent)) && all(isfinite(result.bitangent)) ? 1u : 0u;
    if (!all(isfinite(result.position))) result.reconstructionFault |= 2097152u;
    if (!all(isfinite(result.color)) || !isfinite(result.colorAlpha)) result.reconstructionFault |= 4194304u;
    if (!all(isfinite(result.uv))) result.reconstructionFault |= 8388608u;
    if (!all(isfinite(result.shadingNormal)) || !all(isfinite(result.tangent)) || !all(isfinite(result.bitangent)))
        result.reconstructionFault |= 16777216u;
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

// point.shadingNormal is the oriented interpolated normal BEFORE normal-map
// evaluation; geometricNormal is the triangle face normal and cannot replace
// it on a smooth surface. TANGENT.w already contains instance/deformation
// handedness. Project the authored rotated frame onto the final sampled normal.
float3 ResolveRayAnisotropyTangent(RaySurfacePoint point, float3 finalNormal,
                                  float2 textureRG, float rotation)
{
    float3 normal = RaySurfaceNormalize(point.shadingNormal, float3(0, 1, 0));
    float3 tangent = point.tangent.xyz - normal * dot(normal, point.tangent.xyz);
    tangent = RaySurfaceNormalize(tangent, RaySurfaceFallbackTangent(normal));
    float sign = isfinite(point.tangent.w) && point.tangent.w < 0.0 ? -1.0 : 1.0;
    float3 bitangent = cross(normal, tangent) * sign;
    float2 direction = all(isfinite(textureRG)) ? textureRG * 2.0 - 1.0 : float2(1, 0);
    direction = RaySurfaceNormalize(float3(direction, 0), float3(1, 0, 0)).xy;
    float angle = isfinite(rotation) ? rotation : 0.0;
    float c = cos(angle), s = sin(angle);
    float2 rotated = float2(c * direction.x - s * direction.y,
                            s * direction.x + c * direction.y);
    float3 result = tangent * rotated.x + bitangent * rotated.y;
    finalNormal = RaySurfaceNormalize(finalNormal, normal);
    result -= finalNormal * dot(finalNormal, result);
    return RaySurfaceNormalize(result, RaySurfaceFallbackTangent(finalNormal));
}
