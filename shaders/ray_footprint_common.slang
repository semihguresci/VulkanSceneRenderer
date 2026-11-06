#pragma once

// Ray-cone texture footprints (Akenine-Moller et al. 2019). This mirrors the
// nvpro-samples vk_gltf_renderer reference: a ray carries a world-space
// diameter at its origin plus a constant angular spread, secondary rays
// inherit the slant-corrected footprint of the surface they leave, and hit
// shading converts the world footprint into per-UV-set gradient magnitudes
// for explicit-gradient texture sampling. Coverage candidates and shadow
// connections carry no footprint and keep base-mip reads, exactly like the
// reference renderer's shadow paths.
struct RayCone
{
    float width;  // World-space diameter at the ray origin.
    float spread; // Diameter growth per unit travel (full cone angle tangent).
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

float RayConeSolidAngleSpread(float raysOverSphere)
{
    // N equal-solid-angle rays over the sphere each own 4*pi/N steradians.
    // The equivalent cone grows its diameter by 2*tan(half angle) per unit
    // travel. Tiny ray counts stay finite through the sine clamp.
    float sine = min(2.0 / sqrt(max(raysOverSphere, 1.0)), 1.0);
    return 2.0 * (sine / sqrt(max(1.0 - sine * sine, 1e-6)));
}
