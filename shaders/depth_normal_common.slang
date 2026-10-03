#pragma once
#include "brdf_common.slang"

bool TryDepthNeighborPosition(int2 pixel, uint2 extent,
    Texture2D<float> depthTexture, column_major float4x4 inverseViewProj,
    out float3 position)
{
    position = 0.0.xxx;
    if (any(pixel < int2(0, 0)) || any(pixel >= int2(extent))) return false;
    float depth = depthTexture.Load(int3(pixel, 0));
    if (!isfinite(depth) || depth <= 0.0 || depth > 1.0) return false;
    float2 uv = (float2(pixel) + 0.5.xx) / float2(extent);
    return TryReconstructWorldPosition(uv, depth, inverseViewProj, position);
}

// Explicit neighbors work even when other lanes returned for background or an
// empty cluster. Pick the shorter difference on each axis to avoid silhouettes.
float3 ReconstructDepthGeometricNormal(uint2 pixel, uint2 extent,
    float3 position, float3 fallback, Texture2D<float> depthTexture,
    column_major float4x4 inverseViewProj)
{
    float3 left, right, top, bottom;
    bool hasLeft = TryDepthNeighborPosition(int2(pixel) - int2(1, 0), extent,
        depthTexture, inverseViewProj, left);
    bool hasRight = TryDepthNeighborPosition(int2(pixel) + int2(1, 0), extent,
        depthTexture, inverseViewProj, right);
    bool hasTop = TryDepthNeighborPosition(int2(pixel) - int2(0, 1), extent,
        depthTexture, inverseViewProj, top);
    bool hasBottom = TryDepthNeighborPosition(int2(pixel) + int2(0, 1), extent,
        depthTexture, inverseViewProj, bottom);
    if ((!hasLeft && !hasRight) || (!hasTop && !hasBottom)) return fallback;
    float3 dx = hasRight ? right - position : position - left;
    float3 dy = hasBottom ? bottom - position : position - top;
    if (hasLeft && (!hasRight || length(position - left) < length(dx)))
        dx = position - left;
    if (hasTop && (!hasBottom || length(position - top) < length(dy)))
        dy = position - top;
    // If both neighbors along an axis are on other surfaces, prefer the stored
    // normal over a cross product spanning unrelated geometry.
    float xLength = length(dx);
    float yLength = length(dy);
    if (max(xLength, yLength) > 8.0 * max(min(xLength, yLength), 1e-5))
        return fallback;
    float3 normal = SafeNormalize(cross(dy, dx), fallback);
    return dot(normal, fallback) < 0.0 ? -normal : normal;
}
