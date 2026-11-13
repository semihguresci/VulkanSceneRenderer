#ifndef OCCLUSION_SPHERE_COMMON_SLANG
#define OCCLUSION_SPHERE_COMMON_SLANG

// Allow for rounding in the clip-space dot products and tangent calculation.
static const float kSphereProjectionRoundoff = 8.0 * 1.192092896e-7;

struct SphereClipW
{
    float center;
    float error;
    float minimum;
    float3 radiusRatio;
    float denominator;
};

float SphereVectorLength(float3 value)
{
    // Scale first so very large or small clip coefficients keep their norm.
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    return scale > 0.0 ? scale * length(value / scale) : scale;
}

float ClipCenterError(float4 row, float4 center)
{
    return kSphereProjectionRoundoff * dot(abs(row), abs(center));
}

float2 ProjectSphereAxis(float3 radiusRow, float clipCoordinate, float coordinateError,
                         SphereClipW w)
{
    float center = clipCoordinate / w.center;
    float3 axis = radiusRow / w.center - center * w.radiusRatio;

    // For a unit sphere offset u, the projected offset t is
    // dot(axis, u) / (1 + dot(w.radiusRatio, u)). At an extremum,
    // |t| equals length(axis - t * w.radiusRatio).
    float crossTerm = dot(w.radiusRatio, axis);
    float axisSquared = dot(axis, axis);
    float root = sqrt(crossTerm * crossTerm + w.denominator * axisSquared);
    float outer = root + abs(crossTerm);
    float largeOffset = outer / w.denominator;
    // Rationalize the smaller root to avoid subtracting nearly equal values.
    float smallOffset = outer > 0.0 ? axisSquared / outer : 0.0;
    float2 bounds = center + (crossTerm >= 0.0
        ? float2(-largeOffset, smallOffset) : float2(-smallOffset, largeOffset));

    float magnitude = max(abs(bounds.x), abs(bounds.y));
    float arithmeticError = kSphereProjectionRoundoff *
        (w.center * (1.0 + abs(center) + magnitude) + SphereVectorLength(radiusRow));
    float padding = (coordinateError + magnitude * w.error + arithmeticError) / w.minimum;
    return bounds + float2(-padding, padding);
}

// Bound the actual jittered clip ratios. This also covers asymmetric frusta
// and the orthographic case, where the spatial part of clip W is zero.
bool ProjectSphere(float4x4 viewProj, float3 center, float radius,
                   out float2 minUV, out float2 maxUV, out float closestDepth)
{
    minUV = 0.0;
    maxUV = 1.0;
    closestDepth = 1.0;
    if (!all(isfinite(center)) || !isfinite(radius) || radius <= 0.0)
        return false;

    float4 worldCenter = float4(center, 1.0);
    float4 clipCenter = mul(viewProj, worldCenter);
    // Slang indexes matrix rows, including for column-major buffer storage.
    float3 wRow = viewProj[3].xyz;
    float3 wRadiusRow = radius * wRow;
    float wRadius = SphereVectorLength(wRadiusRow);
    SphereClipW w;
    w.center = clipCenter.w;
    w.error = ClipCenterError(viewProj[3], worldCenter) + kSphereProjectionRoundoff * wRadius;
    w.minimum = w.center - wRadius - w.error;
    if (!all(isfinite(clipCenter)) || !isfinite(w.minimum) ||
        w.minimum <= kSphereProjectionRoundoff * w.center)
        return false;

    // Vulkan reverse-Z's near clip plane is clip Z = clip W. Leave spheres
    // touching or crossing it to the raster depth test.
    float zError = ClipCenterError(viewProj[2], worldCenter);
    float nearRadius = SphereVectorLength(radius * (wRow - viewProj[2].xyz));
    float nearMargin = clipCenter.w - clipCenter.z - nearRadius;
    if (!isfinite(nearMargin) || nearMargin <= w.error + zError + kSphereProjectionRoundoff * nearRadius)
        return false;

    w.radiusRatio = wRadiusRow / w.center;
    float wRadiusOverW = length(w.radiusRatio);
    w.denominator = (1.0 - wRadiusOverW) * (1.0 + wRadiusOverW);
    if (!(w.denominator > 0.0))
        return false;

    float2 ndcX = ProjectSphereAxis(radius * viewProj[0].xyz, clipCenter.x,
                                   ClipCenterError(viewProj[0], worldCenter), w);
    float2 ndcY = ProjectSphereAxis(radius * viewProj[1].xyz, clipCenter.y,
                                   ClipCenterError(viewProj[1], worldCenter), w);
    float2 ndcZ = ProjectSphereAxis(radius * viewProj[2].xyz, clipCenter.z, zError, w);
    if (!all(isfinite(ndcX)) || !all(isfinite(ndcY)) || !isfinite(ndcZ.y))
        return false;

    // The scene uses a negative-height viewport, so framebuffer UV flips Y.
    minUV = saturate(float2(ndcX.x, -ndcY.y) * 0.5 + 0.5);
    maxUV = saturate(float2(ndcX.y, -ndcY.x) * 0.5 + 0.5);
    closestDepth = saturate(ndcZ.y);
    return all(minUV < maxUV);
}

#endif
