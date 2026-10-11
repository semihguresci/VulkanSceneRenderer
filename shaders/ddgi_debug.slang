#define DDGI_SET 1
#include "lighting_structs.slang"
#include "ddgi_lookup_common.slang"

[[vk::binding(6, 0)]] Texture2D<float> gSceneDepth;
[[vk::binding(10, 0)]] ConstantBuffer<CameraBuffer> uCamera;

struct DdgiDebugPushConstants
{
    uint mode;
    float radius;
};
[[vk::push_constant]] ConstantBuffer<DdgiDebugPushConstants> pc;

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation float3 center;
    [[vk::location(1)]] nointerpolation uint probeIndex;
};

float3 DdgiDebugUnproject(float2 ndc, float depth)
{
    float4 world = mul(uCamera.inverseViewProj, float4(ndc, depth, 1.0));
    return world.xyz / world.w;
}

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(-1, 1)
    };
    VSOutput result;
    result.position = float4(2, 2, 2, 1);
    result.center = 0.0.xxx;
    result.probeIndex = instanceIndex;
    // Fallback buffers contain one initialized element. Guard array reads
    // before indexing them when the optional field is disabled/unavailable.
    if (uDdgi.frame.y == 0u || instanceIndex >= uDdgi.counts.w ||
        !isfinite(pc.radius) || pc.radius <= 0.0) return result;
    DdgiProbeState state = uDdgiStates[instanceIndex];
    float3 center = uDdgiBasePositions[instanceIndex].xyz + state.relocation;
    if (!all(isfinite(center))) return result;
    result.center = center;
    float3 clipWRow = float3(uCamera.viewProj[3][0], uCamera.viewProj[3][1], uCamera.viewProj[3][2]);
    bool orthographic = dot(clipWRow, clipWRow) < 1e-12;
    float3 towardEye = uCamera.cameraWorldPosition.xyz - center;
    float distanceSquared = dot(towardEye, towardEye);
    if (!isfinite(distanceSquared)) return result;
    float2 corner = corners[vertexIndex];
    // Reverse-Z clipping is w-z >= 0 at near and z >= 0 at far. A center-plane
    // billboard may be fully clipped while a sphere hemisphere crosses either
    // plane, so crossing spheres require a conservative full-screen impostor.
    float4 nearPlane = uCamera.viewProj[3] - uCamera.viewProj[2];
    float nearPlaneLength = length(nearPlane.xyz);
    float nearDistance = nearPlaneLength > 0.0 ?
        dot(nearPlane, float4(center, 1)) / nearPlaneLength : pc.radius * 2.0;
    bool crossesNear = isfinite(nearDistance) && abs(nearDistance) <= pc.radius * 1.001;
    float4 farPlane = uCamera.viewProj[2];
    float farPlaneLength = length(farPlane.xyz);
    float farDistance = farPlaneLength > 0.0 ?
        dot(farPlane, float4(center, 1)) / farPlaneLength : pc.radius * 2.0;
    bool crossesFar = isfinite(farDistance) && abs(farDistance) <= pc.radius * 1.001;
    // An eye inside a debug sphere needs the whole screen as its conservative
    // impostor. Per-pixel ray/sphere intersection still supplies exact coverage.
    if (crossesNear || crossesFar || (!orthographic && distanceSquared <= pc.radius * pc.radius))
    {
        result.position = float4(corner, 0, 1);
        return result;
    }
    float3 nearCenter = DdgiDebugUnproject(0.0.xx, 1.0);
    float3 cameraUp = SafeNormalize(DdgiDebugUnproject(float2(0, 1), 1.0) - nearCenter,
                                    float3(0, 1, 0));
    float3 cameraRight = SafeNormalize(DdgiDebugUnproject(float2(1, 0), 1.0) - nearCenter,
                                       float3(1, 0, 0));
    float3 right = cameraRight, up = cameraUp;
    if (!orthographic)
    {
        towardEye *= rsqrt(distanceSquared);
        float3 reference = abs(dot(cameraUp, towardEye)) < 0.99 ? cameraUp : cameraRight;
        right = SafeNormalize(cross(reference, towardEye), cameraRight);
        up = SafeNormalize(cross(towardEye, right), cameraUp);
    }
    // A tangent-cone bound covers the complete perspective sphere silhouette.
    // Orthographic projection uses the camera plane and the exact world radius;
    // an eye-facing off-axis plane could underbound an orthographic sphere.
    float bound = orthographic ? pc.radius : pc.radius * sqrt(distanceSquared /
        max(distanceSquared - pc.radius * pc.radius, 1e-12));
    float3 world = center + (right * corner.x + up * corner.y) * bound;
    float4 clip = mul(uCamera.viewProj, float4(world, 1));
    if (all(isfinite(clip))) result.position = clip;
    return result;
}

float3 DdgiDebugHeat(float value)
{
    return lerp(float3(0.04, 0.15, 0.8), float3(1.0, 0.85, 0.08), saturate(value));
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if (uDdgi.frame.y == 0u || input.probeIndex >= uDdgi.counts.w) discard;
    uint width, height;
    gSceneDepth.GetDimensions(width, height);
    uint2 pixel = uint2(input.position.xy);
    if (pixel.x >= width || pixel.y >= height) discard;
    float2 uv = input.position.xy / float2(width, height);
    // Main rendering uses a negative-height viewport: framebuffer +Y becomes
    // NDC -Y. Reverse-Z near=1/far=0 provides a ray for either camera type.
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 nearWorld = DdgiDebugUnproject(ndc, 1.0);
    float3 farWorld = DdgiDebugUnproject(ndc, 0.0);
    float3 direction = SafeNormalize(farWorld - nearWorld, uCamera.cameraForward.xyz);
    if (!all(isfinite(nearWorld)) || !all(isfinite(farWorld))) discard;
    float3 relative = nearWorld - input.center;
    float closestDistance = -dot(relative, direction);
    float3 closestOffset = relative + direction * closestDistance;
    // Avoid subtracting squared camera distances: that loses the sphere radius
    // in float precision when a small marker is far away in a large BIM scene.
    float discriminant = pc.radius * pc.radius - dot(closestOffset, closestOffset);
    if (!isfinite(discriminant) || discriminant < 0.0) discard;
    float root = sqrt(discriminant);
    float distance = closestDistance - root;
    if (distance < 0.0) distance = closestDistance + root;
    if (!isfinite(distance) || distance < 0.0) discard;
    float3 world = nearWorld + direction * distance;
    float4 clip = mul(uCamera.viewProj, float4(world, 1));
    if (!all(isfinite(clip)) || clip.w <= 0.0) discard;
    float depth = clip.z / clip.w;
    if (!isfinite(depth) || depth <= 0.0 || depth > 1.0) discard;
    float sceneDepth = gSceneDepth.Load(int3(pixel, 0));
    // A greater reverse-Z value is nearer. Never overwrite opaque occluders.
    if (isfinite(sceneDepth) && sceneDepth > 0.0 && depth < sceneDepth) discard;

    DdgiProbeState state = uDdgiStates[input.probeIndex];
    float3 normal = SafeNormalize(world - input.center, -direction);
    float3 color = float3(0.22, 0.24, 0.28); // uninitialized
    if (pc.mode == 3u)
    {
        if (state.classification == DDGI_PROBE_INACTIVE) color = float3(1.0, 0.06, 0.08);
        else if (state.classification == DDGI_PROBE_ACTIVE)
            color = state.validRayCount > 0u ? float3(0.08, 0.85, 0.25) : float3(1.0, 0.45, 0.05);
        if ((state.flags & DDGI_PROBE_IRRADIANCE_CLAMPED) != 0u) color = float3(1, 0, 1);
    }
    else if (pc.mode == 4u)
    {
        if (state.classification != DDGI_PROBE_UNINITIALIZED)
        {
            float3 spacing = DdgiProbeVolumeForIndex(uDdgi, input.probeIndex).spacingNormalBias.xyz;
            float movement = length(state.relocation /
                max(spacing, 1e-10.xxx)) / (0.45 * sqrt(3.0));
            color = DdgiDebugHeat(movement);
        }
    }
    else if (state.classification != DDGI_PROBE_ACTIVE || state.validRayCount == 0u)
    {
        color = state.classification == DDGI_PROBE_INACTIVE ? float3(0.7, 0.02, 0.03) : color;
    }
    else if (pc.mode == 1u)
    {
        float2 atlasUv = DdgiAtlasUv(input.probeIndex, normal, uDdgi.irradianceLayout, uDdgi.extents.x);
        float3 irradiance = max(gDdgiIrradiance.SampleLevel(gDdgiSampler, atlasUv, 0).rgb, 0.0.xxx) / PI;
        color = all(isfinite(irradiance)) ? irradiance / (1.0.xxx + irradiance) : float3(1, 0, 1);
    }
    else if (pc.mode == 2u)
    {
        float2 atlasUv = DdgiAtlasUv(input.probeIndex, normal, uDdgi.visibilityLayout, uDdgi.extents.y);
        float2 moments = gDdgiVisibility.SampleLevel(gDdgiSampler, atlasUv, 0);
        color = all(isfinite(moments)) ? DdgiDebugHeat(moments.x / max(uDdgi.trace.x, 1e-10)) : float3(1, 0, 1);
    }
    return float4(color, 1);
}
