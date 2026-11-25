#pragma once

float4x4 StableFrustumViewProj(float4x4 viewProj, float4x4 unjittered,
                              uint temporalActive, float2 renderExtent)
{
    if (temporalActive != 0u && all(renderExtent > 0.0))
    {
        unjittered[0] *= renderExtent.x / (renderExtent.x + 1.0);
        unjittered[1] *= renderExtent.y / (renderExtent.y + 1.0);
        return unjittered;
    }
    return viewProj;
}

// Extract frustum planes from the viewProj matrix (Gribb-Hartmann method).
// Planes point inward (positive half-space = inside frustum).
struct FrustumPlanes
{
    float4 planes[6];
};

FrustumPlanes ExtractFrustumPlanes(float4x4 vp)
{
    FrustumPlanes f;
    // In Slang, matrix indexing is row-first: vp[row][column]. We multiply
    // clip = mul(vp, worldPosition), so the clip inequalities must be built
    // from rows, not GLSL-style column access.
    // Left
    f.planes[0] = float4(vp[3][0] + vp[0][0],
                         vp[3][1] + vp[0][1],
                         vp[3][2] + vp[0][2],
                         vp[3][3] + vp[0][3]);
    // Right
    f.planes[1] = float4(vp[3][0] - vp[0][0],
                         vp[3][1] - vp[0][1],
                         vp[3][2] - vp[0][2],
                         vp[3][3] - vp[0][3]);
    // Bottom
    f.planes[2] = float4(vp[3][0] + vp[1][0],
                         vp[3][1] + vp[1][1],
                         vp[3][2] + vp[1][2],
                         vp[3][3] + vp[1][3]);
    // Top
    f.planes[3] = float4(vp[3][0] - vp[1][0],
                         vp[3][1] - vp[1][1],
                         vp[3][2] - vp[1][2],
                         vp[3][3] - vp[1][3]);
    // Near (Vulkan zero-to-one: clip z >= 0, i.e. the z row)
    f.planes[4] = float4(vp[2][0],
                         vp[2][1],
                         vp[2][2],
                         vp[2][3]);
    // Far (Vulkan zero-to-one: clip z <= w, i.e. w - z >= 0)
    f.planes[5] = float4(vp[3][0] - vp[2][0],
                         vp[3][1] - vp[2][1],
                         vp[3][2] - vp[2][2],
                         vp[3][3] - vp[2][3]);

    // Normalize each plane.
    for (int i = 0; i < 6; ++i)
    {
        float len = length(f.planes[i].xyz);
        if (len > 1e-6)
            f.planes[i] /= len;
    }
    return f;
}

bool SphereFrustumTest(FrustumPlanes f, float3 center, float radius)
{
    for (int i = 0; i < 6; ++i)
    {
        float dist = dot(f.planes[i].xyz, center) + f.planes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}
