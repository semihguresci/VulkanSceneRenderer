#pragma once

// Shared binary-shadow metadata. Matches RaySceneExtraction.h. Optional
// closest-hit attributes live in separate buffers in ray_surface_common.slang.
struct RayTriangleData
{
    float4 uv[3];
    float4 vertexAlpha; // xyz = triangle corners; w reserved
};
struct RayInstanceData { uint4 info; };

float RayTriangleColorAlpha(RayTriangleData triangle, float2 bary)
{
    float3 alpha = triangle.vertexAlpha.xyz;
    float value = alpha.x + (alpha.y - alpha.x) * bary.x + (alpha.z - alpha.x) * bary.y;
    return clamp(value, min(alpha.x, min(alpha.y, alpha.z)), max(alpha.x, max(alpha.y, alpha.z)));
}
