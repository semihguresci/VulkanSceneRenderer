#pragma once

// Ray-cone texture footprints (Akenine-Moller et al. 2019). This mirrors the
// nvpro-samples vk_gltf_renderer reference: a ray carries a world-space
// diameter at its origin plus a constant angular spread, secondary rays
// inherit the slant-corrected footprint of the surface they leave, and hit
// shading converts the world footprint into per-UV-set gradient magnitudes
// for explicit-gradient texture sampling.
struct RayCone
{
    float width;  // World-space diameter at the ray origin.
    float spread; // Diameter growth per unit travel (twice the half-angle tangent).
};

float RayConeWorldFootprint(RayCone cone, float hitT, float3 normal, float3 view)
{
    // Grazing intersections stretch the cone's cross-section across the
    // surface plane; keep the reference renderer's 1e-3 slant floor.
    float width = cone.width + cone.spread * max(hitT, 0.0);
    return width / max(abs(dot(normal, view)), 1e-3);
}

float2 RayConeGradient(RayCone cone, float hitT, float3 normal, float3 view,
                       float2 texelDensity)
{
    // UV-gradient magnitude per texture-coordinate set. A degenerate set
    // (density 0) keeps the base mip, matching a texGrad of zero.
    return max(RayConeWorldFootprint(cone, hitT, normal, view), 0.0) * texelDensity;
}

bool RayConeTransformGradients(float gradient, float2 row0, float2 row1,
                               out float2 dx, out float2 dy)
{
    dx = dy = 0.0.xx;
    if (!(gradient > 0.0) || !isfinite(gradient)) return false;
    // The two UV derivatives are the transform applied to the scaled UV axes.
    dx = float2(row0.x, row1.x) * gradient;
    dy = float2(row0.y, row1.y) * gradient;
    // Finite inputs can still overflow when the authored UV transform scales
    // a large footprint. Only submit usable derivatives to the texture unit.
    if (!all(isfinite(dx)) || !all(isfinite(dy)))
    {
        dx = dy = 0.0.xx;
        return false;
    }
    return any(dx != 0.0.xx) || any(dy != 0.0.xx);
}

float RayConeSolidAngleSpread(float raysOverSphere)
{
    // N equal-solid-angle rays over the sphere each own 4*pi/N steradians.
    // The cap's half-angle has cosine 1 - 2/N. A hemisphere or larger
    // cannot fit in a finite-width forward cone, so use the saturated spread.
    if (raysOverSphere <= 2.0) return 2000.0;
    float inverseCount = 1.0 / raysOverSphere;
    float sine = 2.0 * sqrt(inverseCount * (1.0 - inverseCount));
    float cosine = 1.0 - 2.0 * inverseCount;
    return 2.0 * sine / cosine;
}
